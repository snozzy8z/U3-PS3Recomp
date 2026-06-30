#!/usr/bin/env python3
"""
stream_lift.py - pilote de lifting PPU *streaming* et *reprenable*.

Au lieu de tout lifter en memoire puis de serialiser ~3 M lignes d'un coup
(long, mono-thread), ce pilote :
  * lifte par LOTS,
  * ecrit chaque lot sur le disque IMMEDIATEMENT puis libere la memoire,
  * est REPRENABLE : il saute les lots deja ecrits (etat dans .stream_state.json),
    ce qui permet de finir le travail sur plusieurs executions courtes.

Code genere compatible ppu_lifter.py : memes noms (func_<ADDR>), meme preamble,
meme table function_table[], declarations globales dans ppu_recomp.h.

Usage :
  python stream_lift.py EBOOT.ELF --functions functions.json -o recompiled/ \
      [-j N] [--batch 2000] [--mid-chunk 1000] [--time-budget 30]
Relancer la MEME commande tant qu'elle n'affiche pas "ALL DONE".
"""
import argparse, bisect, glob, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppu_lifter as L
from elf_parser import ELFFile, PT_LOAD


def log(m):
    print(m, flush=True)


def build_segments(elf):
    segs = []
    for ph in elf.program_headers:
        if ph.p_type == PT_LOAD and ph.p_filesz > 0:
            data = elf.get_segment_data(elf.program_headers.index(ph))
            segs.append((ph.p_vaddr, data, bool(ph.p_flags & 1)))
    return segs


def disasm_range(segs, start, end, big_endian):
    """Desassemble UNIQUEMENT la plage [start, end) depuis les blobs de segment
    (comme les workers de ppu_lifter). Rapide et leger."""
    for vaddr, data, _ in segs:
        if vaddr <= start < vaddr + len(data):
            blob = data[start - vaddr:min(end - vaddr, len(data))]
            return L.disassemble_bytes(blob, start, big_endian) if blob else []
    return []


