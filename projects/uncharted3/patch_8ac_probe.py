from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0013.cpp"


def replace_once(data: bytes, old: bytes, new: bytes) -> bytes:
    count = data.count(old)
    if count != 1:
        raise SystemExit(f"expected one match, found {count}: {old[:100]!r}")
    return data.replace(old, new, 1)


data = PATH.read_bytes()
data = replace_once(
    data,
    b"void func_008AC358(ppu_context* ctx) {\r\n",
    b"void func_008AC358(ppu_context* ctx) {\r\n"
    b"        fprintf(stderr, \"[probe8ac] 358 enter sp=%08X r30=%08X\\n\", (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[30]);\r\n",
)
data = replace_once(
    data,
    b"loc_008AC410:\r\n"
    b"        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0xC0);\r\n"
    b"        ctx->gpr[3] = ppc_rldicl(ctx->gpr[29], 0, 32);\r\n"
    b"        ctx->gpr[27] = vm_read64(ctx->gpr[1] + 0x88);\r\n"
    b"        ctx->gpr[28] = vm_read64(ctx->gpr[1] + 0x90);\r\n"
    b"        ctx->lr = ctx->gpr[0];\r\n"
    b"        ctx->gpr[29] = vm_read64(ctx->gpr[1] + 0x98);\r\n"
    b"        ctx->gpr[30] = vm_read64(ctx->gpr[1] + 0xA0);\r\n"
    b"        ctx->gpr[31] = vm_read64(ctx->gpr[1] + 0xA8);\r\n",
    b"loc_008AC410:\r\n"
    b"        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0xC0);\r\n"
    b"        ctx->gpr[3] = ppc_rldicl(ctx->gpr[29], 0, 32);\r\n"
    b"        ctx->gpr[27] = vm_read64(ctx->gpr[1] + 0x88);\r\n"
    b"        ctx->gpr[28] = vm_read64(ctx->gpr[1] + 0x90);\r\n"
    b"        ctx->lr = ctx->gpr[0];\r\n"
    b"        ctx->gpr[29] = vm_read64(ctx->gpr[1] + 0x98);\r\n"
    b"        fprintf(stderr, \"[probe8ac] 358 exit sp=%08X local_r30=%08X saved_r30=%08X\\n\",\r\n"
    b"                (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[30], (uint32_t)vm_read64(ctx->gpr[1] + 0xA0));\r\n"
    b"        ctx->gpr[30] = vm_read64(ctx->gpr[1] + 0xA0);\r\n"
    b"        ctx->gpr[31] = vm_read64(ctx->gpr[1] + 0xA8);\r\n",
)
data = replace_once(
    data,
    b"        ctx->gpr[1] = (int64_t)(int32_t)(ctx->gpr[1] + 0x90);\r\n"
    b"        { g_trampoline_fn = (void(*)(void*))func_008AC358; return; }\r\n",
    b"        ctx->gpr[1] = (int64_t)(int32_t)(ctx->gpr[1] + 0x90);\r\n"
    b"        fprintf(stderr, \"[probe8ac] 438 tail sp=%08X restored_r30=%08X\\n\", (uint32_t)ctx->gpr[1], (uint32_t)ctx->gpr[30]);\r\n"
    b"        { g_trampoline_fn = (void(*)(void*))func_008AC358; return; }\r\n",
)
PATH.write_bytes(data)
print("patched", PATH)
