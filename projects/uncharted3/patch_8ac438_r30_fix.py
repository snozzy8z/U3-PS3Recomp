from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0000.cpp"
data = PATH.read_bytes()
original = (
    b"        ctx->gpr[31] = vm_read32(ctx->gpr[30] + -0x79A8);\r\n"
    b"        ctx->gpr[3] = ctx->gpr[31] | ctx->gpr[31];\r\n"
    b"        func_008AC438(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
)
old_fix = (
    b"        ctx->gpr[31] = vm_read32(ctx->gpr[30] + -0x79A8);\r\n"
    b"        ctx->gpr[3] = ctx->gpr[31] | ctx->gpr[31];\r\n"
    b"        /* Enforce the PPC callee-saved ABI: this lifted tail-chain leaks its private r30 TOC. */\r\n"
    b"        { uint64_t saved_r30 = ctx->gpr[30];\r\n"
    b"          func_008AC438(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"          ctx->gpr[30] = saved_r30; }\r\n"
)
fixed = (
    b"        ctx->gpr[31] = vm_read32(ctx->gpr[30] + -0x79A8);\r\n"
    b"        ctx->gpr[3] = ctx->gpr[31] | ctx->gpr[31];\r\n"
    b"        /* Enforce the PPC callee-saved ABI: this lifted tail-chain can return with its 0x90-byte frame active. */\r\n"
    b"        { uint64_t saved_sp = ctx->gpr[1];\r\n"
    b"          uint64_t saved_r29 = ctx->gpr[29], saved_r30 = ctx->gpr[30], saved_r31 = ctx->gpr[31];\r\n"
    b"          func_008AC438(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"          ctx->gpr[1] = saved_sp;\r\n"
    b"          ctx->gpr[29] = saved_r29; ctx->gpr[30] = saved_r30; ctx->gpr[31] = saved_r31; }\r\n"
)

newline = b"\r\n" if b"\r\n" in data[:4096] else b"\n"
if newline == b"\n":
    original = original.replace(b"\r\n", newline)
    old_fix = old_fix.replace(b"\r\n", newline)
    fixed = fixed.replace(b"\r\n", newline)

if fixed in data:
    print("frame fix already present", PATH)
elif old_fix in data:
    PATH.write_bytes(data.replace(old_fix, fixed, 1))
    print("upgraded r30 fix to frame fix", PATH)
elif original in data:
    PATH.write_bytes(data.replace(original, fixed, 1))
    print("patched", PATH)
else:
    raise SystemExit("expected func_008AC438 call site not found")
