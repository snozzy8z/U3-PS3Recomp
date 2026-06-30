from pathlib import Path


path = Path(__file__).parent / "recompiled" / "ppu_recomp_b0014.cpp"
data = path.read_bytes()

entry_probe = b"""void func_009548D8(ppu_context* ctx) {\r
        uint32_t probe_9548_loops = 0;\r
        vm_write64(ctx->gpr[1] + -0xA0, ctx->gpr[1]); ctx->gpr[1] += -0xA0;"""
entry_clean = b"""void func_009548D8(ppu_context* ctx) {\r
        vm_write64(ctx->gpr[1] + -0xA0, ctx->gpr[1]); ctx->gpr[1] += -0xA0;"""

loop_probe = b"""        { uint32_t tmp = vm_read32(ctx->gpr[10] + 0x10); float ftmp; memcpy(&ftmp, &tmp, 4); ctx->fpr[0] = ftmp; }\r
        probe_9548_loops++;\r
        if (probe_9548_loops == 1 && (!(ctx->fpr[0] > 0.0) || !isfinite(ctx->fpr[0]) || !isfinite(ctx->fpr[13])))\r
            fprintf(stderr, "[probe9548] obj=0x%08X source=0x%08X accum=%g step=%g flags=0x%08X\\n",\r
                    (uint32_t)ctx->gpr[31], (uint32_t)ctx->gpr[10], ctx->fpr[13],\r
                    ctx->fpr[0], vm_read32(ctx->gpr[31]));\r
        if (probe_9548_loops > 100000) {\r
            fprintf(stderr, "[probe9548] loop guard: obj=0x%08X accum=%g step=%g\\n",\r
                    (uint32_t)ctx->gpr[31], ctx->fpr[13], ctx->fpr[0]);\r
            goto loc_00954A38;\r
        }\r
        { double a = ctx->fpr[13]; double b = ctx->fpr[0]; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 0)) | (cr_val << 0); }"""
loop_clean = b"""        { uint32_t tmp = vm_read32(ctx->gpr[10] + 0x10); float ftmp; memcpy(&ftmp, &tmp, 4); ctx->fpr[0] = ftmp; }\r
        { double a = ctx->fpr[13]; double b = ctx->fpr[0]; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 0)) | (cr_val << 0); }"""

for old, new, label in ((entry_probe, entry_clean, "entry"), (loop_probe, loop_clean, "loop")):
    if old not in data:
        print(f"{label}: probe not present")
        continue
    data = data.replace(old, new, 1)
    print(f"{label}: probe removed")

path.write_bytes(data)
