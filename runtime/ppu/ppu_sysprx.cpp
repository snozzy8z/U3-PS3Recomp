/*
 * ps3recomp - sysPrxForUser CRT (boot-critical HLE)
 *
 * The first firmware functions a PS3 program calls at startup come from
 * sysPrxForUser (the libc/CRT bridge). Some need the full ppu_context (e.g.
 * sys_initialize_tls sets the thread pointer r13), so they register as
 * context-aware handlers (ps3_hle_register_ctx) rather than through the generic
 * integer-ABI table.
 *
 * NIDs are computed from the names (ps3_compute_nid), so this stays correct
 * without hand-written NID literals.
 */
#include "ppu_recomp.h"     /* ppu_context */
#include "ps3emu/error_codes.h"
#include "ps3emu/nid.h"     /* ps3_compute_nid */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" uint8_t* vm_base;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" uint32_t vm_read32(uint64_t a);
extern "C" void     vm_write32(uint64_t a, uint32_t v);
extern "C" void     vm_write64(uint64_t a, uint64_t v);
extern "C" void     ppu_dump_guest_caller(uint32_t tag);

/* Simple bump allocator for TLS areas, in a free vm region below the stack. */
static uint32_t s_tls_next = 0x0E000000u;

/* sys_initialize_tls(u64 main_thread_id, u32 tls_seg_addr, u32 tls_seg_size,
 *                     u32 tls_mem_size) -- set up the main thread's TLS block
 * and point r13 (the PPC64 thread pointer) at it. TLS variables are accessed
 * at r13 - 0x7000 (the static TLS block bias). */
extern "C" uint32_t g_tls_vaddr;
extern "C" uint32_t g_tls_filesz;
extern "C" uint32_t g_tls_memsz;

static void sys_initialize_tls(ppu_context* ctx)
{
    uint32_t seg_addr = (uint32_t)ctx->gpr[4];
    uint32_t seg_size = (uint32_t)ctx->gpr[5];
    uint32_t mem_size = (uint32_t)ctx->gpr[6];

    /* The CRT often passes zero TLS params (it sources them from a process
     * param block we don't fully populate). Fall back to the ELF's PT_TLS
     * image captured at load time so TLS variables hold their initialised
     * values instead of zero. */
    if (seg_addr == 0 && g_tls_vaddr) {
        seg_addr = g_tls_vaddr;
        seg_size = g_tls_filesz;
        mem_size = g_tls_memsz;
    }

    uint32_t block = s_tls_next;
    uint32_t total = ((mem_size + 0x7000u + 0x1000u) + 0xFFFu) & ~0xFFFu;
    s_tls_next += total;

    if (seg_addr && seg_size) memcpy(vm_base + block, vm_base + seg_addr, seg_size);
    if (mem_size > seg_size)  memset(vm_base + block + seg_size, 0, mem_size - seg_size);

    ctx->gpr[13] = block + 0x7000u;   /* thread pointer; TLS data at r13-0x7000 */
    ctx->gpr[3]  = 0;                  /* CELL_OK */
    fprintf(stderr, "[crt] sys_initialize_tls: block 0x%08X, r13=0x%08X (seg 0x%X+%u, mem %u)\n",
            block, (uint32_t)ctx->gpr[13], seg_addr, seg_size, mem_size);
}

/* sys_time_get_system_time() -> microseconds since boot (monotonic-ish). */
static void sys_time_get_system_time(ppu_context* ctx)
{
    static uint64_t t = 0;
    t += 1000;                         /* advance so callers see time progress */
    ctx->gpr[3] = t;
}

/* sys_process_is_stack(u32 addr) -> 1 if addr is in the stack region. We model
 * a single stack just below the TLS region; good enough for boot checks. */
