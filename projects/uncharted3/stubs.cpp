/*
 * Uncharted 3 - hooks/stubs specifiques au jeu.
 *
 * IMPORTANT : la table des fonctions et le point d'entree NE sont PLUS ici.
 *   - function_table[] / function_table_count -> recompiled/ppu_recomp_table.cpp
 *   - vm_base et la sequence de boot -> main.cpp
 *   - vm_read/write, ps3_indirect_call, g_trampoline_fn, ppu_run, etc.
 *     -> bibliotheque runtime (runtime/ppu/ppu_loader.cpp)
 *
 * Ce fichier sert a AJOUTER, au fil du bring-up, les fonctions firmware non
 * couvertes par les bridges HLE par defaut, et les correctifs propres a UC3.
 * Au premier run (break_on_unimplemented = true), chaque NID/syscall manquant
 * est loggue : implemente-le ici.
 */
#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "ps3emu/guest_call.h"
#include "ps3emu/nid.h"

#include "ppu_recomp.h"   /* ppu_context */
#include "../../libs/system/cellGame.h"
#include "../../libs/system/sysPrxForUser.h"
#include "../../libs/system/cellSysmodule.h"
#include "../../libs/system/cellSysutil.h"
#include "../../libs/video/cellGcmSys.h"
#include "../../libs/video/cellVideoOut.h"
#include "../../libs/video/rsx_commands.h"
#include "../../libs/video/rsx_null_backend.h"
#include "../../libs/video/rsx_d3d12_backend.h"
#include "../../runtime/spu/spu_context.h"

/* Lifted SPU image spu_0007 (guest elf 0x010CF700) — the SPURS task runner.
 * Defined (as C) in spu_gen/u3/spu_0007.c, added to the build. */
extern "C" void spu_recomp_register(void);
extern "C" void spu_begin_image(int image_id);
extern "C" void spu_func_00003050(spu_context* ctx); /* SPU ELF entry */
#include <csetjmp>
extern "C" { extern std::jmp_buf g_spu_abort_buf; extern volatile int g_spu_abort_armed; void spu_abort_arm(int on); }

#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>

/* ---------------------------------------------------------------------------
 * cellSync mutex bridges (boot-critical).
 *
 * The C++ static-initializer guards in UC3 use cellSyncMutex. The libs/ HLE
 * (libs/sync/cellSync.c) implements these but isn't wired into the NID table,
 * so cellSyncMutexInitialize was resolving to "unresolved NID -> return 0",
 * leaving the guard mutex uninitialized. The guard logic then spins/recurses,
 * leaking stack frames until sp walks into .data and corrupts the TOC. We
 * bridge the mutex family here with proper guest->host pointer translation.
 * A CellSyncMutex is a 4-byte lock word in guest memory. *) */
extern "C" uint8_t* vm_base;
extern "C" uint8_t  vm_read8(uint64_t a);
extern "C" uint16_t vm_read16(uint64_t a);
extern "C" uint32_t vm_read32(uint64_t a);
extern "C" uint64_t vm_read64(uint64_t a);
extern "C" void     vm_write8(uint64_t a, uint8_t v);
extern "C" void     vm_write16(uint64_t a, uint16_t v);
extern "C" void     vm_write32(uint64_t a, uint32_t v);
extern "C" void     vm_write64(uint64_t a, uint64_t v);
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" int stbi_zlib_decode_noheader_buffer(char* output, int output_size,
                                                   const char* input, int input_size);
extern "C" int32_t cellPadInit(uint32_t max_connect);
extern "C" int32_t cellKbInit(uint32_t max_connect);
extern "C" int32_t cellMouseInit(uint32_t max_connect);
/* cellPad polling (Phase 10 input). C linkage: only the symbol name matters,
 * so void* stands in for the struct pointers. Host structs mirror cellPad.h. */
extern "C" int32_t cellPadEnd(void);
extern "C" int32_t cellPadGetData(uint32_t port_no, void* data);
extern "C" int32_t cellPadGetInfo2(void* info);
extern "C" int32_t cellPadSetPortSetting(uint32_t port_no, uint32_t setting);
extern "C" int32_t cellPadGetCapabilityInfo(uint32_t port_no, void* info);
extern "C" int32_t cellPadSetActDirect(uint32_t port_no, void* param);
extern "C" int32_t cellPadClearBuf(uint32_t port_no);

#define CELL_SYNC_ERROR_NULL_POINTER 0x80410111   /* CELL_ERROR_BASE_SYNC | 0x11 */

static void br_cellSyncMutexInitialize(ppu_context* ctx) {
    uint32_t p = (uint32_t)ctx->gpr[3];
    if (!p) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_SYNC_ERROR_NULL_POINTER; return; }
    vm_write32(p, 0);                 /* lock = 0 (unlocked) */
    ctx->gpr[3] = 0;
}
static void br_cellSyncMutexLock(ppu_context* ctx) {
    uint32_t p = (uint32_t)ctx->gpr[3];
    if (!p) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_SYNC_ERROR_NULL_POINTER; return; }
    vm_write32(p, 1);                 /* boot is effectively single-threaded */
    ctx->gpr[3] = 0;
}
static void br_cellSyncMutexTryLock(ppu_context* ctx) {
    uint32_t p = (uint32_t)ctx->gpr[3];
    if (!p) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_SYNC_ERROR_NULL_POINTER; return; }
    vm_write32(p, 1);
    ctx->gpr[3] = 0;                  /* acquired */
}
static void br_cellSyncMutexUnlock(ppu_context* ctx) {
    uint32_t p = (uint32_t)ctx->gpr[3];
    if (!p) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_SYNC_ERROR_NULL_POINTER; return; }
    vm_write32(p, 0);
    ctx->gpr[3] = 0;
}

/* cellGameGetSizeKB(s32* size): the firmware clears the output before doing
 * the (potentially slow) directory-size calculation.  Returning CELL_OK
 * without touching it leaves UC3 consuming stale guest memory.  A zero size
 * is the conservative bring-up result until the full cellGame state machine
 * is implemented. */
static void br_cellGameGetSizeKB(ppu_context* ctx) {
    uint32_t size = (uint32_t)ctx->gpr[3];
    if (!size) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_GAME_ERROR_PARAM;
        return;
    }
    vm_write32(size, 0);
    ctx->gpr[3] = 0;
}

/* CellSpurs begins with the two 16-byte ready-count arrays.  SPURS2 uses the
 * second array for workload IDs 16..31, so both generations map directly to
 * spurs + wid.  Full workload-state validation belongs in the future SPURS
 * runtime; this bridge preserves the operation that callers depend on. */
static void br_cellSpursReadyCountStore(ppu_context* ctx) {
    uint32_t spurs = (uint32_t)ctx->gpr[3];
    uint32_t wid = (uint32_t)ctx->gpr[4];
    uint32_t value = (uint32_t)ctx->gpr[5];
    if (!spurs) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410811; return; }
    if ((spurs & 0x7F) != 0) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410810; return; }
    if (wid >= 32 || value > 0xFF) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410802; return; }
    vm_write8(spurs + wid, (uint8_t)value);
    ctx->gpr[3] = 0;
}

static constexpr int32_t CELL_SPURS_TASK_ERROR_INVAL = (int32_t)0x80410902;
static constexpr int32_t CELL_SPURS_TASK_ERROR_BUSY = (int32_t)0x8041090A;
static constexpr int32_t CELL_SPURS_TASK_ERROR_STAT = (int32_t)0x8041090F;
static constexpr int32_t CELL_SPURS_TASK_ERROR_ALIGN = (int32_t)0x80410910;
static constexpr int32_t CELL_SPURS_TASK_ERROR_NULL_POINTER = (int32_t)0x80410911;
static constexpr int32_t CELL_SPURS_TASK_ERROR_PERM = (int32_t)0x80410909;
static constexpr int32_t CELL_SPURS_CORE_ERROR_INVAL = (int32_t)0x80410702;
static constexpr int32_t CELL_SPURS_CORE_ERROR_ALIGN = (int32_t)0x80410710;
static constexpr int32_t CELL_SPURS_CORE_ERROR_NULL_POINTER = (int32_t)0x80410711;
static uint32_t bump_alloc(uint32_t size, uint32_t align);

struct Uc3SpursState {
    uint32_t address;
    uint32_t nspus;
    uint32_t spu_priority;
    uint32_t ppu_priority;
    uint32_t workload_flag;
    uint32_t next_wid;
    char prefix[16];
};

static std::mutex g_spurs_state_mutex;
static std::vector<Uc3SpursState> g_spurs_states;

static Uc3SpursState* spurs_find_state(uint32_t address) {
    for (auto& state : g_spurs_states) {
        if (state.address == address) return &state;
    }
    return nullptr;
}

static void br_cellSpursAttributeInitialize(ppu_context* ctx) {
    uint32_t attr = (uint32_t)ctx->gpr[3];
    uint32_t nspus = (uint32_t)ctx->gpr[6];
    if (!attr) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_NULL_POINTER; return; }
    if (attr & 7) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_ALIGN; return; }
    if (!nspus || nspus > 8) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_INVAL; return; }

    memset(vm_base + attr, 0, 512);
    vm_write32(attr + 0x00, (uint32_t)ctx->gpr[4]); /* revision */
    vm_write32(attr + 0x04, (uint32_t)ctx->gpr[5]); /* SDK version */
    vm_write32(attr + 0x08, nspus);
    vm_write32(attr + 0x0C, (uint32_t)ctx->gpr[7]);
    vm_write32(attr + 0x10, (uint32_t)ctx->gpr[8]);
    vm_write8(attr + 0x14, (uint8_t)ctx->gpr[9]);
    ctx->gpr[3] = 0;
}

static void br_cellSpursAttributeSetNamePrefix(ppu_context* ctx) {
    uint32_t attr = (uint32_t)ctx->gpr[3];
    uint32_t prefix = (uint32_t)ctx->gpr[4];
    uint32_t size = (uint32_t)ctx->gpr[5];
    if (!attr || (!prefix && size)) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_NULL_POINTER;
        return;
    }
    if (size > 15) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_INVAL; return; }
    memset(vm_base + attr + 0x15, 0, 15);
    if (size) memcpy(vm_base + attr + 0x15, vm_base + prefix, size);
    vm_write32(attr + 0x24, size);
    ctx->gpr[3] = 0;
}

static void br_cellSpursAttributeEnableSystemWorkload(ppu_context* ctx) {
    uint32_t attr = (uint32_t)ctx->gpr[3];
    uint32_t priorities = (uint32_t)ctx->gpr[4];
    if (!attr || !priorities) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_NULL_POINTER;
        return;
    }
    memcpy(vm_base + attr + 0x38, vm_base + priorities, 8);
    vm_write32(attr + 0x40, (uint32_t)ctx->gpr[5]);
    vm_write32(attr + 0x44, (uint32_t)ctx->gpr[6]);
    vm_write32(attr + 0x28, vm_read32(attr + 0x28) | 0x02000000u);
    ctx->gpr[3] = 0;
}

static void br_cellSpursInitializeWithAttribute2(ppu_context* ctx) {
    uint32_t spurs = (uint32_t)ctx->gpr[3];
    uint32_t attr = (uint32_t)ctx->gpr[4];
    if (!spurs || !attr) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_NULL_POINTER;
        return;
    }
    if ((spurs & 0x7F) || (attr & 7)) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_ALIGN;
        return;
    }

    memset(vm_base + spurs, 0, 8192);
    Uc3SpursState state{};
    state.address = spurs;
    state.nspus = vm_read32(attr + 0x08);
    state.spu_priority = vm_read32(attr + 0x0C);
    state.ppu_priority = vm_read32(attr + 0x10);
    uint32_t prefix_size = vm_read32(attr + 0x24);
    if (prefix_size > 15) prefix_size = 15;
    memcpy(state.prefix, vm_base + attr + 0x15, prefix_size);

    std::lock_guard<std::mutex> lock(g_spurs_state_mutex);
    if (Uc3SpursState* old = spurs_find_state(spurs)) *old = state;
    else g_spurs_states.push_back(state);
    printf("[cellSpurs] InitializeWithAttribute2(spurs=%08X, nSpus=%u, prefix='%s')\n",
           spurs, state.nspus, state.prefix);
    ctx->gpr[3] = 0;
}

static void br_cellSpursGetInfo(ppu_context* ctx) {
    uint32_t spurs = (uint32_t)ctx->gpr[3];
    uint32_t info = (uint32_t)ctx->gpr[4];
    if (!spurs || !info) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_NULL_POINTER;
        return;
    }
    std::lock_guard<std::mutex> lock(g_spurs_state_mutex);
    Uc3SpursState* state = spurs_find_state(spurs);
    if (!state) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_INVAL; return; }
    memset(vm_base + info, 0, 280);
    vm_write32(info + 0x00, state->nspus);
    vm_write32(info + 0x04, state->spu_priority);
    vm_write32(info + 0x08, state->ppu_priority);
    vm_write8(info + 0x0D, 1); /* SPURS2 */
    memcpy(vm_base + info + 0x58, state->prefix, sizeof(state->prefix));
    vm_write32(info + 0x68, (uint32_t)strlen(state->prefix));
    ctx->gpr[3] = 0;
}

