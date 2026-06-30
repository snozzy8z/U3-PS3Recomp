from pathlib import Path
import re


root = Path(__file__).parent / "recompiled"
signed = re.compile(
    rb"\(int64_t\)\(int32_t\)\(\(int32_t\)ctx->gpr\[(\d+)\] / "
    rb"\(int32_t\)ctx->gpr\[(\d+)\]\)"
)
unsigned = re.compile(
    rb"\(int64_t\)\(int32_t\)\(\(uint32_t\)ctx->gpr\[(\d+)\] / "
    rb"\(uint32_t\)ctx->gpr\[(\d+)\]\)"
)

changed_files = 0
changed_sites = 0
for path in sorted(root.glob("*.cpp")):
    data = path.read_bytes()
    data, ns = signed.subn(
        rb"(int64_t)(int32_t)ppc_divw((int32_t)ctx->gpr[\1], (int32_t)ctx->gpr[\2])",
        data,
    )
    data, nu = unsigned.subn(
        rb"(int64_t)(int32_t)ppc_divwu((uint32_t)ctx->gpr[\1], (uint32_t)ctx->gpr[\2])",
        data,
    )
    if ns or nu:
        path.write_bytes(data)
        changed_files += 1
        changed_sites += ns + nu

print(f"patched {changed_sites} division sites across {changed_files} files")
