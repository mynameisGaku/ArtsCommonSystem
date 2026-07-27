#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""手書き API リファレンスの型名を現行 C++ 宣言と照合する。

``docs/reference/data/*.js`` の ``name`` / ``kind`` を読み、クラス・構造体・列挙・
インターフェース・テンプレートとして掲載された名前が ``src`` の実宣言と一致するか
確認する。型 alias、delegate、callback、関数、macro は意図的に対象外とする。
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import html
from pathlib import Path
import re
import sys
import tempfile
from typing import Iterable, Sequence

from audit_cpp_conventions import CPP_SUFFIXES, lex_cpp


def configure_utf8_console() -> None:
    """Windows の既定 code page に依存せず、日本語診断を UTF-8 で出力する。"""

    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="strict")


TYPE_KIND_MARKERS = (
    "クラス",
    "構造体",
    "列挙",
    "インターフェース",
    "テンプレート",
)
KNOWN_PREFIXES = "FAEIT"
NAME_PATTERN = re.compile(r'\bname\s*:\s*"((?:\\.|[^"\\])*)"')
KIND_PATTERN = re.compile(r'\bkind\s*:\s*"((?:\\.|[^"\\])*)"')
IDENTIFIER_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*")
EXCLUDED_DIRECTORIES = frozenset(
    {
        ".git",
        ".vs",
        "Binaries",
        "Intermediate",
        "Saved",
        "ThirdParty",
        "third_party",
        "x64",
    }
)


@dataclass(frozen=True)
class FReferenceEntry:
    """リファレンスに掲載された型候補。"""

    path: Path
    line: int
    display_name: str
    kind: str


@dataclass(frozen=True)
class FViolation:
    """旧名と推定される掲載名、および現行宣言の候補。"""

    entry: FReferenceEntry
    old_name: str
    suggested_name: str

    def format(self, root: Path) -> str:
        try:
            display_path = self.entry.path.relative_to(root).as_posix()
        except ValueError:
            display_path = self.entry.path.as_posix()
        return (
            f"{display_path}:{self.entry.line}: error: "
            f"[reference-type-name] {self.old_name} は現行宣言に存在しません。"
            f"{self.suggested_name} を使用してください"
        )


def iter_header_paths(source_root: Path) -> Iterable[Path]:
    """監査対象の C++ header を決定的順序で列挙する。"""

    paths: list[Path] = []
    for path in source_root.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in CPP_SUFFIXES:
            continue
        if any(part in EXCLUDED_DIRECTORIES for part in path.parts):
            continue
        paths.append(path)
    paths.sort(key=lambda item: item.as_posix().casefold())
    return paths


def declaration_name(tokens: Sequence[object], index: int) -> tuple[str | None, int]:
    """class/struct/union/enum token の直後から宣言名を取り出す。"""

    cursor = index + 1
    if tokens[index].text == "enum" and cursor < len(tokens):
        if tokens[cursor].text in {"class", "struct"}:
            cursor += 1

    while cursor < len(tokens):
        if tokens[cursor].text == "alignas":
            cursor += 1
            if cursor >= len(tokens) or tokens[cursor].text != "(":
                return None, cursor
            depth = 0
            while cursor < len(tokens):
                if tokens[cursor].text == "(":
                    depth += 1
                elif tokens[cursor].text == ")":
                    depth -= 1
                    if depth == 0:
                        cursor += 1
                        break
                cursor += 1
            continue

        if (
            tokens[cursor].text == "["
            and cursor + 1 < len(tokens)
            and tokens[cursor + 1].text == "["
        ):
            cursor += 2
            depth = 1
            while cursor < len(tokens):
                if (
                    tokens[cursor].text == "["
                    and cursor + 1 < len(tokens)
                    and tokens[cursor + 1].text == "["
                ):
                    depth += 1
                    cursor += 2
                    continue
                if (
                    tokens[cursor].text == "]"
                    and cursor + 1 < len(tokens)
                    and tokens[cursor + 1].text == "]"
                ):
                    depth -= 1
                    cursor += 2
                    if depth == 0:
                        break
                    continue
                cursor += 1
            if depth != 0:
                return None, cursor
            continue

        break

    if cursor >= len(tokens):
        return None, cursor
    name = tokens[cursor].text
    if not IDENTIFIER_PATTERN.fullmatch(name):
        return None, cursor
    return name, cursor


def collect_declared_types(source_root: Path) -> set[str]:
    """C++ lexer を使い、コメントや文字列を除外して実宣言名を集める。"""

    declared: set[str] = set()
    for path in iter_header_paths(source_root):
        source = path.read_text(encoding="utf-8")
        tokens = lex_cpp(source)
        for index, token in enumerate(tokens):
            if token.text not in {"class", "struct", "union", "enum"}:
                continue
            name, _ = declaration_name(tokens, index)
            if name is not None:
                declared.add(name)
    return declared


def decode_js_string(raw: str) -> str:
    """今回の name/kind で使う最小限の JS escape と HTML entity を戻す。"""

    decoded = (
        raw.replace(r"\"", '"')
        .replace(r"\\", "\\")
        .replace(r"\n", "\n")
        .replace(r"\t", "\t")
    )
    return html.unescape(decoded)


def collect_reference_entries(data_root: Path) -> list[FReferenceEntry]:
    """手書き data file から型掲載 entry を収集する。"""

    entries: list[FReferenceEntry] = []
    for path in sorted(data_root.glob("*.js"), key=lambda item: item.name.casefold()):
        if path.name == "_meta.js":
            continue
        lines = path.read_text(encoding="utf-8").splitlines()
        for index, line in enumerate(lines):
            name_match = NAME_PATTERN.search(line)
            if name_match is None:
                continue
            window = "\n".join(lines[index : min(index + 4, len(lines))])
            kind_match = KIND_PATTERN.search(window)
            if kind_match is None:
                continue
            kind = decode_js_string(kind_match.group(1))
            if not any(marker in kind for marker in TYPE_KIND_MARKERS):
                continue
            entries.append(
                FReferenceEntry(
                    path=path,
                    line=index + 1,
                    display_name=decode_js_string(name_match.group(1)),
                    kind=kind,
                )
            )
    return entries


def entry_identifiers(entry: FReferenceEntry) -> Iterable[str]:
    """``Type / Other<T> / Function()`` から型候補だけを取り出す。"""

    for part in entry.display_name.split("/"):
        part = part.strip()
        if not part or "(" in part:
            continue
        match = IDENTIFIER_PATTERN.match(part)
        if match is not None:
            yield match.group(0)


def candidate_names(name: str, declared: set[str]) -> list[str]:
    """prefix の追加・置換だけで到達する現行宣言候補を返す。"""

    suffix = name[1:] if len(name) >= 2 and name[0] in KNOWN_PREFIXES else name
    exact_candidates = {prefix + suffix for prefix in KNOWN_PREFIXES}
    folded_candidates = {
        candidate.casefold(): candidate for candidate in exact_candidates
    }
    matches = {
        declared_name
        for declared_name in declared
        if declared_name in exact_candidates
        or declared_name.casefold() in folded_candidates
    }
    return sorted(matches)


def audit(
    declared: set[str], entries: Sequence[FReferenceEntry]
) -> list[FViolation]:
    """現行宣言に無く、prefix 修正候補が一意な掲載名を違反として返す。"""

    violations: list[FViolation] = []
    for entry in entries:
        for name in entry_identifiers(entry):
            if name in declared:
                continue
            candidates = candidate_names(name, declared)
            if len(candidates) != 1:
                continue
            violations.append(
                FViolation(
                    entry=entry,
                    old_name=name,
                    suggested_name=candidates[0],
                )
            )
    return violations


def run_self_test() -> int:
    """旧名検出、正名通過、関数・alias 除外の最小 fixture を検証する。"""

    with tempfile.TemporaryDirectory(prefix="acs-reference-audit-") as directory:
        root = Path(directory)
        source_root = root / "src"
        data_root = root / "docs" / "reference" / "data"
        source_root.mkdir(parents=True)
        data_root.mkdir(parents=True)
        (source_root / "Types.h").write_text(
            "class FWorld {};\n"
            "struct FCpuFeatures {};\n"
            "enum class EMode { A };\n"
            "template<typename T> class [[nodiscard]] TResult {};\n"
            "using CompletionCallback = void (*)();\n",
            encoding="utf-8",
        )
        data_path = data_root / "sample.js"
        data_path.write_text(
            'name: "World", kind: "クラス", header: "World.h"\n'
            'name: "CpuFeatures / Cpu()", kind: "構造体・関数"\n'
            'name: "Mode", kind: "列挙"\n'
            'name: "CompletionCallback", kind: "型 alias"\n',
            encoding="utf-8",
        )

        declared = collect_declared_types(source_root)
        if "TResult" not in declared:
            print(
                "reference audit did not recognize an attributed class declaration",
                file=sys.stderr,
            )
            return 1
        entries = collect_reference_entries(data_root)
        violations = audit(declared, entries)
        actual = {(item.old_name, item.suggested_name) for item in violations}
        expected = {
            ("World", "FWorld"),
            ("CpuFeatures", "FCpuFeatures"),
            ("Mode", "EMode"),
        }
        if actual != expected:
            print(
                "reference audit self-test failed: "
                f"expected={sorted(expected)!r}, actual={sorted(actual)!r}",
                file=sys.stderr,
            )
            return 1

        data_path.write_text(
            'name: "FWorld", kind: "クラス", header: "World.h"\n'
            'name: "FCpuFeatures / Cpu()", kind: "構造体・関数"\n'
            'name: "EMode", kind: "列挙"\n',
            encoding="utf-8",
        )
        corrected = audit(
            collect_declared_types(source_root),
            collect_reference_entries(data_root),
        )
        if corrected:
            print("reference audit corrected fixture did not pass", file=sys.stderr)
            return 1

    print("ACS reference type-name audit self-test passed")
    return 0


def parse_arguments(argv: Sequence[str]) -> argparse.Namespace:
    """CLI 引数を解析する。"""

    parser = argparse.ArgumentParser(
        description="ACS 手書き API リファレンスの型名を現行 C++ 宣言と照合します"
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="ACS source tree root (default: script parent)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="一時 fixture だけを検証して終了します",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    """監査を実行し、違反があれば 1、入力不備なら 2 を返す。"""

    configure_utf8_console()
    args = parse_arguments(sys.argv[1:] if argv is None else argv)
    if args.self_test:
        return run_self_test()

    root = args.root.resolve()
    source_root = root / "src"
    data_root = root / "docs" / "reference" / "data"
    if not source_root.is_dir() or not data_root.is_dir():
        print(
            f"error: src または docs/reference/data がありません: {root}",
            file=sys.stderr,
        )
        return 2

    try:
        declared = collect_declared_types(source_root)
        entries = collect_reference_entries(data_root)
    except (OSError, UnicodeError) as error:
        print(f"error: reference audit input を読めません: {error}", file=sys.stderr)
        return 2

    violations = audit(declared, entries)
    for violation in violations:
        print(violation.format(root))
    if violations:
        print(
            f"ACS reference type-name audit failed: {len(violations)} violation(s)",
            file=sys.stderr,
        )
        return 1

    print(
        "ACS reference type-name audit passed: "
        f"{len(entries)} entry(s), {len(declared)} declaration(s)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