static void br_cellSpursWorkloadAttributeInitialize(ppu_context* ctx) {
    uint32_t attr = (uint32_t)ctx->gpr[3];
    if (!attr) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_NULL_POINTER; return; }
    if (attr & 7) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_ALIGN; return; }
    memset(vm_base + attr, 0, 512);
    vm_write32(attr + 0x00, (uint32_t)ctx->gpr[4]);
    vm_write32(attr + 0x04, (uint32_t)ctx->gpr[5]);
    vm_write32(attr + 0x08, (uint32_t)ctx->gpr[6]);
    vm_write32(attr + 0x0C, (uint32_t)ctx->gpr[7]);
    vm_write64(attr + 0x10, ctx->gpr[8]);
    if ((uint32_t)ctx->gpr[9]) memcpy(vm_base + attr + 0x18, vm_base + (uint32_t)ctx->gpr[9], 8);
    vm_write32(attr + 0x20, (uint32_t)ctx->gpr[10]);
    vm_write32(attr + 0x24, vm_read32((uint32_t)ctx->gpr[1] + 0x70));
    ctx->gpr[3] = 0;
}

static void br_cellSpursWorkloadAttributeSetName(ppu_context* ctx) {
    uint32_t attr = (uint32_t)ctx->gpr[3];
    if (!attr) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_NULL_POINTER;
        return;
    }
    vm_write32(attr + 0x28, (uint32_t)ctx->gpr[4]);
    vm_write32(attr + 0x2C, (uint32_t)ctx->gpr[5]);
    ctx->gpr[3] = 0;
}

static void br_cellSpursAddWorkloadWithAttribute(ppu_context* ctx) {
    uint32_t spurs = (uint32_t)ctx->gpr[3];
    uint32_t wid_out = (uint32_t)ctx->gpr[4];
    uint32_t attr = (uint32_t)ctx->gpr[5];
    if (!spurs || !wid_out || !attr) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_NULL_POINTER;
        return;
    }
    std::lock_guard<std::mutex> lock(g_spurs_state_mutex);
    Uc3SpursState* state = spurs_find_state(spurs);
    if (!state) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_INVAL; return; }
    if (state->next_wid >= 32) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410704; return; }
    uint32_t wid = state->next_wid++;
    vm_write32(wid_out, wid);
    /* Populate CellSpurs.wklInfo1[wid] @ spurs+0xB00+wid*0x20 (RPCS3 layout) so the
     * SPU kernel/manager finds the workload's policy-module addr + arg (A2). */
    uint32_t pm   = vm_read32(attr + 0x08);
    uint64_t warg = vm_read64(attr + 0x10);
    uint32_t wsize= vm_read32(attr + 0x0C);
    uint32_t wkl  = spurs + 0xB00 + wid * 0x20;
    vm_write64(wkl + 0x00, pm);     /* addr (policy module EA) */
    vm_write64(wkl + 0x08, warg);   /* arg */
    vm_write32(wkl + 0x10, wsize);  /* size */
    printf("[cellSpurs] AddWorkloadWithAttribute(spurs=%08X, wid=%u, pm=%08X, arg=%08X%08X)\n",
           spurs, wid, pm, (uint32_t)(warg>>32), (uint32_t)warg);
    ctx->gpr[3] = 0;
}

static void br_cellSpursGetWorkloadFlag(ppu_context* ctx) {
    uint32_t spurs = (uint32_t)ctx->gpr[3];
    uint32_t flag_out = (uint32_t)ctx->gpr[4];
    if (!spurs || !flag_out) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_NULL_POINTER;
        return;
    }
    std::lock_guard<std::mutex> lock(g_spurs_state_mutex);
    Uc3SpursState* state = spurs_find_state(spurs);
    if (!state) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_INVAL; return; }
    if (!state->workload_flag) {
        state->workload_flag = bump_alloc(16, 16);
        if (!state->workload_flag) {
            ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410704;
            return;
        }
        memset(vm_base + state->workload_flag, 0, 16);
    }
    vm_write32(flag_out, state->workload_flag);
    ctx->gpr[3] = 0;
}

static void br_cellSpursGetWorkloadInfo(ppu_context* ctx) {
    uint32_t spurs = (uint32_t)ctx->gpr[3];
    uint32_t wid = (uint32_t)ctx->gpr[4];
    uint32_t info = (uint32_t)ctx->gpr[5];
    if (!spurs || !info) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_NULL_POINTER;
        return;
    }

    std::lock_guard<std::mutex> lock(g_spurs_state_mutex);
    Uc3SpursState* state = spurs_find_state(spurs);
    if (!state || wid >= state->next_wid) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_CORE_ERROR_INVAL;
        return;
    }

    /* CellSpursWorkloadInfo is 0x30 bytes. The synthetic workloads do not run
     * on SPUs, so contention, readyCount, idle requests and signals are zero. */
    memset(vm_base + info, 0, 0x30);
    ctx->gpr[3] = 0;
}

static void br_cellSpursCoreNoop(ppu_context* ctx) { ctx->gpr[3] = 0; }

static std::mutex g_spurs_event_mutex;
static std::condition_variable g_spurs_event_cv;

static void spurs_event_set_bits(uint32_t flag, uint16_t bits) {
    if (!flag || !bits) return;
    {
        std::lock_guard<std::mutex> lock(g_spurs_event_mutex);
        vm_write16(flag, (uint16_t)(vm_read16(flag) | bits));
    }
    g_spurs_event_cv.notify_all();
}

/* UC3 uses Sony's Edge raw-DEFLATE SPU task through a CellSpurs LFQueue.
 * ps3recomp does not execute SPURS tasks, so the queue bridge below performs
 * that one task synchronously on the host and preserves its guest completion
 * counter/event side effects. */
static void br_cellSpursLFQueueInitialize(ppu_context* ctx) {
    uint32_t queue = (uint32_t)ctx->gpr[4];
    if (!queue) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410911; return; }
    memset(vm_base + queue, 0, 128);
    vm_write32(queue + 0x10, (uint32_t)ctx->gpr[6]);
    vm_write32(queue + 0x14, (uint32_t)ctx->gpr[7]);
    vm_write64(queue + 0x18, (uint32_t)ctx->gpr[5]);
    vm_write32(queue + 0x24, (uint32_t)ctx->gpr[8]);
    ctx->gpr[3] = 0;
}

static void br_cellSpursLFQueuePush(ppu_context* ctx) {
    static std::mutex inflate_mutex;
    std::lock_guard<std::mutex> lock(inflate_mutex);
    uint32_t entry = (uint32_t)ctx->gpr[4];
    uint32_t source = vm_read32(entry + 0x00);
    uint32_t output = vm_read32(entry + 0x04);
    uint32_t compressed_size = vm_read32(entry + 0x08);
    uint32_t expected_size = vm_read32(entry + 0x0C);
    uint32_t counter_tagged = vm_read32(entry + 0x10);
    uint32_t counter = counter_tagged & ~1u;
    uint32_t event_flag = vm_read32(entry + 0x14);
    uint16_t event_bits = vm_read16(entry + 0x18);
    uint16_t skip_begin = vm_read16(entry + 0x1A);
    uint16_t skip_end = vm_read16(entry + 0x1C);
    if (!source || !output || !compressed_size || !expected_size ||
        compressed_size > 0x10000 || expected_size > 0x10000 ||
        (uint32_t)skip_begin + skip_end > expected_size) {
        fprintf(stderr,
                "[edge-zlib] invalid work item src=%08X dst=%08X comp=%u expected=%u skip=%u+%u\n",
                source, output, compressed_size, expected_size, skip_begin, skip_end);
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410902;
        return;
    }

    std::vector<char> decoded(expected_size);
    int decoded_size = stbi_zlib_decode_noheader_buffer(
        decoded.data(), (int)decoded.size(), (const char*)(vm_base + source),
        (int)compressed_size);
    if (decoded_size != (int)expected_size) {
        fprintf(stderr, "[edge-zlib] inflate failed: got=%d expected=%u\n",
                decoded_size, expected_size);
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410902;
        return;
    }

    uint32_t copy_size = expected_size - skip_begin - skip_end;
    memmove(vm_base + output, decoded.data() + skip_begin, copy_size);

    if (counter) {
        uint32_t pending = vm_read32(counter);
        if (pending) vm_write32(counter, pending - 1);
    }
    if ((counter_tagged & 1) && event_flag && event_bits)
        spurs_event_set_bits(event_flag, event_bits);

    ctx->gpr[3] = 0;
}

static void br_cellSpursEventFlagClear(ppu_context* ctx) {
    uint32_t flag = (uint32_t)ctx->gpr[3];
    uint16_t bits = (uint16_t)ctx->gpr[4];
    if (!flag) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_NULL_POINTER;
        return;
    }
    if (flag & 0x7F) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_ALIGN;
        return;
    }
    std::lock_guard<std::mutex> lock(g_spurs_event_mutex);
    vm_write16(flag, (uint16_t)(vm_read16(flag) & ~bits));
    ctx->gpr[3] = 0;
}

static void br_cellSpursEventFlagSet(ppu_context* ctx) {
    uint32_t flag = (uint32_t)ctx->gpr[3];
    uint16_t bits = (uint16_t)ctx->gpr[4];
    if (!flag) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_NULL_POINTER;
        return;
    }
    if (flag & 0x7F) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_ALIGN;
        return;
    }
    uint8_t direction = vm_read8(flag + 0x0E);
    if (direction != 1 && direction != 3) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_PERM;
        return;
    }
    spurs_event_set_bits(flag, bits);
    ctx->gpr[3] = 0;
}

static void cellSpursEventFlagWaitCommon(ppu_context* ctx, bool block) {
    uint32_t flag = (uint32_t)ctx->gpr[3];
    uint32_t mask_ptr = (uint32_t)ctx->gpr[4];
    uint32_t mode = (uint32_t)ctx->gpr[5];
    if (!flag || !mask_ptr) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_NULL_POINTER;
        return;
    }
    if ((flag & 0x7F) || (mask_ptr & 1)) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_ALIGN;
        return;
    }
    if (mode > 1) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_INVAL;
        return;
    }
    uint8_t direction = vm_read8(flag + 0x0E);
    if (direction != 1 && direction != 3) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_PERM;
        return;
    }
    if (block && vm_read8(flag + 0x0C) == 0xFF) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_STAT;
        return;
    }

    uint16_t requested = vm_read16(mask_ptr);
    std::unique_lock<std::mutex> lock(g_spurs_event_mutex);
    uint16_t relevant = 0;
    for (;;) {
        relevant = (uint16_t)(vm_read16(flag) & requested);
        bool ready = mode ? relevant == requested : relevant != 0;
        if (ready) break;
        if (!block) {
            ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_BUSY;
            return;
        }
        g_spurs_event_cv.wait_for(lock, std::chrono::milliseconds(2));
    }

    vm_write16(mask_ptr, relevant);
    if (vm_read8(flag + 0x0F) == 0)
        vm_write16(flag, (uint16_t)(vm_read16(flag) & ~relevant));
    ctx->gpr[3] = 0;
}

static void br_cellSpursEventFlagWait(ppu_context* ctx) {
    cellSpursEventFlagWaitCommon(ctx, true);
}

static void br_cellSpursEventFlagTryWait(ppu_context* ctx) {
    cellSpursEventFlagWaitCommon(ctx, false);
}

static void br_cellSpursTasksetAttributeInitialize(ppu_context* ctx) {
    ctx->gpr[3] = 0;
}

static void br_cellSpursTasksetAttribute2Initialize(ppu_context* ctx) {
    uint32_t attr = (uint32_t)ctx->gpr[3];
    if (!attr) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_NULL_POINTER;
        return;
    }
    if (attr & 7) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_ALIGN;
        return;
    }
    memset(vm_base + attr, 0, 512);
    vm_write32(attr + 0x00, (uint32_t)ctx->gpr[4]);
    memset(vm_base + attr + 0x10, 1, 8);
    vm_write32(attr + 0x18, 8);
    ctx->gpr[3] = 0;
}

static void br_cellSpursTasksetAttributeSetName(ppu_context* ctx) {
    ctx->gpr[3] = 0;
}

static void br_cellSpursCreateTasksetWithAttribute(ppu_context* ctx) {
    ctx->gpr[3] = 0;
}

static void br_cellSpursCreateTaskset2(ppu_context* ctx) {
    static std::mutex taskset_mutex;
    static uint32_t next_wid = 0;
    uint32_t spurs = (uint32_t)ctx->gpr[3];
    uint32_t taskset = (uint32_t)ctx->gpr[4];
    uint32_t attr = (uint32_t)ctx->gpr[5];
    if (!spurs || !taskset) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_NULL_POINTER;
        return;
    }
    if ((spurs & 0x7F) || (taskset & 0x7F) || (attr && (attr & 7))) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_ALIGN;
        return;
    }

    uint64_t args = attr ? vm_read64(attr + 0x08) : 0;
    uint8_t enable_clear_ls = attr && vm_read32(attr + 0x1C) ? 1 : 0;
    uint32_t task_name_buffer = attr ? vm_read32(attr + 0x20) : 0;
    if (task_name_buffer && (task_name_buffer & 0x0F)) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_ALIGN;
        return;
    }

    uint32_t wid;
    {
        std::lock_guard<std::mutex> lock(taskset_mutex);
        wid = next_wid++ & 31;
    }
    memset(vm_base + taskset, 0, 0x2900);
    vm_write64(taskset + 0x60, spurs);
    vm_write64(taskset + 0x68, args);
    vm_write8(taskset + 0x70, enable_clear_ls);
    vm_write8(taskset + 0x72, 0x80);
    vm_write32(taskset + 0x74, wid);
    vm_write32(taskset + 0x1890, 0x2900);
    ctx->gpr[3] = 0;
}

static void br_cellSpursEventFlagInitialize(ppu_context* ctx) {
    uint32_t flag = (uint32_t)ctx->gpr[5];
    if (flag) {
        std::lock_guard<std::mutex> lock(g_spurs_event_mutex);
        memset(vm_base + flag, 0, 128);
        vm_write8(flag + 0x0C, 0xFF);
        vm_write8(flag + 0x0E, (uint8_t)ctx->gpr[7]);
        vm_write8(flag + 0x0F, (uint8_t)ctx->gpr[6]);
        vm_write8(flag + 0x0D, ctx->gpr[4] ? 0 : 1);
        vm_write64(flag + 0x70, ctx->gpr[4] ? ctx->gpr[4] : ctx->gpr[3]);
    }
    ctx->gpr[3] = 0;
}

