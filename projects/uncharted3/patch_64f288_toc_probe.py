from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0009.cpp"
MARKER = b"probe64f288_calls"

data = PATH.read_bytes()
newline = b"\r\n" if b"\r\n" in data[:4096] else b"\n"
anchor = b"void func_0064F288(ppu_context* ctx) {\r\n"
replacement = b"""void func_0064F288(ppu_context* ctx) {
        static uint32_t probe64f288_calls = 0;
        uint32_t probe64f288_id = probe64f288_calls++;
        uint32_t probe64f288_toc = (uint32_t)ctx->gpr[2];
        uint32_t probe64f288_local_toc = vm_read32(probe64f288_toc - 0x77D4);
        if (probe64f288_id < 16)
                fprintf(stderr, "[probe64f288] call=%u r2=%08X local=%08X global-slot=%08X global=%08X canonical=%08X\\n",
                        probe64f288_id, probe64f288_toc, probe64f288_local_toc,
                        probe64f288_local_toc - 0x7FD4,
                        vm_read32(probe64f288_local_toc - 0x7FD4), g_canonical_toc);
"""

if newline == b"\n":
    anchor = anchor.replace(b"\r\n", newline)
replacement = replacement.replace(b"\n", newline)

if MARKER in data:
    print("func_0064F288 TOC probe already present", PATH)
else:
    if anchor not in data:
        raise SystemExit("func_0064F288 anchor not found")
    PATH.write_bytes(data.replace(anchor, replacement, 1))
    print("patched", PATH)
