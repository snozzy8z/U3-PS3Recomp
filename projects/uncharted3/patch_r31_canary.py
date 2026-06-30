#!/usr/bin/env python3
"""Wrap each DIRECT call inside func_000DDEE4 with an r31 (callee-saved) canary:
save r31 before, log if the callee subtree changed it after. Pinpoints the
ABI-violating callee that corrupts r31 (the suspected common upstream cause of
the corruption chain). Logs only; no behavior change. Run from recompiled/."""
import re

f = "ppu_recomp_b0001.cpp"
lines = open(f, encoding="latin1").readlines()
start = next(i for i, l in enumerate(lines) if l.startswith("void func_000DDEE4("))
end = next(i for i in range(start + 1, len(lines)) if lines[i].startswith("}"))

pat = re.compile(r'^(\s*)(func_[0-9A-F]{8})\(ctx\); DRAIN_TRAMPOLINE\(ctx\);\s*$')
n = 0
NL = chr(92) + "n"   # the two chars backslash-n for the C string literal
for i in range(start, end):
    m = pat.match(lines[i])
    if not m:
        continue
    ind, fn = m.group(1), m.group(2)
    chk = ('if((unsigned)ctx->gpr[31]!=(unsigned)_r){static int _q=0;if(_q<24){_q++;'
           'fprintf(stderr,"[r31] %s -> 0x%08X to 0x%08X' + NL + '","' + fn +
           '",(unsigned)_r,(unsigned)ctx->gpr[31]);}}')
    lines[i] = (ind + "{ unsigned long long _r=ctx->gpr[31]; " + fn +
                "(ctx); DRAIN_TRAMPOLINE(ctx); " + chk + " }\n")
    n += 1

open(f, "w", encoding="latin1").writelines(lines)
print("instrumented %d direct calls in func_000DDEE4" % n)