static void sys_process_is_stack(ppu_context* ctx)
{
    uint32_t a = (uint32_t)ctx->gpr[3];
    ctx->gpr[3] = (a >= 0x0E000000u && a < 0x10000000u) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Lightweight mutexes and condition variables.
 *
 * Guest structures stay in big-endian VM memory. Host synchronization state
 * is keyed by the guest address, so no host pointers leak into PS3 memory.
 * -----------------------------------------------------------------------*/
#define LWM_OWNER   0x00
#define LWM_WAITER  0x04
#define LWM_ATTR    0x08
#define LWM_RECUR   0x0C
#define LWM_SLEEPQ  0x10
#define LWC_MUTEX   0x00
#define LWC_QUEUE   0x04

static constexpr uint32_t LWM_MAX = 256;
static constexpr uint32_t LWC_MAX = 256;
static constexpr uint32_t SYS_SYNC_RECURSIVE = 0x10;

struct LwMutexSlot {
    std::recursive_timed_mutex host;
    std::mutex state_lock;
    std::atomic<bool> in_use{false};
    std::atomic<uint32_t> guest_addr{0};
    uint64_t owner = 0;
    uint32_t recursion = 0;
    bool recursive = false;
};

struct LwCondSlot {
    std::condition_variable_any cv;
    std::atomic<bool> in_use{false};
    std::atomic<uint32_t> guest_addr{0};
    std::atomic<uint64_t> generation{0};
    std::atomic<uint32_t> waiters{0};
    uint32_t mutex_slot = 0;
};

static LwMutexSlot s_lwmutex[LWM_MAX];
static LwCondSlot s_lwcond[LWC_MAX];
static std::mutex s_lw_registry_lock;
static std::atomic<uint32_t> s_lwcond_wait_traces{0};

/* UC3_SYNC_STATS : compteurs NON-CAPES wait/signal par slot lwcond, dump
 * toutes les ~5s. But: voir si les conds FIOS recoivent des signals sans
 * waiter (wakeup perdu) ou des waits sans signal (queue affamee) post-load.
 * Read-only, env-gated. */
static std::atomic<uint64_t> s_cnt_wait[LWC_MAX];
static std::atomic<uint64_t> s_cnt_signal[LWC_MAX];
static void uc3_sync_stats_note(LwCondSlot* slot, bool is_signal)
{
    static const bool on = getenv("UC3_SYNC_STATS") != nullptr;
    if (!on || !slot) return;
    size_t idx = (size_t)(slot - s_lwcond);
    if (idx >= LWC_MAX) return;
    (is_signal ? s_cnt_signal[idx] : s_cnt_wait[idx]).fetch_add(1, std::memory_order_relaxed);
    /* dump periodique par le premier thread qui passe apres l'intervalle */
    static std::atomic<uint64_t> last_ms{0};
    uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t prev = last_ms.load(std::memory_order_relaxed);
    if (now - prev >= 5000 && last_ms.compare_exchange_strong(prev, now)) {
        fprintf(stderr, "[sync-stats] t=%llums", (unsigned long long)now);
        for (size_t i = 0; i < LWC_MAX; ++i) {
            uint64_t w = s_cnt_wait[i].load(std::memory_order_relaxed);
            uint64_t s = s_cnt_signal[i].load(std::memory_order_relaxed);
            if (!w && !s) continue;
            fprintf(stderr, " [%zu]0x%08X w=%llu s=%llu",
                    i, s_lwcond[i].guest_addr.load(std::memory_order_relaxed),
                    (unsigned long long)w, (unsigned long long)s);
        }
        fprintf(stderr, "\n");
    }
}

static uint64_t current_tid(const ppu_context* ctx)
{
    return ctx->thread_id ? ctx->thread_id : 1;
}

static void sync_result(ppu_context* ctx, uint32_t result)
{
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)result;
}

static LwMutexSlot* find_lwmutex(uint32_t guest_addr, uint32_t* index = nullptr)
{
    for (uint32_t i = 0; i < LWM_MAX; i++) {
        if (s_lwmutex[i].in_use.load(std::memory_order_acquire) &&
            s_lwmutex[i].guest_addr.load(std::memory_order_relaxed) == guest_addr) {
            if (index) *index = i;
            return &s_lwmutex[i];
        }
    }
    return nullptr;
}

extern "C" void ppu_dump_guest_caller(uint32_t tag);

static LwCondSlot* find_lwcond(uint32_t guest_addr)
{
    for (uint32_t i = 0; i < LWC_MAX; i++) {
        if (s_lwcond[i].in_use.load(std::memory_order_acquire) &&
            s_lwcond[i].guest_addr.load(std::memory_order_relaxed) == guest_addr)
            return &s_lwcond[i];
    }
    return nullptr;
}

static void sys_lwmutex_create(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    uint32_t attr = (uint32_t)ctx->gpr[4];
    if (!lwm) { sync_result(ctx, CELL_EFAULT); return; }

    std::lock_guard<std::mutex> registry(s_lw_registry_lock);
    if (find_lwmutex(lwm)) { sync_result(ctx, CELL_EBUSY); return; }

    uint32_t protocol = attr ? vm_read32(attr + 0) : 0;
    uint32_t recursive = attr ? vm_read32(attr + 4) : 0;
    for (uint32_t i = 0; i < LWM_MAX; i++) {
        LwMutexSlot& slot = s_lwmutex[i];
        if (slot.in_use.load(std::memory_order_relaxed)) continue;

        {
            std::lock_guard<std::mutex> state(slot.state_lock);
            slot.owner = 0;
            slot.recursion = 0;
            slot.recursive = recursive == SYS_SYNC_RECURSIVE;
        }
        slot.guest_addr.store(lwm, std::memory_order_relaxed);
        slot.in_use.store(true, std::memory_order_release);

        vm_write32(lwm + LWM_OWNER, 0);
        vm_write32(lwm + LWM_WAITER, 0);
        vm_write32(lwm + LWM_ATTR, protocol | recursive);
        vm_write32(lwm + LWM_RECUR, 0);
        vm_write32(lwm + LWM_SLEEPQ, i + 1);
        vm_write32(lwm + 0x14, 0);
        fprintf(stderr, "[sync] lwmutex_create guest=0x%08X slot=%u recursive=%u\n",
                lwm, i, slot.recursive ? 1u : 0u);
        sync_result(ctx, CELL_OK);
        return;
    }
    sync_result(ctx, CELL_EAGAIN);
}

