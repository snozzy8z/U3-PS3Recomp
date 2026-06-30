from pathlib import Path


SOURCE = Path(__file__).parent / "recompiled" / "ppu_recomp_b0000.cpp"
data = SOURCE.read_bytes()
newline = b"\r\n" if b"\r\n" in data[:4096] else b"\n"

include_anchor = b'#include <math.h>' + newline
include_line = b'#include <atomic>' + newline
function_anchor = b'void func_00038FBC(ppu_context* ctx) {' + newline
probe = newline.join(
    (
        b'        static std::atomic<unsigned long long> probe_calls{0};',
        b'        const unsigned long long probe_call = probe_calls.fetch_add(1, std::memory_order_relaxed) + 1;',
        b'        if (probe_call <= 16 || (probe_call % 250000) == 0) {',
        b'                fprintf(stderr,',
        b'                        "[probe38fbc-entry] call=%llu tid=%llu r3=%016llX r4=%08X r5=%08X sp=%08X lr=%08X\\n",',
        b'                        probe_call, (unsigned long long)ctx->thread_id,',
        b'                        (unsigned long long)ctx->gpr[3], (uint32_t)ctx->gpr[4],',
        b'                        (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[1], (uint32_t)ctx->lr);',
        b'        }',
        b'',
    )
)

if b'[probe38fbc-entry]' in data:
    raise SystemExit("probe already installed")
if data.count(include_anchor) != 1 or data.count(function_anchor) != 1:
    raise SystemExit("expected anchors not found exactly once")

data = data.replace(include_anchor, include_anchor + include_line, 1)
data = data.replace(function_anchor, function_anchor + probe, 1)
SOURCE.write_bytes(data)
print("installed func_00038FBC entry probe")
