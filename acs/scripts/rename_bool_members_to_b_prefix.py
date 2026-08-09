# SPDX-License-Identifier: Apache-2.0
"""
bool メンバ変数の `b` 前置: `m_IsActive` -> `m_bIsActive`.

手順 (per-file-pair scope で安全に処理):
  1. 各 .h / .cpp ペアごとに `bool\s+m_([A-Z]\w*)` 宣言を検出
     (`m_b` で既に始まっているものはスキップ)
  2. 検出した member 名を、その file pair 内で `m_X` -> `m_bX` に rename
     (member は private なので class 外から触れないという ACS 前提に依存)

POD struct の public bool field (`bool bEnabled = false;` 等) は `m_` 前置
が無いので本 script の対象外。POD field の b 前置は別の型監査で扱う。
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
ROOTS = ["src","tests","tools"]

# bool 宣言の検出: `bool\s+m_<UpperCase><word>` (= まだ b 前置されていない)
# 直後が = か ; か , (declaration / param) であること
RE_BOOL_DECL = re.compile(
    r"\bbool\s+m_([A-Z][A-Za-z0-9_]*)\s*(?=[=;,)\s\[])"
)

# false positive 除外: 型 alias `bool` (e.g., `using bool = ...`) や
# `static_assert(bool(...))` 等の関数形式は除外したい。
# 「次が ( のもの (関数呼び出し)」は除外する。


def collect_file_pairs(repo_root: Path) -> dict[str, list[Path]]:
    """
    file pair = .cpp と対応する .h をまとめて 1 単位とする。
    ヘッダのみ (.cpp 無し) も独立 unit として扱う。

    Returns: dict mapping "stem-key" -> [list of paths in that pair]
    """
    by_stem: dict[Path, list[Path]] = {}
    for top in ROOTS:
        for dp, dns, fns in os.walk(repo_root / top):
            dns[:] = [d for d in dns if d not in EXCLUDE_DIRS]
            for fn in fns:
                p = Path(dp) / fn
                if p.suffix.lower() in TARGET_EXTS:
                    key = p.with_suffix("")  # path/stem without extension
                    by_stem.setdefault(key, []).append(p)
    return by_stem


def find_bool_members(text: str) -> set[str]:
    """Extract `m_X` (without `b` prefix) of bool members from text."""
    result = set()
    for m in RE_BOOL_DECL.finditer(text):
        name = m.group(1)
        # already starting with 'b' followed by uppercase? then it's already b-prefixed
        # (e.g., `bool m_bIsActive` shouldn't match again)
        if len(name) >= 2 and name[0] == "b" and name[1].isupper():
            continue
        result.add(name)
    return result


def rename_in_text(text: str, names: set[str]) -> tuple[str, int]:
    total = 0
    out = text
    # 長い名前を先に処理 (substring 衝突防止)
    for name in sorted(names, key=len, reverse=True):
        pat = re.compile(rf"\bm_{re.escape(name)}\b")
        out, n = pat.subn(f"m_b{name}", out)
        total += n
    return out, total


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    print(f"[bool-prefix] repo root = {repo_root}")

    # Pass 1: collect ALL `m_X` names declared as bool repo-wide, and
    #         ALL `m_X` names declared as non-bool (any other type).
    # 衝突する名前は rename 対象外とする (= safe set のみ)
    by_stem = collect_file_pairs(repo_root)
    print(f"[bool-prefix] {len(by_stem)} file pairs scanned for declarations")

    bool_names: set[str] = set()
    nonbool_names: set[str] = set()

    # decl pattern for non-bool: anything else followed by `m_X`
    RE_ANY_DECL = re.compile(
        r"\b([A-Za-z_][\w:<>\s*&,]*?)\s+m_([A-Z][A-Za-z0-9_]*)\s*(?=[=;,)\s\[])"
    )

    all_files: list[Path] = []
    for files in by_stem.values():
        all_files.extend(files)

    for f in all_files:
        try:
            t = f.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for m in RE_ANY_DECL.finditer(t):
            type_str = m.group(1).strip()
            name = m.group(2)
            # already b-prefixed? skip
            if len(name) >= 2 and name[0] == "b" and name[1].isupper():
                continue
            # remove qualifiers from type
            type_clean = type_str.replace("const", "").replace("static", "").strip()
            if type_clean == "bool":
                bool_names.add(name)
            else:
                nonbool_names.add(name)

    # safe set: bool only, not also declared as non-bool
    safe_names = bool_names - nonbool_names
    conflict = bool_names & nonbool_names
    if conflict:
        print(f"[bool-prefix] {len(conflict)} conflict names (bool + non-bool) excluded:")
        for n in sorted(conflict):
            print(f"   - m_{n}")

    print(f"[bool-prefix] {len(safe_names)} safe bool names will be renamed")

    # Pass 2: rename `m_X` -> `m_bX` repo-wide for safe_names
    total_n = 0
    total_files = 0
    per_file: list[tuple[Path, int]] = []
    for f in all_files:
        try:
            t = f.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        new_t, n = rename_in_text(t, safe_names)
        if n > 0:
            total_n += n
            total_files += 1
            per_file.append((f, n))
            if not args.dry_run:
                f.write_text(new_t, encoding="utf-8")

    per_file.sort(key=lambda x: -x[1])
    print(f"\n[bool-prefix] top 30 files:")
    for f, n in per_file[:30]:
        rel = f.relative_to(repo_root)
        print(f"  {n:5d}  {rel}")
    print(f"\n[bool-prefix] {total_n} replacements / {total_files} files")
    if args.dry_run:
        print("[bool-prefix] (dry run)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