static void sys_lwmutex_destroy(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    LwMutexSlot* slot = find_lwmutex(lwm);
    if (!slot) { sync_result(ctx, CELL_ESRCH); return; }

    std::lock_guard<std::mutex> registry(s_lw_registry_lock);
    std::lock_guard<std::mutex> state(slot->state_lock);
    if (slot->owner) { sync_result(ctx, CELL_EBUSY); return; }
    slot->in_use.store(false, std::memory_order_release);
    slot->guest_addr.store(0, std::memory_order_relaxed);
    memset(vm_base + lwm, 0, 24);
    sync_result(ctx, CELL_OK);
}

static void sys_lwmutex_lock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    uint64_t timeout = ctx->gpr[4];
    LwMutexSlot* slot = find_lwmutex(lwm);
    if (!slot) { sync_result(ctx, CELL_ESRCH); return; }

    uint64_t tid = current_tid(ctx);
    {
        std::lock_guard<std::mutex> state(slot->state_lock);
        if (slot->owner == tid && !slot->recursive) {
            sync_result(ctx, CELL_EDEADLK);
            return;
        }
    }

    bool locked = true;
    if (timeout == 0) {
        slot->host.lock();
    } else {
        locked = slot->host.try_lock_for(std::chrono::microseconds(timeout));
    }
    if (!locked) { sync_result(ctx, CELL_ETIMEDOUT); return; }

    uint32_t recursion;
    {
        std::lock_guard<std::mutex> state(slot->state_lock);
        slot->owner = tid;
        recursion = ++slot->recursion;
    }
    vm_write32(lwm + LWM_OWNER, (uint32_t)tid);
    vm_write32(lwm + LWM_RECUR, recursion);
    sync_result(ctx, CELL_OK);
}

static void sys_lwmutex_trylock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    LwMutexSlot* slot = find_lwmutex(lwm);
    if (!slot) { sync_result(ctx, CELL_ESRCH); return; }

    uint64_t tid = current_tid(ctx);
    {
        std::lock_guard<std::mutex> state(slot->state_lock);
        if (slot->owner == tid && !slot->recursive) {
            sync_result(ctx, CELL_EDEADLK);
            return;
        }
    }
    if (!slot->host.try_lock()) { sync_result(ctx, CELL_EBUSY); return; }

    uint32_t recursion;
    {
        std::lock_guard<std::mutex> state(slot->state_lock);
        slot->owner = tid;
        recursion = ++slot->recursion;
    }
    vm_write32(lwm + LWM_OWNER, (uint32_t)tid);
    vm_write32(lwm + LWM_RECUR, recursion);
    sync_result(ctx, CELL_OK);
}

static void sys_lwmutex_unlock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    LwMutexSlot* slot = find_lwmutex(lwm);
    if (!slot) { sync_result(ctx, CELL_ESRCH); return; }

    uint64_t tid = current_tid(ctx);
    uint32_t recursion;
    {
        std::lock_guard<std::mutex> state(slot->state_lock);
        if (slot->owner != tid || slot->recursion == 0) {
            sync_result(ctx, CELL_EMUTEX_UNLOCK_NOT_OWNED);
            return;
        }
        recursion = --slot->recursion;
        if (!recursion) slot->owner = 0;
    }
    vm_write32(lwm + LWM_RECUR, recursion);
    if (!recursion) vm_write32(lwm + LWM_OWNER, 0);
    slot->host.unlock();
    sync_result(ctx, CELL_OK);
}

static void sys_lwcond_create(ppu_context* ctx)
{
    uint32_t lwc = (uint32_t)ctx->gpr[3];
    uint32_t lwm = (uint32_t)ctx->gpr[4];
    if (!lwc || !lwm) { sync_result(ctx, CELL_EFAULT); return; }

    uint32_t mutex_index;
    if (!find_lwmutex(lwm, &mutex_index)) { sync_result(ctx, CELL_ESRCH); return; }

    std::lock_guard<std::mutex> registry(s_lw_registry_lock);
    if (find_lwcond(lwc)) { sync_result(ctx, CELL_EBUSY); return; }
    for (uint32_t i = 0; i < LWC_MAX; i++) {
        LwCondSlot& slot = s_lwcond[i];
        if (slot.in_use.load(std::memory_order_relaxed)) continue;
        slot.mutex_slot = mutex_index;
        slot.generation.store(0, std::memory_order_relaxed);
        slot.waiters.store(0, std::memory_order_relaxed);
        slot.guest_addr.store(lwc, std::memory_order_relaxed);
        slot.in_use.store(true, std::memory_order_release);
        vm_write32(lwc + LWC_MUTEX, lwm);
        vm_write32(lwc + LWC_QUEUE, i + 1);
        fprintf(stderr, "[sync] lwcond_create guest=0x%08X mutex=0x%08X slot=%u\n",
                lwc, lwm, i);
        sync_result(ctx, CELL_OK);
        return;
    }
    sync_result(ctx, CELL_EAGAIN);
}