/* ---- SPU task execution harness (point 1) ------------------------------- *
 * Load a SPURS task's SPU ELF into a fresh 256KB local store and run its
 * lifted entry (spu_func_*) on a host thread. The lifted code does DMA to
 * main memory via the shared vm_base, so its output (e.g. FIFO geometry)
 * lands directly in guest memory. Empirical first cut: gated by UC3_SPU_TASK
 * so it can't destabilise the default boot. ELF is big-endian ELF64. */
static inline uint32_t spu_be32(const uint8_t* p){
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static inline uint64_t spu_be64(const uint8_t* p){
    return ((uint64_t)spu_be32(p)<<32)|spu_be32(p+4);
}
static inline uint16_t spu_be16(const uint8_t* p){ return (uint16_t)((p[0]<<8)|p[1]); }

static void uc3_spu_task_run(uint32_t elf_addr, uint32_t arg_ea, uint32_t taskset) {
    static spu_context s_ctx;            /* one persistent task; avoid 256KB stack */
    spu_context* ctx = &s_ctx;
    spu_context_init(ctx, 0);

    const uint8_t* elf = vm_base + elf_addr;
    if (spu_be32(elf) != 0x7F454C46u) {
        fprintf(stderr, "[spu-task] bad ELF magic at 0x%08X\n", elf_addr); return;
    }
    /* SPU ELF is 32-bit big-endian (EI_CLASS=1). */
    uint32_t e_entry     = spu_be32(elf + 0x18);
    uint32_t e_phoff     = spu_be32(elf + 0x1C);
    uint16_t e_phentsize = spu_be16(elf + 0x2A);
    uint16_t e_phnum     = spu_be16(elf + 0x2C);
    int loaded = 0;
    for (uint16_t i = 0; i < e_phnum; i++) {
        const uint8_t* ph = elf + e_phoff + (uint32_t)i * e_phentsize;
        uint32_t p_type   = spu_be32(ph + 0);
        uint32_t p_offset = spu_be32(ph + 4);
        uint32_t p_vaddr  = spu_be32(ph + 8);
        uint32_t p_filesz = spu_be32(ph + 16);
        if (p_type != 1) continue;       /* PT_LOAD */
        if ((uint64_t)p_vaddr + p_filesz > SPU_LS_SIZE) {
            fprintf(stderr, "[spu-task] segment OOB vaddr=0x%X filesz=0x%X\n",
                    p_vaddr, p_filesz);
            continue;
        }
        memcpy(ctx->ls + p_vaddr, elf + p_offset, p_filesz);
        loaded++;
    }
    fprintf(stderr, "[spu-task] loaded %d PT_LOAD seg(s), entry=0x%05X\n", loaded, e_entry);

    /* SPURS task entry ABI (RPCS3 cellSpursSpu.cpp spursTasksetStartTask):
     *   gpr[1] = SP = 0x3FFB0   (CRITICAL: kernel sets the stack; 0 corrupts LS)
     *   gpr[2] = 0
     *   gpr[3] = task args (128-bit quadword from arg_ea)
     *   gpr[4]._u64[0] = taskset->spurs addr ; gpr[4]._u64[1] = taskset->args
     *   gpr[5..127] = 0 ; pc = ELF entry */
    ctx->gpr[1]._u32[0] = 0x3FFB0;
    if (arg_ea) {
        const uint8_t* a = vm_base + arg_ea;
        for (int i = 0; i < 4; i++) ctx->gpr[3]._u32[i] = spu_be32(a + i*4);
    }
    {
        uint32_t spurs = taskset ? vm_read32(taskset + 0x60 + 4) : 0;
        if (!spurs) {                                /* taskset not populated: use
                                                      * the SPURS instance recorded
                                                      * at InitializeWithAttribute2 */
            std::lock_guard<std::mutex> lock(g_spurs_state_mutex);
            if (!g_spurs_states.empty()) spurs = g_spurs_states[0].address;
        }
        uint32_t tsarg_hi = taskset ? vm_read32(taskset + 0x68) : 0;
        uint32_t tsarg_lo = taskset ? vm_read32(taskset + 0x68 + 4) : 0;
        if (!tsarg_hi && !tsarg_lo && spurs) {       /* taskset->args empty: fall back
                                                      * to the workload arg (wklInfo) */
            tsarg_hi = vm_read32(spurs + 0xB00 + 0x08);
            tsarg_lo = vm_read32(spurs + 0xB00 + 0x0C);
        }
        ctx->gpr[4]._u32[1] = spurs;                 /* _u64[0] low = spurs addr */
        ctx->gpr[4]._u32[2] = tsarg_hi;              /* _u64[1] high */
        ctx->gpr[4]._u32[3] = tsarg_lo;              /* _u64[1] low */
        fprintf(stderr, "[spu-task] ABI: SP=0x3FFB0 spurs=0x%08X tsargs=%08X%08X\n",
                spurs, tsarg_hi, tsarg_lo);
    }

    /* Populate the SPURS kernel context @ LS 0x100 (RPCS3 SpursKernelContext).
     * The task DMAs SPURS structures using the spurs pointer the kernel placed
     * here; without it the EAs compute to 0. Big-endian writes into LS. */
    {
        uint32_t spurs_ptr = ctx->gpr[4]._u32[1];   /* set above */
        auto lsw32 = [&](uint32_t a, uint32_t v){
            ctx->ls[(a)&SPU_LS_MASK]=(uint8_t)(v>>24); ctx->ls[(a+1)&SPU_LS_MASK]=(uint8_t)(v>>16);
            ctx->ls[(a+2)&SPU_LS_MASK]=(uint8_t)(v>>8); ctx->ls[(a+3)&SPU_LS_MASK]=(uint8_t)v; };
        lsw32(0x1C0, 0);            /* spurs ptr high 32 (EA is 32-bit) */
        lsw32(0x1C4, spurs_ptr);    /* spurs ptr low 32 @ 0x1C0+4 */
        lsw32(0x1C8, 0);            /* spuNum */
        lsw32(0x1CC, 0);            /* dmaTagId */
        lsw32(0x1DC, 0);            /* wklCurrentId */
        fprintf(stderr, "[spu-task] kernel ctx @0x1C0 spurs=0x%08X\n", spurs_ptr);
    }

    /* Populate the SPURS taskset context @ LS 0x2700 (RPCS3 SpursTasksetContext).
     * The task DMAs its taskset (and task_info) from main memory using the
     * taskset pointer placed here. */
    {
        auto lsw32 = [&](uint32_t a, uint32_t v){
            ctx->ls[(a)&SPU_LS_MASK]=(uint8_t)(v>>24); ctx->ls[(a+1)&SPU_LS_MASK]=(uint8_t)(v>>16);
            ctx->ls[(a+2)&SPU_LS_MASK]=(uint8_t)(v>>8); ctx->ls[(a+3)&SPU_LS_MASK]=(uint8_t)v; };
        lsw32(0x27B8, 0);           /* taskset ptr high 32 */
        lsw32(0x27BC, taskset);     /* taskset ptr low 32 @ 0xB8+4 */
        lsw32(0x27CC, 0);           /* spuNum */
        lsw32(0x27D0, 0);           /* dmaTagId */
        lsw32(0x27D4, 0);           /* taskId */
        fprintf(stderr, "[spu-task] taskset ctx @0x27B8 taskset=0x%08X\n", taskset);
    }

    /* Workload info @ LS 0x3FFE0 (RPCS3): the kernel copies CellSpurs.wklInfo1[wid]
     * (spurs+0xB00+wid*0x20, 20B: addr@0, arg@8, size@0x10) here. The manager reads
     * the workload arg (often the taskset/job-queue pointer) from it. */
    {
        uint32_t spurs_ptr = ctx->gpr[4]._u32[1];
        if (spurs_ptr) {
            for (int i = 0; i < 32; i++)
                ctx->ls[(0x3FFE0 + i) & SPU_LS_MASK] = vm_read8(spurs_ptr + 0xB00 + i);
            uint32_t waddr = vm_read32(spurs_ptr + 0xB00 + 4);
            uint32_t warg  = vm_read32(spurs_ptr + 0xB00 + 0x0C);
            fprintf(stderr, "[spu-task] wklInfo[0] @0x3FFE0 addr=0x%08X arg=0x%08X\n",
                    waddr, warg);
            /* RE option 1: dump the workload-arg structure (taskset/job ring base)
             * to locate the geometry job descriptors the manager pulls. */
            if (warg && getenv("UC3_DUMP_RING") != nullptr) {
                uint32_t sub = vm_read32(warg + 0x30);   /* -> 0x3077F480 (taskset data) */
                fprintf(stderr, "[ring] control@0x%08X sub@0x%08X\n", warg, sub);
                for (uint32_t off = 0; off < 0x300; off += 16) {
                    fprintf(stderr, "[ringsub] +0x%03X:", off);
                    for (int i = 0; i < 16; i++)
                        fprintf(stderr, " %02X", vm_read8(sub + off + i));
                    fprintf(stderr, "\n");
                }
            }
        }
    }

    spu_recomp_register();
    spu_begin_image(0);
    ctx->image_id = 0;
    ctx->pc = e_entry;
    ctx->status = SPU_STATUS_RUNNING;
    fprintf(stderr, "[spu-task] >>> entering SPU entry 0x%05X (arg=[%08X %08X %08X %08X])\n",
            e_entry, ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1],
            ctx->gpr[3]._u32[2], ctx->gpr[3]._u32[3]);
    spu_abort_arm(1);
    if (setjmp(g_spu_abort_buf) == 0) {
        spu_func_00003050(ctx);          /* entry == 0x3050 for this image */
        fprintf(stderr, "[spu-task] <<< returned status=0x%X pc=0x%05X outmbox=%u/0x%08X\n",
                ctx->status, ctx->pc & SPU_LS_MASK,
                ctx->ch_out_mbox.count, ctx->ch_out_mbox.value);
    } else {
        fprintf(stderr, "[spu-task] <<< aborted by runaway guard (job queue empty/"
                        "invalid — A2 work). No crash.\n");
    }
    spu_abort_arm(0);
}

static void br_cellSpursCreateTask(ppu_context* ctx) {
    /* cellSpursCreateTask(taskset, taskId*, elf_addr, ctx_save, ctx_size,
     *                     ls_pattern, argument) — r5=elf is the SPU image
     * (task entry); r9=argument EA. Log to confirm elf_addr maps to an
     * extracted SPU image (spu_000N_at_XXXXXXXX). */
    static int n = 0;
    if (n < 24) {
        n++;
        fprintf(stderr, "[spurs-task] taskset=0x%08X elf=0x%08X ctx_save=0x%08X "
            "ctx_size=0x%X pattern=0x%08X arg=0x%08X lr=0x%08X\n",
            (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6],
            (uint32_t)ctx->gpr[7], (uint32_t)ctx->gpr[8], (uint32_t)ctx->gpr[9],
            (uint32_t)ctx->lr);
    }
    /* Populate the main-memory CellSpursTaskset (RPCS3 layout) so the SPU task,
     * which DMAs the taskset + task_info from main memory, finds valid data.
     * UC3 doesn't always call CreateTaskset2, so the struct can be empty. */
    {
        uint32_t taskset = (uint32_t)ctx->gpr[3];
        uint32_t taskId  = 0;                         /* we assign task 0 */
        uint32_t elf     = (uint32_t)ctx->gpr[5];
        uint32_t ctxsave = (uint32_t)ctx->gpr[6];
        uint32_t pattern = (uint32_t)ctx->gpr[8];
        uint32_t arg     = (uint32_t)ctx->gpr[9];
        if (taskset) {
            if (vm_read32(taskset + 0x60 + 4) == 0) {  /* spurs unset -> populate */
                uint32_t spurs = 0;
                { std::lock_guard<std::mutex> lock(g_spurs_state_mutex);
                  if (!g_spurs_states.empty()) spurs = g_spurs_states[0].address; }
                vm_write64(taskset + 0x60, spurs);     /* spurs ptr */
            }
            /* mark task 0 enabled/ready (bitsets are 16-byte big-endian, bit 0 = MSB) */
            vm_write8(taskset + 0x30, vm_read8(taskset + 0x30) | 0x80); /* enabled */
            vm_write8(taskset + 0x10, vm_read8(taskset + 0x10) | 0x80); /* ready */
            /* task_info[taskId] @ 0x80 + taskId*48 */
            uint32_t ti = taskset + 0x80 + taskId * 48;
            if (arg) for (int i = 0; i < 16; i++) vm_write8(ti + i, vm_read8(arg + i));
            vm_write64(ti + 0x10, elf);                /* elf/entry pointer */
            vm_write64(ti + 0x18, ctxsave);            /* context save storage */
            if (pattern) for (int i = 0; i < 8; i++)
                vm_write8(ti + 0x20 + i, vm_read8(pattern + i)); /* ls_pattern */
        }
    }

    /* RE option 1: monitor the taskset queue for per-frame job submission. The
     * ring is empty at CreateTask; the PPU fills it during rendering. Poll the
     * control block + sub-block and dump when a job descriptor appears. */
    if (getenv("UC3_RING_MON") != nullptr) {
        static std::atomic<bool> s_mon{false};
        uint32_t spurs = 0;
        { std::lock_guard<std::mutex> lock(g_spurs_state_mutex);
          if (!g_spurs_states.empty()) spurs = g_spurs_states[0].address; }
        uint32_t warg = spurs ? vm_read32(spurs + 0xB00 + 0x0C) : 0;
        if (warg && !s_mon.exchange(true)) {
            std::thread([warg]{
                uint32_t sub = vm_read32(warg + 0x30);
                fprintf(stderr, "[ringmon] watching control=0x%08X sub=0x%08X\n", warg, sub);
                uint8_t last_slots[0x40] = {0}; int reported = 0;
                for (;;) {
                    bool changed = false;
                    for (int i = 0; i < 0x40; i++) {
                        uint8_t b = vm_read8(warg + 0x40 + i);
                        if (b != last_slots[i]) { changed = true; last_slots[i] = b; }
                    }
                    if (changed && reported < 8) {
                        reported++;
                        fprintf(stderr, "[ringmon] slot table changed:\n");
                        for (uint32_t off = 0x40; off < 0x100; off += 16) {
                            fprintf(stderr, "  +0x%03X:", off);
                            for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", vm_read8(warg+off+i));
                            fprintf(stderr, "\n");
                        }
                        /* dump the sub-block (job descriptors may appear here) */
                        fprintf(stderr, "  sub@0x%08X:\n", sub);
                        for (uint32_t off = 0; off < 0x80; off += 16) {
                            fprintf(stderr, "    sub+0x%03X:", off);
                            for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", vm_read8(sub+off+i));
                            fprintf(stderr, "\n");
                        }
                        /* follow the job descriptor pointers (sub+0x04, +0x0C) */
                        for (uint32_t e = 0; e < 0x20; e += 8) {
                            uint32_t jp = vm_read32(sub + e + 4);
                            if (jp >= 0x10000 && jp < 0x40000000) {
                                fprintf(stderr, "    job desc=0x%08X ptr=0x%08X ->",
                                        vm_read32(sub+e), jp);
                                for (int i = 0; i < 32; i++) fprintf(stderr, " %02X", vm_read8(jp+i));
                                fprintf(stderr, "\n");
                            }
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }).detach();
        }
    }

    /* SPU task execution (Phase A1, gated by UC3_SPU_TASK). Run the task's SPU
     * ELF (r5) once on a host thread with the SPURS environment set up. */
    if (getenv("UC3_SPU_TASK") != nullptr) {
        static std::atomic<bool> s_task_started{false};
        uint32_t taskset = (uint32_t)ctx->gpr[3];
        uint32_t elf = (uint32_t)ctx->gpr[5];
        uint32_t arg = (uint32_t)ctx->gpr[9];
        if (elf && !s_task_started.exchange(true)) {
            std::thread([elf, arg, taskset]{ uc3_spu_task_run(elf, arg, taskset); }).detach();
        }
    }
    if ((uint32_t)ctx->gpr[4]) vm_write32((uint32_t)ctx->gpr[4], 0);
    ctx->gpr[3] = 0;
}

static void br_cellSpursEventFlagAttach(ppu_context* ctx) {
    static uint8_t next_port = 0x10;
    uint32_t flag = (uint32_t)ctx->gpr[3];
    if (!flag) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_NULL_POINTER;
        return;
    }
    if (flag & 0x7F) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_ALIGN;
        return;
    }
    uint8_t direction = vm_read8(flag + 0x0E);
    if (direction != 1 && direction != 3) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_PERM;
        return;
    }
    if (vm_read8(flag + 0x0C) != 0xFF) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_SPURS_TASK_ERROR_STAT;
        return;
    }
    vm_write8(flag + 0x0C, next_port++ & 0x3F);
    ctx->gpr[3] = 0;
}

