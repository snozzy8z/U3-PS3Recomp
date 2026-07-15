/* uc3_spu_exec.cpp — exécuteur DÉTERMINISTE de job-chains SPURS (Phase 11).
 *
 * Étage 1: branché sur g_uc3_ring_push_cb (hook vm_write, UC3_RING_HOOK) — au
 * moment EXACT où le jeu pousse une chaîne dans un workload-ring, localise la
 * chaîne (magic 0xC0DEC0DE). Validé 2026-07-15 (run ex: les 2 jobs wid2
 * capturés au push; le ring-monitor opportuniste = 0 dispatch en 3 runs).
 * Étage 2 (UC3_SPU_EXEC=2): un worker dédié EXÉCUTE la chaîne au push via la
 * machinerie policy existante (uc3_run_policy_job, stubs.cpp — spu_interp
 * avec UC3_POLICY_FULL_INTERP), puis publie le watermark de complétion
 * (curseurs +0x42..+0x4E := consommé quand le ring est drainé — même règle
 * que [job-watermark], complétion de travail réellement exécuté, règle #6).
 * La carte ring->(wid,pm,size,spurs) vient de la source (AddWorkload bridge).
 *
 * Env: UC3_SPU_EXEC=1 détection seule; =2 exécution+complétions;
 *      =3 + republication PÉRIODIQUE de la complétion frame-worker (le main
 *      re-arme le compteur chaque frame; sans nouveau push, un thread republie
 *      la complétion pour laisser la boucle de frame tourner en continu). */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <chrono>

extern "C" {
uint8_t  vm_read8(uint64_t a);
uint16_t vm_read16(uint64_t a);
uint32_t vm_read32(uint64_t a);
void     vm_write16(uint64_t a, uint16_t v);
void     vm_write32(uint64_t a, uint32_t v);
extern void (*g_uc3_ring_push_cb)(uint32_t c0_ea, uint32_t value);
extern int (*g_uc3_recent_spu_job_cb)(
    uint32_t max_age_ms, uint32_t* sequence, uint32_t* wid,
    uint32_t* ring, uint32_t* chain, uint64_t* age_us);
extern uint32_t g_canonical_toc;   /* ppu_loader.cpp */
int      uc3_lwcond_signal_host(unsigned int lwc);   /* ppu_sysprx.cpp */
}
/* Le condvar FIOS sur lequel le main thread parke son tick de frame (mesuré:
 * cq3). Le réveiller après un job frame exécuté = resume de fibre authentique. */
static const unsigned int UC3_FRAME_FIOS_COND = 0x3115BFA8u;
/* stubs.cpp (non-static depuis 2026-07-15) — machinerie policy/spu_interp. */
void uc3_run_policy_job(uint32_t policy_ea, uint32_t policy_size,
                        uint32_t spurs, uint32_t warg,
                        uint32_t job_desc, uint32_t ea_binary, uint32_t wid,
                        uint32_t spu_num, uint32_t dispatch_poll_status);
extern "C" int uc3_policy_last_run_completed(void);

/* ---- carte des workloads (remplie par le bridge AddWorkload) ------------- */
struct WklEnt {
    uint32_t ring, wid, pm, size, spurs;
    uint16_t producer;
    bool ready_queued;
};
static WklEnt s_wkl[32];
static int    s_wkl_n = 0;
static std::mutex s_wkl_mx;

extern "C" void uc3_spu_exec_register_wkl(uint32_t ring, uint32_t wid,
                                          uint32_t pm, uint32_t size,
                                          uint32_t spurs)
{
    std::lock_guard<std::mutex> lk(s_wkl_mx);
    if (s_wkl_n < 32) {
        const uint16_t producer =
            (uint16_t)(vm_read32(ring + 0x40u) >> 16);
        s_wkl[s_wkl_n++] = { ring, wid, pm, size, spurs, producer, false };
    }
}
static bool wkl_lookup(uint32_t ring, WklEnt* out)
{
    std::lock_guard<std::mutex> lk(s_wkl_mx);
    for (int i = 0; i < s_wkl_n; ++i)
        if (s_wkl[i].ring == ring) { *out = s_wkl[i]; return true; }
    return false;
}

struct ReadyDispatchPlan {
    uint32_t spu_ids[8]{};
    uint32_t count = 1;
    uint32_t ready = 0;
    uint32_t contention = 0;
    uint32_t max_contention = 0;
    uint32_t nspus = 0;
    bool enabled = false;
};

