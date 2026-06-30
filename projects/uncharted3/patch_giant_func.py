#!/usr/bin/env python3
"""Stub func_00E40830 — a ~2MB data blob (size 0x1F2FD0) that find_functions
misdetects as one giant function (532k generated lines), which OOMs MSVC. It is
data, never executed as code, so replacing its body with a no-op is safe. Run
from recompiled/ (idempotent). Re-run after every re-lift."""
import glob

STUB = ("void func_00E40830(ppu_context* ctx) { (void)ctx; "
        "/* 2MB data misdetected as code - stubbed (patch_giant_func.py) */ }\n")

for f in glob.glob("ppu_recomp_*.cpp"):
    lines = open(f, encoding="latin1").readlines()
    out = []
    i = 0
    n = len(lines)
    hit = False
    while i < n:
        if lines[i].startswith("void func_00E40830("):
            if "stubbed" in lines[i]:
                out.append(lines[i]); i += 1; continue   # already stubbed
            out.append(STUB)
            i += 1
            while i < n and not lines[i].startswith("}"):
                i += 1
            i += 1  # skip closing brace
            hit = True
            continue
        out.append(lines[i]); i += 1
    if hit:
        open(f, "w", encoding="latin1").writelines(out)
        print(f"stubbed func_00E40830 in {f} ({n} -> {len(out)} lines)")
        break
else:
    print("func_00E40830 not found (already stubbed or not lifted?)")
