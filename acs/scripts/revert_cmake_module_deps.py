# SPDX-License-Identifier: Apache-2.0
"""
.cmake / CMakeLists.txt 内で renamed されてしまった module 名を revert。

ACS のモジュール名 (acs_module NAME ...) は filename 同様、v1 のまま維持する。
よって PUBLIC_DEPS / PRIVATE_DEPS リストや set() 文中の `FContainer` 等を
`Container` に戻す。
"""
from __future__ import annotations
import os
import re
import sys
from pathlib import Path

# v1 のモジュール名一覧 (src/ ディレクトリ構造から)
MODULE_NAMES = [
    "Foundation",
    "Memory",
    "Container",
    "Math",
    "Threading",
    "Platform",
    "Asset",
    "AssetPack",
    "Audio",
    "Easy",
    "Ecs",
    "Event",
    "GameFramework",
    "Imgui",
    "Mvvm",
    "Network",
    "Render",
    "App",
    "Ui",
    "Test",
]

TARGET_EXTS = {".cmake", ".txt"}
EXCLUDE_DIRS = {"cmake-build-diligent-debug","cmake-build-diligent-release","cmake-build-debug",
                "cmake-build-release","build","_deps","out",".git",".vs",".idea"}
ROOTS = ["src","samples","tests","tools","cmake","scripts"]


def collect(repo_root: Path) -> list[Path]:
    files: list[Path] = []
    for top in ROOTS:
        for dp, dns, fns in os.walk(repo_root / top):
            dns[:] = [d for d in dns if d not in EXCLUDE_DIRS]
            for fn in fns:
                p = Path(dp) / fn
                if p.suffix.lower() in TARGET_EXTS:
                    files.append(p)
    for top_file in ["CMakeLists.txt"]:
        p = repo_root / top_file
        if p.exists():
            files.append(p)
    return files


def revert_text(text: str) -> tuple[str, int]:
    total = 0
    out = text
    # 「F<ModuleName>」を 「<ModuleName>」に戻す (Module 名のみ対象、word boundary)
    # 「T<ModuleName>」も同じく (例: TAtomic は型なのでスキップ、TTest はそもそも無い)
    for name in MODULE_NAMES:
        # F<name> -> <name>
        pat = re.compile(rf"\bF{re.escape(name)}\b")
        out, n = pat.subn(name, out)
        total += n
    return out, total


def main():
    repo_root = Path(__file__).resolve().parent.parent
    files = collect(repo_root)
    print(f"[cmake-mod-revert] scanning {len(files)} files")
    total_n = 0
    total_f = 0
    for f in files:
        try:
            text = f.read_text(encoding="utf-8")
        except Exception:
            continue
        new_text, n = revert_text(text)
        if n > 0:
            total_n += n
            total_f += 1
            f.write_text(new_text, encoding="utf-8")
            rel = f.relative_to(repo_root)
            print(f"  {n:4d}  {rel}")
    print(f"[cmake-mod-revert] {total_n} reverts / {total_f} files")


if __name__ == "__main__":
    main()