static void sys_lwcond_destroy(ppu_context* ctx)
{
    uint32_t lwc = (uint32_t)ctx->gpr[3];
    LwCondSlot* slot = find_lwcond(lwc);
    if (!slot) { sync_result(ctx, CELL_ESRCH); return; }
    if (slot->waiters.load(std::memory_order_acquire)) {
        sync_result(ctx, CELL_EBUSY);
        return;
    }
    std::lock_guard<std::mutex> registry(s_lw_registry_lock);
    slot->in_use.store(false, std::memory_order_release);
    slot->guest_addr.store(0, std::memory_order_relaxed);
    vm_write32(lwc + LWC_MUTEX, 0);
    vm_write32(lwc + LWC_QUEUE, 0);
    sync_result(ctx, CELL_OK);
}

/* LOST-WAKEUP FIX (2026-07-14, session 88d1fa94): publier l'incrément de
 * `generation` (la variable-prédicat lue par cv.wait dans sys_lwcond_wait) SOUS
 * le mutex associé. Sans ça, le modèle mémoire C++ autorise la PERTE du réveil
 * dans la fenêtre entre le pred-check du waiter (`generation != captured`) et son
 * blocage atomique → le waiter dort POUR TOUJOURS. Symptôme mesuré: ~50% des
 * boots calaient à ~3s, main parké dans ZwWaitForAlertByThreadId (course d'init
 * non-déterministe). host = recursive_timed_mutex → si le signaleur tient déjà le
 * lwmutex (pattern signal-sous-lock courant sur PS3), le re-verrou récursif ne
 * deadlocke pas. notify HORS du lock (idiome standard: évite de réveiller un
 * thread qui se rebloquerait aussitôt sur le lock). */
static void uc3_trace_lwcond_signal(const char* kind, ppu_context* ctx,
                                    uint32_t lwc, const LwCondSlot* slot)
{
    if (!getenv("UC3_SIGNAL_TRACE")) return;
    static const uint32_t filter = [] {
        const char* value = getenv("UC3_SIGNAL_TRACE_ADDR");
        return value ? (uint32_t)strtoul(value, nullptr, 0) : 0u;
    }();
    if (filter && filter != lwc) return;

    static std::atomic<uint32_t> traces{0};
    uint32_t n = traces.fetch_add(1, std::memory_order_relaxed);
    if (n >= 8192u) return;
    uint64_t timestamp_us = (uint64_t)std::chrono::duration_cast<
        std::chrono::microseconds>(std::chrono::steady_clock::now()
                                       .time_since_epoch())
                                .count();
    fprintf(stderr,
            "[sig] ts_us=%llu kind=%s guest=0x%08X waiters=%d tid=%llu "
            "lr=0x%08X\n",
            (unsigned long long)timestamp_us, kind, lwc,
            (int)slot->waiters.load(std::memory_order_acquire),
            (unsigned long long)ctx->thread_id, (uint32_t)ctx->lr);
    if (getenv("UC3_SIGNAL_CALLER")) {
        static std::atomic<uint32_t> caller_traces{0};
        if (caller_traces.fetch_add(1, std::memory_order_relaxed) < 8u)
            ppu_dump_guest_caller(lwc);
    }
}

static void sys_lwcond_signal(ppu_context* ctx)
{
    uint32_t lwc = (uint32_t)ctx->gpr[3];
    LwCondSlot* slot = find_lwcond(lwc);
    if (!slot) { sync_result(ctx, CELL_ESRCH); return; }
    uc3_sync_stats_note(slot, true);
    {
        std::lock_guard<std::recursive_timed_mutex>
            lk(s_lwmutex[slot->mutex_slot].host);
        slot->generation.fetch_add(1, std::memory_order_release);
    }
    slot->cv.notify_one();
    uc3_trace_lwcond_signal("one", ctx, lwc, slot);
    sync_result(ctx, CELL_OK);
}

static void sys_lwcond_signal_all(ppu_context* ctx)
{
    uint32_t lwc = (uint32_t)ctx->gpr[3];
    LwCondSlot* slot = find_lwcond(lwc);
    if (!slot) { sync_result(ctx, CELL_ESRCH); return; }
    uc3_sync_stats_note(slot, true);
    {
        std::lock_guard<std::recursive_timed_mutex>
            lk(s_lwmutex[slot->mutex_slot].host);
        slot->generation.fetch_add(1, std::memory_order_release);
    }
    slot->cv.notify_all();
    uc3_trace_lwcond_signal("all", ctx, lwc, slot);
    sync_result(ctx, CELL_OK);
}

/* Signal host-side d'un condvar guest par adresse (sans ppu_context) — pour
 * l'exécuteur SPU déterministe (uc3_spu_exec.cpp): quand un job frame que le
 * main attend est RÉELLEMENT exécuté, réveiller le main sur son lwcond FIOS
 * (resume de fibre authentique via la primitive réelle, PAS un forçage
 * mémoire). Réveil spurieux sûr: le guest re-teste son prédicat et se re-bloque
 * si non satisfait. Réutilise la même publication-sous-mutex que le fix
 * lost-wakeup. Retour: 0 si signalé, -1 si cond inconnu. */