static void br_cellSpursAttachLv2EventQueue(ppu_context* ctx) {
    static std::mutex port_mutex;
    static uint8_t next_dynamic_port = 0x10;
    uint32_t spurs = (uint32_t)ctx->gpr[3];
    uint32_t port_ptr = (uint32_t)ctx->gpr[5];
    bool dynamic = (uint32_t)ctx->gpr[6] != 0;
    if (!spurs || !port_ptr) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410711;
        return;
    }
    if ((spurs & 0x7F) != 0) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410710;
        return;
    }
    std::lock_guard<std::mutex> lock(port_mutex);
    uint8_t port = dynamic ? next_dynamic_port++ : vm_read8(port_ptr);
    if (port > 0x3F) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410702;
        return;
    }
    vm_write8(port_ptr, port & 0x3F);
    ctx->gpr[3] = 0;
}

static void br_cellSpursAttach(ppu_context* ctx) {
    ctx->gpr[3] = 0;
}

/* ---------------------------------------------------------------------------
 * sysPrxForUser CRT bridges (the boot-critical libc the engine leans on).
 *
 * These are the firmware's libc primitives: _sys_memcpy/memset/strlen/strcpy/...
 * and the heap allocator. Without them the engine's init copies/allocates
 * nothing (the unresolved-NID path returns 0), leaving structures full of
 * garbage -> the data-driven recursion we saw. NIDs are taken verbatim from
 * RPCS3's import dump (ground truth) so they match exactly. Args follow the
 * PPC64 ABI (r3..r10), return in r3; guest pointers are translated with vm_base.
 * -----------------------------------------------------------------------*/
#include <cstring>
#include <cctype>

extern "C" uint8_t vm_read8(uint64_t a);

static inline void*    Gp(uint32_t a) { return vm_base + a; }            /* guest->host ptr */
static inline char*    Gs(uint32_t a) { return (char*)(vm_base + a); }
#define A0 ((uint32_t)ctx->gpr[3])
#define A1 ((uint32_t)ctx->gpr[4])
#define A2 ((uint32_t)ctx->gpr[5])
#define A3 ((uint32_t)ctx->gpr[6])
#define RET(v) do { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)(v); } while(0)
#define RETP(v) do { ctx->gpr[3] = (uint64_t)(uint32_t)(v); } while(0)

static void br_sys_process_exit(ppu_context* ctx) {
    sys_process_exit((int32_t)A0);
}

/* --- memory --- */
static void br_memcpy (ppu_context* ctx){ if(A0&&A1&&A2) memmove(Gp(A0),Gp(A1),A2); RETP(A0); }
static void br_memmove(ppu_context* ctx){ if(A0&&A1&&A2) memmove(Gp(A0),Gp(A1),A2); RETP(A0); }
static void br_memset (ppu_context* ctx){ if(A0&&A2) memset(Gp(A0),(int)A1,A2);      RETP(A0); }
static void br_memcmp (ppu_context* ctx){ RET(A0&&A1?memcmp(Gp(A0),Gp(A1),A2):0); }
static void br_memchr (ppu_context* ctx){ if(!A0){RETP(0);return;} void* r=memchr(Gp(A0),(int)A1,A2); RETP(r?(uint32_t)((uint8_t*)r-vm_base):0); }
/* --- strings --- */
static void br_strlen (ppu_context* ctx){ RET(A0?(uint32_t)strlen(Gs(A0)):0); }
static void br_strcpy (ppu_context* ctx){ if(A0&&A1) strcpy(Gs(A0),Gs(A1)); RETP(A0); }
static void br_strncpy(ppu_context* ctx){ if(A0&&A1) strncpy(Gs(A0),Gs(A1),A2); RETP(A0); }
static void br_strcat (ppu_context* ctx){ if(A0&&A1) strcat(Gs(A0),Gs(A1)); RETP(A0); }
static void br_strncat(ppu_context* ctx){ if(A0&&A1) strncat(Gs(A0),Gs(A1),A2); RETP(A0); }
static void br_strcmp (ppu_context* ctx){ RET(A0&&A1?strcmp(Gs(A0),Gs(A1)):0); }
static void br_strncmp(ppu_context* ctx){ RET(A0&&A1?strncmp(Gs(A0),Gs(A1),A2):0); }
static void br_strncasecmp(ppu_context* ctx){ RET(A0&&A1?_strnicmp(Gs(A0),Gs(A1),A2):0); }
static void br_strchr (ppu_context* ctx){ if(!A0){RETP(0);return;} char* r=strchr(Gs(A0),(int)A1); RETP(r?(uint32_t)((uint8_t*)r-vm_base):0); }
static void br_strrchr(ppu_context* ctx){ if(!A0){RETP(0);return;} char* r=strrchr(Gs(A0),(int)A1); RETP(r?(uint32_t)((uint8_t*)r-vm_base):0); }
static void br_ctype  (ppu_context* ctx){ RET(0); }  /* __sys_look_ctype_table: stub */

/* --- printf family: real varargs expansion. The engine builds many strings
 * (asset/param names) via snprintf and then re-parses them, so a non-expanding
 * stub produced garbage that was later dereferenced as a pointer. PPC64 ELFv1
 * varargs: integer/pointer args occupy slots r3..r10 then the stack param save
 * area (sp+0x30 + slot*8); float args go in f1..f13 and also consume a slot. --- */
extern "C" uint64_t vm_read64(uint64_t a);

static int guest_vformat(char* out, size_t cap, ppu_context* ctx, int ai, uint32_t fmt_addr)
{
    const char* f = fmt_addr ? Gs(fmt_addr) : "";
    int fp = 1;                       /* next FPR (f1..f13) */
    size_t pos = 0;
    uint32_t sp = (uint32_t)ctx->gpr[1];
    auto put = [&](char c){ if (pos + 1 < cap) out[pos] = c; pos++; };
    auto next_int = [&]() -> uint64_t {
        uint64_t v = (ai < 8) ? ctx->gpr[3 + ai] : vm_read64(sp + 0x30 + (uint32_t)ai * 8);
        ai++; return v;
    };
    auto next_fp = [&]() -> double {
        double d = (fp <= 13) ? ctx->fpr[fp] : 0.0;
        fp++; ai++; return d;
    };
    for (; *f; ++f) {
        if (*f != '%') { put(*f); continue; }
        char spec[40]; int s = 0; spec[s++] = '%';
        ++f;
        /* flags */
        while (*f && strchr("-+ 0#", *f)) { if (s < 36) spec[s++] = *f; ++f; }
        /* width */
        if (*f == '*') { int w = (int)(int32_t)next_int(); s += snprintf(spec + s, 8, "%d", w); ++f; }
        else while (*f >= '0' && *f <= '9') { if (s < 36) spec[s++] = *f; ++f; }
        /* precision */
        if (*f == '.') { if (s < 36) spec[s++] = *f; ++f;
            if (*f == '*') { int p = (int)(int32_t)next_int(); s += snprintf(spec + s, 8, "%d", p); ++f; }
            else while (*f >= '0' && *f <= '9') { if (s < 36) spec[s++] = *f; ++f; } }
        /* length */
        int lng = 0;  /* 0=int,1=long,2=ll */
        while (*f && strchr("hljztL", *f)) { if (*f=='l') lng++; ++f; }
        char conv = *f;
        char tmp[512];
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = next_int();
            char hs[40]; int hl = 0; hs[hl++] = '%';
            for (int k = 1; k < s; k++) hs[hl++] = spec[k];
            hs[hl++] = 'l'; hs[hl++] = 'l'; hs[hl++] = conv; hs[hl] = 0;
            if (lng >= 2 || conv=='u'||conv=='x'||conv=='X'||conv=='o')
                snprintf(tmp, sizeof tmp, hs, (unsigned long long)v);
            else snprintf(tmp, sizeof tmp, hs, (long long)(int64_t)(int32_t)v);
            for (char* p = tmp; *p; ++p) put(*p); break; }
        case 'p': { uint64_t v = next_int(); snprintf(tmp, sizeof tmp, "0x%08X", (uint32_t)v);
            for (char* p = tmp; *p; ++p) put(*p); break; }
        case 'c': { uint64_t v = next_int(); put((char)v); break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d = next_fp(); spec[s++] = conv; spec[s] = 0;
            snprintf(tmp, sizeof tmp, spec, d);
            for (char* p = tmp; *p; ++p) put(*p); break; }
        case 's': { uint32_t pa = (uint32_t)next_int(); const char* str = pa ? Gs(pa) : "(null)";
            spec[s++] = 's'; spec[s] = 0;
            snprintf(tmp, sizeof tmp, spec, str);
            for (char* p = tmp; *p; ++p) put(*p); break; }
        case '%': put('%'); break;
        default: put('%'); if (conv) put(conv); break;
        }
        if (!*f) break;
    }
    if (cap) out[pos < cap ? pos : cap - 1] = 0;
    return (int)pos;
}

static void br_printf (ppu_context* ctx){ char b[2048]; int n=guest_vformat(b,sizeof b,ctx,1,A0); fputs(b,stderr); RET(n); }
static void br_snprintf(ppu_context* ctx){ /* buf=r3,size=r4,fmt=r5,va=r6.. */
    if(!A0){RET(0);return;} uint32_t cap=A1; if(!cap){RET(0);return;}
    int n=guest_vformat(Gs(A0),cap,ctx,3,A2); RET(n); }
static void br_sprintf(ppu_context* ctx){ /* buf=r3,fmt=r4,va=r5.. */
    if(!A0){RET(0);return;} int n=guest_vformat(Gs(A0),0x10000,ctx,2,A1); RET(n); }
static void br_ret0   (ppu_context* ctx){ RET(0); }

/* --- heap / malloc : one global bump allocator in guest memory. The 4 GB VM
 * commits this region lazily. Frees are no-ops (leak), fine for bring-up. --- */
