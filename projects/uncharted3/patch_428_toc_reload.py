from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0000.cpp"
data = PATH.read_bytes()
original = (
    b"        func_0093CC74(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        /* nop */;\r\n"
    b"        ctx->gpr[3] = vm_read32(ctx->gpr[30] + -0x793C);\r\n"
)
fixed = (
    b"        func_0093CC74(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    b"        /* nop */;\r\n"
    b"        /* Restore this function's module TOC after lifted callees. */\r\n"
    b"        ctx->gpr[30] = vm_read32(ctx->gpr[2] + -0x7F50);\r\n"
    b"        ctx->gpr[3] = vm_read32(ctx->gpr[30] + -0x793C);\r\n"
)

newline = b"\r\n" if b"\r\n" in data[:4096] else b"\n"
if newline == b"\n":
    original = original.replace(b"\r\n", newline)
    fixed = fixed.replace(b"\r\n", newline)

if fixed in data:
    print("TOC reload already present", PATH)
elif original in data:
    PATH.write_bytes(data.replace(original, fixed, 1))
    print("patched", PATH)
else:
    raise SystemExit("expected func_000428A0 configuration call site not found")