extern "C" int uc3_lwcond_signal_host(unsigned int lwc)
{
    LwCondSlot* slot = find_lwcond(lwc);
    if (!slot) return -1;
    uc3_sync_stats_note(slot, true);
    {
        std::lock_guard<std::recursive_timed_mutex>
            lk(s_lwmutex[slot->mutex_slot].host);
        slot->generation.fetch_add(1, std::memory_order_release);
    }
    slot->cv.notify_all();
    return 0;
}

static void sys_lwcond_wait(ppu_context* ctx)
{
    uint32_t lwc = (uint32_t)ctx->gpr[3];
    uint64_t timeout = ctx->gpr[4];
    uint32_t trace_no = s_lwcond_wait_traces.fetch_add(1, std::memory_order_relaxed);
    /* UC3_SYNC_TRACE_MAX: plafond de trace configurable (défaut 128) — la fenêtre
     * FIOS post-save (op notify -> routeur -> flag2) est bien au-delà de 128. */
    static const uint32_t s_trace_max = [] {
        const char* e = getenv("UC3_SYNC_TRACE_MAX");
        return e ? (uint32_t)strtoul(e, nullptr, 0) : 128u;
    }();
    bool trace = trace_no < s_trace_max;
    /* [cond-caller] identifier la fonction GUEST qui attend sur cette condvar
     * (LR = adresse de retour guest) — pour trouver la condition frontend
     * jamais satisfaite (blocage main thread). Cap par condvar. */
    if (getenv("UC3_COND_CALLER")) {
        static std::atomic<int> s_cc{0};
        uint64_t wtid = current_tid(ctx);
        /* Back-chain de la pile GUEST (r1) : remonter les frames PPC64
         * (*(r1)=frame precedent, LR sauve a +0x10) et sortir les adresses
         * dans la plage code guest (0x10000-0x1400000) = les func_ appelants.
         * Fire cap 30x pour tid=1 (main) — capture aussi le wait au BLOCAGE,
         * pas seulement l'init (l'ancien one-shot ratait le blocage). */
        /* Échantillonnage THROTTLÉ des waits du main (1/16, cap 60): couvre
         * early-boot ET la fenêtre post-Init (l'ancien cap-30-des-premiers
         * épuisait avant la fenêtre décisive; UC3_CC_SKIP=n décale encore). */
        static const int s_cc_skip = [] {
            const char* e = getenv("UC3_CC_SKIP");
            return e ? atoi(e) : 0;
        }();
        static std::atomic<int> s_cc_printed{0};
        int _ccn = s_cc.fetch_add(1);
        if (wtid == 1 && _ccn >= s_cc_skip && (_ccn & 15) == 0 &&
            s_cc_printed.fetch_add(1) < 60) {
            uint32_t sp = (uint32_t)ctx->gpr[1];
            fprintf(stderr, "[cond-caller] MAIN wait cond=0x%08X mutex=0x%08X "
                    "r1=0x%08X lr=0x%08X callers:", lwc, vm_read32(lwc + 4), sp,
                    (uint32_t)ctx->lr);
            for (int f = 0; f < 12 && sp >= 0x10000u && sp < 0x40000000u; ++f) {
                uint32_t lr = vm_read32(sp + 0x10);
                if (lr >= 0x00010000u && lr < 0x01400000u)
                    fprintf(stderr, " 0x%08X", lr);
                uint32_t nsp = vm_read32(sp);
                if (nsp <= sp) break;   /* back-chain croissante */
                sp = nsp;
            }
            fprintf(stderr, "\n");
            /* Dump complet des GPR : dans le contexte fibre SPURS la back-chain
             * n'est pas walkable, mais un registre non-volatile (r14-r31) pointe
             * vraisemblablement vers la structure SPURS dont la CONDITION est
             * relue apres reveil. Objectif = trouver la variable-condition +
             * scanner *(reg + 0..0x40) pour reperer un flag/compteur candidat. */
            if (lwc == 0x3115BFA8u) {
                fprintf(stderr, "   [cc-regs] cond=0x3115BFA8:");
                for (int r = 3; r <= 31; ++r)
                    fprintf(stderr, " r%d=0x%08X", r, (uint32_t)ctx->gpr[r]);
                fprintf(stderr, "\n");
                /* pour chaque registre qui pointe dans le heap frontend
                 * (0x3000_0000-0x3200_0000), dump les 8 premiers mots — un est
                 * la condition que le main attend. */
                for (int r = 14; r <= 31; ++r) {
                    uint32_t p = (uint32_t)ctx->gpr[r];
                    /* heap frontend (0x30xx) ET region statique/guest (0x01xx)
                     * — la condition de sortie de boucle est en statique
                     * (r16/r19/r24 pointent 0x011Exxxx/0x0135xxxx). */
                    if ((p >= 0x30000000u && p < 0x32000000u) ||
                        (p >= 0x01000000u && p < 0x01400000u)) {
                        fprintf(stderr, "   [cc-mem] r%d=0x%08X ->", r, p);
                        for (int k = 0; k < 12; ++k)
                            fprintf(stderr, " +0x%X=0x%08X", k*4, vm_read32(p + k*4));
                        fprintf(stderr, "\n");
                    }
                }
            }
        }
    }
    LwCondSlot* cond = find_lwcond(lwc);
    if (cond) uc3_sync_stats_note(cond, false);
    if (!cond) {
        if (trace) fprintf(stderr, "[sync] lwcond_wait #%u tid=%llu guest=0x%08X timeout=%llu -> ESRCH (mem=%08X/%08X)\n",
                           trace_no, (unsigned long long)current_tid(ctx), lwc,
                           (unsigned long long)timeout, vm_read32(lwc), vm_read32(lwc + 4));
        sync_result(ctx, CELL_ESRCH);
        return;
    }

    LwMutexSlot* mutex = &s_lwmutex[cond->mutex_slot];
    uint32_t lwm = mutex->guest_addr.load(std::memory_order_relaxed);
    uint64_t tid = current_tid(ctx);
    {
        std::lock_guard<std::mutex> state(mutex->state_lock);
        if (mutex->owner != tid || mutex->recursion == 0) {
            if (trace) fprintf(stderr, "[sync] lwcond_wait #%u tid=%llu guest=0x%08X mutex=0x%08X timeout=%llu -> NOT_OWNED (owner=%llu recur=%u, guest=%08X/%08X)\n",
                               trace_no, (unsigned long long)tid, lwc, lwm,
                               (unsigned long long)timeout,
                               (unsigned long long)mutex->owner, mutex->recursion,
                               vm_read32(lwm + LWM_OWNER), vm_read32(lwm + LWM_RECUR));
            sync_result(ctx, CELL_ECOND_NOT_OWNED);
            return;
        }
        if (mutex->recursion != 1) {
            if (trace) fprintf(stderr, "[sync] lwcond_wait #%u tid=%llu guest=0x%08X mutex=0x%08X -> EBUSY (recur=%u)\n",
                               trace_no, (unsigned long long)tid, lwc, lwm, mutex->recursion);
            sync_result(ctx, CELL_EBUSY);
            return;
        }
        mutex->owner = 0;
        mutex->recursion = 0;
    }
    vm_write32(lwm + LWM_OWNER, 0);
    vm_write32(lwm + LWM_RECUR, 0);

    uint64_t generation = cond->generation.load(std::memory_order_acquire);
    cond->waiters.fetch_add(1, std::memory_order_acq_rel);
    std::unique_lock<std::recursive_timed_mutex> lock(mutex->host, std::adopt_lock);
    bool signaled = true;
    /* Robustesse OPT-IN (UC3_FIOS_TIMEOUT): borne l'attente du condvar FIOS
     * 0x3115BFA8 à ~4ms puis rend la main au guest (réveil spurieux = sémantique
     * condvar standard). NOTE: la mesure [hb-ip]/UC3_MAIN_SAMPLE de 2026-07-14 a
     * RÉFUTÉ l'hypothèse « le main park sur ce condvar » — au vrai gate menu le
     * main BUSY-SPIN dans func_00D51D60 (frame-sync des rings SPURS wid=2/4), il
     * n'est PAS dans lwcond_wait. Ce timeout est donc inerte sur le gate courant;
     * gardé opt-in comme filet pour d'éventuels blocages lwcond réels. */
    const bool fios_cap = (lwc == 0x3115BFA8u) && (timeout == 0) &&
                          getenv("UC3_FIOS_TIMEOUT");
    if (timeout == 0 && !fios_cap) {
        cond->cv.wait(lock, [&] {
            return cond->generation.load(std::memory_order_acquire) != generation ||
                   !cond->in_use.load(std::memory_order_acquire);
        });
    } else {
        uint64_t eff = fios_cap ? 4000u : timeout;   /* µs */
        bool woke = cond->cv.wait_for(lock, std::chrono::microseconds(eff), [&] {
            return cond->generation.load(std::memory_order_acquire) != generation ||
                   !cond->in_use.load(std::memory_order_acquire);
        });
        /* Cap FIOS: un timeout = réveil spurieux -> retourner OK pour que le
         * guest re-teste son prédicat (et non ETIMEDOUT qu'il traiterait en
         * erreur). Vrai timeout guest: préserver la sémantique ETIMEDOUT. */
        signaled = fios_cap ? true : woke;
    }
    cond->waiters.fetch_sub(1, std::memory_order_acq_rel);
    lock.release(); /* The guest returns with its lwmutex held. */

    {
        std::lock_guard<std::mutex> state(mutex->state_lock);
        mutex->owner = tid;
        mutex->recursion = 1;
    }
    vm_write32(lwm + LWM_OWNER, (uint32_t)tid);
    vm_write32(lwm + LWM_RECUR, 1);
    if (trace) fprintf(stderr, "[sync] lwcond_wait #%u tid=%llu guest=0x%08X mutex=0x%08X timeout=%llu -> %s\n",
                       trace_no, (unsigned long long)tid, lwc, lwm,
                       (unsigned long long)timeout, signaled ? "OK" : "ETIMEDOUT");
    sync_result(ctx, signaled ? CELL_OK : CELL_ETIMEDOUT);
}