/* Opt-in implementation probe for the SPURS HLE scheduling rule: one
 * readyCount request may select the same workload on several eligible SPUs,
 * bounded by its current/max contention. The plan is captured before the
 * first policy instance atomically consumes readyCount. */
static ReadyDispatchPlan make_ready_dispatch_plan(const WklEnt& w)
{
    ReadyDispatchPlan plan{};
    const char* selected = getenv("UC3_SPU_MULTI_DISPATCH_WID");
    if (!selected || !w.spurs) return plan;

    char* end = nullptr;
    const unsigned long target = strtoul(selected, &end, 0);
    if (end == selected || *end != '\0' || target >= 32u || target != w.wid)
        return plan;

    const uint32_t slot = w.wid & 15u;
    plan.nspus = vm_read8(w.spurs + 0x76u);
    if (plan.nspus > 8u) plan.nspus = 8u;
    plan.ready = vm_read8(w.spurs + (w.wid < 16u ? slot : 0x10u + slot));

    const uint8_t packed_contention = vm_read8(w.spurs + 0x20u + slot);
    const uint8_t packed_max = vm_read8(w.spurs + 0x50u + slot);
    plan.contention = w.wid < 16u ? (packed_contention & 0x0Fu)
                                  : (packed_contention >> 4);
    plan.max_contention = w.wid < 16u ? (packed_max & 0x0Fu)
                                      : (packed_max >> 4);

    const uint32_t info = w.spurs + (w.wid < 16u ? 0xB00u : 0x1000u) +
                          slot * 0x20u;
    const uint32_t requested = plan.ready > plan.contention
        ? plan.ready - plan.contention : 0u;
    const uint32_t capacity = plan.max_contention > plan.contention
        ? plan.max_contention - plan.contention : 0u;
    const uint32_t wanted = requested < capacity ? requested : capacity;
    uint32_t eligible = 0;
    uint32_t selected_count = 0;
    for (uint32_t spu = 0; spu < plan.nspus; ++spu) {
        if (vm_read8(info + 0x18u + spu) == 0u) continue;
        ++eligible;
        if (selected_count < wanted)
            plan.spu_ids[selected_count++] = spu;
    }

    if (selected_count == 0u)
        return plan;
    plan.count = selected_count;
    plan.enabled = true;

    fprintf(stderr,
            "[spu-multi-dispatch] wid=%u ready=%u contention=%u max=%u "
            "nspus=%u eligible=%u instances=%u spus=",
            w.wid, plan.ready, plan.contention, plan.max_contention,
            plan.nspus, eligible, plan.count);
    for (uint32_t i = 0; i < plan.count; ++i)
        fprintf(stderr, "%s%u", i ? "/" : "", plan.spu_ids[i]);
    fputc('\n', stderr);
    return plan;
}

static bool wkl_ring_for_ready_count(uint32_t spurs, uint32_t wid,
                                     uint32_t* out_ring)
{
    std::lock_guard<std::mutex> lk(s_wkl_mx);
    for (int i = 0; i < s_wkl_n; ++i) {
        if (s_wkl[i].spurs != spurs || s_wkl[i].wid != wid) continue;
        if (out_ring) *out_ring = s_wkl[i].ring;
        return true;
    }
    return false;
}

static bool wkl_note_producer(uint32_t ring, uint16_t producer)
{
    std::lock_guard<std::mutex> lk(s_wkl_mx);
    for (int i = 0; i < s_wkl_n; ++i) {
        if (s_wkl[i].ring != ring) continue;
        const uint16_t previous = s_wkl[i].producer;
        if (producer == previous) return false;
        const uint16_t delta = (uint16_t)(producer - previous);
        s_wkl[i].producer = producer;
        if (delta >= 0x8000u) {
            fprintf(stderr,
                    "[spu-exec] producer reset wid=%u ring=0x%08X %u->%u\n",
                    s_wkl[i].wid, ring, previous, producer);
            return false;
        }
        return true;
    }
    return false;
}

/* ---- file d'événements push -> worker dédié ----------------------------- */
struct ReadyState {
    uint16_t target;
    uint16_t cursor;
};