static uint32_t g_bump = 0x40000000u;          /* CRT heap pool [0x40000000,0x50000000) */
static uint32_t bump_alloc(uint32_t size, uint32_t align){
    if(align < 16) align = 16;
    g_bump = (g_bump + align - 1) & ~(align - 1);
    uint32_t p = g_bump;
    g_bump += (size + 15) & ~15u;
    if(g_bump >= 0x50000000u) return 0;        /* pool exhausted */
    return p;
}
static void br_malloc  (ppu_context* ctx){ RETP(bump_alloc(A0, 16)); }
static void br_memalign(ppu_context* ctx){ RETP(bump_alloc(A1, A0)); }   /* (boundary,size) */
static void br_free    (ppu_context* ctx){ RET(0); }
static void br_heap_create(ppu_context* ctx){ static uint32_t h=0x1000; RETP(h+=0x10); } /* fake handle */
static void br_heap_malloc(ppu_context* ctx){ RETP(bump_alloc(A1, 16)); }   /* (heap,size) */
static void br_heap_memalign(ppu_context* ctx){ RETP(bump_alloc(A2, A1)); } /* (heap,boundary,size) */
static void br_heap_freesz(ppu_context* ctx){ RET(0x08000000); }            /* 128 MB free */

/* --- cellVideoOut / cellSysutil / cellGame guest-memory bridges ----------
 * The library implementations use native pointers. Keep their state and
 * policy, but marshal every multi-byte output through the guest BE helpers. */
static void guest_copy_cstr(uint32_t address, uint32_t capacity, const char* value) {
    if (!address || !capacity) return;
    if (!value) value = "";
    size_t count = strlen(value);
    if (count >= capacity) count = capacity - 1;
    memcpy(Gp(address), value, count);
    vm_write8(address + (uint32_t)count, 0);
}

static void br_cellVideoOutGetNumberOfDevice(ppu_context* ctx) {
    RET(cellVideoOutGetNumberOfDevice(A0));
}

static void br_cellVideoOutGetState(ppu_context* ctx) {
    if (!A2) { RET(CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER); return; }
    CellVideoOutState state{};
    int32_t rc = cellVideoOutGetState(A0, A1, &state);
    if (rc == CELL_OK) memcpy(Gp(A2), &state, sizeof(state));
    RET(rc);
}

static void br_cellVideoOutGetResolution(ppu_context* ctx) {
    if (!A1) { RET(CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER); return; }
    CellVideoOutResolution resolution{};
    int32_t rc = cellVideoOutGetResolution(A0, &resolution);
    if (rc == CELL_OK) memcpy(Gp(A1), &resolution, sizeof(resolution));
    RET(rc);
}

static void br_cellVideoOutGetResolutionAvailability(ppu_context* ctx) {
    if (A0 != CELL_VIDEO_OUT_PRIMARY) { RET(0); return; }
    switch (A1) {
    case CELL_VIDEO_OUT_RESOLUTION_1080:
    case CELL_VIDEO_OUT_RESOLUTION_720:
    case CELL_VIDEO_OUT_RESOLUTION_480:
    case CELL_VIDEO_OUT_RESOLUTION_576:
        RET(1);
        return;
    default:
        RET(0);
        return;
    }
}

static void br_cellVideoOutConfigure(ppu_context* ctx) {
    RET(cellVideoOutConfigure(A0,
                              A1 ? static_cast<CellVideoOutConfiguration*>(Gp(A1)) : nullptr,
                              A2 ? Gp(A2) : nullptr, A3));
}

struct Uc3SysutilCallback {
    uint32_t opd;
    uint32_t userdata;
};

static Uc3SysutilCallback g_sysutil_callbacks[CELL_SYSUTIL_MAX_CALLBACKS];

static void br_cellSysutilRegisterCallback(ppu_context* ctx) {
    int32_t slot = (int32_t)A0;
    if (slot < 0 || slot >= CELL_SYSUTIL_MAX_CALLBACKS) {
        RET(CELL_SYSUTIL_ERROR_VALUE);
        return;
    }
    g_sysutil_callbacks[slot] = {A1, A2};
    RET(0);
}

static void br_cellSysutilCheckCallback(ppu_context* ctx) {
    /* No host events are queued yet. Guest OPDs must only be invoked through
     * the PPU dispatcher, never by casting them to native function pointers. */
    RET(0);
}

static void br_cellSysutilGetSystemParamInt(ppu_context* ctx) {
    if (!A1) { RET(CELL_SYSUTIL_ERROR_VALUE); return; }
    int32_t value = 0;
    int32_t rc = cellSysutilGetSystemParamInt((int32_t)A0, &value);
    if (rc == CELL_OK) vm_write32(A1, (uint32_t)value);
    RET(rc);
}

static void br_cellSysutilGetSystemParamString(ppu_context* ctx) {
    RET(cellSysutilGetSystemParamString((int32_t)A0, A1 ? Gs(A1) : nullptr, A2));
}

static void br_cellSysCacheMount(ppu_context* ctx) {
    if (!A0) { RET(CELL_EINVAL); return; }
    char path[CELL_SYSCACHE_PATH_MAX]{};
    int32_t rc = cellSysCacheMount(path);
    if (rc == CELL_OK) guest_copy_cstr(A0, CELL_SYSCACHE_PATH_MAX, path);
    RET(rc);
}

static void br_cellSysmoduleLoadModule(ppu_context* ctx) {
    RET(cellSysmoduleLoadModule((uint16_t)A0));
}

static void br_cellSysmoduleUnloadModule(ppu_context* ctx) {
    RET(cellSysmoduleUnloadModule((uint16_t)A0));
}

static void br_cellSysmoduleInitialize(ppu_context* ctx) {
    RET(0);
}

static void uc3_configure_game_identity() {
    cellGame_set_title_id("BCES01175");
    cellGame_set_title("Uncharted 3: Drake's Deception\n1.19");
    cellGame_set_content_path("hdd0/game");
}

static void write_game_size(uint32_t address, const CellGameContentSize& size) {
    if (!address) return;
    vm_write32(address + 0x00, (uint32_t)size.hddFreeSizeKB);
    vm_write32(address + 0x04, (uint32_t)size.sizeKB);
    vm_write32(address + 0x08, (uint32_t)size.sysSizeKB);
}

static void br_cellGameBootCheck(ppu_context* ctx) {
    uint32_t type = 0;
    uint32_t attributes = 0;
    CellGameContentSize size{};
    char dir_name[CELL_GAME_PATH_MAX]{};
    int32_t rc = cellGameBootCheck(A0 ? &type : nullptr,
                                   A1 ? &attributes : nullptr,
                                   A2 ? &size : nullptr,
                                   A3 ? dir_name : nullptr);
    if (rc == CELL_OK) {
        if (A0) vm_write32(A0, type);
        if (A1) vm_write32(A1, attributes);
        write_game_size(A2, size);
        /* CELL_GAME_DIRNAME_SIZE is 32 in the SDK. */
        guest_copy_cstr(A3, 32, dir_name);
    }
    RET(rc);
}

static void br_cellGameContentPermit(ppu_context* ctx) {
    if (!A0 || !A1) { RET(CELL_GAME_ERROR_PARAM); return; }
    char content_info[CELL_GAME_PATH_MAX]{};
    char usrdir[CELL_GAME_PATH_MAX]{};
    int32_t rc = cellGameContentPermit(content_info, usrdir);
    if (rc == CELL_OK) {
        guest_copy_cstr(A0, CELL_GAME_PATH_MAX, content_info);
        guest_copy_cstr(A1, CELL_GAME_PATH_MAX, usrdir);
    }
    RET(rc);
}

static void br_cellGameDataCheck(ppu_context* ctx) {
    CellGameContentSize size{};
    int32_t rc = cellGameDataCheck(A0, A1 ? Gs(A1) : nullptr, A2 ? &size : nullptr);
    if (A2) write_game_size(A2, size);
    RET(rc);
}

static void br_cellGamePatchCheck(ppu_context* ctx) {
    RET(CELL_GAME_ERROR_NOTPATCH);
}

static void br_cellGameCreateGameData(ppu_context* ctx) {
    char content_info[CELL_GAME_PATH_MAX]{};
    char usrdir[CELL_GAME_PATH_MAX]{};
    auto* init = A0 ? reinterpret_cast<const CellGameSetInitParams*>(Gp(A0)) : nullptr;
    int32_t rc = cellGameCreateGameData(init, A1 ? content_info : nullptr,
                                       A2 ? usrdir : nullptr);
    if (rc == CELL_OK) {
        if (A1) guest_copy_cstr(A1, CELL_GAME_PATH_MAX, content_info);
        if (A2) guest_copy_cstr(A2, CELL_GAME_PATH_MAX, usrdir);
    }
    RET(rc);
}

static void br_cellGameSetParamString(ppu_context* ctx) {
    RET(cellGameSetParamString((int32_t)A0, A1 ? Gs(A1) : nullptr));
}

static void br_cellGameGetParamInt(ppu_context* ctx) {
    if (!A1) { RET(CELL_GAME_ERROR_PARAM); return; }
    int32_t value = 0;
    int32_t rc = cellGameGetParamInt((int32_t)A0, &value);
    if (rc == CELL_OK) {
        if (A0 == CELL_GAME_PARAMID_PARENTAL_LEVEL) value = 7;
        vm_write32(A1, (uint32_t)value);
    }
    RET(rc);
}

static void br_cellGameGetParamString(ppu_context* ctx) {
    if (!A1 || !A2) { RET(CELL_GAME_ERROR_PARAM); return; }
    const char* value = nullptr;
    switch (A0) {
    case CELL_GAME_PARAMID_TITLE_ID:     value = "BCES01175"; break;
    case CELL_GAME_PARAMID_TITLE:
    case CELL_GAME_PARAMID_TITLE_DEFAULT:value = "Uncharted 3: Drake's Deception\n1.19"; break;
    case CELL_GAME_PARAMID_APP_VER:      value = "01.19"; break;
    case CELL_GAME_PARAMID_PS3_SYSTEM_VER:value = "03.6000"; break;
    case CELL_GAME_PARAMID_VERSION:      value = "01.00"; break;
    default: break;
    }
    if (value) {
        guest_copy_cstr(A1, A2, value);
        RET(0);
        return;
    }
    RET(cellGameGetParamString((int32_t)A0, Gs(A1), A2));
}

/* --- cellGcmSys guest-memory bridge. The library tracks RSX state on the
 * host, while SDK callers expect pointer-returning objects in guest memory. */
struct Uc3GcmGuestState {
    bool initialized;
    uint32_t context;
    uint32_t control;
    uint32_t labels;
    uint32_t io_table;
    uint32_t ea_table;
    uint32_t io_address;
    uint32_t io_size;
    uint32_t next_io_offset;
    uint32_t vblank_handler;
    uint32_t flip_handler;
    uint32_t graphics_handler;
};

static Uc3GcmGuestState g_gcm;
static std::mutex g_gcm_mutex;

/* RSX command-processor state consumed by the FIFO drainer (Phase 9). */
static rsx_state g_rsx_state;
static std::atomic<bool> g_rsx_ready{false};

static void gcm_guest_write32(uint32_t address, uint32_t value) {
    vm_write32(address, value);
}

static void gcm_populate_offset_table(uint32_t ea, uint32_t io, uint32_t size) {
    if (!g_gcm.io_table || !g_gcm.ea_table) return;
    uint32_t pages = size >> 20;
    uint32_t ea_page = ea >> 20;
    uint32_t io_page = io >> 20;
    for (uint32_t i = 0; i < pages && ea_page + i < 65536 && io_page + i < 65536; i++) {
        vm_write16(g_gcm.io_table + (ea_page + i) * 2, (uint16_t)(io_page + i));
        vm_write16(g_gcm.ea_table + (io_page + i) * 2, (uint16_t)(ea_page + i));
    }
}

static bool gcm_allocate_guest_state(uint32_t io_address, uint32_t io_size) {
    g_gcm.control = bump_alloc(128, 128);
    g_gcm.labels = bump_alloc(256 * 4, 128);
    g_gcm.io_table = bump_alloc(65536 * 2, 128);
    g_gcm.ea_table = bump_alloc(65536 * 2, 128);
    if (!g_gcm.control || !g_gcm.labels || !g_gcm.io_table || !g_gcm.ea_table)
        return false;
    memset(vm_base + g_gcm.control, 0, 128);
    memset(vm_base + g_gcm.labels, 0, 256 * 4);
    memset(vm_base + g_gcm.io_table, 0xFF, 65536 * 2);
    memset(vm_base + g_gcm.ea_table, 0xFF, 65536 * 2);
    g_gcm.io_address = io_address;
    g_gcm.io_size = io_size;
    g_gcm.next_io_offset = (io_size + 0xFFFFF) & ~0xFFFFFu;
    gcm_populate_offset_table(io_address, 0, io_size & ~0xFFFFFu);
    return true;
}

