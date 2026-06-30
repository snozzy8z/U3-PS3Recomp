from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0000.cpp"


def replace_once(data: bytes, old: bytes, new: bytes) -> bytes:
    count = data.count(old)
    if count != 1:
        raise SystemExit(f"expected one match, found {count}: {old[:100]!r}")
    return data.replace(old, new, 1)


data = PATH.read_bytes()

data = replace_once(
    data,
    b"        func_00044898(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        /* nop */;\r\n"
    b"        ctx->gpr[3] = ctx->gpr[29] | ctx->gpr[29];\r\n",
    b"        func_00044898(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        /* nop */;\r\n"
    b"        fprintf(stderr, \"[probe428] after 00044898 flag@%08X=%u r3=%08X\\n\",\r\n"
    b"                (uint32_t)ctx->gpr[31], (unsigned)vm_read8(ctx->gpr[31]), (uint32_t)ctx->gpr[3]);\r\n"
    b"        ctx->gpr[3] = ctx->gpr[29] | ctx->gpr[29];\r\n",
)

data = replace_once(
    data,
    b"        func_00D78874(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        /* nop */;\r\n"
    b"        func_007AD0C0(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        /* nop */;\r\n"
    b"        ctx->gpr[25] = vm_read8(ctx->gpr[31] + 0x0);\r\n",
    b"        func_00D78874(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        /* nop */;\r\n"
    b"        fprintf(stderr, \"[probe428] after 00D78874 flag@%08X=%u r3=%08X\\n\",\r\n"
    b"                (uint32_t)ctx->gpr[31], (unsigned)vm_read8(ctx->gpr[31]), (uint32_t)ctx->gpr[3]);\r\n"
    b"        func_007AD0C0(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        /* nop */;\r\n"
    b"        fprintf(stderr, \"[probe428] after 007AD0C0 flag@%08X=%u r3=%08X\\n\",\r\n"
    b"                (uint32_t)ctx->gpr[31], (unsigned)vm_read8(ctx->gpr[31]), (uint32_t)ctx->gpr[3]);\r\n"
    b"        ctx->gpr[25] = vm_read8(ctx->gpr[31] + 0x0);\r\n",
)

PATH.write_bytes(data)
print("patched", PATH)