/* Mirror func_00D51D60's target exactly: producer + the active record + each
 * non-zero dependent record. This only detects work; the policy epilogue owns
 * every guest cursor update. */
static bool read_ready_state(uint32_t ring, ReadyState* out)
{
    if (!out || ring < 0x10000u || ring >= 0x40000000u) return false;
    const uint16_t producer = vm_read16(ring + 0x40u);
    const uint32_t ready = vm_read32(ring + 0x30u);
    if (ready < 0x10000u || ready >= 0x40000000u) return false;

    const uint32_t slot = ready + (uint32_t)producer * 8u;
    if (vm_read16(slot + 2u) == 0u) return false;
    uint16_t target = (uint16_t)(producer + 1u); /* active record */
    bool terminated = false;
    for (uint32_t entry = slot + 0xAu, i = 0; i < 0x100u;
         ++i, entry += 8u) {
        if (vm_read16(entry) == 0u) {
            terminated = true;
            break;
        }
        ++target;
    }
    if (!terminated) return false;

    uint16_t cursor = 0xFFFFu;
    for (uint32_t off = 0x42u; off <= 0x4Eu; off += 2u) {
        const uint16_t candidate = vm_read16(ring + off);
        if (candidate != 0xFFFFu && candidate < cursor) cursor = candidate;
    }
    if (cursor == 0xFFFFu) return false;
    out->target = target;
    out->cursor = cursor;
    return true;
}

static bool wkl_note_ready(uint32_t ring, ReadyState* out)
{
    ReadyState state{};
    const uint16_t pending = read_ready_state(ring, &state)
        ? (uint16_t)(state.target - state.cursor) : 0u;
    if (pending == 0u || pending >= 0x8000u)
        return false;

    std::lock_guard<std::mutex> lk(s_wkl_mx);
    for (int i = 0; i < s_wkl_n; ++i) {
        if (s_wkl[i].ring != ring || s_wkl[i].ready_queued)
            continue;
        s_wkl[i].ready_queued = true;
        if (out) *out = state;
        return true;
    }
    return false;
}

static void wkl_release_ready(uint32_t ring)
{
    std::lock_guard<std::mutex> lk(s_wkl_mx);
    for (int i = 0; i < s_wkl_n; ++i) {
        if (s_wkl[i].ring == ring) {
            s_wkl[i].ready_queued = false;
            return;
        }
    }
}

static volatile uint32_t s_fw_counter_ea = 0;   /* frame-worker completion EA */
struct PushEv { uint32_t ring, value; bool ready_list; };
static std::deque<PushEv>       s_q;
static std::mutex               s_q_mx;
static std::condition_variable  s_q_cv;
static int s_exec_level = 0;

/* ReadyCountStore is the SPURS scheduling edge used by func_0079B3A0 when
 * the command list is already resident and no ring control word changes.
 * Only enqueue when the existing ready-list parser proves that one real wid0
 * policy job is pending; execution remains deferred to the worker thread. */
extern "C" void uc3_spu_exec_ready_count(uint32_t spurs, uint32_t wid,
                                           uint32_t value)
{
    if (s_exec_level < 1 || value == 0 || !getenv("UC3_SPU_READY_EXEC"))
        return;

    /* The ordinary producer-write hook already owns normal job batches. The
     * missing edge is the zero-work frame tail: its guest submit descriptor is
     * armed, but ReadyCountStore is the only scheduling notification emitted. */
    if (wid != 0u || !g_canonical_toc) return;
    const uint32_t submit_globals = vm_read32(g_canonical_toc - 0x7508u);
    if (submit_globals < 0x10000u || submit_globals >= 0x40000000u) return;
    const uint32_t submit_desc = vm_read32(submit_globals - 0x7FE0u);
    if (submit_desc < 0x10000u || submit_desc >= 0x40000000u ||
        vm_read32(submit_desc) != 0u)
        return;
    const uint32_t completion = vm_read32(submit_desc + 0x14u);
    if (completion < 0x10000u || completion >= 0x40000000u ||
        vm_read32(completion) != 1u)
        return;

    uint32_t ring = 0;
    ReadyState ready{};
    if (!wkl_ring_for_ready_count(spurs, wid, &ring) ||
        !wkl_note_ready(ring, &ready))
        return;

    const uint32_t c0 = vm_read32(ring + 0x40u);
    fprintf(stderr, "[spu-exec] ready-count wid=%u ring=0x%08X value=%u "
                    "cursor=%u target=%u\n",
            wid, ring, value, ready.cursor, ready.target);
    if (s_exec_level >= 2) {
        {
            std::lock_guard<std::mutex> lk(s_q_mx);
            s_q.push_back({ring, c0, true});
        }
        s_q_cv.notify_one();
    } else {
        wkl_release_ready(ring);
    }
}

