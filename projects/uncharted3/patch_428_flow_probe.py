from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0000.cpp"


def replace_once(data: bytes, old: bytes, new: bytes) -> bytes:
    count = data.count(old)
    if count != 1:
        raise SystemExit(f"expected one match, found {count}: {old[:80]!r}")
    return data.replace(old, new, 1)


data = PATH.read_bytes()

data = replace_once(
    data,
    b"void func_000428A0(ppu_context* ctx) {\r\n",
    b"void func_000428A0(ppu_context* ctx) {\r\n"
    b"        fprintf(stderr, \"[probe428] enter tid=%llu sp=%08X r3=%08X\\n\",\r\n"
    b"                (unsigned long long)ctx->thread_id, (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[3]);\r\n",
)

data = replace_once(
    data,
    b"        ctx->gpr[3] = ctx->gpr[29] | ctx->gpr[29];\r\n"
    b"        func_009ABCA0(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        /* nop */;\r\n"
    b"        ctx->gpr[4] = ppc_rldicl(ctx->gpr[3], 0, 32);\r\n"
    b"        ctx->gpr[3] = ctx->gpr[27] | ctx->gpr[27];\r\n"
    b"        func_00044898(ctx); DRAIN_TRAMPOLINE(ctx);\r\n",
    b"        ctx->gpr[3] = ctx->gpr[29] | ctx->gpr[29];\r\n"
    b"        func_009ABCA0(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        /* nop */;\r\n"
    b"        ctx->gpr[4] = ppc_rldicl(ctx->gpr[3], 0, 32);\r\n"
    b"        ctx->gpr[3] = ctx->gpr[27] | ctx->gpr[27];\r\n"
    b"        fprintf(stderr, \"[probe428] before 009ABE84/44898 phase r3=%08X r4=%08X\\n\",\r\n"
    b"                (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4]);\r\n"
    b"        func_00044898(ctx); DRAIN_TRAMPOLINE(ctx);\r\n",
)

data = replace_once(
    data,
    b"        ctx->gpr[3] = ctx->gpr[29] | ctx->gpr[29];\r\n"
    b"        func_009ABCA0(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        /* nop */;\r\n"
    b"        ctx->gpr[4] = ppc_rldicl(ctx->gpr[3], 0, 32);\r\n"
    b"        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[27] + 0x1490);\r\n",
    b"        ctx->gpr[3] = ctx->gpr[29] | ctx->gpr[29];\r\n"
    b"        func_009ABCA0(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        /* nop */;\r\n"
    b"        ctx->gpr[4] = ppc_rldicl(ctx->gpr[3], 0, 32);\r\n"
    b"        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[27] + 0x1490);\r\n",
)

data = replace_once(
    data,
    b"        func_009ABE84(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        /* nop */;\r\n"
    b"        ctx->gpr[31] = vm_read32(ctx->gpr[30] + -0x7E58);\r\n",
    b"        func_009ABE84(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        /* nop */;\r\n"
    b"        fprintf(stderr, \"[probe428] 009ABE84 result=%08X\\n\", (uint32_t)ctx->gpr[3]);\r\n"
    b"        ctx->gpr[31] = vm_read32(ctx->gpr[30] + -0x7E58);\r\n",
)

data = replace_once(
    data,
    b"        vm_write32(ctx->gpr[1] + 0x7C, ctx->gpr[0]);\r\n"
    b"        func_0095F998(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        /* nop */;\r\n",
    b"        vm_write32(ctx->gpr[1] + 0x7C, ctx->gpr[0]);\r\n"
    b"        fprintf(stderr, \"[probe428] call 0095F998 r3=%08X r4=%08X r5=%08X\\n\",\r\n"
    b"                (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5]);\r\n"
    b"        func_0095F998(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        /* nop */;\r\n"
    b"        fprintf(stderr, \"[probe428] return 0095F998 r3=%08X\\n\", (uint32_t)ctx->gpr[3]);\r\n",
)

data = replace_once(
    data,
    b"loc_00044340:\r\n",
    b"loc_00044340:\r\n"
    b"        fprintf(stderr, \"[probe428] early-exit 00044340 r3=%08X\\n\", (uint32_t)ctx->gpr[3]);\r\n",
)

PATH.write_bytes(data)
print("patched", PATH)
