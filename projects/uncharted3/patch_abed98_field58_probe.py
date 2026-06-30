from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0016.cpp"
data = PATH.read_bytes()
old = (
    b"        ctx->gpr[0] = vm_read32(ctx->gpr[9] + 0x0);\r\n"
    b"        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);\r\n"
    b"        ctx->ctr = (uint32_t)ctx->gpr[0];\r\n"
    b"        ctx->gpr[2] = vm_read32(ctx->gpr[9] + 0x4);\r\n"
    b"        if ((uint32_t)ctx->ctr == 0x00011008u)\r\n"
    b"            fprintf(stderr, \"[probe11008] source-line=%d sp=%08X r3=%08X r9=%08X r11=%08X r12=%08X\\n\",\r\n"
    b"                    __LINE__, (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[3],\r\n"
    b"                    (uint32_t)ctx->gpr[9], (uint32_t)ctx->gpr[11], (uint32_t)ctx->gpr[12]);\r\n"
    b"        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);\r\n"
    b"        vm_write32(ctx->gpr[16] + 0x58, ctx->gpr[3]);\r\n"
    b"        ctx->gpr[3] = ctx->gpr[29] | ctx->gpr[29];\r\n"
)
new = (
    b"        ctx->gpr[0] = vm_read32(ctx->gpr[9] + 0x0);\r\n"
    b"        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);\r\n"
    b"        ctx->ctr = (uint32_t)ctx->gpr[0];\r\n"
    b"        ctx->gpr[2] = vm_read32(ctx->gpr[9] + 0x4);\r\n"
    b"        fprintf(stderr, \"[probe58] replacement opd=%08X target=%08X toc=%08X arg=%08X\\n\",\r\n"
    b"                (uint32_t)ctx->gpr[9], (uint32_t)ctx->ctr, (uint32_t)ctx->gpr[2], (uint32_t)ctx->gpr[3]);\r\n"
    b"        if ((uint32_t)ctx->ctr == 0x00011008u)\r\n"
    b"            fprintf(stderr, \"[probe11008] source-line=%d sp=%08X r3=%08X r9=%08X r11=%08X r12=%08X\\n\",\r\n"
    b"                    __LINE__, (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[3],\r\n"
    b"                    (uint32_t)ctx->gpr[9], (uint32_t)ctx->gpr[11], (uint32_t)ctx->gpr[12]);\r\n"
    b"        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);\r\n"
    b"        fprintf(stderr, \"[probe58] replacement result=%08X\\n\", (uint32_t)ctx->gpr[3]);\r\n"
    b"        vm_write32(ctx->gpr[16] + 0x58, ctx->gpr[3]);\r\n"
    b"        ctx->gpr[3] = ctx->gpr[29] | ctx->gpr[29];\r\n"
)
count = data.count(old)
if count != 1:
    raise SystemExit(f"expected one replacement callback, found {count}")
PATH.write_bytes(data.replace(old, new, 1))
print("patched", PATH)