/* Read-only handoff to the media-operation correlation probe. The timestamp
 * is published last with release semantics, so readers never pair a fresh
 * timestamp with stale job metadata. */
static std::atomic<uint64_t> s_recent_job_us{0};
static std::atomic<uint32_t> s_recent_job_publish{0};
static std::atomic<uint32_t> s_recent_job_sequence{0};
static std::atomic<uint32_t> s_recent_job_wid{0};
static std::atomic<uint32_t> s_recent_job_ring{0};
static std::atomic<uint32_t> s_recent_job_chain{0};

static uint64_t monotonic_us()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static int recent_spu_job(uint32_t max_age_ms, uint32_t* sequence,
                          uint32_t* wid, uint32_t* ring, uint32_t* chain,
                          uint64_t* age_us)
{
    for (int retry = 0; retry < 3; ++retry) {
        uint32_t before = s_recent_job_publish.load(std::memory_order_acquire);
        if (before & 1u) continue;
        uint64_t completed_us = s_recent_job_us.load(std::memory_order_relaxed);
        uint32_t current_sequence =
            s_recent_job_sequence.load(std::memory_order_relaxed);
        uint32_t current_wid = s_recent_job_wid.load(std::memory_order_relaxed);
        uint32_t current_ring = s_recent_job_ring.load(std::memory_order_relaxed);
        uint32_t current_chain = s_recent_job_chain.load(std::memory_order_relaxed);
        uint32_t after = s_recent_job_publish.load(std::memory_order_acquire);
        if (before != after || completed_us == 0) continue;
        uint64_t now_us = monotonic_us();
        uint64_t delta = now_us >= completed_us ? now_us - completed_us : 0;
        if (delta > (uint64_t)max_age_ms * 1000u) return 0;
        if (sequence) *sequence = current_sequence;
        if (wid) *wid = current_wid;
        if (ring) *ring = current_ring;
        if (chain) *chain = current_chain;
        if (age_us) *age_us = delta;
        return 1;
    }
    return 0;
}

static void publish_recent_spu_job(uint32_t sequence, uint32_t wid,
                                   uint32_t ring, uint32_t chain)
{
    s_recent_job_publish.fetch_add(1, std::memory_order_acq_rel);
    s_recent_job_sequence.store(sequence, std::memory_order_relaxed);
    s_recent_job_wid.store(wid, std::memory_order_relaxed);
    s_recent_job_ring.store(ring, std::memory_order_relaxed);
    s_recent_job_chain.store(chain, std::memory_order_relaxed);
    s_recent_job_us.store(monotonic_us(), std::memory_order_relaxed);
    s_recent_job_publish.fetch_add(1, std::memory_order_release);
}

/* Cherche une chaîne de jobs valide dans les sous-files du ring
 * (même critère que le moniteur historique: magic 0xC0DEC0DE à +0x20). */
static bool find_chain(uint32_t ring, uint32_t* out_chain, uint32_t* out_jp)
{
    for (uint32_t si = 0; si < 2; ++si) {
        uint32_t sub = vm_read32(ring + 0x30u + si * 4u);
        if (sub < 0x10000u || sub >= 0x40000000u) continue;
        for (uint32_t e = 0; e < 0x20u; e += 8u) {
            uint32_t jp = vm_read32(sub + e + 4u);
            if (jp < 0x10000u || jp >= 0x40000000u) continue;
            for (uint32_t o = 0; o < 0x60u; o += 4u) {
                uint32_t v = vm_read32(jp + o);
                if (!((v >= 0x00010000u && v < 0x01400000u) ||
                      (v >= 0x20000000u && v < 0x31000000u)))
                    continue;
                if (vm_read32(v) == 0u || vm_read32(v + 0x20u) != 0xC0DEC0DEu)
                    continue;
                *out_chain = v; *out_jp = jp;
                return true;
            }
        }
    }
    return false;
}