static void br_cellGcmInitBody(ppu_context* ctx) {
    std::lock_guard<std::mutex> lock(g_gcm_mutex);
    uint32_t context = cellGcmSetupContext(A0, A1, A2, A3,
                                           bump_alloc, gcm_guest_write32);
    if (!context) { RET(CELL_GCM_ERROR_FAILURE); return; }
    memset(&g_gcm, 0, sizeof(g_gcm));
    g_gcm.context = context;
    if (!gcm_allocate_guest_state(A3, A2)) {
        RET(CELL_GCM_ERROR_FAILURE);
        return;
    }
    g_gcm.initialized = true;
    printf("[cellGcm] guest context=%08X control=%08X io=%08X+%08X\n",
           g_gcm.context, g_gcm.control, g_gcm.io_address, g_gcm.io_size);

    /* NULL RSX backend (doc step 3): no real GPU consumes the FIFO, so the RSX
     * read pointer GET never advances and the game's command-buffer logic blocks
     * (overflow callback / func_00038FBC poll wait for GET to catch up to PUT).
     * Spawn a host thread that continuously drains the FIFO: GET = PUT and
     * REF = PUT. This "consumes" all submitted commands instantly so the render
     * loop is never throttled. (A real backend would parse the FIFO here.) */
    {
        static std::atomic<bool> s_null_rsx_started{false};
        if (!s_null_rsx_started.exchange(true)) {
            uint32_t control = g_gcm.control;

            /* Phase 9 backend: presentation thread owns the Win32 window (must
             * create + pump messages on the same thread). It registers the null
             * backend (clear-color GDI window) so the FIFO drainer below can
             * route clears/draws to it. Disable with UC3_NO_WINDOW. */
            if (getenv("UC3_NO_WINDOW") == nullptr) {
                rsx_state_init(&g_rsx_state);
                std::thread([]{
                    /* Phase 9 Step 2 (RSX_GRAPHICS.md): D3D12 backend for real GPU
                     * rendering; fall back to the null GDI backend if D3D12 init
                     * fails (no D3D12 GPU) or UC3_NULL_RSX forces it. */
                    bool use_null = (getenv("UC3_NULL_RSX") != nullptr);
                    bool d3d12_ok = false;
                    if (!use_null) {
                        d3d12_ok = (rsx_d3d12_backend_init(1280, 720,
                                        "Uncharted 3 (ps3recomp D3D12)") == 0);
                        if (!d3d12_ok)
                            fprintf(stderr, "[uc3-window] D3D12 init failed, "
                                            "falling back to null backend\n");
                    }
                    if (!d3d12_ok) {
                        if (rsx_null_backend_init(1280, 720,
                                "Uncharted 3 (ps3recomp)") != 0) {
                            fprintf(stderr, "[uc3-window] backend init failed\n");
                            return;
                        }
                    }
                    g_rsx_ready.store(true);
                    for (;;) {
                        int rc = d3d12_ok ? rsx_d3d12_backend_pump_messages()
                                          : rsx_null_backend_pump_messages();
                        if (rc < 0) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(8));
                    }
                }).detach();
            }

            std::thread([control]{
                /* Phase 9 state-tracking (per GAME_PORTING_GUIDE): instead of
                 * silently discarding the FIFO, decode the NV4097 method headers
                 * we consume so we can SEE what the menu actually renders.
                 * header: [31:29] type | [28:18] count | method=header&0x1FFC.
                 * type 0=increasing, 1=jump, 2=non-increasing, 3=call/ret. */
                uint32_t prev_get = vm_read32(control + 4);
                uint64_t n_headers=0, n_draws=0, n_clears=0, n_begin_end=0, n_jumps=0;
                uint64_t total_methods=0; int report=0;
                static uint64_t s_hist[0x1000] = {0}; /* method>>2 -> count */
                const bool track = (getenv("UC3_FIFO_TRACK") != nullptr);
                const bool route = g_rsx_ready.load() || (getenv("UC3_NO_WINDOW") == nullptr);
                for (;;) {
                    uint32_t put = vm_read32(control + 0);
                    if (put != prev_get) {
                        /* Walk consumed window [prev_get, put). PUT/GET are IO
                         * offsets; FIFO base EA = io_address. Decode NV4097
                         * headers, route each (method,data) to the RSX backend
                         * (clears/state) and optionally log a histogram. */
                        uint32_t base = g_gcm.io_address;
                        uint32_t off = prev_get;
                        int guard = 0;
                        while (off != put && guard++ < 200000) {
                            uint32_t hdr = vm_read32(base + off);
                            off += 4;
                            if (hdr == 0) continue;
                            uint32_t type = hdr >> 29;
                            if (type == 1) {            /* jump */
                                n_jumps++;
                                uint32_t tgt = hdr & 0x1FFFFFFC;
                                off = tgt; continue;    /* follow jump within IO */
                            }
                            if ((hdr & 3) == 2 || hdr == 0x00020000u) continue; /* call/ret */
                            uint32_t count = (hdr >> 18) & 0x7FF;
                            uint32_t method = hdr & 0x1FFC;
                            bool noinc = (type == 2);   /* non-increasing */
                            if (track) {
                                /* one-time probe of key surface/viewport/clear data */
                                static bool seen[8] = {false};
                                int si = (method==0x204)?0:(method==0x208)?1:(method==0x300)?2:
                                         (method==0x304)?3:(method==0x1D0)?4:(method==0x200)?5:
                                         (method==0x1D94)?6:-1;
                                if (si>=0 && !seen[si]) {
                                    seen[si]=true;
                                    uint32_t d0 = count?vm_read32(base+off):0;
                                    fprintf(stderr,"[rsx-probe] method=0x%04X count=%u data0=0x%08X "
                                            "clearval=0x%08X\n",
                                            method,count,d0,g_rsx_state.color_clear_value);
                                }
                            }
                            if (route && g_rsx_ready.load()) {
                                for (uint32_t i = 0; i < count; i++) {
                                    uint32_t data = vm_read32(base + off + i*4);
                                    rsx_process_method(&g_rsx_state,
                                        noinc ? method : method + i*4, data);
                                }
                            }
                            if (track) {
                                /* Phase 9 Step 3: dump the full method sequence of
                                 * the first few begin/end primitive blocks to see
                                 * exactly how geometry is submitted (draw method?). */
                                static int s_be_block = -1;     /* -1=not in block */
                                static int s_blocks_dumped = 0;
                                uint32_t d0 = count ? vm_read32(base+off) : 0;
                                if (method == 0x1808) {         /* SET_BEGIN_END */
                                    if (d0 != 0 && s_blocks_dumped < 3) {
                                        s_be_block = s_blocks_dumped;
                                        fprintf(stderr,"[be-block %d] BEGIN prim=%u\n",
                                                s_be_block, d0);
                                    } else if (d0 == 0 && s_be_block >= 0) {
                                        fprintf(stderr,"[be-block %d] END\n", s_be_block);
                                        s_be_block = -1; s_blocks_dumped++;
                                    }
                                } else if (s_be_block >= 0) {
                                    fprintf(stderr,"   [be-block %d] m=0x%04X cnt=%u d0=0x%08X\n",
                                            s_be_block, method, count, d0);
                                }
                            }
                            n_headers++; total_methods += count;
                            if (track) {
                                if ((method>>2) < 0x1000) s_hist[method>>2]++;
                                if (method == 0x1D94) n_clears++;
                                else if (method == 0x1808) n_begin_end++;
                                else if (method == 0x1814 || method == 0x1820 ||
                                         method == 0x1824 || method == 0x1818) n_draws++;
                            }
                            off += count * 4;           /* skip data words */
                        }
                        if (track && ++report >= 200) {  /* ~every 20ms of polling */
                            report = 0;
                            fprintf(stderr, "[fifo-track] headers=%llu methods=%llu "
                                "begin_end=%llu draws=%llu clears=%llu jumps=%llu\n",
                                (unsigned long long)n_headers,(unsigned long long)total_methods,
                                (unsigned long long)n_begin_end,(unsigned long long)n_draws,
                                (unsigned long long)n_clears,(unsigned long long)n_jumps);
                            /* top-16 method histogram */
                            for (int top=0; top<16; top++) {
                                int bi=-1; uint64_t bv=0;
                                for (int i=0;i<0x1000;i++) if (s_hist[i]>bv){bv=s_hist[i];bi=i;}
                                if (bi<0||bv==0) break;
                                fprintf(stderr,"   [fifo-hist] method=0x%04X count=%llu\n",
                                        (unsigned)(bi<<2),(unsigned long long)bv);
                                s_hist[bi] = 0; /* consume for this dump (reset each report) */
                            }
                        }
                    }
                    prev_get = put;
                    vm_write32(control + 4, put);   /* GET = PUT (FIFO drained) */
                    vm_write32(control + 8, put);   /* REF = PUT (refs satisfied) */
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }).detach();
            printf("[null-rsx] FIFO drainer started (GET/REF follow PUT)\n");
        }
    }
    RET(0);
}

static void br_cellGcmGetConfiguration(ppu_context* ctx) {
    if (!A0) { RET(CELL_GCM_ERROR_INVALID_VALUE); return; }
    vm_write32(A0 + 0x00, 0xC0000000u);
    vm_write32(A0 + 0x04, g_gcm.io_address);
    vm_write32(A0 + 0x08, 0x10000000u);
    vm_write32(A0 + 0x0C, g_gcm.io_size);
    vm_write32(A0 + 0x10, 650000000u);
    vm_write32(A0 + 0x14, 500000000u);
    RET(0);
}

static void br_cellGcmGetControlRegister(ppu_context* ctx) { RETP(g_gcm.control); }

static void br_cellGcmGetLabelAddress(ppu_context* ctx) {
    RETP(A0 < 256 && g_gcm.labels ? g_gcm.labels + A0 * 4 : 0);
}

static void br_cellGcmGetOffsetTable(ppu_context* ctx) {
    if (!A0) { RET(CELL_GCM_ERROR_INVALID_VALUE); return; }
    vm_write32(A0 + 0, g_gcm.io_table);
    vm_write32(A0 + 4, g_gcm.ea_table);
    RET(0);
}

static void br_cellGcmMapMainMemory(ppu_context* ctx) {
    uint32_t ea = A0, size = A1, offset_out = A2;
    if (!offset_out) { RET(CELL_GCM_ERROR_INVALID_VALUE); return; }
    if (!size || (ea & 0xFFFFF) || (size & 0xFFFFF)) {
        RET(CELL_GCM_ERROR_INVALID_ALIGNMENT);
        return;
    }
    std::lock_guard<std::mutex> lock(g_gcm_mutex);
    uint32_t offset = g_gcm.next_io_offset;
    g_gcm.next_io_offset += size;
    gcm_populate_offset_table(ea, offset, size);
    vm_write32(offset_out, offset);
    printf("[cellGcm] map main ea=%08X size=%08X -> io=%08X\n", ea, size, offset);
    RET(0);
}

static void br_cellGcmMapEaIoAddressWithFlags(ppu_context* ctx) {
    uint32_t ea = A0, io = A1, size = A2;
    if (!size || (ea & 0xFFFFF) || (io & 0xFFFFF) || (size & 0xFFFFF)) {
        RET(CELL_GCM_ERROR_INVALID_ALIGNMENT);
        return;
    }
    std::lock_guard<std::mutex> lock(g_gcm_mutex);
    gcm_populate_offset_table(ea, io, size);
    uint32_t end = io + size;
    if (end > g_gcm.next_io_offset) g_gcm.next_io_offset = end;
    RET(0);
}

static void br_cellGcmSetFlipMode(ppu_context* ctx) {
    cellGcmSetFlipMode(A0);
    RET(0);
}

static void br_cellGcmSetDisplayBuffer(ppu_context* ctx) {
    RET(cellGcmSetDisplayBuffer(A0, A1, A2, A3, (uint32_t)ctx->gpr[7]));
}

static void br_cellGcmSetTileInfo(ppu_context* ctx) {
    RET(cellGcmSetTileInfo((uint8_t)A0, (uint8_t)A1, A2, A3,
                           (uint32_t)ctx->gpr[7], (uint8_t)ctx->gpr[8],
                           (uint16_t)ctx->gpr[9], (uint8_t)ctx->gpr[10]));
}

static void br_cellGcmBindTile(ppu_context* ctx) { RET(cellGcmBindTile((uint8_t)A0)); }
static void br_cellGcmBindZcull(ppu_context* ctx) { RET(cellGcmBindZcull((uint8_t)A0)); }

static void br_cellGcmSetDebugOutputLevel(ppu_context* ctx) {
    cellGcmSetDebugOutputLevel(A0);
    RET(0);
}

static void br_cellGcmSetVBlankHandler(ppu_context* ctx) {
    g_gcm.vblank_handler = A0;
    RET(0);
}

static void br_cellGcmSetFlipHandler(ppu_context* ctx) {
    g_gcm.flip_handler = A0;
    RET(0);
}

static void br_cellGcmSetGraphicsHandler(ppu_context* ctx) {
    g_gcm.graphics_handler = A0;
    RET(0);
}

static void br_cellGcmSetFlipCommand(ppu_context* ctx) {
    int32_t rc = cellGcmSetFlipCommand(A0);
    if (g_gcm.context && g_gcm.control) {
        uint32_t begin = vm_read32(g_gcm.context + 0x00);
        uint32_t current = vm_read32(g_gcm.context + 0x08);
        uint32_t put = current >= begin ? current - begin : 0;
        vm_write32(g_gcm.control + 0x00, put);
        vm_write32(g_gcm.control + 0x04, put);
    }
    rsx_backend* backend = rsx_get_backend();
    if (backend && backend->present) backend->present(backend->userdata, A0);
    RET(rc);
}

/* --- cellAudio guest-memory bridge --------------------------------------- */
struct Uc3AudioPort {
    bool open;
    bool started;
    uint32_t channels;
    uint32_t blocks;
    uint32_t size;
    uint32_t buffer;
    uint32_t read_index;
};

static Uc3AudioPort g_audio_ports[8];

static void br_cellAudioOutGetNumberOfDevice(ppu_context* ctx) {
    RET(1); /* one primary output device */
}

static void br_cellAudioOutGetDeviceInfo(ppu_context* ctx) {
    uint32_t info = (uint32_t)ctx->gpr[5];
    if (!info) { RET(0x80010002); return; }
    memset(vm_base + info, 0, 72);
    vm_write8(info + 0x00, 0); /* HDMI */
    vm_write8(info + 0x01, 1); /* one mode */
    vm_write8(info + 0x02, 2); /* connected */
    vm_write8(info + 0x06, 0); /* LPCM */
    vm_write8(info + 0x07, 2); /* stereo */
    vm_write8(info + 0x08, 4); /* 48 kHz */
    RET(0);
}

static void br_cellAudioInit(ppu_context* ctx) {
    memset(g_audio_ports, 0, sizeof(g_audio_ports));
    RET(0);
}

