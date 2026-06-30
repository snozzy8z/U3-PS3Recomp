#!/usr/bin/env python3
"""Add bounded probes around the FIOS worker's invalid rwlock path."""

from pathlib import Path


ROOT = Path(__file__).parent / "recompiled"


def patch_function(path: Path, function: bytes, marker: bytes, probe: bytes) -> None:
    data = path.read_bytes()
    start = data.index(b"void " + function + b"(ppu_context* ctx) {")
    end = data.index(b"\r\n}\r\n", start)
    body = data[start:end]
    if marker in body:
        return
    anchor = b"void " + function + b"(ppu_context* ctx) {\r\n"
    if body.count(anchor) != 1:
        raise RuntimeError(f"unexpected {function.decode()} shape in {path}")
    data = data[:start] + body.replace(anchor, anchor + probe, 1) + data[end:]
    path.write_bytes(data)


def main() -> None:
    mid = ROOT / "ppu_recomp_mid0007.cpp"
    base = ROOT / "ppu_recomp_b0016.cpp"

    mid_data = mid.read_bytes()
    tramp_decl = b'extern "C" void ppu_tramp_dump_c(void);\r\n'
    if tramp_decl not in mid_data:
        anchor = b'extern "C" void ppu_tramp_rec(void*);\r\n'
        if mid_data.count(anchor) != 1:
            raise RuntimeError("unexpected trampoline declaration shape")
        mid.write_bytes(mid_data.replace(anchor, anchor + tramp_decl, 1))

    patch_function(
        mid,
        b"func_00A49568",
        b"[fios-tramp]",
        b"        if (ctx->thread_id == 7) {\r\n"
        b"            static uint32_t dbg_fios_tramp = 0;\r\n"
        b"            if (dbg_fios_tramp++ == 0) {\r\n"
        b"                fprintf(stderr, \"[fios-tramp] chain before A49568\\n\");\r\n"
        b"                ppu_tramp_dump_c();\r\n"
        b"            }\r\n"
        b"        }\r\n",
    )

    patch_function(
        mid,
        b"func_00A49568",
        b"[fios-A49568]",
        b"        if (ctx->thread_id == 7) {\r\n"
        b"            static uint32_t dbg_fios_a49568 = 0;\r\n"
        b"            if (dbg_fios_a49568++ < 4)\r\n"
        b"                fprintf(stderr, \"[fios-A49568] r26=%08X r27=%08X r28=%08X r29=%08X r31=%08X cr=%08X sp=%08X\\n\",\r\n"
        b"                        (uint32_t)ctx->gpr[26], (uint32_t)ctx->gpr[27],\r\n"
        b"                        (uint32_t)ctx->gpr[28], (uint32_t)ctx->gpr[29],\r\n"
        b"                        (uint32_t)ctx->gpr[31], ctx->cr, (uint32_t)ctx->gpr[1]);\r\n"
        b"        }\r\n",
    )

    patch_function(
        base,
        b"func_00A46DC0",
        b"[fios-A46DC0]",
        b"        if (ctx->thread_id == 7) {\r\n"
        b"            static uint32_t dbg_fios_a46dc0 = 0;\r\n"
        b"            if (dbg_fios_a46dc0++ < 4) {\r\n"
        b"                uint32_t dbg_parent = (uint32_t)ctx->gpr[3];\r\n"
        b"                fprintf(stderr, \"[fios-A46DC0] parent=%08X arg4=%08X lock=%08X magic=%08X fields=%08X/%08X/%08X/%08X\\n\",\r\n"
        b"                        dbg_parent, (uint32_t)ctx->gpr[4], dbg_parent + 0x98,\r\n"
        b"                        vm_read32(dbg_parent + 0x9C), vm_read32(dbg_parent),\r\n"
        b"                        vm_read32(dbg_parent + 4), vm_read32(dbg_parent + 8),\r\n"
        b"                        vm_read32(dbg_parent + 0xC));\r\n"
        b"            }\r\n"
        b"        }\r\n",
    )

    base_data = base.read_bytes()
    if b"[fios-A4D398]" not in base_data:
        anchor = (
            b"loc_00A4D398:\r\n"
            b"        ctx->gpr[9] = vm_read32(ctx->gpr[9] + 0x14);\r\n"
            b"        ctx->gpr[0] = vm_read32(ctx->gpr[9] + 0x0);\r\n"
        )
        probe = (
            b"        if (ctx->thread_id == 7)\r\n"
            b"            fprintf(stderr, \"[fios-A4D398] r21=%08X r22=%08X r25=%08X r27=%08X opd=%08X target=%08X r3=%08X r4=%08X\\n\",\r\n"
            b"                    (uint32_t)ctx->gpr[21], (uint32_t)ctx->gpr[22],\r\n"
            b"                    (uint32_t)ctx->gpr[25], (uint32_t)ctx->gpr[27],\r\n"
            b"                    (uint32_t)ctx->gpr[9], (uint32_t)ctx->gpr[0],\r\n"
            b"                    (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4]);\r\n"
        )
        if base_data.count(anchor) != 1:
            raise RuntimeError("unexpected A4D398 shape")
        base.write_bytes(base_data.replace(anchor, anchor + probe, 1))

    patch_function(
        base,
        b"func_00A39A30",
        b"[fios-rw-dump]",
        b"        if (ctx->thread_id == 7) {\r\n"
        b"            static uint32_t dbg_fios_rw_dump = 0;\r\n"
        b"            if (dbg_fios_rw_dump++ == 0) {\r\n"
        b"                uint32_t dbg_base = (uint32_t)ctx->gpr[3] - 0x98;\r\n"
        b"                fprintf(stderr, \"[fios-rw-dump] parent=%08X\\n\", dbg_base);\r\n"
        b"                for (uint32_t dbg_o = 0; dbg_o < 0x140; dbg_o += 0x20) {\r\n"
        b"                    fprintf(stderr, \"  +%03X:\", dbg_o);\r\n"
        b"                    for (uint32_t dbg_i = 0; dbg_i < 0x20; dbg_i += 4)\r\n"
        b"                        fprintf(stderr, \" %08X\", vm_read32(dbg_base + dbg_o + dbg_i));\r\n"
        b"                    fprintf(stderr, \"\\n\");\r\n"
        b"                }\r\n"
        b"            }\r\n"
        b"        }\r\n",
    )

    print("patched bounded FIOS path probes")


if __name__ == "__main__":
    main()
