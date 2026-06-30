#!/usr/bin/env python3
"""Route bctr (computed JUMP / tail-call / gcc switch-case) through
ps3_indirect_jump instead of ps3_indirect_call. The runtime ps3_indirect_call
save/restores callee-saved r14-r31 (to fix truncated callees that violate the
ABI across a real bctrl CALL); but a bctr target shares the caller's frame and
legitimately modifies r14-r31 (e.g. the printf jump-table cases like
func_00D7A608) — restoring them there corrupts the formatter and leaves the
format context = garbage (0x06900000). The lifter emits bctr as
`ps3_indirect_call(ctx); return;` and bctrl as
`ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);`, so we can tell them apart by
the trailing `return;`. Idempotent. Run from recompiled/ after every re-lift."""
import glob

decl = "extern \"C\" void ps3_indirect_call(ppu_context* ctx);\n"
decl_jump = "extern \"C\" void ps3_indirect_jump(ppu_context* ctx);\n"
bctr_old = "ps3_indirect_call(ctx); return;"
bctr_new = "ps3_indirect_jump(ctx); return;"

nfiles = 0
nsites = 0
for f in glob.glob("ppu_recomp_*.cpp"):
    s = open(f, encoding="latin1").read()
    if bctr_old not in s and decl_jump in s:
        continue
    cnt = s.count(bctr_old)
    s = s.replace(bctr_old, bctr_new)
    if decl in s and decl_jump not in s:
        s = s.replace(decl, decl + decl_jump, 1)
    open(f, "w", encoding="latin1").write(s)
    if cnt:
        nfiles += 1
        nsites += cnt
print(f"routed {nsites} bctr sites to ps3_indirect_jump across {nfiles} files")
