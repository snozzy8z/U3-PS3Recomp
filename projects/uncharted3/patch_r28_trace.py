#!/usr/bin/env python3
"""Trace corruption of func_0073F230's callee-saved r28 stack slot."""

from pathlib import Path


TARGET = Path(__file__).parent / "recompiled" / "ppu_recomp_b0011.cpp"


def patch_body(
    data: bytes, function: bytes, needle: bytes, replacement: bytes
) -> bytes:
    start = data.index(b"void " + function + b"(ppu_context* ctx) {")
    end = data.index(b"\r\n}\r\n", start) + len(b"\r\n}\r\n")
    body = data[start:end]
    if replacement in body:
        return data
    count = body.count(needle)
    if count != 1:
        raise RuntimeError(f"expected one insertion point, found {count}")
    body = body.replace(needle, replacement, 1)
    return data[:start] + body + data[end:]


def check_after(callee: bytes) -> bytes:
    return (
        b"        if (ctx->gpr[1] != dbg_sp_73f230 || "
        b"vm_read64(dbg_sp_73f230 + 0x90) != dbg_r28_73f230) "
        b"fprintf(stderr, \"[r28] after "
        + callee
        + b" sp=0x%08X expected_sp=0x%08X saved=0x%08X expected=0x%08X\\n\", "
        b"(unsigned)ctx->gpr[1], (unsigned)dbg_sp_73f230, "
        b"(unsigned)vm_read64(dbg_sp_73f230 + 0x90), "
        b"(unsigned)dbg_r28_73f230);\r\n"
    )


def add_sp_trace(
    data: bytes,
    function: bytes,
    anchor: bytes,
    callees: tuple[bytes, ...],
) -> bytes:
    suffix = function.removeprefix(b"func_").lower()
    sp_var = b"dbg_sp_" + suffix
    data = patch_body(
        data,
        function,
        anchor,
        anchor
        + b"        uint64_t "
        + sp_var
        + b" = ctx->gpr[1];\r\n"
        + b"        fprintf(stderr, \"[sp] "
        + function
        + b" frame=0x%08X\\n\", (unsigned)"
        + sp_var
        + b");\r\n",
    )
    for callee in callees:
        call = b"        " + callee + b"(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
        probe = (
            call
            + b"        if (ctx->gpr[1] != "
            + sp_var
            + b") fprintf(stderr, \"[sp] "
            + function
            + b" after "
            + callee
            + b" sp=0x%08X expected=0x%08X\\n\", (unsigned)ctx->gpr[1], "
            b"(unsigned)"
            + sp_var
            + b");\r\n"
        )
        data = patch_body(data, function, call, probe)
    return data


def main() -> None:
    data = TARGET.read_bytes()

    anchor = b"        ctx->gpr[29] = ctx->gpr[4] | ctx->gpr[4];\r\n"
    data = patch_body(
        data,
        b"func_0073F230",
        anchor,
        anchor
        + b"        uint64_t dbg_sp_73f230 = ctx->gpr[1];\r\n"
        + b"        uint64_t dbg_r28_73f230 = vm_read64(dbg_sp_73f230 + 0x90);\r\n"
        + b"        fprintf(stderr, \"[r28] func_0073F230 sp=0x%08X r28=0x%08X saved=0x%08X\\n\", "
        + b"(unsigned)dbg_sp_73f230, (unsigned)ctx->gpr[28], "
        + b"(unsigned)dbg_r28_73f230);\r\n",
    )

    for callee in (
        b"func_00A0F910",
        b"ps3_indirect_call",
        b"func_00721600",
        b"func_00724DCC",
        b"func_0073EB9C",
        b"func_00725008",
    ):
        call = b"        " + callee + b"(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
        data = patch_body(data, b"func_0073F230", call, call + check_after(callee))

    data = add_sp_trace(
        data,
        b"func_0073EB9C",
        b"        ctx->gpr[29] = ctx->gpr[3] | ctx->gpr[3];\r\n",
        (
            b"func_00721600",
            b"func_00724DF8",
            b"func_00725008",
            b"func_0073E8BC",
        ),
    )
    data = add_sp_trace(
        data,
        b"func_0073E8BC",
        b"        ctx->gpr[29] = ctx->gpr[4] | ctx->gpr[4];\r\n",
        (b"func_00A97E14", b"func_0073E760"),
    )

    TARGET.write_bytes(data)
    print(f"patched {TARGET}")


if __name__ == "__main__":
    main()
