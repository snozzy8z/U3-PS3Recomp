from pathlib import Path


PATH = Path(__file__).parent / "recompiled" / "ppu_recomp_b0011.cpp"
MARKER = b"probe767a18_calls"

data = PATH.read_bytes()
newline = b"\r\n" if b"\r\n" in data[:4096] else b"\n"

entry = b"void func_00767A18(ppu_context* ctx) {\r\n"
entry_probe = b"""void func_00767A18(ppu_context* ctx) {
        static uint32_t probe767a18_calls = 0;
        uint32_t probe767a18_id = probe767a18_calls++;
        uint32_t probe767a18_base = (uint32_t)ctx->gpr[3];
        uint32_t probe767a18_slot = (uint32_t)ctx->gpr[7];
        uint32_t probe767a18_count0 = vm_read32(probe767a18_base + 0x50);
        uint32_t probe767a18_count1 = vm_read32(probe767a18_base + 0x54);
        if (probe767a18_id < 16 || (probe767a18_id & 0x7F) == 0)
                fprintf(stderr, "[probe767a18] entry=%u base=%08X slot=%u counts=%u/%u arg4=%08X arg5=%08X arg6=%u\\n",
                        probe767a18_id, probe767a18_base, probe767a18_slot,
                        probe767a18_count0, probe767a18_count1,
                        (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6]);
"""

call = (
    b"        ctx->gpr[28] = (int64_t)(int32_t)(0);\r\n"
    b"        func_0075BDC0(ctx); DRAIN_TRAMPOLINE(ctx);\r\n"
)
call_probe = b"""        ctx->gpr[28] = (int64_t)(int32_t)(0);
        {
                uint32_t probe_allocator = (uint32_t)ctx->gpr[3];
                uint32_t probe_count = vm_read32(probe767a18_base + 0x50 + probe767a18_slot * 4);
                uint32_t probe_current = vm_read32(probe_allocator + 0x0);
                uint32_t probe_end = vm_read32(probe_allocator + 0x4);
                if (probe767a18_id < 16 || (probe767a18_id & 0x7F) == 0 ||
                    (probe_end >= probe_current && probe_end - probe_current < 0x10000))
                        fprintf(stderr, "[probe767a18] alloc entry=%u iter=%u/%u allocator=%08X current=%08X end=%08X base=%08X flags=%08X high=%08X\\n",
                                probe767a18_id, (uint32_t)ctx->gpr[27], probe_count,
                                probe_allocator, probe_current, probe_end,
                                vm_read32(probe_allocator + 0x8), vm_read32(probe_allocator + 0xC),
                                vm_read32(probe_allocator + 0x18));
        }
        func_0075BDC0(ctx); DRAIN_TRAMPOLINE(ctx);
"""

if newline == b"\n":
    entry = entry.replace(b"\r\n", newline)
    call = call.replace(b"\r\n", newline)
entry_probe = entry_probe.replace(b"\n", newline)
call_probe = call_probe.replace(b"\n", newline)

if MARKER in data:
    print("func_00767A18 allocator probe already present", PATH)
else:
    start = data.index(entry)
    end = data.index(b"void func_00768104", start)
    body = data[start:end]
    if call not in body:
        raise SystemExit("func_0075BDC0 call anchor not found")
    body = body.replace(entry, entry_probe, 1).replace(call, call_probe, 1)
    PATH.write_bytes(data[:start] + body + data[end:])
    print("patched", PATH)