static void br_cellAudioPortOpen(ppu_context* ctx) {
    uint32_t param = A0;
    uint32_t out_port = A1;
    if (!param || !out_port) { RET(0x80310704); return; }

    uint32_t channels = (uint32_t)vm_read64(param + 0x00);
    uint32_t blocks = (uint32_t)vm_read64(param + 0x08);
    if ((channels != 2 && channels != 8) ||
        (blocks != 2 && blocks != 4 && blocks != 8 && blocks != 16 && blocks != 32)) {
        fprintf(stderr, "[cellAudio] invalid port parameters channels=%u blocks=%u\n",
                channels, blocks);
        RET(0x80310704);
        return;
    }

    uint32_t port_num = 8;
    for (uint32_t i = 0; i < 8; ++i) {
        if (!g_audio_ports[i].open) { port_num = i; break; }
    }
    if (port_num == 8) { RET(0x80310705); return; }

    Uc3AudioPort& port = g_audio_ports[port_num];
    port.open = true;
    port.channels = channels;
    port.blocks = blocks;
    port.size = channels * blocks * 256u * sizeof(float);
    port.buffer = bump_alloc(port.size, 128);
    port.read_index = bump_alloc(16, 16);
    if (!port.buffer || !port.read_index) {
        port = {};
        RET(0x8031070B);
        return;
    }

    memset(vm_base + port.buffer, 0, port.size);
    memset(vm_base + port.read_index, 0, 16);
    vm_write32(out_port, port_num);
    fprintf(stderr,
            "[cellAudio] opened port=%u channels=%u blocks=%u buffer=%08X index=%08X\n",
            port_num, channels, blocks, port.buffer, port.read_index);
    RET(0);
}

static void br_cellAudioGetPortConfig(ppu_context* ctx) {
    uint32_t port_num = A0;
    uint32_t config = A1;
    if (port_num >= 8 || !config || !g_audio_ports[port_num].open) {
        RET(0x80310704);
        return;
    }

    const Uc3AudioPort& port = g_audio_ports[port_num];
    vm_write32(config + 0x00, port.read_index);
    vm_write32(config + 0x04, port.started ? 2 : 1);
    vm_write64(config + 0x08, port.channels);
    vm_write64(config + 0x10, port.blocks);
    vm_write32(config + 0x18, port.size);
    vm_write32(config + 0x1C, port.buffer);
    RET(0);
}

static void br_cellAudioPortStart(ppu_context* ctx) {
    uint32_t port_num = A0;
    if (port_num >= 8 || !g_audio_ports[port_num].open) {
        RET(0x80310707);
        return;
    }
    g_audio_ports[port_num].started = true;
    RET(0);
}

static void br_cellPadInit(ppu_context* ctx) { RET(cellPadInit(A0)); }
static void br_cellKbInit(ppu_context* ctx) { RET(cellKbInit(A0)); }
static void br_cellMouseInit(ppu_context* ctx) { RET(cellMouseInit(A0)); }

/* Host mirrors of cellPad.h structs (same layout) for marshaling to guest. */
struct HostPadData  { int32_t len; uint16_t button[64]; };
struct HostPadInfo2 { uint32_t max_connect, now_connect, system_info;
                      uint32_t port_status[7], port_setting[7],
                               device_capability[7], device_type[7]; };

static void br_cellPadEnd(ppu_context* ctx) { RET(cellPadEnd()); }

static void br_cellPadGetData(ppu_context* ctx) {
    uint32_t port = A0, ea = A1;
    HostPadData pd; memset(&pd, 0, sizeof(pd));
    int32_t rc = cellPadGetData(port, &pd);
    if (ea) {
        vm_write32(ea + 0, (uint32_t)pd.len);          /* big-endian via vm_write */
        for (int i = 0; i < 64; i++) vm_write16(ea + 4 + i*2, pd.button[i]);
    }
    RET(rc);
}

static void br_cellPadGetInfo2(ppu_context* ctx) {
    uint32_t ea = A0;
    HostPadInfo2 in; memset(&in, 0, sizeof(in));
    int32_t rc = cellPadGetInfo2(&in);
    if (ea) {
        uint32_t* p = &in.max_connect;             /* 31 contiguous u32 fields */
        for (int i = 0; i < 31; i++) vm_write32(ea + i*4, p[i]);
    }
    RET(rc);
}

static void br_cellPadGetCapabilityInfo(ppu_context* ctx) {
    uint32_t port = A0, ea = A1;
    uint32_t info[64]; memset(info, 0, sizeof(info));
    int32_t rc = cellPadGetCapabilityInfo(port, info);
    if (ea) for (int i = 0; i < 64; i++) vm_write32(ea + i*4, info[i]);
    RET(rc);
}

static void br_cellPadSetPortSetting(ppu_context* ctx) {
    RET(cellPadSetPortSetting(A0, A1));
}

static void br_cellPadClearBuf(ppu_context* ctx) { RET(cellPadClearBuf(A0)); }

static void br_cellPadSetActDirect(ppu_context* ctx) {
    uint32_t port = A0, ea = A1;
    uint8_t motor[8] = {0};
    if (ea) for (int i = 0; i < 8; i++) motor[i] = vm_read8(ea + i);
    RET(cellPadSetActDirect(port, motor));
}

/* --- sceNpUtil bandwidth test (offline). GetStatus returns the status value
 * directly in r3; a generic CELL_OK stub therefore means NONE forever and the
 * game polls indefinitely. Complete the synthetic test immediately. --- */
static bool g_np_bandwidth_test_started = false;

static void br_sceNpUtilBandwidthTestInitStart(ppu_context* ctx) {
    g_np_bandwidth_test_started = true;
    fprintf(stderr, "[sceNpUtil] bandwidth test started (offline)\n");
    RET(0);
}

static void br_sceNpUtilBandwidthTestGetStatus(ppu_context* ctx) {
    RET(g_np_bandwidth_test_started ? 2 : 0); /* FINISHED / NONE */
}

static void br_sceNpUtilBandwidthTestShutdown(ppu_context* ctx) {
    const uint32_t result = A0;
    if (result) {
        const double bandwidth = 100000000.0;
        uint64_t bits = 0;
        memcpy(&bits, &bandwidth, sizeof(bits));
        vm_write64(result + 0x00, bits); /* upload_bps */
        vm_write64(result + 0x08, bits); /* download_bps */
        vm_write32(result + 0x10, 0);    /* result = CELL_OK */
        vm_write32(result + 0x14, 0);    /* padding */
    }
    g_np_bandwidth_test_started = false;
    RET(0);
}

/* --- sys_net (offline). During init the engine opens a socket, connect()s,
 * recv()s and parses the reply; with these unresolved (returning 0) recv gave
 * "0 bytes" and the reply parser looped forever on empty data -> infinite
 * recursion -> crash. Fail the network cleanly so the game takes its offline
 * path: socket/connect/recv return -1, lookups return null/error. --- */
static void br_net_fail (ppu_context* ctx){ RET(-1); }   /* recv/send error */
static void br_net_ok   (ppu_context* ctx){ RET(0); }    /* connect/setsockopt/close success */
static void br_net_null (ppu_context* ctx){ RETP(0); }   /* gethostbyname -> NULL */
static void br_net_sock (ppu_context* ctx){ RET(3); }    /* socket -> fake valid fd */
/* recv(fd, buf, len, flags): zero the caller's buffer so any string it reads is
 * a valid empty string (not uninitialized garbage), then return 0 (clean EOF). */
static void br_recv (ppu_context* ctx){
    uint32_t buf = A1, len = A2;
    if (buf && len) { if (len > 0x10000) len = 0x10000; memset(Gp(buf), 0, len); }
    RET(0);
}

