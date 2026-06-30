from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0000.cpp"
START = b"void func_000428A0(ppu_context* ctx) {\r\n"
END = b"\r\nvoid func_000443C0(ppu_context* ctx) {\r\n"
MARKER = b"[probe428] first r30 drift"

data = PATH.read_bytes()
if MARKER in data:
    raise SystemExit("r30 probe already present")

start = data.index(START)
end = data.index(END, start)
body = data[start:end]

declaration_anchor = (
    b"        fprintf(stderr, \"[probe428] enter tid=%llu sp=%08X r3=%08X\\n\",\r\n"
    b"                (unsigned long long)ctx->thread_id, (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[3]);\r\n"
)
if body.count(declaration_anchor) != 1:
    raise SystemExit("flow probe declaration anchor missing")
body = body.replace(
    declaration_anchor,
    declaration_anchor + b"        bool probe428_r30_reported = false;\r\n",
    1,
)

anchor = b"DRAIN_TRAMPOLINE(ctx);\r\n"
check = (
    b"DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        if (!probe428_r30_reported && (uint32_t)ctx->gpr[30] != 0x011354E0u) {\r\n"
    b"            fprintf(stderr, \"[probe428] first r30 drift line=%d r30=%08X\\n\", __LINE__, (uint32_t)ctx->gpr[30]);\r\n"
    b"            probe428_r30_reported = true;\r\n"
    b"        }\r\n"
)
count = body.count(anchor)
if count < 20:
    raise SystemExit(f"unexpectedly few call sites: {count}")
body = body.replace(anchor, check)

PATH.write_bytes(data[:start] + body + data[end:])
print(f"patched {PATH} ({count} checks)")
