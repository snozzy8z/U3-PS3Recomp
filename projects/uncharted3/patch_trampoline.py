#!/usr/bin/env python3
"""Make DRAIN_TRAMPOLINE force r2 to the canonical TOC at every trampoline tail
call. UC3 is a single-TOC static binary: r2 is reserved for the TOC and must
always equal it, so any drift (0 or stub-deref garbage) is a bug we correct
here. Idempotent: upgrades the earlier ==0 variant to unconditional. Run from
recompiled/."""
import glob

BS = "\\"
old_fix = "        if ((ctx)->gpr[2] == 0) (ctx)->gpr[2] = g_canonical_toc; " + BS + "\n"
new_fix = "        (ctx)->gpr[2] = g_canonical_toc; " + BS + "\n"
gz_line = "        g_trampoline_fn = 0; " + BS + "\n"
define_line = "#define DRAIN_TRAMPOLINE(ctx) do { " + BS + "\n"
extern_line = 'extern "C" unsigned int g_canonical_toc;\n'

n = 0
for f in glob.glob("ppu_recomp_*.cpp"):
    lines = open(f, encoding="latin1").readlines()
    changed = False
    # Upgrade existing ==0 variant.
    if old_fix in lines:
        lines = [new_fix if ln == old_fix else ln for ln in lines]
        changed = True
    # Or fresh-install if never patched.
    elif new_fix not in lines and gz_line in lines:
        out = []
        for ln in lines:
            if ln == define_line:
                out.append(extern_line)
            out.append(ln)
            if ln == gz_line:
                out.append(new_fix)
        lines = out
        changed = True
    if changed:
        open(f, "w", encoding="latin1").writelines(lines)
        n += 1
print(f"patched {n} files")
