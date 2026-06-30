import re
hdr = "recompiled/ppu_recomp.h"
out = "recompiled/ppu_recomp_unlifted_stubs.cpp"
pat = re.compile(r'void (func_[0-9A-Fa-f]{8})\(ppu_context\* ctx\);\s*/\* external \*/')
syms = []
with open(hdr, 'r', encoding='utf-8', errors='replace') as f:
    for line in f:
        m = pat.search(line)
        if m:
            syms.append(m.group(1))
syms = sorted(set(syms))
header = (
"/*\n"
" * AUTO-GENERATED - do not edit by hand.\n"
" * Fallback definitions for functions REFERENCED but NOT lifted (bl/branch\n"
" * targets outside the discovered function set: low/null addresses, PRX\n"
" * imports 0xFF..., and functions missed by analysis).\n"
" * Each stub routes to the runtime ppu_unlifted_stub(), which logs the address\n"
" * and returns -> the binary LINKS, and at runtime the log shows which targets\n"
" * are actually reached (to be lifted next).\n"
" * Regenerate via: python ../../tools/gen_unlifted_stubs.py\n"
" */\n"
'#include "ppu_recomp.h"\n\n'
'extern "C" void ppu_unlifted_stub(uint64_t addr, ppu_context* ctx);\n\n'
)
with open(out, 'w', encoding='utf-8') as f:
    f.write(header)
    for s in syms:
        addr = s.split('_')[1]
        f.write("void %s(ppu_context* ctx) { ppu_unlifted_stub(0x%sull, ctx); }\n" % (s, addr))
print("symbols:", len(syms))
