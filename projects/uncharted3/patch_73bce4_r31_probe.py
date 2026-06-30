from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0011.cpp"
MARKER = b"probe73bce4_base"

data = PATH.read_bytes()
newline = b"\r\n" if b"\r\n" in data[:4096] else b"\n"
start_marker = b"void func_0073BCE4(ppu_context* ctx) {\r\n".replace(b"\r\n", newline)
end_marker = b"\r\nvoid func_0073C958(ppu_context* ctx) {\r\n".replace(b"\r\n", newline)

start = data.index(start_marker)
end = data.index(end_marker, start)
body = data[start:end]

if MARKER in body:
    print("func_0073BCE4 r31 probe already present", PATH)
else:
    entry = b"""void func_0073BCE4(ppu_context* ctx) {
        uint32_t probe73bce4_base = (uint32_t)ctx->gpr[3];
        bool probe73bce4_reported = false;
""".replace(b"\n", newline)
    body = body.replace(start_marker, entry, 1)
    anchor = b" DRAIN_TRAMPOLINE(ctx);"
    check = b""" DRAIN_TRAMPOLINE(ctx);
        if (!probe73bce4_reported && (uint32_t)ctx->gpr[31] != probe73bce4_base) {
                fprintf(stderr, "[probe73bce4] r31 changed after source line %u: base=%08X r31=%08X r24=%08X\\n",
                        (unsigned)__LINE__, probe73bce4_base,
                        (uint32_t)ctx->gpr[31], (uint32_t)ctx->gpr[24]);
                probe73bce4_reported = true;
        }
""".replace(b"\n", newline)
    if anchor not in body:
        raise SystemExit("DRAIN_TRAMPOLINE anchor not found")
    body = body.replace(anchor, check)
    PATH.write_bytes(data[:start] + body + data[end:])
    print("patched", PATH)
