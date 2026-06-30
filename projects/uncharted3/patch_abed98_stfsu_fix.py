from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0016.cpp"
START = b"void func_00ABED98(ppu_context* ctx) {\r\n"
END = b"\r\nvoid func_00AC05D8(ppu_context* ctx) {\r\n"

data = PATH.read_bytes()
start = data.index(START)
end = data.index(END, start)
body = data[start:end]
old = (
    b"        { float ftmp = (float)ctx->fpr[23]; uint32_t tmp; memcpy(&tmp, &ftmp, 4); "
    b"vm_write32(ctx->gpr[31] + 0x220, tmp); }\r\n"
)
new = (
    b"        { uint64_t ea = ctx->gpr[31] + 0x220; float ftmp = (float)ctx->fpr[23]; "
    b"uint32_t tmp; memcpy(&tmp, &ftmp, 4); vm_write32(ea, tmp); ctx->gpr[31] = ea; }\r\n"
)
count = body.count(old)
if count != 1:
    raise SystemExit(f"expected one stfsu site, found {count}")
body = body.replace(old, new, 1)
PATH.write_bytes(data[:start] + body + data[end:])
print("patched", PATH)
