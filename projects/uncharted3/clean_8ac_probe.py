from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0013.cpp"
data = PATH.read_bytes()
blocks = [
    b"        fprintf(stderr, \"[probe8ac] 358 enter sp=%08X r30=%08X\\n\", (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[30]);\r\n",
    b"        fprintf(stderr, \"[probe8ac] 358 exit sp=%08X local_r30=%08X saved_r30=%08X\\n\",\r\n"
    b"                (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[30], (uint32_t)vm_read64(ctx->gpr[1] + 0xA0));\r\n",
    b"        fprintf(stderr, \"[probe8ac] 438 tail sp=%08X restored_r30=%08X\\n\", (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[30]);\r\n",
]
for block in blocks:
    data = data.replace(block, b"")
PATH.write_bytes(data)
print("cleaned", PATH)
