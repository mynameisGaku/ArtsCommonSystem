# SPDX-License-Identifier: Apache-2.0
"""
.cmake / CMakeLists.txt 内で誤って renamed された identifier を一括 revert。

ACS の filename / CMake module name は v1 の名前のまま維持する方針。
よって以下を revert:

  1. file PATH: `F<X>.h` / `F<X>.cpp` / `T<X>.h` / `T<X>.cpp` を `<X>.h` / `<X>.cpp` に戻す
  2. CMake NAME: `NAME\s+F<X>` を `NAME    <X>` に
  3. CMake MODULE: `MODULE\s+F<X>` を `MODULE  <X>` に
  4. ACS:: target: `ACS::F<X>` を `ACS::<X>` に
  5. コメント中の "include path" 風記述 (in any file type): `<dir>/F<X>.h` を `<dir>/<X>.h`

スコープ: src/, samples/, tests/, tools/, cmake/, scripts/ + repo root の CMakeLists.txt。
"""
from __future__ import annotations
import os
import re
import sys
from pathlib import Path

# 対象拡張子
TARGET_EXTS = {".h", ".hpp", ".inl", ".cpp", ".cxx", ".cc", ".c", ".md", ".cmake", ".txt"}
EXCLUDE_DIRS = {"cmake-build-diligent-debug","cmake-build-diligent-release","cmake-build-debug",
                "cmake-build-release","build","_deps","out",".git",".vs",".idea"}
ROOTS = ["src","samples","tests","tools","cmake","scripts","docs"]


# pattern 1: file PATH 風 (F or T + UpperCase + 識別子) + .h/.cpp/.hpp/.inl
# 例: FApplication.h -> Application.h, TArray.h -> Array.h
RE_PATH_F = re.compile(r"\bF([A-Z][A-Za-z0-9_]*)(\.(?:h|cpp|hpp|inl|cxx|cc))\b")
RE_PATH_T = re.compile(r"\bT([A-Z][A-Za-z0-9_]*)(\.(?:h|cpp|hpp|inl|cxx|cc))\b")

# pattern 2: CMake NAME / MODULE keyword 後の F prefix
RE_CMAKE_NAME = re.compile(r"\b(NAME|MODULE)(\s+)F([A-Z][A-Za-z0-9_]*)\b")

# pattern 3: ACS::F<X> target reference
RE_ACS_TARGET = re.compile(r"\bACS::F([A-Z][A-Za-z0-9_]*)\b")

# pattern 4: include path 風 (dir/FX.h, dir/TX.h)
RE_INC_PATH_F = re.compile(r"(/|\")F([A-Z][A-Za-z0-9_]*)(\.(?:h|cpp|hpp|inl))\b")
RE_INC_PATH_T = re.compile(r"(/|\")T([A-Z][A-Za-z0-9_]*)(\.(?:h|cpp|hpp|inl))\b")


def revert_text(text: str) -> tuple[str, int]:
    total = 0
    out = text
    out, n = RE_PATH_F.subn(r"\1\2", out)
    total += n
    out, n = RE_PATH_T.subn(r"\1\2", out)
    total += n
    out, n = RE_CMAKE_NAME.subn(r"\1\2\3", out)
    total += n
    out, n = RE_ACS_TARGET.subn(r"ACS::\1", out)
    total += n
    return out, total


def collect(repo_root: Path) -> list[Path]:
    files: list[Path] = []
    for top in ROOTS:
        for dp, dns, fns in os.walk(repo_root / top):
            dns[:] = [d for d in dns if d not in EXCLUDE_DIRS]
            for fn in fns:
                p = Path(dp) / fn
                if p.suffix.lower() in TARGET_EXTS:
                    files.append(p)
    for top_file in ["CMakeLists.txt", "README.md"]:
        p = repo_root / top_file
        if p.exists():
            files.append(p)
    return files


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    files = collect(repo_root)
    print(f"[cmake-revert] scanning {len(files)} files")
    total_files = 0
    total_n = 0
    for f in files:
        try:
            text = f.read_text(encoding="utf-8")
        except Exception:
            continue
        new_text, n = revert_text(text)
        if n > 0:
            total_files += 1
            total_n += n
            f.write_text(new_text, encoding="utf-8")
            rel = f.relative_to(repo_root)
            print(f"  {n:4d}  {rel}")
    print(f"[cmake-revert] {total_n} reverts across {total_files} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
