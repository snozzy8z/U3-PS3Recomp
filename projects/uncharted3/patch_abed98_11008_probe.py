from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0016.cpp"
START = b"void func_00ABED98(ppu_context* ctx) {\r\n"
END = b"\r\nvoid func_00AC05D8(ppu_context* ctx) {\r\n"
MARKER = b"[probe11008]"

data = PATH.read_bytes()
if MARKER in data:
    raise SystemExit("probe already present")
start = data.index(START)
end = data.index(END, start)
body = data[start:end]
anchor = b"        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
probe = (
    b"        if ((uint32_t)ctx->ctr == 0x00011008u)\r\n"
    b"            fprintf(stderr, \"[probe11008] source-line=%d sp=%08X r3=%08X r9=%08X r11=%08X r12=%08X\\n\",\r\n"
    b"                    __LINE__, (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[3],\r\n"
    b"                    (uint32_t)ctx->gpr[9], (uint32_t)ctx->gpr[11], (uint32_t)ctx->gpr[12]);\r\n"
    b"        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
)
count = body.count(anchor)
if count != 32:
    raise SystemExit(f"expected 32 indirect calls, found {count}")
body = body.replace(anchor, probe)
PATH.write_bytes(data[:start] + body + data[end:])
print(f"patched {PATH} ({count} sites)")