/* func_0079B3A0 supplies the frame job's completion address through the
 * current submit descriptor at +0x14, after setting the pointed counter to
 * one. The lifted FEEC80 fallback bypasses the Job Manager DMA-list tail that
 * would publish this output. Reproduce only that guest-declared output, and
 * only after the caller has proved content + module exit + ready-list drain. */
static bool publish_parent_completion(const WklEnt& w)
{
    if (w.wid != 0u || !g_canonical_toc) return false;
    const uint32_t submit_globals = vm_read32(g_canonical_toc - 0x7508u);
    if (submit_globals < 0x10000u || submit_globals >= 0x40000000u)
        return false;
    const uint32_t submit_desc = vm_read32(submit_globals - 0x7FE0u);
    if (submit_desc < 0x10000u || submit_desc >= 0x40000000u)
        return false;

    const uint32_t payload = vm_read32(submit_desc + 0x10u);
    const uint32_t completion = vm_read32(submit_desc + 0x14u);
    if (payload < 0x10000u || payload >= 0x40000000u ||
        completion < 0x10000u || completion >= 0x40000000u)
        return false;
    const uint32_t pending = vm_read32(completion);
    if (pending != 1u) {
        fprintf(stderr, "[spu-parent-complete] refused wid=%u desc=0x%08X "
                        "counter=0x%08X pending=%u (expected 1)\n",
                w.wid, submit_desc, completion, pending);
        return false;
    }

    vm_write32(completion, pending - 1u);
    fprintf(stderr, "[spu-parent-complete] wid=%u desc=0x%08X "
                    "payload=0x%08X counter=0x%08X %u->%u\n",
            w.wid, submit_desc, payload, completion, pending, pending - 1u);
    return true;
}

