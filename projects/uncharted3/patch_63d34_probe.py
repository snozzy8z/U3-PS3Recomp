from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0016.cpp"
MARKER = b"probe63d34"

data = PATH.read_bytes()
newline = b"\r\n" if b"\r\n" in data[:4096] else b"\n"
start = b"void func_00A63D34(ppu_context* ctx) {\r\n"
start_probe = (
    b"void func_00A63D34(ppu_context* ctx) {\r\n"
    b"        static uint32_t probe63d34 = 0;\r\n"
    b"        uint32_t probe63d34_id = probe63d34++;\r\n"
    b"        if (probe63d34_id < 8) fprintf(stderr, \"[probe63d34] in #%u ctx=%08X pair=%016llX cb=%08X aux=%08X\\n\", probe63d34_id, (uint32_t)ctx->gpr[3], (unsigned long long)ctx->gpr[4], (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6]);\r\n"
)
end = (
    b"        vm_write32(ctx->gpr[11] + 0x4, ctx->gpr[9]);\r\n"
    b"        return;\r\n"
)
end_probe = (
    b"        vm_write32(ctx->gpr[11] + 0x4, ctx->gpr[9]);\r\n"
    b"        if (probe63d34_id < 8) fprintf(stderr, \"[probe63d34] out #%u begin=%08X end=%08X current=%08X cb=%08X\\n\", probe63d34_id, vm_read32(ctx->gpr[11] + 0x0), vm_read32(ctx->gpr[11] + 0x4), vm_read32(ctx->gpr[11] + 0x8), vm_read32(ctx->gpr[11] + 0xC));\r\n"
    b"        return;\r\n"
)

if newline == b"\n":
    start = start.replace(b"\r\n", newline)
    start_probe = start_probe.replace(b"\r\n", newline)
    end = end.replace(b"\r\n", newline)
    end_probe = end_probe.replace(b"\r\n", newline)

if MARKER in data:
    print("func_00A63D34 probe already present", PATH)
else:
    function_at = data.index(start)
    function_end = data.index(b"void func_00A63E18", function_at)
    body = data[function_at:function_end]
    if end not in body:
        raise SystemExit("func_00A63D34 return anchor not found")
    body = body.replace(start, start_probe, 1).replace(end, end_probe, 1)
    PATH.write_bytes(data[:function_at] + body + data[function_end:])
    print("patched", PATH)
