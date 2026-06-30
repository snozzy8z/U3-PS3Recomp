from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0016.cpp"
MARKER = b"probe_a6e140_entry_sp"

data = PATH.read_bytes()
newline = b"\r\n" if b"\r\n" in data[:4096] else b"\n"
start_marker = b"void func_00A6E140(ppu_context* ctx) {\r\n".replace(b"\r\n", newline)
end_marker = b"\r\nvoid func_00A6F23C(ppu_context* ctx) {\r\n".replace(b"\r\n", newline)

start = data.index(start_marker)
end = data.index(end_marker, start)
body = data[start:end]

if MARKER in body:
    print("func_00A6E140 stack probe already present", PATH)
else:
    entry = b"""void func_00A6E140(ppu_context* ctx) {
        uint32_t probe_a6e140_entry_sp = (uint32_t)ctx->gpr[1];
        bool probe_a6e140_reported = false;
""".replace(b"\n", newline)
    body = body.replace(start_marker, entry, 1)
    anchor = b" DRAIN_TRAMPOLINE(ctx);"
    check = b""" DRAIN_TRAMPOLINE(ctx);
        if (!probe_a6e140_reported && (uint32_t)ctx->gpr[1] != probe_a6e140_entry_sp - 0x190) {
                fprintf(stderr, "[probeA6E140] r1 changed after source line %u: entry=%08X expected=%08X actual=%08X\\n",
                        (unsigned)__LINE__, probe_a6e140_entry_sp,
                        probe_a6e140_entry_sp - 0x190, (uint32_t)ctx->gpr[1]);
                probe_a6e140_reported = true;
        }
""".replace(b"\n", newline)
    if anchor not in body:
        raise SystemExit("DRAIN_TRAMPOLINE anchor not found")
    body = body.replace(anchor, check)
    PATH.write_bytes(data[:start] + body + data[end:])
    print("patched", PATH)