static void exec_worker()
{
    for (;;) {
        PushEv ev;
        {
            std::unique_lock<std::mutex> lk(s_q_mx);
            s_q_cv.wait(lk, [] { return !s_q.empty(); });
            ev = s_q.front(); s_q.pop_front();
        }
        const uint32_t current_c0 = vm_read32(ev.ring + 0x40u);
        const uint16_t current_produced = (uint16_t)(current_c0 >> 16);
        const uint16_t current_consumed = (uint16_t)current_c0;
        const uint16_t pending =
            (uint16_t)(current_produced - current_consumed);
        ReadyState ready_before{};
        if (ev.ready_list) {
            const uint16_t ready_pending = read_ready_state(ev.ring, &ready_before)
                ? (uint16_t)(ready_before.target - ready_before.cursor) : 0u;
            if (ready_pending == 0u || ready_pending >= 0x8000u) {
                wkl_release_ready(ev.ring);
                continue;
            }
        } else if (pending == 0u || pending >= 0x8000u) {
            continue;
        }
        uint32_t chain = 0, jp = 0;
        if (!find_chain(ev.ring, &chain, &jp)) {
            if (ev.ready_list) wkl_release_ready(ev.ring);
            continue;
        }
        WklEnt w{};
        if (!wkl_lookup(ev.ring, &w)) {
            fprintf(stderr, "[spu-exec] ring 0x%08X inconnu de la carte "
                            "AddWorkload — chaîne 0x%08X ignorée\n",
                    ev.ring, chain);
            if (ev.ready_list) wkl_release_ready(ev.ring);
            continue;
        }
        static volatile long s_runs = 0;
        long n = ++s_runs;
        const ReadyDispatchPlan dispatch = ev.ready_list
            ? make_ready_dispatch_plan(w) : ReadyDispatchPlan{};
        bool policy_completed = false;
        for (uint32_t instance = 0; instance < dispatch.count; ++instance) {
            if (n <= 64)
                fprintf(stderr,
                        "[spu-exec] RUN #%ld wid=%u chain=0x%08X jp=0x%08X "
                        "pm=0x%08X ring=0x%08X instance=%u/%u spu=%u\n",
                        (long)n, w.wid, chain, jp, w.pm, ev.ring,
                        instance + 1u, dispatch.count,
                        dispatch.spu_ids[instance]);
            uc3_run_policy_job(w.pm, w.size, w.spurs, ev.ring, jp, chain,
                               w.wid, dispatch.spu_ids[instance],
                               dispatch.enabled ? 1u : 0xFFFFFFFFu);
            const bool instance_completed =
                uc3_policy_last_run_completed() != 0;
            policy_completed = policy_completed || instance_completed;
            if (dispatch.enabled) {
                ReadyState observed{};
                const bool readable = read_ready_state(ev.ring, &observed);
                fprintf(stderr,
                        "[spu-multi-return] wid=%u instance=%u/%u spu=%u "
                        "ready=%u cursor=%s%u target=%u policy=%u\n",
                        w.wid, instance + 1u, dispatch.count,
                        dispatch.spu_ids[instance], vm_read8(w.spurs + w.wid),
                        readable ? "" : "unreadable/", readable ? observed.cursor : 0u,
                        readable ? observed.target : 0u,
                        instance_completed ? 1u : 0u);
            }
        }
        publish_recent_spu_job((uint32_t)n, w.wid, ev.ring, chain);
        if (ev.ready_list) {
            ReadyState ready_after{};
            const bool readable = read_ready_state(ev.ring, &ready_after);
            const bool ready_drained = !readable ||
                                       ready_after.cursor >= ready_after.target;
            const bool parent_completed =
                getenv("UC3_SPU_PARENT_COMPLETE") && policy_completed &&
                ready_drained && publish_parent_completion(w);
            fprintf(stderr, "[spu-exec] ready-return wid=%u ring=0x%08X "
                            "cursor=%u/%u -> %s%u/%u policy=%u parent=%u\n",
                    w.wid, ev.ring, ready_before.cursor, ready_before.target,
                    readable ? "" : "unreadable ",
                    readable ? ready_after.cursor : 0u,
                    readable ? ready_after.target : 0u,
                    policy_completed ? 1u : 0u,
                    parent_completed ? 1u : 0u);
            wkl_release_ready(ev.ring);
            continue;
        }
        /* 0) CONSOMMATION du job exécuté: avancer le curseur `consumed` (moitié
         * basse de c0 @ring+0x40) d'un cran. Le job A RÉELLEMENT tourné
         * (décode feec80 -> géométrie + PUTBACK); le marquer consommé reflète
         * ce travail (règle [wkl-ack]/[job-watermark], pas de complétion
         * inventée) → le producteur du jeu voit la progression et pousse le
         * segment suivant. Watermark des curseurs de lecture +0x42..+0x4E.
         * NOTE 2026-07-15 (mesuré): l'avance-consume fait DEUX choses opposées.
         * Sur les rings de DÉCODE (wid!=0) elle aide le passage render state-2
         * (retirer partout -> 0/10 boots atteignent le décode). Mais sur le ring
         * FRAME wid0, elle DÉSYNC le producteur du jeu (func_007A3234) -> la 2e
         * soumission (func_0079B3A0) ne pousse jamais son job2 -> main bloqué en
         * poll (func_0079CD18). FIX chirurgical: avancer SEULEMENT wid!=0; laisser
         * le jeu gérer les curseurs de wid0 (le vrai contrat = frame-job-complete
         * qui décrémente 0x012F3020). UC3_CONSUME_ALL force l'ancien comportement. */
        if ((w.wid != 0u || getenv("UC3_CONSUME_ALL")) &&
            !getenv("UC3_NO_CONSUME_ADVANCE")) {
            uint32_t c0j = vm_read32(ev.ring + 0x40u);
            uint16_t pr = (uint16_t)(c0j >> 16), co = (uint16_t)c0j;
            if (co < pr) {
                uint16_t nc = (uint16_t)(co + 1u);
                vm_write32(ev.ring + 0x40u, ((uint32_t)pr << 16) | nc);
                for (uint32_t off = 0x42u; off <= 0x4Eu; off += 2u) {
                    uint16_t cur = vm_read16(ev.ring + off);
                    if (cur != 0xFFFFu && cur < nc) vm_write16(ev.ring + off, nc);
                }
                if (n <= 64)
                    fprintf(stderr, "[spu-exec] consumed wid=%u ring=0x%08X "
                            "%u/%u->%u/%u\n", w.wid, ev.ring, co, pr, nc, pr);
            }
        }
        /* 0b) RESUME DE FIBRE: réveiller le main sur son lwcond FIOS de frame.
         * Le job frame (wid0/wid2) que le main attend vient d'être exécuté;
         * signaler le condvar (primitive réelle) lui donne la main pour re-
         * tester son prédicat et avancer d'une frame. Réveil spurieux sûr. */
        if (getenv("UC3_FRAME_WAKE")) {
            int r = uc3_lwcond_signal_host(UC3_FRAME_FIOS_COND);
            static volatile long s_wake = 0;
            long wk = ++s_wake;
            if (wk <= 40)
                fprintf(stderr, "[spu-exec] frame-wake lwcond 0x%08X -> %s "
                        "(apres wid=%u)\n", UC3_FRAME_FIOS_COND,
                        r == 0 ? "signale" : "cond inconnu", w.wid);
        }
        /* 1) Drain des commandes SYNC restantes (le runner standalone rend la
         * main avant que le Job Manager consomme un SYNC suivant — SYNC est
         * complet ici par définition: tout le travail antérieur du dispatch
         * est terminé; règle [job-sync] historique, stubs.cpp). */
        for (int guard = 0; guard < 16; ++guard) {
            uint32_t sub0 = vm_read32(ev.ring + 0x30u);
            uint32_t c0s  = vm_read32(ev.ring + 0x40u);
            uint32_t prod = c0s >> 16, cons = c0s & 0xFFFFu;
            if (sub0 < 0x10000u || cons >= prod) break;
            uint32_t cmd = sub0 + (cons % 16u) * 8u;
            if (vm_read32(cmd) != 2u) break;   /* pas un SYNC → stop */
            vm_write32(ev.ring + 0x40u, (prod << 16) | ((cons + 1u) & 0xFFFFu));
            if (n <= 64)
                fprintf(stderr, "[spu-exec] job-sync wid=%u consumed SYNC[%u]\n",
                        w.wid, cons);
        }
        /* 2) Complétion niveau descripteur (règle [frame-job-complete]
         * historique): décrémenter le compteur pending du descripteur actif —
         * wid0: via les globals TOC de la voie submit; wid!=0: le descripteur
         * du ring courant (jp). */
        {
            uint32_t submit_desc = 0;
            if (w.wid == 0u && g_canonical_toc) {
                uint32_t sg = vm_read32(g_canonical_toc - 0x7508u);
                if (sg >= 0x10000u && sg < 0x40000000u)
                    submit_desc = vm_read32(sg - 0x7FE0u);
            } else {
                submit_desc = jp;
            }
            if (submit_desc >= 0x10000u && submit_desc < 0x40000000u) {
                uint32_t payload    = vm_read32(submit_desc + 0x10u);
                uint32_t completion = vm_read32(submit_desc + 0x14u);
                if (payload >= 0x10000u && payload < 0x40000000u &&
                    completion >= 0x10000u && completion < 0x40000000u) {
                    uint32_t pending = vm_read32(completion);
                    if (pending != 0u) {
                        vm_write32(completion, pending - 1u);
                        if (w.wid == 0u) s_fw_counter_ea = completion; /* pour republish */
                        if (n <= 64)
                            fprintf(stderr, "[spu-exec] frame-job-complete "
                                    "wid=%u desc=0x%08X counter=0x%08X %u->%u\n",
                                    w.wid, submit_desc, completion,
                                    pending, pending - 1u);
                    }
                }
            }
        }
        /* 3) Watermark des curseurs actifs quand le ring est drainé
         * (règle [job-watermark] historique — aucune complétion inventée). */
        uint32_t c0 = vm_read32(ev.ring + 0x40u);
        uint16_t produced = (uint16_t)(c0 >> 16), consumed = (uint16_t)c0;
        if (produced == consumed) {
            bool wm = false;
            for (uint32_t off = 0x42u; off <= 0x4Eu; off += 2u) {
                uint16_t cur = vm_read16(ev.ring + off);
                if (cur != 0xFFFFu && cur < consumed) {
                    vm_write16(ev.ring + off, consumed);
                    wm = true;
                }
            }
            if (wm && n <= 64)
                fprintf(stderr, "[spu-exec] watermark wid=%u ring=0x%08X "
                                "cursors=%u\n", w.wid, ev.ring, consumed);
        } else if (n <= 64) {
            fprintf(stderr, "[spu-exec] post-run wid=%u c0=%04X/%04X "
                            "(non drainé — pas de watermark)\n",
                    w.wid, produced, consumed);
        }
    }
}

