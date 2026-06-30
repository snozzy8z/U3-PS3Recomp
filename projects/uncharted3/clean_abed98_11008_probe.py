from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0016.cpp"
data = PATH.read_bytes()
block = (
    b"        if ((uint32_t)ctx->ctr == 0x00011008u)\r\n"
    b"            fprintf(stderr, \"[probe11008] source-line=%d sp=%08X r3=%08X r9=%08X r11=%08X r12=%08X\\n\",\r\n"
    b"                    __LINE__, (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[3],\r\n"
    b"                    (uint32_t)ctx->gpr[9], (uint32_t)ctx->gpr[11], (uint32_t)ctx->gpr[12]);\r\n"
)
count = data.count(block)
data = data.replace(block, b"")
data = data.replace(
    b"        fprintf(stderr, \"[probe58] replacement opd=%08X target=%08X toc=%08X arg=%08X\\n\",\r\n"
    b"                (uint32_t)ctx->gpr[9], (uint32_t)ctx->ctr, (uint32_t)ctx->gpr[2], (uint32_t)ctx->gpr[3]);\r\n",
    b"",
)
data = data.replace(
    b"        fprintf(stderr, \"[probe58] replacement result=%08X\\n\", (uint32_t)ctx->gpr[3]);\r\n",
    b"",
)
PATH.write_bytes(data)
print(f"cleaned {PATH} ({count} sites)")