class _N:
    """Objet leger avec attribut .name (pour les trampolines de fallthrough)."""
    __slots__ = ("name",)
    def __init__(self, a):
        self.name = "func_%08X" % a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("--functions", required=True)
    ap.add_argument("-o", "--output", default="recompiled")
    ap.add_argument("-j", "--jobs", type=int, default=max(1, os.cpu_count() or 1))
    ap.add_argument("--batch", type=int, default=2000)
    ap.add_argument("--mid-chunk", type=int, default=1000)
    ap.add_argument("--time-budget", type=float, default=30.0)
    ap.add_argument("--header-name", default="ppu_recomp.h")
    ap.add_argument("--base", default="ppu_recomp")
    ap.add_argument("--refresh-jump-tables", action="store_true",
                    help="re-run jump-table discovery and add newly found mid entries")
    ap.add_argument("--refresh-code", action="store_true",
                    help="regenerate every base and mid source with the current lifter")
    args = ap.parse_args()

    t0 = time.time()
    def out_of_time():
        return (time.time() - t0) > args.time_budget

    os.makedirs(args.output, exist_ok=True)
    state_path = os.path.join(args.output, ".stream_state.json")
    state = {"batches_done": [], "referenced": [], "mid_seq": 0,
             "mid_done": [], "finalized": False}
    if os.path.exists(state_path):
        state.update(json.load(open(state_path)))
    def save_state():
        json.dump(state, open(state_path, "w"))

    if args.refresh_code:
        output_root = os.path.abspath(args.output)
        for pattern in (f"{args.base}_b[0-9][0-9][0-9][0-9].cpp",
                        f"{args.base}_mid[0-9][0-9][0-9][0-9].cpp"):
            for path in glob.glob(os.path.join(output_root, pattern)):
                if os.path.dirname(os.path.abspath(path)) != output_root:
                    raise RuntimeError(f"refusing to remove generated file outside {output_root}")
                os.remove(path)
        state["batches_done"] = []
        state["mid_done"] = []
        state["mid_seq"] = 0
        state["finalized"] = False
        save_state()

    elf = ELFFile(args.input)
    elf.load()
    big_endian = elf.elf_header.big_endian
    segs = build_segments(elf)
    seg_blobs = [(v, d) for (v, d, _) in segs]

    func_list = json.load(open(args.functions))
    func_bounds = sorted((int(str(e["start"]), 0), int(str(e["end"]), 0))
                         for e in func_list)
    base_starts = [s for s, _ in func_bounds]
    text_lo, text_hi = base_starts[0], max(e for _, e in func_bounds)
    log("[stream_lift] %d fonctions, lots de %d, j=%d, budget %.0fs"
        % (len(func_bounds), args.batch, args.jobs, args.time_budget))

    preamble = "\n".join(L.PPULifter()._preamble_lines())
    referenced = set(state["referenced"])

    # ----------------------------------------------------------------
    # DECOUVERTE DES JUMP-TABLES (switch gcc : `mtctr; bctr` via table de
    # donnees). Ces cibles de case sont invisibles a l'analyse statique des
    # branches : sans elles, le `bctr` -> ps3_indirect_call atterrit sur une
    # adresse non liftee (ex. le moteur printf func_00D79DB4). On les ajoute a
    # `referenced` : la PHASE 2 les liftera comme entrees mid-fonction (le ctx
    # partage tous les registres, donc une case standalone voit bien r28/r31...
    # mis en place par le prologue du dispatcher). Passe unique, persistee.
    # ----------------------------------------------------------------
    if args.refresh_jump_tables:
        state["jt_done"] = False
    if not state.get("jt_done"):
        log("[stream_lift] decouverte des jump-tables (passe unique)...")
        all_insns = []
        for vaddr, data, is_exec in segs:
            if is_exec:
                all_insns.extend(L.disassemble_bytes(data, vaddr, big_endian))
        def _read_u32(a):
            for v, d in seg_blobs:
                if v <= a and a + 4 <= v + len(d):
                    return int.from_bytes(d[a - v:a - v + 4],
                                          'big' if big_endian else 'little')
            return None
        toc = _read_u32((elf.elf_header.e_entry + 4) & 0xFFFFFFFF) or 0
        tables = L.discover_jump_tables(all_insns, _read_u32, toc, text_lo, text_hi)
        jt_targets = set()
        for ts in tables.values():
            jt_targets.update(ts)
        referenced |= jt_targets
        del all_insns
        state["jt_done"] = True
        state["referenced"] = sorted(referenced)
        save_state()
        log("[stream_lift] jump-tables : %d dispatchers, %d cibles de case (toc=0x%08X)"
            % (len(tables), len(jt_targets), toc))

    # Structures GLOBALES pour les trampolines de fallthrough.
    global_starts = sorted(set(base_starts) | set(state["mid_done"]))
    g_by_addr = {a: _N(a) for a in global_starts}

    # ----------------------------------------------------------------
    # PHASE 1 - lift de base, par lots, streaming + reprenable
    # ----------------------------------------------------------------
    n_batches = (len(func_bounds) + args.batch - 1) // args.batch
    done = set(state["batches_done"])
    gs = sorted(g_by_addr)
    gi = {a: i for i, a in enumerate(gs)}
    for b in range(n_batches):
        if b in done:
            continue
        if out_of_time():
            state["referenced"] = sorted(referenced)
            save_state()
            log("[stream_lift] budget atteint, base %d/%d - relance."
                % (len(done), n_batches))
            return
        sub = func_bounds[b * args.batch:(b + 1) * args.batch]
        lifter = L.PPULifter()
        if args.jobs > 1:
            L._parallel_lift(lifter, sub, seg_blobs, big_endian, args.jobs)
        else:
            for s, e in sub:
                lifter.lift_function(disasm_range(segs, s, e, big_endian), s, e)
        path = os.path.join(args.output, "%s_b%04d.cpp" % (args.base, b))
        with open(path, "w") as f:
            f.write(preamble + "\n")
            for func in lifter.functions:
                f.write("\n".join(
                    lifter._function_def_lines(func, g_by_addr, gs, gi)) + "\n")
        referenced |= lifter.call_targets | lifter.branch_targets
        done.add(b)
        state["batches_done"] = sorted(done)
        state["referenced"] = sorted(referenced)
        save_state()
        log("  [base] lot %d/%d -> %s (%d fn)"
            % (b + 1, n_batches, os.path.basename(path), len(lifter.functions)))
        del lifter

    log("[stream_lift] PHASE 1 terminee : %d lots de base." % n_batches)

    # ----------------------------------------------------------------
    # PHASE 2 - entrees mid-fonction (cibles referencees non definies tombant
    # a l'interieur d'une fonction). Worklist par sous-lots, reprenable.
    # ----------------------------------------------------------------
    defined = set(base_starts) | set(state["mid_done"])
    # Boundaries for mid-entry ranges: EVERY entry point (base functions AND all
    # referenced mid targets in .text), so each mid body extends exactly to the
    # next entry point. Using only `defined` (and the old 0x800 cap) either
    # truncated a body before its terminal `b epilogue` (-> fallthrough to the
    # wrong next function -> infinite loop leaking the stack) or, with mids
    # reset, let one mid span over many others (-> giant overlapping functions).
    all_starts_sorted = sorted(set(base_starts) |
                               {t for t in referenced if text_lo <= t < text_hi})

    def next_boundary(a):
        k = bisect.bisect_right(all_starts_sorted, a)
        return all_starts_sorted[k] if k < len(all_starts_sorted) else text_hi

    seq = state["mid_seq"]
    while True:
        undefined = sorted(t for t in referenced
                           if t not in defined and text_lo <= t < text_hi)
        if not undefined:
            break
        if out_of_time():
            state["referenced"] = sorted(referenced)
            state["mid_done"] = sorted(defined - set(base_starts))
            state["mid_seq"] = seq
            save_state()
            log("[stream_lift] budget atteint en phase mid (%d restantes) - relance."
                % len(undefined))
            return
        batch = undefined[:args.mid_chunk]
        seq += 1
        lifter = L.PPULifter()
        for t in batch:
            end = next_boundary(t)
            # Une entree mid couvre jusqu'a la prochaine frontiere definie
            # (fonction ou autre mid). next_boundary borne deja la plage, donc on
            # l'utilise telle quelle : un plafond arbitraire (l'ancien 0x800)
            # TRONQUE les gros corps de fonction atteints uniquement par une
            # branche interne (typique des constructeurs C++), coupant leur
            # branche terminale `b epilogue` et produisant un faux trampoline de
            # fallthrough vers la fonction suivante -> boucle infinie qui fuit la
            # pile. On ne plafonne que les spans reellement pathologiques.
            if end <= t:
                end = min(t + 0x800, text_hi)
            elif end - t > 0x10000:
                end = min(t + 0x10000, text_hi)
            lifter.lift_function(disasm_range(segs, t, end, big_endian), t, end)
        for t in batch:
            if t not in g_by_addr:
                g_by_addr[t] = _N(t)
        mgs = sorted(g_by_addr)
        mgi = {a: i for i, a in enumerate(mgs)}
        path = os.path.join(args.output, "%s_mid%04d.cpp" % (args.base, seq))
        with open(path, "w") as f:
            f.write(preamble + "\n")
            for func in lifter.functions:
                f.write("\n".join(
                    lifter._function_def_lines(func, g_by_addr, mgs, mgi)) + "\n")
        referenced |= lifter.call_targets | lifter.branch_targets
        defined.update(batch)
        # Keep boundaries = ALL entry points (base + every referenced target),
        # not just the mids defined so far. Bounding by `defined` alone lets a
        # mid span over later-defined mids -> giant overlapping functions.
        all_starts_sorted = sorted(set(base_starts) |
                                   {t for t in referenced if text_lo <= t < text_hi})
        state["referenced"] = sorted(referenced)
        state["mid_done"] = sorted(defined - set(base_starts))
        state["mid_seq"] = seq
        save_state()
        log("  [mid] sous-lot %d: +%d entrees -> %s (reste ~%d)"
            % (seq, len(batch), os.path.basename(path),
               len(undefined) - len(batch)))
        del lifter

    log("[stream_lift] PHASE 2 terminee : %d entrees mid-fonction."
        % (len(defined) - len(base_starts)))

    # ----------------------------------------------------------------
    # PHASE 3 - header global + table function_table[] combinee
    # ----------------------------------------------------------------
    all_defined = sorted(defined)
    hdr = os.path.join(args.output, args.header_name)
    with open(hdr, "w") as f:
        f.write(L.HEADER_PREAMBLE + "\n")
        for a in all_defined:
            f.write("void func_%08X(ppu_context* ctx);\n" % a)
        for t in sorted(referenced - defined):
            f.write("void func_%08X(ppu_context* ctx); /* external */\n" % t)
        f.write("\n")

    tbl = os.path.join(args.output, "%s_table.cpp" % args.base)
    with open(tbl, "w") as f:
        f.write('#include "ppu_recomp.h"\n')
        f.write("/* Function table combinee (stream_lift) */\n")
        f.write("const func_entry function_table[] = {\n")
        for a in all_defined:
            f.write('    { 0x%08XULL, func_%08X, "func_%08X" },\n' % (a, a, a))
        f.write("    { 0, NULL, NULL }\n")
        f.write("};\n")
        f.write("const uint64_t function_table_count = %d;\n" % len(all_defined))

    state["finalized"] = True
    save_state()
    log("[stream_lift] PHASE 3 : %s + %s (%d fonctions)."
        % (os.path.basename(hdr), os.path.basename(tbl), len(all_defined)))
    log("[stream_lift] ALL DONE")


if __name__ == "__main__":
    main()