static void sys_lwcond_signal_to(ppu_context* ctx)
{
    /* Targeted wakeups are uncommon during boot; waking one preserves the
     * condition-variable semantics until per-waiter thread IDs are needed. */
    sys_lwcond_signal(ctx);
}

/* sys_ppu_thread_get_id(vm::ptr<u64> id). */
static void sys_ppu_thread_get_id(ppu_context* ctx)
{
    uint32_t p = (uint32_t)ctx->gpr[3];
    if (p) vm_write64(p, current_tid(ctx));
    ctx->gpr[3] = 0;
}

/* sys_mmapper_allocate_memory(u32 size, u64 flags, vm::ptr<u32> mem_id) ->
 * hand back a unique opaque id; the backing is the flat VM, so the later
 * search_and_map just needs a non-zero id to track. */
/* mem_id -> size table so search_and_map can hand back a region of the right
 * size. id starts at 0x1000; index = id - 0x1000. */
static uint32_t s_mmap_size[0x1000];
static uint32_t s_mmap_next_id = 0x1000;
static uint32_t s_mmap_va = 0x80000000u;   /* bump VA for search_and_map (flat VM, high to avoid the game's 0x30-0x60M regions + malloc bump 0x40M) */

static void sys_mmapper_allocate_memory(ppu_context* ctx)
{
    uint32_t size       = (uint32_t)ctx->gpr[3];
    uint32_t mem_id_ptr = (uint32_t)ctx->gpr[5];   /* (size, flags, mem_id*) */
    uint32_t id = s_mmap_next_id++;
    if (id - 0x1000 < 0x1000) s_mmap_size[id - 0x1000] = size;
    if (mem_id_ptr) vm_write32(mem_id_ptr, id);
    ctx->gpr[3] = 0;
}

