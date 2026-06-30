from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0000.cpp"
data = PATH.read_bytes()

blocks = [
    b"        fprintf(stderr, \"[probe428] enter tid=%llu sp=%08X r3=%08X\\n\",\r\n"
    b"                (unsigned long long)ctx->thread_id, (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[3]);\r\n",
    b"        fprintf(stderr, \"[probe428] before 009ABE84/44898 phase r3=%08X r4=%08X\\n\",\r\n"
    b"                (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4]);\r\n",
    b"        fprintf(stderr, \"[probe428] 009ABE84 result=%08X\\n\", (uint32_t)ctx->gpr[3]);\r\n",
    b"        fprintf(stderr, \"[probe428] call 0095F998 r3=%08X r4=%08X r5=%08X\\n\",\r\n"
    b"                (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5]);\r\n",
    b"        fprintf(stderr, \"[probe428] return 0095F998 r3=%08X\\n\", (uint32_t)ctx->gpr[3]);\r\n",
    b"        fprintf(stderr, \"[probe428] early-exit 00044340 r3=%08X\\n\", (uint32_t)ctx->gpr[3]);\r\n",
    b"        fprintf(stderr, \"[probe428] after 00044898 flag@%08X=%u r3=%08X\\n\",\r\n"
    b"                (uint32_t)ctx->gpr[31], (unsigned)vm_read8(ctx->gpr[31]), (uint32_t)ctx->gpr[3]);\r\n",
    b"        fprintf(stderr, \"[probe428] after 00D78874 flag@%08X=%u r3=%08X\\n\",\r\n"
    b"                (uint32_t)ctx->gpr[31], (unsigned)vm_read8(ctx->gpr[31]), (uint32_t)ctx->gpr[3]);\r\n",
    b"        fprintf(stderr, \"[probe428] after 007AD0C0 flag@%08X=%u r3=%08X\\n\",\r\n"
    b"                (uint32_t)ctx->gpr[31], (unsigned)vm_read8(ctx->gpr[31]), (uint32_t)ctx->gpr[3]);\r\n",
]

for block in blocks:
    count = data.count(block)
    if count > 1:
        raise SystemExit(f"unexpected duplicate probe block: {count}")
    data = data.replace(block, b"")

PATH.write_bytes(data)
print("cleaned", PATH)
