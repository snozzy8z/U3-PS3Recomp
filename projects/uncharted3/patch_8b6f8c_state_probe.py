from pathlib import Path


SOURCE = Path(__file__).parent / "recompiled" / "ppu_recomp_b0013.cpp"
data = SOURCE.read_bytes()
newline = b"\r\n" if b"\r\n" in data[:4096] else b"\n"
anchor = b"void func_008B6F8C(ppu_context* ctx) {" + newline
probe = newline.join(
    (
        b"        static unsigned long long state_probe_calls = 0;",
        b"        const unsigned long long state_probe_call = ++state_probe_calls;",
        b"        if (state_probe_call <= 8 || (state_probe_call % 250) == 0) {",
        b"                const uint32_t state_probe_obj = (uint32_t)ctx->gpr[3];",
        b"                fprintf(stderr,",
        b'                        "[probe8b6f8c] call=%llu tid=%llu obj=%08X state=%u p0=%08X p1=%08X\\n",',
        b"                        state_probe_call, (unsigned long long)ctx->thread_id, state_probe_obj,",
        b"                        vm_read32(state_probe_obj + 0x20), vm_read32(state_probe_obj),",
        b"                        vm_read32(state_probe_obj + 4));",
        b"        }",
        b"",
    )
)

if b"[probe8b6f8c]" in data:
    raise SystemExit("probe already installed")
if data.count(anchor) != 1:
    raise SystemExit("expected function anchor not found exactly once")

SOURCE.write_bytes(data.replace(anchor, anchor + probe, 1))
print("installed func_008B6F8C state probe")
