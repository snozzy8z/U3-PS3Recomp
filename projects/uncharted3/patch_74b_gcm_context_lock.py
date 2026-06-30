from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0011.cpp"
MARKER = b"Serialize the process-global GCM context across host PPU threads."

data = PATH.read_bytes()
newline = b"\r\n" if b"\r\n" in data[:4096] else b"\n"

declaration_anchor = b'#include <math.h>\r\n'
declarations = (
    b'#include <math.h>\r\n\r\n'
    b'extern "C" void ps3_gcm_context_lock(void);\r\n'
    b'extern "C" void ps3_gcm_context_unlock(void);\r\n'
)
lock_anchor = b"void func_0074B5C8(ppu_context* ctx) {\r\n"
lock_code = (
    b"void func_0074B5C8(ppu_context* ctx) {\r\n"
    b"        /* Serialize the process-global GCM context across host PPU threads. */\r\n"
    b"        ps3_gcm_context_lock();\r\n"
)
unlock_anchor = (
    b"        vm_write32(ctx->gpr[9] + 0x0, ctx->gpr[0]);\r\n"
    b"        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0xB0);\r\n"
)
unlock_code = (
    b"        vm_write32(ctx->gpr[9] + 0x0, ctx->gpr[0]);\r\n"
    b"        ps3_gcm_context_unlock();\r\n"
    b"        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0xB0);\r\n"
)

if newline == b"\n":
    declaration_anchor = declaration_anchor.replace(b"\r\n", newline)
    declarations = declarations.replace(b"\r\n", newline)
    lock_anchor = lock_anchor.replace(b"\r\n", newline)
    lock_code = lock_code.replace(b"\r\n", newline)
    unlock_anchor = unlock_anchor.replace(b"\r\n", newline)
    unlock_code = unlock_code.replace(b"\r\n", newline)

if MARKER in data:
    print("GCM context lock already present", PATH)
else:
    if declaration_anchor not in data:
        raise SystemExit("include anchor not found")
    if lock_anchor not in data:
        raise SystemExit("func_0074B5C8 anchor not found")

    unlock_function = data.index(b"void func_0074B7FC(ppu_context* ctx)")
    unlock_at = data.index(unlock_anchor, unlock_function)
    data = data.replace(declaration_anchor, declarations, 1)
    data = data.replace(lock_anchor, lock_code, 1)
    unlock_at = data.index(unlock_anchor, data.index(b"void func_0074B7FC(ppu_context* ctx)"))
    data = data[:unlock_at] + data[unlock_at:].replace(unlock_anchor, unlock_code, 1)
    PATH.write_bytes(data)
    print("patched", PATH)
