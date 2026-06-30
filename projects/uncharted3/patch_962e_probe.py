from pathlib import Path


path = Path(__file__).parent / "recompiled" / "ppu_recomp_b0014.cpp"
data = path.read_bytes()

old = b"""void func_00962EA0(ppu_context* ctx) {\r
        vm_write64(ctx->gpr[1] + -0xF0, ctx->gpr[1]); ctx->gpr[1] += -0xF0;"""
new = b"""void func_00962EA0(ppu_context* ctx) {\r
        {\r
            static uint32_t probe_calls = 0;\r
            uint32_t call = ++probe_calls;\r
            if (call <= 16) {\r
                uint32_t p = (uint32_t)ctx->gpr[3];\r
                fprintf(stderr,\r
                        "[probe962e] call=%u sp=%08X lr=%08X r3=%08X r4=%08X r6=%08X r7=%08X f1=%g "\r
                        "obj=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X\\n",\r
                        call, (uint32_t)ctx->gpr[1], (uint32_t)ctx->lr, p,\r
                        (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[6],\r
                        (uint32_t)ctx->gpr[7], ctx->fpr[1],\r
                        vm_read32(p + 0x00), vm_read32(p + 0x04), vm_read32(p + 0x08),\r
                        vm_read32(p + 0x0C), vm_read32(p + 0x10), vm_read32(p + 0x14),\r
                        vm_read32(p + 0x18), vm_read32(p + 0x1C), vm_read32(p + 0x20),\r
                        vm_read32(p + 0x24), vm_read32(p + 0x28), vm_read32(p + 0x2C));\r
            }\r
        }\r
        vm_write64(ctx->gpr[1] + -0xF0, ctx->gpr[1]); ctx->gpr[1] += -0xF0;"""

if new in data:
    print("func_00962EA0 probe already patched")
elif data.count(old) != 1:
    raise SystemExit(f"expected one func_00962EA0 entry, found {data.count(old)}")
else:
    path.write_bytes(data.replace(old, new, 1))
    print("func_00962EA0 probe patched")
