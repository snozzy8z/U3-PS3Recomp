from pathlib import Path


SOURCE = Path(__file__).parent / "recompiled" / "ppu_recomp_mid0007.cpp"
data = SOURCE.read_bytes()
newline = b"\r\n" if b"\r\n" in data[:4096] else b"\n"

old = newline.join(
    (
        b"        if (((ctx->cr >> 0) & 8)) goto loc_008B6270;",
        b"}",
        b"",
        b"void func_008B62A4(ppu_context* ctx) {",
    )
)
new = newline.join(
    (
        b"        if (((ctx->cr >> 0) & 8)) goto loc_008B6270;",
        b"        { g_trampoline_fn = (void(*)(void*))func_008B62A4; return; }",
        b"}",
        b"",
        b"void func_008B62A4(ppu_context* ctx) {",
    )
)

if data.count(new) == 1:
    raise SystemExit("func_008B60BC fallthrough already patched")
if data.count(old) != 1:
    raise SystemExit("expected func_008B60BC tail not found exactly once")

SOURCE.write_bytes(data.replace(old, new, 1))
print("patched func_008B60BC fallthrough to func_008B62A4")
