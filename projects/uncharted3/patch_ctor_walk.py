#!/usr/bin/env python3
"""Replace the generated func_00D91404 (C++ global-constructor table walker)
with a robust hand-rewrite: keep the table cursor in a HOST local instead of
ctx->gpr[31]. The faithful lift kept the cursor in r31 (callee-saved); if any
constructor's callee violates the ABI and fails to restore r31, the walk reads a
garbage entry and crashes. Host-side cursor makes the walk immune. Run from
recompiled/ (idempotent). Re-run after every re-lift."""
import glob, re

ROBUST = '''void func_00D91404(ppu_context* ctx) {
        /* ROBUST hand-rewrite (patch_ctor_walk.py): cursor in a HOST local, not
         * ctx->gpr[31], so an ABI-violating constructor callee cannot derail the
         * walk. Walks the OPD table high->low to the -1 terminator; entry=OPD
         * addr, *(OPD)=code, *(OPD+4)=toc. (g_canonical_toc is already declared
         * at file scope by the generated preamble / patch_trampoline.) */
        uint32_t saved_lr = (uint32_t)ctx->lr;
        uint32_t end = vm_read32((uint32_t)(ctx->gpr[2] - 0x458C));
        uint32_t cursor = end - 4;
        unsigned count = 0;
        int32_t entry = (int32_t)vm_read32(cursor);
        while (entry != -1) {
                uint32_t opd = (uint32_t)entry;
                cursor -= 4;
                ctx->ctr = vm_read32(opd);
                ctx->gpr[2] = vm_read32(opd + 4);
                ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
                ctx->gpr[2] = g_canonical_toc;
                count++;
                entry = (int32_t)vm_read32(cursor);
        }
        fprintf(stderr, "[ctor] all %u global constructors done\\n", count);
        ctx->lr = saved_lr;
        return;
}
'''

for f in glob.glob("ppu_recomp_*.cpp"):
    lines = open(f, encoding="latin1").readlines()
    start = next((k for k, l in enumerate(lines)
                  if l.startswith("void func_00D91404(")), None)
    if start is None:
        continue
    if "patch_ctor_walk.py" in lines[start + 1] if start + 1 < len(lines) else False:
        print("already patched"); break
    end = start + 1
    while end < len(lines) and not lines[end].startswith("}"):
        end += 1
    lines[start:end + 1] = [ROBUST]
    open(f, "w", encoding="latin1").writelines(lines)
    print(f"patched func_00D91404 in {f}")
    break
else:
    print("func_00D91404 not found")
