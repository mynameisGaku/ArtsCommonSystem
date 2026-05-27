# SPDX-License-Identifier: Apache-2.0
"""
メンバ変数 / private helper 関数の rename:
  `_snake_case` -> `m_PascalCase` (ハイブリッド、Microsoft/Naughty Dog 流)。

UE5 純正 (prefix 無し) は accessor method 名 (`T Member() const`) と衝突する
ため、`m_` 前置で衝突回避する choice を確定 (2026-05-28)。

検出パターン:
  `\\b_[a-z][a-z0-9_]*\\b`

除外:
  - `__FILE__` / `__LINE__` / `__cdecl` 等 (double underscore + 任意)
  - `_Out_` / `_In_` / `_M_X64` (uppercase 直後始まり)
  - C ライブラリ `_open` `_close` 等 (rare、ACS では使用していない想定)

bool 変数の `b` 前置 (`_is_active` -> `m_bIsActive`) は型解析必要なので別 phase。
ここでは `_is_active` -> `m_IsActive` のように単純 m_PascalCase。
"""
from __future__ import annotations
import argparse
import os
import re
import sys
from pathlib import Path


TARGET_EXTS = {".h", ".hpp", ".inl", ".cpp", ".cxx", ".cc", ".c"}
EXCLUDE_DIRS = {"cmake-build-diligent-debug","cmake-build-diligent-release",
                "cmake-build-debug","cmake-build-release","build","_deps",
                "out",".git",".vs",".idea"}
ROOTS = ["src","samples","tests","tools"]

# preprocessor / reserved (skip these specific names even if they match pattern)
RESERVED_NAMES = {
    "_t", "_s",  # iterator-like, probably rare but skip just in case
}

# Intrinsic prefixes to skip (SIMD / Win32 / compiler intrinsics)
# Compiler / SIMD intrinsics start with these (after the leading underscore).
INTRINSIC_PREFIXES = (
    # SIMD
    "mm_", "mm256_", "mm512_", "m_",
    # Win32 / compiler
    "bittest", "bittest64", "bittestand",
    "interlockedand", "interlockedor", "interlockedxor",
    "interlockedincrement", "interlockeddecrement",
    "interlockedexchange", "interlockedcompareexchange",
    "byteswap_",
    "rotl", "rotr",
    "ulltoa", "ltoa", "itoa", "ultoa",
    "atoi64", "atoi_l", "atof_l",
    # CPU
    "xgetbv", "xsetbv", "cpuid", "cpuidex", "rdtsc",
    "popcnt", "lzcnt", "tzcnt",
    # memory
    "aligned_malloc", "aligned_free", "aligned_realloc",
    "alloca", "malloca", "freea",
    # CRT (rare in ACS but safety)
    "snprintf", "vsnprintf", "snwprintf",
    "open", "close", "read", "write", "lseek",
    "stat", "fstat", "wstat",
)

# pattern: `\b_[a-z][a-z0-9_]*\b`
# negative lookbehind for `_` (= avoid matching second _ of __foo)
RE_MEMBER = re.compile(r"(?<![_A-Za-z0-9])_([a-z][a-z0-9_]*)\b")


def snake_to_pascal(s: str) -> str:
    parts = s.split("_")
    return "".join(p[:1].upper() + p[1:] for p in parts if p)


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


def transform(text: str) -> tuple[str, int]:
    total = 0
    def repl(m: re.Match) -> str:
        nonlocal total
        snake = m.group(1)
        if snake in RESERVED_NAMES:
            return m.group(0)
        # SSE / SIMD / Win32 intrinsics like `_mm_mfence`, `_m_empty` -- skip
        for pre in INTRINSIC_PREFIXES:
            if snake.startswith(pre):
                return m.group(0)
        # exclude SAL-like `_In_`, `_Out_` (mixed case followed by `_`)
        # our regex already requires lowercase first, so SAL with uppercase is excluded
        total += 1
        return "m_" + snake_to_pascal(snake)
    new_text = RE_MEMBER.sub(repl, text)
    return new_text, total


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    print(f"[member-rename] repo root = {repo_root}")

    files = collect(repo_root)
    print(f"[member-rename] scanning {len(files)} files")

    total_files = 0
    total_n = 0
    per_file: list[tuple[Path, int]] = []
    for f in files:
        try:
            text = f.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        new_text, n = transform(text)
        if n > 0:
            total_files += 1
            total_n += n
            per_file.append((f, n))
            if not args.dry_run:
                f.write_text(new_text, encoding="utf-8")

    per_file.sort(key=lambda x: -x[1])
    print(f"\n[member-rename] top 30 files:")
    for p, n in per_file[:30]:
        rel = p.relative_to(repo_root)
        print(f"  {n:5d}  {rel}")
    print(f"\n[member-rename] {total_n} replacements / {total_files} files")
    if args.dry_run:
        print("[member-rename] (dry run)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
