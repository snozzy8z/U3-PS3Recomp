#!/usr/bin/env python3
"""Add a bounded trace to the FIOS condition-wait wrapper."""

from pathlib import Path


TARGET = Path(__file__).parent / "recompiled" / "ppu_recomp_b0016.cpp"
FUNCTION = b"func_00A5F204"
MARKER = b"[wait-wrapper]"


def patch_function(data: bytes, function: bytes, marker: bytes, probe: bytes) -> bytes:
    start = data.index(b"void " + function + b"(ppu_context* ctx) {")
    end = data.index(b"\r\n}\r\n", start)
    body = data[start:end]
    if marker in body:
        return data
    anchor = b"void " + function + b"(ppu_context* ctx) {\r\n"
    if body.count(anchor) != 1:
        raise RuntimeError(f"unexpected {function.decode()} shape")
    return data[:start] + body.replace(anchor, anchor + probe, 1) + data[end:]


def main() -> None:
    data = TARGET.read_bytes()
    wrapper_probe = (
        b"        if (ctx->thread_id == 7) {\r\n"
        + b"            static uint32_t dbg_wait_wrapper_count = 0;\r\n"
        + b"            uint32_t dbg_n = dbg_wait_wrapper_count++;\r\n"
        + b"            if (dbg_n < 32) {\r\n"
        + b"                uint32_t dbg_obj = (uint32_t)ctx->gpr[3];\r\n"
        + b"                fprintf(stderr, \"[wait-wrapper] #%u obj=0x%08X cond=%08X/%08X state=%08X/%08X\\n\",\r\n"
        + b"                        dbg_n, dbg_obj, vm_read32(dbg_obj + 0x10),\r\n"
        + b"                        vm_read32(dbg_obj + 0x14), vm_read32(dbg_obj),\r\n"
        + b"                        vm_read32(dbg_obj + 4));\r\n"
        + b"            }\r\n"
        + b"        }\r\n"
    )
    data = patch_function(data, FUNCTION, MARKER, wrapper_probe)

    for function, label in (
        (b"func_00A39968", b"rw-read-lock"),
        (b"func_00A39A30", b"rw-write-lock"),
        (b"func_00A39E18", b"rw-read-unlock"),
    ):
        marker = b"[" + label + b"]"
        probe = (
            b"        {\r\n"
            + b"            static uint32_t dbg_rw_count = 0;\r\n"
            + b"            uint32_t dbg_n = dbg_rw_count++;\r\n"
            + b"            if (dbg_n < 64) {\r\n"
            + b"                uint32_t dbg_obj = (uint32_t)ctx->gpr[3];\r\n"
            + b"                fprintf(stderr, \"["
            + label
            + b"] #%u tid=%llu obj=0x%08X counters=%u/%u/%u/%u\\n\",\r\n"
            + b"                        dbg_n, (unsigned long long)ctx->thread_id, dbg_obj,\r\n"
            + b"                        vm_read32(dbg_obj + 0x68), vm_read32(dbg_obj + 0x6C),\r\n"
            + b"                        vm_read32(dbg_obj + 0x70), vm_read32(dbg_obj + 0x74));\r\n"
            + b"            }\r\n"
            + b"        }\r\n"
        )
        data = patch_function(data, function, marker, probe)

    TARGET.write_bytes(data)
    print(f"patched {TARGET}")


if __name__ == "__main__":
    main()
