from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0000.cpp"
data = PATH.read_bytes()

data = data.replace(b"        bool probe428_r30_reported = false;\r\n", b"")
block = (
    b"        if (!probe428_r30_reported && (uint32_t)ctx->gpr[30] != 0x011354E0u) {\r\n"
    b"            fprintf(stderr, \"[probe428] first r30 drift line=%d r30=%08X\\n\", __LINE__, (uint32_t)ctx->gpr[30]);\r\n"
    b"            probe428_r30_reported = true;\r\n"
    b"        }\r\n"
)
count = data.count(block)
data = data.replace(block, b"")
PATH.write_bytes(data)
print(f"cleaned {PATH} ({count} checks)")
