# SPDX-License-Identifier: Apache-2.0
"""
ACS v1 -> v2 命名規則移行: Tier 1 (canonical types).

対象 (高信頼度・name collision リスク最小):
  - 数学 POD:   Vec2/3/4, Mat3/4, Quat, Aabb, Plane, Ray, Sphere, Frustum
  - foundation: ErrorCode, Color
  - container:  String, StringView, Array, UniquePtr, Rc, WeakRc, HashMap,
                HashSet, Span, Pool
  - util:       Result

ルール:
  - 単一 struct/class -> F prefix
  - template -> T prefix
  - word boundary regex で誤検出を最小化

スコープ:
  src/**, samples/**, tests/**, docs/**.md
  cmake-build-*/_deps/ は除外。

使い方:
  python scripts/rename_to_ue5_style_tier1.py [--dry-run]
"""
from __future__ import annotations
import argparse
import os
import re
import sys
from pathlib import Path

# (old_name, new_name) - Tier 1 only
TIER1 = [
    # math POD (struct)
    ("Vec2",       "FVec2"),
    ("Vec3",       "FVec3"),
    ("Vec4",       "FVec4"),
    ("Mat3",       "FMat3"),
    ("Mat4",       "FMat4"),
    ("Quat",       "FQuat"),
    ("Aabb",       "FAabb"),
    ("Plane",      "FPlane"),
    ("Ray",        "FRay"),
    ("Sphere",     "FSphere"),
    ("Frustum",    "FFrustum"),
    # foundation
    ("ErrorCode",  "FErrorCode"),
    # container (non-template)
    ("String",     "FString"),
    ("StringView", "FStringView"),
    # container (template)
    ("Array",      "TArray"),
    ("UniquePtr",  "TUniquePtr"),
    ("Rc",         "TRc"),
    ("WeakRc",     "TWeakRc"),
    ("HashMap",    "THashMap"),
    ("HashSet",    "THashSet"),
    ("Span",       "TSpan"),
    ("Pool",       "TPool"),
    # util
    ("Result",     "TResult"),
]

# ファイル拡張子: テキスト処理対象
TARGET_EXTS = {".h", ".hpp", ".inl", ".cpp", ".cxx", ".cc", ".c", ".md", ".cmake", ".txt"}

# 除外パス (3rd party / build artifacts)
EXCLUDE_DIRS = {
    "cmake-build-diligent-debug",
    "cmake-build-diligent-release",
    "cmake-build-debug",
    "cmake-build-release",
    "build",
    "_deps",
    "out",
    ".git",
    ".vs",
    ".idea",
    "node_modules",
}

# CMake 関連だがプロジェクトの一部
INCLUDE_DIRS_WHITELIST = {"src", "samples", "tests", "docs", "tools", "cmake", "scripts"}


def is_excluded(path: Path, repo_root: Path) -> bool:
    rel = path.relative_to(repo_root)
    parts = rel.parts
    for p in parts:
        if p in EXCLUDE_DIRS:
            return True
    return False


def collect_files(repo_root: Path) -> list[Path]:
    files: list[Path] = []
    for top in INCLUDE_DIRS_WHITELIST:
        top_path = repo_root / top
        if not top_path.exists():
            continue
        for dirpath, dirnames, filenames in os.walk(top_path):
            dirnames[:] = [d for d in dirnames if d not in EXCLUDE_DIRS]
            for fn in filenames:
                p = Path(dirpath) / fn
                if p.suffix.lower() in TARGET_EXTS:
                    if not is_excluded(p, repo_root):
                        files.append(p)
    # CMakeLists.txt が repo 直下にあるので追加
    for top_file in ["CMakeLists.txt", "README.md", "ONBOARDING.md"]:
        p = repo_root / top_file
        if p.exists():
            files.append(p)
    return files


def build_patterns(mapping: list[tuple[str, str]]) -> list[tuple[re.Pattern, str]]:
    """
    word-boundary regex を構築。
    method 呼び出し (.Foo() / ->Foo()) と member 名 (.x.Vec2 みたいなパターン)
    は意図的に rename しない (negative lookbehind で '.', '->' を除外)。
    """
    patterns: list[tuple[re.Pattern, str]] = []
    for old, new in mapping:
        # negative lookbehind で '.' (member 経由) と '>' (-> の > 経由) を除外。
        # ':' は除外しない (acs::Array<T> や Foo::Result のような namespace 修飾
        # は rename 対象に含めるため)。\w lookbehind は word boundary が既に
        # 担当しているので冗長だが安全側で残す。
        pat = re.compile(rf"(?<![\w.>])\b{re.escape(old)}\b(?![\w])")
        patterns.append((pat, new))
    return patterns


_INCLUDE_LINE = re.compile(r"^\s*#\s*include\b")


def transform_text(text: str, patterns: list[tuple[re.Pattern, str]]) -> tuple[str, int]:
    """
    行単位処理: `#include` 行はファイル PATH を含むので rename 対象外。
    それ以外の行は word-boundary regex で rename。
    """
    total = 0
    out_lines: list[str] = []
    for line in text.splitlines(keepends=True):
        if _INCLUDE_LINE.match(line):
            out_lines.append(line)
            continue
        new_line = line
        for pat, new in patterns:
            new_line, n = pat.subn(new, new_line)
            total += n
        out_lines.append(new_line)
    return "".join(out_lines), total


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true", help="変更を書き込まず件数だけ表示")
    ap.add_argument("--root", default=None, help="repo root (default: スクリプトの親の親)")
    args = ap.parse_args()

    script_dir = Path(__file__).resolve().parent
    repo_root = Path(args.root).resolve() if args.root else script_dir.parent

    print(f"[rename] repo root = {repo_root}")
    print(f"[rename] tier 1 mapping = {len(TIER1)} types")
    for old, new in TIER1:
        print(f"           {old:14s} -> {new}")

    files = collect_files(repo_root)
    print(f"[rename] scanning {len(files)} files")

    patterns = build_patterns(TIER1)

    total_files_changed = 0
    total_replacements = 0
    per_file: list[tuple[Path, int]] = []

    for f in files:
        try:
            text = f.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            # binary skipped
            continue
        new_text, n = transform_text(text, patterns)
        if n > 0:
            total_files_changed += 1
            total_replacements += n
            per_file.append((f, n))
            if not args.dry_run:
                f.write_text(new_text, encoding="utf-8")

    # 上位 30 ファイル表示
    per_file.sort(key=lambda x: -x[1])
    print(f"\n[rename] top 30 files by replacement count:")
    for p, n in per_file[:30]:
        rel = p.relative_to(repo_root)
        print(f"  {n:5d}  {rel}")

    print(f"\n[rename] summary: {total_replacements} replacements across {total_files_changed} files")
    if args.dry_run:
        print("[rename] (dry run, no files written)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
