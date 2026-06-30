#!/usr/bin/env python3
"""Trace preservation of the formatter's r22 output context."""

from pathlib import Path


TARGET = Path(__file__).parent / "recompiled" / "ppu_recomp_mid0013.cpp"
BASE_TARGET = Path(__file__).parent / "recompiled" / "ppu_recomp_b0020.cpp"


def patch_body(data: bytes, function: bytes, needle: bytes, replacement: bytes) -> bytes:
    start = data.index(b"void " + function + b"(ppu_context* ctx) {")
    end = data.index(b"\r\n}\r\n", start)
    body = data[start:end]
    if replacement in body:
        return data
    if body.count(needle) != 1:
        raise RuntimeError(f"unexpected {function.decode()} insertion point")
    return data[:start] + body.replace(needle, replacement, 1) + data[end:]


def patch_formatter_call(data: bytes) -> bytes:
    function = b"func_00D7A6C0"
    start = data.index(b"void " + function + b"(ppu_context* ctx) {")
    end = data.index(b"\r\n}\r\n", start)
    body = data[start:end]
    debug_tokens = (
        b"dbg_printf_r22",
        b"dbg_printf_dumped",
        b"[printf-r22]",
        b"ppu_tramp_dump_c();",
    )
    body = b"".join(
        line for line in body.splitlines(keepends=True)
        if not any(token in line for token in debug_tokens)
    )
    call = b"        func_00D859A8(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    if body.count(call) != 1:
        raise RuntimeError("unexpected func_00D7A6C0 call shape")
    canonical = (
        b"        uint64_t dbg_printf_r22 = ctx->gpr[22];\r\n"
        b"        static int dbg_printf_dumped = 0;\r\n"
        b"        if (!dbg_printf_dumped++) ppu_tramp_dump_c();\r\n"
        b"        fprintf(stderr, \"[printf-r22] before=%08X r3=%08X r4=%08X sp=%08X\\n\", "
        b"(unsigned)dbg_printf_r22, (unsigned)ctx->gpr[3], (unsigned)ctx->gpr[4], "
        b"(unsigned)ctx->gpr[1]);\r\n"
        + call
        + b"        fprintf(stderr, \"[printf-r22] after=%08X expected=%08X r3=%08X sp=%08X\\n\", "
        b"(unsigned)ctx->gpr[22], (unsigned)dbg_printf_r22, (unsigned)ctx->gpr[3], "
        b"(unsigned)ctx->gpr[1]);\r\n"
    )
    body = body.replace(call, canonical, 1)
    return data[:start] + body + data[end:]


def main() -> None:
    data = TARGET.read_bytes()
    declaration = b'extern "C" void ppu_tramp_dump_c(void);\r\n'
    if declaration not in data:
        include = b'#include "ppu_recomp.h"\r\n'
        if include not in data:
            raise RuntimeError("generated source preamble not found")
        data = data.replace(include, include + declaration, 1)

    data = patch_formatter_call(data)
    entry = b"void func_00D7A608(ppu_context* ctx) {\r\n"
    entry_probe = (
        entry
        + b"        fprintf(stderr, \"[printf-entry] D7A608 r3=%08X r30=%08X sp=%08X\\n\", "
        + b"(unsigned)ctx->gpr[3], (unsigned)ctx->gpr[30], (unsigned)ctx->gpr[1]);\r\n"
    )
    if entry_probe not in data:
        data = data.replace(entry, entry_probe, 1)
    assignment = b"        ctx->gpr[22] = ppc_rldicl(ctx->gpr[3], 0, 32);\r\n"
    assignment_probe = (
        assignment
        + b"        fprintf(stderr, \"[printf-step] after-rldicl r22=%08X\\n\", "
        + b"(unsigned)ctx->gpr[22]);\r\n"
    )
    if assignment_probe not in data:
        data = data.replace(assignment, assignment_probe, 1)
    store = b"        { uint64_t tmp; memcpy(&tmp, &ctx->fpr[0], 8); vm_write64(ctx->gpr[3] + 0x0, tmp); }\r\n"
    store_probe = (
        store
        + b"        fprintf(stderr, \"[printf-step] after-store r22=%08X\\n\", "
        + b"(unsigned)ctx->gpr[22]);\r\n"
    )
    if store_probe not in data:
        data = data.replace(store, store_probe, 1)
    for function in (b"func_00D7A63C", b"func_00D7A72C", b"func_00D7A754", b"func_00D7ABA8"):
        label = function.removeprefix(b"func_")
        path_entry = b"void " + function + b"(ppu_context* ctx) {\r\n"
        path_probe = (
            path_entry
            + b"        fprintf(stderr, \"[printf-path] "
            + label
            + b" r22=%08X r3=%08X r20=%08X\\n\", "
            + b"(unsigned)ctx->gpr[22], (unsigned)ctx->gpr[3], (unsigned)ctx->gpr[20]);\r\n"
        )
        if path_probe not in data:
            data = data.replace(path_entry, path_probe, 1)
    TARGET.write_bytes(data)

    base_data = BASE_TARGET.read_bytes()
    base_entry = b"void func_00D79DB4(ppu_context* ctx) {\r\n"
    base_probe = (
        base_entry
        + b"        fprintf(stderr, \"[printf-entry] D79DB4 r3=%08X r4=%08X r5=%08X sp=%08X\\n\", "
        + b"(unsigned)ctx->gpr[3], (unsigned)ctx->gpr[4], (unsigned)ctx->gpr[5], "
        + b"(unsigned)ctx->gpr[1]);\r\n"
    )
    if base_probe not in base_data:
        base_data = base_data.replace(base_entry, base_probe, 1)
    BASE_TARGET.write_bytes(base_data)
    print(f"patched {TARGET}")


if __name__ == "__main__":
    main()
