#!/usr/bin/env python3
"""Remove temporary generated-code probes used during the current bring-up."""

from pathlib import Path


ROOT = Path(__file__).parent / "recompiled"


def remove(path: Path, block: bytes, label: str) -> None:
    data = path.read_bytes()
    count = data.count(block)
    if count == 0:
        print(f"already clean: {label}")
        return
    if count != 1:
        raise RuntimeError(f"unexpected count {count} for {label} in {path}")
    path.write_bytes(data.replace(block, b"", 1))
    print(f"removed: {label}")


def main() -> None:
    mid7 = ROOT / "ppu_recomp_mid0007.cpp"
    base16 = ROOT / "ppu_recomp_b0016.cpp"
    mid13 = ROOT / "ppu_recomp_mid0013.cpp"
    base20 = ROOT / "ppu_recomp_b0020.cpp"

    remove(
        mid7,
        b"        if (ctx->thread_id == 7) {\r\n"
        b"            static uint32_t dbg_fios_tramp = 0;\r\n"
        b"            if (dbg_fios_tramp++ == 0) {\r\n"
        b"                fprintf(stderr, \"[fios-tramp] chain before A49568\\n\");\r\n"
        b"                ppu_tramp_dump_c();\r\n"
        b"            }\r\n"
        b"        }\r\n",
        "FIOS trampoline ring",
    )
    remove(
        mid7,
        b"        if (ctx->thread_id == 7) {\r\n"
        b"            static uint32_t dbg_fios_a49568 = 0;\r\n"
        b"            if (dbg_fios_a49568++ < 4)\r\n"
        b"                fprintf(stderr, \"[fios-A49568] r26=%08X r27=%08X r28=%08X r29=%08X r31=%08X cr=%08X sp=%08X\\n\",\r\n"
        b"                        (uint32_t)ctx->gpr[26], (uint32_t)ctx->gpr[27],\r\n"
        b"                        (uint32_t)ctx->gpr[28], (uint32_t)ctx->gpr[29],\r\n"
        b"                        (uint32_t)ctx->gpr[31], ctx->cr, (uint32_t)ctx->gpr[1]);\r\n"
        b"        }\r\n",
        "FIOS A49568 registers",
    )
    remove(mid7, b'extern "C" void ppu_tramp_dump_c(void);\r\n', "FIOS trampoline declaration")

    remove(
        base16,
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
        "FIOS A46DC0 object",
    )
    remove(
        base16,
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
        "FIOS rwlock memory dump",
    )
    remove(
        base16,
        b"        if (ctx->thread_id == 7)\r\n"
        b"            fprintf(stderr, \"[fios-A4D398] r21=%08X r22=%08X r25=%08X r27=%08X opd=%08X target=%08X r3=%08X r4=%08X\\n\",\r\n"
        b"                    (uint32_t)ctx->gpr[21], (uint32_t)ctx->gpr[22],\r\n"
        b"                    (uint32_t)ctx->gpr[25], (uint32_t)ctx->gpr[27],\r\n"
        b"                    (uint32_t)ctx->gpr[9], (uint32_t)ctx->gpr[0],\r\n"
        b"                    (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4]);\r\n",
        "FIOS A4D398 dispatch",
    )

    wrapper_probe = (
        b"        if (ctx->thread_id == 7) {\r\n"
        b"            static uint32_t dbg_wait_wrapper_count = 0;\r\n"
        b"            uint32_t dbg_n = dbg_wait_wrapper_count++;\r\n"
        b"            if (dbg_n < 32) {\r\n"
        b"                uint32_t dbg_obj = (uint32_t)ctx->gpr[3];\r\n"
        b"                fprintf(stderr, \"[wait-wrapper] #%u obj=0x%08X cond=%08X/%08X state=%08X/%08X\\n\",\r\n"
        b"                        dbg_n, dbg_obj, vm_read32(dbg_obj + 0x10),\r\n"
        b"                        vm_read32(dbg_obj + 0x14), vm_read32(dbg_obj),\r\n"
        b"                        vm_read32(dbg_obj + 4));\r\n"
        b"            }\r\n"
        b"        }\r\n"
    )
    remove(base16, wrapper_probe, "FIOS wait wrapper")
    for label in (b"rw-read-lock", b"rw-write-lock", b"rw-read-unlock"):
        probe = (
            b"        {\r\n"
            b"            static uint32_t dbg_rw_count = 0;\r\n"
            b"            uint32_t dbg_n = dbg_rw_count++;\r\n"
            b"            if (dbg_n < 64) {\r\n"
            b"                uint32_t dbg_obj = (uint32_t)ctx->gpr[3];\r\n"
            b"                fprintf(stderr, \"[" + label
            + b"] #%u tid=%llu obj=0x%08X counters=%u/%u/%u/%u\\n\",\r\n"
            b"                        dbg_n, (unsigned long long)ctx->thread_id, dbg_obj,\r\n"
            b"                        vm_read32(dbg_obj + 0x68), vm_read32(dbg_obj + 0x6C),\r\n"
            b"                        vm_read32(dbg_obj + 0x70), vm_read32(dbg_obj + 0x74));\r\n"
            b"            }\r\n"
            b"        }\r\n"
        )
        remove(base16, probe, label.decode())

    call = b"        func_00D859A8(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
    printf_block = (
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
    data = mid13.read_bytes()
    if printf_block in data:
        data = data.replace(printf_block, call, 1)
    for marker in (
        b"        fprintf(stderr, \"[printf-entry] D7A608 r3=%08X r30=%08X sp=%08X\\n\", (unsigned)ctx->gpr[3], (unsigned)ctx->gpr[30], (unsigned)ctx->gpr[1]);\r\n",
        b"        fprintf(stderr, \"[printf-step] after-rldicl r22=%08X\\n\", (unsigned)ctx->gpr[22]);\r\n",
        b"        fprintf(stderr, \"[printf-step] after-store r22=%08X\\n\", (unsigned)ctx->gpr[22]);\r\n",
    ):
        data = data.replace(marker, b"")
    for label in (b"00D7A63C", b"00D7A72C", b"00D7A754", b"00D7ABA8"):
        marker = (
            b"        fprintf(stderr, \"[printf-path] " + label
            + b" r22=%08X r3=%08X r20=%08X\\n\", (unsigned)ctx->gpr[22], (unsigned)ctx->gpr[3], (unsigned)ctx->gpr[20]);\r\n"
        )
        data = data.replace(marker, b"")
    data = data.replace(b'extern "C" void ppu_tramp_dump_c(void);\r\n', b"")
    mid13.write_bytes(data)

    data = base20.read_bytes()
    data = data.replace(
        b"        fprintf(stderr, \"[printf-entry] D79DB4 r3=%08X r4=%08X r5=%08X sp=%08X\\n\", (unsigned)ctx->gpr[3], (unsigned)ctx->gpr[4], (unsigned)ctx->gpr[5], (unsigned)ctx->gpr[1]);\r\n",
        b"",
    )
    base20.write_bytes(data)
    print("removed: formatter probes")


if __name__ == "__main__":
    main()
