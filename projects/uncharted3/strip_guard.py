#!/usr/bin/env python3
"""Strip the per-function PpuDepthGuard (it exploded MSVC compile time). The
recursion it guarded is now bounded centrally in ps3_indirect_call. Removes the
5-line preamble block and the 2-line per-function guard. Run from recompiled/."""
import glob

preamble = (
    'extern "C" __declspec(thread) int g_ppu_depth;\n'
    "#ifndef PPU_MAX_DEPTH\n"
    "#define PPU_MAX_DEPTH 6000\n"
    "#endif\n"
    "struct PpuDepthGuard { PpuDepthGuard() { ++g_ppu_depth; } ~PpuDepthGuard() { --g_ppu_depth; } };\n"
)
g1 = "    if (g_ppu_depth >= PPU_MAX_DEPTH) return;\n"
g2 = "    PpuDepthGuard _pdg;\n"

n = 0
for f in glob.glob("ppu_recomp_*.cpp"):
    s = open(f, encoding="latin1").read()
    if "PpuDepthGuard" not in s:
        continue
    s = s.replace(preamble, "")
    s = s.replace(g1, "")
    s = s.replace(g2, "")
    open(f, "w", encoding="latin1").write(s)
    n += 1
print(f"stripped {n} files")
