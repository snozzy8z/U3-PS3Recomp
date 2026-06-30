#!/usr/bin/env python3
"""Add ppu_tramp_rec(_tf) to DRAIN_TRAMPOLINE so the trampoline-chain tracer
sees every tail call. Idempotent. Run from recompiled/."""
import glob

BS = "\\"
rec_decl = 'extern "C" void ppu_tramp_rec(void*);\n'
gz   = "        g_trampoline_fn = 0; " + BS + "\n"
recl = "        ppu_tramp_rec((void*)_tf); " + BS + "\n"
canon = "        (ctx)->gpr[2] = g_canonical_toc; " + BS + "\n"  # anchor (added earlier)
define_line = "#define DRAIN_TRAMPOLINE(ctx) do { " + BS + "\n"

n = 0
for f in glob.glob("ppu_recomp_*.cpp"):
    lines = open(f, encoding="latin1").readlines()
    if recl in lines:
        continue
    if gz not in lines:
        continue
    out = []
    for ln in lines:
        if ln == define_line:
            out.append(rec_decl)
        out.append(ln)
        if ln == gz:               # insert recorder right after clearing g_trampoline_fn
            out.append(recl)
    open(f, "w", encoding="latin1").writelines(out)
    n += 1
print(f"patched {n} files")
