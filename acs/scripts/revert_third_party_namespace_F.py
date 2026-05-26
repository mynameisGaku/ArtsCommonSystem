# SPDX-License-Identifier: Apache-2.0
"""
Tier 2 の副作用修正:
`Diligent::F<X>` や `ImGui::F<X>` のような 3rd-party 名前空間に F prefix が
誤って付いた箇所を revert する。

ACS の F<X> は維持、`acs::F<X>` も維持、bare `F<X>` も維持。
3rd-party 名前空間 (Diligent, ImGui, std, D3D12, DXGI, dxc, etc.) 内の
F<X> のみ削除。
"""
from __future__ import annotations
import os
import re
import sys
from pathlib import Path

# 3rd-party namespaces (ACS 外部、F prefix を強制的に外す)
THIRD_PARTY_NAMESPACES = [
    "Diligent",
    "ImGui",
    "DiligentTools",
    "DiligentEngine",
    "Microsoft",
    "Microsoft::WRL",
    "WRL",
    "DirectX",
    "dxc",
    "D3D12",
    "DXGI",
    "winrt",
]

TARGET_EXTS = {".h", ".hpp", ".inl", ".cpp", ".cxx", ".cc", ".c"}
EXCLUDE_DIRS = {"cmake-build-diligent-debug","cmake-build-diligent-release","cmake-build-debug",
                "cmake-build-release","build","_deps","out",".git",".vs",".idea"}
ROOTS = ["src","samples","tests","tools"]


def collect(repo_root: Path) -> list[Path]:
    files: list[Path] = []
    for top in ROOTS:
        for dp, dns, fns in os.walk(repo_root / top):
            dns[:] = [d for d in dns if d not in EXCLUDE_DIRS]
            for fn in fns:
                p = Path(dp) / fn
                if p.suffix.lower() in TARGET_EXTS:
                    files.append(p)
    return files


def revert_text(text: str) -> tuple[str, int]:
    total = 0
    out = text
    for ns in THIRD_PARTY_NAMESPACES:
        ns_esc = re.escape(ns)
        # `Diligent::FFoo` -> `Diligent::Foo`
        pat = re.compile(rf"(\b{ns_esc}::)F([A-Z][A-Za-z0-9_]*)")
        out, n = pat.subn(r"\1\2", out)
        total += n
        # `Diligent::FFoo::Member` も同じパターンでカバー (上記の subn が処理)
        # T prefix (TArray 等) は ACS 専用なので削除しない
    return out, total


def main():
    repo_root = Path(__file__).resolve().parent.parent
    files = collect(repo_root)
    print(f"[3rd-party-F-revert] scanning {len(files)} files")
    total_n = 0
    total_f = 0
    per_file: list[tuple[Path, int]] = []
    for f in files:
        try:
            text = f.read_text(encoding="utf-8")
        except Exception:
            continue
        new_text, n = revert_text(text)
        if n > 0:
            total_n += n
            total_f += 1
            per_file.append((f, n))
            f.write_text(new_text, encoding="utf-8")
    per_file.sort(key=lambda x: -x[1])
    for p, n in per_file[:30]:
        rel = p.relative_to(repo_root)
        print(f"  {n:4d}  {rel}")
    print(f"[3rd-party-F-revert] {total_n} reverts / {total_f} files")


if __name__ == "__main__":
    main()