struct InstallUc3Bridges {
    InstallUc3Bridges() {
        uc3_configure_game_identity();

        ps3_hle_register_ctx(ps3_compute_nid("cellSyncMutexInitialize"), "cellSyncMutexInitialize", br_cellSyncMutexInitialize);
        ps3_hle_register_ctx(ps3_compute_nid("cellSyncMutexLock"),       "cellSyncMutexLock",       br_cellSyncMutexLock);
        ps3_hle_register_ctx(ps3_compute_nid("cellSyncMutexTryLock"),    "cellSyncMutexTryLock",    br_cellSyncMutexTryLock);
        ps3_hle_register_ctx(ps3_compute_nid("cellSyncMutexUnlock"),     "cellSyncMutexUnlock",     br_cellSyncMutexUnlock);
        ps3_hle_register_ctx(0xEF9D42D5, "cellGameGetSizeKB", br_cellGameGetSizeKB);
        ps3_hle_register_ctx(0x95180230, "_cellSpursAttributeInitialize", br_cellSpursAttributeInitialize);
        ps3_hle_register_ctx(0x07529113, "cellSpursAttributeSetNamePrefix", br_cellSpursAttributeSetNamePrefix);
        ps3_hle_register_ctx(0x9DCBCB5D, "cellSpursAttributeEnableSystemWorkload", br_cellSpursAttributeEnableSystemWorkload);
        ps3_hle_register_ctx(0x30AA96C4, "cellSpursInitializeWithAttribute2", br_cellSpursInitializeWithAttribute2);
        ps3_hle_register_ctx(0x1F402F8F, "cellSpursGetInfo", br_cellSpursGetInfo);
        ps3_hle_register_ctx(0xEFEB2679, "_cellSpursWorkloadAttributeInitialize", br_cellSpursWorkloadAttributeInitialize);
        ps3_hle_register_ctx(0x4A5EAB63, "cellSpursWorkloadAttributeSetName", br_cellSpursWorkloadAttributeSetName);
        ps3_hle_register_ctx(0xC0158D8B, "cellSpursAddWorkloadWithAttribute", br_cellSpursAddWorkloadWithAttribute);
        ps3_hle_register_ctx(0x182D9890, "cellSpursRequestIdleSpu", br_cellSpursCoreNoop);
        ps3_hle_register_ctx(0xD2E23FA9, "cellSpursSetExceptionEventHandler", br_cellSpursCoreNoop);
        ps3_hle_register_ctx(0xA73BF47E, "_cellSpursWorkloadFlagReceiver", br_cellSpursCoreNoop);
        ps3_hle_register_ctx(0xC765B995, "cellSpursGetWorkloadFlag", br_cellSpursGetWorkloadFlag);
        ps3_hle_register_ctx(0x4E153E3E, "cellSpursGetWorkloadInfo", br_cellSpursGetWorkloadInfo);
        ps3_hle_register_ctx(0xF843818D, "cellSpursReadyCountStore", br_cellSpursReadyCountStore);
        ps3_hle_register_ctx(0x011EE38B, "_cellSpursLFQueueInitialize", br_cellSpursLFQueueInitialize);
        ps3_hle_register_ctx(0x8A85674D, "_cellSpursLFQueuePushBody", br_cellSpursLFQueuePush);
        ps3_hle_register_ctx(0x4AC7BAE4, "cellSpursEventFlagClear", br_cellSpursEventFlagClear);
        ps3_hle_register_ctx(0xF5507729, "cellSpursEventFlagSet", br_cellSpursEventFlagSet);
        ps3_hle_register_ctx(0x373523D4, "cellSpursEventFlagWait", br_cellSpursEventFlagWait);
        ps3_hle_register_ctx(0x6D2D9339, "cellSpursEventFlagTryWait", br_cellSpursEventFlagTryWait);
        ps3_hle_register_ctx(0x16394A4E, "_cellSpursTasksetAttributeInitialize", br_cellSpursTasksetAttributeInitialize);
        ps3_hle_register_ctx(0xC2ACDF43, "_cellSpursTasksetAttribute2Initialize", br_cellSpursTasksetAttribute2Initialize);
        ps3_hle_register_ctx(0x652B70E2, "cellSpursTasksetAttributeSetName", br_cellSpursTasksetAttributeSetName);
        ps3_hle_register_ctx(0xC10931CB, "cellSpursCreateTasksetWithAttribute", br_cellSpursCreateTasksetWithAttribute);
        ps3_hle_register_ctx(0x4A6465E3, "cellSpursCreateTaskset2", br_cellSpursCreateTaskset2);
        ps3_hle_register_ctx(0x5EF96465, "_cellSpursEventFlagInitialize", br_cellSpursEventFlagInitialize);
        ps3_hle_register_ctx(0xBEB600AC, "cellSpursCreateTask", br_cellSpursCreateTask);
        ps3_hle_register_ctx(0x87630976, "cellSpursEventFlagAttachLv2EventQueue", br_cellSpursEventFlagAttach);
        ps3_hle_register_ctx(0xB9BC6207, "cellSpursAttachLv2EventQueue", br_cellSpursAttachLv2EventQueue);
        ps3_hle_register_ctx(0x1656D49F, "cellSpursLFQueueAttachLv2EventQueue", br_cellSpursAttach);
        ps3_hle_register_ctx(0x73E06F91, "cellSpursLFQueueDetachLv2EventQueue", br_cellSpursCoreNoop);
        ps3_hle_register_ctx(0x22AAB31D, "cellSpursEventFlagDetachLv2EventQueue", br_cellSpursCoreNoop);
        ps3_hle_register_ctx(0x4E66D483, "cellSpursDetachLv2EventQueue", br_cellSpursCoreNoop);
        ps3_hle_register_ctx(0xA789E631, "cellSpursShutdownTaskset", br_cellSpursCoreNoop);
        ps3_hle_register_ctx(0x9F72ADD3, "cellSpursJoinTaskset", br_cellSpursCoreNoop);
        ps3_hle_register_ctx(0x98D5B343, "cellSpursShutdownWorkload", br_cellSpursCoreNoop);
        ps3_hle_register_ctx(0x5FD43FE4, "cellSpursWaitForWorkloadShutdown", br_cellSpursCoreNoop);
        ps3_hle_register_ctx(0x57E4DEC3, "cellSpursRemoveWorkload", br_cellSpursCoreNoop);
        ps3_hle_register_ctx(0xCA4C4600, "cellSpursFinalize", br_cellSpursCoreNoop);

        ps3_hle_register_ctx(0xE5E2B09D, "cellAudioOutGetNumberOfDevice", br_cellAudioOutGetNumberOfDevice);
        ps3_hle_register_ctx(0x7663E368, "cellAudioOutGetDeviceInfo", br_cellAudioOutGetDeviceInfo);
        ps3_hle_register_ctx(0x0B168F92, "cellAudioInit", br_cellAudioInit);
        ps3_hle_register_ctx(0xCD7BC431, "cellAudioPortOpen", br_cellAudioPortOpen);
        ps3_hle_register_ctx(0x74A66AF0, "cellAudioGetPortConfig", br_cellAudioGetPortConfig);
        ps3_hle_register_ctx(0x89BE28F2, "cellAudioPortStart", br_cellAudioPortStart);
        ps3_hle_register_ctx(0x1CF98800, "cellPadInit", br_cellPadInit);
        ps3_hle_register_ctx(0x433F6EC0, "cellKbInit", br_cellKbInit);
        ps3_hle_register_ctx(0xC9030138, "cellMouseInit", br_cellMouseInit);
        /* Phase 10 input: cellPad polling functions (XInput backend in
         * libs/input/cellPad.c). NIDs from tools/nid_database.compute_nid. */
        ps3_hle_register_ctx(0x4D9B75D5, "cellPadEnd", br_cellPadEnd);
        ps3_hle_register_ctx(0x8B72CDA1, "cellPadGetData", br_cellPadGetData);
        ps3_hle_register_ctx(0xA703A51D, "cellPadGetInfo2", br_cellPadGetInfo2);
        ps3_hle_register_ctx(0x578E3C98, "cellPadSetPortSetting", br_cellPadSetPortSetting);
        ps3_hle_register_ctx(0xDBF4C59C, "cellPadGetCapabilityInfo", br_cellPadGetCapabilityInfo);
        ps3_hle_register_ctx(0xF65544EE, "cellPadSetActDirect", br_cellPadSetActDirect);
        ps3_hle_register_ctx(0x0D5F2C14, "cellPadClearBuf", br_cellPadClearBuf);

        ps3_hle_register_ctx(0xC2CED2B7, "sceNpUtilBandwidthTestInitStart", br_sceNpUtilBandwidthTestInitStart);
        ps3_hle_register_ctx(0xC880F37D, "sceNpUtilBandwidthTestGetStatus", br_sceNpUtilBandwidthTestGetStatus);
        ps3_hle_register_ctx(0x432B3CBF, "sceNpUtilBandwidthTestShutdown", br_sceNpUtilBandwidthTestShutdown);

        ps3_hle_register_ctx(0x75BBB672, "cellVideoOutGetNumberOfDevice", br_cellVideoOutGetNumberOfDevice);
        ps3_hle_register_ctx(0x887572D5, "cellVideoOutGetState", br_cellVideoOutGetState);
        ps3_hle_register_ctx(0xE558748D, "cellVideoOutGetResolution", br_cellVideoOutGetResolution);
        ps3_hle_register_ctx(0xA322DB75, "cellVideoOutGetResolutionAvailability", br_cellVideoOutGetResolutionAvailability);
        ps3_hle_register_ctx(0x0BAE8772, "cellVideoOutConfigure", br_cellVideoOutConfigure);

        ps3_hle_register_ctx(0x9D98AFA0, "cellSysutilRegisterCallback", br_cellSysutilRegisterCallback);
        ps3_hle_register_ctx(0x189A74DA, "cellSysutilCheckCallback", br_cellSysutilCheckCallback);
        ps3_hle_register_ctx(0x40E895D3, "cellSysutilGetSystemParamInt", br_cellSysutilGetSystemParamInt);
        ps3_hle_register_ctx(0x938013A0, "cellSysutilGetSystemParamString", br_cellSysutilGetSystemParamString);
        ps3_hle_register_ctx(0x1E7BFF94, "cellSysCacheMount", br_cellSysCacheMount);

        ps3_hle_register_ctx(0x32267A31, "cellSysmoduleLoadModule", br_cellSysmoduleLoadModule);
        ps3_hle_register_ctx(0x112A5EE9, "cellSysmoduleUnloadModule", br_cellSysmoduleUnloadModule);
        ps3_hle_register_ctx(0x63FF6FF9, "cellSysmoduleInitialize", br_cellSysmoduleInitialize);

        ps3_hle_register_ctx(0xF52639EA, "cellGameBootCheck", br_cellGameBootCheck);
        ps3_hle_register_ctx(0xCE4374F6, "cellGamePatchCheck", br_cellGamePatchCheck);
        ps3_hle_register_ctx(0x70ACEC67, "cellGameContentPermit", br_cellGameContentPermit);
        ps3_hle_register_ctx(0xDB9819F3, "cellGameDataCheck", br_cellGameDataCheck);
        ps3_hle_register_ctx(0x42A2E133, "cellGameCreateGameData", br_cellGameCreateGameData);
        ps3_hle_register_ctx(0xB7A45CAF, "cellGameGetParamInt", br_cellGameGetParamInt);
        ps3_hle_register_ctx(0x3A5D726A, "cellGameGetParamString", br_cellGameGetParamString);
        ps3_hle_register_ctx(0xDAA5CD20, "cellGameSetParamString", br_cellGameSetParamString);

        ps3_hle_register_ctx(0x15BAE46B, "_cellGcmInitBody", br_cellGcmInitBody);
        ps3_hle_register_ctx(0xE315A0B2, "cellGcmGetConfiguration", br_cellGcmGetConfiguration);
        ps3_hle_register_ctx(0xF80196C1, "cellGcmGetLabelAddress", br_cellGcmGetLabelAddress);
        ps3_hle_register_ctx(0x2922AED0, "cellGcmGetOffsetTable", br_cellGcmGetOffsetTable);
        ps3_hle_register_ctx(0xA547ADDE, "cellGcmGetControlRegister", br_cellGcmGetControlRegister);
        ps3_hle_register_ctx(0x4AE8D215, "cellGcmSetFlipMode", br_cellGcmSetFlipMode);
        ps3_hle_register_ctx(0xA114EC67, "cellGcmMapMainMemory", br_cellGcmMapMainMemory);
        ps3_hle_register_ctx(0xBD100DBC, "cellGcmSetTileInfo", br_cellGcmSetTileInfo);
        ps3_hle_register_ctx(0x4524CCCD, "cellGcmBindTile", br_cellGcmBindTile);
        ps3_hle_register_ctx(0x9DC04436, "cellGcmBindZcull", br_cellGcmBindZcull);
        ps3_hle_register_ctx(0xA53D12AE, "cellGcmSetDisplayBuffer", br_cellGcmSetDisplayBuffer);
        ps3_hle_register_ctx(0x626E8518, "cellGcmMapEaIoAddressWithFlags", br_cellGcmMapEaIoAddressWithFlags);
        ps3_hle_register_ctx(0x51C9D62B, "cellGcmSetDebugOutputLevel", br_cellGcmSetDebugOutputLevel);
        ps3_hle_register_ctx(0xA91B0402, "cellGcmSetVBlankHandler", br_cellGcmSetVBlankHandler);
        ps3_hle_register_ctx(0xA41EF7E8, "cellGcmSetFlipHandler", br_cellGcmSetFlipHandler);
        ps3_hle_register_ctx(0xD01B570D, "cellGcmSetGraphicsHandler", br_cellGcmSetGraphicsHandler);
        ps3_hle_register_ctx(0x21397818, "_cellGcmSetFlipCommand", br_cellGcmSetFlipCommand);

        /* sysPrxForUser CRT — NIDs verbatim from RPCS3's import dump. */
        ps3_hle_register_ctx(0x6bf66ea7, "_sys_memcpy",      br_memcpy);
        ps3_hle_register_ctx(0x27427742, "_sys_memmove",     br_memmove);
        ps3_hle_register_ctx(0x68b9b011, "_sys_memset",      br_memset);
        ps3_hle_register_ctx(0xfb5db080, "_sys_memcmp",      br_memcmp);
        ps3_hle_register_ctx(0x3bd53c7b, "_sys_memchr",      br_memchr);
        ps3_hle_register_ctx(0x2d36462b, "_sys_strlen",      br_strlen);
        ps3_hle_register_ctx(0x99c88692, "_sys_strcpy",      br_strcpy);
        ps3_hle_register_ctx(0xd3039d4d, "_sys_strncpy",     br_strncpy);
        ps3_hle_register_ctx(0x052d29a6, "_sys_strcat",      br_strcat);
        ps3_hle_register_ctx(0x996f7cf8, "_sys_strncat",     br_strncat);
        ps3_hle_register_ctx(0x459b4393, "_sys_strcmp",      br_strcmp);
        ps3_hle_register_ctx(0x04e83d2c, "_sys_strncmp",     br_strncmp);
        ps3_hle_register_ctx(0x1ca525a2, "_sys_strncasecmp", br_strncasecmp);
        ps3_hle_register_ctx(0x7498887b, "_sys_strchr",      br_strchr);
        ps3_hle_register_ctx(0x191f0c4a, "_sys_strrchr",     br_strrchr);
        ps3_hle_register_ctx(0x3ef17f8c, "__sys_look_ctype_table", br_ctype);
        ps3_hle_register_ctx(0x9f04f7af, "_sys_printf",      br_printf);
        ps3_hle_register_ctx(0x06574237, "_sys_snprintf",    br_snprintf);
        ps3_hle_register_ctx(0xa1f9eafe, "_sys_sprintf",     br_sprintf);
        ps3_hle_register_ctx(0x0618936b, "_sys_vsnprintf",   br_snprintf);
        ps3_hle_register_ctx(0x791b9219, "_sys_vsprintf",    br_sprintf);
        ps3_hle_register_ctx(0xfa7f693d, "_sys_vprintf",     br_printf);
        ps3_hle_register_ctx(0xc4fd6121, "_sys_qsort",       br_ret0);   /* TODO: real qsort w/ guest cmp */
        /* heap / malloc */
        ps3_hle_register_ctx(0xbdb18f83, "_sys_malloc",          br_malloc);
        ps3_hle_register_ctx(0x318f17e1, "_sys_memalign",        br_memalign);
        ps3_hle_register_ctx(0xf7f7fb20, "_sys_free",            br_free);
        ps3_hle_register_ctx(0xb2fcf2c8, "_sys_heap_create_heap",br_heap_create);
        ps3_hle_register_ctx(0x35168520, "_sys_heap_malloc",     br_heap_malloc);
        ps3_hle_register_ctx(0x44265c08, "_sys_heap_memalign",   br_heap_memalign);
        ps3_hle_register_ctx(0x8a561d92, "_sys_heap_free",       br_free);
        ps3_hle_register_ctx(0xaede4b03, "_sys_heap_delete_heap",br_ret0);
        ps3_hle_register_ctx(0xb6369393, "_sys_heap_get_total_free_size", br_heap_freesz);
        ps3_hle_register_ctx(0xd1ad4570, "_sys_heap_get_mallinfo",br_ret0);
        ps3_hle_register_ctx(0x42b23552, "sys_prx_register_library", br_ret0);
        ps3_hle_register_ctx(0xe6f2c1e7, "sys_process_exit", br_sys_process_exit);

        /* sys_net — mimic RPCS3 "Internet: Connected": the socket/connect
         * SUCCEED so the game doesn't take its buggy "connect failed" error
         * path (which corrupts func_00D91404's TOC slot), but recv returns -1
         * (error) so the game doesn't parse an empty reply buffer (which
         * re-triggers the byte-search recursion). NIDs from RPCS3 dump. */
        ps3_hle_register_ctx(0x9c056962, "socket",        br_net_sock);  /* fake fd */
        ps3_hle_register_ctx(0x64f66d35, "connect",       br_net_ok);    /* success */
        ps3_hle_register_ctx(0xfba04f37, "recv",          br_recv);      /* zero buf, EOF */
        ps3_hle_register_ctx(0x88f03575, "setsockopt",    br_net_ok);
        ps3_hle_register_ctx(0x6db6e8cd, "socketclose",   br_net_ok);
        ps3_hle_register_ctx(0x71f4c717, "gethostbyname", br_net_null);
    }
} g_install_uc3_bridges;

/*
 * Exemple 1 : stub d'un NID non critique (retourne CELL_OK).
 *
 *   extern "C" int32_t cellFooBar(uint32_t a, uint32_t b)
 *   {
 *       printf("[STUB] cellFooBar(0x%x, 0x%x)\n", a, b);
 *       return CELL_OK;
 *   }
 *
 * Enregistre-le ensuite via le systeme de NID (docs/NID_SYSTEM.md) ou un
 * bridge ppu_context ("HLE Bridge Pattern" du GAME_PORTING_GUIDE).
 */

/*
 * Exemple 2 : installer le "guest caller" pour que le HLE rappelle du code
 * invite (callbacks cellSysutil, cellSaveData, ...). Le runtime owne la table
 * d'adresses + ps3_indirect_call ; on branche un trampoline OPD.
 *
 *   static void uc3_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
 *                                uint64_t a2, uint64_t a3) { ... }
 *   struct InstallGuestCaller {
 *       InstallGuestCaller() { g_ps3_guest_caller = uc3_guest_caller; }
 *   } g_install_guest_caller;
 *
 * Pour un premier boot, on laisse g_ps3_guest_caller a NULL (defaut) : les
 * callbacks invites ne se declenchent pas, mais le boot avance.
 */

/*
 * Exemple 3 : patch d'adresse invitee (sauter un check, forcer un retour).
 *   ps3::patches::nop_range(0x00812FA0, 0x00812FB0);
 *   ps3::patches::force_return(0x009A1000, 0);
 */
