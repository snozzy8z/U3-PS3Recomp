from pathlib import Path


SOURCE = Path(__file__).parent / "recompiled" / "ppu_recomp_b0014.cpp"
data = SOURCE.read_bytes()
newline = b"\r\n" if b"\r\n" in data[:4096] else b"\n"
anchor = b"void func_0093C370(ppu_context* ctx) {" + newline
probe = newline.join(
    (
        b"        {",
        b"                const uint32_t path_probe = (uint32_t)ctx->gpr[3];",
        b"                fprintf(stderr,",
        b'                        "[probe93c370] path=%08X bytes=%02X %02X %02X %02X %02X %02X %02X %02X\\n",',
        b"                        path_probe, vm_read8(path_probe + 0), vm_read8(path_probe + 1),",
        b"                        vm_read8(path_probe + 2), vm_read8(path_probe + 3),",
        b"                        vm_read8(path_probe + 4), vm_read8(path_probe + 5),",
        b"                        vm_read8(path_probe + 6), vm_read8(path_probe + 7));",
        b"        }",
        b"",
    )
)

if b"[probe93c370]" in data:
    raise SystemExit("probe already installed")
if data.count(anchor) != 1:
    raise SystemExit("expected function anchor not found exactly once")

SOURCE.write_bytes(data.replace(anchor, anchor + probe, 1))
print("installed func_0093C370 path probe")