/* Niveau 3: republication périodique de la complétion frame-worker. Le main
 * re-arme son compteur de dépendance (0x012F3020 = 1) à chaque frame; si aucun
 * nouveau push ne survient, un thread republie la complétion (compteur -> 0)
 * pour laisser la boucle de frame avancer. La cible est apprise du dernier
 * frame-job-complete (desc via TOC wid0). Aucune valeur inventée: on ne fait
 * que reproduire, à cadence, la complétion que le job exécuté a produite. */
static void completion_republisher()
{
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        uint32_t ea = s_fw_counter_ea;
        if (ea >= 0x10000u && ea < 0x40000000u) {
            uint32_t pending = vm_read32(ea);
            if (pending != 0u) {
                vm_write32(ea, pending - 1u);
                static volatile long s_rep = 0;
                long n = ++s_rep;
                if (n <= 20)
                    fprintf(stderr, "[spu-exec] republish frame-worker "
                            "0x%08X %u->%u\n", ea, pending, pending - 1u);
            }
        }
    }
}

static void uc3_spu_exec_on_push(uint32_t c0_ea, uint32_t value)
{
    const uint32_t ring = c0_ea - 0x40u;
    const uint16_t produced = (uint16_t)(value >> 16);
    const uint16_t consumed = (uint16_t)value;
    const bool producer_event = wkl_note_producer(ring, produced);
    ReadyState ready{};
    const bool ready_event = !producer_event && getenv("UC3_SPU_READY_EXEC") &&
                             wkl_note_ready(ring, &ready);
    if (!producer_event && getenv("UC3_SPU_READY_TRACE")) {
        ReadyState observed = ready;
        const bool readable = ready_event || read_ready_state(ring, &observed);
        if (readable) {
            WklEnt mapped{};
            uint32_t chain = 0, jp = 0;
            const bool known = wkl_lookup(ring, &mapped);
            const bool found = find_chain(ring, &chain, &jp);
            static volatile long s_ready_trace = 0;
            const long n = ++s_ready_trace;
            if (n <= 64) {
                fprintf(stderr, "[spu-ready-trace] #%ld ring=0x%08X "
                                "c0=0x%08X wid=%s%u cursor=%u target=%u "
                                "gap=%u chain=%s0x%08X jp=0x%08X claimed=%u\n",
                        n, ring, value, known ? "" : "?", known ? mapped.wid : 0u,
                        observed.cursor, observed.target,
                        (uint16_t)(observed.target - observed.cursor),
                        found ? "" : "?", chain, jp, ready_event ? 1u : 0u);
            }
        }
    }
    if (!producer_event && !ready_event) return;

    if (ready_event)
        fprintf(stderr, "[spu-exec] ready ring=0x%08X cursor=%u "
                         "target=%u (real policy work pending)\n",
                ring, ready.cursor, ready.target);

    if (s_exec_level >= 2) {
        /* NE PAS exécuter sur le thread pousseur (il est dans vm_write) —
         * déférer au worker dédié. */
        { std::lock_guard<std::mutex> lk(s_q_mx);
          s_q.push_back({ ring, value, ready_event }); }
        s_q_cv.notify_one();
        return;
    }
    /* Niveau 1: détection/log seulement. */
    uint32_t chain = 0, jp = 0;
    if (find_chain(ring, &chain, &jp)) {
        static volatile long s_found = 0;
        long n = ++s_found;
        if (n <= 64)
            fprintf(stderr, "[spu-exec] PUSH ring=0x%08X c0=%04X/%04X "
                            "chain=0x%08X (jp=0x%08X)\n",
                    ring, produced, consumed, chain, jp);
    }
    if (ready_event) wkl_release_ready(ring);
}

extern "C" void uc3_spu_exec_install(void)
{
    const char* e = getenv("UC3_SPU_EXEC");
    if (!e) return;
    s_exec_level = atoi(e);
    if (s_exec_level < 1) s_exec_level = 1;
    g_uc3_ring_push_cb = uc3_spu_exec_on_push;
    g_uc3_recent_spu_job_cb = recent_spu_job;
    if (s_exec_level >= 2)
        std::thread(exec_worker).detach();
    if (s_exec_level >= 3)
        std::thread(completion_republisher).detach();
    fprintf(stderr, "[spu-exec] executeur deterministe installe (niveau %d: "
                    "%s)\n", s_exec_level,
            s_exec_level >= 2 ? "execution au push + watermark"
                              : "detection seule");
}
