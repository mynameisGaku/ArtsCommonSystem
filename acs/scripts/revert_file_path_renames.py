# SPDX-License-Identifier: Apache-2.0
"""
Tier 1 rename script の副作用修正:
ファイル PATH 風の出現 (X.h / X.cpp / module-relative path) は元に戻す。

filename は変えていない (Array.h は Array.h のまま) ので、
PATH 参照だけ revert する。
"""
from __future__ import annotations
import os
import re
import sys
from pathlib import Path

REVERT_MAP = {
    "FVec2":       "Vec2",
    "FVec3":       "Vec3",
    "FVec4":       "Vec4",
    "FMat3":       "Mat3",
    "FMat4":       "Mat4",
    "FQuat":       "Quat",
    "FAabb":       "Aabb",
    "FPlane":      "Plane",
    "FRay":        "Ray",
    "FSphere":     "Sphere",
    "FFrustum":    "Frustum",
    "FErrorCode":  "ErrorCode",
    "FString":     "String",
    "FStringView": "StringView",
    "TArray":      "Array",
    "TUniquePtr":  "UniquePtr",
    "TRc":         "Rc",
    "TWeakRc":     "WeakRc",
    "THashMap":    "HashMap",
    "THashSet":    "HashSet",
    "TSpan":       "Span",
    "TPool":       "Pool",
    "TResult":     "Result",
}

# .h / .cpp / .hpp / .inl 拡張子の前にある型名のみ revert
# また、include / source / header keyword 周辺の path-like context を捕捉
TARGET_EXTS = {".h", ".hpp", ".inl", ".cpp", ".cxx", ".cc", ".c", ".md", ".cmake", ".txt"}
EXCLUDE_DIRS = {"cmake-build-diligent-debug", "cmake-build-diligent-release", "cmake-build-debug",
                "cmake-build-release", "build", "_deps", ".git", ".vs", ".idea"}
ROOTS = ["src", "samples", "tests", "tools", "docs", "cmake", "scripts"]


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


def revert_text(text: str) -> tuple[str, int]:
    total = 0
    out = text
    # PATH 風: PrefixedName.h / .cpp (file path のドット拡張子直前のみ)
    for new, old in REVERT_MAP.items():
        # \b{new}\.(h|cpp|hpp|inl|cxx|cc) を {old}.<ext> に戻す
        pat = re.compile(rf"\b{re.escape(new)}(\.(h|cpp|hpp|inl|cxx|cc))\b")
        out, n = pat.subn(rf"{old}\1", out)
        total += n
    # CMake/Module.cmake のような SOURCES / HEADERS リスト内 (行頭 indent + 名前 + .ext 無し
    # な記述もあるので別パターン)
    return out, total


def main():
    repo_root = Path(__file__).resolve().parent.parent
    files = collect(repo_root)
    print(f"[revert-paths] scanning {len(files)} files")
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
            print(f"  {n:3d}  {rel}")
    print(f"[revert-paths] {total_n} reverts across {total_files} files")


if __name__ == "__main__":
    main()
