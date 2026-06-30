from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0020.cpp"
START = b"void func_00D50DC4(ppu_context* ctx) {\r\n"
END = b"\r\nvoid func_00D50E5C(ppu_context* ctx) {\r\n"
MARKER = b"Synthetic SPU workers complete this seven-lane fence synchronously."

data = PATH.read_bytes()
newline = b"\r\n" if b"\r\n" in data[:4096] else b"\n"
start_marker = START.replace(b"\r\n", newline)
end_marker = END.replace(b"\r\n", newline)

start = data.index(start_marker)
end = data.index(end_marker, start)
body = data[start:end]

if MARKER in body:
    print("SPU fence fallback already present", PATH)
else:
    replacement = b"""void func_00D50DC4(ppu_context* ctx) {
        /* Synthetic SPU workers complete this seven-lane fence synchronously. */
        uint32_t base = vm_read32(ctx->gpr[3] + 0x0);
        uint32_t lane = base + ((uint32_t)ctx->gpr[4] << 4) + 0x40;
        uint16_t target = (uint16_t)ctx->gpr[5];
        static bool reported = false;
        if (!reported) {
                fprintf(stderr, "[spu-fence] object=%08X base=%08X lane=%08X target=%u\\n",
                        (uint32_t)ctx->gpr[3], base, lane, target);
                reported = true;
        }
        if (base && lane >= base && lane <= 0xFFFFFFF0u) {
                for (uint32_t offset = 2; offset <= 0xE; offset += 2) {
                        if (vm_read16(lane + offset) < target)
                                vm_write16(lane + offset, target);
                }
        }
        return;
}"""
    replacement = replacement.replace(b"\r\n", newline)
    PATH.write_bytes(data[:start] + replacement + data[end:])
    print("patched", PATH)
