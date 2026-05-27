# SPDX-License-Identifier: Apache-2.0
"""
ローカル変数 / 関数引数の rename: **deferred (AST 必須)**。

WARNING: このスクリプトを単独で適用すると POD struct の public field
(`u32 message_len = 0;`、`bool use_console = true;` 等) や namespace-scope
global (`static const char* g_hook_user = nullptr;`) も誤って rename して
しまう。regex では「型キーワード + identifier」が local / field / global
のどれか区別できないため、安全な変換は AST refactoring (clang-tidy plugin)
が必須。

本 script は実装の試行版として残しているが、適用は禁止 (2026-05-28 試行で
hello_full_game build break、即 revert)。

将来対応案:
  - clang-tidy `acs_lint` plugin で AST-aware identifier rename を実装
    (StyleGuide v1 §12.6、acs_lint Phase 3 後続)
  - もしくは clang-tidy `readability-identifier-naming` の VariableCase /
    ParameterCase を有効化して fix-it 自動適用

対象パターン (将来の AST 実装の参考):
  - function-local variable declarations: PascalCase
  - function parameters: PascalCase
  - bool 変数は `b` 前置 (bIsActive)
  - 1-2 文字略形 (i/j/k/dt/dx 等) は許可

POD struct field は別ルール (StyleGuide §2.4: PascalCase prefix 無し)。
"""
from __future__ import annotations
import argparse
import os
import re
import sys
from pathlib import Path


# ACS の primitive type + 主要 ACS 型 (FXxx) を前置文字列として認識する。
TYPE_KEYWORDS = (
    # primitive aliases
    "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64",
    "f32", "f64", "usize", "isize", "byte", "uint", "uchar",
    # built-in
    "bool", "char", "void", "int", "long", "short", "float", "double",
    # keywords
    "auto", "const", "static", "constexpr", "consteval",
    # specific ACS types likely to appear in local declarations
    # (Tier 1/2/3 v2 で renamed なので F prefix)
    "FVec2", "FVec3", "FVec4", "FMat3", "FMat4", "FQuat",
    "FString", "FStringView", "FErrorCode",
    "FColor", "FRgba8", "FViewport", "FScissorRect",
)

# 1 つの大きな alt パターン
_TYPE_ALT = "|".join(re.escape(t) for t in TYPE_KEYWORDS)

# Match: <type keyword> [*&\s]* <snake_case_with_underscore>
RE_LOCAL = re.compile(
    rf"(\b(?:{_TYPE_ALT})\b[\s*&]*)"
    rf"([a-z][a-z0-9]*_[a-z0-9_]+)"
    rf"(?=[\s,;=()\[\]<])"
)


def snake_to_pascal(s: str) -> str:
    parts = s.split("_")
    return "".join(p[:1].upper() + p[1:] for p in parts if p)


TARGET_EXTS = {".h", ".hpp", ".inl", ".cpp", ".cxx", ".cc", ".c"}
EXCLUDE_DIRS = {"cmake-build-diligent-debug","cmake-build-diligent-release",
                "cmake-build-debug","cmake-build-release","build","_deps",
                "out",".git",".vs",".idea"}
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


def transform(text: str) -> tuple[str, int]:
    total = 0
    def repl(m: re.Match) -> str:
        nonlocal total
        prefix = m.group(1)
        snake = m.group(2)
        total += 1
        return prefix + snake_to_pascal(snake)
    new_text = RE_LOCAL.sub(repl, text)
    return new_text, total


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    files = collect(repo_root)
    print(f"[local-rename] scanning {len(files)} files")

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
    print(f"\n[local-rename] top 30 files:")
    for p, n in per_file[:30]:
        rel = p.relative_to(repo_root)
        print(f"  {n:5d}  {rel}")
    print(f"\n[local-rename] {total_n} replacements / {total_files} files")
    if args.dry_run:
        print("[local-rename] (dry run)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
