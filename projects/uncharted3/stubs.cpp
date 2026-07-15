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
#include "../../libs/system/cellRtc.h"
#include "../../libs/system/cellSaveData.h"
#include "../../libs/network/sceNpTrophy.h"
#include "../../libs/video/rsx_commands.h"
#include <filesystem>
#include <string>
#include "../../libs/video/rsx_null_backend.h"
#include "../../libs/video/rsx_d3d12_backend.h"
#include "../../runtime/spu/spu_context.h"

/* [UC3 diagnostic] Host-stack backtrace + symbolization (dbghelp). The guest
 * back-chain is untraceable for utility functions reached via trampolines, but
 * the recompiler runs lifted functions as real C calls, so the HOST stack shows
 * the true chain of func_00XXXXXX. Symbolize via the .pdb. Gated at call sites. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
extern "C" void uc3_dump_host_backtrace(const char* tag) {
    static bool s_init = false; static HANDLE s_proc = 0;
    if (!s_init) { s_proc = GetCurrentProcess(); SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(s_proc, NULL, TRUE); s_init = true; }
    void* fr[40]; USHORT n = RtlCaptureStackBackTrace(1, 40, fr, NULL);
    fprintf(stderr, "[bt] %s (%u frames):\n", tag, n);
    char buf[sizeof(SYMBOL_INFO) + 300]; SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 290;
    for (USHORT i = 0; i < n; i++) {
        DWORD64 disp = 0;
        if (SymFromAddr(s_proc, (DWORD64)fr[i], &disp, si))
            fprintf(stderr, "   #%2u %s +0x%llX\n", i, si->Name, (unsigned long long)disp);
        else
            fprintf(stderr, "   #%2u %p\n", i, fr[i]);
    }
}

/* Lifted SPU image spu_0007 (guest elf 0x010CF700) — the SPURS task runner.
 * Defined (as C) in spu_gen/u3/spu_0007.c, added to the build. */
extern "C" void spu_recomp_register(void);
extern "C" void feec80_spu_recomp_register(void);
extern "C" void feec80_spu_func_00004000(spu_context* ctx);
extern "C" void feec80_spu_func_0000A2A8(spu_context* ctx);
extern "C" void policy_spu_recomp_register(void);
extern "C" void policy_spu_func_00000A00(spu_context* ctx);
extern "C" void policy_spu_func_000031E0(spu_context* ctx);
extern "C" void spu_begin_image(int image_id);
extern "C" void spu_func_00004000(spu_context* ctx); /* Edge job entry (LS 0x4000) */
extern "C" void spu_register_function(uint32_t addr, void (*fn)(spu_context*));
extern "C" void spu_indirect_branch(spu_context* ctx);
extern "C" int g_spu_dma_log;
extern "C" void spu_dma_set_deferred(spu_context* ctx, int enabled);
#include <csetjmp>
extern "C" { extern std::jmp_buf g_spu_abort_buf; extern volatile int g_spu_abort_armed; void spu_abort_arm(int on); }

/* [UC3_MOVIE_PROBE] Sonde du player film (pourquoi sce-ndi-logos.m2v n'est
 * jamais ouvert): les 4 builders de chemin movie1 (lisent le slot TOC-0x5898 =
 * fmt "%s/build/%s/movie1/%s.%s" @0x00DFA768) identifies statiquement dans
 * b0019. On wrappe leurs entrees de table PPU au runtime (ppu_register_function
 * ecrase l'entree existante), ainsi que les racines sans appelant direct qui
 * dominent leurs ppu_pcall. Les racines couvrent donc les appels directs sans
 * modifier les fichiers generes; un hit builder reste possible pour les appels
 * indirects. Un run sans aucun hit racine indique que cette famille de player
 * n'est pas encore dispatchee par la machine d'etats boot. */
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
/* func_00C9EA20/00CA4618/00CA59A8/00CB0388 : déjà déclarées via ppu_recomp.h */
#define UC3_MOVIE_WRAP(NAME, ADDR) \
    static void uc3_mw_##NAME(ppu_context* ctx) { \
        static int _n = 0; \
        if (_n < 16) { _n++; \
            fprintf(stderr, "[movie-probe] %s ENTRE r3=0x%08X r4=0x%08X lr=0x%08X\n", \
                    #NAME, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->lr); } \
        NAME(ctx); \
    }
UC3_MOVIE_WRAP(func_00C9EA20, 0x00C9EA20)
UC3_MOVIE_WRAP(func_00CA4618, 0x00CA4618)
UC3_MOVIE_WRAP(func_00CA59A8, 0x00CA59A8)
UC3_MOVIE_WRAP(func_00CB0388, 0x00CB0388)
UC3_MOVIE_WRAP(func_00C0CC14, 0x00C0CC14)
UC3_MOVIE_WRAP(func_00C0CDF0, 0x00C0CDF0)
UC3_MOVIE_WRAP(func_00C0CE80, 0x00C0CE80)
UC3_MOVIE_WRAP(func_00C0CE94, 0x00C0CE94)
UC3_MOVIE_WRAP(func_00C0CEF4, 0x00C0CEF4)
UC3_MOVIE_WRAP(func_00C76E70, 0x00C76E70)
UC3_MOVIE_WRAP(func_00C76F2C, 0x00C76F2C)
UC3_MOVIE_WRAP(func_00C76FB0, 0x00C76FB0)
UC3_MOVIE_WRAP(func_00C77494, 0x00C77494)
UC3_MOVIE_WRAP(func_00C78F24, 0x00C78F24)
extern "C" void uc3_install_movie_probe(void) {
    if (!getenv("UC3_MOVIE_PROBE")) return;
    ppu_register_function(0x00C9EA20, uc3_mw_func_00C9EA20);
    ppu_register_function(0x00CA4618, uc3_mw_func_00CA4618);
    ppu_register_function(0x00CA59A8, uc3_mw_func_00CA59A8);
    ppu_register_function(0x00CB0388, uc3_mw_func_00CB0388);
    ppu_register_function(0x00C0CC14, uc3_mw_func_00C0CC14);
    ppu_register_function(0x00C0CDF0, uc3_mw_func_00C0CDF0);
    ppu_register_function(0x00C0CE80, uc3_mw_func_00C0CE80);
    ppu_register_function(0x00C0CE94, uc3_mw_func_00C0CE94);
    ppu_register_function(0x00C0CEF4, uc3_mw_func_00C0CEF4);
    ppu_register_function(0x00C76E70, uc3_mw_func_00C76E70);
    ppu_register_function(0x00C76F2C, uc3_mw_func_00C76F2C);
    ppu_register_function(0x00C76FB0, uc3_mw_func_00C76FB0);
    ppu_register_function(0x00C77494, uc3_mw_func_00C77494);
    ppu_register_function(0x00C78F24, uc3_mw_func_00C78F24);
    fprintf(stderr, "[movie-probe] wrappers installes (4 builders + 10 racines directes)\n");
}

/* Read-only hook injected at func_009ADFC4, the per-frame save manager. The
 * nested object at +0xC is passed to func_009B05E0 and its +4 state gates the
 * next operation (RPCS3 goes family 3 -> family 2/AutoLoad). */
static uint64_t uc3_save_trace_timestamp_us() {
    return (uint64_t)GetTickCount64() * 1000ULL;
}

static uint64_t uc3_save_target_tid(uint32_t op) {
    return op >= 0x10000u ? vm_read64(op + 0x1398u) : 0;
}

void uc3_note_save_manager(uint32_t manager, uint64_t caller_tid, uint64_t lr) {
    const bool consume_trace = getenv("UC3_SAVE_CONSUME_TRACE") != nullptr;
    if ((!getenv("UC3_SAVE_MANAGER") && !consume_trace) || manager < 0x10000u) return;

    const uint32_t op = vm_read32(manager + 0xCu);
    const uint32_t state = op >= 0x10000u ? vm_read32(op + 0x4u) : 0xFFFFFFFFu;
    static uint32_t s_manager = 0;
    static uint32_t s_op = 0;
    static uint32_t s_state = 0xFFFFFFFFu;
    static int s_initial = 0;
    if (s_initial < 8 || manager != s_manager || op != s_op || state != s_state) {
        if (s_initial < 8) ++s_initial;
        fprintf(stderr,
                "[save-manager] mgr=0x%08X request=%u/%u op=0x%08X "
                "fields=%d/%d/%d/%d/%d\n",
                manager, vm_read32(manager + 0x50u), vm_read32(manager + 0x54u), op,
                op >= 0x10000u ? (int)vm_read32(op + 0x0u) : -1,
                op >= 0x10000u ? (int)state : -1,
                op >= 0x10000u ? (int)vm_read32(op + 0x8u) : -1,
                op >= 0x10000u ? (int)vm_read32(op + 0xCu) : -1,
                op >= 0x10000u ? (int)vm_read32(op + 0x24u) : -1);
        if (consume_trace) {
            fprintf(stderr,
                    "[save-consume] ts_us=%llu event=manager-call caller_tid=%llu "
                    "target_tid=%llu state=%d->%d lr=0x%08llX rc=0x00000000 "
                    "parent=0x%08X context=0x%08X observed=%d\n",
                    (unsigned long long)uc3_save_trace_timestamp_us(),
                    (unsigned long long)caller_tid,
                    (unsigned long long)uc3_save_target_tid(op),
                    (int)state, (int)state, (unsigned long long)lr,
                    manager, op, (int)vm_read32(manager + 0x44u));
        }
        s_manager = manager;
        s_op = op;
        s_state = state;
    }
}

/* Read-only observer for the three-node Save/Load state tree. site identifies
 * the frame tick (009AE7D4) or the later wait predicate (009ADE4C). */
void uc3_note_save_tree(uint32_t root, uint32_t site, uint64_t caller_tid, uint64_t lr) {
    const bool consume_trace = getenv("UC3_SAVE_CONSUME_TRACE") != nullptr;
    if ((!getenv("UC3_SAVE_TREE") && !consume_trace) || root < 0x10000u) return;

    const uint32_t marker = vm_read32(root + 0x190u);
    const uint32_t state0 = vm_read32(root + 0x34u);
    const uint32_t state1 = vm_read32(root + 0x98u);
    const uint32_t state2 = vm_read32(root + 0xFCu);
    static uint32_t s_root = 0;
    static uint32_t s_site = 0;
    static uint32_t s_marker = 0xFFFFFFFFu;
    static uint32_t s_state0 = 0xFFFFFFFFu;
    static uint32_t s_state1 = 0xFFFFFFFFu;
    static uint32_t s_state2 = 0xFFFFFFFFu;
    static uint64_t s_calls = 0;
    ++s_calls;

    if (root != s_root || site != s_site || marker != s_marker || state0 != s_state0 ||
        state1 != s_state1 || state2 != s_state2 || s_calls <= 8u ||
        (s_calls % 1000u) == 0u) {
        fprintf(stderr,
                "[save-tree] call=%llu site=0x%08X root=0x%08X marker=0x%08X "
                "states=%d/%d/%d\n",
                (unsigned long long)s_calls, site, root, marker,
                (int)state0, (int)state1, (int)state2);
        if (consume_trace) {
            const uint32_t op = vm_read32(root + 0xCu);
            fprintf(stderr,
                    "[save-consume] ts_us=%llu event=%s caller_tid=%llu "
                    "target_tid=%llu state=%d->%d lr=0x%08llX rc=0x00000000 "
                    "parent=0x%08X context=0x%08X states=%d/%d/%d\n",
                    (unsigned long long)uc3_save_trace_timestamp_us(),
                    site == 0x009AE7D4u ? "root-tick" : "late-predicate",
                    (unsigned long long)caller_tid,
                    (unsigned long long)uc3_save_target_tid(op),
                    (int)state0, (int)state0, (unsigned long long)lr,
                    root, op, (int)state0, (int)state1, (int)state2);
        }
        s_root = root;
        s_site = site;
        s_marker = marker;
        s_state0 = state0;
        s_state1 = state1;
        s_state2 = state2;
    }
}

void uc3_note_save_child(uint32_t child, uint32_t root, uint32_t phase,
                         uint64_t caller_tid, uint64_t lr) {
    if (!getenv("UC3_SAVE_CONSUME_TRACE") || child < 0x10000u) return;

    struct child_snapshot {
        uint32_t child;
        uint32_t child_state;
        uint32_t observed;
        uint32_t op_state;
        uint64_t calls;
    };
    static child_snapshot snapshots[3] = {};
    child_snapshot* snap = nullptr;
    for (auto& candidate : snapshots) {
        if (candidate.child == child || candidate.child == 0) {
            snap = &candidate;
            if (candidate.child == 0) candidate.child = child;
            break;
        }
    }
    if (!snap) return;

    const uint32_t op = vm_read32(child + 0xCu);
    const uint32_t child_state = vm_read32(child + 0x34u);
    const uint32_t observed = vm_read32(child + 0x44u);
    const uint32_t op_state = op >= 0x10000u ? vm_read32(op + 0x4u) : 0xFFFFFFFFu;
    ++snap->calls;

    const uint32_t before_child = phase ? snap->child_state : child_state;
    const uint32_t before_observed = phase ? snap->observed : observed;
    const uint32_t before_op = phase ? snap->op_state : op_state;
    const bool changed = snap->calls <= 12u || child_state != snap->child_state ||
                         observed != snap->observed || op_state != snap->op_state;
    if (changed) {
        fprintf(stderr,
                "[save-consume] ts_us=%llu event=%s caller_tid=%llu target_tid=%llu "
                "state=%d->%d lr=0x%08llX rc=0x00000000 parent=0x%08X "
                "context=0x%08X op_state=%d->%d observed=%d->%d\n",
                (unsigned long long)uc3_save_trace_timestamp_us(),
                phase ? "poll-return" : "poll-enter",
                (unsigned long long)caller_tid,
                (unsigned long long)uc3_save_target_tid(op),
                (int)before_child, (int)child_state, (unsigned long long)lr,
                root, op, (int)before_op, (int)op_state,
                (int)before_observed, (int)observed);
    }

    snap->child_state = child_state;
    snap->observed = observed;
    snap->op_state = op_state;
}

#include <cstdint>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <atomic>
#include <memory>
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
extern "C" uint32_t g_canonical_toc;
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
extern "C" void uc3_spu_exec_ready_count(uint32_t spurs, uint32_t wid,
                                           uint32_t value);
static void br_cellSpursReadyCountStore(ppu_context* ctx) {
    uint32_t spurs = (uint32_t)ctx->gpr[3];
    uint32_t wid = (uint32_t)ctx->gpr[4];
    uint32_t value = (uint32_t)ctx->gpr[5];
    if (!spurs) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410811; return; }
    if ((spurs & 0x7F) != 0) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410810; return; }
    if (wid >= 32 || value > 0xFF) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410802; return; }
    vm_write8(spurs + wid, (uint8_t)value);
    uc3_spu_exec_ready_count(spurs, wid, value);
    if (getenv("UC3_RCS")) {
        static int n = 0;
        if (n < 60 || value != 0) { n++;
            fprintf(stderr, "[rcs] ReadyCountStore spurs=0x%08X wid=%u value=%u\n", spurs, wid, value);
        }
    }
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

    /* Keep the guest-visible scheduler header coherent with the HLE state.
     * Workload policy modules DMA this header directly even though SPURS
     * management itself is handled on the host. */
    vm_write32(spurs + 0x6Cu, 0xFFFFFFFFu); /* workload flag: no receiver */
    vm_write8(spurs + 0x74u, 0x40u);        /* SPURS2 / 32 workloads */
    vm_write8(spurs + 0x76u, (uint8_t)state.nspus);
    vm_write8(spurs + 0x77u, 0xFFu);        /* no workload-flag receiver */

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
    if (getenv("UC3_MEDIAGATE")) { static int _gi=0; if(_gi<8){_gi++; fprintf(stderr, "[getinfo] cellSpursGetInfo(spurs=0x%08X info=0x%08X) state=%s\n", spurs, info, state?"TROUVE -> retour 0":"NULL -> retour INVAL (init media echoue!)"); } }
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
    /* The ninth integer argument is right-justified in its 8-byte PPC64 ABI
     * parameter slot. The value is therefore at sp+0x74, not sp+0x70. */
    vm_write32(attr + 0x24, vm_read32((uint32_t)ctx->gpr[1] + 0x74));
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

/* uc3_spu_exec.cpp — carte ring->(wid,pm,size,spurs) pour l'exécuteur
 * déterministe (UC3_SPU_EXEC). */
extern "C" void uc3_spu_exec_register_wkl(uint32_t, uint32_t, uint32_t,
                                          uint32_t, uint32_t);

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
    if (wid == 1u && getenv("UC3_POLICY_ELIGIBILITY_TRACE") != nullptr) {
        fprintf(stderr,
                "[spurs-workload-input] wid=%u attr=0x%08X nspus=%u "
                "priorities=%u/%u/%u/%u/%u/%u/%u/%u min=%u max=%u\n",
                wid, attr, state->nspus,
                vm_read8(attr + 0x18u), vm_read8(attr + 0x19u),
                vm_read8(attr + 0x1Au), vm_read8(attr + 0x1Bu),
                vm_read8(attr + 0x1Cu), vm_read8(attr + 0x1Du),
                vm_read8(attr + 0x1Eu), vm_read8(attr + 0x1Fu),
                vm_read32(attr + 0x20u), vm_read32(attr + 0x24u));
    }
    const uint32_t slot = wid & 15u;
    uint32_t wkl = spurs + (wid < 16u ? 0xB00u : 0x1000u) + slot * 0x20u;
    vm_write64(wkl + 0x00, pm);     /* addr (policy module EA) */
    vm_write64(wkl + 0x08, warg);   /* arg */
    vm_write32(wkl + 0x10, wsize);  /* size */
    for (uint32_t i = 0; i < 8u; ++i)
        vm_write8(wkl + 0x18u + i, vm_read8(attr + 0x18u + i));

    /* Publish the scheduler fields consumed by the SPURS kernel/policy ABI.
     * This only mirrors the workload registration supplied by the game; the
     * host executor still owns scheduling and completion. */
    const uint8_t max_contention =
        (uint8_t)(vm_read32(attr + 0x24u) > 8u ? 8u : vm_read32(attr + 0x24u));
    uint8_t packed_max = vm_read8(spurs + 0x50u + slot);
    if (wid < 16u)
        packed_max = (uint8_t)((packed_max & 0xF0u) | max_contention);
    else
        packed_max = (uint8_t)((packed_max & 0x0Fu) | (max_contention << 4));
    vm_write8(spurs + 0x50u + slot, packed_max);
    vm_write8(spurs + (wid < 16u ? 0x80u : 0xD0u) + slot, 2u);
    vm_write8(spurs + (wid < 16u ? 0x90u : 0xE0u) + slot, 0u);
    vm_write8(spurs + (wid < 16u ? 0xA0u : 0xF0u) + slot, 0u);
    const uint32_t workload_mask = 0x80000000u >> wid;
    vm_write32(spurs + 0xB0u, vm_read32(spurs + 0xB0u) | workload_mask);
    vm_write32(spurs + 0xB4u, vm_read32(spurs + 0xB4u) | workload_mask);
    printf("[cellSpurs] AddWorkloadWithAttribute(spurs=%08X, wid=%u, pm=%08X, arg=%08X%08X)\n",
           spurs, wid, pm, (uint32_t)(warg>>32), (uint32_t)warg);
    /* Carte ring->(wid,pm,size,spurs) pour l'exécuteur déterministe
     * (uc3_spu_exec.cpp, UC3_SPU_EXEC) — enregistrée à la source. */
    uc3_spu_exec_register_wkl((uint32_t)warg, wid, pm, wsize, spurs);
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
    static const bool trace = getenv("UC3_WKLINFO") != nullptr;
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

    /* Mirror CellSpursWorkloadInfo from the guest CellSpurs control block.
     * Returning an all-zero structure deadlocks callers which wait for the
     * active workload to acquire an SPU and later become idle. */
    memset(vm_base + info, 0, 0x30);
    uint32_t wkl = spurs + 0xB00u + wid * 0x20u;
    uint64_t policy_module = vm_read64(wkl + 0x00);
    uint64_t data = vm_read64(wkl + 0x08);
    uint32_t policy_size = vm_read32(wkl + 0x10);
    vm_write64(info + 0x00, data);
    for (uint32_t i = 0; i < 8; ++i)
        vm_write8(info + 0x08 + i, vm_read8(wkl + 0x18 + i));
    vm_write64(info + 0x10, policy_module);
    vm_write32(info + 0x18, policy_size);

    uint32_t ring = (uint32_t)data;
    uint32_t counters = ring >= 0x10000u ? vm_read32(ring + 0x40) : 0;
    uint16_t produced = (uint16_t)(counters >> 16);
    uint16_t consumed = (uint16_t)counters;
    uint8_t ready = vm_read8(spurs + wid);
    uint8_t contention = vm_read8(spurs + 0x20u + wid);
    uint8_t min_contention = vm_read8(spurs + 0x40u + wid);
    uint8_t max_contention = vm_read8(spurs + 0x50u + wid);
    uint8_t idle = vm_read8(spurs + 0x10u + wid);

    /* In the HLE scheduler there is no persistent SPU worker pool to retire
     * the remaining ready requests. Once this workload's command ring is
     * drained, expose the same quiescent state that the SPURS kernel would
     * publish after its workers go idle. */
    /* UC3's Edge producer resets the published producer cursor after its last
     * command, while the consumer cursor retains the completed position for
     * the current window (observed terminal state 0x00000001). The real SPURS
     * epilogue then retires the remaining scheduling requests. */
    bool ring_quiescent = produced == consumed ||
                          (produced == 0u && consumed != 0u && contention == 0u);
    if (ring && ring_quiescent && ready) {
        vm_write8(spurs + wid, 0);
        ready = 0;
    }

    /* EXPERIMENT UC3_WKL_IDLE : sur PS3 le kernel SPURS marque le workload IDLE
     * quand ses SPU se retirent apres drainage du ring. Notre HLE ne pose jamais
     * ce flag (idle reste 0). Si le jeu attend idle!=0 (workload retire) avant de
     * cellSpursShutdownWorkload -> transition, il attend a l'infini. Ici : quand
     * le ring est quiescent ET ready==0 (plus de travail), exposer idle comme le
     * ferait le kernel. Env-gate (chemin par defaut inchange = pas de regression).
     * Diagnostic pour tester l'hypothese "gate = flag idle du workload". */
    if (getenv("UC3_WKL_IDLE") && ring && ring_quiescent && ready == 0) {
        idle = 1;
        vm_write8(spurs + 0x10u + wid, 1);
    }
    vm_write8(info + 0x20, contention);
    vm_write8(info + 0x21, min_contention);
    vm_write8(info + 0x22, max_contention);
    vm_write8(info + 0x23, ready);
    vm_write8(info + 0x24, idle);
    if (trace) {
        struct WorkloadTraceState {
            uint32_t counters;
            uint8_t contention;
            uint8_t ready;
            uint8_t idle;
            bool initialized;
        };
        static WorkloadTraceState previous[32]{};
        WorkloadTraceState& old = previous[wid < 32u ? wid : 0u];
        if (wid < 32u && (!old.initialized || old.counters != counters ||
                          old.contention != contention || old.ready != ready ||
                          old.idle != idle)) {
            fprintf(stderr, "[wkl-info] wid=%u ring=%08X c=%04X/%04X cont=%u ready=%u idle=%u\n",
                    wid, ring, produced, consumed, contention, ready, idle);
            old = {counters, contention, ready, idle, true};
        }
    }
    ctx->gpr[3] = 0;
}

static void br_cellSpursCoreNoop(ppu_context* ctx) { ctx->gpr[3] = 0; }

/* Log SPURS workload-shutdown calls. These PRECEDE the frontend transition in
 * RPCS3 (ShutdownWorkload+WaitForWorkloadShutdown -> func_00BFA768 -> DEAD78 ->
 * Load vTex). If the game calls these, it reached the transition phase. */
static void br_cellSpursShutdownLog(ppu_context* ctx) {
    static int n = 0;
    fprintf(stderr, "[spurs-shutdown] call #%d r3=0x%08X r4=%u lr=0x%08X\n",
            ++n, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
            (uint32_t)ctx->lr);
    ctx->gpr[3] = 0;
}

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
    /* [lfq-probe] (UC3_LFQ_PROBE) read-only diagnostic. Established 2026-07-06
     * that the game DOES call the firmware LFQueue push (~40x at menu) with
     * valid edge-zlib work-items {src,dst,comp,exp,counter,flag,bits,skip} and
     * that the inflate below SUCCEEDS (0 "[edge-zlib]" failures) — so asset
     * decompression is NOT the menu gate. Gated off by default. */
    if (getenv("UC3_LFQ_PROBE")) {
        static std::atomic<int> s_lfq_calls{0};
        int nc = ++s_lfq_calls;
        if (nc <= 40)
            fprintf(stderr, "[lfq-probe] call#%d queue=0x%08X entry=0x%08X : "
                    "%08X %08X %08X %08X %08X %08X %08X %08X\n",
                    nc, (uint32_t)ctx->gpr[3], entry,
                    vm_read32(entry + 0x00), vm_read32(entry + 0x04),
                    vm_read32(entry + 0x08), vm_read32(entry + 0x0C),
                    vm_read32(entry + 0x10), vm_read32(entry + 0x14),
                    vm_read32(entry + 0x18), vm_read32(entry + 0x1C));
    }
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
    /* Size cap raised from 0x10000 (64KB) to 8MB: the 64KB cap silently
     * REJECTED every world/menu-scene geometry+texture chunk (> 64KB),
     * returning error 0x80410902 so the scene never decoded. Edge chunks go
     * up to a few MB. Validate source/output/end are within the guest VM
     * (4GB) instead of an arbitrary small cap, so large legit decodes run
     * while garbage work items are still rejected. */
    const uint32_t MAXDEC = 0x800000u; /* 8MB */
    if (!source || !output || !compressed_size || !expected_size ||
        compressed_size > MAXDEC || expected_size > MAXDEC ||
        (uint64_t)source + compressed_size > 0xFFFFFFF0ull ||
        (uint64_t)output + expected_size   > 0xFFFFFFF0ull ||
        (uint32_t)skip_begin + skip_end > expected_size) {
        static int s_iw = 0;
        if (s_iw < 40) { s_iw++;
            fprintf(stderr,
                "[edge-zlib] invalid work item src=%08X dst=%08X comp=%u expected=%u skip=%u+%u\n",
                source, output, compressed_size, expected_size, skip_begin, skip_end);
        }
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

    /* UC3_MANSCAN: detect whether the bin.psarc MANIFEST (file list, contains
     * "render-settings" and file paths) is decompressed, and whether it spans
     * to the block-2 entries like "dc1/messages.bin" (byte ~87924, block 2). */
    if (getenv("UC3_MANSCAN")) {
        const char* p = decoded.data();
        int n = (int)decoded.size();
        auto contains = [&](const char* needle)->int {
            int nl = (int)strlen(needle);
            for (int i = 0; i + nl <= n; i++) { int j=0; for(;j<nl;j++) if(p[i+j]!=needle[j]) break; if(j==nl) return i; }
            return -1;
        };
        int rs = contains("render-settings");
        int mg = contains("dc1/messages");
        if (rs >= 0 || mg >= 0) {
            static int ms = 0;
            if (ms < 20) { ms++;
                fprintf(stderr, "[manscan] decode dst=0x%08X exp=%u : render-settings@%d messages@%d\n",
                        output, expected_size, rs, mg);
            }
        }
    }

    /* UC3_LFQ_PROF: push-interval profiler. The boot race (children converge
     * vs the loadlevel pump window) is decided by how fast the PPU producer
     * emits these pushes. Prints a compact interval histogram every 200
     * pushes: if intervals cluster at ~15.6ms the producer's inter-chunk wait
     * is quantized by the Windows timer (UC3_HIRES_TIMER should collapse it);
     * ~2ms clusters point at the event-flag cv wait; sub-ms = healthy. */
    if (getenv("UC3_LFQ_PROF")) {
        static LARGE_INTEGER s_freq, s_last;
        static int s_n = 0;
        static int s_h[5]; /* <1ms, 1-4ms, 4-10ms, 10-20ms, >20ms */
        LARGE_INTEGER now;
        if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
        QueryPerformanceCounter(&now);
        if (s_n > 0) {
            double ms = (double)(now.QuadPart - s_last.QuadPart) * 1000.0 / (double)s_freq.QuadPart;
            int b = ms < 1.0 ? 0 : ms < 4.0 ? 1 : ms < 10.0 ? 2 : ms < 20.0 ? 3 : 4;
            s_h[b]++;
        }
        s_last = now;
        s_n++;
        if ((s_n % 200) == 0)
            fprintf(stderr, "[lfq-prof] n=%d intervals: <1ms=%d 1-4=%d 4-10=%d 10-20=%d >20=%d (tid=%lu)\n",
                    s_n, s_h[0], s_h[1], s_h[2], s_h[3], s_h[4], GetCurrentThreadId());
    }
    if (counter) {
        uint32_t pending = vm_read32(counter);
        if (pending) vm_write32(counter, pending - 1);
        if (getenv("UC3_LFQ_PROBE")) {
            static int nc = 0;
            fprintf(stderr, "[lfq-counter] push#%d counter@0x%08X %u->%u tag=%d evflag=0x%08X bits=0x%04X %s\n",
                    ++nc, counter, pending, pending ? pending - 1 : 0,
                    (counter_tagged & 1), event_flag, event_bits,
                    (pending == 1 && (counter_tagged & 1)) ? "*** REACHES 0 -> event fires" : "");
        }
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
    if (getenv("UC3_EVT_LOG")) {
        static int wn = 0;
        if (wn < 40) { wn++;
            fprintf(stderr, "[evt] EFlagWait flag=0x%08X req=0x%04X mode=%u cur=0x%04X block=%d\n",
                    flag, requested, mode, vm_read16(flag), block); }
    }
    if (flag == 0x3115E980u && getenv("UC3_EVTBT")) {
        extern void uc3_dump_host_backtrace(const char*);
        static int bn = 0;
        if (++bn <= 2) uc3_dump_host_backtrace("edgezlib-event-waiter");
    }
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

/* Walk the guest PPC stack (back chain at *(r1), std-ABI return addr at
 * frame+0x10) and print plausible code return addresses. Reveals the caller
 * chain even when lr==0 (indirect/OPD dispatch). */
static void uc3_dump_guest_backtrace(ppu_context* ctx, const char* tag) {
    uint32_t fp = (uint32_t)ctx->gpr[1];
    fprintf(stderr, "[bt] %s guest stack (r1=0x%08X lr=0x%08X):\n", tag, fp,
            (uint32_t)ctx->lr);
    for (int i = 0; i < 16 && fp >= 0x10000u; ++i) {
        uint32_t next = vm_read32(fp);
        uint32_t ret  = vm_read32(fp + 0x10u);
        if (ret >= 0x10000u && ret < 0x00E00000u)
            fprintf(stderr, "[bt]   #%2d fp=0x%08X ret=func_%08X\n", i, fp, ret);
        if (next <= fp || next < 0x10000u) break;
        fp = next;
    }
}
static void br_cellSpursCreateTasksetWithAttribute(ppu_context* ctx) {
    if (getenv("UC3_TASKSET_LOG")) {
        static int _n = 0; ++_n;
        fprintf(stderr, "[taskset] CreateTasksetWithAttribute #%d spurs=0x%08X "
                "taskset=0x%08X attr=0x%08X lr=0x%08X\n", _n,
                (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
                (uint32_t)ctx->gpr[5], (uint32_t)ctx->lr);
        if (_n <= 4) uc3_dump_guest_backtrace(ctx, "CreateTasksetWithAttribute");
    }
    ctx->gpr[3] = 0;
}

static void br_cellSpursCreateTaskset2(ppu_context* ctx) {
    static std::mutex taskset_mutex;
    static uint32_t next_wid = 0;
    uint32_t spurs = (uint32_t)ctx->gpr[3];
    uint32_t taskset = (uint32_t)ctx->gpr[4];
    uint32_t attr = (uint32_t)ctx->gpr[5];
    if (getenv("UC3_TASKSET_LOG")) {
        static int _n = 0; ++_n;
        fprintf(stderr, "[taskset] CreateTaskset2 #%d spurs=0x%08X taskset=0x%08X "
                "attr=0x%08X lr=0x%08X\n", _n, spurs, taskset, attr,
                (uint32_t)ctx->lr);
        if (_n <= 4) uc3_dump_guest_backtrace(ctx, "CreateTaskset2");
    }
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
        spu_func_00004000(ctx);          /* (manager path retired; edge0 in build) */
        fprintf(stderr, "[spu-task] <<< returned status=0x%X pc=0x%05X outmbox=%u/0x%08X\n",
                ctx->status, ctx->pc & SPU_LS_MASK,
                ctx->ch_out_mbox.count, ctx->ch_out_mbox.value);
    } else {
        fprintf(stderr, "[spu-task] <<< aborted by runaway guard (job queue empty/"
                        "invalid — A2 work). No crash.\n");
    }
    spu_abort_arm(0);
}

/* ---- Edge geometry job executor (Phase 11 / option 1) -------------------- *
 * Run a lifted Edge job binary. The Jobbin2 header stores the payload offset
 * at +0x1C and the LS load address at +0x28. */
enum : int {
    UC3_SPU_IMAGE_EDGE = 1,
    UC3_SPU_IMAGE_POLICY_EDGE = 2,
    UC3_SPU_IMAGE_SSCULL = 3,   /* wkl[6] screen-space light culling job */
    UC3_SPU_IMAGE_WKL2GEO = 4,  /* wkl[2] asset-geometry job program */
    UC3_SPU_IMAGE_SSCULL_JOB2 = 5, /* nested screen-space-culling Jobbin2 */
};

/* cellSpursJob2 runtime-helper HLE (per docs/SPU_FALLBACK.md). The Edge job
 * trampoline at LS 0x4000 calls back into SPURS runtime helpers via a context
 * vtable: it loads a context pointer from LS[0x5110], then bisl's the function
 * pointers at context+52, +60, +68. The real framework (policy/kernel) provides
 * these; standalone we register C implementations at synthetic LS addresses and
 * point the vtable at them. Minimal contract: report CELL_OK in gpr[3]. */
#define UC3_JOB2_CTX     0x6000u   /* CellSpursJobContext2 we build in LS */
#define UC3_JOB2_HLP_52  0x7F00u
#define UC3_JOB2_HLP_60  0x7F10u
#define UC3_JOB2_HLP_68  0x7F20u
#define UC3_POLICY_COMMAND_LSA 0x12A0u
#define UC3_POLICY_SLOTS_LSA   0x12F0u
#define UC3_POLICY_CONTENT_LSA 0x1320u
#define UC3_POLICY_OVERLAY_BEGIN 0x4A80u
#define UC3_POLICY_OVERLAY_END   0x5B10u
#define UC3_POLICY_OVERLAY_SIZE  (UC3_POLICY_OVERLAY_END - UC3_POLICY_OVERLAY_BEGIN)
static uint8_t s_feec80_code_overlay[UC3_POLICY_OVERLAY_SIZE];
static bool s_feec80_code_overlay_valid = false;

/* --- CORRECT-data cellSpursJob2 context HLE ---------------------------------
 * The real feec80 job entry (cellSpursJobMain2) is LS 0xA2A8. It reads a context
 * pointer from LS[0xC240] and bisl's fn-ptrs at ctx+0x34 (get-input-buffer,
 * called with r4=index 1..3) and ctx+0x44 (init). ctx+0x20 points at job params.
 * The resident SPU job2 kernel builds this; standalone we build it in free LS and
 * point the vtable at C handlers registered at synthetic LS addresses. */
#define UC3_F80_CTX      0x20000u   /* CellSpursJobContext2 (free LS, above image) */
#define UC3_F80_PARAMS   0x20100u   /* job params block (ctx+0x20 -> here) */
#define UC3_F80_BUFBASE  0x24000u   /* input buffers returned by get-input-buffer */
#define UC3_F80_HLP_GET  0x0300u    /* synthetic LS addr: get-input-buffer handler */
#define UC3_F80_HLP_INIT 0x0308u    /* synthetic LS addr: init handler */
static uint32_t s_feec80_job_desc = 0;  /* set by uc3_run_policy_job (0x20E42E00) */
/* Contexte du dispatch policy en cours — echantillonne par le job-guard au
 * timeout (diagnostic du pc du job pendu, style UC3_F80_WATCH). */
static std::atomic<spu_context*> g_uc3_policy_ctx_live{nullptr};

/* ctx+0x34 : get-input-buffer(index=r4). Returns a deterministic LS range in r3.
 * The PPU producer func_009E69C0 proves one initial input DMA (EA at command
 * +0x24, size 0x300) and one zero-backed scratch command at +0x28. */
/* DMA a guest EA into an LS buffer (like the job2 kernel loading an input) and
 * return the LS address, or 0 if the EA is implausible. */
static uint32_t uc3_f80_dma_in(spu_context* ctx, uint32_t ls_buf, uint32_t ea,
                               uint32_t sz) {
    if (ea < 0x10000u || ea >= 0x30000000u || sz == 0 || sz > 0x2000u) return 0;
    if (ls_buf + sz > SPU_LS_SIZE) return 0;
    memcpy(ctx->ls + ls_buf, vm_base + ea, sz);
    return ls_buf;
}

static void uc3_f80_get_input_buffer(spu_context* ctx) {
    uint32_t index = ctx->gpr[4]._u32[0];
    uint32_t buf   = UC3_F80_BUFBASE + index * 0x2000u;
    /* Rebuild the descriptor-backed Job2 buffers in deterministic LS ranges.
     * Buffer 1 is the custom in/out region, buffer 2 the explicit 0x300-byte
     * input DMA, and buffer 3 the zero-backed scratch command. */
    uint32_t ctrl = s_feec80_job_desc ? vm_read32(s_feec80_job_desc + 0x50u) : 0;
    uint32_t source_ea = 0;
    uint32_t source_size = 0;
    if (buf + 0x2000u <= SPU_LS_SIZE)
        memset(ctx->ls + buf, 0, 0x2000u);
    switch (index) {
        case 1:
            source_ea = vm_read32(s_feec80_job_desc + 0x48u);
            source_size = vm_read32(s_feec80_job_desc + 0x4Cu);
            break;
        case 2:
            source_ea = vm_read32(s_feec80_job_desc + 0x24u);
            source_size = 0x300u;
            break;
        case 3:
            /* func_00D4E594 emits command 0x00000440 with EA=0. */
            break;
        default: break;
    }
    if (source_ea && source_size)
        uc3_f80_dma_in(ctx, buf, source_ea, source_size);
    static int n = 0;
    if (n < 8) { n++;
        fprintf(stderr, "[f80-getbuf] index=%u EA=0x%08X size=0x%X -> "
                        "LS 0x%05X (ctrl=0x%08X)\n",
                index, source_ea, source_size, buf, ctrl); }
    ctx->gpr[3]._u32[0] = buf;
}

/* ctx+0x44 : init. Return CELL_OK. */
static void uc3_f80_init(spu_context* ctx) {
    ctx->gpr[3]._u32[0] = 0;
}

static void uc3_job2_helper(spu_context* ctx);

static void uc3_feec80_dispatch(spu_context* ctx) {
    if (s_feec80_code_overlay_valid) {
        memcpy(ctx->ls + UC3_POLICY_OVERLAY_BEGIN, s_feec80_code_overlay,
               UC3_POLICY_OVERLAY_SIZE);
        s_feec80_code_overlay_valid = false;
    }
    /* Build the cellSpursJob2 context the resident kernel would provide, then enter
     * the real job main (cellSpursJobMain2 @ LS 0xA2A8). It reads ctx from LS[0xC240]
     * and calls the vtable fn-ptrs at ctx+0x34 (get-input-buffer) / ctx+0x44 (init),
     * which we point at C handlers registered at synthetic LS addresses. */
    if (!s_feec80_job_desc) s_feec80_job_desc = 0x20E42E00u;  /* chosen jobchain job */
    spu_register_function(UC3_F80_HLP_GET,  uc3_f80_get_input_buffer);
    spu_register_function(UC3_F80_HLP_INIT, uc3_f80_init);
    spu_ls_write32(ctx, 0xC240,             UC3_F80_CTX);          /* context ptr */
    spu_ls_write32(ctx, UC3_F80_CTX + 0x20, UC3_F80_PARAMS);       /* params ptr */
    spu_ls_write32(ctx, UC3_F80_CTX + 0x34, UC3_F80_HLP_GET);      /* get-input-buffer */
    spu_ls_write32(ctx, UC3_F80_CTX + 0x44, UC3_F80_HLP_INIT);     /* init */

    /* Experiment (PLAN_SPURS_RUNTIME.md Étape 1): entering at 0xA2A8 halts because
     * its first act is `bi 0x4458`, skipping the setup chain 0x43F0->0x4408->0x4420->
     * 0x4424 that initialises the DMA-loop regs r13/r14/r15/r63-r68. The job BODY
     * (0x4000) runs that setup itself; it only needs the I/O buffer / job-header
     * pointers (r83/r85/r87/r89). Pre-fill them and enter 0x4000 directly, logging
     * the halt PC to reveal the next missing register. Gated so the default build
     * (which enters 0xA2A8) is unchanged. */
    if (getenv("UC3_F80_ENTRY4000") != nullptr) {
        /* The full feec80 image (704 lifted functions) includes the real Edge geometry lib at
         * LS 0x7xxx-0x9xxx. uc3_register_job2_runtime_helpers stubbed those to
         * return-0 (a legacy of when the lib was unlifted) — which replaces the
         * real geometry with no-ops. Re-register the real lifted functions so
         * 0x4020's call to 0x7B58 (Edge geom main) runs the actual code. */
        /* NB: le registre est premier-arrive-gagne ; l'anti-spin kernel-helper
         * (0x0A78...) est installe AVANT les tables dans uc3_run_policy_job. */
        if (getenv("UC3_F80_KEEP_STUBS") == nullptr)
            feec80_spu_recomp_register();
        /* Log the INCOMING register state the policy left (this ctx is the policy's,
         * so r24/r25/r26 job-args + r83/r85/r89 buffers are whatever the policy set). */
        fprintf(stderr, "[f80-entry4000] INCOMING r24=%08X r25=%08X r26=%08X | "
                        "r83=%08X r85=%08X r86=%08X r89=%08X r93=%08X r95=%08X\n",
                ctx->gpr[24]._u32[0], ctx->gpr[25]._u32[0], ctx->gpr[26]._u32[0],
                ctx->gpr[83]._u32[0], ctx->gpr[85]._u32[0], ctx->gpr[86]._u32[0],
                ctx->gpr[89]._u32[0], ctx->gpr[93]._u32[0], ctx->gpr[95]._u32[0]);
        auto getbuf = [&](uint32_t index) -> uint32_t {
            ctx->gpr[4]._u32[0] = index;
            uc3_f80_get_input_buffer(ctx);
            return ctx->gpr[3]._u32[0];
        };
        uint32_t b1 = getbuf(1), b2 = getbuf(2), b3 = getbuf(3);
        uint32_t out_ea = vm_read32(s_feec80_job_desc + 0x48u);
        uint32_t out_size = vm_read32(s_feec80_job_desc + 0x4Cu);
        bool out_valid = out_ea >= 0x10000u && out_size > 0 && out_size <= 0x2000u &&
                         b1 + out_size <= SPU_LS_SIZE &&
                         static_cast<uint64_t>(out_ea) + out_size <= 0x100000000ull;
        std::vector<uint8_t> out_before;
        if (out_valid)
            out_before.assign(ctx->ls + b1, ctx->ls + b1 + out_size);
        /* Job header in LS: copy the chosen descriptor so r87+0/0x10/0x20 read real
         * fields (0x40A0/0x4078 read the job header via r87). */
        for (uint32_t i = 0; i < 0x60u; ++i)
            ctx->ls[UC3_F80_PARAMS + i] = vm_read8(s_feec80_job_desc + i);
        /* 0x4020 issues four 2 KiB merge calls. Buffer 1 is descriptor-backed
         * output/inout, buffer 2 its explicit input mask, and buffer 3 scratch. */
        ctx->gpr[85]._u32[0] = b1;              /* output/inout                    */
        ctx->gpr[86]._u32[0] = b2;              /* explicit input/mask             */
        ctx->gpr[95]._u32[0] = b3;              /* zero-backed scratch             */
        ctx->gpr[93]._u32[0] = b1;              /* geom call 4 primary             */
        ctx->gpr[83]._u32[0] = b2;              /* common context (0x4020 r6/r5)   */
        ctx->gpr[89]._u32[0] = b3;
        ctx->gpr[90]._u32[0] = b3;
        ctx->gpr[91]._u32[0] = b3;
        ctx->gpr[87]._u32[0] = UC3_F80_PARAMS;  /* job header ptr (0x40A0 reads)   */
        fprintf(stderr, "[f80-entry4000] enter 0x4000 r85/86/95/93="
                        "0x%05X/0x%05X/0x%05X/0x%05X r87=0x%05X\n",
                b1, b2, b3, b1, (unsigned)UC3_F80_PARAMS);
        /* One-shot snapshot for the standalone feec80 harness (UC3_F80_DUMP):
         * exact LS + the 128-bit GPR file + the guest regions the job may DMA,
         * so spu_gen/edge_feec80/feec80_harness.c can replay this dispatch and
         * iterate on the Edge geometry input in seconds (no 90s boot). */
        if (getenv("UC3_F80_DUMP") != nullptr) {
            if (FILE* fp = fopen("feec80_ls.bin", "wb")) {
                fwrite(ctx->ls, 1, SPU_LS_SIZE, fp); fclose(fp); }
            if (FILE* fp = fopen("feec80_gpr.bin", "wb")) {
                fwrite(ctx->gpr, 16, 128, fp); fclose(fp); }
            /* Guest regions (ea,size) the job's DMA/graph touches. */
            static const uint32_t regs[][2] = {
                {0x20E42E00u, 0x100}, {0x013A75F8u, 0x100}, {0x012FC730u, 0x100},
                {0x20E34190u, 0x1000}, {0x313ED700u, 0x400}, {0x313F71E4u, 0x400},
                {0x313F1204u, 0x400}, {0x313B2700u, 0x400}, {0x013A72F0u, 0x440},
                {0x011B52E0u, 0x800}, {0x0129453Du, 0x1000},
            };
            if (FILE* fp = fopen("feec80_vm.bin", "wb")) {
                for (auto& r : regs) {
                    fwrite(&r[0], 4, 1, fp); fwrite(&r[1], 4, 1, fp);
                    for (uint32_t i = 0; i < r[1]; ++i) {
                        uint8_t b = vm_read8(r[0] + i); fwrite(&b, 1, 1, fp);
                    }
                }
                fclose(fp);
            }
            fprintf(stderr, "[f80-dump] wrote feec80_ls.bin (256K) + feec80_gpr.bin + "
                            "feec80_vm.bin (%zu regions)\n", sizeof(regs)/sizeof(regs[0]));
        }
        /* Integration brick (UC3_F80_REALLS): AFTER the get-input-buffer/register setup
         * (which would otherwise zero 0x26000), load the REAL Edge geometry input regions
         * (0x10000..0x2C000) from the PS3-dump reference real_ls.bin — a stand-in for the
         * not-yet-reconstructed decode passes. Makes the LIVE port produce geometry
         * end-to-end. r85/r86/etc. already point into this region. */
        /* [STEP 0 fork] UC3_F80_INPUT: compare the DMA'd input (LS 0x10000, from
         * the descriptor's decoded-geometry EA) to real_ls.bin BEFORE substitution.
         * If they match (or LS has real decoded data) -> routing already works and
         * real_ls.bin is redundant. If LS is zero/garbage -> the intermediate wkl2
         * decode pass genuinely didn't produce it. Decides STEP 1. */
        if (getenv("UC3_F80_INPUT") != nullptr) {
            static int _n=0; if(_n<4){_n++;
                uint32_t in_ea = vm_read32(s_feec80_job_desc + 0x48u);
                uint32_t in_sz = vm_read32(s_feec80_job_desc + 0x4Cu);
                /* LS 0x10000 = où feec80 attend l'entrée; compare aux 32 premiers octets */
                uint32_t nz_ls=0; for(uint32_t i=0;i<0x1C000u;i++) if(ctx->ls[0x10000u+i]) nz_ls++;
                fprintf(stderr,"[f80-input] descEA=0x%08X sz=0x%X | LS0x10000 nonzero=%u/0x1C000 first8=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                    in_ea,in_sz,nz_ls,ctx->ls[0x10000],ctx->ls[0x10001],ctx->ls[0x10002],ctx->ls[0x10003],
                    ctx->ls[0x10004],ctx->ls[0x10005],ctx->ls[0x10006],ctx->ls[0x10007]);
                if (in_ea>=0x10000u && in_ea<0x30000000u)
                    fprintf(stderr,"[f80-input] guest@descEA first8=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                        vm_read8(in_ea),vm_read8(in_ea+1),vm_read8(in_ea+2),vm_read8(in_ea+3),
                        vm_read8(in_ea+4),vm_read8(in_ea+5),vm_read8(in_ea+6),vm_read8(in_ea+7));
            }
        }
        if (getenv("UC3_F80_REALLS") != nullptr) {
            if (FILE* rf = fopen("real_ls.bin", "rb")) {
                static uint8_t rls[SPU_LS_SIZE];
                if (fread(rls, 1, SPU_LS_SIZE, rf) == SPU_LS_SIZE)
                    memcpy(ctx->ls + 0x10000u, rls + 0x10000u, 0x1C000u);
                fclose(rf);
                fprintf(stderr, "[f80-realls] loaded real geometry LS 0x10000..0x2C000\n");
            } else fprintf(stderr, "[f80-realls] real_ls.bin not found\n");
        }
        /* Post-REALLS snapshot (UC3_F80_DUMP2): the pre-REALLS dump (feec80_ls.bin)
         * is config-only and cannot decode geometry in the harness. Dump AFTER
         * REALLS + the register/buffer setup so the harness reproduces the real
         * decode (produces the 0xEC0 output), making it a faithful bench for the
         * wkl[2] segment-decode experiment (swap the region for a segment). */
        if (getenv("UC3_F80_DUMP2") != nullptr) {
            if (FILE* fp = fopen("feec80_ls_decode.bin", "wb")) {
                fwrite(ctx->ls, 1, SPU_LS_SIZE, fp); fclose(fp); }
            if (FILE* fp = fopen("feec80_gpr_decode.bin", "wb")) {
                fwrite(ctx->gpr, 16, 128, fp); fclose(fp); }
            fprintf(stderr, "[f80-dump2] post-REALLS decode snapshot -> "
                            "feec80_ls_decode.bin + feec80_gpr_decode.bin\n");
        }
        /* Hang diagnosis (UC3_F80_WATCH): the job completed on 2026-07-06
         * (halt pc=0x41E0) but never returns since the Jul-08 re-lift (722
         * registered funcs). Sample ctx->pc from a sibling thread — every
         * lifted inter-function transfer updates it, so the samples name the
         * spinning function without a full trace. */
        std::shared_ptr<std::atomic<bool>> f80_watch_done;
        if (getenv("UC3_F80_WATCH") != nullptr) {
            f80_watch_done = std::make_shared<std::atomic<bool>>(false);
            std::thread([ctx, done = f80_watch_done]{
                for (int k = 0; k < 30 && !done->load(); ++k) {
                    for (int j = 0; j < 20 && !done->load(); ++j)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if (done->load()) break;
                    fprintf(stderr, "[f80-watch] t=%ds pc=0x%05X status=0x%X "
                            "r0=%08X r1=%08X r3=%08X\n", (k + 1) * 2,
                            ctx->pc & SPU_LS_MASK, ctx->status,
                            ctx->gpr[0]._u32[0], ctx->gpr[1]._u32[0],
                            ctx->gpr[3]._u32[0]);
                }
            }).detach();
        }
        ctx->pc = 0x4000u;
        feec80_spu_func_00004000(ctx);
        if (f80_watch_done) f80_watch_done->store(true);
        uint32_t local_nz = 0;
        uint32_t local_changed = 0;
        if (out_valid) {
            for (uint32_t i = 0; i < out_size; ++i) {
                local_nz += ctx->ls[b1 + i] != 0;
                local_changed += ctx->ls[b1 + i] != out_before[i];
            }
            fprintf(stderr, "[f80-entry4000] LS output candidate 0x%05X -> "
                            "EA 0x%08X size=0x%X nonzero=%u changed=%u\n",
                    b1, out_ea, out_size, local_nz, local_changed);
            if (getenv("UC3_F80_PUTBACK") != nullptr) {
                memcpy(vm_base + out_ea, ctx->ls + b1, out_size);
                fprintf(stderr, "[f80-entry4000] PUTBACK LS 0x%05X -> "
                                "EA 0x%08X size=0x%X\n",
                        b1, out_ea, out_size);
            }
        } else {
            fprintf(stderr, "[f80-entry4000] invalid output candidate "
                            "EA=0x%08X size=0x%X LS=0x%05X\n",
                    out_ea, out_size, b1);
        }
        /* Did the optional PUT populate the descriptor's guest output? */
        uint32_t out_nz = 0;
        if (out_valid) {
            for (uint32_t i = 0; i < out_size; ++i)
                if (vm_read8(out_ea + i)) { out_nz = 1; break; }
        }
        fprintf(stderr, "[f80-entry4000] halt/return pc=0x%05X status=0x%X "
                        "output 0x%08X = %s\n",
                ctx->pc & SPU_LS_MASK, ctx->status, out_ea,
                out_nz ? "HAS DATA" : "still zero");
        return;
    }

    ctx->pc = 0xA2A8u;
    feec80_spu_func_0000A2A8(ctx);
}

/* Decode one landed WKL2 Edge segment with the already validated lifted
 * FEEC80 path. This is a host fallback for the segment payload, not a SPURS
 * completion shim: the caller remains responsible for returning through the
 * lifted Job Manager epilogue. */
static bool uc3_decode_wkl2_segment(spu_context* ctx) {
    if (!ctx || !s_feec80_job_desc) return false;

    const uint32_t B1 = 0x26000u;
    const uint32_t B2 = 0x28000u;
    const uint32_t B3 = 0x2A000u;
    const uint32_t PAR = 0x20100u;
    memcpy(ctx->ls + B3, ctx->ls + 0x4000u + 0x1800u, 0x2000u);
    memcpy(ctx->ls + B2, ctx->ls + 0x4000u + 0x3800u, 0x2000u);
    for (uint32_t i = 0; i < 0x60u; ++i)
        ctx->ls[PAR + i] = ctx->ls[0x4000u + i];
    ctx->gpr[85]._u32[0] = B1;
    ctx->gpr[86]._u32[0] = B2;
    ctx->gpr[95]._u32[0] = B3;
    ctx->gpr[93]._u32[0] = B1;
    ctx->gpr[83]._u32[0] = B2;
    ctx->gpr[89]._u32[0] = B3;
    ctx->gpr[90]._u32[0] = B3;
    ctx->gpr[91]._u32[0] = B3;
    ctx->gpr[87]._u32[0] = PAR;
    spu_dma_set_deferred(ctx, 0);

    uint8_t out_before[0xEC0];
    memcpy(out_before, ctx->ls + B1, sizeof(out_before));
    ctx->status = SPU_STATUS_RUNNING;
    ctx->pc = 0x4000u;
    feec80_spu_func_00004000(ctx);

    uint32_t nz = 0, changed = 0;
    for (uint32_t i = 0; i < sizeof(out_before); ++i) {
        nz += ctx->ls[B1 + i] != 0;
        changed += ctx->ls[B1 + i] != out_before[i];
    }

    uint32_t output_ea = 0, output_size = 0;
    for (uint32_t offset = 0; offset < 0x50u; offset += 8u) {
        uint32_t ea = vm_read32(s_feec80_job_desc + offset + 4u);
        uint32_t size = vm_read16(s_feec80_job_desc + offset + 2u);
        if (ea >= 0x20E00000u && ea < 0x21000000u && size) {
            output_ea = ea;
            output_size = size;
            break;
        }
    }

    uint32_t guest_nz = 0, matches = 0;
    if (output_ea) {
        for (uint32_t i = 0; i < sizeof(out_before); ++i) {
            uint8_t guest = vm_read8(output_ea + i);
            guest_nz += guest != 0;
            matches += guest == ctx->ls[B1 + i];
        }
    }
    fprintf(stderr, "[wkl2-decode] feec80 on segment: out nz=%u changed=%u "
                    "| dmaList out EA=0x%08X sz=0x%X | guest nz=%u "
                    "match/0xEC0=%u\n",
            nz, changed, output_ea, output_size, guest_nz, matches);
    if (getenv("UC3_F80_PUTBACK") && output_ea >= 0x10000u &&
        (uint64_t)output_ea + sizeof(out_before) <= 0x100000000ull) {
        memcpy(vm_base + output_ea, ctx->ls + B1, sizeof(out_before));
        fprintf(stderr, "[wkl2-decode] PUTBACK 0xEC0 -> EA 0x%08X\n",
                output_ea);
    }
    return true;
}

/* `policy_spu_func_000031E0` performs a real bisl into the landed content and
 * then continues through its Job Manager epilogue. Previously the unknown
 * target halted, the generated caller ran its epilogue, and only afterwards
 * did uc3_run_policy_job decode the segment. This handler places the fallback
 * at the bisl target so the ordering is content -> epilogue, as on SPU.
 *
 * The fallback's observable contract is its guest output. Preserve the entire
 * resident policy context around it so FEEC80 scratch LS/GPR writes cannot
 * corrupt the caller that must publish completion. */
static thread_local bool s_wkl2_sync_return_ran = false;
static void uc3_wkl2_sync_return(spu_context* ctx) {
    std::unique_ptr<spu_context> saved(new spu_context);
    memcpy(saved.get(), ctx, sizeof(*ctx));
    const uint32_t target = ctx->pc & SPU_LS_MASK;
    const bool decoded = uc3_decode_wkl2_segment(ctx);
    memcpy(ctx, saved.get(), sizeof(*ctx));
    s_wkl2_sync_return_ran = decoded;
    fprintf(stderr, "[wkl2-return] target=0x%05X decoded=%u; returning to "
                    "lifted policy epilogue r0=0x%05X\n",
            target, decoded ? 1u : 0u, ctx->gpr[0]._u32[0] & SPU_LS_MASK);
}

/* Raw FEEC80 jobs do not carry the Jobbin2 entry-offset header expected by the
 * policy preamble. The preamble therefore computes a non-LS bisl target from
 * the first instruction at +0x10. Register that exact raw target as the
 * content-call boundary: execute the lifted job, preserve its guest/DMA side
 * effects, then restore the resident policy context so its real epilogue owns
 * ready-list completion. */
static thread_local bool s_feec80_sync_return_ran = false;
static void uc3_feec80_sync_return(spu_context* ctx) {
    std::unique_ptr<spu_context> saved(new spu_context);
    memcpy(saved.get(), ctx, sizeof(*ctx));
    const uint32_t raw_target = ctx->pc;

    ctx->status = SPU_STATUS_RUNNING;
    uc3_feec80_dispatch(ctx);
    const uint32_t content_pc = ctx->pc & SPU_LS_MASK;
    const uint32_t content_status = ctx->status;
    const bool completed = content_status == SPU_STATUS_RUNNING &&
                           content_pc == 0x41E0u;
    memcpy(ctx, saved.get(), sizeof(*ctx));
    saved.reset();

    s_feec80_sync_return_ran = completed;
    fprintf(stderr, "[feec80-return] raw=0x%08X ls=0x%05X content "
                    "pc=0x%05X status=0x%X completed=%u; policy-r0=0x%05X\n",
            raw_target, raw_target & SPU_LS_MASK, content_pc, content_status,
            completed ? 1u : 0u, ctx->gpr[0]._u32[0] & SPU_LS_MASK);

    /* A failed content job must never fall through an epilogue that would
     * publish success. The runner already owns this guarded unwind boundary. */
    if (!completed) {
        ctx->pc = raw_target;
        ctx->status = SPU_STATUS_STOPPED_BY_HALT;
        if (g_spu_abort_armed)
            longjmp(g_spu_abort_buf, 1);
    }
}

/* The first resident-kernel interface used by policy helper 0x3660 is
 * cellSpursModuleExit. It is non-returning: the kernel receives control at the
 * per-context exit address in LS 0x1E0. Use the runner's existing guarded
 * setjmp boundary to unwind the lifted C frames, which must not fall through
 * into helper 0x3678 after a successful module exit. */
static constexpr int UC3_SPU_JUMP_MODULE_EXIT = 2;
static thread_local bool s_policy_module_exit_ran = false;
static thread_local bool s_policy_last_run_completed = false;

extern "C" int uc3_policy_last_run_completed(void) {
    return s_policy_last_run_completed ? 1 : 0;
}

static void uc3_spurs_module_exit_hle(spu_context* ctx) {
    const uint32_t exit_to_kernel =
        spu_ls_read32(ctx, 0x1E0u) & SPU_LS_MASK;
    s_policy_module_exit_ran = true;
    ctx->pc = exit_to_kernel;
    ctx->status = SPU_STATUS_STOPPED_BY_STOP;
    fprintf(stderr, "[spurs-module-exit] wid=%u exit=0x%05X sp=0x%05X "
                    "cmd=0x%08X\n",
            spu_ls_read32(ctx, 0x1DCu) & 31u, exit_to_kernel,
            ctx->gpr[1]._u32[0] & SPU_LS_MASK,
            spu_ls_read32(ctx, UC3_POLICY_COMMAND_LSA));
    if (g_spu_abort_armed)
        longjmp(g_spu_abort_buf, UC3_SPU_JUMP_MODULE_EXIT);
}

static void uc3_job2_helper(spu_context* ctx) {
    static int n = 0;
    if (n < 12) { n++;
        fprintf(stderr, "[job2-helper] called pc=0x%05X gpr[3]=0x%08X gpr[4]=0x%08X\n",
                ctx->pc & SPU_LS_MASK, ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0]); }
    ctx->gpr[3]._u32[0] = 0; /* CELL_OK */
}

static void uc3_register_job2_runtime_helpers() {
    /* Direct call targets referenced by the captured FEEC80 image but owned
     * by the resident SPURS policy/kernel and Job Manager runtime. */
    static constexpr uint32_t targets[] = {
        0x0A78u, 0x1FF8u, 0x2E58u, 0x38F0u, 0x3E10u,
        0x7090u, 0x7100u, 0x7B58u, 0x7BB8u, 0x7E80u,
        0x8080u, 0x8338u, 0x83C8u, 0x8FA8u, 0x9250u, 0x96F8u,
    };
    for (uint32_t target : targets)
        spu_register_function(target, uc3_job2_helper);
}

static void uc3_run_edge_job(uint32_t ea_binary) {
    static spu_context s_ejc;
    spu_context* ctx = &s_ejc;
    spu_context_init(ctx, 0);
    const uint8_t* b = vm_base + ea_binary;
    /* validate Edge job header (C0DEC0DE at +0x20) */
    if (spu_be32(b + 0x20) != 0xC0DEC0DEu) {
        fprintf(stderr, "[edge-job] no C0DEC0DE magic at 0x%08X (got %08X)\n",
                ea_binary, spu_be32(b + 0x20));
        return;
    }
    uint32_t payload_offset = spu_be32(b + 0x1C);     /* edge0: 0x50 */
    uint32_t load_addr = spu_be32(b + 0x28);           /* edge0: 0x4000 */
    uint32_t code_size = spu_be32(b + 0x14);           /* LS image size */
    if (payload_offset < 0x30 || payload_offset > 0x1000) {
        fprintf(stderr, "[edge-job] invalid payload offset 0x%X at 0x%08X\n",
                payload_offset, ea_binary);
        return;
    }
    if (load_addr + code_size > SPU_LS_SIZE) code_size = SPU_LS_SIZE - load_addr;
    memcpy(ctx->ls + load_addr, b + payload_offset, code_size);
    ctx->gpr[1]._u32[0] = 0x3F000;                    /* a stack near LS top */
    spu_begin_image(UC3_SPU_IMAGE_EDGE);
    spu_recomp_register();
    /* Register the cellSpursJob2 runtime-helper HLE stubs and wire the context
     * vtable so the trampoline's callbacks resolve instead of branching to 0. */
    spu_register_function(UC3_JOB2_HLP_52, uc3_job2_helper);
    spu_register_function(UC3_JOB2_HLP_60, uc3_job2_helper);
    spu_register_function(UC3_JOB2_HLP_68, uc3_job2_helper);
    spu_ls_write32(ctx, 0x5110,            UC3_JOB2_CTX);   /* context pointer */
    spu_ls_write32(ctx, UC3_JOB2_CTX + 52, UC3_JOB2_HLP_52);
    spu_ls_write32(ctx, UC3_JOB2_CTX + 60, UC3_JOB2_HLP_60);
    spu_ls_write32(ctx, UC3_JOB2_CTX + 68, UC3_JOB2_HLP_68);
    ctx->image_id = UC3_SPU_IMAGE_EDGE;
    /* Entry point: default 0x4000 (cellSpursJob2 trampoline). UC3_EDGE_ENTRY lets
     * us bypass the trampoline and enter the job logic directly (experimental
     * path (b): run edge0's own functions, e.g. 0x4108 job logic / 0x4990 geom). */
    uint32_t entry = load_addr;
    if (const char* e = getenv("UC3_EDGE_ENTRY")) entry = (uint32_t)strtoul(e, nullptr, 0);
    ctx->pc = entry;
    ctx->status = SPU_STATUS_RUNNING;
    fprintf(stderr, "[edge-job] >>> running Edge job @0x%08X "
                    "(payload +0x%X, load LS 0x%X, %u bytes) entry=0x%05X\n",
            ea_binary, payload_offset, load_addr, code_size, entry);
    spu_abort_arm(1);
    if (setjmp(g_spu_abort_buf) == 0) {
        spu_indirect_branch(ctx);
        fprintf(stderr, "[edge-job] <<< returned status=0x%X pc=0x%05X\n",
                ctx->status, ctx->pc & SPU_LS_MASK);
    } else {
        fprintf(stderr, "[edge-job] <<< aborted by runaway guard (param block / inputs"
                        " not set up yet — observing DMA pattern).\n");
    }
    spu_abort_arm(0);
}

static void uc3_log_policy_quad(const spu_context* ctx, uint32_t lsa) {
    u128 v = spu_ls_read128(ctx, lsa);
    fprintf(stderr, "[policy-job] LS[%04X] = %08X %08X %08X %08X\n",
            lsa, v._u32[0], v._u32[1], v._u32[2], v._u32[3]);
}

static void uc3_dump_live_job_binaries(uint32_t warg) {
    if (getenv("UC3_DUMP_POLICY_JOB") == nullptr || !warg)
        return;
    uint32_t sub = vm_read32(warg + 0x30);
    for (uint32_t entry = 0; entry < 0x20; entry += 8) {
        uint32_t descriptor_word = vm_read32(sub + entry);
        uint32_t descriptor = vm_read32(sub + entry + 4);
        uint32_t descriptor_size = descriptor_word >> 16;
        if (!descriptor || descriptor_size < 0x20 || descriptor_size > 0x100)
            continue;
        for (uint32_t offset = 0; offset + 4 <= descriptor_size; offset += 4) {
            uint32_t binary_ea = vm_read32(descriptor + offset);
            if (binary_ea < 0x10000 || binary_ea >= 0x40000000 ||
                vm_read32(binary_ea + 0x20) != 0xC0DEC0DEu)
                continue;
            uint32_t binary_size = vm_read32(binary_ea + 0x14);
            if (binary_size < 0x30 || binary_size > 0x10000)
                continue;
            char path[96];
            snprintf(path, sizeof(path),
                     "spu_programs/edge_job_%08X_live.bin", binary_ea);
            if (FILE* fp = fopen(path, "wb")) {
                size_t written = fwrite(vm_base + binary_ea, 1, binary_size, fp);
                fclose(fp);
                fprintf(stderr, "[policy-job] dumped live binary ea=0x%08X "
                                "size=0x%X to %s (%zu bytes)\n",
                        binary_ea, binary_size, path, written);
            }
            /* This UC3 Jobbin2 descriptor stores its public header 0x10000 bytes
             * after the image base used by the policy DMA. Capture that exact
             * source range as well so lifting matches the LS image byte-for-byte. */
            if (binary_ea == 0x00FFEC80u) {
                uint32_t image_ea = binary_ea - 0x10000u;
                snprintf(path, sizeof(path),
                         "spu_programs/edge_image_%08X_live.bin", image_ea);
                if (FILE* fp = fopen(path, "wb")) {
                    size_t written = fwrite(vm_base + image_ea, 1, binary_size, fp);
                    fclose(fp);
                    fprintf(stderr, "[policy-job] dumped DMA image ea=0x%08X "
                                    "size=0x%X to %s (%zu bytes)\n",
                            image_ea, binary_size, path, written);
                }
            }
        }
    }
}

static void uc3_seed_spurs_kernel_quads(spu_context* ctx) {
    struct KernelQuad {
        uint32_t lsa;
        uint32_t words[4];
    };
    /* The policy reads these instruction quads from the resident SPURS kernel.
     * They are identical in the real BCES01175 LS dump and RPCS3's kernel. */
    static const KernelQuad quads[] = {
        {0x5D0, {0x3E82C0B7, 0x32880038, 0x16081C12, 0xB66D1637}},
        {0x700, {0x40880022, 0x32800009, 0x5C03C19C, 0x24004080}},
        {0x720, {0x4085801F, 0x36800D95, 0x7C000E19, 0x3FBF0E96}},
        {0x740, {0x43FFF018, 0x3FBF0D12, 0x82658B95, 0x21A00818}},
        {0x770, {0x40802007, 0x40800108, 0x1CF80081, 0x6804480F}},
        {0x780, {0x43FFD801, 0x3FE1078D, 0x0400078E, 0x21A0088E}},
        {0x840, {0x337F4A00, 0x337FD480, 0x43FFE801, 0x127FD389}},
    };
    for (const KernelQuad& quad : quads)
        for (uint32_t i = 0; i < 4; ++i)
            spu_ls_write32(ctx, quad.lsa + i * 4, quad.words[i]);
}

/* Execute the real policy entry on the captured workload. Edge is registered
 * in the same composed image so its indirect dispatch can enter the job. */
extern "C" void spu_trace_init(const char* path);
extern "C" void sscull_spu_recomp_register(void);
extern "C" void sscull_spu_func_00004000(spu_context* ctx);
extern "C" void sscull_job2_spu_recomp_register(void);
extern "C" void sscull_job2_spu_func_0000E6E8(spu_context* ctx);
extern "C" void wkl2_spu_recomp_register(void);   /* wkl[2] asset-geometry job (image 4) */
extern "C" void wkl2_spu_func_00004000(spu_context* ctx);
/* gen_sampler_mpeg (spu_0004) = décodeur MPEG-2 du film (logos sce-ndi-logos.m2v).
 * Lifté dans spu_gen/mpeg/ via la méthode auteur. Registre GLOBAL keyé par
 * adresse LS: les fn MPEG (0x4000+) CHEVAUCHENT les jobs Edge (0x4000+) -> ne
 * PAS enregistrer inconditionnellement (collision). Appelé seulement dans le
 * dispatch dédié au décode film. */
extern "C" void mpeg_spu_recomp_register(void);
extern "C" void mpeg_spu_func_00004080(spu_context* ctx);  /* MPEG job entry */

/* NOTE (trace finding): the policy has two paths out of 0x2C70 — yield to the
 * kernel trampoline (run 1, where the HLE completes the content DMA before
 * dispatching) or enter job-start 0x31E0 DIRECTLY (wkl[6]). The direct entry is
 * a lifter-emitted DIRECT C call, so a registry override at 0x31E0 does NOT
 * intercept it (tried, removed); recovery happens post-halt instead. */

/* Image SPU du job du workload en cours d'exécution (dispatch par range dans
 * spu_indirect_branch): 2=POLICY_EDGE/feec80 (défaut), 4=WKL2GEO pour wid 2. */
extern "C" { int g_spu_current_job_image = 2; }
extern "C" int spu_interp_run(spu_context* ctx, long max_insns);
using spu_interp_pc_hook = void (*)(spu_context*, uint32_t);
extern "C" void spu_interp_set_pc_hook(spu_context* ctx,
                                         spu_interp_pc_hook hook);

struct Uc3PolicyPcEdge {
    uint32_t from;
    uint32_t to;
};

static constexpr uint32_t UC3_POLICY_PC_HISTORY = 128u;
static constexpr uint32_t UC3_POLICY_EDGE_HISTORY = 256u;
static thread_local spu_context* s_policy_pc_trace_ctx = nullptr;
static thread_local uint32_t s_policy_pc_history[UC3_POLICY_PC_HISTORY]{};
static thread_local Uc3PolicyPcEdge
    s_policy_edge_history[UC3_POLICY_EDGE_HISTORY]{};
static thread_local Uc3PolicyPcEdge
    s_policy_edge_head[UC3_POLICY_EDGE_HISTORY]{};
static thread_local uint64_t s_policy_pc_total = 0;
static thread_local uint64_t s_policy_edge_total = 0;
static thread_local uint32_t s_policy_pc_previous = 0;

static void uc3_policy_interp_pc_hook(spu_context* ctx, uint32_t pc) {
    if (ctx != s_policy_pc_trace_ctx)
        return;

    pc &= SPU_LS_MASK;
    s_policy_pc_history[s_policy_pc_total % UC3_POLICY_PC_HISTORY] = pc;
    if (s_policy_pc_total != 0u &&
        pc != ((s_policy_pc_previous + 4u) & SPU_LS_MASK)) {
        const Uc3PolicyPcEdge edge{s_policy_pc_previous, pc};
        s_policy_edge_history[s_policy_edge_total % UC3_POLICY_EDGE_HISTORY] = edge;
        if (s_policy_edge_total < UC3_POLICY_EDGE_HISTORY)
            s_policy_edge_head[s_policy_edge_total] = edge;
        ++s_policy_edge_total;
    }
    s_policy_pc_previous = pc;
    ++s_policy_pc_total;
}

static void uc3_policy_pc_trace_begin(spu_context* ctx) {
    s_policy_pc_trace_ctx = ctx;
    s_policy_pc_total = 0;
    s_policy_edge_total = 0;
    s_policy_pc_previous = 0;
    spu_interp_set_pc_hook(ctx, uc3_policy_interp_pc_hook);
}

static void uc3_policy_pc_trace_end(uint32_t wid) {
    spu_interp_set_pc_hook(nullptr, nullptr);

    const uint64_t edge_count =
        s_policy_edge_total < UC3_POLICY_EDGE_HISTORY
            ? s_policy_edge_total
            : UC3_POLICY_EDGE_HISTORY;
    const uint64_t edge_first = s_policy_edge_total - edge_count;
    fprintf(stderr,
            "[policy-pc-trace] wid=%u instructions=%llu branches=%llu "
            "retained-branches=%llu\n",
            wid, (unsigned long long)s_policy_pc_total,
            (unsigned long long)s_policy_edge_total,
            (unsigned long long)edge_count);
    if (s_policy_edge_total > UC3_POLICY_EDGE_HISTORY) {
        for (uint64_t i = 0; i < UC3_POLICY_EDGE_HISTORY; ++i) {
            const Uc3PolicyPcEdge& edge = s_policy_edge_head[i];
            fprintf(stderr,
                    "[policy-pc-head-edge] seq=%llu from=0x%05X to=0x%05X\n",
                    (unsigned long long)i, edge.from, edge.to);
        }
    }
    for (uint64_t i = 0; i < edge_count; ++i) {
        const uint64_t sequence = edge_first + i;
        const Uc3PolicyPcEdge& edge =
            s_policy_edge_history[sequence % UC3_POLICY_EDGE_HISTORY];
        fprintf(stderr,
                "[policy-pc-edge] seq=%llu from=0x%05X to=0x%05X\n",
                (unsigned long long)sequence, edge.from, edge.to);
    }

    const uint64_t pc_count =
        s_policy_pc_total < UC3_POLICY_PC_HISTORY
            ? s_policy_pc_total
            : UC3_POLICY_PC_HISTORY;
    const uint64_t pc_first = s_policy_pc_total - pc_count;
    for (uint64_t i = 0; i < pc_count; i += 16u) {
        fprintf(stderr, "[policy-pc-tail] seq=%llu:",
                (unsigned long long)(pc_first + i));
        const uint64_t row_count =
            pc_count - i < 16u ? pc_count - i : 16u;
        for (uint64_t j = 0; j < row_count; ++j) {
            const uint64_t sequence = pc_first + i + j;
            fprintf(stderr, " %05X",
                    s_policy_pc_history[sequence % UC3_POLICY_PC_HISTORY]);
        }
        fputc('\n', stderr);
    }
    s_policy_pc_trace_ctx = nullptr;
}

static bool uc3_policy_pc_trace_requested(uint32_t wid) {
    if (getenv("UC3_POLICY_PC_TRACE") == nullptr ||
        getenv("UC3_POLICY_FULL_INTERP") == nullptr) {
        return false;
    }

    uint32_t target_wid = 1u;
    if (const char* value = getenv("UC3_POLICY_PC_TRACE_WID")) {
        char* end = nullptr;
        const unsigned long parsed = strtoul(value, &end, 0);
        if (end != value && *end == '\0' && parsed < 32u)
            target_wid = static_cast<uint32_t>(parsed);
    }
    if (wid != target_wid)
        return false;

    if (getenv("UC3_POLICY_PC_TRACE_ONCE") != nullptr) {
        static std::atomic<bool> claimed{false};
        bool expected = false;
        return claimed.compare_exchange_strong(expected, true);
    }
    return true;
}

static thread_local uint32_t s_sscull_job2_source_ea = 0;

static void uc3_sscull_job2_no_buffer(spu_context* ctx) {
    static int count = 0;
    if (count++ < 8) {
        fprintf(stderr,
                "[sscull-job2] unimplemented buffer callback pc=0x%05X "
                "index=%u\n",
                ctx->pc & SPU_LS_MASK, ctx->gpr[4]._u32[0]);
    }
    memset(&ctx->gpr[3], 0, sizeof(ctx->gpr[3]));
}

static void uc3_sscull_job2_init(spu_context* ctx) {
    static int count = 0;
    if (count++ < 8)
        fprintf(stderr, "[sscull-job2] init callback pc=0x%05X\n",
                ctx->pc & SPU_LS_MASK);
    memset(&ctx->gpr[3], 0, sizeof(ctx->gpr[3]));
}

/* HLE only the resident Job2 launcher, as recommended by SPU_FALLBACK.md; the
 * game's nested culling payload itself remains lifted SPU code. */
static void uc3_sscull_job2_service(spu_context* ctx) {
    const uint32_t image_ea = s_sscull_job2_source_ea;
    if (!image_ea || vm_read32(image_ea + 0x20u) != 0xC0DEC0DEu) {
        fprintf(stderr, "[sscull-job2] invalid nested image EA=0x%08X\n",
                image_ea);
        ctx->status = SPU_STATUS_STOPPED_BY_HALT;
        return;
    }

    const uint32_t entry_offset = vm_read32(image_ea + 0x10u);
    const uint32_t image_size = vm_read32(image_ea + 0x14u);
    const uint32_t load_addr = vm_read32(image_ea + 0x28u);
    if (load_addr != 0x4000u || image_size < 0x30u ||
        load_addr + image_size > SPU_LS_SIZE ||
        entry_offset >= image_size) {
        fprintf(stderr,
                "[sscull-job2] invalid header entry=0x%X size=0x%X load=0x%X\n",
                entry_offset, image_size, load_addr);
        ctx->status = SPU_STATUS_STOPPED_BY_HALT;
        return;
    }

    const uint32_t params_ea = vm_read32(s_feec80_job_desc + 0x24u);
    const uint32_t params_size = vm_read32(s_feec80_job_desc + 0x28u);
    if (params_ea < 0x10000u || params_ea >= 0x30000000u ||
        params_size == 0 || params_size > 0x2000u ||
        UC3_F80_PARAMS + params_size > SPU_LS_SIZE) {
        fprintf(stderr,
                "[sscull-job2] invalid params EA=0x%08X size=0x%X desc=0x%08X\n",
                params_ea, params_size, s_feec80_job_desc);
        ctx->status = SPU_STATUS_STOPPED_BY_HALT;
        return;
    }

    memcpy(ctx->ls + load_addr, vm_base + image_ea, image_size);
    memset(ctx->ls + UC3_F80_CTX, 0, 0x80u);
    memcpy(ctx->ls + UC3_F80_PARAMS, vm_base + params_ea, params_size);

    constexpr uint32_t helper_get_input = 0x0300u;
    constexpr uint32_t helper_get_output = 0x0308u;
    constexpr uint32_t helper_init = 0x0310u;
    static std::once_flag registration;
    std::call_once(registration, [] {
        spu_begin_image(UC3_SPU_IMAGE_SSCULL_JOB2);
        spu_register_function(helper_get_input, uc3_sscull_job2_no_buffer);
        spu_register_function(helper_get_output, uc3_sscull_job2_no_buffer);
        spu_register_function(helper_init, uc3_sscull_job2_init);
        sscull_job2_spu_recomp_register();
    });
    spu_ls_write32(ctx, UC3_F80_CTX + 0x20u, UC3_F80_PARAMS);
    spu_ls_write32(ctx, UC3_F80_CTX + 0x34u, helper_get_input);
    spu_ls_write32(ctx, UC3_F80_CTX + 0x3Cu, helper_get_output);
    spu_ls_write32(ctx, UC3_F80_CTX + 0x44u, helper_init);

    const int previous_image = ctx->image_id;
    ctx->image_id = UC3_SPU_IMAGE_SSCULL_JOB2;
    memset(&ctx->gpr[3], 0, sizeof(ctx->gpr[3]));
    memset(&ctx->gpr[4], 0, sizeof(ctx->gpr[4]));
    ctx->gpr[3]._u32[0] = UC3_F80_CTX;
    ctx->pc = load_addr + entry_offset;
    ctx->status = SPU_STATUS_RUNNING;
    fprintf(stderr,
            "[sscull-job2] launch EA=0x%08X entry=0x%05X size=0x%X "
            "params=0x%08X/0x%X\n",
            image_ea, ctx->pc & SPU_LS_MASK, image_size,
            params_ea, params_size);
    sscull_job2_spu_func_0000E6E8(ctx);
    fprintf(stderr,
            "[sscull-job2] returned pc=0x%05X status=0x%X r3=0x%08X\n",
            ctx->pc & SPU_LS_MASK, ctx->status, ctx->gpr[3]._u32[0]);
    ctx->image_id = previous_image;
}

/* D51D60 does not wait for the raw producer index. Its completion target is
 * the producer plus the number of chained records following the active ready
 * entry. Keep this scheduler-side bookkeeping in the HLE executor: generated
 * PPU code remains a read-only consumer of the state produced here. */
static bool uc3_workload_completion_target(uint32_t ring, uint16_t* out_target) {
    if (!out_target || ring < 0x10000u || ring >= 0x40000000u)
        return false;

    const uint16_t producer = vm_read16(ring + 0x40u);
    const uint32_t ready = vm_read32(ring + 0x30u);
    if (ready < 0x10000u || ready >= 0x40000000u)
        return false;

    const uint32_t slot = ready + (uint32_t)producer * 8u;
    uint16_t target = producer;
    if (vm_read16(slot + 2u) != 0u) {
        uint32_t entry = slot + 0xAu;
        bool terminated = false;
        for (uint32_t i = 0; i < 0x100u; ++i, entry += 8u) {
            if (vm_read16(entry) == 0u) {
                terminated = true;
                break;
            }
            ++target;
        }
        if (!terminated)
            return false;
    }

    *out_target = target;
    return true;
}

static bool uc3_advance_workload_completion(uint32_t ring, uint32_t wid,
                                            uint16_t steps,
                                            const char* source) {
    uint16_t target = 0;
    if (!steps || !uc3_workload_completion_target(ring, &target))
        return false;

    bool changed = false;
    uint16_t before = 0xFFFFu;
    uint16_t after = 0xFFFFu;
    for (uint32_t off = 0x42u; off <= 0x4Eu; off += 2u) {
        const uint16_t cursor = vm_read16(ring + off);
        if (cursor == 0xFFFFu || cursor >= target)
            continue;
        const uint32_t advanced = (uint32_t)cursor + steps;
        const uint16_t next = advanced < target ? (uint16_t)advanced : target;
        vm_write16(ring + off, next);
        if (!changed) {
            before = cursor;
            after = next;
        }
        changed = true;
    }

    if (changed) {
        fprintf(stderr,
                "[wkl-progress] wid=%u ring=0x%08X cursor=%u->%u "
                "target=%u source=%s\n",
                wid, ring, before, after, target, source ? source : "unknown");
    }
    return changed;
}

/* Non-static: l'exécuteur déterministe uc3_spu_exec.cpp (étage 2, callback
 * push UC3_RING_HOOK) appelle cette machinerie directement. */
void uc3_run_policy_job(uint32_t policy_ea, uint32_t policy_size,
                        uint32_t spurs, uint32_t warg,
                        uint32_t job_desc, uint32_t ea_binary,
                        uint32_t wid = 0, uint32_t spu_num = 0,
                        uint32_t dispatch_poll_status = 0xFFFFFFFFu) {
    s_policy_module_exit_ran = false;
    s_policy_last_run_completed = false;
    s_wkl2_sync_return_ran = false;
    s_feec80_sync_return_ran = false;
    g_spu_current_job_image = (wid == 2) ? UC3_SPU_IMAGE_WKL2GEO
                                         : UC3_SPU_IMAGE_POLICY_EDGE;
    /* Per-run function-entry trace (requires a --trace-functions policy lift;
     * no-op otherwise). One file per run so run paths can be diffed to find
     * where e.g. run 1 yields at the scheduler boundary and run 3 does not. */
    if (getenv("UC3_POLICY_TRACE") != nullptr) {
        static int s_trace_run = 0;
        char tp[64];
        snprintf(tp, sizeof(tp), "policy_trace_%d.txt", ++s_trace_run);
        spu_trace_init(tp);
    }
    /* Per-run descriptor I/O: the ENTRY4000 buffer wiring reads +0x48/+0x4C and
     * +0x24 from s_feec80_job_desc. wkl[0] keeps the historically proven
     * 0x20E42E00; every other workload's run uses ITS OWN descriptor so outputs
     * land in the job's real buffers (jobs without a +0x48 output simply skip
     * PUTBACK instead of writing wkl[0]'s buffer 19 times). */
    s_feec80_job_desc = (wid == 0 || !job_desc) ? 0x20E42E00u : job_desc;
    if (getenv("UC3_SSCULL_JOB2_PROBE") != nullptr) {
        static std::atomic<bool> s_sscull_job2_guest_probed{false};
        if (!s_sscull_job2_guest_probed.exchange(true)) {
            /* The wkl[6] policy DMA source observed in the current build is
             * 0x00ECBB80; its nested Jobbin2 starts at source + 0x900. */
            constexpr uint32_t nested_ea = 0x00ECC480u;
            fprintf(stderr,
                    "[sscull-job2-guest] EA=%08X first=%08X %08X %08X %08X "
                    "hdr=%08X entry=%08X size=%08X load=%08X\n",
                    nested_ea,
                    vm_read32(nested_ea + 0x00u),
                    vm_read32(nested_ea + 0x04u),
                    vm_read32(nested_ea + 0x08u),
                    vm_read32(nested_ea + 0x0Cu),
                    vm_read32(nested_ea + 0x20u),
                    vm_read32(nested_ea + 0x10u),
                    vm_read32(nested_ea + 0x14u),
                    vm_read32(nested_ea + 0x28u));
            if (FILE* image = fopen(
                    "analysis/codex_sscull_dump/sscull_jobbin2_live.bin", "wb")) {
                fwrite(vm_base + nested_ea, 1, 0xC0F0u, image);
                fclose(image);
            }
            if (FILE* params = fopen(
                    "analysis/codex_sscull_dump/sscull_params_live.bin", "wb")) {
                fwrite(vm_base + 0x20E61400u, 1, 0x440u, params);
                fclose(params);
            }
            fprintf(stderr,
                    "[sscull-job2-guest] captured live image (0xC0F0) and "
                    "params EA=20E61400 (0x440)\n");
        }
    }
    uc3_dump_live_job_binaries(warg);
    /* Sous UC3_JOB_GUARD, chaque dispatch recoit un contexte NEUF (heap) :
     * un job abandonne apres timeout garde le sien, le dispatch suivant ne
     * le partage pas (le static est une course sinon). unique_ptr : libere
     * au retour normal — meme tardif —, fuite seulement si le job ne
     * revient jamais (bornee par le marquage s_seen_chains). */
    static spu_context s_policy_ctx;
    std::unique_ptr<spu_context> owned_ctx;
    spu_context* ctx;
    if (getenv("UC3_JOB_GUARD") != nullptr) {
        owned_ctx.reset(new spu_context());
        ctx = owned_ctx.get();
    } else {
        ctx = &s_policy_ctx;
    }
    g_uc3_policy_ctx_live.store(ctx);
    spu_context_init(ctx, spu_num);
    spu_dma_set_deferred(ctx, 1);

    if (!policy_ea || policy_size == 0 || policy_size > 0xF000 ||
        0xA00u + policy_size > SPU_LS_SIZE) {
        fprintf(stderr, "[policy-job] invalid policy image ea=0x%08X size=0x%X\n",
                policy_ea, policy_size);
        return;
    }
    memcpy(ctx->ls + 0xA00, vm_base + policy_ea, policy_size);
    uc3_seed_spurs_kernel_quads(ctx);

    auto lsw32 = [&](uint32_t a, uint32_t v) { spu_ls_write32(ctx, a, v); };
    /* The policy image carries its real stack seed at LS 0x1580. */
    lsw32(0x1C0, spurs);          /* SpursKernelContext.spurs (32-bit EA / hi of 64-bit) */
    lsw32(0x1C4, spurs);          /* SpursKernelContext.spurs, low EA (64-bit lo) */
    lsw32(0x1C8, spu_num);        /* spuNum */
    lsw32(0x1CC, 31);             /* CELL_SPURS_KERNEL_DMA_TAG_ID */
    lsw32(0x1D4, policy_ea);      /* current workload policy EA, low word */
    lsw32(0x1DC, wid & 31u);      /* workload id (drives the LS slot lookup) */
    lsw32(0x1E0, 0x808);          /* kernel exit address */
    lsw32(0x1E4, 0x290);          /* select-workload address */
    /* The persistent policy context uses -1 for "no active scheduler slot".
     * A fresh standalone LS is zeroed, which otherwise makes the policy run
     * slot 0 before its parser has selected one. */
    for (uint32_t offset = 0; offset < 16; offset += 4)
        lsw32(UC3_POLICY_SLOTS_LSA + offset, 0xFFFFFFFFu);
    if (spurs) {
        /* A dispatched SPURS workload receives the kernel's cached first
         * 0x80 bytes before entering its policy module. */
        for (uint32_t i = 0; i < 0x80; ++i)
            ctx->ls[0x100u + i] = vm_read8(spurs + i);
        /* Copy THIS workload's wklInfo entry (0x20 bytes each), not wkl[0]'s. */
        for (uint32_t i = 0; i < 0x20; ++i)
            ctx->ls[0x3FFE0 + i] =
                vm_read8(spurs + 0xB00 + (wid & 31u) * 0x20 + i);

        if (getenv("UC3_POLICY_ELIGIBILITY_TRACE") != nullptr) {
            const uint32_t slot = wid & 15u;
            const uint32_t info = spurs + (wid < 16u ? 0xB00u : 0x1000u) +
                                  slot * 0x20u;
            const uint32_t state_offset = wid < 16u ? 0x80u : 0xD0u;
            const uint32_t status_offset = wid < 16u ? 0x90u : 0xE0u;
            const uint32_t nspus = vm_read8(spurs + 0x76u);
            const uint8_t packed_max = vm_read8(spurs + 0x50u + slot);
            const uint8_t max_contention =
                wid < 16u ? (packed_max & 0x0Fu) : (packed_max >> 4);
            uint8_t priorities[8]{};
            int first_eligible_spu = -1;
            for (uint32_t i = 0; i < 8u; ++i) {
                priorities[i] = vm_read8(info + 0x18u + i);
                if (first_eligible_spu < 0 && i < nspus && priorities[i] != 0)
                    first_eligible_spu = (int)i;
            }
            fprintf(stderr,
                    "[policy-eligibility] wid=%u spurs=0x%08X nspus=%u "
                    "selected-spu=%u selected-prio=%u first-eligible=%d "
                    "priorities=%u/%u/%u/%u/%u/%u/%u/%u "
                    "ready=%u contention=%u/%u min=%u max=%u "
                    "state=%u status=0x%02X local-prio=0x%02X "
                    "runnable=0x%08X unique=%u\n",
                    wid, spurs, nspus, spu_num,
                    priorities[spu_num < 8u ? spu_num : 0u], first_eligible_spu,
                    priorities[0], priorities[1], priorities[2], priorities[3],
                    priorities[4], priorities[5], priorities[6], priorities[7],
                    vm_read8(spurs + wid), vm_read8(spurs + 0x20u + slot),
                    vm_read8(spurs + 0x30u + slot),
                    vm_read8(spurs + 0x40u + slot),
                    max_contention,
                    vm_read8(spurs + state_offset + slot),
                    vm_read8(spurs + status_offset + slot),
                    ctx->ls[0x1A0u + slot], spu_ls_read32(ctx, 0x1ECu),
                    vm_read8(info + 0x14u));
        }
    }
    /* Note: do NOT pre-register the LS workload slot (0x1460 + wid*0x20) here —
     * the policy's own type-1 command processing registers it during the run,
     * and a pre-filled slot derails that state machine (run 1 then parks at
     * state 2 instead of dispatching). LS[0x1DC]=wid is what routes the slot
     * lookup to the right workload. */

    ctx->gpr[0]._u32[0] = 0x808;
    ctx->gpr[1]._u32[0] = 0x3FFB0;
    ctx->gpr[3]._u32[0] = 0x100;
    /* Workload arg is a 64-bit EA. In the SPU's logical big-endian word order
     * the high and low halves occupy words 0 and 1 respectively. */
    ctx->gpr[4]._u32[0] = 0;
    ctx->gpr[4]._u32[1] = warg;
    uint32_t poll_status = dispatch_poll_status;
    if (poll_status == 0xFFFFFFFFu)
        poll_status = 0;
    if (dispatch_poll_status == 0xFFFFFFFFu && spurs &&
        vm_read8(spurs + (wid & 31u)) != 0) {
        /* The SPURS scheduler selected this workload because readyCount was
         * non-zero. Dispatch conveys that reason to the policy in r5. */
        poll_status = 1u;
    }
    ctx->gpr[5]._u32[0] = poll_status;
    if (getenv("UC3_POLICY_ELIGIBILITY_TRACE") != nullptr)
        fprintf(stderr,
                "[policy-dispatch] wid=%u spu=%u poll-status=0x%X\n",
                wid, spu_num, poll_status);

    spu_begin_image(UC3_SPU_IMAGE_POLICY_EDGE);
    /* REGISTRE = premier-arrive-gagne (spu_lookup scanne lineairement et
     * retourne la PREMIERE entree). Le re-lift 2026-07-08 a ajoute au module
     * policy des fonctions kernel-resident REELLES (0x0A78...) que le job
     * feec80 appelle comme services du Job Manager ; le vrai code SPURS y
     * attend un etat kernel que ce runtime n'emule pas -> spin infini
     * (watchdog fw2/fx2.log : pc=0x00A78 fige, r0=0x4090, executeur mort,
     * 1 chaine max, 0 segment). Le run conforme du 2026-07-06 passait par
     * uc3_job2_helper (CELL_OK) car la table policy d'alors n'avait pas ces
     * adresses. Les reinstaller AVANT les tables pour qu'elles gagnent. */
    {
        /* Pre-register the FULL Job-Manager/kernel-helper set (all 16 targets)
         * BEFORE policy_spu_recomp_register + feec80_spu_recomp_register. The
         * feec80 module's re-lift registers REAL lifted functions at the eleven
         * 0x7090..0x96F8 addresses (kernel-resident SPURS services that spin on
         * un-emulated kernel state); with first-come-wins lookup, those must be
         * claimed by uc3_job2_helper (CELL_OK) FIRST or the job hangs at pc≈0x41E0
         * instead of completing. This mirrors exactly the standalone feec80
         * harness (register_job2_helpers before feec80_spu_recomp_register), which
         * runs the job to its 0x41E0 completion halt with real geometry output. */
        static constexpr uint32_t kJob2KernelTargets[] = {
            0x0A78u, 0x1FF8u, 0x2E58u, 0x38F0u, 0x3E10u,
            0x7090u, 0x7100u, 0x7B58u, 0x7BB8u, 0x7E80u,
            0x8080u, 0x8338u, 0x83C8u, 0x8FA8u, 0x9250u, 0x96F8u };
        static bool s_kernel_helpers_done = false;
        if (!s_kernel_helpers_done) {
            s_kernel_helpers_done = true;
            for (uint32_t t : kJob2KernelTargets)
                spu_register_function(t, uc3_job2_helper);
        }
    }
    policy_spu_recomp_register();
    if (getenv("UC3_POLICY_USE_EDGE0") != nullptr)
        spu_recomp_register();
    else {
        /* Registered first so the scheduler can switch the overlapping tail
         * from policy data back to FE code immediately before job entry. */
        spu_register_function(0x4000u, uc3_feec80_dispatch);
        feec80_spu_recomp_register();
        uc3_register_job2_runtime_helpers();
    }
    /* Pré-enregistrer les overlays de jobs Edge (sscull image 3) pour que le
     * fallback overlay (spu_indirect_branch) résolve les branchements du policy
     * vers ces overlays (ex. 0xCB00) au lieu de crasher. */
    if (getenv("UC3_SPU_OVERLAY_FALLBACK")) {
        static bool s_ovl_pre = false;
        if (!s_ovl_pre) { s_ovl_pre = true;
            /* Enregistrer wkl2 (image 4) pour que le fallback résolve les
             * branchements du job wid 2 vers son code (0xCB00 etc.). sscull garde
             * son handler dédié (1772) pour éviter l'ambiguïté d'adresse. */
            spu_begin_image(UC3_SPU_IMAGE_WKL2GEO);
            wkl2_spu_recomp_register();
            spu_begin_image(UC3_SPU_IMAGE_POLICY_EDGE);
        }
    }
    ctx->image_id = UC3_SPU_IMAGE_POLICY_EDGE;
    ctx->pc = 0xA00;
    ctx->status = SPU_STATUS_RUNNING;
    g_spu_dma_log = 256;

    fprintf(stderr, "[policy-job] >>> policy=0x%08X/0x%X warg=0x%08X "
                    "job=0x%08X edge=0x%08X spu=%u\n",
            policy_ea, policy_size, warg, job_desc, ea_binary, spu_num);
    /* One-shot: dump the real content-job code the policy DMAs to LS 0x4000
     * (guest 0x00FEEC80, 6928 bytes) so it can be lifted as the actual job. */
    if (getenv("UC3_DUMP_CONTENT") != nullptr) {
        if (FILE* fp = fopen("spu_programs/content_job_FEEC80.bin", "wb")) {
            fwrite(vm_base + 0x00FEEC80u, 1, 6928, fp);
            fclose(fp);
            fprintf(stderr, "[policy-job] dumped content job 0x00FEEC80 (6928 bytes)\n");
        }
    }
    /* Dump the chosen job descriptor (0x20E42E00 per the jobchain command list)
     * and the referenced buffers, to map the cellSpursJob2 I/O DMA lists that
     * should populate r83/r85/r89 (the job's input/output buffer pointers). */
    /* One-shot: dump this job's binary (descriptor +0x20 EA, +0x28 size) so new
     * Edge job binaries (beyond edge0/feec80) can be identified and lifted. */
    if (getenv("UC3_DUMP_JOBBIN") != nullptr && job_desc) {
        uint32_t bin_ea = vm_read32(job_desc + 0x20);
        uint32_t bin_sz = vm_read32(job_desc + 0x28);
        if (bin_ea >= 0x10000 && bin_ea < 0x01400000 &&
            bin_sz >= 0x80 && bin_sz <= 0x40000) {
            char name[64];
            snprintf(name, sizeof(name), "spu_programs/edge_job_%08X.bin", bin_ea);
            static std::set<uint32_t> s_dumped;
            static std::mutex s_dump_mutex;
            std::lock_guard<std::mutex> lk(s_dump_mutex);
            if (s_dumped.insert(bin_ea).second) {
                if (FILE* f = fopen(name, "wb")) {
                    for (uint32_t i = 0; i < bin_sz; ++i) {
                        uint8_t b = vm_read8(bin_ea + i);
                        fwrite(&b, 1, 1, f);
                    }
                    fclose(f);
                    fprintf(stderr, "[policy-job] dumped job binary 0x%08X "
                            "(%u bytes) -> %s\n", bin_ea, bin_sz, name);
                }
            }
        }
    }
    for (uint32_t jd : {0x20E42E00u, 0x20E1E380u, job_desc}) {
        if (!jd) continue;
        fprintf(stderr, "[policy-job] job desc 0x%08X:\n", jd);
        for (uint32_t o = 0; o < 0x60; o += 16) {
            fprintf(stderr, "    +0x%02X:", o);
            for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", vm_read8(jd + o + i));
            fprintf(stderr, "\n");
        }
        /* Recurse one level: dump the edge-geom control struct + its geometry target. */
        if (jd == 0x20E42E00u) {
            /* Edge Geometry input graph (host-HLE path, docs Phase 11 vertex proc):
             * control -> geomset "Basic" + streams -> segment table 0x313B2700
             * (0x40-byte descriptors) -> compressed vertex/index data. Dump each
             * level to RE the Edge format for the C decoder. */
            for (uint32_t s : {0x013A75F8u, 0x012FC730u, 0x20E34190u,
                               0x313ED700u, 0x313F71E4u, 0x313F1204u,
                               0x313B2700u, 0x313B2740u, 0x313B2780u}) {
                fprintf(stderr, "    struct 0x%08X:\n", s);
                for (uint32_t o = 0; o < 0x40u; o += 16) {
                    fprintf(stderr, "      +0x%02X:", o);
                    for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", vm_read8(s + o + i));
                    fprintf(stderr, "\n");
                }
            }
        }
        /* Decode DMA-list candidates: any u32 field that looks like a guest EA,
         * with the adjacent u32 as a size. Dump the target so we can confirm the
         * (ea,size) packing and see whether the target holds geometry data. */
        for (uint32_t o = 0x10; o < 0x60; o += 4) {
            uint32_t a = vm_read32(jd + o);
            if (a < 0x100000 || a >= 0x30000000) continue;   /* plausible EA */
            uint32_t szNext = vm_read32(jd + o + 4);
            uint32_t szPrev = vm_read32(jd + o - 4);
            fprintf(stderr, "    EA@+0x%02X=0x%08X (szNext=0x%X szPrev=0x%X) data:",
                    o, a, szNext, szPrev);
            for (int i = 0; i < 24; i++) fprintf(stderr, " %02X", vm_read8(a + i));
            fprintf(stderr, "\n");
        }
    }
    /* Dump the jobchain header (warg): the command loop at 0x24B8 reads commands
     * from EA=0, so one of these header fields (the command-list "pc") is 0. */
    if (warg) {
        for (uint32_t o = 0; o < 0xC0; o += 16) {
            fprintf(stderr, "[policy-job] jobchain+0x%02X:", o);
            for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", vm_read8(warg + o + i));
            fprintf(stderr, "\n");
        }
    }
    const bool trace_policy_pc = uc3_policy_pc_trace_requested(wid);
    if (trace_policy_pc)
        uc3_policy_pc_trace_begin(ctx);

    spu_abort_arm(1);
    const int policy_jump = setjmp(g_spu_abort_buf);
    if (policy_jump == 0) {
        uint32_t native_job_entry = 0;
        /* The lifted wid0 policy still computes r5 as a guest EA
         * (0x2894F0A0) and branches to empty LS 0xF0A0. The interpreter was
         * already proven to avoid that bad target. Keep the historical global
         * switch, and add a wid0-only switch so the validated lifted WKL2 and
         * screen-space-culling paths are not bypassed. */
        const bool use_full_interp =
            getenv("UC3_POLICY_FULL_INTERP") != nullptr ||
            (wid == 0u && getenv("UC3_POLICY_FULL_INTERP_WID0") != nullptr);
        if (use_full_interp) {
            ctx->pc = 0xA00; ctx->status = SPU_STATUS_RUNNING;
            int _ir = spu_interp_run(ctx, 8000000L);
            static int _il = 0;
            if (_il < 24) { _il++;
                const uint32_t _pc = ctx->pc & SPU_LS_MASK;
                fprintf(stderr, "[policy-full-interp] entree 0xA00 -> %s "
                                "(pc=0x%05X status=%u prev=0x%08X "
                                "insn=0x%08X r0=0x%08X r3=0x%08X "
                                "r4=0x%08X r5=0x%08X r78=0x%08X)\n",
                        _ir == 0 ? "stop propre" : "opcode NON GERE", ctx->pc & SPU_LS_MASK,
                        ctx->status, spu_ls_read32(ctx, (_pc - 4u) & SPU_LS_MASK),
                        spu_ls_read32(ctx, _pc), ctx->gpr[0]._u32[0],
                        ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0],
                        ctx->gpr[5]._u32[0], ctx->gpr[78]._u32[0]); }
            /* The interpreter cannot execute the resident SPURS kernel. On
             * wid0 it exhausts its bounded slice in the kernel wait at 0x2C38,
             * or at the following 0x2C3C load, with command state 1. Convert
             * those observed PCs to the same 0x2D08/r0 handoff produced by the
             * lifted path below; the existing scheduler HLE then grants the
             * slot and returns to the game's policy/content code. */
            if (_ir == 0 && ctx->status == SPU_STATUS_RUNNING &&
                ((ctx->pc & SPU_LS_MASK) == 0x2C38u ||
                 (ctx->pc & SPU_LS_MASK) == 0x2C3Cu) &&
                (spu_ls_read32(ctx, UC3_POLICY_COMMAND_LSA) & 0xFFFFu) == 1u) {
                fprintf(stderr, "[policy-full-interp] kernel boundary "
                                "0x%05X -> scheduler HLE handoff\n",
                        ctx->pc & SPU_LS_MASK);
                ctx->gpr[0]._u32[0] = 0x2C38u;
                ctx->pc = 0x2D08u;
            }
        } else
        policy_spu_func_00000A00(ctx);
        /* The lifted SPURS coordination trampoline at 0x2D08 is just `bi r0`.
         * Type 1 registered the job; the HLE scheduler advances it to type 2.
         * The real policy then parses LS 0xC00, builds its context, and owns the
         * type 2 -> 3 transition before dispatching the content job. */
        if (ctx->status == SPU_STATUS_RUNNING &&
            (ctx->pc & SPU_LS_MASK) == 0x2D08 &&
            ctx->gpr[0]._u32[0] == 0x2C38 &&
            (spu_ls_read32(ctx, UC3_POLICY_COMMAND_LSA) & 0xFFFFu) == 1) {
            uint32_t command = spu_ls_read32(ctx, UC3_POLICY_COMMAND_LSA);
            spu_ls_write32(ctx, UC3_POLICY_COMMAND_LSA,
                           (command & 0xFFFF0000u) | 2u);
            uint32_t workload_id = spu_ls_read32(ctx, 0x1DC) & 31u;
            uint32_t slot = 0x1460u + workload_id * 0x20u;
            if (spu_ls_read32(ctx, slot) == warg &&
                spu_ls_read32(ctx, slot + 8) == 1u) {
                uint32_t slot_flags = spu_ls_read32(ctx, slot + 4);
                spu_ls_write32(ctx, slot + 4, slot_flags | 0x00030000u);
                spu_ls_write32(ctx, slot + 8, 2u);
                fprintf(stderr, "[policy-job] HLE scheduler granted slot %u: "
                                "flags=0x%08X state 1 -> 2\n",
                        workload_id, slot_flags | 0x00030000u);
            }
            fprintf(stderr, "[policy-job] HLE scheduler job state 1 -> 2, resume 0x02C38\n");
            ctx->pc = 0x2C38;
            spu_indirect_branch(ctx);
        }
        /* A job can also arrive already in state 2 (granted; e.g. a later run
         * on a queue whose grant happened earlier). The native parser owns the
         * 2 -> 3 transition — just resume it at the trampoline continuation. */
        if (ctx->status == SPU_STATUS_RUNNING &&
            (ctx->pc & SPU_LS_MASK) == 0x2D08 &&
            ctx->gpr[0]._u32[0] == 0x2C38 &&
            (spu_ls_read32(ctx, UC3_POLICY_COMMAND_LSA) & 0xFFFFu) == 2) {
            fprintf(stderr, "[policy-job] HLE scheduler: job already state 2, "
                            "resume 0x02C38 for native 2 -> 3 parse\n");
            ctx->pc = 0x2C38;
            spu_indirect_branch(ctx);
        }
        /* A successful native parse changes command 2 -> 3, builds the job
         * context, and returns to the same kernel trampoline. Re-enter the
         * policy once as the HLE scheduler would; do not synthesize state 3. */
        if (ctx->status == SPU_STATUS_RUNNING &&
            (ctx->pc & SPU_LS_MASK) == 0x2D08 &&
            ctx->gpr[0]._u32[0] == 0x2C38 &&
            (spu_ls_read32(ctx, UC3_POLICY_COMMAND_LSA) & 0xFFFFu) == 3) {
            native_job_entry =
                spu_ls_read32(ctx, UC3_POLICY_CONTENT_LSA) & SPU_LS_MASK;
            {
                uint32_t full = spu_ls_read32(ctx, UC3_POLICY_CONTENT_LSA);
                uint32_t base = full & SPU_LS_MASK;
                fprintf(stderr, "[policy-job] LS[content]=0x%08X (base 0x%05X); "
                        "[base+0x10]=0x%08X -> preamble r5 would be 0x%08X\n",
                        full, base, spu_ls_read32(ctx, base + 0x10),
                        base + spu_ls_read32(ctx, base + 0x10));
                for (uint32_t o = 0; o < 0x20; o += 16) {
                    fprintf(stderr, "[policy-job]   LS[0x%05X+0x%02X]:", base, o);
                    for (int i = 0; i < 16; i++)
                        fprintf(stderr, " %02X", ctx->ls[(base + o + i) & SPU_LS_MASK]);
                    fprintf(stderr, "\n");
                }
            }
            fprintf(stderr, "[policy-job] native parser reached state 3; "
                            "HLE scheduler re-entering 0x02C38\n");
            ctx->pc = 0x2C38;
            spu_indirect_branch(ctx);
        }
        /* Diagnostic: when the policy parks at the kernel trampoline but neither
         * HLE branch fired (command not 1 or 3), log the gating values so the
         * queue-view question ("why does this ring show no ready item?") is
         * answered with data instead of guesses. */
        if (ctx->status == SPU_STATUS_RUNNING &&
            (ctx->pc & SPU_LS_MASK) == 0x2D08 && native_job_entry == 0) {
            fprintf(stderr, "[policy-job] parked idle: r0=0x%05X LS[4A80]=0x%08X "
                            "LS[4C80]=0x%08X LS[1DC]=0x%08X\n",
                    ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                    spu_ls_read32(ctx, UC3_POLICY_COMMAND_LSA),
                    spu_ls_read32(ctx, UC3_POLICY_CONTENT_LSA),
                    spu_ls_read32(ctx, 0x1DC));
            /* The 0x2480 dispatcher reads the queue entry's first halfword as
             * the command type (0=empty, 1/3=work). Dump the DMA'd queue area
             * so an "empty" parse can be compared against a working one. */
            for (uint32_t o = 0xC00; o < 0xC60; o += 16) {
                fprintf(stderr, "[policy-job]   LS[0x%04X]:", o);
                for (int b = 0; b < 16; b++)
                    fprintf(stderr, " %02X", ctx->ls[o + b]);
                fprintf(stderr, "\n");
            }
        }
        /* The state-3 pass queues the content image and yields to the SPURS
         * kernel while its DMA is pending. At that scheduler boundary, retain
         * the entry selected by the native policy, complete the transfer, and
         * dispatch the lifted job. The DMA intentionally overwrites the policy
         * workspace at 0x4000+, so the entry must be captured first. */
        if (native_job_entry >= 0x4000u &&
            ctx->status == SPU_STATUS_RUNNING &&
            (ctx->pc & SPU_LS_MASK) == 0x2D08) {
            fprintf(stderr, "[policy-job] HLE scheduler completing content DMA "
                            "and dispatching LS 0x%05X (resume r0=0x%05X "
                            "r3=0x%08X r4=0x%08X r78=0x%08X)\n",
                    native_job_entry, ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                    ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0],
                    ctx->gpr[78]._u32[0]);
            uint8_t policy_overlay[UC3_POLICY_OVERLAY_SIZE];
            memcpy(policy_overlay, ctx->ls + UC3_POLICY_OVERLAY_BEGIN,
                   UC3_POLICY_OVERLAY_SIZE);
            spu_dma_set_deferred(ctx, 0);
            /* Identify the ACTUAL content image the policy DMA'd for this job
             * (7D000D16=feec80, 206F9051=sscull, 434F3E02=edge0, else new). */
            fprintf(stderr, "[policy-job] content image LS[4000..]=");
            for (int ib = 0; ib < 16; ib++)
                fprintf(stderr, "%02X", ctx->ls[0x4000 + ib]);
            fprintf(stderr, " (desc 0x%08X wid %u)\n", s_feec80_job_desc, wid);
            memcpy(s_feec80_code_overlay,
                   ctx->ls + UC3_POLICY_OVERLAY_BEGIN,
                   UC3_POLICY_OVERLAY_SIZE);
            s_feec80_code_overlay_valid = true;
            memcpy(ctx->ls + UC3_POLICY_OVERLAY_BEGIN, policy_overlay,
                   UC3_POLICY_OVERLAY_SIZE);

            /* Opt-in ordering fix: resolve the landed segment's dynamic bisl
             * target to the deterministic decoder, so the generated policy
             * executes its own epilogue only after the content returns. */
            s_wkl2_sync_return_ran = false;
            s_feec80_sync_return_ran = false;
            uint32_t wkl2_return_target = 0;
            uint32_t feec80_return_target = 0;
            if (getenv("UC3_WKL2_EPILOGUE") &&
                getenv("UC3_WKL2_DECODE") &&
                spu_ls_read32(ctx, native_job_entry + 0x20u) == 0xC0DEC0DEu) {
                wkl2_return_target =
                    (native_job_entry +
                     spu_ls_read32(ctx, native_job_entry + 0x10u)) & SPU_LS_MASK;
                if (wkl2_return_target >= 0x4000u) {
                    spu_begin_image(UC3_SPU_IMAGE_POLICY_EDGE);
                    spu_register_function(wkl2_return_target,
                                          uc3_wkl2_sync_return);
                    fprintf(stderr, "[wkl2-return] armed target=0x%05X "
                                    "before policy preamble\n",
                            wkl2_return_target);
                } else {
                    wkl2_return_target = 0;
                }
            }
            if (getenv("UC3_FEEC80_EPILOGUE") && wid == 0u &&
                spu_ls_read32(ctx, native_job_entry) == 0x340240A1u &&
                spu_ls_read32(ctx, native_job_entry + 0x20u) != 0xC0DEC0DEu) {
                /* Reproduce 0x31E0's raw add exactly. For this image it is
                 * 0x4000 + 0x2894B0A0 = 0x2894F0A0, while the LS view is
                 * 0x0F0A0. Registering the raw value catches the first lookup
                 * without relying on the optional overlay fallback. */
                feec80_return_target =
                    native_job_entry + spu_ls_read32(ctx, native_job_entry + 0x10u);
                const uint32_t target_ls = feec80_return_target & SPU_LS_MASK;
                if (target_ls >= 0x4000u) {
                    spu_begin_image(UC3_SPU_IMAGE_POLICY_EDGE);
                    spu_register_function(feec80_return_target,
                                          uc3_feec80_sync_return);
                    fprintf(stderr, "[feec80-return] armed raw=0x%08X "
                                    "ls=0x%05X before policy preamble\n",
                            feec80_return_target, target_ls);
                } else {
                    feec80_return_target = 0;
                }
            }

            memset(&ctx->gpr[0], 0, sizeof(ctx->gpr[0]));
            ctx->gpr[0]._u32[0] = 0x2C78u;
            ctx->pc = 0x31E0u;
            policy_spu_func_000031E0(ctx);
            if (wkl2_return_target || feec80_return_target) {
                const bool content_return_ran =
                    s_wkl2_sync_return_ran || s_feec80_sync_return_ran;
                fprintf(stderr, "[policy-content-return] family=%s ran=%u "
                                "pc=0x%05X r0=0x%05X status=0x%X cmd=0x%08X "
                                "ring-c0=0x%08X\n",
                        feec80_return_target ? "feec80" : "wkl2",
                        content_return_ran ? 1u : 0u,
                        ctx->pc & SPU_LS_MASK,
                        ctx->gpr[0]._u32[0] & SPU_LS_MASK, ctx->status,
                        spu_ls_read32(ctx, UC3_POLICY_COMMAND_LSA),
                        vm_read32(warg + 0x40u));

                /* After the lifted content and Job Manager epilogue return,
                 * the policy yields at the resident-kernel trampoline 0x2D08.
                 * Its link register already names the policy cleanup path.
                 * Follow that exact link only for the proven WKL2 completion;
                 * do not synthesize a command, signal, or guest-memory state. */
                uint32_t final_resume =
                    ctx->gpr[0]._u32[0] & SPU_LS_MASK;
                const uint32_t final_command =
                    spu_ls_read32(ctx, UC3_POLICY_COMMAND_LSA);
                if (getenv("UC3_POLICY_FINAL_RETURN") &&
                    content_return_ran &&
                    ctx->status == SPU_STATUS_RUNNING &&
                    (final_command & 0xFFFFu) == 0u) {
                    const uint32_t content_resume =
                        ctx->gpr[5]._u32[0] & SPU_LS_MASK;
                    const uint32_t saved_resume =
                        spu_ls_read32(ctx, 0x1530u) & SPU_LS_MASK;
                    if (content_resume == 0x2C78u &&
                        saved_resume == content_resume) {
                        fprintf(stderr, "[policy-content-continuation] before "
                                        "pc=0x%05X link=0x%05X saved=0x%05X "
                                        "status=0x%X cmd=0x%08X\n",
                                ctx->pc & SPU_LS_MASK, content_resume,
                                saved_resume, ctx->status, final_command);
                        ctx->pc = content_resume;
                        spu_indirect_branch(ctx);
                        final_resume = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
                        fprintf(stderr, "[policy-content-continuation] after "
                                        "pc=0x%05X r0=0x%05X status=0x%X "
                                        "cmd=0x%08X\n",
                                ctx->pc & SPU_LS_MASK, final_resume,
                                ctx->status,
                                spu_ls_read32(ctx, UC3_POLICY_COMMAND_LSA));
                    }
                }
                if (getenv("UC3_POLICY_FINAL_RETURN") &&
                    content_return_ran &&
                    ctx->status == SPU_STATUS_RUNNING &&
                    (ctx->pc & SPU_LS_MASK) == 0x2D08u &&
                    final_resume == 0x2D10u &&
                    (final_command & 0xFFFFu) == 0u) {
                    const uint32_t ring_before = vm_read32(warg + 0x40u);
                    fprintf(stderr, "[policy-final-return] before pc=0x%05X "
                                    "r0=0x%05X status=0x%X cmd=0x%08X "
                                    "ring-c0=0x%08X\n",
                            ctx->pc & SPU_LS_MASK, final_resume, ctx->status,
                            final_command, ring_before);
                    ctx->pc = final_resume;
                    spu_indirect_branch(ctx);
                    fprintf(stderr, "[policy-final-return] after pc=0x%05X "
                                    "r0=0x%05X status=0x%X cmd=0x%08X "
                                    "ring-c0=0x%08X->0x%08X\n",
                            ctx->pc & SPU_LS_MASK,
                            ctx->gpr[0]._u32[0] & SPU_LS_MASK, ctx->status,
                            spu_ls_read32(ctx, UC3_POLICY_COMMAND_LSA), ring_before,
                            vm_read32(warg + 0x40u));

                    /* 0x2D24 installs 0x2D30 in r0 and clears r2 before its
                     * host-lowered `bi r0` returns. ctx->pc still names the last
                     * called helper, so the restored r0 is the authoritative
                     * continuation. The corrected lqa target for the resident
                     * kernel interface is LS 0x1E0. */
                    if (getenv("UC3_POLICY_MODULE_EXIT") &&
                        ctx->status == SPU_STATUS_RUNNING &&
                        (ctx->gpr[0]._u32[0] & SPU_LS_MASK) == 0x2D30u &&
                        ctx->gpr[2]._u32[0] == 0u) {
                        const uint32_t module_exit_target =
                            spu_ls_read32(ctx, 0x1E0u);
                        static std::once_flag module_exit_registration;
                        std::call_once(module_exit_registration,
                                      [module_exit_target] {
                            spu_begin_image(UC3_SPU_IMAGE_POLICY_EDGE);
                            spu_register_function(module_exit_target,
                                                  uc3_spurs_module_exit_hle);
                        });
                        fprintf(stderr, "[policy-module-exit] resume=0x02D30 "
                                        "interface-raw=0x%08X interface-ls=0x%05X\n",
                                module_exit_target,
                                module_exit_target & SPU_LS_MASK);
                        ctx->pc = 0x2D30u;
                        spu_indirect_branch(ctx);
                        fprintf(stderr, "[policy-module-exit] ERROR: "
                                        "non-returning interface returned "
                                        "pc=0x%05X status=0x%X\n",
                                ctx->pc & SPU_LS_MASK, ctx->status);
                    }
                }
            }
            /* The preamble sets up the job's argument registers and issues its
             * input-DMA brsl helpers, then `bisl r5` where r5 = LS[0x4C80] +
             * LS[0x4010]. For a raw cellSpursJob2 image (feec80) there is no
             * header word at +0x10, so r5 is garbage and the branch halts. The
             * real content-job entry is simply LS 0x4000 (the job main). Re-enter
             * it directly, preserving the registers/DMA the preamble established. */
            if (ctx->status != SPU_STATUS_RUNNING &&
                native_job_entry >= 0x4000u && native_job_entry < 0x5B10u) {
                fprintf(stderr, "[policy-job] preamble done (halt at 0x%05X); "
                                "entering content job at LS 0x%05X\n",
                        ctx->pc & SPU_LS_MASK, native_job_entry);
                fprintf(stderr, "[policy-job]   job args: r24=%08X r25=%08X "
                                "r26=%08X r83=%08X r85=%08X r89=%08X\n",
                        ctx->gpr[24]._u32[0], ctx->gpr[25]._u32[0],
                        ctx->gpr[26]._u32[0], ctx->gpr[83]._u32[0],
                        ctx->gpr[85]._u32[0], ctx->gpr[89]._u32[0]);
                /* Route by the LANDED image family. Edge0-family Jobbin2 images
                 * carry their bbox/size header with the C0DEC0DE cookie at
                 * +0x20 and their code AFTER the header (edge0's lifted entry is
                 * a veneer that skips to 0x4058); run them with the lifted
                 * edge0 program on their own landed content. Raw feec80 images
                 * (code at +0x0) keep the historic feec80 path. */
                if (spu_ls_read32(ctx, 0x4020) == 0xC0DEC0DEu) {
                    /* One-shot: dump the landed Jobbin2 image so the wkl[2] job
                     * program can be lifted (its real entry is header-derived:
                     * LS[0x4010] -> 0x5528, covered by no lifted table yet). */
                    static std::atomic<bool> s_wkl2_img_dumped{false};
                    if (!s_wkl2_img_dumped.exchange(true)) {
                        if (FILE* f = fopen("spu_programs/wkl2_job_image.bin", "wb")) {
                            fwrite(ctx->ls + 0x4000, 1, 0xC000, f);
                            fclose(f);
                            fprintf(stderr, "[policy-job] dumped landed wkl2 "
                                    "image (0xC000) -> spu_programs/"
                                    "wkl2_job_image.bin\n");
                        }
                    }
                    /* IDENTIFIED (bytes, not guesses): this payload is NOT a
                     * program — it is an Edge geometry SEGMENT: bbox header,
                     * shuffle table at +0xC0, zero fill around the header's
                     * "entry" offsets (0x1528/0x1740 = STREAM offsets), and
                     * high-entropy COMPRESSED STREAMS from +0x1800. Their
                     * consumer is the Edge geometry library lifted INSIDE
                     * feec80 (LS 0x5B10-0x10000; the +0xC0 shuffle table is
                     * Edge's shufb-based stream unpacking) — NOT the ov1/ov2
                     * overlays (navigation jobs, cf. STATUS Phase 13 note).
                     * No content job to run here; return through the epilogue
                     * so the queue bookkeeping advances. The captured segment
                     * (spu_programs/wkl2_job_image.bin) is the harness input
                     * for the feec80-decodes-segment experiment (next lock). */
                    fprintf(stderr, "[policy-job] edge segment payload "
                                    "(streams@+0x1800)\n");
                    /* Live segment decode (gated UC3_WKL2_DECODE). Recipe proven
                     * in the feec80 harness: the decoder's geometry input is LS
                     * 0x28000-0x2C000; placing this segment's stream body (landed
                     * at LS 0x4000+0x1800) there and running feec80 decodes THIS
                     * segment's geometry (output differs per segment). PUTBACK to
                     * the segment descriptor's output field. The preamble already
                     * set this job's buffer registers (r83/r85/r89). */
                    if (getenv("UC3_WKL2_DECODE") != nullptr) {
                        if (s_wkl2_sync_return_ran) {
                            fprintf(stderr, "[wkl2-decode] segment already "
                                            "completed inside policy bisl\n");
                        } else {
                            /* Legacy ordering retained when the experiment is
                             * disabled, so the new path has a strict baseline. */
                            uc3_decode_wkl2_segment(ctx);
                        }
                    }
                } else {
                /* The content DMA only loaded the job (LS 0x4000-0x5B10). The
                 * Edge geometry library the job calls (LS 0x5B10-0x10000) lives
                 * contiguously right after it at guest 0x00FEEC80+0x1B10; load it
                 * so the lib's LS-resident constants/tables are present. */
                memcpy(ctx->ls + 0x5B10u,
                       vm_base + 0x00FEEC80u + 0x1B10u, 0x10000u - 0x5B10u);
                ctx->status = SPU_STATUS_RUNNING;
                ctx->pc = native_job_entry;
                uc3_feec80_dispatch(ctx);
                fprintf(stderr, "[policy-job] content job returned status=0x%X "
                                "pc=0x%05X\n", ctx->status, ctx->pc & SPU_LS_MASK);
                }
            }
            s_feec80_code_overlay_valid = false;
        }
        /* Direct job-start path (no scheduler yield, e.g. wkl[6]): the native
         * bisl at 0x32AC halts on a garbage target because the content GET is
         * still deferred. Complete the DMA now and identify what image actually
         * lands (first bytes + entry words) so the dispatch for this job family
         * can be built on data. Recovery/dispatch is the next step. */
        if (ctx->status == SPU_STATUS_STOPPED_BY_HALT &&
            native_job_entry == 0) {
            /* NOTE: after an unknown-branch halt the lifted C frames keep
             * unwinding and overwrite r0/pc, so the halt origin can only be
             * read from the [SPU] unknown-branch log line, not from ctx here. */
            spu_dma_set_deferred(ctx, 0);
            uint32_t e4c80 = spu_ls_read32(ctx, UC3_POLICY_CONTENT_LSA);
            fprintf(stderr, "[policy-job] direct job-start halt (r0=0x%05X): "
                            "DMAs completed; LS[4C80]=0x%08X LS[4010]=0x%08X "
                            "LS[4000..]=",
                    ctx->gpr[0]._u32[0] & SPU_LS_MASK, e4c80,
                    spu_ls_read32(ctx, 0x4010));
            for (int i = 0; i < 16; i++)
                fprintf(stderr, "%02X", ctx->ls[0x4000 + i]);
            fprintf(stderr, " feec80@guest=");
            for (int i = 0; i < 16; i++)
                fprintf(stderr, "%02X", vm_read8(0x00FEEC80u + i));
            fprintf(stderr, "\n");
            if (getenv("UC3_SSCULL_JOB2_PROBE") != nullptr) {
                static std::atomic<bool> s_sscull_job2_probed{false};
                if (!s_sscull_job2_probed.exchange(true)) {
                    constexpr uint32_t nested_lsa = 0x4900u;
                    constexpr uint32_t nested_ea = 0x00ECC480u;
                    fprintf(stderr,
                            "[sscull-job2-probe] r3=%08X r4=%08X r81=%08X "
                            "desc=%08X | LS[%05X]=%08X %08X %08X %08X "
                            "hdr=%08X entry=%08X size=%08X load=%08X | "
                            "guest[%08X]=%08X %08X %08X %08X "
                            "hdr=%08X entry=%08X size=%08X load=%08X\n",
                            ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0],
                            ctx->gpr[81]._u32[0], s_feec80_job_desc,
                            nested_lsa,
                            spu_ls_read32(ctx, nested_lsa + 0x00u),
                            spu_ls_read32(ctx, nested_lsa + 0x04u),
                            spu_ls_read32(ctx, nested_lsa + 0x08u),
                            spu_ls_read32(ctx, nested_lsa + 0x0Cu),
                            spu_ls_read32(ctx, nested_lsa + 0x20u),
                            spu_ls_read32(ctx, nested_lsa + 0x10u),
                            spu_ls_read32(ctx, nested_lsa + 0x14u),
                            spu_ls_read32(ctx, nested_lsa + 0x28u),
                            nested_ea,
                            vm_read32(nested_ea + 0x00u),
                            vm_read32(nested_ea + 0x04u),
                            vm_read32(nested_ea + 0x08u),
                            vm_read32(nested_ea + 0x0Cu),
                            vm_read32(nested_ea + 0x20u),
                            vm_read32(nested_ea + 0x10u),
                            vm_read32(nested_ea + 0x14u),
                            vm_read32(nested_ea + 0x28u));
                }
            }
            /* The landed image identifies the job family. 206F9051... = the
             * screen-space light-culling job (guest 0x00C51380, lifted as
             * spu_gen/edge_sscull). The native 0x31E0 preamble already prepared
             * the job registers and input DMAs before its bisl halted, so enter
             * the lifted job main at LS 0x4000 with state as-is. */
            if (ctx->ls[0x4000] == 0x20 && ctx->ls[0x4001] == 0x6F &&
                ctx->ls[0x4002] == 0x90 && ctx->ls[0x4003] == 0x51) {
                const bool use_job2 = getenv("UC3_SSCULL_JOB2") != nullptr;
                s_sscull_job2_source_ea = 0;
                if (use_job2 && ea_binary >= 0x10000u) {
                    /* The policy's public pointer is 0x10000 past its module
                     * DMA source. The nested Jobbin2 header is at source+0x900;
                     * validate both against the bytes that actually landed. */
                    const uint32_t outer_ea = ea_binary - 0x10000u;
                    const uint32_t nested_ea = outer_ea + 0x900u;
                    if (vm_read32(outer_ea) == spu_ls_read32(ctx, 0x4000u) &&
                        vm_read32(nested_ea) == spu_ls_read32(ctx, 0x4900u) &&
                        vm_read32(nested_ea + 0x20u) == 0xC0DEC0DEu) {
                        s_sscull_job2_source_ea = nested_ea;
                    } else {
                        fprintf(stderr,
                                "[sscull-job2] source validation failed "
                                "public=0x%08X outer=0x%08X nested=0x%08X\n",
                                ea_binary, outer_ea, nested_ea);
                    }
                }
                static std::atomic<bool> s_sscull_registered{false};
                if (!s_sscull_registered.exchange(true)) {
                    spu_begin_image(UC3_SPU_IMAGE_SSCULL);
                    if (s_sscull_job2_source_ea)
                        spu_register_function(0x3BC80u,
                                              uc3_sscull_job2_service);
                    sscull_spu_recomp_register();
                }
                int prev_image = ctx->image_id;
                ctx->image_id = UC3_SPU_IMAGE_SSCULL;
                ctx->status = SPU_STATUS_RUNNING;
                ctx->pc = 0x4000;
                fprintf(stderr, "[policy-job] dispatching screen-space-culling "
                                "job (lifted sscull%s) at LS 0x4000\n",
                        s_sscull_job2_source_ea ? "+Jobbin2" : "");
                sscull_spu_func_00004000(ctx);
                fprintf(stderr, "[policy-job] sscull job returned status=0x%X "
                                "pc=0x%05X\n",
                        ctx->status, ctx->pc & SPU_LS_MASK);
                ctx->image_id = prev_image;
            }
        }
        fprintf(stderr, "[policy-job] <<< returned status=0x%X pc=0x%05X\n",
                ctx->status, ctx->pc & SPU_LS_MASK);
    } else if (policy_jump == UC3_SPU_JUMP_MODULE_EXIT &&
               s_policy_module_exit_ran) {
        fprintf(stderr, "[policy-job] <<< HLE module exit at kernel pc=0x%05X "
                        "status=0x%X\n",
                ctx->pc & SPU_LS_MASK, ctx->status);
    } else {
        fprintf(stderr, "[policy-job] <<< stopped by runaway guard at pc=0x%05X\n",
                ctx->pc & SPU_LS_MASK);
    }
    if (trace_policy_pc)
        uc3_policy_pc_trace_end(wid);
    spu_abort_arm(0);
    s_policy_last_run_completed =
        s_policy_module_exit_ran &&
        (s_wkl2_sync_return_ran || s_feec80_sync_return_ran);
    if (s_wkl2_sync_return_ran || s_feec80_sync_return_ran)
        fprintf(stderr, "[policy-job] completion contract content=%u "
                        "module-exit=%u -> %u\n",
                (s_wkl2_sync_return_ran || s_feec80_sync_return_ran) ? 1u : 0u,
                s_policy_module_exit_ran ? 1u : 0u,
                s_policy_last_run_completed ? 1u : 0u);

    for (uint32_t lsa : {0x0080u,
                         0x0D00u, 0x0D80u, 0x0DF0u, 0x0E30u,
                         0x0EB0u, 0x0EC0u, 0x10B0u,
                         0x11B0u, 0x11D0u, 0x11F0u, 0x1210u,
                         0x0170u, 0x01C0u, 0x01D0u, 0x01E0u, 0x0210u,
                         0x1230u, 0x1250u, 0x1270u, 0x1290u, 0x12A0u,
                         0x12B0u, 0x12C0u, 0x12D0u,
                         0x12F0u, 0x1320u, 0x1330u,
                         0x1380u, 0x13A0u, 0x13C0u, 0x13E0u,
                         0x1400u, 0x1410u, 0x1420u, 0x1430u,
                         0x1440u, 0x1450u,
                         0x1460u, 0x1470u, 0x1480u, 0x14E0u, 0x1580u,
                         0x4A80u, 0x4B00u, 0x4B40u, 0x4BC0u, 0x4C80u, 0x4CC0u,
                         0x5000u, 0x5040u, 0x5080u, 0x50C0u,
                         0x5100u, 0x5130u, 0x5140u})
        uc3_log_policy_quad(ctx, lsa);

    /* ÉTAPE 4 EXPÉRIENCE (gate UC3_SCHED_POKE) : après avoir fait tourner la tâche
     * SPU, simuler le signal de complétion que le handler PPU du jeu (highSpursHdlr)
     * pousserait — le scheduler func_00038FBC idle sur *(0x011E1A24)==-1 && flag
     * @0x011DFDA8==0. On attend l'idle stable puis on pose le flag, et on observe si
     * le jeu avance (cellGcmSetFlipCommand) ou crashe (=> il faut un vrai work-item).
     * Gated : le build par défaut est intact. */
    if (getenv("UC3_SCHED_POKE") != nullptr) {
        static bool poked = false;
        if (!poked) {
            poked = true;
            std::thread([]{
                int stable = 0;
                for (int i = 0; i < 600 && stable < 5; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if (vm_read32(0x011E1A24u) == 0xFFFFFFFFu &&
                        vm_read8(0x011DFDA8u) == 0) stable++;
                    else stable = 0;
                }
                fprintf(stderr, "[sched-poke] idle stable; before: work=0x%08X flag=0x%02X\n",
                        vm_read32(0x011E1A24u), vm_read8(0x011DFDA8u));
                vm_write8(0x011DFDA8u, 1);   /* simulate completion handler */
                fprintf(stderr, "[sched-poke] wrote flag@0x011DFDA8 = 1\n");
                for (int i = 0; i < 50; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                fprintf(stderr, "[sched-poke] after 5s: work=0x%08X flag=0x%02X\n",
                        vm_read32(0x011E1A24u), vm_read8(0x011DFDA8u));
            }).detach();
        }
    }
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
    /* edgezlib_inflate_ta decompressor (RPCS3 ground truth: SPU @0x010CF700) —
     * dump its task-argument (EdgeZlib context) so we can locate the work queue
     * the PPU pushes compressed items to, and HLE it with host zlib. */
    if ((uint32_t)ctx->gpr[5] == 0x010CF700u) {
        uint32_t arg = (uint32_t)ctx->gpr[9];
        fprintf(stderr, "[edgezlib-ctx] arg=0x%08X:\n", arg);
        for (uint32_t o = 0; o < 0x40; o += 16) {
            fprintf(stderr, "    +0x%02X:", o);
            for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", vm_read8(arg + o + i));
            fprintf(stderr, "\n");
        }
        /* follow plausible pointers one level (incl. SPURS-region 0x31xxxxxx) */
        for (uint32_t o = 0; o < 0x20; o += 4) {
            uint32_t p = vm_read32(arg + o);
            if (p >= 0x10000 && p < 0x40000000) {
                fprintf(stderr, "    ptr@+0x%02X=0x%08X ->", o, p);
                for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", vm_read8(p + i));
                fprintf(stderr, "\n");
            }
        }
        /* Decompression queue monitor (UC3_ZLIB_MON): the EdgeZlib work queue is
         * arg+0x00. Poll it; dump structure + any changes so we can decode the
         * work-item format and see whether items are pushed by the menu. */
        if (getenv("UC3_ZLIB_MON") != nullptr) {
            uint32_t queue = vm_read32(arg + 0x00);
            std::thread([queue]{
                /* Watch the header (0x00-0x60) AND the ring items buffer. The
                 * buffer EA is the 64-bit ptr at queue+0x18 (low word +0x1C);
                 * depth*elem = 12*0x20 = 0x180 bytes. Dump any changed 16-byte
                 * row so we can see real 32-byte work-items land (or prove the
                 * ring stays empty = push happens elsewhere). Read-only. */
                uint32_t items = vm_read32(queue + 0x1C);
                fprintf(stderr, "[zlib-mon] watching queue 0x%08X items=0x%08X\n",
                        queue, items);
                uint8_t hlast[0x60] = {0};
                uint8_t ilast[0x180] = {0};
                for (int iter = 0;; iter++) {
                    bool hchanged = (iter == 0);
                    for (int i = 0; i < 0x60; i++) {
                        uint8_t b = vm_read8(queue + i);
                        if (b != hlast[i]) { hchanged = true; hlast[i] = b; }
                    }
                    if (hchanged)
                        for (uint32_t o = 0; o < 0x60; o += 16) {
                            fprintf(stderr, "[zlib-mon] q+0x%02X:", o);
                            for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", vm_read8(queue + o + i));
                            fprintf(stderr, "\n");
                        }
                    if (items >= 0x10000 && items < 0x40000000)
                        for (uint32_t o = 0; o < 0x180; o += 16) {
                            bool rchanged = (iter == 0);
                            for (int i = 0; i < 16; i++) {
                                uint8_t b = vm_read8(items + o + i);
                                if (b != ilast[o + i]) { rchanged = true; ilast[o + i] = b; }
                            }
                            if (rchanged) {
                                fprintf(stderr, "[zlib-mon] item+0x%03X:", o);
                                for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", vm_read8(items + o + i));
                                fprintf(stderr, "\n");
                            }
                        }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }).detach();
        }
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
            uint32_t policy_ea = spurs ? vm_read32(spurs + 0xB00 + 0x04) : 0;
            uint32_t policy_size = spurs ? vm_read32(spurs + 0xB00 + 0x10) : 0;
            std::thread([warg, spurs, policy_ea, policy_size]{
                uint32_t sub = vm_read32(warg + 0x30);
                fprintf(stderr, "[ringmon] watching control=0x%08X sub=0x%08X\n", warg, sub);
                /* Identify every registered SPURS workload: wklInfo1[i] @
                 * spurs+0xB00 (0x20 bytes each: pm EA @+0, arg @+8 (64-bit),
                 * size @+0x10, uniqueId @+0x14), readyCount1 @+0x00, state @+0x80.
                 * The game signals work by bumping readyCount[i]; knowing each
                 * workload's pm/arg tells the executor which ring to watch. */
                if (spurs) {
                    for (uint32_t i = 0; i < 16; ++i) {
                        uint32_t base = spurs + 0xB00 + i * 0x20;
                        uint32_t pm   = vm_read32(base + 0x04); /* pm EA64 low */
                        uint32_t arg  = vm_read32(base + 0x0C); /* arg64 low */
                        uint32_t sz   = vm_read32(base + 0x10);
                        if (!pm && !arg) continue;
                        fprintf(stderr, "[ringmon] wkl[%2u] pm=0x%08X arg=0x%08X "
                                "size=0x%X ready=%u state=%u\n",
                                i, pm, arg, sz,
                                vm_read8(spurs + i),
                                vm_read8(spurs + 0x80 + i));
                    }
                }
                /* The game runs MANY workloads (16 seen), each with its own ring
                 * (wklInfo1[i].arg) and policy module. Completion of one ring's
                 * jobs makes the game signal readyCount on ANOTHER workload, so
                 * the executor must watch every ring, not just wkl[0]'s. */
                struct RingState {
                    uint32_t arg, pm, size, wid;
                    uint32_t promoted;    /* items promoted from sub1 so far */
                    bool     force;       /* readyCount rose: rescan even if
                                           * slots/hash unchanged */
                    uint32_t last_c0_disp; /* c0 au dernier dispatch: re-force
                                            * tant que la fenetre sub0 a des
                                            * items non consommes ET que le
                                            * dispatch precedent a progresse
                                            * (ringstat gd6: c0=0x00020001
                                            * fige = 1 seul item consomme par
                                            * dispatch, drain bloque). */
                    uint8_t  slots[0x40];
                };
                std::vector<RingState> rings;
                auto refresh_rings = [&] {
                    if (!spurs) return;
                    for (uint32_t i = 0; i < 16; ++i) {
                        uint32_t base = spurs + 0xB00 + i * 0x20;
                        uint32_t pm   = vm_read32(base + 0x04);
                        uint32_t arg  = vm_read32(base + 0x0C);
                        uint32_t sz   = vm_read32(base + 0x10);
                        if (!arg) continue;
                        bool known = false;
                        for (auto& r : rings)
                            if (r.arg == arg) { known = true; break; }
                        if (!known) {
                            RingState rs{};
                            rs.arg = arg; rs.pm = pm; rs.size = sz; rs.wid = i;
                            rings.push_back(rs);
                            fprintf(stderr, "[ringmon] tracking ring 0x%08X "
                                    "(wkl %u, pm=0x%08X size=0x%X)\n",
                                    arg, i, pm, sz);
                        }
                    }
                };
                uint8_t last_ready[16] = {0};
                uint8_t last_slots[0x40] = {0}; int reported = 0;
                /* Post-job watcher (completion-propagation research): after each
                 * policy run, snapshot the jobchain header, the ring control and
                 * the SPURS workload state, then log every word the PPU (or our
                 * next run) changes for ~5 s. Reveals what the game polls/writes
                 * while waiting for job completion. */
                std::vector<std::pair<uint32_t, uint32_t>> watch_words;
                int watch_ticks = 0;
                int watch_lines = 0;
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
                        /* parse the job descriptors as CellSpursJob256
                         * (libs/spurs/cellSpursJq.h): header@0, dmaList[7]@0x08,
                         * workArea@0x40, eaJobBinary@0x48, sizeDmaList@0x50,
                         * sizeJobBinary@0x54. eaJobBinary low 32 = the SPU code. */
                        for (uint32_t e = 0; e < 0x20; e += 8) {
                            uint32_t jp = vm_read32(sub + e + 4);
                            if (jp >= 0x10000 && jp < 0x40000000) {
                                uint32_t eaBin   = vm_read32(jp + 0x48);     /* EA @+0x48 */
                                uint32_t szBin   = vm_read32(jp + 0x4C);     /* size @+0x4C */
                                uint32_t szDma   = vm_read32(jp + 0x50);
                                uint32_t hdrlo   = vm_read32(jp + 0x04);
                                fprintf(stderr, "    JOB@0x%08X header=..%08X eaJobBinary=0x%08X "
                                        "sizeJobBin=0x%X sizeDma=0x%X\n",
                                        jp, hdrlo, eaBin, szBin, szDma);
                                if (eaBin >= 0x10000 && eaBin < 0x40000000) {
                                    fprintf(stderr, "      binary@0x%08X:", eaBin);
                                    for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", vm_read8(eaBin+i));
                                    fprintf(stderr, "\n");
                                }
                                /* Full 256-byte job dump + scan for fields that look
                                 * like EAs (plausible code pointers) to find eaBinary. */
                                fprintf(stderr, "      JOB256 full dump:\n");
                                for (uint32_t o = 0; o < 0x100; o += 16) {
                                    fprintf(stderr, "        +0x%02X:", o);
                                    for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", vm_read8(jp+o+i));
                                    fprintf(stderr, "\n");
                                }
                                fprintf(stderr, "      EA-like fields (u32, in code range):\n");
                                for (uint32_t o = 0; o < 0x60; o += 4) {
                                    uint32_t v = vm_read32(jp + o);
                                    if ((v >= 0x00010000 && v < 0x01400000) ||
                                        (v >= 0x20000000 && v < 0x31000000)) {
                                        uint32_t w0 = vm_read32(v);
                                        if (w0 != 0) {  /* populated target -> dump 48 bytes */
                                            fprintf(stderr, "        +0x%02X = 0x%08X (POPULATED) ->", o, v);
                                            for (int i = 0; i < 48; i++) fprintf(stderr, " %02X", vm_read8(v+i));
                                            fprintf(stderr, "\n");
                                        }
                                    }
                                }
                            }
                        }
                    }
                    /* Continuous Edge-job executor (doc Phase 11, HLE-first, in
                     * the game's own flow), generalized to EVERY workload ring:
                     * on any ring's slot-table change, scan its job pointers for
                     * Edge Jobbin2 chains (C0DEC0DE @ +0x20) and run that ring's
                     * policy for each NEW or CHANGED chain (FNV content hash) —
                     * the game rewrites/advances chains as jobs complete and
                     * signals readyCount on sibling workloads. */
                    refresh_rings();
                    if (spurs) {
                        for (int i = 0; i < 16; ++i) {
                            uint8_t rc = vm_read8(spurs + i);
                            if (rc != last_ready[i]) {
                                fprintf(stderr, "[ringmon] readyCount[%d]: "
                                        "%u -> %u\n", i, last_ready[i], rc);
                                /* The game signals new work via readyCount, not
                                 * necessarily by rewriting the chain: force that
                                 * workload's ring to re-dispatch. */
                                if (rc > last_ready[i]) {
                                    uint32_t wa = vm_read32(
                                        spurs + 0xB00 + (uint32_t)i * 0x20 + 0x0C);
                                    for (auto& r : rings)
                                        if (r.arg == wa) { r.force = true; break; }
                                    /* Identify what this workload's ring holds
                                     * when the game signals it (one dump per
                                     * workload): ring header + sub block. */
                                    static uint32_t s_dumped_wkl = 0;
                                    if (wa && !(s_dumped_wkl & (1u << i))) {
                                        s_dumped_wkl |= 1u << i;
                                        fprintf(stderr, "[ringmon] wkl[%d] ring "
                                                "0x%08X on signal:\n", i, wa);
                                        for (uint32_t o = 0; o < 0x60; o += 16) {
                                            fprintf(stderr, "    +0x%02X:", o);
                                            for (int b = 0; b < 16; b++)
                                                fprintf(stderr, " %02X",
                                                        vm_read8(wa + o + b));
                                            fprintf(stderr, "\n");
                                        }
                                        uint32_t ws = vm_read32(wa + 0x30);
                                        if (ws >= 0x10000 && ws < 0x40000000) {
                                            fprintf(stderr, "    sub 0x%08X:\n", ws);
                                            for (uint32_t o = 0; o < 0x40; o += 16) {
                                                fprintf(stderr, "      +0x%02X:", o);
                                                for (int b = 0; b < 16; b++)
                                                    fprintf(stderr, " %02X",
                                                            vm_read8(ws + o + b));
                                                fprintf(stderr, "\n");
                                            }
                                        }
                                    }
                                }
                                last_ready[i] = rc;
                            }
                        }
                    }
                    for (auto& ring : rings) {
                        bool rchanged = false;
                        for (int i = 0; i < 0x40; i++) {
                            uint8_t b = vm_read8(ring.arg + 0x40 + i);
                            if (b != ring.slots[i]) {
                                rchanged = true;
                                ring.slots[i] = b;
                            }
                        }
                        /* A standalone policy dispatch has no SPURS kernel
                         * epilogue. Once UC3 resets the producer cursor and no
                         * changed chain remains, retire the unused scheduling
                         * requests so PPU frame synchronization can complete. */
                        uint32_t terminal_c0 = vm_read32(ring.arg + 0x40);
                        /* Cas fenetre-vide (fw2.log, frame 2) : wid 2 garde
                         * c0=prod2/cons1 mais sa fenetre sub0 est INTEGRALE-
                         * MENT nulle et sub1 est vide — l'item restant est un
                         * fantome, il n'y a rien a consommer. L'epilogue
                         * kernel aurait aligne les curseurs actifs sur le
                         * niveau deja acte par le consommateur (cons), pas
                         * au-dela (aucune completion inventee). */
                        bool empty_window = false;
                        if (!rchanged && !ring.force &&
                            (terminal_c0 >> 16) != (terminal_c0 & 0xFFFFu) &&
                            (terminal_c0 & 0xFFFFu) != 0u) {
                            uint32_t s0 = vm_read32(ring.arg + 0x30);
                            uint32_t c1 = vm_read32(ring.arg + 0x50);
                            if (s0 >= 0x10000u &&
                                (c1 >> 16) == (c1 & 0xFFFFu)) {
                                empty_window = true;
                                for (uint32_t it = 0; it < 4 && empty_window;
                                     ++it)
                                    if (vm_read32(s0 + it * 8) ||
                                        vm_read32(s0 + it * 8 + 4))
                                        empty_window = false;
                            }
                        }
                        if (empty_window && spurs && ring.wid < 16u &&
                            vm_read8(spurs + 0x20u + ring.wid) == 0u) {
                            uint16_t cons = (uint16_t)terminal_c0;
                            bool wm = false;
                            for (uint32_t off = 0x42; off <= 0x4E; off += 2) {
                                uint16_t c = vm_read16(ring.arg + off);
                                if (c != 0xFFFFu && c < cons) {
                                    vm_write16(ring.arg + off, cons);
                                    wm = true;
                                }
                            }
                            if (wm)
                                fprintf(stderr,
                                        "[wkl-retire-wm] wid=%u ring=0x%08X "
                                        "fenetre vide, watermark=%u publie "
                                        "(c0=0x%08X)\n",
                                        ring.wid, ring.arg, cons, terminal_c0);
                        }
                        if (!rchanged && !ring.force &&
                            (terminal_c0 >> 16) == 0u &&
                            (terminal_c0 & 0xFFFFu) != 0u &&
                            spurs && ring.wid < 16u &&
                            vm_read8(spurs + 0x20u + ring.wid) == 0u) {
                            uint8_t ready = vm_read8(spurs + ring.wid);
                            if (ready) {
                                vm_write8(spurs + ring.wid, 0);
                                fprintf(stderr,
                                        "[wkl-retire] wid=%u ready=%u->0 ring=0x%08X c0=0x%08X\n",
                                        ring.wid, ready, ring.arg, terminal_c0);
                            }
                            /* Epilogue SPURS manquant (STATUS frame 2) : sur
                             * ce ring quiescent-terminal (producteur remis a
                             * zero, curseur consommateur conserve), publier
                             * le watermark que le kernel aurait ecrit — meme
                             * regle que [job-watermark] post-dispatch :
                             * elever tout curseur valide < consomme.
                             * func_00D51D60 boucle sinon sur le min de
                             * +0x42..+0x4E. Aucune autre ecriture. */
                            {
                                uint16_t consumed = (uint16_t)terminal_c0;
                                bool wm = false;
                                for (uint32_t off = 0x42; off <= 0x4E;
                                     off += 2) {
                                    uint16_t c = vm_read16(ring.arg + off);
                                    if (c != 0xFFFFu && c < consumed) {
                                        vm_write16(ring.arg + off, consumed);
                                        wm = true;
                                    }
                                }
                                if (wm)
                                    fprintf(stderr,
                                            "[wkl-retire-wm] wid=%u ring=0x%08X"
                                            " watermark=%u publie\n",
                                            ring.wid, ring.arg, consumed);
                            }
                        }
                        if (!rchanged && !ring.force) continue;
                        bool forced = ring.force;
                        ring.force = false;
                        static std::map<uint32_t, uint64_t> s_seen_chains;
                        static int s_policy_runs = 0;
                        /* A ring can carry several sub-queues (wkl[2] has two,
                         * at +0x30 and +0x34); scan job pointers in each. */
                        for (uint32_t si = 0; si < 2; ++si) {
                        uint32_t rsub = vm_read32(ring.arg + 0x30 + si * 4);
                        if (rsub < 0x10000 || rsub >= 0x40000000) continue;
                        for (uint32_t e = 0; e < 0x20; e += 8) {
                            uint32_t jp = vm_read32(rsub + e + 4);
                            if (jp < 0x10000 || jp >= 0x40000000) continue;
                            for (uint32_t o = 0; o < 0x60; o += 4) {
                                uint32_t v = vm_read32(jp + o);
                                if (!((v >= 0x00010000 && v < 0x01400000) ||
                                      (v >= 0x20000000 && v < 0x31000000)))
                                    continue;
                                if (vm_read32(v) == 0 ||
                                    vm_read32(v + 0x20) != 0xC0DEC0DEu)
                                    continue;
                                uint64_t h = 1469598103934665603ull; /* FNV-1a */
                                for (uint32_t i = 0; i < 0x90; ++i) {
                                    h ^= vm_read8(v + i);
                                    h *= 1099511628211ull;
                                }
                                auto it = s_seen_chains.find(v);
                                bool fresh = it == s_seen_chains.end() ||
                                             it->second != h || forced;
                                if (getenv("UC3_POLICY_JOB") != nullptr && fresh &&
                                    s_policy_runs < 128) {
                                    s_seen_chains[v] = h;
                                    ++s_policy_runs;
                                    /* Each workload carries its own policy module
                                     * and ring; fall back to wkl[0]'s when the
                                     * wklInfo entry lacks one. */
                                    uint32_t rpm = ring.pm ? ring.pm : policy_ea;
                                    uint32_t rsz = ring.size ? ring.size : policy_size;
                                    /* (sub-queue promotion is handled by the
                                     * autonomous drain step below, per tick.) */
                                    fprintf(stderr,
                                            "      >>> Edge job chain @0x%08X (run %d, "
                                            "ring 0x%08X pm 0x%08X, hash %016llX), "
                                            "executing policy\n",
                                            v, s_policy_runs, ring.arg, rpm,
                                            (unsigned long long)h);
                                    /* UC3_JOB_GUARD=<s> : l'EXECUTEUR NE MEURT
                                     * JAMAIS. Chaque dispatch tourne sur un
                                     * thread ouvrier avec date limite ; au
                                     * timeout le job est abandonne (il garde
                                     * SON contexte heap, cf. uc3_run_policy_job)
                                     * et la boucle continue. Motif : lg1.log —
                                     * le dispatch sscull (chaine wkl[6]
                                     * 0x00EDBB80) pendait le moniteur 12 min,
                                     * gelant tout le pipeline segments. Le
                                     * marquage s_seen_chains empeche le
                                     * re-dispatch du meme contenu pendu. */
                                    /* readyCount is a scheduling request count,
                                     * not a job-completion flag. SPURS consumes
                                     * one request when it selects the workload;
                                     * mirror that HLE side effect before running
                                     * the lifted content job. */
                                    if (spurs && ring.wid < 16u) {
                                        uint8_t ready = vm_read8(spurs + ring.wid);
                                        if (ready) {
                                            vm_write8(spurs + ring.wid,
                                                      (uint8_t)(ready - 1u));
                                            fprintf(stderr,
                                                    "[wkl-ack] wid=%u ready=%u->%u ring=0x%08X\n",
                                                    ring.wid, ready, ready - 1u,
                                                    ring.arg);
                                        }
                                    }
                                    bool job_completed = true;
                                    if (const char* ge = getenv("UC3_JOB_GUARD")) {
                                        int tmo_s = atoi(ge);
                                        if (tmo_s <= 0) tmo_s = 60;
                                        auto done = std::make_shared<std::atomic<bool>>(false);
                                        uint32_t rarg = ring.arg, rwid = ring.wid;
                                        std::thread([=]{
                                            uc3_run_policy_job(rpm, rsz, spurs,
                                                               rarg, jp, v, rwid);
                                            done->store(true);
                                        }).detach();
                                        for (int w = 0; w < tmo_s * 10 && !done->load(); ++w)
                                            std::this_thread::sleep_for(
                                                std::chrono::milliseconds(100));
                                        job_completed = done->load();
                                        if (!job_completed) {
                                            spu_context* jc = g_uc3_policy_ctx_live.load();
                                            fprintf(stderr, "[job-guard] chaine 0x%08X"
                                                    " (hash %016llX, wid %u) TIMEOUT %ds"
                                                    " — abandonnee; pc=0x%05X status=0x%X\n",
                                                    v, (unsigned long long)h, rwid, tmo_s,
                                                    jc ? (jc->pc & SPU_LS_MASK) : 0,
                                                    jc ? jc->status : 0);
                                        }
                                    } else {
                                        uc3_run_policy_job(rpm, rsz,
                                                           spurs, ring.arg, jp, v,
                                                           ring.wid);
                                    }
                                    /* Flush the per-run trace file (init closes
                                     * the previous file; "" redirects to stderr). */
                                    if (getenv("UC3_POLICY_TRACE") != nullptr)
                                        spu_trace_init("");
                                    /* The standalone policy runner returns after
                                     * the content job, before the SPURS kernel
                                     * epilogue publishes each active worker's
                                     * completion watermark. This is true for a
                                     * normal return as well as the timeout
                                     * fallback. Retire cursors only when the
                                     * command ring is already fully drained. */
                                    uint32_t completed_c0 = vm_read32(ring.arg + 0x40);
                                    uint16_t completed_produced =
                                        (uint16_t)(completed_c0 >> 16);
                                    uint16_t completed_consumed =
                                        (uint16_t)completed_c0;
                                    if (completed_produced == completed_consumed) {
                                        bool changed = false;
                                        for (uint32_t off = 0x42; off <= 0x4E;
                                             off += 2) {
                                            uint16_t cursor = vm_read16(ring.arg + off);
                                            if (cursor != 0xFFFFu &&
                                                cursor < completed_consumed) {
                                                vm_write16(ring.arg + off,
                                                           completed_consumed);
                                                changed = true;
                                            }
                                        }
                                        if (changed)
                                            fprintf(stderr,
                                                    "[job-watermark] wid=%u cursor=%u ring=0x%08X completed=%d\n",
                                                    ring.wid, completed_consumed,
                                                    ring.arg,
                                                    job_completed ? 1 : 0);
                                    }
                                    if (!job_completed) {
                                        continue; /* no watcher on an abandoned job */
                                    }
                                    /* The policy runner returns at a SPURS kernel
                                     * boundary after servicing one ready record.
                                     * D51D60 expects the expanded command cursor,
                                     * not ring+0x40's raw producer. Validate this
                                     * HLE bookkeeping independently before making
                                     * it part of the default executor. */
                                    if (getenv("UC3_WKL_PROGRESS_HLE"))
                                        uc3_advance_workload_completion(
                                            ring.arg, ring.wid, 1u,
                                            "policy-return");
                                    /* [UC3_RL_COMPLETE] EXPERIENCE: publier la
                                     * completion du job de contenu dans la
                                     * ready-list du kernel SPURS. Notre job A
                                     * REELLEMENT tourne (dispatch+interp jusqu'au
                                     * stop) mais yield au niveau scheduler avant
                                     * le DMA d'ecriture ready-list. On reproduit
                                     * cet effet observe: mettre le slot ready-list
                                     * du workload dans l'etat "draine" de wid0 qui
                                     * PASSE (slot[prod] a zero => D51D60 saute le
                                     * walk de sous-liste). Meme classe que
                                     * l'epilogue/watermark deja acceptes (regle #6:
                                     * completion de travail reellement execute). */
                                    if (getenv("UC3_RL_COMPLETE")) {
                                        /* index 0 de la base du ring (ring.arg) —
                                         * la ready-list du workload est a
                                         * ring.arg+0x30 (cf. sonde [wkl] qui lit
                                         * le slot bloque a cet offset). */
                                        uint32_t rlp = vm_read32(ring.arg + 0x30u);
                                        uint16_t rprod = vm_read16(ring.arg + 0x40u);
                                        if (rlp >= 0x10000u && rlp < 0x40000000u) {
                                            uint32_t rslot = rlp + (uint32_t)rprod*8u;
                                            uint32_t before0 = vm_read32(rslot);
                                            /* zero le slot[prod] + sa sous-liste
                                             * (jusqu'au terminateur, borne 16). */
                                            vm_write32(rslot, 0);
                                            vm_write32(rslot + 4u, 0);
                                            for (uint32_t k = 0; k < 16u; ++k) {
                                                uint32_t se = rslot + 0xAu + k*8u;
                                                if (vm_read16(se) == 0) break;
                                                vm_write16(se, 0);
                                            }
                                            static int _rc = 0;
                                            if (_rc < 24 && before0) { _rc++;
                                                fprintf(stderr, "[rl-complete] wid=%u ring=0x%08X slot=0x%08X drained (was 0x%08X)\n",
                                                        ring.wid, ring.arg, rslot, before0); }
                                        }
                                    }
                                    /* The lifted wid 0 content job currently
                                     * exits through an unresolved LS branch,
                                     * before publishing the descriptor's
                                     * completion counter. Recover the active
                                     * descriptor through the same TOC globals
                                     * used by the submit path, then reproduce
                                     * only that observed completion side effect. */
                                    /* Mode 2 (UC3_FRAME_JOB_COMPLETE_HLE=2):
                                     * generaliser a TOUS les wids — le main
                                     * traverse les syncs de frame workload par
                                     * workload (wid0 franchi -> mur wid1...).
                                     * Pour wid!=0 le descripteur actif est le
                                     * job desc du ring (jp), avec les memes
                                     * garde-fous payload/completion. */
                                    const char* fjc =
                                        getenv("UC3_FRAME_JOB_COMPLETE_HLE");
                                    if (fjc != nullptr &&
                                        (ring.wid == 0u || atoi(fjc) >= 2)) {
                                        uint32_t submit_globals =
                                            vm_read32(g_canonical_toc - 0x7508u);
                                        uint32_t submit_desc = 0u;
                                        if (ring.wid == 0u) {
                                            submit_desc =
                                                submit_globals >= 0x10000u &&
                                                submit_globals < 0x40000000u
                                                    ? vm_read32(submit_globals - 0x7FE0u)
                                                    : 0u;
                                        } else {
                                            submit_desc = jp; /* descripteur du ring courant */
                                            static int _fjs = 0;
                                            if (_fjs < 8) { _fjs++;
                                                fprintf(stderr, "[frame-job2] wid=%u jp=0x%08X +0x10=0x%08X +0x14=0x%08X (gardes?)\n",
                                                        ring.wid, jp, vm_read32(jp + 0x10u), vm_read32(jp + 0x14u)); }
                                        }
                                        if (submit_desc >= 0x10000u &&
                                            submit_desc < 0x40000000u) {
                                            uint32_t payload =
                                                vm_read32(submit_desc + 0x10u);
                                            uint32_t completion =
                                                vm_read32(submit_desc + 0x14u);
                                            if (payload >= 0x10000u &&
                                                payload < 0x40000000u &&
                                                completion >= 0x10000u &&
                                                completion < 0x40000000u) {
                                                uint32_t pending =
                                                    vm_read32(completion);
                                                if (pending != 0u) {
                                                    vm_write32(completion,
                                                               pending - 1u);
                                                    fprintf(stderr,
                                                            "[frame-job-complete] wid=%u desc=0x%08X payload=0x%08X counter=0x%08X %u->%u\n",
                                                            ring.wid, submit_desc, payload,
                                                            completion, pending,
                                                            pending - 1u);
                                                }
                                            }
                                        }
                                    }
                                    /* The standalone policy runner returns after
                                     * one content job, before Job Manager can
                                     * consume a following SYNC command. SYNC is
                                     * complete here by definition: all earlier
                                     * work in this dispatch has returned. */
                                    if (getenv("UC3_JOB_SYNC_HLE") != nullptr) {
                                        uint32_t sub0 = vm_read32(ring.arg + 0x30);
                                        uint32_t c0 = vm_read32(ring.arg + 0x40);
                                        uint32_t produced = c0 >> 16;
                                        uint32_t consumed = c0 & 0xFFFFu;
                                        if (sub0 >= 0x10000u && consumed < produced) {
                                            uint32_t cmd = sub0 + (consumed % 16u) * 8u;
                                            uint32_t opcode = vm_read32(cmd);
                                            if (opcode == 2u) { /* CELL_SPURS_JOB_OPCODE_SYNC */
                                                vm_write32(ring.arg + 0x40,
                                                           (produced << 16) |
                                                           ((consumed + 1u) & 0xFFFFu));
                                                fprintf(stderr,
                                                        "[job-sync] ring 0x%08X "
                                                        "consumed SYNC[%u] arg=0x%08X "
                                                        "c0=0x%08X->0x%08X\n",
                                                        ring.arg, consumed,
                                                        vm_read32(cmd + 4), c0,
                                                        (produced << 16) |
                                                        ((consumed + 1u) & 0xFFFFu));
                                            }
                                        }
                                    }
                                    /* Arm the post-job watcher on the structures
                                     * the PPU could poll for completion. */
                                    watch_words.clear();
                                    auto add_region = [&](uint32_t base, uint32_t size) {
                                        for (uint32_t wo = 0; wo < size; wo += 4)
                                            watch_words.emplace_back(
                                                base + wo, vm_read32(base + wo));
                                    };
                                    add_region(v, 0x100);        /* jobchain header */
                                    add_region(jp, 0x100);       /* job descriptor  */
                                    add_region(ring.arg, 0x100); /* ring control    */
                                    if (spurs) {
                                        add_region(spurs, 0x80);        /* wkl state */
                                        add_region(spurs + 0xB00, 0x40);/* wklInfo   */
                                    }
                                    watch_ticks = 250; /* ~5 s at 20 ms */
                                    watch_lines = 0;
                                    /* Consommation continue : tant que la
                                     * fenetre sub0 de CET anneau garde des
                                     * items non consommes ET que ce dispatch
                                     * a change c0 (progres), re-forcer un
                                     * dispatch au prochain tick. Sans ca, un
                                     * seul item est consomme par reecriture
                                     * du jeu (ringstat gd6 : c0=0x00020001
                                     * fige -> drain bloque -> famine). */
                                    {
                                        uint32_t c0_now =
                                            vm_read32(ring.arg + 0x40);
                                        if ((c0_now >> 16) != (c0_now & 0xFFFFu)
                                            && c0_now != ring.last_c0_disp) {
                                            ring.force = true;
                                            fprintf(stderr, "[requeue] ring "
                                                    "0x%08X c0=0x%08X -> "
                                                    "re-dispatch\n",
                                                    ring.arg, c0_now);
                                        }
                                        ring.last_c0_disp = c0_now;
                                    }
                                }
                                static std::atomic<bool> s_edge_ran{false};
                                if (getenv("UC3_EDGE_JOB") != nullptr &&
                                    !s_edge_ran.exchange(true)) {
                                    fprintf(stderr, "      >>> Edge job binary found "
                                            "@0x%08X, executing\n", v);
                                    uc3_run_edge_job(v);
                                }
                            }
                        }
                        } /* for si (sub-queues) */
                    }
                    /* Autonomous asset-queue drain (gated UC3_WKL2_PROMOTE).
                     * Layout proven by the pumped-state dump: sub counters are
                     * (produced<<16)|consumed at ring+0x40 (sub0) / +0x50 (sub1);
                     * the POLICY advances sub0's consumed halfword itself; sub1
                     * exposes a window of (0x00700001, descEA) pairs. Drain =
                     * when sub0 is fully consumed and sub1 has unpromoted items,
                     * copy the next batch of pairs into sub0, reset c0 to
                     * (batch<<16)|0, and PROPAGATE the consumption into c1's low
                     * halfword (the completion signal the game's producer polls).
                     * NOTE: writing c1 races the game's producer updating its
                     * high halfword; acceptable for the gated experiment. */
                    if (getenv("UC3_WKL2_PROMOTE") != nullptr) {
                        for (auto& ring : rings) {
                            uint32_t s0 = vm_read32(ring.arg + 0x30);
                            uint32_t s1 = vm_read32(ring.arg + 0x34);
                            if (s0 < 0x10000 || s1 < 0x10000 || s0 == s1)
                                continue;
                            uint32_t c0 = vm_read32(ring.arg + 0x40);
                            uint32_t c1 = vm_read32(ring.arg + 0x50);
                            uint32_t prod0 = c0 >> 16, cons0 = c0 & 0xFFFFu;
                            uint32_t prod1 = c1 >> 16;
                            if (prod0 != cons0)
                                continue;
                            /* sub1 ownership returns to the producer only after
                             * the corresponding promoted sub0 jobs completed. */
                            uint32_t cons1 = c1 & 0xFFFFu;
                            if (cons1 != ring.promoted) {
                                vm_write32(ring.arg + 0x50,
                                           (prod1 << 16) |
                                           (ring.promoted & 0xFFFFu));
                            }
                            if (prod1 == 0 || ring.promoted >= prod1)
                                continue;
                            uint32_t batch = prod1 - ring.promoted;
                            if (batch > 8) batch = 8;
                            for (uint32_t k = 0; k < batch; ++k) {
                                uint32_t src = s1 +
                                    ((ring.promoted + k) % 16u) * 8u;
                                uint32_t dst = s0 + k * 8u;
                                vm_write32(dst, vm_read32(src));
                                vm_write32(dst + 4, vm_read32(src + 4));
                            }
                            if (batch < 8) {
                                vm_write32(s0 + batch * 8u, 0);
                                vm_write32(s0 + batch * 8u + 4, 0);
                            }
                            vm_write32(ring.arg + 0x40, batch << 16);
                            /* consumption signal for the items already drained */
                            ring.promoted += batch;
                            ring.force = true;
                            fprintf(stderr, "[drain] ring 0x%08X: promoted %u..%u "
                                    "of %u (c0=0x%08X)\n",
                                    ring.arg, ring.promoted - batch,
                                    ring.promoted - 1, prod1, batch << 16);
                        }
                    }
                    /* Etat periodique des compteurs d'anneaux (UC3_RING_STAT,
                     * ~5 s) : c0=sub0 (prod<<16|cons), c1=sub1, promoted=drain.
                     * Sert a departager, au stall post-batch-2 (prog=509808),
                     * producteur jeu arrete (prod1 fige) vs executeur/drain. */
                    if (getenv("UC3_RING_STAT") != nullptr) {
                        static int s_stat_tick = 0;
                        /* Seuil bas: la boucle est bloquee de longues periodes
                         * dans les dispatches (interp/retries), 250 ticks
                         * n'arrivent jamais en pratique (nl5.log: 0 print). */
                        if (++s_stat_tick >= 50) {
                            s_stat_tick = 0;
                            for (auto& ring : rings) {
                                uint32_t c0 = vm_read32(ring.arg + 0x40);
                                uint32_t c1 = vm_read32(ring.arg + 0x50);
                                if (c0 || c1)
                                    fprintf(stderr, "[ringstat] wid=%u arg=0x%08X "
                                            "c0=0x%08X c1=0x%08X "
                                            "w0=%08X/%08X/%08X w1=%08X/%08X/%08X "
                                            "promoted=%u\n",
                                            ring.wid, ring.arg, c0, c1,
                                            vm_read32(ring.arg + 0x44),
                                            vm_read32(ring.arg + 0x48),
                                            vm_read32(ring.arg + 0x4C),
                                            vm_read32(ring.arg + 0x54),
                                            vm_read32(ring.arg + 0x58),
                                            vm_read32(ring.arg + 0x5C),
                                            ring.promoted);
                            }
                            /* Cellule de completion presumee du jobchain
                             * wkl[2] (commandes +0x80/+0x90 -> 0x012FC788 =
                             * struct SPURS 0x012FC730 + 0x58). Suivre ses
                             * octets pour identifier le protocole d'ecriture
                             * attendu par la policy (item 2 idle sinon). */
                            fprintf(stderr, "[cell788] %08X %08X %08X %08X\n",
                                    vm_read32(0x012FC780u), vm_read32(0x012FC784u),
                                    vm_read32(0x012FC788u), vm_read32(0x012FC78Cu));
                        }
                    }
                    /* One-shot deep dump of the asset queue at PUMPED state: when
                     * a ring's sub1 produce counter (ring+0x50 high halfword)
                     * jumps past 8, dump the sub windows and probe the descriptor
                     * array stride so the continuous-drain semantics can be built
                     * on the real layout. */
                    static bool s_pumped_dumped = false;
                    if (!s_pumped_dumped) {
                        for (auto& ring : rings) {
                            uint32_t c1 = vm_read32(ring.arg + 0x50);
                            if ((c1 >> 16) < 8) continue;
                            s_pumped_dumped = true;
                            uint32_t s0 = vm_read32(ring.arg + 0x30);
                            uint32_t s1 = vm_read32(ring.arg + 0x34);
                            fprintf(stderr, "[pumped] ring 0x%08X c0=0x%08X "
                                    "c1=0x%08X sub0=0x%08X sub1=0x%08X\n",
                                    ring.arg, vm_read32(ring.arg + 0x40), c1,
                                    s0, s1);
                            for (uint32_t s : {s0, s1}) {
                                if (s < 0x10000) continue;
                                fprintf(stderr, "  sub 0x%08X:\n", s);
                                for (uint32_t o = 0; o < 0x80; o += 16) {
                                    fprintf(stderr, "    +0x%02X:", o);
                                    for (int b = 0; b < 16; b++)
                                        fprintf(stderr, " %02X",
                                                vm_read8(s + o + b));
                                    fprintf(stderr, "\n");
                                }
                            }
                            /* Probe descriptor stride: known descs 0x20E6C400,
                             * 0x20E6CD00 (delta 0x900). Check headers there. */
                            for (int di = 0; di < 6; ++di) {
                                uint32_t dea = 0x20E6C400u + (uint32_t)di * 0x900u;
                                fprintf(stderr, "  desc[%d] 0x%08X:", di, dea);
                                for (int b = 0; b < 16; b++)
                                    fprintf(stderr, " %02X", vm_read8(dea + b));
                                fprintf(stderr, "\n");
                            }
                            break;
                        }
                    }
                    /* Post-job diff pass: log every watched word the PPU changed. */
                    if (watch_ticks > 0) {
                        --watch_ticks;
                        for (auto& wp : watch_words) {
                            uint32_t nv = vm_read32(wp.first);
                            if (nv != wp.second && watch_lines < 96) {
                                ++watch_lines;
                                fprintf(stderr,
                                        "[postjob-watch] 0x%08X: %08X -> %08X\n",
                                        wp.first, wp.second, nv);
                                wp.second = nv;
                            }
                        }
                        if (watch_ticks == 0)
                            fprintf(stderr, "[postjob-watch] window closed "
                                            "(%d change lines)\n", watch_lines);
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
static void br_strchr (ppu_context* ctx){ if(!A0){RETP(0);return;}
    /* [UC3_STRCHR_TRACE] le thread Init early-stall passe ~80% de ses
     * échantillons DANS strchr (boucle de parse guest chaude/infinie).
     * Tous les 200k appels: appelant (lr), EA, char cherché, aperçu. */
    if (getenv("UC3_STRCHR_TRACE")) {
        static std::atomic<uint64_t> s_n{0};
        uint64_t n = ++s_n;
        if ((n % 200000ull) == 1ull) {
            char prev[49] = {0};
            for (int i = 0; i < 48; i++) { char c = (char)vm_read8(A0 + i);
                prev[i] = (c >= 32 && c < 127) ? c : (c ? '.' : 0); if (!c) break; }
            fprintf(stderr, "[strchr-trace] #%lluk lr=0x%08X ea=0x%08X ch=0x%02X '%s'\n",
                    (unsigned long long)(n/1000), (uint32_t)ctx->lr, (uint32_t)A0,
                    (unsigned)A1 & 0xFF, prev);
        }
    }
    char* r=strchr(Gs(A0),(int)A1); RETP(r?(uint32_t)((uint8_t*)r-vm_base):0); }
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

static void br_cellVideoOutGetDeviceInfo(ppu_context* ctx) {
    /* RPCS3 ground truth: main thread calls this between the movies and the
     * menu (t=39.4s, LR=0x00728500). It was an unresolved NID (0x1E930EEF)
     * returning 0 with an UNFILLED struct — the frontend's video-mode probe
     * saw an empty device. The lib fills a guest-BE struct; copy it out. */
    if (!A2) { RET(CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER); return; }
    CellVideoOutDeviceInfo dev_info{};
    int32_t rc = cellVideoOutGetDeviceInfo(A0, A1, &dev_info);
    if (rc == CELL_OK) memcpy(Gp(A2), &dev_info, sizeof(dev_info));
    RET(rc);
}

/* cellRtc bridges — the lib fills HOST-endian structs; marshal to guest BE.
 * Both were unresolved NIDs called during init (silent 0-return left the game
 * with a zero clock). Phase-8 rule: implement properly. */
static void br_cellRtcGetCurrentClockLocalTime(ppu_context* ctx) {
    if (!A0) { RET(0x80010002 /* CELL_EINVAL */); return; }
    CellRtcDateTime dt{};
    int32_t rc = cellRtcGetCurrentClockLocalTime(&dt);
    if (rc == CELL_OK) {
        vm_write16(A0 + 0x0, dt.year);
        vm_write16(A0 + 0x2, dt.month);
        vm_write16(A0 + 0x4, dt.day);
        vm_write16(A0 + 0x6, dt.hour);
        vm_write16(A0 + 0x8, dt.minute);
        vm_write16(A0 + 0xA, dt.second);
        vm_write32(A0 + 0xC, dt.microsecond);
    }
    RET(rc);
}

static void br_cellRtcGetTime_t(ppu_context* ctx) {
    if (!A0 || !A1) { RET(0x80010002 /* CELL_EINVAL */); return; }
    CellRtcTick tick;
    tick.tick = vm_read64(A0);
    s64 t = 0;
    int32_t rc = cellRtcGetTime_t(&tick, &t);
    if (rc == CELL_OK) vm_write64(A1, (uint64_t)t);
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

/* --- CUSTOM GCM command buffer drain (gate final du menu, 2026-07-08) ---------
 * Le jeu rend le menu dans un command buffer AUXILIAIRE (CellGcmContextData :
 * begin=+0x00, end=+0x04, current=+0x08) et NON le contexte defaut g_gcm.control
 * que notre draineur suit. Sans drainer ce buffer custom, les 20 draws + textures
 * du menu ne sont jamais routes vers D3D12 -> aucun flip. rsx_process_method appelle
 * D3D12 (flush_index_draw), donc le drain DOIT s'executer sur le thread draineur
 * (owner D3D12). Le handler d'overflow (thread jeu, ppu_loader.cpp func_0074A438)
 * poste le contexte via uc3_request_custom_gcm_drain() et ATTEND que le draineur
 * l'ait traite AVANT que func_0074A438 ne rembobine current=begin (sinon commandes
 * perdues). Preuve: STATUS_AND_ROADMAP.md [gcm-overflow] + workflow doc-mining. */
static std::atomic<uint32_t> g_custom_gcm_pending{0};
uint32_t g_adrain_hist[0x800] = {0};   /* histo méthodes ring custom (UC3_ADRAIN_HIST) */

static void uc3_drain_custom_gcm(uint32_t context)
{
    if (!context) return;
    uint32_t begin   = vm_read32(context + 0x00);
    uint32_t end     = vm_read32(context + 0x04);
    uint32_t current = vm_read32(context + 0x08);
    if (begin < 0x10000u || end <= begin || current <= begin || current > end) return;
    const bool route = g_rsx_ready.load();
    uint32_t off = begin, guard = 0, nmeth = 0;
    while (off < current && guard++ < 400000u) {
        uint32_t hdr = vm_read32(off); off += 4;
        if (hdr == 0) continue;
        uint32_t type = hdr >> 29;
        if (type == 1) {                    /* jump within the aux ring */
            uint32_t t = hdr & 0x1FFFFFFC;
            if (t >= begin && t < end) off = t;
            else if (begin + t >= begin && begin + t < end) off = begin + t;
            else break;
            continue;
        }
        if ((hdr & 3) == 2) continue;       /* call/ret */
        uint32_t count  = (hdr >> 18) & 0x7FF;
        uint32_t method = hdr & 0x1FFC;
        bool noinc = (type == 2);
        /* UC3_GCM_HIST: tally the draw-relevant methods present in the aux ring
         * so we can see whether the fade/menu actually emits draw commands
         * (SET_BEGIN_END 0x1808, DRAW_ARRAYS 0x1814, DRAW_INDEX 0x1824,
         * inline SET_VERTEX_DATA 0x1c00-0x1ffc). */
        if (getenv("UC3_GCM_HIST")) {
            static unsigned long long h_begin=0,h_draw=0,h_inlv=0,h_idx=0,h_other=0,h_tot=0;
            h_tot++;
            if (method==0x1808) h_begin++;
            else if (method==0x1814||method==0x1824) h_draw++;
            else if (method>=0x1c00 && method<=0x1ffc) h_inlv++;
            else if (method==0x1800||method==0x1804) h_idx++;
            else h_other++;
            if ((h_tot % 4000ull)==0)
                fprintf(stderr,"[gcm-hist] tot=%llu begin_end=%llu draw=%llu inline_vtx=%llu idxfmt=%llu other=%llu\n",
                        h_tot,h_begin,h_draw,h_inlv,h_idx,h_other);
        }
        if (route)
            for (uint32_t i = 0; i < count; i++)
                rsx_process_method(&g_rsx_state, noinc ? method : method + i*4,
                                   vm_read32(off + i*4));
        off += count * 4;
        nmeth++;
    }
    static int s_log = 0;
    if (getenv("UC3_CUSTOM_GCM") && s_log < 30) { s_log++;
        fprintf(stderr, "[custom-gcm] ctx=0x%08X ring[0x%08X..0x%08X) methods=%u route=%d\n",
                context, begin, current, nmeth, (int)route);
    }
}

/* Called from the game thread (func_0074A438 overflow callback). Post the aux
 * context to the RSX/drainer thread and block briefly until it is drained. */
extern "C" void uc3_request_custom_gcm_drain(uint32_t context)
{
    if (!context) return;
    g_custom_gcm_pending.store(context);
    for (int i = 0; i < 300 && g_custom_gcm_pending.load(); i++)  /* ~30ms cap */
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    g_custom_gcm_pending.store(0);   /* timeout safety: never wedge the game thread */
}

struct Uc3RrcFifoDraw {
    uint32_t vertex_count = 0;
    float mvp[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    std::vector<uint8_t> vertices_be;
    std::vector<uint8_t> colors;
    std::vector<uint8_t> uvs_be;
    std::vector<uint8_t> attr2_be;
    std::vector<uint8_t> attr3_be;
    std::vector<uint8_t> attr4_be;
    std::vector<uint8_t> indices_be;
    uint32_t shader_index = UINT32_MAX;
    uint32_t texture_set_index = 0;
    uint32_t target_surface = UINT32_MAX;
    uint32_t target_width = 1280;
    uint32_t target_height = 720;
    uint32_t color_output = 0;
    std::vector<float> transform_constants;
};

struct Uc3RrcFifoTexture {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    std::vector<uint8_t> data;
};

struct Uc3RrcFifoShader {
    uint32_t input_mask = 0;
    uint32_t output_mask = 0;
    uint32_t control = 0;
    std::vector<uint8_t> vertex_program;
    std::vector<uint8_t> fragment_program;
};

struct Uc3RrcFifoTextureSet {
    uint32_t textures[16] = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
    };
};

struct Uc3RrcFifoSurface {
    uint32_t address = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t flags = 0;
};

static uint32_t rrc_read_le32(FILE* stream, bool& ok) {
    uint8_t bytes[4];
    if (fread(bytes, 1, sizeof(bytes), stream) != sizeof(bytes)) {
        ok = false;
        return 0;
    }
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool rrc_load_fifo_replay(const char* path,
                                 std::vector<Uc3RrcFifoDraw>& draws,
                                 std::vector<Uc3RrcFifoTexture>& textures,
                                 std::vector<Uc3RrcFifoShader>& shaders,
                                 std::vector<Uc3RrcFifoTextureSet>& texture_sets,
                                 std::vector<Uc3RrcFifoSurface>& surfaces) {
    FILE* stream = fopen(path, "rb");
    if (!stream) {
        fprintf(stderr, "[rrc-fifo] cannot open %s\n", path);
        return false;
    }
    uint8_t magic[8];
    bool ok = fread(magic, 1, sizeof(magic), stream) == sizeof(magic) &&
              memcmp(magic, "UC3RRCF\0", sizeof(magic)) == 0;
    uint32_t version = ok ? rrc_read_le32(stream, ok) : 0;
    uint32_t draw_count = ok ? rrc_read_le32(stream, ok) : 0;
    if (!ok || version < 1 || version > 8 || draw_count > 4096) {
        fprintf(stderr, "[rrc-fifo] invalid header in %s\n", path);
        fclose(stream);
        return false;
    }

    draws.clear();
    textures.clear();
    shaders.clear();
    texture_sets.clear();
    surfaces.clear();
    if (version >= 4) {
        uint32_t texture_count = rrc_read_le32(stream, ok);
        if (!ok || texture_count > 256) {
            fclose(stream);
            return false;
        }
        textures.reserve(texture_count);
        for (uint32_t i = 0; i < texture_count && ok; ++i) {
            Uc3RrcFifoTexture texture;
            texture.width = rrc_read_le32(stream, ok);
            texture.height = rrc_read_le32(stream, ok);
            texture.format = rrc_read_le32(stream, ok);
            uint32_t size = rrc_read_le32(stream, ok);
            if (!ok || texture.width < 4 || texture.height < 4 ||
                size == 0 || size > 64u * 1024u * 1024u) {
                ok = false;
                break;
            }
            texture.data.resize(size);
            ok = fread(texture.data.data(), 1, size, stream) == size;
            textures.push_back(std::move(texture));
        }
    }
    if (version >= 5 && ok) {
        uint32_t shader_count = rrc_read_le32(stream, ok);
        if (!ok || shader_count > 256) {
            fclose(stream);
            return false;
        }
        shaders.reserve(shader_count);
        for (uint32_t i = 0; i < shader_count && ok; ++i) {
            uint32_t vertex_size = rrc_read_le32(stream, ok);
            uint32_t fragment_size = rrc_read_le32(stream, ok);
            Uc3RrcFifoShader shader;
            shader.input_mask = rrc_read_le32(stream, ok);
            shader.output_mask = rrc_read_le32(stream, ok);
            shader.control = rrc_read_le32(stream, ok);
            if (!ok || vertex_size == 0 || vertex_size > 512u * 16u ||
                vertex_size % 16 != 0 || fragment_size == 0 ||
                fragment_size > 64u * 1024u) {
                ok = false;
                break;
            }
            shader.vertex_program.resize(vertex_size);
            shader.fragment_program.resize(fragment_size);
            ok = fread(shader.vertex_program.data(), 1, vertex_size, stream) == vertex_size &&
                 fread(shader.fragment_program.data(), 1, fragment_size, stream) == fragment_size;
            shaders.push_back(std::move(shader));
        }
    }
    if (version >= 7 && ok) {
        uint32_t set_count = rrc_read_le32(stream, ok);
        if (!ok || set_count == 0 || set_count > 64) {
            fclose(stream);
            return false;
        }
        texture_sets.reserve(set_count);
        for (uint32_t set = 0; set < set_count && ok; ++set) {
            Uc3RrcFifoTextureSet texture_set;
            for (uint32_t unit = 0; unit < 16; ++unit) {
                texture_set.textures[unit] = rrc_read_le32(stream, ok);
                if (ok && texture_set.textures[unit] != UINT32_MAX &&
                    !(texture_set.textures[unit] & 0x80000000u) &&
                    texture_set.textures[unit] >= textures.size()) {
                    ok = false;
                }
            }
            texture_sets.push_back(texture_set);
        }
    }
    if (version >= 8 && ok) {
        uint32_t surface_count = rrc_read_le32(stream, ok);
        if (!ok || surface_count == 0 || surface_count > 64) {
            fclose(stream);
            return false;
        }
        surfaces.reserve(surface_count);
        for (uint32_t i = 0; i < surface_count && ok; ++i) {
            Uc3RrcFifoSurface surface;
            surface.address = rrc_read_le32(stream, ok);
            surface.width = rrc_read_le32(stream, ok);
            surface.height = rrc_read_le32(stream, ok);
            surface.flags = rrc_read_le32(stream, ok);
            if (!ok || surface.width == 0 || surface.height == 0 ||
                surface.width > 4096 || surface.height > 4096) {
                ok = false;
                break;
            }
            surfaces.push_back(surface);
        }
        for (const Uc3RrcFifoTextureSet& texture_set : texture_sets) {
            for (uint32_t unit = 0; unit < 16; ++unit) {
                uint32_t reference = texture_set.textures[unit];
                if (reference != UINT32_MAX && (reference & 0x80000000u) &&
                    (reference & 0x7FFFFFFFu) >= surfaces.size()) {
                    ok = false;
                }
            }
        }
    }
    draws.reserve(draw_count);
    uint64_t total_vertices = 0;
    for (uint32_t i = 0; i < draw_count && ok; ++i) {
        Uc3RrcFifoDraw draw;
        draw.vertex_count = rrc_read_le32(stream, ok);
        if (!ok || draw.vertex_count == 0 || draw.vertex_count > 0xFFFFu ||
            draw.vertex_count % 3 != 0) {
            ok = false;
            break;
        }
        if (version >= 2)
            ok = fread(draw.mvp, 1, sizeof(draw.mvp), stream) == sizeof(draw.mvp);
        uint32_t flags = version >= 3 ? rrc_read_le32(stream, ok) : 0;
        if (!ok)
            break;
        if (version >= 5 && (flags & 4)) {
            draw.shader_index = rrc_read_le32(stream, ok);
            if (!ok || draw.shader_index >= shaders.size()) {
                ok = false;
                break;
            }
            draw.transform_constants.resize(512u * 4u);
            ok = fread(draw.transform_constants.data(), sizeof(float),
                       draw.transform_constants.size(), stream) ==
                 draw.transform_constants.size();
            if (!ok)
                break;
        }
        if (version >= 7 && (flags & 64)) {
            draw.texture_set_index = rrc_read_le32(stream, ok);
            if (!ok || draw.texture_set_index >= texture_sets.size()) {
                ok = false;
                break;
            }
        }
        if (version >= 8) {
            draw.target_surface = rrc_read_le32(stream, ok);
            draw.target_width = rrc_read_le32(stream, ok);
            draw.target_height = rrc_read_le32(stream, ok);
            draw.color_output = rrc_read_le32(stream, ok);
            if (!ok || draw.target_surface >= surfaces.size() ||
                draw.target_width == 0 || draw.target_height == 0 ||
                draw.target_width > 4096 || draw.target_height > 4096 ||
                draw.color_output > 3) {
                ok = false;
                break;
            }
        }
        draw.vertices_be.resize((size_t)draw.vertex_count * 12);
        if (flags & 1)
            draw.colors.resize((size_t)draw.vertex_count * 4);
        if (flags & 2)
            draw.uvs_be.resize((size_t)draw.vertex_count * 8);
        if (version >= 6 && (flags & 8))
            draw.attr2_be.resize((size_t)draw.vertex_count * 16);
        if (version >= 6 && (flags & 16))
            draw.attr3_be.resize((size_t)draw.vertex_count * 16);
        if (version >= 6 && (flags & 32))
            draw.attr4_be.resize((size_t)draw.vertex_count * 16);
        draw.indices_be.resize((size_t)draw.vertex_count * 2);
        ok = fread(draw.vertices_be.data(), 1, draw.vertices_be.size(), stream) ==
                 draw.vertices_be.size();
        if (ok && !draw.colors.empty())
            ok = fread(draw.colors.data(), 1, draw.colors.size(), stream) == draw.colors.size();
        if (ok && !draw.uvs_be.empty())
            ok = fread(draw.uvs_be.data(), 1, draw.uvs_be.size(), stream) == draw.uvs_be.size();
        if (ok && !draw.attr2_be.empty())
            ok = fread(draw.attr2_be.data(), 1, draw.attr2_be.size(), stream) ==
                 draw.attr2_be.size();
        if (ok && !draw.attr3_be.empty())
            ok = fread(draw.attr3_be.data(), 1, draw.attr3_be.size(), stream) ==
                 draw.attr3_be.size();
        if (ok && !draw.attr4_be.empty())
            ok = fread(draw.attr4_be.data(), 1, draw.attr4_be.size(), stream) ==
                 draw.attr4_be.size();
        ok = ok &&
             fread(draw.indices_be.data(), 1, draw.indices_be.size(), stream) ==
                 draw.indices_be.size();
        total_vertices += draw.vertex_count;
        draws.push_back(std::move(draw));
    }
    fclose(stream);
    if (!ok || draws.size() != draw_count) {
        fprintf(stderr, "[rrc-fifo] truncated draw data in %s\n", path);
        draws.clear();
        textures.clear();
        shaders.clear();
        texture_sets.clear();
        surfaces.clear();
        return false;
    }
    fprintf(stderr, "[rrc-fifo] loaded %zu draws / %llu vertices / %zu textures / "
                    "%zu shaders / %zu texture sets / %zu surfaces from %s\n",
            draws.size(), (unsigned long long)total_vertices, textures.size(),
            shaders.size(), texture_sets.size(), surfaces.size(), path);
    return true;
}

static void rrc_submit_fifo_frame(const std::vector<Uc3RrcFifoDraw>& draws,
                                  const std::vector<Uc3RrcFifoShader>& shaders) {
    constexpr uint32_t vertex_ea = 0x18000000u;
    constexpr uint32_t index_ea = 0x19000000u;
    constexpr uint32_t color_ea = 0x1A000000u;
    constexpr uint32_t uv_ea = 0x1B000000u;
    constexpr uint32_t shader_ea = 0x1C000000u;
    constexpr uint32_t attr2_ea = 0x1D000000u;
    constexpr uint32_t attr3_ea = 0x1E000000u;
    constexpr uint32_t attr4_ea = 0x1F000000u;
    const bool use_captured_shaders = getenv("UC3_RRC_USE_SHADERS") != nullptr;
    rsx_state state;
    rsx_state_init(&state);
    rsx_d3d12_begin_debug_frame();
    uint32_t last_shader = UINT32_MAX;
    uint32_t last_color_output = UINT32_MAX;

    for (const Uc3RrcFifoDraw& draw : draws) {
        rsx_d3d12_set_debug_texture_set(draw.texture_set_index);
        rsx_d3d12_set_debug_render_target(
            draw.target_surface, draw.target_width, draw.target_height);
        rsx_d3d12_set_debug_color_output(draw.color_output);
        memcpy(vm_base + vertex_ea, draw.vertices_be.data(), draw.vertices_be.size());
        memcpy(vm_base + index_ea, draw.indices_be.data(), draw.indices_be.size());
        if (!draw.colors.empty())
            memcpy(vm_base + color_ea, draw.colors.data(), draw.colors.size());
        if (!draw.uvs_be.empty())
            memcpy(vm_base + uv_ea, draw.uvs_be.data(), draw.uvs_be.size());
        if (!draw.attr2_be.empty())
            memcpy(vm_base + attr2_ea, draw.attr2_be.data(), draw.attr2_be.size());
        if (!draw.attr3_be.empty())
            memcpy(vm_base + attr3_ea, draw.attr3_be.data(), draw.attr3_be.size());
        if (!draw.attr4_be.empty())
            memcpy(vm_base + attr4_ea, draw.attr4_be.data(), draw.attr4_be.size());
        if (draw.transform_constants.size() == 512u * 4u) {
            memcpy(state.vertex_constants, draw.transform_constants.data(),
                   sizeof(state.vertex_constants));
        } else {
            memcpy(state.vertex_constants, draw.mvp, sizeof(draw.mvp));
        }
        /* Perspective world draws need X un-mirroring. Keep orthographic UI
         * draws untouched so text and logos remain readable. */
        const bool perspective_draw =
            std::fabs(state.vertex_constants[3][3]) > 1.0f ||
            std::fabs(state.vertex_constants[3][2]) > 1.0e-6f;
        if (getenv("UC3_RRC_NOFLIPX") == nullptr && perspective_draw) {
            for (uint32_t lane = 0; lane < 4; ++lane)
                state.vertex_constants[0][lane] = -state.vertex_constants[0][lane];
        }
        if (draw.shader_index < shaders.size() &&
            (draw.shader_index != last_shader || draw.color_output != last_color_output)) {
            const Uc3RrcFifoShader& shader = shaders[draw.shader_index];
            memcpy(vm_base + shader_ea, shader.fragment_program.data(),
                   shader.fragment_program.size());
            rsx_process_method(&state, NV4097_SET_SHADER_PROGRAM, shader_ea | 2u);
            rsx_process_method(&state, NV4097_SET_SHADER_CONTROL, shader.control);
            rsx_process_method(&state, NV4097_SET_VERTEX_ATTRIB_INPUT_MASK,
                               shader.input_mask);
            rsx_process_method(&state, NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK,
                               shader.output_mask);
            uint32_t word_count = (uint32_t)shader.vertex_program.size() / 4u;
            for (uint32_t word = 0; word < word_count; ++word) {
                if ((word % 32u) == 0) {
                    rsx_process_method(&state, NV4097_SET_TRANSFORM_PROGRAM_LOAD,
                                       word / 4u);
                }
                uint32_t value;
                memcpy(&value, shader.vertex_program.data() + word * 4u, sizeof(value));
                rsx_process_method(&state,
                                   NV4097_SET_TRANSFORM_PROGRAM + (word % 32u) * 4u,
                                   value);
            }
            rsx_process_method(&state, NV4097_SET_TRANSFORM_PROGRAM_START, 0);
            last_shader = draw.shader_index;
            last_color_output = draw.color_output;
        }
        rsx_process_method(&state, NV4097_SET_VERTEX_DATA_ARRAY_FORMAT, 0x00000C32u);
        rsx_process_method(&state, NV4097_SET_VERTEX_DATA_ARRAY_OFFSET,
                           vertex_ea | 0x80000000u);
        rsx_process_method(&state, NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 1 * 4,
                           draw.uvs_be.empty() ? 0u : 0x00000822u);
        rsx_process_method(&state, NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 1 * 4,
                           draw.uvs_be.empty() ? 0u : (uv_ea | 0x80000000u));
        rsx_process_method(&state, NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 2 * 4,
                           draw.attr2_be.empty() ? 0u : 0x00001042u);
        rsx_process_method(&state, NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 2 * 4,
                           draw.attr2_be.empty() ? 0u : (attr2_ea | 0x80000000u));
        rsx_process_method(&state, NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4,
                           use_captured_shaders && !draw.attr3_be.empty()
                               ? 0x00001042u
                               : (draw.colors.empty() ? 0u : 0x00000444u));
        rsx_process_method(&state, NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4,
                           use_captured_shaders && !draw.attr3_be.empty()
                               ? (attr3_ea | 0x80000000u)
                               : (draw.colors.empty() ? 0u : (color_ea | 0x80000000u)));
        rsx_process_method(&state, NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * 4,
                           draw.attr4_be.empty() ? 0u : 0x00001042u);
        rsx_process_method(&state, NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * 4,
                           draw.attr4_be.empty() ? 0u : (attr4_ea | 0x80000000u));
        rsx_process_method(&state, NV4097_SET_INDEX_ARRAY_ADDRESS, index_ea);
        rsx_process_method(&state, NV4097_SET_INDEX_ARRAY_FORMAT, 0x11u);
        rsx_process_method(&state, NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
        for (uint32_t first = 0; first < draw.vertex_count;) {
            uint32_t count = draw.vertex_count - first;
            if (count > 256) count = 256;
            uint32_t packet = ((count - 1) << 24) | first;
            rsx_process_method(&state, NV4097_DRAW_INDEX_ARRAY, packet);
            first += count;
        }
        rsx_process_method(&state, NV4097_SET_BEGIN_END, 0);
    }
    rsx_d3d12_backend_present();
}

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
                    /* Étape 3 injection: bypass the idle game FIFO and draw geometry
                     * directly each frame. UC3_INJECT_GEOM=tri draws a test triangle
                     * to prove the geometry->screen path; UC3_INJECT_GEOM=<file.ply>
                     * loads and draws a real mesh (clip-space float3 verts). */
                    const char* inject = getenv("UC3_INJECT_GEOM");
                    std::vector<float> inj_verts; uint32_t inj_prim = 5;
                    const char* fifo_replay = getenv("UC3_RRC_FIFO");
                    std::vector<Uc3RrcFifoDraw> fifo_draws;
                    std::vector<Uc3RrcFifoTexture> fifo_textures;
                    std::vector<Uc3RrcFifoShader> fifo_shaders;
                    std::vector<Uc3RrcFifoTextureSet> fifo_texture_sets;
                    std::vector<Uc3RrcFifoSurface> fifo_surfaces;
                    if (fifo_replay && d3d12_ok &&
                        rrc_load_fifo_replay(
                            fifo_replay, fifo_draws, fifo_textures, fifo_shaders,
                            fifo_texture_sets, fifo_surfaces)) {
                        for (uint32_t surface = 0; surface < fifo_surfaces.size(); ++surface) {
                            const Uc3RrcFifoSurface& definition = fifo_surfaces[surface];
                            if (rsx_d3d12_define_debug_surface(
                                    surface, definition.width, definition.height,
                                    definition.flags) != 0) {
                                fprintf(stderr, "[rrc-fifo] surface %u creation failed\n",
                                        surface);
                            }
                        }
                        if (fifo_texture_sets.empty()) {
                            for (uint32_t unit = 0; unit < fifo_textures.size(); ++unit) {
                                const Uc3RrcFifoTexture& texture = fifo_textures[unit];
                                if (rsx_d3d12_set_debug_texture(
                                        unit, texture.data.data(),
                                        (uint32_t)texture.data.size(), texture.width,
                                        texture.height, texture.format) != 0) {
                                    fprintf(stderr, "[rrc-fifo] texture %u upload failed\n", unit);
                                }
                            }
                        } else {
                            for (uint32_t set = 0; set < fifo_texture_sets.size(); ++set) {
                                for (uint32_t unit = 0; unit < 16; ++unit) {
                                    uint32_t texture_index =
                                        fifo_texture_sets[set].textures[unit];
                                    if (texture_index == UINT32_MAX)
                                        continue;
                                    if (texture_index & 0x80000000u) {
                                        if (rsx_d3d12_set_debug_surface_for_set(
                                                set, unit,
                                                texture_index & 0x7FFFFFFFu) != 0) {
                                            fprintf(stderr,
                                                    "[rrc-fifo] surface set%u:t%u bind failed\n",
                                                    set, unit);
                                        }
                                        continue;
                                    }
                                    const Uc3RrcFifoTexture& texture =
                                        fifo_textures[texture_index];
                                    if (rsx_d3d12_set_debug_texture_for_set(
                                            set, unit, texture.data.data(),
                                            (uint32_t)texture.data.size(), texture.width,
                                            texture.height, texture.format) != 0) {
                                        fprintf(stderr,
                                                "[rrc-fifo] texture set%u:t%u upload failed\n",
                                                set, unit);
                                    }
                                }
                            }
                        }
                    }
                    if (inject && d3d12_ok) {
                        if (!strcmp(inject, "tri")) {
                            inj_verts = { 0.0f,0.6f,0.0f, -0.6f,-0.6f,0.0f, 0.6f,-0.6f,0.0f };
                        } else if (FILE* pf = fopen(inject, "r")) {
                            char line[256]; int hdr = 1;
                            while (fgets(line, sizeof(line), pf)) {
                                if (hdr) { if (!strncmp(line, "end_header", 10)) hdr = 0; continue; }
                                float x,y,z;
                                if (sscanf(line, "%f %f %f", &x,&y,&z) == 3) {
                                    inj_verts.push_back(x); inj_verts.push_back(y); inj_verts.push_back(z);
                                }
                            }
                            fclose(pf);
                            inj_prim = 5; /* triangle list (PLY = generated triangle soup) */
                            if (const char* pe = getenv("UC3_INJECT_PRIM"))
                                inj_prim = (uint32_t)strtoul(pe, nullptr, 10); /* 1=points */
                            /* Normalize the mesh to clip space so it fills the view. */
                            if (!inj_verts.empty()) {
                                float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f};
                                for (size_t i=0;i<inj_verts.size();i+=3)
                                    for (int k=0;k<3;k++){ float v=inj_verts[i+k];
                                        if(v<mn[k])mn[k]=v; if(v>mx[k])mx[k]=v; }
                                float c[3]={(mn[0]+mx[0])*0.5f,(mn[1]+mx[1])*0.5f,(mn[2]+mx[2])*0.5f};
                                float ext=0; for(int k=0;k<3;k++){float e=mx[k]-mn[k]; if(e>ext)ext=e;}
                                float s = ext>1e-6f ? 1.6f/ext : 1.0f;
                                for (size_t i=0;i<inj_verts.size();i+=3){
                                    inj_verts[i+0]=(inj_verts[i+0]-c[0])*s;
                                    inj_verts[i+1]=(inj_verts[i+1]-c[1])*s;
                                    inj_verts[i+2]=(inj_verts[i+2]-c[2])*s*0.25f;
                                }
                                fprintf(stderr,"[inject] bbox x[%.3f,%.3f] y[%.3f,%.3f] z[%.3f,%.3f] scale=%.2f\n",
                                        mn[0],mx[0],mn[1],mx[1],mn[2],mx[2],s);
                            }
                            fprintf(stderr, "[inject] loaded %zu verts from %s\n",
                                    inj_verts.size()/3, inject);
                        }
                    }
                    bool probed_surfaces = false;
                    const char* dump_bb = getenv("UC3_DUMP_BB");
                    int dump_bb_done = 0;
                    unsigned loop_frame = 0;
                    /* This replay loop owns the screen; stop the game's idle GCM
                     * flip from presenting black frames over the menu. */
                    if (!fifo_draws.empty())
                        rsx_d3d12_set_replay_present_owner(1);
                    for (;;) {
                        int rc = d3d12_ok ? rsx_d3d12_backend_pump_messages()
                                          : rsx_null_backend_pump_messages();
                        if (rc < 0) break;
                        if (!fifo_draws.empty())
                            rrc_submit_fifo_frame(fifo_draws, fifo_shaders);
                        ++loop_frame;
                        /* Ground-truth GPU capture: dump the presented backbuffer to
                         * raw RGBA once the render is stable (PrintWindow/GDI cannot
                         * capture a D3D12 flip-model swapchain). */
                        if (dump_bb && d3d12_ok && loop_frame >= 30 &&
                            (loop_frame == 30 || (loop_frame % 600) == 0)) {
                            dump_bb_done = 1;
                            rsx_d3d12_dump_backbuffer(dump_bb); /* re-dump periodique post-load */
                        }
                        if (!probed_surfaces && !fifo_draws.empty() &&
                            getenv("UC3_RRC_PROBE") != nullptr) {
                            probed_surfaces = true;
                            for (uint32_t si = 0; si < 12; ++si)
                                rsx_d3d12_debug_surface_probe(si);
                        }
                        else if (!inj_verts.empty())
                            rsx_d3d12_inject_mesh(inj_verts.data(),
                                                  (unsigned)(inj_verts.size()/3), inj_prim);
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
                const bool debug_replay = getenv("UC3_INJECT_GEOM") != nullptr ||
                                          getenv("UC3_RRC_FIFO") != nullptr;
                const bool route = !debug_replay &&
                                   (g_rsx_ready.load() || getenv("UC3_NO_WINDOW") == nullptr);
                /* Watchdog de gel (UC3_BB_DUMP diag) : g_drainer_stage suit la
                 * section courante; un thread imprime stage+iter toutes les 5 s
                 * → identifie EXACTEMENT où la boucle se fige. */
                static std::atomic<int> g_drainer_stage{0};
                static std::atomic<uint64_t> g_drainer_iter{0};
                if (getenv("UC3_BB_DUMP")) {
                    std::thread([]{
                        for (;;) {
                            std::this_thread::sleep_for(std::chrono::seconds(5));
                            fprintf(stderr, "[drainer-watch] iter=%llu stage=%d\n",
                                    (unsigned long long)g_drainer_iter.load(),
                                    g_drainer_stage.load());
                        }
                    }).detach();
                }
                for (;;) {
                    g_drainer_iter.fetch_add(1);
                    g_drainer_stage.store(1);   /* 1 = custom-pending drain */
                    /* Drain the game's CUSTOM menu command buffer if the overflow
                     * callback posted one (runs here on the D3D12-owner thread). */
                    if (uint32_t cg = g_custom_gcm_pending.load()) {
                        uc3_drain_custom_gcm(cg);
                        g_custom_gcm_pending.store(0);
                    }
                    g_drainer_stage.store(2);   /* 2 = autodrain ring custom */
                    /* UC3_GCM_AUTODRAIN[=hexctx] : drain PÉRIODIQUE INCRÉMENTAL du
                     * command buffer custom. Le drain overflow-only ne consomme
                     * RIEN tant que le ring (0x54000) ne déborde pas — or le flip
                     * du frontend est émis comme MÉTHODE GCM dans CE ring, jamais
                     * comme appel HLE : sans consommation continue, aucun draw ni
                     * flip n'atteint D3D12. On traite [last, current) à chaque
                     * itération ; si current recule (rewind du jeu), on repart de
                     * begin. Contexte par défaut : 0x3074F000 (STATUS §595-612). */
                    {
                        static uint32_t s_ad_ctx = 0; static uint32_t s_ad_last = 0;
                        static int s_ad_init = 0;
                        if (!s_ad_init) { s_ad_init = 1;
                            if (const char* ad = getenv("UC3_GCM_AUTODRAIN")) {
                                uint32_t v = (uint32_t)strtoul(ad, nullptr, 16);
                                s_ad_ctx = v >= 0x10000u ? v : 0x3074F000u;
                            }
                        }
                        if (s_ad_ctx) {
                            uint32_t begin   = vm_read32(s_ad_ctx + 0x00);
                            uint32_t end     = vm_read32(s_ad_ctx + 0x04);
                            uint32_t current = vm_read32(s_ad_ctx + 0x08);
                            if (begin >= 0x10000u && end > begin &&
                                current >= begin && current <= end) {
                                if (s_ad_last < begin || s_ad_last > end ||
                                    current < s_ad_last)
                                    s_ad_last = begin;      /* rewind / 1er passage */
                                if (current > s_ad_last) {
                                    /* réutilise le walker en simulant un contexte
                                     * borné [s_ad_last, current) : on écrit un
                                     * pseudo-contexte sur pile guest ? Non — on
                                     * duplique la boucle, bornée, ici même. */
                                    const bool route = g_rsx_ready.load();
                                    uint32_t off = s_ad_last, guard = 0; uint32_t nm = 0;
                                    while (off < current && guard++ < 400000u) {
                                        uint32_t hdr = vm_read32(off); off += 4;
                                        if (hdr == 0) continue;
                                        uint32_t type = hdr >> 29;
                                        if (type == 1) {
                                            uint32_t t = hdr & 0x1FFFFFFC;
                                            if (t >= begin && t < end) off = t;
                                            else break;
                                            continue;
                                        }
                                        if ((hdr & 3) == 2) continue;
                                        uint32_t count  = (hdr >> 18) & 0x7FF;
                                        uint32_t method = hdr & 0x1FFC;
                                        bool noinc = (type == 2);
                                        g_adrain_hist[(method >> 2) & 0x7FF]++;
                                        /* UC3_ADRAIN_DUMPBE : dump brut de la séquence
                                         * de méthodes autour des BEGIN_END du burst —
                                         * révèle COMMENT la géométrie EDGE est soumise
                                         * (aucun SET_VERTEX_DATA_ARRAY vu : inline?
                                         * autre fenêtre?). 2 blocs max, 48 méthodes. */
                                        static int s_dbe_left = 0; static int s_dbe_blocks = 0;
                                        if (getenv("UC3_ADRAIN_DUMPBE")) {
                                            uint32_t d0 = count ? vm_read32(off) : 0;
                                            if (method == 0x1808 && d0 != 0 &&
                                                s_dbe_blocks < 2) {
                                                s_dbe_blocks++; s_dbe_left = 48;
                                            }
                                            if (s_dbe_left > 0) { s_dbe_left--;
                                                fprintf(stderr, "[be-seq] m=%04X n=%u d0=%08X%s\n",
                                                        method, count, d0,
                                                        noinc ? " ni" : "");
                                            }
                                        }
                                        if (route)
                                            for (uint32_t i = 0; i < count; i++)
                                                rsx_process_method(&g_rsx_state,
                                                    noinc ? method : method + i*4,
                                                    vm_read32(off + i*4));
                                        off += count * 4;
                                        nm++;
                                    }
                                    s_ad_last = current;
                                    static int s_adl = 0;
                                    if (nm && s_adl < 40) { s_adl++;
                                        fprintf(stderr, "[gcm-autodrain] ctx=0x%08X +%u methods "
                                                "(-> 0x%08X/0x%08X)\n", s_ad_ctx, nm, current, end);
                                    }
                                    /* UC3_ADRAIN_HIST : histogramme des méthodes du
                                     * ring custom — révèle la méthode FLIP/queue que
                                     * rsx_process_method ignorerait (le jeu attend
                                     * son acquittement pour dessiner la frame
                                     * suivante → il s'arrête après ~10 frames). */
                                    if (getenv("UC3_ADRAIN_HIST")) {
                                        static uint32_t s_mh[0x800] = {0};
                                        static uint64_t s_mh_total = 0;
                                        /* re-walk juste pour compter (borné, léger) */
                                        uint32_t o2 = begin, g2 = 0;
                                        (void)o2; (void)g2;
                                        /* compté au vol ci-dessus? non — recomptons
                                         * sur la fenêtre traitée [ancien last, current) :
                                         * approximation: compter à partir de begin
                                         * n'est pas correct; on compte DANS la boucle
                                         * principale la prochaine fois. Ici: dump
                                         * périodique du cumul. */
                                        s_mh_total += nm;
                                        extern uint32_t g_adrain_hist[0x800];
                                        static int s_mhd = 0;
                                        if (s_mh_total > 400 && s_mhd < 8 &&
                                            (s_adl % 5) == 0) { s_mhd++;
                                            fprintf(stderr, "[adrain-hist] total=%llu top:",
                                                    (unsigned long long)s_mh_total);
                                            for (int t = 0; t < 12; t++) {
                                                uint32_t bi = 0, bv = 0;
                                                for (uint32_t i = 0; i < 0x800; i++)
                                                    if (g_adrain_hist[i] > bv) { bv = g_adrain_hist[i]; bi = i; }
                                                if (!bv) break;
                                                fprintf(stderr, " %04X:%u", bi << 2, bv);
                                                g_adrain_hist[bi] = 0;
                                            }
                                            fprintf(stderr, "\n");
                                        }
                                    }
                                }
                            }
                        }
                    }
                    g_drainer_stage.store(3);   /* 3 = FIFO principal */
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
                    /* UC3_VBLANK: the game renders its menu to its FIFO but never
                     * issues cellGcmSetFlipCommand (flip=0), so the D3D12 backend
                     * never presents. Force a present at ~60Hz here on the RSX
                     * thread (which owns D3D12 — safe) so the rendered menu frame
                     * becomes visible. */
                    /* Présentateur découplé 60Hz activé PAR DÉFAUT (opt-out
                     * UC3_NO_VBLANK). Le jeu rend sa frame dans la FIFO mais
                     * n'émet jamais cellGcmSetFlipCommand → sans ce présentateur,
                     * D3D12 ne présente jamais. C'est le bon modèle de présent et
                     * la voie de visibilité une fois le gate SPU levé. NOTE: à lui
                     * seul il n'affiche PAS le menu — au gate courant les frames
                     * sont noires (textures non-décodées par les workloads SPURS
                     * wid=2/4) et le render loop se bloque à ~24s dans le
                     * frame-sync func_00D51D60. Vérifié par readback GPU
                     * (UC3_BB_DUMP: bb.120 = 100% noir) 2026-07-14. */
                    if (route && g_rsx_ready.load() && !getenv("UC3_NO_VBLANK")) {
                        /* Cadence au TEMPS réel : le seuil "160 itérations"
                         * supposait des itérations de 100µs, mais sleep_for(100µs)
                         * sous Windows dort ~15ms (résolution timer) → 1 present
                         * toutes les ~2,5s (0,4 FPS). Un pas de 16ms d'horloge
                         * donne un vrai ~60Hz quel que soit le coût d'itération. */
                        static auto s_last_present = std::chrono::steady_clock::now();
                        auto now = std::chrono::steady_clock::now();
                        if (now - s_last_present >= std::chrono::milliseconds(16)) {
                            s_last_present = now;
                            /* Pomper les messages de la fenêtre AVANT Present :
                             * un swapchain DXGI flip-model sans message pump se
                             * bloque après ~1 frame (la file de présentation ne
                             * se vide que si la fenêtre traite ses messages) —
                             * observé : 1 seul present en 75 s, boucle figée. La
                             * voie replay pompait déjà à chaque frame. */
                            g_drainer_stage.store(4);   /* 4 = pump+present */
                            rsx_d3d12_backend_pump_messages();
                            g_drainer_stage.store(5);   /* 5 = render_frame/present */
                            rsx_d3d12_backend_present();
                            g_drainer_stage.store(6);   /* 6 = post-present */
                            /* ACQUITTEMENT DU FLIP : le jeu émet ses draws puis
                             * ATTEND le flip-complete (handler d'interrupt flip +
                             * flip status) avant de dessiner la frame suivante —
                             * personne n'appelait cellGcmTickFlip() → le frontend
                             * s'arrêtait de dessiner après ~10 frames (les buffers
                             * en vol) et plus aucun draw n'était émis. Un tick par
                             * present = flip-complete à ~60Hz, comme la console. */
                            { extern void cellGcmTickFlip(void);
                              cellGcmTickFlip(); }
                            /* UC3_BB_DUMP=<path.raw> : vérité-terrain GPU — dump du
                             * backbuffer AVANT Present après ~5s puis toutes les ~10s
                             * (GDI/PrintWindow ne peut PAS capturer un swapchain
                             * flip-model, donc une capture écran noire ne prouve
                             * rien; seul ce readback montre ce que le GPU dessine). */
                            if (const char* bb = getenv("UC3_BB_DUMP")) {
                                static int s_bbn = 0; s_bbn++;
                                if ((s_bbn % 60) == 1)
                                    fprintf(stderr, "[bb-present] #%d\n", s_bbn);
                                static int s_at = 0;
                                if (!s_at) { const char* a = getenv("UC3_BB_DUMP_AT");
                                             s_at = (a && atoi(a) > 0) ? atoi(a) : 300; }
                                if (s_bbn == s_at || (s_bbn > s_at && (s_bbn % 600) == 0)) {
                                    char pth[512];
                                    snprintf(pth, sizeof pth, "%s.%d.raw", bb, s_bbn);
                                    int rc = rsx_d3d12_dump_backbuffer(pth);
                                    fprintf(stderr, "[bb-dump] armed '%s' (present #%d) rc=%d\n",
                                            pth, s_bbn, rc);
                                }
                            }
                        }
                    }
                    g_drainer_stage.store(9);   /* 9 = sleep fin d'itération */
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

static void uc3_start_vblank_driver();   /* fwd (defined below) */
static void br_cellGcmSetFlipMode(ppu_context* ctx) {
    cellGcmSetFlipMode(A0);
    uc3_start_vblank_driver();            /* display active -> ensure VBLANK driver runs */
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

/* ---- UC3 VBLANK driver (gated by UC3_VBLANK) --------------------------------
 * GCM games are VBLANK-driven: the RSX driver invokes the registered VBLANK
 * handler ~60x/s. Our HLE stored g_gcm.vblank_handler but NEVER invoked it, so
 * the game's display/init loop that waits on VBLANK spun forever (never flips).
 * This thread invokes the guest handler at ~60Hz on a dedicated guest stack,
 * via the same guest-call path used for thread entries. */
extern "C" uint32_t uc3_alloc_guest_stack(uint32_t size);
extern "C" void ppu_thread_entry_trampoline_impl(ppu_context* ictx);
extern "C" uint32_t g_canonical_toc;

static std::atomic<bool> g_vblank_started{false};
static void uc3_start_vblank_driver() {
    if (!getenv("UC3_VBLANK")) return;
    if (g_vblank_started.exchange(true)) return;
    std::thread([]{
        const uint32_t ssize = 0x20000u;
        uint32_t sbase = uc3_alloc_guest_stack(ssize);
        fprintf(stderr, "[vblank] driver started (stack=0x%08X)\n", sbase);
        if (!sbase) return;
        unsigned long long n = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::microseconds(16667)); /* ~60Hz */
            uint32_t h;
            { std::lock_guard<std::mutex> lk(g_gcm_mutex); h = g_gcm.vblank_handler; }
            if (!h) continue;
            ppu_context c;
            memset(&c, 0, sizeof(c));
            c.cia   = h;                                   /* handler OPD */
            c.gpr[3] = 0;                                  /* head number 0 */
            c.gpr[1] = (uint64_t)((sbase + ssize - 48) & ~0xFu); /* stack ptr */
            c.gpr[2] = g_canonical_toc;
            ppu_thread_entry_trampoline_impl(&c);
            /* UC3_GFXCB: also invoke the other registered-but-ignored GCM handlers
             * (flip + graphics/RSX-interrupt) — same winning pattern as VBLANK, in
             * case the next busy-wait polls for one of them. Sequential reuse of
             * the same stack is fine (each call returns before the next). */
            if (getenv("UC3_GFXCB")) {
                uint32_t fh, gh;
                { std::lock_guard<std::mutex> lk(g_gcm_mutex);
                  fh = g_gcm.flip_handler; gh = g_gcm.graphics_handler; }
                for (uint32_t hh : {fh, gh}) {
                    if (!hh) continue;
                    ppu_context c2; memset(&c2, 0, sizeof(c2));
                    c2.cia = hh; c2.gpr[3] = 0;
                    c2.gpr[1] = (uint64_t)((sbase + ssize - 48) & ~0xFu);
                    c2.gpr[2] = g_canonical_toc;
                    ppu_thread_entry_trampoline_impl(&c2);
                }
            }
            if ((++n % 300ull) == 0)
                fprintf(stderr, "[vblank] handler fired %llu times\n", n);
        }
    }).detach();
}

static void br_cellGcmSetVBlankHandler(ppu_context* ctx) {
    g_gcm.vblank_handler = A0;
    fprintf(stderr, "[gcm] SetVBlankHandler(0x%08X)\n", (uint32_t)A0);
    uc3_start_vblank_driver();
    RET(0);
}

static void br_cellGcmSetFlipHandler(ppu_context* ctx) {
    g_gcm.flip_handler = A0;
    RET(0);
}

/* ---------------------------------------------------------------------------
 * cellSaveDataListAutoLoad HLE (boot save-check).
 *
 * RPCS3 ground truth (rpcs3.log): the main thread reaches the boot milestone
 * cellGameUnregisterDiscChangeCallback (LR=0x00044314) at t=51.77s, spawns the
 * "Save/Load Game Thread" (entry OPD 0x011B3FE8 -> func_009B05D8), and that
 * thread calls cellSaveDataListAutoLoad 0.7ms later (LR=0x009B011C, prefix
 * 'BCES01175_NDI_UNCHARTED3_BT_'); the menu starts rendering right after the
 * op completes. Our port spawned the thread but the NID (0x21425307) was
 * UNREGISTERED. funcFixed/funcStat/funcFile are GUEST OPDs — dispatch them
 * synchronously on this (guest) thread via the proven vblank guest-call
 * pattern. Struct layouts are the SDK/RPCS3 ones (our cellSaveData.h CBRESULT
 * constants are inverted vs SDK — SDK literals used here: OK_NEXT=0,
 * OK_LAST=1, ERR_NODATA=-4).
 * -----------------------------------------------------------------------*/
static void uc3_call_guest7(uint32_t opd, uint32_t a0, uint32_t a1,
                            uint32_t a2, uint32_t a3, uint32_t a4,
                            uint32_t a5, uint32_t a6) {
    static thread_local uint32_t t_stack = 0;
    const uint32_t ssize = 0x20000u;
    if (!t_stack) t_stack = uc3_alloc_guest_stack(ssize);
    if (!t_stack) return;
    ppu_context c;
    memset(&c, 0, sizeof(c));
    c.cia    = opd;
    c.gpr[3] = a0; c.gpr[4] = a1; c.gpr[5] = a2; c.gpr[6] = a3;
    c.gpr[7] = a4; c.gpr[8] = a5; c.gpr[9] = a6;
    c.gpr[1] = (uint64_t)((t_stack + ssize - 48) & ~0xFu);
    c.gpr[2] = g_canonical_toc;
    ppu_thread_entry_trampoline_impl(&c);
}
static void uc3_call_guest5(uint32_t opd, uint32_t a0, uint32_t a1,
                            uint32_t a2, uint32_t a3, uint32_t a4) {
    uc3_call_guest7(opd, a0, a1, a2, a3, a4, 0, 0);
}
static void uc3_call_guest4(uint32_t opd, uint32_t a0, uint32_t a1,
                            uint32_t a2, uint32_t a3) {
    uc3_call_guest5(opd, a0, a1, a2, a3, 0);
}

/* sceNpTrophy family bridges. This project has NO generated HLE registration
 * unit (only the hand bridges below exist), so the whole trophy family was
 * unresolved: the NpTrophy worker (entry OPD 0x0117D318 -> func_0009DE34) ran
 * on GARBAGE context/handle values (silent 0 returns, outputs never written)
 * — a boot-variance source, since the main thread's Gate 2 waits on that
 * worker's completion flag before "Init Time". The lib is host-complete; the
 * bridges marshal guest pointers. */
static void br_sceNpTrophyInit(ppu_context* ctx) {
    RET(sceNpTrophyInit(NULL, (u32)A1, (u32)A2, 0));
}
static void br_sceNpTrophyTerm(ppu_context* ctx) {
    RET(sceNpTrophyTerm());
}
static void br_sceNpTrophyCreateContext(ppu_context* ctx) {
    /* (context* out, commId*, commSign*, options) */
    if (!A0) { RET((int32_t)0x8002290Cu); return; }
    SceNpTrophyContext c = 0;
    int32_t rc = sceNpTrophyCreateContext(&c, NULL, NULL, 0);
    if (rc == 0) vm_write32((uint32_t)A0, (uint32_t)c);
    fprintf(stderr, "[trophy] CreateContext -> rc=0x%X ctx=%d\n", (uint32_t)rc, (int)c);
    RET(rc);
}
static void br_sceNpTrophyDestroyContext(ppu_context* ctx) {
    RET(sceNpTrophyDestroyContext((s32)A0));
}
static void br_sceNpTrophyCreateHandle(ppu_context* ctx) {
    if (!A0) { RET((int32_t)0x8002290Cu); return; }
    SceNpTrophyHandle h = 0;
    int32_t rc = sceNpTrophyCreateHandle(&h);
    if (rc == 0) vm_write32((uint32_t)A0, (uint32_t)h);
    RET(rc);
}
static void br_sceNpTrophyDestroyHandle(ppu_context* ctx) {
    RET(sceNpTrophyDestroyHandle((s32)A0));
}
static void br_sceNpTrophyGetRequiredDiskSpace(ppu_context* ctx) {
    /* (context, handle, u64* reqSpace, options) */
    u64 space = 0;
    int32_t rc = sceNpTrophyGetRequiredDiskSpace((s32)A0, (s32)A1, &space, 0);
    if (rc == 0 && A2) vm_write64((uint32_t)A2, space);
    RET(rc);
}
static void br_sceNpTrophySetSoundLevel(ppu_context* ctx) {
    RET(0); /* (context, handle, level, options) — nothing to do on host */
}
static void br_sceNpTrophyGetGameProgress(ppu_context* ctx) {
    s32 pct = 0;
    int32_t rc = sceNpTrophyGetGameProgress((s32)A0, (s32)A1, &pct);
    if (rc == 0 && A2) vm_write32((uint32_t)A2, (uint32_t)pct);
    RET(rc);
}
static void br_sceNpTrophyUnlockTrophy(ppu_context* ctx) {
    /* (context, handle, trophyId, platinumId* out) */
    s32 plat = -1;
    int32_t rc = sceNpTrophyUnlockTrophy((s32)A0, (s32)A1, (s32)A2, &plat);
    if (A3) vm_write32((uint32_t)A3, (uint32_t)plat);
    fprintf(stderr, "[trophy] UnlockTrophy(ctx=%d,h=%d,id=%d) -> rc=0x%X\n",
            (int)A0, (int)A1, (int)A2, (uint32_t)rc);
    RET(rc);
}

/* sceNpTrophyRegisterContext — the REAL API is (context, handle, statusCb,
 * arg, options): our lib's 3-arg version silently ate the STATUS CALLBACK, so
 * the game's trophy-init never saw completion. RPCS3 oracle dispatches it once
 * with trp_status=3 (SCE_NP_TROPHY_STATUS_INSTALLED) when the trophy config is
 * already installed — the game's statusCb (OPD 0x117d2f8) then marks trophy
 * init done, a step of the boot chain leading to the Save/Load milestone. */
static void br_sceNpTrophyRegisterContext(ppu_context* ctx) {
    const uint32_t context_id = (uint32_t)A0;
    const uint32_t handle     = (uint32_t)A1;
    const uint32_t statusCb   = (uint32_t)A2;
    const uint32_t cb_arg     = (uint32_t)A3;
    int32_t rc = sceNpTrophyRegisterContext((s32)context_id, (s32)handle, 0);
    fprintf(stderr, "[trophy] RegisterContext(ctx=%u,h=%u,cb=0x%08X) rc=0x%X%s\n",
            context_id, handle, statusCb, (uint32_t)rc,
            (rc == 0 && statusCb) ? " -> dispatch statusCb(INSTALLED)" : "");
    if (rc == 0 && statusCb)
        uc3_call_guest5(statusCb, context_id, 3 /*SCE_NP_TROPHY_STATUS_INSTALLED*/,
                        100, 100, cb_arg);
    RET(rc);
}

static void br_sceNpTrophyGetTrophyUnlockState(ppu_context* ctx) {
    /* (context, handle, SceNpTrophyFlagArray* flags[16B], u32* count) — was an
     * unresolved NID (0xB3AC3478) returning 0 with UNFILLED outputs. */
    if (!A2 || !A3) { RET((int32_t)0x8002290Cu /*INVALID_ARGUMENT*/); return; }
    SceNpTrophyFlagArray flags;
    u32 count = 0;
    int32_t rc = sceNpTrophyGetTrophyUnlockState((s32)A0, (s32)A1, &flags, &count);
    if (rc == 0) {
        for (int i = 0; i < 4; i++) vm_write32((uint32_t)A2 + i*4, flags.flag[i]);
        vm_write32((uint32_t)A3, count);
    }
    RET(rc);
}

/* Disc assets are already authenticated by the user's mounted game data.
 * sceNpDrmIsAvailable2 has no output structure: it validates a k_licensee/path
 * pair and returns an error code. UC3 uses it from its FIOS media path. */
static void br_sceNpDrmIsAvailable2(ppu_context* ctx) {
    if (!A0 || !A1) {
        RET((int32_t)0x80010002u /* CELL_EINVAL */);
        return;
    }
    RET(0);
}

/* -------------------------------------------------------------------------
 * Pump keeper (UC3_PUMP_KEEPER, opt-in).
 *
 * Root cause (STATUS 2026-07-10): the frontend children's state machine is
 * pumped by func_0089FAE0 ONLY from the level-load window (func_008BC86C
 * loop) and the per-frame level update (downstream of boot). When the load
 * window closes before the children converge (a decode-speed race our port
 * loses ~85% of runs), NOTHING pumps them again -> flag1 never set -> Gate 3
 * parks forever. On a real PS3 the SPU decode finishes in milliseconds so
 * the race is never lost. This keeper re-ticks the SAME pump the game uses,
 * only after the game's own pump has been quiet >250ms (func_0089FAE0 entry
 * stamps uc3_note_pump), so it never runs concurrently with the game's.
 * Manager chain: *( *( *(TOC-0x7218) - 0x7FBC ) + 0x1C ).
 * -----------------------------------------------------------------------*/
static std::atomic<unsigned long long> g_uc3_last_pump_ms{0};
static thread_local bool g_uc3_keeper_pump = false;
/* Helper pour le code injecte par patches.json (starter-defer-ctor) :
 * sleep portable exportee vers les fichiers generes (linkage C++ pour
 * matcher la declaration bloc-scope dans le code injecte). */
void uc3_sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

extern "C" void uc3_note_pump(void) {
    /* UC3_PUMP_TRACE : tracer PUMP-FREQUENCE (chaque tick de func_0089FAE0,
     * jeu + keeper) READ-ONLY. Capture les transitions de cur (mgr+0x7F0) que
     * la sonde fe-state a 300ms manque (push->dispatch->pop intra-tick). Logue
     * seulement au CHANGEMENT de (cur, ring-count DA4, pendCount 7EC, gate7F8).
     * Prouve si func_0089F680 POUSSE reellement un ecran (cur!=0) et si le ring
     * d'ecrans (mgr+0xDA4) est jamais peuple. Aucune ecriture (regle #6). */
    if (getenv("UC3_PUMP_TRACE") && vm_base != nullptr && g_canonical_toc != 0) {
        uint32_t r30 = vm_read32(g_canonical_toc - 0x7218);
        uint32_t r9  = (r30 >= 0x10000u) ? vm_read32(r30 - 0x7FBC) : 0;
        uint32_t mgr = (r9  >= 0x10000u) ? vm_read32(r9 + 0x1C) : 0;
        if (mgr >= 0x10000u) {
            uint32_t cur   = vm_read32(mgr + 0x7F0);
            uint32_t da4   = vm_read32(mgr + 0xDA4);  /* ring: nb ecrans en file */
            uint32_t d9c   = vm_read32(mgr + 0xD9C);  /* ring head */
            uint32_t da8   = vm_read32(mgr + 0xDA8);  /* ring base ptr */
            uint32_t pend  = vm_read32(mgr + 0x7EC);
            uint8_t  g7f8  = vm_read8(mgr + 0x7F8);
            static thread_local uint32_t last_cur = 0xFFFFFFFFu, last_da4 = 0xFFFFFFFFu,
                                         last_pend = 0xFFFFFFFFu; static thread_local uint8_t last_g = 0xFF;
            static thread_local unsigned long long pushes = 0, ticks = 0; ++ticks;
            if (cur != last_cur || da4 != last_da4 || pend != last_pend || g7f8 != last_g) {
                if (last_cur == 0 && cur != 0) ++pushes;  /* transition 0->non-nul = PUSH */
                fprintf(stderr, "[pump-trace] tick=%llu mgr=0x%08X cur=0x%08X ring:DA4=%u head=%u base=0x%08X "
                        "pend7EC=%u gate7F8=%u pushes=%llu\n",
                        ticks, mgr, cur, da4, d9c, da8, pend, g7f8, pushes);
                last_cur = cur; last_da4 = da4; last_pend = pend; last_g = g7f8;
            }
        }
    }
    if (g_uc3_keeper_pump) return;
    g_uc3_last_pump_ms.store(GetTickCount64(), std::memory_order_relaxed);
}
extern void func_0089FAE0(ppu_context* ctx);
extern void func_0089F418(ppu_context* ctx);
extern void func_008BA510(ppu_context* ctx);
extern void func_008B6F8C(ppu_context* ctx);
extern void func_0074A160(ppu_context* ctx);
extern void func_0003E724(ppu_context* ctx);
extern "C" __declspec(thread) void (*g_trampoline_fn)(void*);

static void uc3_start_pump_keeper(void) {
    if (!getenv("UC3_PUMP_KEEPER")) return;
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([]{
        const uint32_t ssize = 0x20000u;
        /* This thread is started by a static constructor, before main has
         * initialized the guest VM. Wait for boot instead of losing the keeper
         * permanently to the first pre-VM allocation attempt. */
        while (!vm_base || !g_canonical_toc)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        uint32_t sbase = 0;
        for (int retry = 0; retry < 200 && !sbase; ++retry) {
            sbase = uc3_alloc_guest_stack(ssize);
            if (!sbase)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!sbase) {
            fprintf(stderr, "[pump-keeper] guest stack allocation failed\n");
            return;
        }
        fprintf(stderr, "[pump-keeper] armed (stack=0x%08X)\n", sbase);
        unsigned long long kicks = 0;
        size_t active_cursor = 0;
        /* UC3_PUMP_TURBO : experience — pomper le frontend AGRESSIVEMENT (sleep
         * 1ms, seuil-quiet 30ms) pour traverser les 51 records en secondes et
         * OBSERVER l'etat POST-registre (transition ? re-cycle ? idle ?) que les
         * runs a vitesse-keeper (~2.7 chg/s) n'atteignent jamais. Le main dort
         * en FIOS donc concurrence improbable. Env-gate (defaut inchange). */
        const bool turbo_env = getenv("UC3_PUMP_TURBO") != nullptr;
        /* UC3_PUMP_FRAME : pompe le pump de chargement frontend a CADENCE FRAME
         * (~10ms) sans dependre de flag1. Racine (workflow FIOS 2026-07-14) : la
         * chaine reaper(func_0089B900)->drain(func_0089EBB0) qui draine les chunks
         * et avance CHILD+0x18 5->8 tourne sur le MAIN THREAD dans le pump
         * func_0089FAE0, mais le main est parke en sys_lwcond_wait FIOS et ne
         * remonte pas executer le pump -> ~3 pumps/50s au lieu de 60fps -> les
         * chunks ne drainent jamais -> render state=6 bloque. On donne au reaper/
         * drain DU JEU le CPU que le main parke lui refuse (regle #6 : le travail
         * reel = les octets deja lus par FIOS ; on ne fait qu'appeler le pump du
         * jeu plus souvent). Le garde g_uc3_keeper_pump evite la concurrence. */
        const bool frame_env = getenv("UC3_PUMP_FRAME") != nullptr;
        for (;;) {
            /* Turbo SEULEMENT apres load-complete (flag1==0) : pendant la
             * fenetre de chargement le pump du jeu est actif et un keeper
             * agressif risquerait de courser le sien (la course decode est
             * deja perdue ~85% des runs — ne pas l'empirer). Post-load le
             * main dort en FIOS: aucune concurrence. */
            const bool turbo = turbo_env && vm_base && vm_read8(0x011CE590u) == 0;
            const unsigned sleep_ms = frame_env ? 5u : (turbo ? 1u : 50u);
            const unsigned quiet_ms = frame_env ? 16u : (turbo ? 30u : 1000u);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            unsigned long long last = g_uc3_last_pump_ms.load(std::memory_order_relaxed);
            if (!last) continue;                     /* game never pumped yet */
            unsigned long long now = GetTickCount64();
            if (now - last < quiet_ms) continue;     /* game pump active */
            /* resolve manager: *( *( *(TOC-0x7218) - 0x7FBC ) + 0x1C ) */
            uint32_t r30 = vm_read32(g_canonical_toc - 0x7218);
            if (r30 < 0x10000) continue;
            uint32_t r9 = vm_read32(r30 - 0x7FBC);
            if (r9 < 0x10000) continue;
            uint32_t mgr = vm_read32(r9 + 0x1C);
            if (mgr < 0x10000) continue;
            /* Manager records remain authoritative while their objects are in
             * flight, even after the world-test load window has closed. */
            uint32_t cur = vm_read32(mgr + 0x7F0);
            bool inflight = cur != 0;
            uint32_t arr = 0;
            std::vector<uint32_t> active;
            uint32_t state_counts[12] = {};
            uint32_t record_counts[8] = {};
            uint32_t terminal_records = 0;
            arr = vm_read32(mgr + 0x18);
            if (arr >= 0x10000) {
                for (int i = 0; i < 250; i++) {
                    uint32_t record = arr + i * 0x2F8;
                    uint32_t record_state = vm_read32(record + 0x18);
                    if (record_state == 0xBu) ++terminal_records;
                    if (record_state < 8) ++record_counts[record_state];
                    uint32_t obj = vm_read32(record + 0x20);
                    if (obj >= 0x10000) {
                        uint32_t state = vm_read32(obj + 0x20);
                        if (state < 12) ++state_counts[state];
                        if (record_state != 0xBu && state > 0 && state < 0xBu) {
                            inflight = true;
                            active.push_back(obj);
                        }
                    }
                }
            }
            if (getenv("UC3_LOAD_COMPLETE_HLE") && terminal_records >= 50u &&
                vm_read8(0x011CE590u) != 0) {
                vm_write8(0x011CE590u, 0);
                fprintf(stderr, "[load-complete] %u terminal records -> flag1=0\n",
                        terminal_records);
            }
            if (!inflight) {
                static unsigned miss = 0;
                if ((++miss % 100u) == 1u) {
                    fprintf(stderr, "[pump-keeper] idle lookup r30=%08X r9=%08X "
                                    "mgr=%08X cur=%08X arr=%08X states=",
                            r30, r9, mgr, cur, arr);
                    for (int i = 0; i < 6 && arr >= 0x10000; ++i) {
                        uint32_t obj = vm_read32(arr + i * 0x2F8 + 0x20);
                        fprintf(stderr, "%08X:%08X%s", obj,
                                obj >= 0x10000 ? vm_read32(obj + 0x20) : 0,
                                i == 5 ? "" : ",");
                    }
                    fprintf(stderr, "\n");
                    /* NOM des assets figes a state 8 (obj+0x24) -> identifier
                     * ce que le decode SPU doit finaliser (Phase 11). Declencheur
                     * corrige : ne dumper QUE si >=1 objet est REELLEMENT a
                     * state 8 (one-shot sinon consomme trop tot -> 0 nom). */
                    static bool s_named = false;
                    if (!s_named && arr >= 0x10000) {
                        int n8 = 0;
                        for (int i = 0; i < 250; ++i) {
                            uint32_t obj = vm_read32(arr + i * 0x2F8 + 0x20);
                            if (obj >= 0x10000u &&
                                vm_read32(obj + 0x20) == 8u) { n8++; break; }
                        }
                        if (n8 > 0) {
                            s_named = true;
                            for (int i = 0; i < 250; ++i) {
                                uint32_t rec = arr + i * 0x2F8;
                                uint32_t obj = vm_read32(rec + 0x20);
                                if (obj < 0x10000u) continue;
                                if (vm_read32(obj + 0x20) != 8u) continue;
                                char nm[24];
                                int k = 0;
                                for (; k < 23; ++k) {
                                    char c = (char)vm_read8(obj + 0x24 + k);
                                    if (c == 0) break;
                                    nm[k] = (c >= 32 && c < 127) ? c : '?';
                                }
                                nm[k] = 0;
                                fprintf(stderr, "[state8-asset] rec=%d "
                                        "obj=0x%08X rec+0x18=0x%08X name='%s'\n",
                                        i, obj, vm_read32(rec + 0x18), nm);
                            }
                        }
                    }
                }
                continue;
            }
            if (active.size() == 50) {
                static bool dumped_record_states = false;
                if (!dumped_record_states) {
                    dumped_record_states = true;
                    fprintf(stderr, "[pump-keeper] active record states:");
                    for (int i = 0; i < 250; ++i) {
                        uint32_t record = arr + i * 0x2F8;
                        uint32_t obj = vm_read32(record + 0x20);
                        if (obj >= 0x10000u && vm_read32(obj + 0x20) == 8u)
                            fprintf(stderr, " %d:%08X", i,
                                    vm_read32(record + 0x18));
                    }
                    fprintf(stderr, "\n");
                }
            }
            ppu_context c;
            memset(&c, 0, sizeof(c));
            c.cia    = 0;                            /* direct host call */
            c.fpr[1] = 0.016;   /* dt, threaded to per-child handlers */
            c.gpr[1] = (uint64_t)((sbase + ssize - 48) & ~0xFu);
            c.gpr[2] = g_canonical_toc;
            /* Match func_008BC86C: prepare the per-frame token through
             * func_0074A160(owner+0x20), then pass its return in r5. */
            c.gpr[3] = vm_read32(r9 + 0x20);
            func_0074A160(&c);
            while (g_trampoline_fn) { void(*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; c.gpr[2] = g_canonical_toc; tf((void*)&c); }
            uint32_t pump_token = (uint32_t)c.gpr[3];
            /* func_008BC86C receives this published level object in r3 and
             * naturally calls func_008BA510(level, token) each iteration. */
            uint32_t level = vm_read32(0x0112D5BCu);
            if (level < 0x10000u && !frame_env)
                continue;
            g_uc3_keeper_pump = true;
            /* The manager pump owns the child state machines. Parent polling
             * alone can observe child+0x18, but cannot dispatch state 2 into
             * func_0089CA34. Resume the same manager pump used during the
             * natural load window before checking the parent object. */
            c.fpr[1] = 0.016;
            c.gpr[1] = (uint64_t)((sbase + ssize - 48) & ~0xFu);
            c.gpr[2] = g_canonical_toc;
            c.gpr[3] = mgr;
            c.gpr[5] = pump_token;
            func_0089FAE0(&c);
            while (g_trampoline_fn) { void(*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; c.gpr[2] = g_canonical_toc; tf((void*)&c); }
            if (frame_env) {
                static unsigned long long _fk = 0; ++_fk;
                if ((_fk % 60ull) == 1ull) {
                    /* progression: render obj misc-fx 0x31473E20 state (+0x20) doit quitter 6 */
                    uint32_t ro = 0x31473E20u;
                    fprintf(stderr, "[pump-frame] kicks=%llu render-state(0x%08X)=%u terminal=%u inflight=%zu\n",
                            _fk, ro, vm_read32(ro + 0x20), terminal_records, active.size());
                }
            }
            /* A completed SPU job can leave its child detached from the manager
             * queue in this port. Resume only an orphaned state-2 child while
             * the manager is idle, then let the game's dispatcher construct
             * the descriptor and perform every subsequent state transition. */
            auto guest_name_has_prefix = [](uint32_t address,
                                            const char* prefix) {
                if (address < 0x10000u || !prefix)
                    return false;
                for (uint32_t i = 0; prefix[i]; ++i)
                    if (vm_read8(address + i) != (uint8_t)prefix[i])
                        return false;
                return true;
            };
            if (getenv("UC3_CHILD_RESUME_HLE") && active.size() == 1 &&
                vm_read32(mgr + 0x7F0) == 0 &&
                guest_name_has_prefix(active[0] + 0x24u,
                                      "tre-lon-diamond-lion-ba")) {
                uint32_t parent = active[0];
                uint32_t child_count = vm_read32(parent + 0x68);
                for (uint32_t i = 0; i < child_count; ++i) {
                    uint32_t child = vm_read32(parent + 0x6C + i * 4);
                    if (child < 0x10000u || vm_read32(child + 0x18) != 2u)
                        continue;
                    vm_write32(mgr + 0x7F0, child);
                    c.fpr[1] = 0.016;
                    c.gpr[1] = (uint64_t)((sbase + ssize - 48) & ~0xFu);
                    c.gpr[2] = g_canonical_toc;
                    c.gpr[3] = mgr;
                    c.gpr[4] = child;
                    func_0089F418(&c);
                    while (g_trampoline_fn) { void(*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; c.gpr[2] = g_canonical_toc; tf((void*)&c); }
                    fprintf(stderr, "[child-resume] parent=%08X child=%08X ss=%u cur=%08X\n",
                            parent, child, vm_read32(child + 0x18),
                            vm_read32(mgr + 0x7F0));
                    break;
                }
            }
            if (!active.empty()) {
                uint32_t obj = active[active_cursor++ % active.size()];
                c.fpr[1] = 0.016;
                c.gpr[1] = (uint64_t)((sbase + ssize - 48) & ~0xFu);
                c.gpr[2] = g_canonical_toc;
                c.gpr[3] = obj;
                c.gpr[5] = pump_token;
                func_008B6F8C(&c);
                while (g_trampoline_fn) { void(*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; c.gpr[2] = g_canonical_toc; tf((void*)&c); }
            }
            c.fpr[1] = 0.016;
            c.gpr[1] = (uint64_t)((sbase + ssize - 48) & ~0xFu);
            c.gpr[2] = g_canonical_toc;
            c.gpr[3] = level;
            c.gpr[5] = pump_token;
            uint32_t before_cur = vm_read32(mgr + 0x7F0);
            uint32_t before_958 = vm_read32(mgr + 0x958);
            uint32_t before_7ec = vm_read32(mgr + 0x7EC);
            uint32_t before_da4 = vm_read32(mgr + 0xDA4);
            uint32_t before_q0 = vm_read32(mgr + 0xD9C);
            uint32_t before_q8 = vm_read32(mgr + 0xDA4);
            uint32_t before_qc = vm_read32(mgr + 0xDA8);
            uint32_t before_q10 = vm_read32(mgr + 0xDAC);
            uint32_t before_slot = vm_read32(mgr + 0x404);
            func_008BA510(&c);
            while (g_trampoline_fn) { void(*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; c.gpr[2] = g_canonical_toc; tf((void*)&c); }
            g_uc3_keeper_pump = false;
            if ((++kicks % 20ull) == 1ull) {
                uint32_t active_obj = active.empty() ? 0u : active[0];
                uint32_t child_count = active_obj ? vm_read32(active_obj + 0x68) : 0u;
                uint32_t child0 = child_count > 0 ? vm_read32(active_obj + 0x6C) : 0u;
                uint32_t child1 = child_count > 1 ? vm_read32(active_obj + 0x70) : 0u;
                fprintf(stderr, "[pump-keeper] kick #%llu active=%zu level=%08X mgr=%08X token=%08X "
                                "cur=%08X>%08X x958=%08X x7ec=%08X "
                                "xda4=%08X q0=%08X q8=%08X qc=%08X "
                                "q10=%08X slot0=%08X stg=%u prog=%u "
                                "states[1..10]=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u "
                                "records[0..7]=%u,%u,%u,%u,%u,%u,%u,%u\n",
                        kicks, active.size(), level, mgr, pump_token, before_cur,
                        vm_read32(mgr + 0x7F0), before_958, before_7ec,
                        before_da4, before_q0, before_q8, before_qc,
                        before_q10, before_slot, vm_read32(mgr + 0x948),
                        vm_read32(mgr + 0x94C), state_counts[1], state_counts[2],
                        state_counts[3], state_counts[4], state_counts[5],
                        state_counts[6], state_counts[7], state_counts[8],
                        state_counts[9], state_counts[10], record_counts[0],
                        record_counts[1], record_counts[2], record_counts[3],
                        record_counts[4], record_counts[5], record_counts[6],
                        record_counts[7]);
                if (active_obj) {
                    fprintf(stderr, "[pump-active] obj=%08X state=%u name='%.16s' "
                                    "children=%u c0=%08X:%u c1=%08X:%u\n",
                            active_obj, vm_read32(active_obj + 0x20),
                            (const char*)(vm_base + active_obj + 0x24), child_count,
                            child0, child0 >= 0x10000u ? vm_read32(child0 + 0x18) : 0u,
                            child1, child1 >= 0x10000u ? vm_read32(child1 + 0x18) : 0u);
                }
            }
        }
    }).detach();
}
struct Uc3PumpKeeperInit { Uc3PumpKeeperInit() { uc3_start_pump_keeper(); } };
static Uc3PumpKeeperInit g_uc3_pump_keeper_init;

static void br_cellSaveDataListAutoLoad(ppu_context* ctx) {
    const uint32_t setList   = (uint32_t)A2;
    const uint32_t setBuf    = (uint32_t)A3;
    const uint32_t funcFixed = (uint32_t)ctx->gpr[7];
    const uint32_t funcStat  = (uint32_t)ctx->gpr[8];
    const uint32_t funcFile  = (uint32_t)ctx->gpr[9];
    /* arg9 (userdata) lives in the caller's param save area at r1+0x70 */
    const uint32_t userdata  = (uint32_t)vm_read64((uint32_t)ctx->gpr[1] + 0x70);
    if (!setList || !setBuf || !funcFixed || !funcStat) {
        RET(CELL_SAVEDATA_ERROR_PARAM); return;
    }

    char prefix[64] = {0};
    { uint32_t p = vm_read32(setList + 8);
      if (p) for (int i = 0; i < 63; i++) { char ch = (char)vm_read8(p + i); prefix[i] = ch; if (!ch) break; } }
    const uint32_t dirListMax  = vm_read32(setBuf + 0x0);
    const uint32_t fileListMax = vm_read32(setBuf + 0x4);
    fprintf(stderr, "[savedata] ListAutoLoad prefix='%s' dirListMax=%u fileListMax=%u "
            "fixed=0x%08X stat=0x%08X file=0x%08X ud=0x%08X\n",
            prefix, dirListMax, fileListMax, funcFixed, funcStat, funcFile, userdata);

    /* Guest scratch: upper half of the savedata scratch window (lib uses the
     * lower half; only one savedata op is ever in flight). */
    uint32_t ea = 0x024F0000u;
    auto salloc = [&](uint32_t size) { uint32_t a = ea; size = (size + 0xFu) & ~0xFu;
        ea += size; memset(vm_base + a, 0, size); return a; };
    const uint32_t cb_ea      = salloc(32);    /* CBResult (20B, SDK layout) */
    const uint32_t listGet_ea = salloc(16);    /* dirNum, dirListNum, dirList */
    const uint32_t nDirCap    = dirListMax ? (dirListMax < 16 ? dirListMax : 16) : 1;
    const uint32_t dirList_ea = salloc(168 * nDirCap);
    const uint32_t fixedSet_ea= salloc(16);
    const uint32_t statGet_ea = salloc(1648);
    const uint32_t statSet_ea = salloc(16);
    const uint32_t nFileCap   = fileListMax ? (fileListMax < 16 ? fileListMax : 16) : 1;
    const uint32_t fileList_ea= salloc(108 * nFileCap);
    const uint32_t fileGet_ea = salloc(96);
    const uint32_t fileSet_ea = salloc(64);

    /* Enumerate existing save dirs matching the prefix (same root as the lib). */
    const char* save_root = "./gamedata/dev_hdd0/home/00000001/savedata";
    uint32_t nDirs = 0;
    {
        std::error_code ec;
        std::filesystem::create_directories(save_root, ec);
        for (std::filesystem::directory_iterator it(save_root, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            std::string name = it->path().filename().string();
            if (strncmp(name.c_str(), prefix, strlen(prefix)) != 0) continue;
            if (nDirs < nDirCap) {
                uint32_t de = dirList_ea + 168 * nDirs;
                size_t len = name.size() < 31 ? name.size() : 31;
                memcpy(vm_base + de, name.c_str(), len);   /* dirName[32] */
            }
            nDirs++;
        }
    }
    vm_write32(listGet_ea + 0, nDirs);
    vm_write32(listGet_ea + 4, nDirs < nDirCap ? nDirs : nDirCap);
    vm_write32(listGet_ea + 8, dirList_ea);

    auto cb_init = [&](void) {
        vm_write32(cb_ea + 0x00, 0);          /* result = OK_NEXT (SDK: 0) */
        vm_write32(cb_ea + 0x04, 0);          /* progressBarInc */
        vm_write32(cb_ea + 0x08, 0);          /* errNeedSizeKB */
        vm_write32(cb_ea + 0x0C, 0);          /* invalidMsg */
        vm_write32(cb_ea + 0x10, userdata);   /* userdata */
    };
    auto map_cb_error = [](int32_t r) -> int32_t {
        return (r == -4) ? CELL_SAVEDATA_ERROR_NODATA : CELL_SAVEDATA_ERROR_CBRESULT;
    };

    /* 1) funcFixed picks the directory (even from an empty list). */
    cb_init();
    uc3_call_guest4(funcFixed, cb_ea, listGet_ea, fixedSet_ea, 0);
    int32_t r = (int32_t)vm_read32(cb_ea + 0x00);
    fprintf(stderr, "[savedata] funcFixed -> result=%d fixedSet.dirName=0x%08X\n",
            r, vm_read32(fixedSet_ea + 0));
    if (r < 0) { RET(map_cb_error(r)); return; }

    char dirName[64] = {0};
    { uint32_t p = vm_read32(fixedSet_ea + 0);
      if (p) for (int i = 0; i < 63; i++) { char ch = (char)vm_read8(p + i); dirName[i] = ch; if (!ch) break; } }
    if (!dirName[0]) { RET(CELL_SAVEDATA_ERROR_PARAM); return; }

    char save_path[512];
    snprintf(save_path, sizeof(save_path), "%s/%s", save_root, dirName);
    std::error_code ec;
    const bool exists = std::filesystem::is_directory(save_path, ec);

    /* 2) funcStat on the chosen dir. */
    vm_write32(statGet_ea + 0,    1024 * 1024);      /* hddFreeSizeKB = 1GB */
    vm_write32(statGet_ea + 4,    exists ? 0 : 1);   /* isNewData */
    { size_t len = strnlen(dirName, 31);
      memcpy(vm_base + statGet_ea + 32, dirName, len); } /* dir.dirName[32] */
    uint32_t nFiles = 0;
    uint64_t totalKB = 0;
    if (exists) {
        for (std::filesystem::directory_iterator it(save_path, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::string fn = it->path().filename().string();
            if (fn == "PARAM.SFO") continue;
            uint64_t fsz = (uint64_t)it->file_size(ec);
            totalKB += (fsz + 1023) / 1024;
            if (nFiles < nFileCap) {
                uint32_t fe = fileList_ea + 108 * nFiles;
                vm_write32(fe + 0, 1);                /* fileType = NORMALFILE */
                vm_write64(fe + 32, fsz);             /* fileSize */
                size_t len = fn.size() < 64 ? fn.size() : 64;
                memcpy(vm_base + fe + 40, fn.c_str(), len);
            }
            nFiles++;
        }
    }
    vm_write32(statGet_ea + 1616, 0);                 /* bind */
    vm_write32(statGet_ea + 1620, (uint32_t)totalKB); /* sizeKB */
    vm_write32(statGet_ea + 1624, 0);                 /* sysSizeKB */
    vm_write32(statGet_ea + 1628, nFiles);
    vm_write32(statGet_ea + 1632, nFiles < nFileCap ? nFiles : nFileCap);
    vm_write32(statGet_ea + 1636, fileList_ea);
    cb_init();
    uc3_call_guest4(funcStat, cb_ea, statGet_ea, statSet_ea, 0);
    r = (int32_t)vm_read32(cb_ea + 0x00);
    fprintf(stderr, "[savedata] funcStat(new=%d files=%u) -> result=%d\n",
            !exists, nFiles, r);
    if (r < 0) { RET(map_cb_error(r)); return; }

    /* 3) funcFile loop (load requested files into guest buffers). */
    if (funcFile && r == 0 /* OK_NEXT */) {
        for (int iter = 0; iter < 64; iter++) {
            memset(vm_base + fileGet_ea, 0, 96);
            memset(vm_base + fileSet_ea, 0, 64);
            cb_init();
            uc3_call_guest4(funcFile, cb_ea, fileGet_ea, fileSet_ea, 0);
            r = (int32_t)vm_read32(cb_ea + 0x00);
            uint32_t fn_ea  = vm_read32(fileSet_ea + 0x00);
            uint32_t fbuf   = vm_read32(fileSet_ea + 0x24);
            uint32_t fbufsz = vm_read32(fileSet_ea + 0x20);
            uint32_t foff   = vm_read32(fileSet_ea + 0x18);
            if (fn_ea && fbuf) {
                char fn[128] = {0};
                for (int i = 0; i < 127; i++) { char ch = (char)vm_read8(fn_ea + i); fn[i] = ch; if (!ch) break; }
                char fpath[768];
                snprintf(fpath, sizeof(fpath), "%s/%s", save_path, fn);
                uint32_t got = 0;
                FILE* fp = fopen(fpath, "rb");
                if (fp) {
                    if (foff) fseek(fp, (long)foff, SEEK_SET);
                    got = (uint32_t)fread(vm_base + fbuf, 1, fbufsz, fp);
                    fclose(fp);
                }
                vm_write32(fileGet_ea + 0, got);      /* excSize */
                fprintf(stderr, "[savedata] funcFile read '%s' off=%u -> %u bytes (result=%d)\n",
                        fn, foff, got, r);
            }
            if (r != 0) break;                        /* OK_LAST / error */
            if (!fn_ea || !fbuf) break;               /* nothing requested */
        }
        if (r < 0) { RET(map_cb_error(r)); return; }
    }

    fprintf(stderr, "[savedata] ListAutoLoad complete (dir='%s', %s)\n",
            dirName, exists ? "loaded" : "new");
    RET(0);
}

/* cellSaveDataListAutoSave — SAVE mirror of ListAutoLoad. Oracle: the FIRST
 * Save/Load thread op is family 3 (autosave/bind) and calls this import
 * (0x4DD03A4E, stub func_00D94614) — it was unregistered, so the bind-save
 * flow could never complete and the AutoLoad op was never posted. Same
 * guest-callback marshalling; file ops WRITE into the save dir. */
static void br_cellSaveDataListAutoSave(ppu_context* ctx) {
    const uint32_t setList   = (uint32_t)A2;
    const uint32_t setBuf    = (uint32_t)A3;
    const uint32_t funcFixed = (uint32_t)ctx->gpr[7];
    const uint32_t funcStat  = (uint32_t)ctx->gpr[8];
    const uint32_t funcFile  = (uint32_t)ctx->gpr[9];
    const uint32_t userdata  = (uint32_t)vm_read64((uint32_t)ctx->gpr[1] + 0x70);
    if (!setList || !setBuf || !funcFixed || !funcStat) {
        RET(CELL_SAVEDATA_ERROR_PARAM); return;
    }

    char prefix[64] = {0};
    { uint32_t p = vm_read32(setList + 8);
      if (p) for (int i = 0; i < 63; i++) { char ch = (char)vm_read8(p + i); prefix[i] = ch; if (!ch) break; } }
    const uint32_t dirListMax = vm_read32(setBuf + 0x0);
    fprintf(stderr, "[savedata] ListAutoSave prefix='%s' dirListMax=%u fixed=0x%08X stat=0x%08X file=0x%08X\n",
            prefix, dirListMax, funcFixed, funcStat, funcFile);

    uint32_t ea = 0x024F8000u;   /* upper quarter of the scratch window */
    auto salloc = [&](uint32_t size) { uint32_t a = ea; size = (size + 0xFu) & ~0xFu;
        ea += size; memset(vm_base + a, 0, size); return a; };
    const uint32_t cb_ea      = salloc(32);
    const uint32_t listGet_ea = salloc(16);
    const uint32_t nDirCap    = dirListMax ? (dirListMax < 16 ? dirListMax : 16) : 1;
    const uint32_t dirList_ea = salloc(168 * nDirCap);
    const uint32_t fixedSet_ea= salloc(16);
    const uint32_t statGet_ea = salloc(1648);
    const uint32_t statSet_ea = salloc(16);
    const uint32_t fileGet_ea = salloc(96);
    const uint32_t fileSet_ea = salloc(64);

    const char* save_root = "./gamedata/dev_hdd0/home/00000001/savedata";
    uint32_t nDirs = 0;
    {
        std::error_code ec;
        std::filesystem::create_directories(save_root, ec);
        for (std::filesystem::directory_iterator it(save_root, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            std::string name = it->path().filename().string();
            if (strncmp(name.c_str(), prefix, strlen(prefix)) != 0) continue;
            if (nDirs < nDirCap) {
                uint32_t de = dirList_ea + 168 * nDirs;
                size_t len = name.size() < 31 ? name.size() : 31;
                memcpy(vm_base + de, name.c_str(), len);
            }
            nDirs++;
        }
    }
    vm_write32(listGet_ea + 0, nDirs);
    vm_write32(listGet_ea + 4, nDirs < nDirCap ? nDirs : nDirCap);
    vm_write32(listGet_ea + 8, dirList_ea);

    auto cb_init = [&](void) {
        vm_write32(cb_ea + 0x00, 0);
        vm_write32(cb_ea + 0x04, 0);
        vm_write32(cb_ea + 0x08, 0);
        vm_write32(cb_ea + 0x0C, 0);
        vm_write32(cb_ea + 0x10, userdata);
    };
    auto map_cb_error = [](int32_t r) -> int32_t {
        return (r == -4) ? CELL_SAVEDATA_ERROR_NODATA : CELL_SAVEDATA_ERROR_CBRESULT;
    };

    cb_init();
    uc3_call_guest4(funcFixed, cb_ea, listGet_ea, fixedSet_ea, 0);
    int32_t r = (int32_t)vm_read32(cb_ea + 0x00);
    fprintf(stderr, "[savedata] AutoSave funcFixed -> result=%d dirName=0x%08X\n",
            r, vm_read32(fixedSet_ea + 0));
    if (r < 0) { RET(map_cb_error(r)); return; }

    char dirName[64] = {0};
    { uint32_t p = vm_read32(fixedSet_ea + 0);
      if (p) for (int i = 0; i < 63; i++) { char ch = (char)vm_read8(p + i); dirName[i] = ch; if (!ch) break; } }
    if (!dirName[0]) { RET(CELL_SAVEDATA_ERROR_PARAM); return; }

    char save_path[512];
    snprintf(save_path, sizeof(save_path), "%s/%s", save_root, dirName);
    std::error_code ec;
    const bool existed = std::filesystem::is_directory(save_path, ec);
    std::filesystem::create_directories(save_path, ec);

    vm_write32(statGet_ea + 0,    1024 * 1024);
    vm_write32(statGet_ea + 4,    existed ? 0 : 1);
    { size_t len = strnlen(dirName, 31);
      memcpy(vm_base + statGet_ea + 32, dirName, len); }
    vm_write32(statGet_ea + 1636, salloc(108));   /* empty fileList */
    cb_init();
    uc3_call_guest4(funcStat, cb_ea, statGet_ea, statSet_ea, 0);
    r = (int32_t)vm_read32(cb_ea + 0x00);
    fprintf(stderr, "[savedata] AutoSave funcStat(new=%d) -> result=%d setParam=0x%08X\n",
            !existed, r, vm_read32(statSet_ea + 0));
    if (r < 0) { RET(map_cb_error(r)); return; }

    /* Persist setParam (title block) raw — enough for our AutoLoad to see the
     * dir as valid on later boots (we don't parse PARAM.SFO contents). */
    { uint32_t sp = vm_read32(statSet_ea + 0);
      if (sp) {
          char ppath[768]; snprintf(ppath, sizeof(ppath), "%s/PARAM.RAW", save_path);
          FILE* fp = fopen(ppath, "wb");
          if (fp) { fwrite(vm_base + sp, 1, 1552, fp); fclose(fp); }
      } }

    if (funcFile && r == 0) {
        for (int iter = 0; iter < 64; iter++) {
            memset(vm_base + fileGet_ea, 0, 96);
            memset(vm_base + fileSet_ea, 0, 64);
            cb_init();
            uc3_call_guest4(funcFile, cb_ea, fileGet_ea, fileSet_ea, 0);
            r = (int32_t)vm_read32(cb_ea + 0x00);
            uint32_t fn_ea  = vm_read32(fileSet_ea + 0x00);
            uint32_t fbuf   = vm_read32(fileSet_ea + 0x24);
            uint32_t fsize  = vm_read32(fileSet_ea + 0x1C);
            uint32_t foff   = vm_read32(fileSet_ea + 0x18);
            if (fn_ea && fbuf) {
                char fn[128] = {0};
                for (int i = 0; i < 127; i++) { char ch = (char)vm_read8(fn_ea + i); fn[i] = ch; if (!ch) break; }
                char fpath[768];
                snprintf(fpath, sizeof(fpath), "%s/%s", save_path, fn);
                uint32_t wrote = 0;
                FILE* fp = fopen(fpath, foff ? "r+b" : "wb");
                if (!fp && foff) fp = fopen(fpath, "wb");
                if (fp) {
                    if (foff) fseek(fp, (long)foff, SEEK_SET);
                    wrote = (uint32_t)fwrite(vm_base + fbuf, 1, fsize, fp);
                    fclose(fp);
                }
                vm_write32(fileGet_ea + 0, wrote);
                fprintf(stderr, "[savedata] AutoSave funcFile wrote '%s' off=%u size=%u -> %u (result=%d)\n",
                        fn, foff, fsize, wrote, r);
            }
            if (r != 0) break;
            if (!fn_ea || !fbuf) break;
        }
        if (r < 0) { RET(map_cb_error(r)); return; }
    }

    fprintf(stderr, "[savedata] ListAutoSave complete (dir='%s')\n", dirName);
    RET(0);
}

static void br_cellGcmSetGraphicsHandler(ppu_context* ctx) {
    g_gcm.graphics_handler = A0;
    RET(0);
}

/* Frontend flip bridge. UC3's frontend does NOT call the _cellGcmSetFlipCommand
 * HLE NID — it submits the flip as a GCM method into its aux ring (0x3074F000/40,
 * drained by uc3_drain_custom_gcm). That drain routes draw/state methods to the
 * D3D12 backend but never triggers a PRESENT, so the rendered frontend frame is
 * never shown (the historic "0 SetFlipCommand / black screen"). This hook is
 * invoked from the game's sole flip site (func_00A93CA4, fade/frame tick
 * func_0074B7FC) and presents the accumulated frame — the same present the HLE
 * flip path uses. Default-on; UC3_NO_FRONTEND_FLIP disables for A/B. */
/* Stash/flush pair for the frontend-flip patch (patches.json:frontend-flip-hook):
 * the `before` patch captures r4(bufferId) before the func_00D54D24 call (r4 is
 * volatile and clobbered by it); the `after` patch presents with the stashed id.
 * Replaces the old hand-edit of the recompiled flip site (now re-lift-safe). */
extern "C" void uc3_frontend_flip(uint32_t);   /* forward decl (portee globale) */
static thread_local uint32_t g_uc3_flip_buf = 0;
extern "C" void uc3_stash_flip_buf(uint32_t b) { g_uc3_flip_buf = b; }
extern "C" void uc3_flush_flip(void) { uc3_frontend_flip(g_uc3_flip_buf); }

extern "C" void uc3_frontend_flip(uint32_t bufferId) {
    if (getenv("UC3_NO_FRONTEND_FLIP")) return;
    /* make sure any pending aux-ring methods are drained into the backend first */
    if (g_custom_gcm_pending.load()) return;   /* a drain is already in flight */
    uint32_t ctx = 0x3074F000u;
    uc3_drain_custom_gcm(ctx);
    uc3_drain_custom_gcm(0x3074F040u);
    rsx_backend* backend = rsx_get_backend();
    if (backend && backend->present &&
        getenv("UC3_INJECT_GEOM") == nullptr && getenv("UC3_RRC_FIFO") == nullptr) {
        backend->present(backend->userdata, bufferId & 0xFF);
        static unsigned long long fn = 0;
        if ((++fn % 60ull) == 1ull)
            fprintf(stderr, "[frontend-flip] present #%llu buffer=%u\n", fn, bufferId & 0xFF);
    }
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
    /* When the Étape-3 geometry injection owns the frame, suppress the game's
     * (blank, idle) present so it doesn't overwrite the injected mesh. */
    if (backend && backend->present && getenv("UC3_INJECT_GEOM") == nullptr &&
        getenv("UC3_RRC_FIFO") == nullptr)
        backend->present(backend->userdata, A0);
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

/* 8 sur PS3 reelle, mais notre init media peut etre re-invoquee (voie-b MODE2)
 * sans fermer les ports -> epuisement -> cellAudioPortOpen 0x80310705 ->
 * func_00D3B850=-5 -> completion media jamais posee -> main bloque. Marge a 32
 * pour que les re-invocations aient de la place (chaine media/menu debloquee). */
static Uc3AudioPort g_audio_ports[64];
/* Cache buffer/index par slot (survit aux reset) : bump_alloc est monotone (ne
 * libere jamais), donc re-ouvrir des ports (re-invocation media voie-b) fuit la
 * memoire -> 0x8031070B -> func_00D3B850=-5. On alloue UNE fois par slot (taille
 * max 8ch*32blocks*256*4=256KB) et on reutilise. */
static uint32_t g_audio_buf_cache[64] = {0};
static uint32_t g_audio_idx_cache[64] = {0};

/* --- audio-hardware tick (read_index advance) ---------------------------- *
 * Sur PS3 reelle, le materiel audio consomme un bloc de 256 samples toutes
 * les ~5.3 ms et incremente *readIndexAddr (index de bloc courant, 0..nBlock-1).
 * Les boucles de synchro audio du jeu (ex. func_00D35788 chemin B) attendent
 * cet avancement pour progresser. Notre HLE ouvrait le port mais n'avancait
 * jamais l'index -> le compteur de completion aval ne bougeait pas -> timeout
 * -> media completion (+0x5) jamais posee -> menu bloque. Ce thread emule la
 * consommation materielle, comme le fait RPCS3. (HLE cote runtime, conforme.) */
static std::atomic<bool> g_audio_tick_started{false};

/* borne de validite d'un pointeur guest (data ~0x01xxxxxx, heap ~0x3xxxxxxx) */
static inline bool uc3_guest_ptr_ok(uint32_t ea) {
    return ea >= 0x00010000u && ea < 0x40000000u;
}

/* --- BRB/SCREAM primary-output SPU-job-completion consumer ---------------- *
 * func_00D35788 = brb_StartSession() (middleware audio Naughty Dog SCREAM/BRB).
 * Il enfile des commandes DSP dans un ring de job SPU (s_pPrimaryOutput =
 * *(0x01448C50)) : le producteur PPU incremente submit=*(r9+0x10) (r9=*(r7+0x10)),
 * et attend que le CONSOMMATEUR (le programme SPU 'scream_plugin_pack' dispatche
 * par cellSpursCreateTask) DMA-ecrive done=*(r10+0x24) (r10=*(r7+0x14)) jusqu'a
 * l'egalite. ps3recomp n'execute pas ce SPU (docs/SPU_FALLBACK.md) -> done reste
 * 0 -> brb_StartSession timeout (~10s) -> -1 -> chaine media echoue -> +0x5 media
 * jamais pose -> main parke -> pas de menu.
 * FIX CONFORME (runtime HLE, pas recompiled/): on emule le SPU drainant le ring.
 * Fidele (regle #6) car notre decode audio (cellAdec/Atrac3p) sort deja du
 * silence -> le filtergraph SCREAM ne produirait rien de plus a consommer. Garde:
 * on n'avance done QUE quand submit>done et que tous les pointeurs sont valides,
 * donc on ne peut jamais devancer un producteur reel ni ecrire hors-VM. */
static void uc3_brb_drain_primary_output() {
    uint32_t holder = 0x01448C50u; /* r27 = *(TOC-0x7FFC), global tenant le ring */
    uint32_t r7 = vm_read32(holder);              /* s_pPrimaryOutput */
    if (!uc3_guest_ptr_ok(r7)) return;
    uint32_t r9 = vm_read32(r7 + 0x10);           /* endpoint producteur */
    uint32_t r10 = vm_read32(r7 + 0x14);          /* endpoint consommateur */
    if (!uc3_guest_ptr_ok(r9) || !uc3_guest_ptr_ok(r10)) return;
    uint32_t submit = vm_read32(r9 + 0x10);       /* TARGET (compte soumis) */
    uint32_t done = vm_read32(r10 + 0x24);        /* CURRENT (compte complete) */
    if ((int32_t)(submit - done) > 0) {           /* consommateur en retard */
        static int _dbg = 0;
        if (_dbg < 8) { ++_dbg; fprintf(stderr, "[brb-consumer] ring=%08X submit=%u done=%u -> done:=%u\n", r7, submit, done, submit); }
        vm_write32(r10 + 0x24, submit);
    }
}

static void uc3_audio_tick_thread() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(5333)); /* 256/48000 s */
        /* materiel audio: avance le read_index de chaque port ouvert */
        for (uint32_t i = 0; i < 64; ++i) {
            Uc3AudioPort& p = g_audio_ports[i];
            if (p.open && p.started && p.read_index && p.blocks) {
                uint32_t idx = vm_read32(p.read_index);
                idx = (idx + 1u) % p.blocks;
                vm_write32(p.read_index, idx);
            }
        }
        /* consommateur SPU du ring BRB (brb_StartSession) */
        uc3_brb_drain_primary_output();
        /* UC3_S6MON : moniteur READ-ONLY du gate render state=6 (fiable, hors
         * recompiled). mgr = *(*(TOC-0x7220)-0x7F58); mgr+0x14 = byte busy que
         * func_007AD34C retourne (decide boucle-FIOS vs avance dans le handler
         * state=6 @0x008B68EC). obj render misc-fx = 0x31473E20. */
        if (getenv("UC3_S6MON") && g_canonical_toc != 0) {
            static int _t = 0;
            static bool _jt = false;
            if (!_jt) { _jt = true;
                uint32_t r30j = vm_read32(g_canonical_toc - 0x7220);
                uint32_t tbl = (r30j >= 0x10000u) ? vm_read32(r30j - 0x7F30) : 0; /* *(r30-0x7F30) = jump table base */
                if (tbl >= 0x10000u) {
                    for (uint32_t st = 0; st <= 12; ++st) {
                        int32_t off = (int32_t)vm_read32(tbl + st*4);
                        uint32_t hdl = (uint32_t)((int32_t)tbl + off);
                        fprintf(stderr, "[jumptbl] state=%u -> handler=0x%08X\n", st, hdl);
                    }
                }
            }
            if ((++_t % 90) == 1) { /* ~0.5s */
                /* compte les records terminaux (record+0x18==0xB) du frontend mgr */
                uint32_t fr30 = vm_read32(g_canonical_toc - 0x7218);
                uint32_t fr9 = (fr30 >= 0x10000u) ? vm_read32(fr30 - 0x7FBC) : 0;
                uint32_t fmgr = (fr9 >= 0x10000u) ? vm_read32(fr9 + 0x1C) : 0;
                uint32_t farr = (fmgr >= 0x10000u) ? vm_read32(fmgr + 0x18) : 0;
                if (farr >= 0x10000u) {
                    int term = 0, os8 = 0, tot = 0, oss[13] = {0};
                    for (int i = 0; i < 250; ++i) {
                        uint32_t rec = farr + i * 0x2F8;
                        uint32_t rs = vm_read32(rec + 0x18);
                        uint32_t obj = vm_read32(rec + 0x20);
                        if (obj < 0x10000u) continue;
                        tot++;
                        if (rs == 0xBu) term++;
                        uint32_t os = vm_read32(obj + 0x20);
                        if (os == 8u) os8++;
                        if (os <= 12u) oss[os]++;
                    }
                    fprintf(stderr, "[recmon] fmgr=%08X records=%d terminal(0xB)=%d objstate8=%d | os-dist: 6=%d 7=%d 8=%d 9=%d 10=%d 11=%d 12=%d\n",
                            fmgr, tot, term, os8, oss[6], oss[7], oss[8], oss[9], oss[10], oss[11], oss[12]);
                }
                uint32_t r30 = vm_read32(g_canonical_toc - 0x7220);
                uint32_t mgr = (r30 >= 0x10000u) ? vm_read32(r30 - 0x7F58) : 0;
                /* flags gatant le handler state-8 func_008B6B54 */
                if (r30 >= 0x10000u) {
                    uint32_t g20 = vm_read32(r30 - 0x7F20), g08 = vm_read32(r30 - 0x7F08);
                    fprintf(stderr, "[s8flags] *(r30-0x7F20)=%08X byte=%u | *(r30-0x7F08)=%08X byte=%u\n",
                            g20, (g20>=0x10000u)?(unsigned)vm_read8(g20):999u,
                            g08, (g08>=0x10000u)?(unsigned)vm_read8(g08):999u);
                    if (getenv("UC3_S8FORCE") && g08 >= 0x10000u && vm_read8(g08) == 0) {
                        vm_write8(g08, 1);
                        static int _f=0; if(_f<3){_f++; fprintf(stderr,"[s8force] *(r30-0x7F08) byte 0->1 (debloque bloc soumission state-8)\n");}
                    }
                }
                uint32_t o = 0x31473E20u;
                uint32_t ch = vm_read32(o + 0x80);
                uint32_t h = (ch >= 0x10000u) ? vm_read32(ch + 0x22C) : 0; /* streamer handle */
                fprintf(stderr, "[s6mon] busy=%u obj-state=%u chC=%08X | streamer=%08X h+0x18(ss)=%u h+0x0=%08X h+0x268(rd)=%08X h+0x2B0(tot)=%u h+0x948=%u h+0x8=%08X\n",
                        (mgr >= 0x10000u) ? (unsigned)vm_read8(mgr + 0x14) : 999u,
                        (unsigned)vm_read32(o + 0x20),
                        (ch >= 0x10000u) ? (unsigned)vm_read32(ch + 0xC) : 0u,
                        h,
                        (h >= 0x10000u) ? (unsigned)vm_read32(h + 0x18) : 999u,
                        (h >= 0x10000u) ? (unsigned)vm_read32(h + 0x0) : 0u,
                        (h >= 0x10000u) ? (unsigned)vm_read32(h + 0x268) : 0u,
                        (h >= 0x10000u) ? (unsigned)vm_read32(h + 0x2B0) : 0u,
                        (h >= 0x10000u) ? (unsigned)vm_read32(h + 0x948) : 0u,
                        (h >= 0x10000u) ? (unsigned)vm_read32(h + 0x8) : 0u);
            }
            /* UC3_S6FORCE : diagnostic — efface le busy media_mgr+0x14 pour voir
             * si l'avance state=6 se debloque (regle #6 : force diagnostique). */
            if (getenv("UC3_S6FORCE")) {
                uint32_t r30 = vm_read32(g_canonical_toc - 0x7220);
                uint32_t mgr = (r30 >= 0x10000u) ? vm_read32(r30 - 0x7F58) : 0;
                if (mgr >= 0x10000u && vm_read8(mgr + 0x14) != 0) {
                    vm_write8(mgr + 0x14, 0);
                    static int _f = 0; if (_f < 4) { ++_f; fprintf(stderr, "[s6force] mgr+0x14 busy -> 0\n"); }
                }
            }
            /* UC3_S6ADV : diagnostic — force l'objet render misc-fx state 6->7 pour
             * voir si l'avance du render dessine du contenu ou OOB (regle #6). */
            if (getenv("UC3_S6ADV")) {
                uint32_t o = 0x31473E20u;
                if (vm_read32(o + 0x20) == 6u) {
                    vm_write32(o + 0x20, 7u);
                    static int _a = 0; if (_a < 6) { ++_a; fprintf(stderr, "[s6adv] obj=%08X state 6 -> 7\n", o); }
                }
            }
        }
    }
}

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
    if (getenv("UC3_MEDIAGATE")) { static int _o=0; ++_o; if(_o<=40) fprintf(stderr, "[portopen] appel #%d lr=0x%08X ch=%u blk=%u\n", _o, (uint32_t)ctx->lr, param?(uint32_t)vm_read64(param):0, param?(uint32_t)vm_read64(param+8):0); }
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

    uint32_t port_num = 64;
    for (uint32_t i = 0; i < 64; ++i) {
        if (!g_audio_ports[i].open) { port_num = i; break; }
    }
    if (port_num == 64) { RET(0x80310705); return; }

    Uc3AudioPort& port = g_audio_ports[port_num];
    port.open = true;
    port.channels = channels;
    port.blocks = blocks;
    port.size = channels * blocks * 256u * sizeof(float);
    /* alloue une fois par slot a la taille MAX (8*32*256*4), reutilise ensuite */
    if (!g_audio_buf_cache[port_num]) g_audio_buf_cache[port_num] = bump_alloc(8u*32u*256u*sizeof(float), 128);
    if (!g_audio_idx_cache[port_num]) g_audio_idx_cache[port_num] = bump_alloc(16, 16);
    port.buffer = g_audio_buf_cache[port_num];
    port.read_index = g_audio_idx_cache[port_num];
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
    if (port_num >= 64 || !config || !g_audio_ports[port_num].open) {
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
    if (port_num >= 64 || !g_audio_ports[port_num].open) {
        RET(0x80310707);
        return;
    }
    g_audio_ports[port_num].started = true;
    /* demarre le tick materiel audio a la 1re activation de port */
    if (!g_audio_tick_started.exchange(true)) {
        std::thread(uc3_audio_tick_thread).detach();
        fprintf(stderr, "[cellAudio] audio-tick thread demarre (read_index avance)\n");
    }
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
    /* UC3_PRESS_START=<sec>: pulse the START button (300ms on / 700ms off)
     * starting <sec> seconds after launch. Hypothesis (STATUS 2026-07-10):
     * flag2@0x012FD5F9=0x70 is the title screen's response to the player
     * pressing START — every post-Init loop (fade, level-load, family-3
     * save op, dispatcher func_008A3D4C) merely WAITS for it. This is real
     * input through the real cellPad path, not a state force. */
    if (const char* ps = getenv("UC3_PRESS_START")) {
        static ULONGLONG t0 = GetTickCount64();
        ULONGLONG el = GetTickCount64() - t0;
        int delay = atoi(ps); if (delay <= 0) delay = 60;
        if (port == 0 && el > (ULONGLONG)delay * 1000ull) {
            if (((el / 100ull) % 10ull) < 3ull) {
                if (pd.len < 8) pd.len = 8;
                pd.button[2] |= 0x8;   /* CELL_PAD_CTRL_START (DIGITAL1) */
            }
            static int announced = 0;
            if (!announced) { announced = 1;
                fprintf(stderr, "[pad] UC3_PRESS_START pulsing from t=%ds\n", delay); }
        }
    }
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

        /* TICKER EPILOGUE SPURS (independant du ring-monitor, que les
         * job-guard waits bloquent pendant que D51D60 se fige — fw4.log).
         * Toutes les 100 ms, publie les epilogues quiescents manquants :
         * ring terminal (prod=0,cons!=0) ou fenetre-vide (fantome wid2).
         * Watermark = cons uniquement (aucune completion inventee). */
        if (getenv("UC3_RING_MON") != nullptr) {
            std::thread([]{
                while (vm_base == nullptr)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                int logged = 0, dbg = 0, tick = 0;
                fprintf(stderr, "[wkl-tick] arme\n");
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    uint32_t spurs = 0;
                    { std::lock_guard<std::mutex> lk(g_spurs_state_mutex);
                      if (!g_spurs_states.empty())
                          spurs = g_spurs_states[0].address; }
                    if (!spurs) continue;
                    if (++tick % 50 == 0 && dbg < 20) { ++dbg;
                        uint32_t r2 = vm_read32(spurs + 0xB00 + 2*0x20 + 0x0C);
                        if (r2 >= 0x10000u)
                            fprintf(stderr, "[wkl-tick-dbg] wid2 ring=%08X "
                                    "cont=%u c0=%08X c1=%08X s0w0=%08X\n", r2,
                                    vm_read8(spurs + 0x22),
                                    vm_read32(r2 + 0x40), vm_read32(r2 + 0x50),
                                    vm_read32(vm_read32(r2 + 0x30)));
                    }
                    for (uint32_t wid = 0; wid < 16; ++wid) {
                        uint32_t ring = vm_read32(spurs + 0xB00 + wid * 0x20 + 0x0C);
                        if (ring < 0x10000u) continue;
                        if (vm_read8(spurs + 0x20 + wid) != 0) continue;
                        uint32_t c0 = vm_read32(ring + 0x40);
                        uint16_t prod = (uint16_t)(c0 >> 16), cons = (uint16_t)c0;
                        /* BARRIERE type 0x50 (fw6: s0w0=0x00500001) : item de
                         * sync que le policy reel consomme en publiant le
                         * curseur +0x4C. Notre executeur ne traite que 0x70
                         * (jobs Edge). Consommer UN item 0x50 par tick :
                         * avancer cons, vider le slot, publier les curseurs
                         * actifs au nouveau cons. */
                        if (prod != cons && prod != 0) {
                            uint32_t s0b = vm_read32(ring + 0x30);
                            if (s0b >= 0x10000u) {
                                for (uint32_t it = 0; it < 4; ++it) {
                                    uint32_t w0 = vm_read32(s0b + it * 8);
                                    if ((w0 >> 16) != 0x0050u) continue;
                                    uint16_t nc = (uint16_t)(cons + 1);
                                    vm_write32(s0b + it * 8, 0);
                                    vm_write32(s0b + it * 8 + 4, 0);
                                    vm_write32(ring + 0x40,
                                               ((uint32_t)prod << 16) | nc);
                                    for (uint32_t off = 0x42; off <= 0x4E;
                                         off += 2) {
                                        uint16_t cur = vm_read16(ring + off);
                                        if (cur != 0xFFFFu && cur < nc)
                                            vm_write16(ring + off, nc);
                                    }
                                    if (logged < 40) { ++logged;
                                        fprintf(stderr, "[wkl-tick-bar] wid=%u"
                                                " barriere 0x50 slot=%u"
                                                " consommee cons=%u->%u\n",
                                                wid, it, cons, nc);
                                    }
                                    break;
                                }
                            }
                            continue;
                        }
                        if (cons == 0 || prod == cons) continue;
                        bool quiescent = (prod == 0);
                        if (!quiescent) {
                            uint32_t c1 = vm_read32(ring + 0x50);
                            uint32_t s0 = vm_read32(ring + 0x30);
                            if ((c1 >> 16) != (c1 & 0xFFFFu) || s0 < 0x10000u)
                                continue;
                            bool empty = true;
                            for (uint32_t it = 0; it < 4 && empty; ++it)
                                if (vm_read32(s0 + it * 8) ||
                                    vm_read32(s0 + it * 8 + 4)) empty = false;
                            if (!empty) continue;
                        }
                        bool wm = false;
                        for (uint32_t off = 0x42; off <= 0x4E; off += 2) {
                            uint16_t c = vm_read16(ring + off);
                            if (c != 0xFFFFu && c < cons) {
                                vm_write16(ring + off, cons); wm = true;
                            }
                        }
                        if (wm && logged < 40) { ++logged;
                            fprintf(stderr, "[wkl-tick-wm] wid=%u ring=0x%08X "
                                    "%s watermark=%u (c0=0x%08X)\n", wid, ring,
                                    quiescent ? "quiescent" : "fenetre-vide",
                                    cons, c0);
                        }
                    }
                }
            }).detach();
        }

        /* Echantillonneur du staging frontend (UC3_STG_SAMPLER), demarre DES
         * LE PROCESS (le round 1 du chargement 10MB se joue tres tot — avant
         * le ring-monitor et avant le 1er cellSpursEventFlagInitialize, cf.
         * ab4.log lignes 1108 vs 2276). Polle manager 0x0134F848 : staged
         * (+948), prog(+94C), 940/944, cnt(+83C), child(+7F0)->reste(+278)
         * toutes les 250 us; logue chaque CHANGEMENT de staged/prog avec
         * horodatage. Decouverte deja acquise: apres le gel a prog=509808,
         * un RESET remet prog=0 avec budget f940=2MB/f944=245456 et le
         * round 2 ne produit jamais. Objectif: chronologie complete round 1
         * -> reset -> silence round 2, correlable aux autres logs. */
        if (getenv("UC3_STG_SAMPLER") != nullptr) {
            std::thread([]{
                const uint32_t MGR = 0x0134F848u;
                /* attendre la VM guest (mise en place au boot) */
                while (vm_base == nullptr)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                auto t0 = std::chrono::steady_clock::now();
                uint32_t p_staged = 0xFFFFFFFFu, p_prog = 0xFFFFFFFFu;
                int lines = 0;
                while (lines < 400) {
                    uint32_t staged = vm_read32(MGR + 0x948);
                    uint32_t prog   = vm_read32(MGR + 0x94C);
                    if (staged != p_staged || prog != p_prog) {
                        uint32_t f940 = vm_read32(MGR + 0x940);
                        uint32_t f944 = vm_read32(MGR + 0x944);
                        uint32_t cnt  = vm_read32(MGR + 0x83C);
                        uint32_t ch   = vm_read32(MGR + 0x7F0);
                        uint32_t rem  = ch >= 0x10000u ? vm_read32(ch + 0x278) : 0;
                        long ms = (long)std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
                        fprintf(stderr, "[stg-smp] t=%ldms staged=%u prog=%u "
                                "f940=%u f944=%u cnt=%u reste=%u\n",
                                ms, staged, prog, f940, f944, cnt, rem);
                        p_staged = staged; p_prog = prog; ++lines;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(250));
                }
                fprintf(stderr, "[stg-smp] cap atteint (400 transitions)\n");
            }).detach();
        }

        /* UC3_FE_STATE : sonde READ-ONLY de l'etat-machine frontend live
         * (func_0089F418 dispatch sur *(*(obj+0x7F0)+0x18)-2, index 0..0xA).
         * obj = MGR 0x0134F848 (meme structure, meme offset +0x7F0). Logue
         * l'etat courant + changements toutes les 250ms. Aucune ecriture
         * (conforme regle #6) : identifier la valeur d'etat bloquee et son
         * evolution pour trouver l'evenement que l'etat N attend. */
        if (getenv("UC3_FE_STATE") != nullptr) {
            std::thread([]{
                while (vm_base == nullptr || g_canonical_toc == 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                auto t0 = std::chrono::steady_clock::now();
                /* Resolution du VRAI manager frontend (comme le pump-keeper) :
                 * mgr = *(*(*(TOC-0x7218)-0x7FBC)+0x1C). READ-ONLY, aucun kick. */
                uint32_t p_sig = 0xFFFFFFFFu;
                int lines = 0, ticks = 0;
                while (lines < 200 && ticks < 1200) {
                    ++ticks;
                    uint32_t r30 = vm_read32(g_canonical_toc - 0x7218);
                    uint32_t r9  = (r30 >= 0x10000u) ? vm_read32(r30 - 0x7FBC) : 0;
                    uint32_t mgr = (r9  >= 0x10000u) ? vm_read32(r9 + 0x1C) : 0;
                    if (mgr < 0x10000u) { std::this_thread::sleep_for(std::chrono::milliseconds(250)); continue; }
                    uint32_t cur = vm_read32(mgr + 0x7F0);
                    uint32_t arr = vm_read32(mgr + 0x18);
                    uint32_t io958 = vm_read32(mgr + 0x958);  /* io-in-flight : pompe SKIP si !=0 */
                    uint32_t r948  = vm_read32(mgr + 0x948);  /* remaining */
                    uint8_t  flag1 = vm_read8(0x011CE590u);   /* asset-load-complete flag (poll func_000428A0) */
                    /* [mediacfg] READ-ONLY: objet media 0x01351A34 (+0x4 stamp,
                     * +0x5 achevement) + globals config media ecrits par
                     * loc_00D32800 (base = *(TOC-0x5110), offs -0x7BF4/-0x7C0C/
                     * -0x7C24/-0x7C50/-0x7E90/-0x7E20). Tous a 0 => contenu media
                     * jamais charge (config vide) => func_00D333E0 reste a l'etat
                     * 8. Confirme le gate sans toucher au recompile (runtime). */
                    if (getenv("UC3_MEDIAGATE") && (ticks % 20) == 3) {
                        uint32_t md = vm_read32(g_canonical_toc - 0x5110u);
                        uint32_t g_ch=0,g_bl=0,g_a=0,g_b=0,g_c=0,g_d=0;
                        if (md >= 0x10000u) {
                            g_ch = vm_read32(md - 0x7BF4u); g_bl = vm_read32(md - 0x7C0Cu);
                            g_a  = vm_read32(md - 0x7C24u); g_b  = vm_read32(md - 0x7C50u);
                            g_c  = vm_read32(md - 0x7E90u); g_d  = vm_read32(md - 0x7E20u);
                        }
                        /* deref des pointeurs config -> les VRAIES valeurs (channels/
                         * blocks/etc). 0 partout = contenu media pas charge. */
                        auto DV=[&](uint32_t p){ return (p>=0x10000u&&p<0x40000000u)?vm_read32(p):0xFFFFFFFFu; };
                        uint32_t etat = 0; { uint32_t sp=vm_read32(md - 0x7FECu); if(sp>=0x10000u) etat=vm_read32(sp); }
                        fprintf(stderr, "[mediacfg] media@1351A34 vt=0x%08X +4=0x%02X +5=0x%02X etatD333E0=%u | "
                                "cfgVAL(*BF4/*C0C/*C24/*C50/*E90/*E20)=%u/%u/%u/%u/%u/%u\n",
                                vm_read32(0x01351A34u), vm_read8(0x01351A38u), vm_read8(0x01351A39u), etat,
                                DV(g_ch), DV(g_bl), DV(g_a), DV(g_b), DV(g_c), DV(g_d));
                    }
                    uint8_t  flag2 = vm_read8(0x012FD5F9u);   /* transition flag */
                    /* Flag TRANSITION-ENABLE lu par le handler state-8 func_008B6B54 :
                     * s8=*(*(*(TOC-0x7220)-0x7F20)+0). Si 0 -> objets state-8
                     * font early-out (func_008B6F5C) = LE GATE de transition menu. */
                    uint32_t s8base = vm_read32(g_canonical_toc - 0x7220u);
                    uint32_t s8ptr  = (s8base >= 0x10000u) ? vm_read32(s8base - 0x7F20u) : 0;
                    uint32_t s8flag = (s8ptr >= 0x10000u) ? vm_read8(s8ptr + 0x0u) : 0xFFu;
                    /* UC3_FIOS_DUMP : caracteriser l'op media que le FIOS scheduler
                     * sert (le main y est retenu, cf. dump registres 0x3115BFA8).
                     * READ-ONLY. media obj 0x01351A34 (vt attendu 0x3110E728), FIOS
                     * schd 0x31154EA8 (magie "FIOS" @+0x4). Distingue: op dispatchee
                     * a un mediathread coince VS op jamais emise. Throttle ~2.4s. */
                    if (getenv("UC3_FIOS_DUMP") && (ticks % 8) == 1) {
                        const uint32_t MEDIA = 0x01351A34u, SCHD = 0x31154EA8u;
                        fprintf(stderr, "[fios] media@0x01351A34 vt=0x%08X:", vm_read32(MEDIA));
                        for (int k = 1; k < 24; ++k) fprintf(stderr, " +0x%X=0x%08X", k*4, vm_read32(MEDIA + k*4));
                        fprintf(stderr, "\n");
                        if (vm_read32(SCHD + 4) == 0x46494F53u) { /* "FIOS" */
                            fprintf(stderr, "[fios] schd@0x31154EA8:");
                            for (int k = 0; k < 20; ++k) fprintf(stderr, " +0x%X=0x%08X", k*4, vm_read32(SCHD + k*4));
                            fprintf(stderr, "\n");
                        } else {
                            fprintf(stderr, "[fios] schd@0x31154EA8 no-FIOS-magic (+0x4=0x%08X)\n", vm_read32(SCHD + 4));
                        }
                    }
                    /* distribution des etats obj (obj+0x20) sur les 250 records */
                    uint32_t sc[13] = {}; uint32_t nrec = 0; uint32_t term = 0;
                    if (arr >= 0x10000u) {
                        for (int i = 0; i < 250; ++i) {
                            uint32_t rec = arr + i * 0x2F8u;
                            uint32_t rst = vm_read32(rec + 0x18);
                            if (rst == 0xBu) ++term;
                            uint32_t obj = vm_read32(rec + 0x20);
                            if (obj < 0x10000u) continue;
                            ++nrec;
                            uint32_t st = vm_read32(obj + 0x20);
                            if (st < 13u) ++sc[st];
                        }
                    }
                    uint32_t sig = cur ^ (nrec<<8) ^ (term<<16);
                    for (int k=0;k<13;++k) sig ^= sc[k] << (k%5);
                    bool changed = (sig != p_sig);
                    if (changed || (ticks % 16) == 0) {
                        long ms = (long)std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
                        fprintf(stderr, "[fe-state] t=%ldms mgr=0x%08X cur=0x%08X nrec=%u term(0xB)=%u "
                                "flag1=%u flag2=%u s8obj=0x%08X s8flag=%u io958=0x%08X r948=%u st[0..12]=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u%s\n",
                                ms, mgr, cur, nrec, term, flag1, flag2, s8ptr, s8flag, io958, r948,
                                sc[0],sc[1],sc[2],sc[3],sc[4],sc[5],sc[6],sc[7],sc[8],sc[9],sc[10],sc[11],sc[12],
                                changed ? " <CHANGE>" : "");
                        /* [wkl] etat des 4 sync-objects SPURS statiques (D51D60)
                         * + cloture media: table ops @0x01449EC4, achevement
                         * +0x5 @0x01351A39. Capture le spin TERMINAL que le
                         * probe d'entree d51 ne voit pas (main coince DANS la
                         * boucle interne). Read-only. */
                        if (getenv("UC3_D51_PROBE")) {
                            static const uint32_t sos[4] = {0x012FC9C4u,0x012FC788u,0x012FC75Cu,0x012FC730u};
                            for (int siy = 0; siy < 4; ++siy) {
                                uint32_t so = sos[siy];
                                uint32_t base = vm_read32(so);
                                if (base < 0x10000u) continue;
                                for (uint32_t wid = 0; wid < 3; ++wid) {
                                    uint32_t e = base + wid*16u;
                                    uint16_t prod = vm_read16(e + 0x40u);
                                    uint16_t c0 = vm_read16(e + 0x42u);
                                    if (prod == 0 && c0 == 0) continue;    /* silencieux si idle */
                                    uint32_t rlp = vm_read32(base + wid*4u + 0x30u);
                                    uint32_t slot = (rlp >= 0x10000u) ? rlp + (uint32_t)prod*8u : 0u;
                                    fprintf(stderr, "[wkl] so=0x%08X wid=%u base=0x%08X prod=0x%04X "
                                            "cur=%04X/%04X/%04X ready=0x%08X rdy[prod]=0x%04X\n",
                                            so, wid, base, prod, c0, vm_read16(e+0x44u), vm_read16(e+0x46u),
                                            rlp, slot ? vm_read16(slot + 2u) : 0);
                                    /* STRUCTURE ready-list: slot[prod] (8o) + sous-liste
                                     * a slot+0xA (le walk D51D60 lit +0,+8,... jusqu'a 0). */
                                    if (slot) {
                                        fprintf(stderr, "[wkl-rl]   slot@0x%08X: %08X %08X | sublist@+0xA:",
                                                slot, vm_read32(slot), vm_read32(slot+4u));
                                        for (uint32_t q = 0; q < 6; ++q)
                                            fprintf(stderr, " %04X", vm_read16(slot + 0xAu + q*8u));
                                        fprintf(stderr, "\n");
                                    }
                                }
                            }
                            fprintf(stderr, "[wkl] mediaOps@1449EC4=0x%08X achevmt(+0x5)@1351A39=0x%02X\n",
                                    vm_read32(0x01449EC4u), vm_read8(0x01351A39u));
                        }
                        /* SOFT-FAIL CHECK (func_008A8AA0): si l'octet ready du
                         * contexte FIOS frontend (*(g7FC8+0x14)+0x4) est 0, TOUTE
                         * probe d'existence retourne 0x8001002F SANS soumettre
                         * (retour ignore par l'appelant) -> rings jamais crees.
                         * + octet mode global (g7FC8_obj+0x215C) qui choisit la
                         * voie A/B dans func_0089CA34. READ-ONLY. */
                        {
                            uint32_t g7FC8 = vm_read32(g_canonical_toc - 0x7254u); /* r30 du module b0013 */
                            uint32_t gobj  = (g7FC8 >= 0x10000u) ? vm_read32(g7FC8 - 0x7FC8u) : 0;
                            uint32_t fctx  = (gobj  >= 0x10000u) ? vm_read32(gobj + 0x14u) : 0;
                            uint32_t gmode = (gobj  >= 0x10000u) ? vm_read32(gobj + 0x0u) : 0;
                            uint8_t  ready = (fctx  >= 0x10000u) ? vm_read8(fctx + 0x4u) : 0xFF;
                            uint8_t  mode215C = (gmode >= 0x10000u) ? vm_read8(gmode + 0x215Cu) : 0xFF;
                            fprintf(stderr, "   [fios-ctx] gobj=0x%08X fctx(+0x14)=0x%08X READY(+0x4)=%u "
                                    "gmode=0x%08X mode215C=%u\n", gobj, fctx, ready, gmode, mode215C);
                        }
                        /* VUE func_0089B294 (predicat d'avance d'ecran) : READ-ONLY.
                         * mgr+0x7F8 = octet de gate d'entree ; mgr+0x7EC = nombre
                         * d'items en attente ; mgr+0x1C[i] = ptr item ; item+0x22C
                         * = HANDLE de decode async (poll par func_008A85DC) ;
                         * item+0x18 = bits d'etat (bit ~26 = decode-ready).
                         * BUT: distinguer handle NUL (decode jamais demande) vs
                         * handle valide non-pret (decode demande, jamais termine). */
                        if (changed) {
                            uint8_t  gate7F8 = vm_read8(mgr + 0x7F8);
                            uint32_t pcount  = vm_read32(mgr + 0x7EC);
                            uint32_t d9c8    = vm_read32(mgr + 0xD9C + 0x8);
                            fprintf(stderr, "   [adv] gate7F8=%u pendCount(7EC)=%u count(D9C+8)=%u\n",
                                    gate7F8, pcount, d9c8);
                            uint32_t lim = pcount; if (lim > 16u) lim = 16u;
                            for (uint32_t i = 0; i < lim; ++i) {
                                uint32_t item = vm_read32(mgr + 0x1C + i * 4u);
                                if (item < 0x10000u) { fprintf(stderr, "   [adv]   item[%u]=0x%08X (null/invalide)\n", i, item); continue; }
                                uint32_t hdl = vm_read32(item + 0x22C);
                                uint32_t b18 = vm_read32(item + 0x18);
                                uint32_t readybit = (b18 >> 26) & 1u; /* rldicl(x,38,63) = bit 26 */
                                char inm[24]; int ik=0;
                                for(;ik<23;++ik){char c=(char)vm_read8(item+0x24+ik); if(!c)break; inm[ik]=(c>=32&&c<127)?c:'?';}
                                inm[ik]=0;
                                fprintf(stderr, "   [adv]   item[%u]=0x%08X '%s' handle22C=0x%08X %s | +0x18=0x%08X readybit26=%u\n",
                                        i, item, inm, hdl,
                                        (hdl < 0x10000u) ? "<<HANDLE NUL: decode JAMAIS demande" : "handle valide",
                                        b18, readybit);
                            }
                        }
                        /* nommer les objets non-terminaux (state 1..10) = les bloqueurs */
                        if (changed && arr >= 0x10000u) {
                            int shown = 0;
                            for (int i = 0; i < 250 && shown < 8; ++i) {
                                uint32_t rec = arr + i*0x2F8u;
                                uint32_t obj = vm_read32(rec + 0x20);
                                if (obj < 0x10000u) continue;
                                uint32_t st = vm_read32(obj + 0x20);
                                uint32_t rst = vm_read32(rec + 0x18);
                                if (st == 8u && shown < 8) {
                                    /* Champs lus par le handler state-8 func_008B6B54
                                     * (early-outs vers func_008B6F5C = attente) :
                                     * +0x2 bit3 doit etre 0 ; +0x0(16b) doit != 2. */
                                    uint32_t o0 = vm_read16(obj + 0x0);
                                    uint8_t  o2 = vm_read8(obj + 0x2);
                                    uint32_t oC8 = vm_read32(obj + 0xC8);
                                    uint32_t oBC = vm_read32(obj + 0xBC);
                                    uint32_t o80 = vm_read32(obj + 0x80);  /* decode-ready desc (dump misc-fx=non-null) */
                                    uint32_t o6C = vm_read32(obj + 0x6C);  /* child (dump misc-fx=non-null) */
                                    uint32_t o68 = vm_read32(obj + 0x68);  /* child count */
                                    char n8[24]; int k8=0;
                                    for(;k8<23;++k8){char c=(char)vm_read8(obj+0x24+k8); if(!c)break; n8[k8]=(c>=32&&c<127)?c:'?';}
                                    n8[k8]=0;
                                    fprintf(stderr, "   [s8] obj=0x%08X name='%s' +0x0=%u +0x2=0x%02X(bit3=%u) "
                                            "+0x68=%u +0x6C=0x%08X +0x80=0x%08X +0xBC=0x%08X +0xC8=0x%08X\n",
                                            obj, n8, o0, o2, (o2>>3)&1u, o68, o6C, o80, oBC, oC8);
                                    /* DIAGNOSTIC (UC3_S8_CLEARBIT3, comme FORCE_CHILD) :
                                     * clear bit3 de obj+0x2 pour CONFIRMER que c'est
                                     * le gate (la transition fire-t-elle ?). PAS une
                                     * solution (regle #6) : sonde de confirmation. */
                                    if (getenv("UC3_S8_CLEARBIT3") && ((o2 >> 3) & 1u)) {
                                        vm_write8(obj + 0x2, (uint8_t)(o2 & ~0x08u));
                                        fprintf(stderr, "   [s8-clear] obj=0x%08X '%s' bit3 cleared -> +0x2=0x%02X\n",
                                                obj, n8, (uint8_t)(o2 & ~0x08u));
                                    }
                                    ++shown; continue;
                                }
                                if (st == 0 || st >= 0xBu || rst == 0xBu) continue;
                                char nm[24]; int kk=0;
                                for(;kk<23;++kk){char c=(char)vm_read8(obj+0x24+kk); if(!c)break; nm[kk]=(c>=32&&c<127)?c:'?';}
                                nm[kk]=0;
                                /* Champs de gating state-2 (uc3-render-hang-state2-gate) :
                                 * obj+0x6C = child ressource (besoin child+0x18==11),
                                 * obj+0x80 = pointeur decode-ready (func_00979DA8). */
                                uint32_t c6c = vm_read32(obj + 0x6C);
                                uint32_t c6c_st = (c6c >= 0x10000u) ? vm_read32(c6c + 0x18) : 0xFFFFFFFFu;
                                uint32_t f80 = vm_read32(obj + 0x80);
                                fprintf(stderr, "   [blk] rec=%d obj=0x%08X st=%u recst=0x%X name='%s' "
                                        "| child6C=0x%08X child6C+0x18=%u obj+0x80=0x%08X\n",
                                        i, obj, st, rst, nm, c6c, c6c_st, f80);
                                /* Détail du handle enfant (child6C) : async-read ring
                                 * +0x240, count +0x244, handle +0x268, req.status +0x98,
                                 * son propre child +0x7F0, +0x6C (récursion), +0x80. */
                                if (c6c >= 0x10000u) {
                                    uint32_t hring = vm_read32(c6c + 0x240);
                                    uint32_t hcnt  = vm_read32(c6c + 0x244);
                                    uint32_t hhdl  = vm_read32(c6c + 0x268);
                                    uint32_t h7f0  = vm_read32(c6c + 0x7F0);
                                    uint32_t h6c   = vm_read32(c6c + 0x6C);
                                    uint32_t h80   = vm_read32(c6c + 0x80);
                                    uint32_t hreq  = (hring >= 0x10000u) ? vm_read32(hring + 0x98) : 0xFFFFFFFFu;
                                    char hn[24]; int hk=0;
                                    for(;hk<23;++hk){char c=(char)vm_read8(c6c+0x24+hk); if(!c)break; hn[hk]=(c>=32&&c<127)?c:'?';}
                                    hn[hk]=0;
                                    fprintf(stderr, "      [c6c] 0x%08X name='%s' ring240=0x%08X cnt244=%u "
                                            "hdl268=0x%08X reqStatus98=%d ch7F0=0x%08X ch6C=0x%08X +0x80=0x%08X\n",
                                            c6c, hn, hring, hcnt, hhdl, (int)hreq, h7f0, h6c, h80);
                                }
                                ++shown;
                            }
                        }
                        p_sig = sig; ++lines;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                }
                fprintf(stderr, "[fe-state] fin d'echantillonnage\n");
            }).detach();
        }

        /* UC3_SCREEN : sonde READ-ONLY de l'ecran frontend COURANT (etat-machine
         * d'ecrans dispatchee par vtable dans func_009CB1EC). Chaine exacte
         * (derivee du code) : S=*(*(TOC-0x6FD4)-0x7FE8) ; C=*(S+0x10) ;
         * holder=func_0081F490(C,1)=*(C+0x24) ; screen=*(holder+0x54) ;
         * vtable=*(screen+0). Si le vtable de l'ecran ne CHANGE JAMAIS apres
         * flag1=0 => l'ecran 'loading' ne transitionne pas vers 'menu' = LE GATE.
         * Aucune ecriture (regle #6). */
        if (getenv("UC3_SCREEN") != nullptr) {
            std::thread([]{
                while (vm_base == nullptr || g_canonical_toc == 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                auto t0 = std::chrono::steady_clock::now();
                uint32_t p_scr = 0xFFFFFFFFu, p_vt = 0xFFFFFFFFu;
                int lines = 0, ticks = 0;
                while (lines < 120 && ticks < 3000) {
                    ++ticks;
                    uint32_t r30 = vm_read32(g_canonical_toc - 0x6FD4u);
                    uint32_t s   = (r30 >= 0x10000u) ? vm_read32(r30 - 0x7FE8u) : 0;
                    uint32_t c   = (s   >= 0x10000u) ? vm_read32(s + 0x10u) : 0;
                    uint32_t hld = (c   >= 0x10000u) ? vm_read32(c + 0x24u) : 0;
                    uint32_t scr = (hld >= 0x10000u) ? vm_read32(hld + 0x54u) : 0;
                    uint32_t vt  = (scr >= 0x10000u) ? vm_read32(scr + 0x0u) : 0;
                    uint8_t  f1  = vm_read8(0x011CE590u);
                    if (scr != p_scr || vt != p_vt) {
                        long ms = (long)std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
                        /* chercher un nom lisible via un pointeur dans l'objet */
                        char nm[48]; nm[0] = 0;
                        static const uint32_t offs[] = {0x08u,0x0Cu,0x10u,0x14u,0x18u,0x1Cu,0x20u,0x28u};
                        for (uint32_t off : offs) {
                            uint32_t p = (scr >= 0x10000u) ? vm_read32(scr + off) : 0;
                            if (p < 0x10000u) continue;
                            char b[40]; int k = 0;
                            for (; k < 39; ++k) { char ch = (char)vm_read8(p + k);
                                if (ch >= 32 && ch < 127) b[k] = ch; else break; }
                            b[k] = 0;
                            if (k >= 4) { snprintf(nm, sizeof nm, "+0x%X->'%s'", off, b); break; }
                        }
                        /* La methode UPDATE dispatchee par func_009CB1EC est
                         * a vtable+0x18 (r9=*(screen+0)=vt ; r9=*(vt+0x18)=OPD ;
                         * call *(OPD+0)). Resoudre l'OPD -> fonction update. */
                        uint32_t upd_opd = (vt >= 0x10000u) ? vm_read32(vt + 0x18u) : 0;
                        uint32_t upd_fn  = (upd_opd >= 0x10000u) ? vm_read32(upd_opd + 0x0u) : 0;
                        fprintf(stderr, "[screen] t=%ldms S=0x%08X C=0x%08X "
                                "holder=0x%08X screen=0x%08X vtable=0x%08X "
                                "update(vt+0x18)=OPD 0x%08X -> func_%08X "
                                "flag1=%u name=%s\n",
                                ms, s, c, hld, scr, vt, upd_opd, upd_fn, f1, nm);
                        p_scr = scr; p_vt = vt; ++lines;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                fprintf(stderr, "[screen] fin d'echantillonnage\n");
            }).detach();
        }

        /* UC3_SUBSYS : enumere la FILE de sous-systemes remplie par func_00281F30
         * via func_00264FB0 (append {hash@+0, subsystem@+4, flag@+8} a
         * mgr+0xDF4, count@mgr+0xDF0). mgr = *(*(TOC-0x7B1C)-0x7FC8). But :
         * identifier le sous-systeme SCREEN-FLOW (celui qui switche loading->menu
         * + declenche func_009416CC/vtex). Dump one-shot apres flag1=0 : pour
         * chaque entree, hash + objet + son vtable + methode update (vt+0x18) +
         * nom lisible. Aucune ecriture (regle #6). */
        if (getenv("UC3_SUBSYS") != nullptr) {
            std::thread([]{
                while (vm_base == nullptr || g_canonical_toc == 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                /* attendre flag1=0 (post-load) pour que la file soit remplie */
                for (int w = 0; w < 400 && vm_read8(0x011CE590u) != 0; ++w)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                uint32_t r30 = vm_read32(g_canonical_toc - 0x7B1Cu);
                uint32_t mgr = (r30 >= 0x10000u) ? vm_read32(r30 - 0x7FC8u) : 0;
                if (mgr < 0x10000u) { fprintf(stderr, "[subsys] mgr non resolu (r30=0x%08X)\n", r30); return; }
                uint32_t cnt = vm_read32(mgr + 0xDF0u);
                fprintf(stderr, "[subsys] mgr=0x%08X count=%u (file @0x%08X)\n", mgr, cnt, mgr + 0xDF4u);
                if (cnt > 200u) cnt = 200u;
                for (uint32_t i = 0; i < cnt; ++i) {
                    uint32_t e    = mgr + 0xDF4u + i * 0xCu;
                    uint32_t hash = vm_read32(e + 0x0u);
                    uint32_t obj  = vm_read32(e + 0x4u);
                    uint32_t flag = vm_read32(e + 0x8u);
                    uint32_t vt   = (obj >= 0x10000u) ? vm_read32(obj + 0x0u) : 0;
                    uint32_t uopd = (vt  >= 0x10000u) ? vm_read32(vt + 0x18u) : 0;
                    uint32_t ufn  = (uopd>= 0x10000u) ? vm_read32(uopd + 0x0u) : 0;
                    char nm[40]; nm[0] = 0;
                    static const uint32_t offs[] = {0x08u,0x0Cu,0x10u,0x14u,0x18u,0x1Cu,0x20u,0x24u};
                    for (uint32_t off : offs) {
                        uint32_t p = (obj >= 0x10000u) ? vm_read32(obj + off) : 0;
                        if (p < 0x10000u) continue;
                        char b[36]; int k = 0;
                        for (; k < 35; ++k) { char ch = (char)vm_read8(p + k);
                            if (ch >= 32 && ch < 127) b[k] = ch; else break; }
                        b[k] = 0;
                        if (k >= 4) { snprintf(nm, sizeof nm, "+0x%X'%s'", off, b); break; }
                    }
                    fprintf(stderr, "[subsys] #%3u hash=0x%08X obj=0x%08X flag=%u "
                            "vtable=0x%08X update=func_%08X name=%s\n",
                            i, hash, obj, flag, vt, ufn, nm);
                }
                fprintf(stderr, "[subsys] fin\n");
            }).detach();
        }

        /* UC3_CTRL : trouve l'objet CONTROLEUR FRONTEND (dont la vtable @0x011C35B0
         * contient func_00BFA768 a +0x10 = la methode de TRANSITION qui cree
         * l'event port 0xDEAD78 + spawn 'Load vTex'). Scan le heap frontend pour
         * *(obj+0)==0x011C35B0, dump son etat (+0xC gate, +0x14 event-queue,
         * +0x18). Read-only. Avec UC3_FORCE_TRANSITION=1 : appelle func_00BFA768
         * sur l'objet REEL quand flag1=0 (diagnostic, comme FORCE_CHILD). */
        if (getenv("UC3_CTRL") != nullptr) {
            std::thread([]{
                while (vm_base == nullptr || g_canonical_toc == 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                /* attendre flag1=0 (frontend charge) */
                for (int w = 0; w < 500 && vm_read8(0x011CE590u) != 0; ++w)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                const uint32_t VT = 0x011C35B0u;
                uint32_t m10 = vm_read32(VT + 0x10u);
                fprintf(stderr, "[ctrl] vtable@0x%08X +0x10=0x%08X (expect 0x00BFA768)\n", VT, m10);
                int found = 0;
                for (uint32_t a = 0x30000000u; a < 0x32000000u && found < 8; a += 4u) {
                    if (vm_read32(a) != VT) continue;
                    uint32_t obj = a;
                    uint8_t  gate = vm_read8(obj + 0xCu);
                    fprintf(stderr, "[ctrl] obj=0x%08X gate(+0xC)=0x%02X +0x14=0x%08X +0x18=0x%08X\n",
                            obj, gate, vm_read32(obj + 0x14u), vm_read32(obj + 0x18u));
                    ++found;
                }
                fprintf(stderr, "[ctrl] found %d controller objects\n", found);
                /* DIAGNOSTIC (UC3_FORCE_TRANSITION, comme FORCE_CHILD) : construire
                 * un contexte minimal et appeler func_00BFA768 (la fn de transition)
                 * pour VOIR si elle cree le port DEAD78 + spawn 'Load vTex' + rend le
                 * menu. Contexte au scratch 0x31E00000 (heap frontend, mappe) :
                 * +0xC=0 (gate), +0x14=queue-event (0=faute geree?), +0x18=0. PAS une
                 * solution (regle #6) : sonde de confirmation du gate transition. */
                if (getenv("UC3_FORCE_TRANSITION") && m10 == 0x00BFA768u) {
                    uint32_t sc = 0x31E00000u;
                    /* verifier que le scratch est mappe (lecture sans faute) */
                    (void)vm_read32(sc);
                    for (int k = 0; k < 0x40; k += 4) vm_write32(sc + (uint32_t)k, 0);
                    vm_write8(sc + 0xCu, 0);                 /* gate = 0 */
                    uint32_t q = getenv("UC3_FORCE_QUEUE") ? (uint32_t)strtoul(getenv("UC3_FORCE_QUEUE"), nullptr, 0) : 0u;
                    vm_write32(sc + 0x14u, q);               /* event queue id */
                    fprintf(stderr, "[ctrl] FORCE func_00BFA768(ctx=0x%08X gate=0 q=0x%08X) [diagnostic]\n", sc, q);
                    extern void func_00BFA768(ppu_context*);
                    static ppu_context c; memset(&c, 0, sizeof c);
                    c.gpr[1] = 0x31D00000u;                  /* scratch guest stack */
                    c.gpr[2] = g_canonical_toc;
                    c.gpr[3] = sc;
                    func_00BFA768(&c);
                    while (g_trampoline_fn) { void(*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; c.gpr[2] = g_canonical_toc; tf((void*)&c); }
                    fprintf(stderr, "[ctrl] FORCE retour r3=0x%08X (voir [evt] DEAD78 + Load vTex)\n", (uint32_t)c.gpr[3]);
                }
            }).detach();
        }

        /* Scan périodique (UC3_SCAN_OPD) : où l'OPD du writer flag2
         * (0x011ACF90, réf. statique unique 0x01160300) apparaît-il en RAM ?
         * Si un slot handler le reçoit -> c'est l'enregistrement du délégué;
         * si jamais -> l'enregistrement ne se produit pas. Scan BE
         * 01 1A CF 90 sur data+heap toutes les 2 s, diff des trouvailles. */
        if (getenv("UC3_SCAN_OPD") != nullptr) {
            std::thread([]{
                while (vm_base == nullptr)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                const uint8_t pat[4] = {0x01, 0x1A, 0xCF, 0x90};
                std::set<uint32_t> known;
                auto t0 = std::chrono::steady_clock::now();
                for (int round = 0; round < 150; ++round) {
                    struct { uint32_t lo, hi; } ranges[] = {
                        {0x01000000u, 0x01400000u},
                        {0x30000000u, 0x32000000u},
                        {0x20000000u, 0x21000000u} };
                    for (auto& r : ranges) {
                        const uint8_t* base = vm_base + r.lo;
                        size_t len = r.hi - r.lo;
                        for (const uint8_t* p = base;
                             (p = (const uint8_t*)memchr(p, 0x01,
                                  (size_t)(base + len - p))) != nullptr; ++p) {
                            if ((size_t)(base + len - p) < 4) break;
                            if (memcmp(p, pat, 4) == 0) {
                                uint32_t ea = r.lo + (uint32_t)(p - base);
                                if (ea != 0x01160300u && !known.count(ea)) {
                                    known.insert(ea);
                                    long ms = (long)std::chrono::duration_cast<
                                        std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - t0)
                                        .count();
                                    fprintf(stderr, "[opd-scan] t=%ldms writer-"
                                            "OPD 0x011ACF90 trouve a 0x%08X\n",
                                            ms, ea);
                                }
                            }
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                }
            }).detach();
        }

        /* Echantillonneur B8 (UC3_B8_SAMPLER) — les charges 10MB se
         * completent en ~100ms (se5.log) ; le SEUL verrou est l'etat-machine
         * post-chargement : objet media 0x01351A34 jamais construit (vt=0),
         * flag2 0x012FD5F9 jamais ecrit. Dater toute transition de ces
         * champs + flag1 0x011CE590 + le mot d'etat media obj+0x5C (delegate
         * de completion) depuis le demarrage du process. */
        if (getenv("UC3_B8_SAMPLER") != nullptr) {
            /* Diff fin de l'objet media 0x01351A34 (0x00..0x6C) des le
             * process : l'ordre des ecritures du STARTER original (qui
             * construit l'objet mais s'arrete avant le delegate +0x5C)
             * revele ou il s'arrete. */
            std::thread([]{
                while (vm_base == nullptr)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                auto t0 = std::chrono::steady_clock::now();
                uint32_t prev[28] = {0};
                int dlines = 0;
                while (dlines < 300) {
                    for (int k = 0; k < 28; ++k) {
                        uint32_t v = vm_read32(0x01351A34u + (uint32_t)k * 4);
                        if (v != prev[k]) {
                            long ms = (long)std::chrono::duration_cast<
                                std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();
                            fprintf(stderr, "[b8-obj] t=%ldms obj+0x%02X: "
                                    "0x%08X -> 0x%08X\n",
                                    ms, k * 4, prev[k], v);
                            prev[k] = v; ++dlines;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(250));
                }
                fprintf(stderr, "[b8-obj] cap atteint\n");
            }).detach();
            std::thread([]{
                while (vm_base == nullptr)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                auto t0 = std::chrono::steady_clock::now();
                uint32_t p_vt = 0xDEADBEEFu, p_d5c = 0xDEADBEEFu;
                uint8_t  p_f1 = 0xEE, p_f2 = 0xEE;
                int lines = 0;
                uint32_t p_sess = 0xDEADBEEFu, p_s5c = 0xDEADBEEFu;
                while (lines < 200) {
                    uint32_t vt  = vm_read32(0x01351A34u);
                    uint32_t d5c = vm_read32(0x01351A34u + 0x5Cu);
                    uint8_t  f1  = vm_read8(0x011CE590u);
                    uint8_t  f2  = vm_read8(0x012FD5F9u);
                    /* L'OPD ps3media utilise TOC=0x011D27D0 puis
                     * r30=*(TOC-0x7240)=0x01168280. L'objet-session media
                     * (alloue 0x490) est publie dans
                     * *(r30-0x7FEC)=0x01352A70.
                     * est celui qui recoit le delegate flag2 via
                     * func_00A4A778 (r4=OPD writer). Surveiller le global et
                     * sessObj+0x5C. */
                    /* b0013:57538 ecrit directement l'objet dans la case de
                     * publication 0x01352A70. L'ancienne sonde dereferencait
                     * une seconde fois et prenait la vtable pour l'objet. */
                    uint32_t sess = vm_read32(0x01352A70u);
                    uint32_t s5c  = sess >= 0x10000u
                                    ? vm_read32(sess + 0x5Cu) : 0;
                    /* Jalons du flux STARTER (b0013) pour encadrer son point
                     * d'arret : J1=*(r30-0x7CD4)=0x01352A78 (noeud "zlib",
                     * 57445) ; J2=*(r30-0x7D7C)=0x01352994 (post-A562C8,
                     * 57526) ; J3=sess (publication session, 57538). */
                    uint32_t j1 = vm_read32(0x01352A78u);
                    uint32_t j2 = vm_read32(0x01352994u);
                    static uint32_t p_j1 = 0xDEADBEEFu, p_j2 = 0xDEADBEEFu;
                    if (sess != p_sess || s5c != p_s5c ||
                        j1 != p_j1 || j2 != p_j2) {
                        long ms2 = (long)std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
                        fprintf(stderr, "[b8-sess] t=%ldms sessObj=0x%08X "
                                "sess+5C=0x%08X J1zlib=0x%08X J2=0x%08X\n",
                                ms2, sess, s5c, j1, j2);
                        p_sess = sess; p_s5c = s5c; p_j1 = j1; p_j2 = j2;
                        ++lines;
                    }
                    if (vt != p_vt || d5c != p_d5c || f1 != p_f1 || f2 != p_f2) {
                        long ms = (long)std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
                        fprintf(stderr, "[b8-smp] t=%ldms mediaVt=0x%08X "
                                "obj+5C=0x%08X flag1=%u flag2=0x%02X\n",
                                ms, vt, d5c, f1, f2);
                        p_vt = vt; p_d5c = d5c; p_f1 = f1; p_f2 = f2;
                        ++lines;
                    }
                    /* DELEGATE-KICK (UC3_DELEGATE_KICK) : l'OPD du writer
                     * flag2 (0x011ACF90 = func_008A2858) EST enregistre a
                     * 0x31155678+0x5C (os9.log, scan RAM) — seul le POST du
                     * message de completion manque (pompe a queue vide).
                     * Livrer nous-memes : appeler le delegue avec son objet
                     * apres flag1=0. Si flag2 passe a 1 -> chaine aval
                     * deverrouillee (menu). */
                    /* UC3_DELEGATE_KICK=<s> : delai en secondes apres
                     * flag1=0 avant la livraison (dk5: livrer a +15ms fait
                     * SORTIR le jeu — le frontend n'a pas encore charge ses
                     * taches menu ; sur console la completion arrive a la
                     * fin des films, ~30-60s). */
                    static bool s_dk_done = false;
                    static auto s_dk_flag1_at =
                        std::chrono::steady_clock::time_point{};
                    const char* delegate_kick = getenv("UC3_DELEGATE_KICK");
                    /* DECOUVERTE DYNAMIQUE de l'objet media (l'ancienne adresse
                     * 0x31155678 etait un heap fige d'un run precedent -> le
                     * kick ne tirait plus car le heap varie par run, e2.log).
                     * Scanner la region media (0x31000000-0x31400000) pour un
                     * mot == 0x011ACF90 (OPD writer flag2 enregistre a obj+0x5C)
                     * -> objet = addr-0x5C. Cache le resultat. */
                    static uint32_t s_media_obj = 0;
                    if (delegate_kick && !s_media_obj &&
                        vm_read8(0x011CE590u) == 0) {
                        /* Plage etendue a 0x33000000 : le NOUVEL objet session
                         * cree par le re-run MODE2 (0x32B0A798) est hors de
                         * l'ancienne plage. Preferer le hit le plus RECENT
                         * (adresse la plus haute = alloc la plus tardive) pour
                         * viser la session VIVANTE, pas l'ancien objet du run
                         * pre-construction (0x31155678). */
                        uint32_t best = 0;
                        for (uint32_t a = 0x31000000u; a < 0x33000000u; a += 4) {
                            if (vm_read32(a) == 0x011ACF90u) {
                                best = a;   /* garde le dernier = plus haut */
                                fprintf(stderr, "[dlg-scan] hit delegue a "
                                        "0x%08X (obj 0x%08X)\n", a, a - 0x5Cu);
                            }
                        }
                        if (best) {
                            s_media_obj = best - 0x5Cu;
                            fprintf(stderr, "[dlg-scan] CHOISI objet media "
                                    "0x%08X (hit le plus recent)\n", s_media_obj);
                        }
                        if (!s_media_obj) { static int _w=0; if(_w<3){_w++;
                            fprintf(stderr, "[dlg-scan] delegue 0x011ACF90 "
                                    "absent du heap media (pas encore "
                                    "enregistre)\n"); } }
                    }
                    if (!s_dk_done && delegate_kick && s_media_obj &&
                        vm_read8(0x011CE590u) == 0) {
                        if (s_dk_flag1_at ==
                            std::chrono::steady_clock::time_point{})
                            s_dk_flag1_at = std::chrono::steady_clock::now();
                        int delay_s = atoi(delegate_kick);
                        if (delay_s < 1) delay_s = 1;
                        if (std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - s_dk_flag1_at)
                            .count() < delay_s)
                            goto dk_skip;
                        s_dk_done = true;
                        fprintf(stderr, "[dlg-kick] appel func_008A2858(obj="
                                "0x%08X) [decouvert dynamiquement]\n",
                                s_media_obj);
                        uc3_call_guest4(0x011ACF90u, s_media_obj, 0, 0, 0);
                        fprintf(stderr, "[dlg-kick] retour: flag2=0x%02X\n",
                                vm_read8(0x012FD5F9u));
                    }
                    dk_skip:;
                    /* LE CALLBACK CONFIE AU SYSTEME MEDIA (bo3.log) : le
                     * STARTER stocke a obj+0xC l'OPD 0x0117A528 =
                     * func_0003E38C, routine de service SANS argument
                     * (ticks: func_002B796C x2, 0079B1FC, 0098E3A8, 009CB630,
                     * 008A32E4 [media], 007BB564, 008AC51C). Notre HLE media
                     * ne l'invoque jamais. UC3_CB_KICK=1: un appel apres
                     * flag1=0 ; =2: periodique 100ms (cap 600). */
                    static int s_cb_kicks = 0;
                    static auto s_cb_last = std::chrono::steady_clock::now();
                    if (const char* ck = getenv("UC3_CB_KICK")) {
                        int mode = atoi(ck);
                        bool due = false;
                        if (vm_read8(0x011CE590u) == 0) {
                            if (mode == 1 && s_cb_kicks == 0) due = true;
                            else if (mode >= 2 && s_cb_kicks < 600) {
                                auto now = std::chrono::steady_clock::now();
                                if (std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    now - s_cb_last).count() >= 100) {
                                    due = true; s_cb_last = now;
                                }
                            }
                        }
                        if (due) {
                            ++s_cb_kicks;
                            if (s_cb_kicks <= 3 || s_cb_kicks % 100 == 0)
                                fprintf(stderr, "[cb-kick] #%d "
                                        "func_0003E38C (OPD 0x0117A528)\n",
                                        s_cb_kicks);
                            uc3_call_guest4(0x0117A528u, 0, 0, 0, 0);
                            if (s_cb_kicks <= 3)
                                fprintf(stderr, "[cb-kick] #%d retour "
                                        "obj+5C=0x%08X flag2=0x%02X\n",
                                        s_cb_kicks,
                                        vm_read32(0x01351A34u + 0x5Cu),
                                        vm_read8(0x012FD5F9u));
                        }
                    }
                    /* COURSE D'INIT identifiee (b9_1.log) : le STARTER
                     * func_008AA72C tourne AVANT la construction de l'objet
                     * media (il voit vt=0, n'enregistre pas le delegate
                     * obj+0x5C) ; l'objet est construit juste apres
                     * (vt=0x3110E728 a t=0 du sampler). Re-kick conforme :
                     * re-invoquer LE CODE DU JEU (OPD 0x011AD2F0 ->
                     * func_008AA72C, r3=obj) une fois l'objet construit et
                     * l'init finie (flag1=0). Gated UC3_MEDIA_RESTART. */
                    static bool s_media_rekick = false;
                    /* Mode 3 (avec UC3_STARTER_SKIP): le 1er appel a ete SAUTE
                     * (aucun stamp, vt encore 0) -> re-invoquer le code du jeu
                     * a flag1==0 SANS exiger vt!=0 (le STARTER construit). */
                    int s_rmode = getenv("UC3_MEDIA_RESTART") ?
                                  atoi(getenv("UC3_MEDIA_RESTART")) : 0;
                    if (!s_media_rekick && s_rmode &&
                        (s_rmode >= 3 || vm_read32(0x01351A34u) != 0) &&
                        vm_read32(0x01351A34u + 0x5Cu) == 0 &&
                        vm_read8(0x011CE590u) == 0) {
                        s_media_rekick = true;
                        /* MODE 2 (diagnostic, regle #6) : le STARTER early-out
                         * sur *(obj+0x4)!=0 (b0013:57286/57315). Notre course
                         * d'init a stampe +0x4=1 sur la memoire PRE-construction
                         * (STARTER couru avant le ctor, vt=0) -> toute relance
                         * voit "deja demarre" et saute l'init (queue/delegate
                         * jamais crees). Mode 2 = annuler CE stamp parasite de
                         * notre propre course (+0x4=0) puis re-invoquer le code
                         * du jeu. Si l'init complete alors (delegate +0x5C,
                         * queue, soumissions), le VRAI fix = corriger l'ordre
                         * d'init dans le runtime. */
                        int rmode = atoi(getenv("UC3_MEDIA_RESTART"));
                        uint8_t started4 = vm_read8(0x01351A34u + 0x4u);
                        if (rmode >= 2 && started4 != 0) {
                            vm_write8(0x01351A34u + 0x4u, 0);
                            fprintf(stderr, "[b8-smp] MODE2: stamp course annule "
                                    "(+0x4 %u -> 0) avant RE-KICK\n", started4);
                        }
                        fprintf(stderr, "[b8-smp] RE-KICK STARTER "
                                "func_008AA72C(obj=0x01351A34) via OPD "
                                "0x011AD2F0\n");
                        uc3_call_guest7(0x011AD2F0u, 0x01351A34u, 1, 0,
                                        0x17, 0x21, 4, 0x0117A528u);
                        fprintf(stderr, "[b8-smp] RE-KICK retour : "
                                "obj+5C=0x%08X +0x4=%u +0x8=0x%08X\n",
                                vm_read32(0x01351A34u + 0x5Cu),
                                vm_read8(0x01351A34u + 0x4u),
                                vm_read32(0x01351A34u + 0x8u));
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }).detach();
        }

        /* UC3_MEMDUMP=<path> : après 90s (jeu en régime établi), dumpe la région
         * de contrôle ELF (0x01000000..0x01400000) pour la comparer octet-à-octet
         * au dump RAM PS3 à l'état menu (ss5_ram.bin) → localiser précisément où
         * l'état de notre port diverge de l'état-menu réel. */
        if (const char* md = getenv("UC3_MEMDUMP")) {
            std::string path = md;
            std::thread([path]{
                std::this_thread::sleep_for(std::chrono::seconds(90));
                const uint32_t base = 0x01000000u, len = 0x00400000u; /* 4MB */
                if (FILE* fp = fopen(path.c_str(), "wb")) {
                    std::vector<uint8_t> buf(len);
                    for (uint32_t i = 0; i < len; i++) buf[i] = vm_read8(base + i);
                    fwrite(buf.data(), 1, len, fp); fclose(fp);
                    fprintf(stderr, "[memdump] wrote %s (0x%08X..0x%08X)\n",
                            path.c_str(), base, base + len);
                }
            }).detach();
        }
        /* UC3_LOADERSTAT : observe l'état du STRUCT LOADER que func_000428A0 lit
         * pour décider "boot terminé". Chaîne: TOC=0x011D27D0 → r30=*(TOC-0x7F50)
         * → loader=*(r30-0x7F88) → champs +0x2C64/+0x2C95/+0x2CBC/+0x2CC4 (gate) et
         * +0x2C64.. Montre POURQUOI le statut reste "pas terminé" dans notre port,
         * sans dépendre du dump (version BCES01670 vs notre BCES01175). Runtime. */
        if (getenv("UC3_LOADERSTAT")) {
            std::thread([]{
                const uint32_t TOC = 0x011D27D0u;
                for (int i = 0; i < 120; i++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    uint32_t r30 = vm_read32(TOC - 0x7F50u);
                    if (r30 < 0x10000u || r30 >= 0x10000000u) {
                        if (i % 10 == 0) fprintf(stderr, "[loaderstat] r30=0x%08X (invalide)\n", r30);
                        continue;
                    }
                    uint32_t ld = vm_read32(r30 - 0x7F88u);
                    /* Les octets "ressource prête" de la chaîne de statut boot :
                     * r31=*(r30-0x7E58), check *(r31+0); r27=*(r30-0x7A28),
                     * check *(r27+0x2B71). Non-nul = ressource bloquée = boot
                     * jamais "terminé". */
                    uint32_t r31 = vm_read32(r30 - 0x7E58u);
                    uint32_t r27 = vm_read32(r30 - 0x7A28u);
                    uint32_t b31 = (r31>=0x10000u && r31<0x40000000u) ? vm_read8(r31) : 0xFF;
                    uint32_t b27 = (r27>=0x10000u && r27<0x40000000u) ? vm_read8(r27+0x2B71u) : 0xFF;
                    /* Boucle d'attente loc_000442A8 (57923..) : r29=*(r30-0x7DA8)=flag1 ptr,
                     * boucle tant que flag1!=0 && flag2==0. Requêtes posées juste avant
                     * (57916-57922): reqA ptr=*(r30-0x7DB8), reqB ptr=*(r30-0x78F4). */
                    uint32_t f1p = vm_read32(r30 - 0x7DA8u);
                    uint32_t rAp = vm_read32(r30 - 0x7DB8u);
                    uint32_t rBp = vm_read32(r30 - 0x78F4u);
                    uint32_t f1  = (f1p>=0x10000u && f1p<0x40000000u) ? vm_read8(f1p) : 0xFF;
                    uint32_t rA  = (rAp>=0x10000u && rAp<0x40000000u) ? vm_read8(rAp) : 0xFF;
                    uint32_t rB  = (rBp>=0x10000u && rBp<0x40000000u) ? vm_read8(rBp) : 0xFF;
                    fprintf(stderr, "[loaderstat] t=%ds flag1@%08X=%02X reqA@%08X=%02X reqB@%08X=%02X | r31=0x%08X *r31=0x%02X | r27=0x%08X *(r27+2B71)=0x%02X",
                            i, f1p, f1, rAp, rA, rBp, rB, r31, b31, r27, b27);
                    if (ld >= 0x10000u && ld < 0x40000000u)
                        fprintf(stderr, " | loader+2C94=0x%02X", vm_read8(ld+0x2C94));
                    fprintf(stderr, "\n");
                }
            }).detach();
        }
        /* UC3_S8STAT : le VRAI frontier (2026-07-09). Les objets frontend
         * atteignent l'OBJET-etat 8 (func_008B6B54) puis calent. Ce handler
         * early-return si (a) *(*(r30-0x7F20))==0 [gflag global A], (b) un bit de
         * *(obj+0x2)==0, ou (c) *(obj+0)==2. r30=*(TOC-0x7220). Lit les 2 gflags
         * globaux (independants de l'objet) pour voir lequel gate. Runtime. */
        if (getenv("UC3_S8STAT")) {
            std::thread([]{
                const uint32_t TOC = 0x011D27D0u;
                for (int i = 0; i < 120; i++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    uint32_t r30 = vm_read32(TOC - 0x7220u);
                    if (r30 < 0x10000u || r30 >= 0x40000000u) {
                        if (i % 10 == 0) fprintf(stderr, "[s8stat] t=%ds r30=0x%08X (invalide)\n", i, r30);
                        continue;
                    }
                    uint32_t gAp = vm_read32(r30 - 0x7F20u);
                    uint32_t gBp = vm_read32(r30 - 0x7F08u);
                    uint32_t gA = (gAp>=0x10000u && gAp<0x40000000u) ? vm_read8(gAp) : 0xFF;
                    uint32_t gB = (gBp>=0x10000u && gBp<0x40000000u) ? vm_read8(gBp) : 0xFF;
                    uint32_t obj = 0x31473E20u; /* misc-fx (adresse deterministe) */
                    uint32_t st = vm_read32(obj + 0x20u);
                    uint32_t o0 = vm_read16(obj + 0x0u);
                    uint32_t o2 = vm_read8(obj + 0x2u);
                    fprintf(stderr, "[s8stat] t=%ds objstate=%u gflagA@%08X=%02X gflagB@%08X=%02X | obj0=%04X obj2=%02X\n",
                            i, st, gAp, gA, gBp, gB, o0, o2);
                }
            }).detach();
        }
        /* UC3_FLAG1_ZERO[=sec] : après <sec>s (défaut 30, laissant le vrai
         * chargement essayer), force flag1@0x011CE590 = 0 côté RUNTIME. C'est la
         * condition de sortie de la boucle d'attente boot loc_000442A8
         * (while flag1!=0 && flag2==0). Le dump PS3-menu montre flag1=0. Avec I/O+
         * décode+rendu désormais fonctionnels, ceci devrait débloquer la transition
         * boot->frontend (Load vTex) — l'ancien OOB venait de ce que les assets ne
         * chargeaient pas, ce qui est corrigé. Runtime, gated, aucune édition du
         * généré. */
        if (const char* fz = getenv("UC3_FLAG1_ZERO")) {
            int sec = atoi(fz); if (sec <= 0) sec = 30;
            std::thread([sec]{
                std::this_thread::sleep_for(std::chrono::seconds(sec));
                fprintf(stderr, "[flag1-zero] flag1@0x011CE590 was 0x%02X, forcing 0 "
                        "(exit boot load-wait)\n", vm_read8(0x011CE590u));
                vm_write8(0x011CE590u, 0);
                /* re-force périodiquement au cas où un writer le remet à 1 */
                for (int i = 0; i < 120; i++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    if (vm_read8(0x011CE590u) != 0) vm_write8(0x011CE590u, 0);
                }
            }).detach();
        }
        /* UC3_FORCE_STUCK[=ticks] : force RUNTIME + TEMPORISÉ. Scanne les objets à
         * state==2 (nom imprimable, cnt68 raisonnable) et, pour chaque enfant bloqué
         * au MÊME child+0x18 (<11) depuis >= <ticks> ticks (défaut 12 = ~6s), le force
         * à 11. Contrairement à FORCE_CHILD (force tout tout de suite -> aucune donnée),
         * ceci laisse les vrais chargements avancer et ne force QUE les enfants
         * génuinement calés (le loader ne les initie jamais). But: débloquer la
         * transition frontend->menu. Écriture mémoire guest depuis un thread runtime
         * (autorisé; AUCUNE édition du code généré). */
        if (const char* fs = getenv("UC3_FORCE_STUCK")) {
            int thr = atoi(fs); if (thr <= 0) thr = 12;
            std::thread([thr]{
                struct S { uint32_t child; uint32_t ss; int cnt; };
                static std::vector<S> seen;
                int forced_total = 0;
                for (int iter = 0; iter < 400; iter++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    for (uint32_t o = 0x31000000u; o < 0x31800000u; o += 0x10) {
                        char c0 = (char)vm_read8(o + 0x24);
                        if (c0 < 'a' || c0 > 'z') continue;
                        if (vm_read32(o + 0x20) != 2u) continue;
                        uint32_t cnt = vm_read32(o + 0x68);
                        if (cnt == 0 || cnt > 16) continue;   /* rejette le garbage */
                        for (uint32_t i = 0; i < cnt; i++) {
                            uint32_t ch = vm_read32(o + 0x6C + i*4);
                            if (ch < 0x30000000u || ch >= 0x32000000u) continue;
                            uint32_t ss = vm_read32(ch + 0x18);
                            if (ss == 11u || ss > 11u) continue;   /* déjà prêt/garbage */
                            /* suivi par enfant : compte des ticks au MÊME ss */
                            S* rec = nullptr;
                            for (auto& s : seen) if (s.child == ch) { rec = &s; break; }
                            if (!rec) { seen.push_back({ch, ss, 0}); rec = &seen.back(); }
                            if (rec->ss != ss) { rec->ss = ss; rec->cnt = 0; }
                            else if (++rec->cnt >= thr) {
                                vm_write32(ch + 0x18, 11u);
                                rec->child = 0;
                                if (forced_total < 40) { forced_total++;
                                    char nm[9]; for(int k=0;k<8;k++) nm[k]=(char)vm_read8(o+0x24+k); nm[8]=0;
                                    fprintf(stderr, "[force-stuck] obj=0x%08X '%s' child[%u]=0x%08X ss=%u->11 (stuck %d ticks)\n",
                                            o, nm, i, ch, ss, thr); }
                            }
                        }
                    }
                }
            }).detach();
        }
        /* UC3_WATCHNAME=<nom> : observateur RUNTIME qui scanne le tas d'objets
         * (0x31400000..0x31600000) pour un objet dont obj+0x24 == <nom> et qui est
         * à state==2 (obj+0x20), puis dumpe périodiquement ce qui le BLOQUE :
         * enfants (obj+0x6C, count obj+0x68) + leur ss (child+0x18), et la
         * collection-2 (obj+0x80, counts +0x10/+0x14). Répond à "pourquoi
         * world-gr/u2-journ/proto ne complètent jamais state-2". Runtime, gated. */
        if (const char* wn = getenv("UC3_WATCHNAME")) {
            std::string want = wn;
            bool all = (want == "*");
            std::thread([want, all]{
                for (int iter = 0; iter < 240; iter++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    int shown = 0;
                    for (uint32_t o = 0x31000000u; o < 0x32000000u && shown < 12; o += 0x10) {
                        char nm[9]; for (int i = 0; i < 8; i++) nm[i] = (char)vm_read8(o + 0x24 + i); nm[8] = 0;
                        /* nom imprimable [a-z] au 1er caractère */
                        if (nm[0] < 'a' || nm[0] > 'z') continue;
                        uint32_t st = vm_read32(o + 0x20);
                        if (st != 2u) continue;
                        if (!all && strncmp(nm, want.c_str(), want.size()) != 0) continue;
                        uint32_t cnt = vm_read32(o + 0x68);
                        uint32_t p80 = vm_read32(o + 0x80);
                        char line[400]; int pp = 0;
                        pp += snprintf(line+pp, sizeof(line)-pp,
                            "[watchname] obj=0x%08X '%s' state=2 cnt68=%u p80=0x%08X c2=(%u/%u) children:",
                            o, nm, cnt, p80,
                            p80>=0x10000u?vm_read16(p80+0x10):0xFFFF,
                            p80>=0x10000u?vm_read16(p80+0x14):0xFFFF);
                        for (uint32_t i = 0; i < cnt && i < 12; i++) {
                            uint32_t ch = vm_read32(o + 0x6C + i*4);
                            uint32_t ss = (ch>=0x10000u&&ch<0x3FF00000u)?vm_read32(ch+0x18):0xFFFFFFFF;
                            pp += snprintf(line+pp, sizeof(line)-pp, " [%u]ss=%u", i, ss);
                        }
                        fprintf(stderr, "%s\n", line);
                        shown++;
                    }
                    if (shown) fprintf(stderr, "[watchname] --- tick %d: %d objets state=2 ---\n", iter, shown);
                }
            }).detach();
        }
        /* UC3_CHILDWATCH=<hexaddr> : observateur RUNTIME (aucune édition du code
         * généré) d'un enfant d'asset frontend pendant le stall state-2. Dump
         * périodique des champs pilotant la pompe de sous-état (func_0089F7B0 →
         * dispatcher func_0089F418) : child+0x18 (ss), +0x7F0 (handle), +0x948
         * (remaining), +0x958 (io-in-flight ? la pompe SKIPPE si !=0), et côté
         * handle : +0x18 (sub-état), +0x268 (desc lecture), +0x0/+0x2B0 (n/tot).
         * Adresse par défaut : 0x3135F200 = child[0] "misc-fx" (stable). */
        if (const char* cw = getenv("UC3_CHILDWATCH")) {
            uint32_t addr = (uint32_t)strtoul(cw, nullptr, 16);
            if (addr < 0x10000u) addr = 0x3135F200u;
            std::thread([addr]{
                for (int i = 0; i < 600; i++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    uint32_t ss   = vm_read32(addr + 0x18);
                    uint32_t h    = vm_read32(addr + 0x7F0);
                    uint32_t r948 = vm_read32(addr + 0x948);
                    uint32_t r958 = vm_read32(addr + 0x958);
                    uint32_t hss  = h >= 0x10000u ? vm_read32(h + 0x18)  : 0;
                    uint32_t hdsc = h >= 0x10000u ? vm_read32(h + 0x268) : 0;
                    uint32_t hn   = h >= 0x10000u ? vm_read32(h + 0x0)   : 0;
                    uint32_t htot = h >= 0x10000u ? vm_read32(h + 0x2B0) : 0;
                    if ((i % 4) == 0 || ss == 11)
                        fprintf(stderr, "[childwatch] t=%0.1fs child=0x%08X ss=%u h=0x%08X "
                                "hss=%u desc=0x%08X n=%u tot=%u rem948=%u io958=%u\n",
                                i * 0.5, addr, ss, h, hss, hdsc, hn, htot, r948, r958);
                    if (ss == 11) break;   /* complété — observation terminée */
                }
            }).detach();
        }

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
        ps3_hle_register_ctx(0x98D5B343, "cellSpursShutdownWorkload", br_cellSpursShutdownLog);
        ps3_hle_register_ctx(0x5FD43FE4, "cellSpursWaitForWorkloadShutdown", br_cellSpursShutdownLog);
        ps3_hle_register_ctx(0x57E4DEC3, "cellSpursRemoveWorkload", br_cellSpursShutdownLog);
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
        ps3_hle_register_ctx(0xF042B14F, "sceNpDrmIsAvailable2", br_sceNpDrmIsAvailable2);

        ps3_hle_register_ctx(0x75BBB672, "cellVideoOutGetNumberOfDevice", br_cellVideoOutGetNumberOfDevice);
        ps3_hle_register_ctx(0x887572D5, "cellVideoOutGetState", br_cellVideoOutGetState);
        ps3_hle_register_ctx(0x1E930EEF, "cellVideoOutGetDeviceInfo", br_cellVideoOutGetDeviceInfo);
        ps3_hle_register_ctx(0x2CCE9CF5, "cellRtcGetCurrentClockLocalTime", br_cellRtcGetCurrentClockLocalTime);
        ps3_hle_register_ctx(0xCB90C761, "cellRtcGetTime_t", br_cellRtcGetTime_t);
        ps3_hle_register_ctx(0x21425307, "cellSaveDataListAutoLoad", br_cellSaveDataListAutoLoad);
        ps3_hle_register_ctx(0x4DD03A4E, "cellSaveDataListAutoSave", br_cellSaveDataListAutoSave);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpTrophyRegisterContext"), "sceNpTrophyRegisterContext", br_sceNpTrophyRegisterContext);
        ps3_hle_register_ctx(0xB3AC3478, "sceNpTrophyGetTrophyUnlockState", br_sceNpTrophyGetTrophyUnlockState);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpTrophyInit"), "sceNpTrophyInit", br_sceNpTrophyInit);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpTrophyTerm"), "sceNpTrophyTerm", br_sceNpTrophyTerm);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpTrophyCreateContext"), "sceNpTrophyCreateContext", br_sceNpTrophyCreateContext);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpTrophyDestroyContext"), "sceNpTrophyDestroyContext", br_sceNpTrophyDestroyContext);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpTrophyCreateHandle"), "sceNpTrophyCreateHandle", br_sceNpTrophyCreateHandle);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpTrophyDestroyHandle"), "sceNpTrophyDestroyHandle", br_sceNpTrophyDestroyHandle);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpTrophyGetRequiredDiskSpace"), "sceNpTrophyGetRequiredDiskSpace", br_sceNpTrophyGetRequiredDiskSpace);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpTrophySetSoundLevel"), "sceNpTrophySetSoundLevel", br_sceNpTrophySetSoundLevel);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpTrophyGetGameProgress"), "sceNpTrophyGetGameProgress", br_sceNpTrophyGetGameProgress);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpTrophyUnlockTrophy"), "sceNpTrophyUnlockTrophy", br_sceNpTrophyUnlockTrophy);
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
