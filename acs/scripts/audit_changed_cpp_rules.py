#!/usr/bin/env python3
"""基準コミットから追加したC++行へ、ユーザー共通規約の機械判定部分を適用する。"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


CPP_SUFFIXES = {".h", ".hpp", ".inl", ".cpp", ".cc", ".cxx"}
JAPANESE_PATTERN = re.compile(r"[\u3040-\u30ff\u3400-\u9fff]")
HUNK_PATTERN = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@")
GENERATED_ROOTS = {"dist"}


@dataclass(frozen=True)
class AddedLine:
    """差分で追加された一つのC++行。"""

    path: Path
    line: int
    text: str


@dataclass(frozen=True)
class Violation:
    """規約違反の場所と理由。"""

    path: Path
    line: int
    message: str


def parse_added_lines(diff_text: str) -> tuple[list[AddedLine], set[Path]]:
    """unified diffから追加行と変更対象headerを取り出す。"""
    added: list[AddedLine] = []
    changed_headers: set[Path] = set()
    current_path: Path | None = None
    current_line = 0
    for raw_line in diff_text.splitlines():
        if raw_line.startswith("+++ b/"):
            current_path = Path(raw_line[6:])
            if current_path.suffix.lower() in {".h", ".hpp", ".inl"}:
                changed_headers.add(current_path)
            continue
        hunk = HUNK_PATTERN.match(raw_line)
        if hunk:
            current_line = int(hunk.group(1))
            continue
        if current_path is None:
            continue
        if raw_line.startswith("+") and not raw_line.startswith("+++"):
            added.append(AddedLine(current_path, current_line, raw_line[1:]))
            current_line += 1
        elif raw_line.startswith(" "):
            current_line += 1
    return added, changed_headers


def code_without_literals(line: str) -> str:
    """文字列・文字リテラルと行コメントを空白へ置き換える。"""
    output: list[str] = []
    quote = ""
    escaped = False
    index = 0
    while index < len(line):
        character = line[index]
        if quote:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = ""
            output.append(" ")
            index += 1
            continue
        if character in {"\"", "'"}:
            quote = character
            output.append(" ")
            index += 1
            continue
        if character == "/" and index + 1 < len(line) and line[index + 1] == "/":
            break
        output.append(character)
        index += 1
    return "".join(output)


def comment_text(line: str) -> str | None:
    """コメント行なら装飾を除いた本文を返す。"""
    stripped = line.strip()
    if stripped.startswith("//"):
        return stripped[2:].strip()
    if stripped.startswith("/*"):
        return stripped.lstrip("/*").rstrip("*/").strip()
    if stripped == "*" or stripped.startswith("* ") or stripped.startswith("*@") or stripped.startswith("*/"):
        return stripped[1:].rstrip("*/").strip()
    return None


def inspect_added_lines(lines: Iterable[AddedLine]) -> list[Violation]:
    """追加行だけで確実に判定できる一行形式と日本語コメントを検査する。"""
    violations: list[Violation] = []
    for added in lines:
        code = code_without_literals(added.text)
        if code.count("(") != code.count(")"):
            violations.append(Violation(added.path, added.line, "括弧内部を一行に収めてください"))
        has_braced_initializer = re.search(r"(?<![=!<>])=(?!=)\s*\{", code) is not None
        if has_braced_initializer and code.count("{") != code.count("}"):
            violations.append(Violation(added.path, added.line, "初期化子内部を一行に収めてください"))

        comment = comment_text(added.text)
        if comment is None or not comment:
            continue
        if comment.startswith("SPDX-License-Identifier:"):
            continue
        if comment.startswith("@") and JAPANESE_PATTERN.search(comment) is None:
            violations.append(Violation(added.path, added.line, "Doxygen説明を日本語で補ってください"))
        elif re.search(r"[A-Za-z]", comment) and JAPANESE_PATTERN.search(comment) is None:
            violations.append(Violation(added.path, added.line, "コメントを日本語で記述してください"))
    return violations


def inspect_header_preambles(root: Path, headers: Iterable[Path]) -> list[Violation]:
    """変更headerの冒頭がSPDXとinclude guardだけかを検査する。"""
    violations: list[Violation] = []
    for relative_path in sorted(headers):
        path = root / relative_path
        if not path.exists():
            continue
        lines = path.read_text(encoding="utf-8-sig").splitlines()
        nonempty = [(index + 1, text.strip()) for index, text in enumerate(lines) if text.strip()]
        if not nonempty:
            violations.append(Violation(relative_path, 1, "空のheaderです"))
            continue
        if not nonempty[0][1].startswith("// SPDX-License-Identifier:"):
            violations.append(Violation(relative_path, nonempty[0][0], "header先頭にSPDX識別子が必要です"))
            continue
        if len(nonempty) < 2 or not (nonempty[1][1] == "#pragma once" or nonempty[1][1].startswith("#ifndef ")):
            line = nonempty[1][0] if len(nonempty) >= 2 else nonempty[0][0]
            violations.append(Violation(relative_path, line, "SPDX直後にはinclude guardだけを置いてください"))
    return violations


def is_generated_path(path: Path) -> bool:
    """生成物として専用監査へ委ねるパスならtrueを返す。"""
    return bool(path.parts) and path.parts[0].lower() in GENERATED_ROOTS


def self_test() -> int:
    """成功例と各違反例を区別できることを確認する。"""
    sample = "\n".join(
        [
            "+++ b/src/FExample.h",
            "@@ -0,0 +1,5 @@",
            "+// SPDX-License-Identifier: Apache-2.0",
            "+#pragma once",
            "+/** 値を返す。 */",
            "+inline int Value(int input) noexcept { return input; }",
            "+const int kValue = 1;",
            "+++ b/src/Bad.cpp",
            "@@ -0,0 +1,3 @@",
            "+// English only",
            "+Call(",
            "+const int values[] = {",
        ]
    )
    lines, headers = parse_added_lines(sample)
    violations = inspect_added_lines(lines)
    generated = is_generated_path(Path("dist/acs.h")) and not is_generated_path(Path("src/FExample.h"))
    return 0 if len(lines) == 8 and headers == {Path("src/FExample.h")} and len(violations) == 3 and generated else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--base-ref", default="origin/main")
    parser.add_argument("--include-generated", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    root = args.root.resolve()
    completed = subprocess.run(
        ["git", "diff", "--unified=0", "--no-color", args.base_ref, "--", "*.h", "*.hpp", "*.inl", "*.cpp", "*.cc", "*.cxx"],
        cwd=root,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if completed.returncode != 0:
        sys.stderr.write(completed.stderr)
        return completed.returncode or 1

    lines, headers = parse_added_lines(completed.stdout)
    if not args.include_generated:
        lines = [line for line in lines if not is_generated_path(line.path)]
        headers = {header for header in headers if not is_generated_path(header)}
    violations = inspect_added_lines(lines)
    violations.extend(inspect_header_preambles(root, headers))
    if violations:
        for violation in violations:
            print(f"{violation.path}:{violation.line}: {violation.message}", file=sys.stderr)
        return 1
    print(f"changed_cpp_rules=ok files={len({line.path for line in lines})} lines={len(lines)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