/* sys_mmapper_allocate_memory_from_container(size, cid, flags, mem_id*) — same
 * as allocate_memory; the container is irrelevant with the flat VM. The game
 * uses this for its object pools; leaving it unbridged returned 0 without
 * writing mem_id -> NULL regions -> NULL singletons -> corruption cascade. */
static void sys_mmapper_allocate_memory_from_container(ppu_context* ctx)
{
    uint32_t size       = (uint32_t)ctx->gpr[3];
    uint32_t mem_id_ptr = (uint32_t)ctx->gpr[6];   /* (size, cid, flags, mem_id*) */
    uint32_t id = s_mmap_next_id++;
    if (id - 0x1000 < 0x1000) s_mmap_size[id - 0x1000] = size;
    if (mem_id_ptr) vm_write32(mem_id_ptr, id);
    ctx->gpr[3] = 0;
}

/* sys_mmapper_search_and_map(start_addr, mem_id, flags, alloc_addr*) — find a
 * free VA for the mem_id's region and return it. With the flat 4 GB VM any VA
 * is usable (committed on demand), so just bump-allocate one. */
static void sys_mmapper_search_and_map(ppu_context* ctx)
{
    uint32_t mem_id     = (uint32_t)ctx->gpr[4];
    uint32_t addr_out   = (uint32_t)ctx->gpr[6];
    uint32_t size = (mem_id - 0x1000 < 0x1000) ? s_mmap_size[mem_id - 0x1000] : 0x100000;
    if (size == 0) size = 0x100000;
    s_mmap_va = (s_mmap_va + 0xFFFFFu) & ~0xFFFFFu;   /* 1 MB align */
    uint32_t va = s_mmap_va;
    s_mmap_va += (size + 0xFFFFFu) & ~0xFFFFFu;
    if (addr_out) vm_write32(addr_out, va);
    ctx->gpr[3] = 0;
}

/* A handful of CRT helpers the early boot tends to hit; accept and continue. */
static void crt_ok(ppu_context* ctx) { ctx->gpr[3] = 0; }

/* sys_prx_get_module_list(flags, info)
 *
 * This runtime does not load guest PRX images yet, so the truthful result is
 * an empty list. Returning CELL_OK without writing info->count made callers
 * walk stale IDs and module-info buffers indefinitely. The guest structure is:
 *   u64 size; u32 max; u32 count; u32 idlist;
 */
static void sys_prx_get_module_list(ppu_context* ctx)
{
    uint32_t info = (uint32_t)ctx->gpr[4];
    if (!info) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80011324; /* PRX INVAL */
        return;
    }

    uint32_t max = vm_read32(info + 0x08);
    uint32_t idlist = vm_read32(info + 0x10);
    if (idlist && max) {
        if (max > 4096) max = 4096;
        memset(vm_base + idlist, 0, (size_t)max * sizeof(uint32_t));
    }
    vm_write32(info + 0x0C, 0);
    ctx->gpr[3] = 0;
}

static void sys_prx_get_module_info(ppu_context* ctx)
{
    /* No module IDs can be returned by the empty-list implementation. */
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8001112E;
}

