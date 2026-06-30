from pathlib import Path


path = Path(__file__).parent / "recompiled" / "ppu_recomp_b0014.cpp"
data = path.read_bytes()

old = b"""        { uint32_t tmp = vm_read32(ctx->gpr[10] + 0x10); float ftmp; memcpy(&ftmp, &tmp, 4); ctx->fpr[0] = ftmp; }\r
        { double a = ctx->fpr[13]; double b = ctx->fpr[0]; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 0)) | (cr_val << 0); }"""
new = b"""        { uint32_t tmp = vm_read32(ctx->gpr[10] + 0x10); float ftmp; memcpy(&ftmp, &tmp, 4); ctx->fpr[0] = ftmp; }\r
        /* Bring-up guard: an unconstructed clock has a zero period, which\r
         * makes the original subtraction loop non-terminating. */\r
        if (!(ctx->fpr[0] > 0.0)) goto loc_00954A38;\r
        { double a = ctx->fpr[13]; double b = ctx->fpr[0]; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 0)) | (cr_val << 0); }"""

if new in data:
    print("clock zero-step guard already patched")
elif data.count(old) != 1:
    raise SystemExit(f"expected one clock loop, found {data.count(old)}")
else:
    path.write_bytes(data.replace(old, new, 1))
    print("clock zero-step guard patched")
