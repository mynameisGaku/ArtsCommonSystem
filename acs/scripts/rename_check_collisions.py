# SPDX-License-Identifier: Apache-2.0
"""
rename 前の collision check.

各候補名について、word-boundary regex で何件マッチするか、
そのうち「危なそう」(string literal / コメント中 / .Foo() 呼出) が何件
含まれているかを試算する。
"""
from __future__ import annotations
import os
import re
import sys
from pathlib import Path

CANDIDATES = [
    "Vec2", "Vec3", "Vec4", "Mat3", "Mat4", "Quat",
    "Aabb", "Plane", "Ray", "Sphere", "Frustum",
    "ErrorCode", "String", "StringView",
    "Array", "UniquePtr", "Rc", "WeakRc", "HashMap", "HashSet",
    "Span", "Pool", "Result",
]

TARGET_EXTS = {".h", ".hpp", ".inl", ".cpp", ".cxx", ".cc", ".c"}
EXCLUDE = {"cmake-build-diligent-debug", "cmake-build-diligent-release", "cmake-build-debug", "cmake-build-release", "build", "_deps", ".git"}
ROOTS = ["src", "tests"]


def collect(repo_root: Path) -> list[Path]:
    files = []
    for top in ROOTS:
        for dp, dns, fns in os.walk(repo_root / top):
            dns[:] = [d for d in dns if d not in EXCLUDE]
            for fn in fns:
                p = Path(dp) / fn
                if p.suffix.lower() in TARGET_EXTS:
                    files.append(p)
    return files


def main():
    repo_root = Path(__file__).resolve().parent.parent
    files = collect(repo_root)

    for name in CANDIDATES:
        pat_word = re.compile(rf"(?<![\w.>:])\b{re.escape(name)}\b(?![\w])")
        # 「.Foo」「->Foo」「"...Foo..."」を別カウント
        pat_member = re.compile(rf"[.>]{re.escape(name)}\b")
        pat_in_string = re.compile(rf'"[^"\n]*\b{re.escape(name)}\b[^"\n]*"')

        word_count = 0
        member_count = 0
        string_count = 0

        for f in files:
            try:
                text = f.read_text(encoding="utf-8")
            except Exception:
                continue
            word_count += len(pat_word.findall(text))
            member_count += len(pat_member.findall(text))
            string_count += len(pat_in_string.findall(text))

        print(f"{name:14s}  word={word_count:5d}  member-access={member_count:4d}  in-string={string_count:3d}")


if __name__ == "__main__":
    main()