/* PPU thread family lives in syscalls/sys_ppu_thread.c (returns int64 in r3).
 * The game imports these through sysPrxForUser (not as raw syscalls), so they
 * MUST be registered here too — otherwise sys_ppu_thread_create hit the
 * unresolved-import stub and returned fake CELL_OK *without spawning a thread*,
 * so worker threads never ran and the boot deadlocked in sys_event_flag_wait
 * waiting for a signal that would never come. */
extern "C" int64_t sys_ppu_thread_create(ppu_context*);
extern "C" int64_t sys_ppu_thread_exit(ppu_context*);
extern "C" int64_t sys_ppu_thread_join(ppu_context*);
extern "C" int64_t sys_ppu_thread_detach(ppu_context*);
extern "C" int64_t sys_ppu_thread_yield(ppu_context*);
static void hle_ppu_thread_create(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)sys_ppu_thread_create(ctx); }
static void hle_ppu_thread_exit(ppu_context* ctx)   { (void)sys_ppu_thread_exit(ctx); }
static void hle_ppu_thread_join(ppu_context* ctx)   { ctx->gpr[3] = (uint64_t)sys_ppu_thread_join(ctx); }
static void hle_ppu_thread_detach(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)sys_ppu_thread_detach(ctx); }
static void hle_ppu_thread_yield(ppu_context* ctx)  { (void)sys_ppu_thread_yield(ctx); }

extern "C" void ppu_sysprx_register(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("sys_initialize_tls"),       "sys_initialize_tls",       sys_initialize_tls);
    ps3_hle_register_ctx(ps3_compute_nid("sys_time_get_system_time"), "sys_time_get_system_time", sys_time_get_system_time);
    ps3_hle_register_ctx(ps3_compute_nid("sys_process_is_stack"),     "sys_process_is_stack",     sys_process_is_stack);
    /* Atexit registration: nothing to do at boot, just succeed. */
    ps3_hle_register_ctx(ps3_compute_nid("_sys_process_atexitspawn"), "_sys_process_atexitspawn", crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("_sys_process_at_Exitspawn"),"_sys_process_at_Exitspawn",crt_ok);

    /* Lightweight mutex family (guards global/singleton init in the CRT). */
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_create"),  "sys_lwmutex_create",  sys_lwmutex_create);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_destroy"), "sys_lwmutex_destroy", sys_lwmutex_destroy);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_lock"),    "sys_lwmutex_lock",    sys_lwmutex_lock);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_unlock"),  "sys_lwmutex_unlock",  sys_lwmutex_unlock);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_trylock"), "sys_lwmutex_trylock", sys_lwmutex_trylock);

    /* Thread id + memory manager (high-frequency boot imports). The flat VM
     * means map/unmap/free are no-ops: the memory already exists everywhere. */
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_get_id"),      "sys_ppu_thread_get_id",      sys_ppu_thread_get_id);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_allocate_memory"), "sys_mmapper_allocate_memory", sys_mmapper_allocate_memory);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_allocate_memory_from_container"), "sys_mmapper_allocate_memory_from_container", sys_mmapper_allocate_memory_from_container);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_search_and_map"), "sys_mmapper_search_and_map", sys_mmapper_search_and_map);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_map_memory"),     "sys_mmapper_map_memory",     crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_unmap_memory"),   "sys_mmapper_unmap_memory",   crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_free_memory"),    "sys_mmapper_free_memory",    crt_ok);

    /* PPU thread family imported via sysPrxForUser (the boot-critical gap:
     * sys_ppu_thread_create must actually spawn a host thread). */
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_create"), "sys_ppu_thread_create", hle_ppu_thread_create);
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_exit"),   "sys_ppu_thread_exit",   hle_ppu_thread_exit);
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_join"),   "sys_ppu_thread_join",   hle_ppu_thread_join);
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_detach"), "sys_ppu_thread_detach", hle_ppu_thread_detach);
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_yield"),  "sys_ppu_thread_yield",  hle_ppu_thread_yield);

    /* PRX module enumeration used by boot memory accounting. */
    ps3_hle_register_ctx(ps3_compute_nid("sys_prx_get_module_list"), "sys_prx_get_module_list", sys_prx_get_module_list);
    ps3_hle_register_ctx(ps3_compute_nid("sys_prx_get_module_info"), "sys_prx_get_module_info", sys_prx_get_module_info);

    /* Lightweight conditions need real blocking semantics once worker threads
     * start; a success-only wait turns every scheduler into a busy loop. */
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_create"),     "sys_lwcond_create",     sys_lwcond_create);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_destroy"),    "sys_lwcond_destroy",    sys_lwcond_destroy);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_wait"),       "sys_lwcond_wait",       sys_lwcond_wait);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_signal"),     "sys_lwcond_signal",     sys_lwcond_signal);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_signal_all"), "sys_lwcond_signal_all", sys_lwcond_signal_all);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_signal_to"),  "sys_lwcond_signal_to",  sys_lwcond_signal_to);
}
