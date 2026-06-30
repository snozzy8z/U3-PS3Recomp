#!/usr/bin/env python3
"""Add one-shot TOC diagnostics around the current 0x65699C boot wall."""

from pathlib import Path


TARGET = Path(__file__).parent / "recompiled" / "ppu_recomp_b0009.cpp"


def insert_once(data: bytes, needle: bytes, replacement: bytes) -> bytes:
    if replacement in data:
        return data
    count = data.count(needle)
    if count != 1:
        raise RuntimeError(f"expected one insertion point, found {count}")
    return data.replace(needle, replacement, 1)


def insert_in_function(
    data: bytes, function: bytes, needle: bytes, replacement: bytes
) -> bytes:
    start = data.index(b"void " + function + b"(ppu_context* ctx) {")
    end = data.index(b"\r\n}\r\n", start) + len(b"\r\n}\r\n")
    body = data[start:end]
    if replacement in body:
        return data
    count = body.count(needle)
    if count != 1:
        raise RuntimeError(
            f"expected one insertion point in {function.decode()}, found {count}"
        )
    body = body.replace(needle, replacement, 1)
    return data[:start] + body + data[end:]


def main() -> None:
    data = TARGET.read_bytes()

    entry_64f9dc = b"void func_0064F9DC(ppu_context* ctx) {\r\n"
    data = insert_once(
        data,
        entry_64f9dc,
        entry_64f9dc
        + b"        { static int once = 0; if (!once++) fprintf(stderr,\r\n"
        + b"            \"[toc] func_0064F9DC entry r2=0x%08X slot=0x%08X value=0x%08X\\n\",\r\n"
        + b"            (unsigned)ctx->gpr[2], (unsigned)(ctx->gpr[2] - 0x77D0),\r\n"
        + b"            (unsigned)vm_read32(ctx->gpr[2] - 0x77D0)); }\r\n",
    )

    entry_65699c = b"void func_0065699C(ppu_context* ctx) {\r\n"
    data = insert_once(
        data,
        entry_65699c,
        entry_65699c
        + b"        { static int once = 0; if (!once++) fprintf(stderr,\r\n"
        + b"            \"[toc] func_0065699C entry r2=0x%08X slot=0x%08X value=0x%08X\\n\",\r\n"
        + b"            (unsigned)ctx->gpr[2], (unsigned)(ctx->gpr[2] - 0x77C8),\r\n"
        + b"            (unsigned)vm_read32(ctx->gpr[2] - 0x77C8)); }\r\n",
    )

    r28_load = b"        ctx->gpr[28] = vm_read32(ctx->gpr[30] + -0x7FE0);\r\n"
    data = insert_once(
        data,
        r28_load,
        r28_load
        + b"        { static int once = 0; if (!once++) fprintf(stderr,\r\n"
        + b"            \"[toc] func_0065699C r30=0x%08X object_slot=0x%08X r28=0x%08X\\n\",\r\n"
        + b"            (unsigned)ctx->gpr[30], (unsigned)(ctx->gpr[30] - 0x7FE0),\r\n"
        + b"            (unsigned)ctx->gpr[28]); }\r\n",
    )

    for callee in (
        b"func_0073F170",
        b"func_00744AF0",
        b"func_00654D98",
        b"func_007221E0",
        b"func_0073F230",
    ):
        call = b"        " + callee + b"(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
        trace = (
            call
            + b"        { static int once = 0; if (!once++) fprintf(stderr, \"[toc] after "
            + callee
            + b" r28=0x%08X\\n\", (unsigned)ctx->gpr[28]); }\r\n"
        )
        data = insert_in_function(data, b"func_0065699C", call, trace)

    TARGET.write_bytes(data)
    print(f"patched {TARGET}")


if __name__ == "__main__":
    main()
