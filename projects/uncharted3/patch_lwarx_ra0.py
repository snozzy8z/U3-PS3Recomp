#!/usr/bin/env python3
"""Post-lift fix for a SYSTEMIC lifter bug: indexed memory ops (lwarx/stwcx/lvx/
stvx/lwzx/stwx/ldx/stdx/...) use X-form addressing EA = (rA==0 ? 0 : gpr[rA]) +
gpr[rB]. When rA is register 0, the PPC spec uses LITERAL 0, NOT the contents of
gpr[0]. The lifter wrongly emitted `ctx->gpr[0] + ctx->gpr[rB]`, so every such op
computed a garbage EA (gpr[0] frequently holds LR or stale values) -> wrong
loads/stores, corrupted lock CAS (infinite spinlock spin), etc.

Fix: `uint64_t ea = ctx->gpr[0] + ctx->gpr[N]` -> `uint64_t ea = ctx->gpr[N]`.
This is correct for all cases incl. rB=r0 (`gpr[0] + gpr[0]` -> `gpr[0]`, since
only rA gets the literal-0 treatment, never rB).

Re-run after every re-lift. (The lifter itself should be fixed too — see
tools/ppu_lifter.py X-form EA emission.)
"""
import re, glob, os

ROOT = os.path.dirname(os.path.abspath(__file__))
# X-form indexed EA (lwarx/stwcx/lvx/stvx/lwzx/...): rA=r0 -> 0
pat_x   = re.compile(r'uint64_t ea = ctx->gpr\[0\] \+ ctx->gpr\[(\d+)\]')
# D-form load:  vm_readN(ctx->gpr[0] + DISP)  -> vm_readN(DISP)   (rA=r0 -> 0)
pat_ld  = re.compile(r'(vm_read\d+)\(ctx->gpr\[0\] \+ (-?0x[0-9A-Fa-f]+|-?\d+)\)')
# D-form store: vm_writeN(ctx->gpr[0] + DISP, -> vm_writeN(DISP,
pat_st  = re.compile(r'(vm_write\d+)\(ctx->gpr\[0\] \+ (-?0x[0-9A-Fa-f]+|-?\d+), ')
# PARENTHESIZED vector/aligned EA (lvx/stvx/lvsl/lvsr/dcbz...): the lifter emits
# (ctx->gpr[ra] + ctx->gpr[rb]) for the EA; rA=r0 -> 0. The plain `add` op is
# NOT parenthesized, so this only matches indexed EA computations -> safe.
pat_vec = re.compile(r'\(ctx->gpr\[0\] \+ ctx->gpr\[(\d+)\]\)')
total = {'x': 0, 'ld': 0, 'st': 0, 'vec': 0}
files = 0
for f in glob.glob(os.path.join(ROOT, 'recompiled', '*.cpp')):
    s = open(f, encoding='utf-8', errors='surrogateescape').read()
    s, nx  = pat_x.subn(r'uint64_t ea = ctx->gpr[\1]', s)
    s, nl  = pat_ld.subn(r'\1(\2)', s)
    s, ns  = pat_st.subn(r'\1(\2, ', s)
    s, nv  = pat_vec.subn(r'(ctx->gpr[\1])', s)
    if nx or nl or ns or nv:
        open(f, 'w', encoding='utf-8', errors='surrogateescape').write(s)
        total['x'] += nx; total['ld'] += nl; total['st'] += ns; total['vec'] += nv
        files += 1
print(f"patch_lwarx_ra0: X-form={total['x']} D-load={total['ld']} D-store={total['st']} vec/lvsl={total['vec']} across {files} files")
