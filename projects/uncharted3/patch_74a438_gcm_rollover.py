from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0011.cpp"
START = b"void func_0074A438(ppu_context* ctx) {\r\n"
END = b"\r\nvoid func_0074A488(ppu_context* ctx) {\r\n"
MARKER = b"Synthetic RSX consumer recycles the fixed command buffer."

data = PATH.read_bytes()
newline = b"\r\n" if b"\r\n" in data[:4096] else b"\n"
start_marker = START.replace(b"\r\n", newline)
end_marker = END.replace(b"\r\n", newline)

start = data.index(start_marker)
end = data.index(end_marker, start)
body = data[start:end]

if MARKER in body:
    print("GCM rollover fallback already present", PATH)
else:
    replacement = b"""void func_0074A438(ppu_context* ctx) {
        /* Synthetic RSX consumer recycles the fixed command buffer. */
        uint32_t context = (uint32_t)ctx->gpr[3];
        uint32_t begin = vm_read32(context + 0x0);
        uint32_t end = vm_read32(context + 0x4);
        if (begin && end > begin)
                vm_write32(context + 0x8, begin);
        ctx->gpr[3] = 0;
        return;
}"""
    replacement = replacement.replace(b"\r\n", newline)
    PATH.write_bytes(data[:start] + replacement + data[end:])
    print("patched", PATH)
