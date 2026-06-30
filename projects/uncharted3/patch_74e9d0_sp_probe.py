from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0011.cpp"
MARKER = b"probe_74e9d0_entry_sp"

data = PATH.read_bytes()
newline = b"\r\n" if b"\r\n" in data[:4096] else b"\n"
start_marker = b"void func_0074E9D0(ppu_context* ctx) {\r\n".replace(b"\r\n", newline)
end_marker = b"\r\nvoid func_0074EE08(ppu_context* ctx) {\r\n".replace(b"\r\n", newline)

start = data.index(start_marker)
end = data.index(end_marker, start)
body = data[start:end]

if MARKER in body:
    print("func_0074E9D0 stack probe already present", PATH)
else:
    entry = b"""void func_0074E9D0(ppu_context* ctx) {
        uint32_t probe_74e9d0_entry_sp = (uint32_t)ctx->gpr[1];
        bool probe_74e9d0_reported = false;
""".replace(b"\n", newline)
    body = body.replace(start_marker, entry, 1)
    anchor = b" DRAIN_TRAMPOLINE(ctx);"
    check = b""" DRAIN_TRAMPOLINE(ctx);
        if (!probe_74e9d0_reported && (uint32_t)ctx->gpr[1] != probe_74e9d0_entry_sp - 0x110) {
                fprintf(stderr, "[probe74E9D0] r1 changed after source line %u: entry=%08X expected=%08X actual=%08X\\n",
                        (unsigned)__LINE__, probe_74e9d0_entry_sp,
                        probe_74e9d0_entry_sp - 0x110, (uint32_t)ctx->gpr[1]);
                probe_74e9d0_reported = true;
        }
""".replace(b"\n", newline)
    if anchor not in body:
        raise SystemExit("DRAIN_TRAMPOLINE anchor not found")
    body = body.replace(anchor, check)
    epilogue = b"loc_0074EDCC:" + newline
    epilogue_probe = b"""loc_0074EDCC:
        if (!probe_74e9d0_reported && (uint32_t)ctx->gpr[1] != probe_74e9d0_entry_sp - 0x110)
                fprintf(stderr, "[probe74E9D0] r1 changed before epilogue: entry=%08X expected=%08X actual=%08X\\n",
                        probe_74e9d0_entry_sp, probe_74e9d0_entry_sp - 0x110,
                        (uint32_t)ctx->gpr[1]);
""".replace(b"\n", newline)
    if epilogue not in body:
        raise SystemExit("func_0074E9D0 epilogue anchor not found")
    body = body.replace(epilogue, epilogue_probe, 1)
    PATH.write_bytes(data[:start] + body + data[end:])
    print("patched", PATH)
