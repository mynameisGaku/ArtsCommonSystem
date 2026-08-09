#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""ACS C++ 宣言、enum 値、メソッド名、削除済み node API の利用を監査する。

単純なテキスト検索ではなく、小さな C++ lexer を意図的に使用する。コメント、通常の
文字列・文字リテラル、HLSL を含む raw string は token を生成しないため、記述例や
shader source が監査へ誤検出されない。prefix を要求するのは class、struct、union、
template class/struct、interface、enum の実宣言だけである。enum 値は型 prefix を
付けない PascalCase とし、既存の class / struct / union / enum 型名そのものを
列挙子へ流用しない。さらに、``object.FType()`` / ``pointer->FType()`` のように
メンバー呼び出し名が既存型名と完全一致する場合は、型 rename の誤波及として検出する。
``using`` / ``typedef`` が
導入する名前は delegate・関数ポインタ callback を含め、意図的に prefix を要求しない。
ただし、新規コードの alias 宣言には ``using`` を使い、``typedef`` は既存 API との互換性を
保つ legacy 宣言としてのみ許容する。この監査器は両者を prefix 監査から除外する。
"""

from __future__ import annotations

import argparse
from contextlib import redirect_stderr, redirect_stdout
from dataclasses import dataclass
import io
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
from typing import Iterable, Optional, Sequence


CPP_SUFFIXES = frozenset({".h", ".hh", ".hpp", ".inl", ".cpp", ".cc", ".cxx", ".ixx"})
DEFAULT_SCOPES = ("src", "tests", "tools", "editor")
EXCLUDED_DIRECTORY_NAMES = frozenset(
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
REMOVED_HEADERS = frozenset(
    {
        "gameframework/node2d.h",
        "gameframework/node3d.h",
        "gameframework/component2d.h",
        "gameframework/component3d.h",
    }
)
REMOVED_HEADER_BASENAMES = frozenset(
    {"node2d.h", "node3d.h", "component2d.h", "component3d.h"}
)
REMOVED_TYPES = frozenset({"FNode2D", "FNode3D", "FComponent2D", "FComponent3D"})
# 実宣言は prefix だけでなく名前全体を PascalCase として検証する。
# underscore は許可せず、T prefix は template class/struct 専用とする。
TYPE_PREFIX = re.compile(r"^[ACFI][A-Z0-9][A-Za-z0-9]*$")
TEMPLATE_TYPE_PREFIX = re.compile(r"^T[A-Z0-9][A-Za-z0-9]*$")
ENUM_PREFIX = re.compile(r"^E[A-Z0-9][A-Za-z0-9]*$")
RAW_STRING_START = re.compile(r'(?:u8|u|U|L)?R"([^\s()\\]{0,16})\(')
IDENTIFIER_START = re.compile(r"[A-Za-z_]")
IDENTIFIER_CONTINUE = re.compile(r"[A-Za-z0-9_]")


@dataclass(frozen=True)
class Token:
    text: str
    line: int
    column: int


@dataclass(frozen=True)
class Violation:
    path: Path
    line: int
    column: int
    rule: str
    message: str

    def format(self, root: Path) -> str:
        try:
            display_path = self.path.relative_to(root)
        except ValueError:
            display_path = self.path
        return (
            f"{display_path.as_posix()}:{self.line}:{self.column}: "
            f"error: [{self.rule}] {self.message}"
        )


JSON_SCHEMA_VERSION = 1


def configure_utf8_console() -> None:
    """Windows の既定 code page に依存せず、日本語診断を UTF-8 で出力する。"""

    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="strict")


def _display_path(path: Path, root: Path) -> str:
    """root 内は相対POSIX形式、それ以外は絶対POSIX形式で返す。"""

    try:
        display_path = path.relative_to(root)
    except ValueError:
        display_path = path
    return display_path.as_posix()


def build_json_report(
    root: Path, files: Sequence[Path], violations: Sequence[Violation]
) -> dict[str, object]:
    """CI向けの安定した監査レポートを構築する。"""

    ordered_violations = sorted(
        violations,
        key=lambda item: (
            _display_path(item.path, root).casefold(),
            _display_path(item.path, root),
            item.line,
            item.column,
            item.rule,
            item.message,
        ),
    )
    return {
        "schema_version": JSON_SCHEMA_VERSION,
        "scanned_file_count": len(files),
        "violation_count": len(ordered_violations),
        "violations": [
            {
                "rule": violation.rule,
                "path": _display_path(violation.path, root),
                "line": violation.line,
                "column": violation.column,
                "message": violation.message,
            }
            for violation in ordered_violations
        ],
    }


def serialize_json_report(report: dict[str, object]) -> str:
    """UTF-8へ直接書ける、末尾改行付きの決定的JSONを返す。"""

    return json.dumps(report, ensure_ascii=False, indent=2) + "\n"


def write_json_stdout(report_text: str) -> None:
    """実stdoutにはUTF-8 bytes、差替え済みtext streamには文字列を書く。"""

    binary_stream = getattr(sys.stdout, "buffer", None)
    if binary_stream is None:
        sys.stdout.write(report_text)
        return
    binary_stream.write(report_text.encode("utf-8"))
    binary_stream.flush()


def lexical_absolute_path(path: Path) -> Path:
    """symlinkを辿らず、現在directory基準の絶対pathへ字句的に変換する。"""

    return Path(os.path.abspath(os.fspath(path.expanduser())))


def is_symlink_or_reparse_point(path: Path) -> bool:
    """既存の最終path要素がsymlinkまたはWindows reparse pointかを返す。"""

    try:
        path_status = os.lstat(path)
    except FileNotFoundError:
        return False

    if stat.S_ISLNK(path_status.st_mode):
        return True
    reparse_attribute = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    file_attributes = getattr(path_status, "st_file_attributes", 0)
    return bool(reparse_attribute and file_attributes & reparse_attribute)


def write_json_report_atomic(output_path: Path, report_text: str) -> None:
    """同一directoryの一時ファイルからreplaceし、不完全な成果物を残さない。"""

    temporary_path: Optional[Path] = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            dir=output_path.parent,
            prefix=f".{output_path.name}.",
            suffix=".tmp",
            delete=False,
        ) as temporary_file:
            temporary_path = Path(temporary_file.name)
            temporary_file.write(report_text)
            temporary_file.flush()
            os.fsync(temporary_file.fileno())
        temporary_path.replace(output_path)
        temporary_path = None
    finally:
        if temporary_path is not None:
            try:
                temporary_path.unlink()
            except OSError:
                # 元の書込み例外を優先し、一時ファイル清掃失敗で上書きしない。
                pass


def try_write_json_report(
    requested_output_path: Path, report_text: str
) -> Optional[BaseException]:
    """CLI指定pathへ安全に保存し、失敗時はtracebackではなく原因を返す。"""

    try:
        output_path = lexical_absolute_path(requested_output_path)
        if is_symlink_or_reparse_point(output_path):
            raise OSError(
                "symbolic link or reparse point output destinations are forbidden"
            )
        write_json_report_atomic(output_path, report_text)
    except (OSError, UnicodeError, RuntimeError, ValueError) as error:
        return error
    return None


def _advance_position(segment: str, line: int, column: int) -> tuple[int, int]:
    newline_count = segment.count("\n")
    if newline_count:
        return line + newline_count, len(segment.rsplit("\n", 1)[1]) + 1
    return line, column + len(segment)


def _line_splice_length(source: str, index: int) -> int:
    """phase 2 で削除される backslash-newline の長さを返す。"""

    if source.startswith("\\\r\n", index):
        return 3
    if source.startswith("\\\n", index):
        return 2
    return 0


def _find_logical_line_end(source: str, start: int) -> int:
    """行継続を越えて最初の論理改行位置を返す。"""

    index = start
    while index < len(source):
        splice_length = _line_splice_length(source, index)
        if splice_length:
            index += splice_length
            continue
        if source[index] == "\n":
            return index
        index += 1
    return len(source)


def _remove_line_splices(source: str) -> tuple[str, list[tuple[int, int]]]:
    """phase 2 後の source と各文字の元の物理行・列を返す。"""

    characters: list[str] = []
    positions: list[tuple[int, int]] = []
    index = 0
    line = 1
    column = 1
    while index < len(source):
        splice_length = _line_splice_length(source, index)
        if splice_length:
            end = index + splice_length
            line, column = _advance_position(source[index:end], line, column)
            index = end
            continue
        characters.append(source[index])
        positions.append((line, column))
        line, column = _advance_position(source[index], line, column)
        index += 1
    return "".join(characters), positions


def _skip_quoted(source: str, start: int, quote: str) -> int:
    index = start + 1
    while index < len(source):
        if source[index] == "\\":
            index += 2
            continue
        if source[index] == quote:
            return index + 1
        index += 1
    return len(source)


def lex_cpp(source: str) -> list[Token]:
    """コメントと全リテラルの内容を除外して C++ token を返す。"""

    source, source_positions = _remove_line_splices(source)
    tokens: list[Token] = []
    index = 0
    source_length = len(source)

    while index < source_length:
        raw_match = RAW_STRING_START.match(source, index)
        if raw_match is not None:
            delimiter = raw_match.group(1)
            terminator = ")" + delimiter + '"'
            end = source.find(terminator, raw_match.end())
            end = source_length if end < 0 else end + len(terminator)
            index = end
            continue

        if source.startswith("//", index):
            end = source.find("\n", index + 2)
            end = source_length if end < 0 else end
            index = end
            continue

        if source.startswith("/*", index):
            end = source.find("*/", index + 2)
            end = source_length if end < 0 else end + 2
            index = end
            continue

        char = source[index]
        if char in {'"', "'"}:
            end = _skip_quoted(source, index, char)
            index = end
            continue

        if IDENTIFIER_START.match(char):
            end = index + 1
            while end < source_length and IDENTIFIER_CONTINUE.match(source[end]):
                end += 1
            line, column = source_positions[index]
            tokens.append(Token(source[index:end], line, column))
            index = end
            continue

        if source.startswith("::", index):
            line, column = source_positions[index]
            tokens.append(Token("::", line, column))
            index += 2
            continue

        if not char.isspace():
            line, column = source_positions[index]
            tokens.append(Token(char, line, column))

        index += 1

    return tokens


def _skip_directive_whitespace_and_comments(directive: str, index: int) -> int:
    """前処理 directive 内で空白相当になる範囲を読み飛ばす。"""

    while index < len(directive):
        if directive[index] in " \t\v\f\r":
            index += 1
            continue
        if directive.startswith("/*", index):
            comment_end = directive.find("*/", index + 2)
            if comment_end < 0:
                return len(directive)
            index = comment_end + 2
            continue
        break
    return index


def _extract_include_directive(directive: str) -> Optional[tuple[str, int]]:
    """有効な include directive から header 名と開始位置を返す。"""

    index = _skip_directive_whitespace_and_comments(directive, 0)
    if index >= len(directive) or directive[index] != "#":
        return None
    index = _skip_directive_whitespace_and_comments(directive, index + 1)

    keyword_start = index
    while index < len(directive) and IDENTIFIER_CONTINUE.match(directive[index]):
        index += 1
    if directive[keyword_start:index] != "include":
        return None

    index = _skip_directive_whitespace_and_comments(directive, index)
    if index >= len(directive) or directive[index] not in {'<', '"'}:
        return None
    closing = ">" if directive[index] == "<" else '"'
    header_start = index + 1
    header_end = directive.find(closing, header_start)
    if header_end < 0:
        return None
    return directive[header_start:header_end], header_start


def _read_logical_directive(source: str, start: int) -> tuple[str, list[int], int]:
    """行継続を除いた directive と各文字の元 source offset を返す。"""

    characters: list[str] = []
    source_offsets: list[int] = []
    index = start
    while index < len(source):
        splice_length = _line_splice_length(source, index)
        if splice_length:
            index += splice_length
            continue
        if source[index] == "\n":
            break
        characters.append(source[index])
        source_offsets.append(index)
        index += 1
    return "".join(characters), source_offsets, index


def extract_includes(source: str) -> list[tuple[str, int, int]]:
    """コメントとリテラルの外にある有効な include directive を抽出する。"""

    includes: list[tuple[str, int, int]] = []
    index = 0
    line = 1
    line_start = 0
    only_whitespace_on_line = True
    source_length = len(source)

    while index < source_length:
        splice_length = _line_splice_length(source, index)
        if splice_length:
            end = index + splice_length
            line, _ = _advance_position(source[index:end], line, 1)
            index = end
            line_start = index
            continue

        if source.startswith("//", index):
            end = _find_logical_line_end(source, index + 2)
            segment = source[index:end]
            newline_count = segment.count("\n")
            if newline_count:
                line += newline_count
                line_start = index + segment.rfind("\n") + 1
            index = end
            continue
        if source.startswith("/*", index):
            end = source.find("*/", index + 2)
            end = source_length if end < 0 else end + 2
            segment = source[index:end]
            newline_count = segment.count("\n")
            if newline_count:
                line += newline_count
                line_start = index + segment.rfind("\n") + 1
                # block comment 内で改行した後の文字は全て空白相当である。
                only_whitespace_on_line = True
            index = end
            continue

        raw_match = RAW_STRING_START.match(source, index)
        if raw_match is not None:
            terminator = ")" + raw_match.group(1) + '"'
            end = source.find(terminator, raw_match.end())
            end = source_length if end < 0 else end + len(terminator)
            segment = source[index:end]
            newline_count = segment.count("\n")
            if newline_count:
                line += newline_count
                line_start = index + segment.rfind("\n") + 1
                only_whitespace_on_line = False
            index = end
            continue

        char = source[index]
        if char == "\n":
            line += 1
            index += 1
            line_start = index
            only_whitespace_on_line = True
            continue
        if char in {'"', "'"}:
            index = _skip_quoted(source, index, char)
            only_whitespace_on_line = False
            continue
        if char == "#" and only_whitespace_on_line:
            directive, source_offsets, end = _read_logical_directive(source, index)
            # 直前の block comment を混ぜず、directive 自体の開始位置から解析する。
            include_directive = _extract_include_directive(directive)
            if include_directive is not None:
                include_path, header_start = include_directive
                header_offset = source_offsets[header_start]
                header_line = line + source[index:header_offset].count("\n")
                header_line_start = source.rfind("\n", index, header_offset)
                if header_line_start < 0:
                    header_line_start = line_start
                else:
                    header_line_start += 1
                includes.append(
                    (
                        include_path.replace("\\", "/"),
                        header_line,
                        header_offset - header_line_start + 1,
                    )
                )
            consumed = source[index:end]
            newline_count = consumed.count("\n")
            if newline_count:
                line += newline_count
                line_start = index + consumed.rfind("\n") + 1
            index = end
            continue
        if not char.isspace():
            only_whitespace_on_line = False
        index += 1

    return includes


def _skip_balanced(
    tokens: Sequence[Token], index: int, opening: str, closing: str
) -> int:
    if index >= len(tokens) or tokens[index].text != opening:
        return index
    depth = 0
    while index < len(tokens):
        if tokens[index].text == opening:
            depth += 1
        elif tokens[index].text == closing:
            depth -= 1
            if depth == 0:
                return index + 1
        index += 1
    return index


def _skip_attributes_and_alignment(tokens: Sequence[Token], index: int) -> int:
    while index < len(tokens):
        if (
            tokens[index].text == "["
            and index + 1 < len(tokens)
            and tokens[index + 1].text == "["
        ):
            index += 2
            while index + 1 < len(tokens):
                if tokens[index].text == "]" and tokens[index + 1].text == "]":
                    index += 2
                    break
                index += 1
            continue
        if tokens[index].text == "alignas" and index + 1 < len(tokens):
            index = _skip_balanced(tokens, index + 1, "(", ")")
            continue
        break
    return index


def _declaration_name(
    tokens: Sequence[Token], keyword_index: int, enum_declaration: bool
) -> tuple[Optional[Token], int]:
    index = keyword_index + 1
    if enum_declaration and index < len(tokens) and tokens[index].text in {"class", "struct"}:
        index += 1
    index = _skip_attributes_and_alignment(tokens, index)

    # export / visibility macro は keyword と型名の間に置かれることがある。
    while index + 1 < len(tokens):
        candidate = tokens[index].text
        if candidate == "ACS_CONCAT" and tokens[index + 1].text == "(":
            macro_end = _skip_balanced(tokens, index + 1, "(", ")")
            if index + 2 < macro_end and IDENTIFIER_START.match(tokens[index + 2].text[0]):
                return tokens[index + 2], macro_end
            return None, macro_end
        if (
            IDENTIFIER_START.match(candidate[0])
            and (candidate.startswith("ACS_") or candidate.endswith("_API"))
        ):
            if tokens[index + 1].text == "(":
                index = _skip_balanced(tokens, index + 1, "(", ")")
            else:
                index += 1
            index = _skip_attributes_and_alignment(tokens, index)
            continue
        break

    if index >= len(tokens) or not IDENTIFIER_START.match(tokens[index].text[0]):
        return None, index
    name = tokens[index]
    index += 1

    # `class namespace::FType` は修飾された elaborated name / 再宣言であり、
    # `namespace` を型名として導入する宣言ではない。
    if index < len(tokens) and tokens[index].text == "::":
        return None, index

    simple_template_id = index < len(tokens) and tokens[index].text == "<"
    if simple_template_id:
        index = _skip_balanced(tokens, index, "<", ">")
    index = _skip_attributes_and_alignment(tokens, index)
    while index < len(tokens) and tokens[index].text in {"final"}:
        index += 1
        index = _skip_attributes_and_alignment(tokens, index)

    if index < len(tokens) and tokens[index].text in {":", "{", ";"}:
        return name, index
    # `struct Missing* value;` や引数中の `class Missing*` は、未修飾名なら
    # 暗黙の forward declaration になる。simple-template-id は既存型の利用なので除外する。
    if not enum_declaration and not simple_template_id:
        return name, index
    return None, index


def collect_declared_type_names(tokens: Sequence[Token]) -> frozenset[str]:
    """token 列が導入する class / struct / union / enum 型名を収集する。"""

    names: set[str] = set()
    index = 0
    while index < len(tokens):
        text = tokens[index].text
        if text not in {"class", "struct", "union", "enum"}:
            index += 1
            continue
        if (
            text in {"class", "struct"}
            and index > 0
            and tokens[index - 1].text == "enum"
        ):
            index += 1
            continue
        if (
            text in {"class", "struct", "union"}
            and index > 0
            and tokens[index - 1].text == "template"
        ):
            index += 1
            continue

        name, declaration_end = _declaration_name(
            tokens, index, enum_declaration=text == "enum"
        )
        if (
            name is not None
            and declaration_end < len(tokens)
            and tokens[declaration_end].text in {":", "{", ";"}
        ):
            names.add(name.text)
        index = max(index + 1, declaration_end)
    return frozenset(names)


def audit_enum_value_type_collisions(
    path: Path,
    tokens: Sequence[Token],
    declared_type_names: frozenset[str],
) -> list[Violation]:
    """既存型名と完全一致する enum 値を検出する。"""

    violations: list[Violation] = []
    collision_names = set(declared_type_names)
    for type_name in declared_type_names:
        family_name = type_name.rstrip("0123456789")
        if family_name != type_name:
            collision_names.add(family_name)
    index = 0
    while index < len(tokens):
        if tokens[index].text != "enum":
            index += 1
            continue

        _name, declaration_end = _declaration_name(
            tokens, index, enum_declaration=True
        )
        body_start = declaration_end
        while (
            body_start < len(tokens)
            and tokens[body_start].text not in {"{", ";"}
        ):
            body_start += 1
        if body_start >= len(tokens) or tokens[body_start].text != "{":
            index = max(index + 1, body_start)
            continue

        brace_depth = 1
        parenthesis_depth = 0
        bracket_depth = 0
        expect_enumerator = True
        cursor = body_start + 1
        while cursor < len(tokens) and brace_depth > 0:
            token = tokens[cursor]
            text = token.text

            if (
                expect_enumerator
                and brace_depth == 1
                and parenthesis_depth == 0
                and bracket_depth == 0
            ):
                candidate_index = _skip_attributes_and_alignment(tokens, cursor)
                if candidate_index != cursor:
                    cursor = candidate_index
                    continue
                if IDENTIFIER_START.match(text[0]):
                    if text in collision_names:
                        violations.append(
                            Violation(
                                path,
                                token.line,
                                token.column,
                                "ACS-R027",
                                f"enum 値 '{text}' が型名または型 family と衝突している; "
                                "値は型 prefix なしの PascalCase にする",
                            )
                        )
                    expect_enumerator = False

            if text == "{":
                brace_depth += 1
            elif text == "}":
                brace_depth -= 1
            elif text == "(":
                parenthesis_depth += 1
            elif text == ")" and parenthesis_depth > 0:
                parenthesis_depth -= 1
            elif text == "[":
                bracket_depth += 1
            elif text == "]" and bracket_depth > 0:
                bracket_depth -= 1
            elif (
                text == ","
                and brace_depth == 1
                and parenthesis_depth == 0
                and bracket_depth == 0
            ):
                expect_enumerator = True
            cursor += 1

        index = max(index + 1, cursor)
    return violations


def audit_member_call_type_collisions(
    path: Path,
    tokens: Sequence[Token],
    declared_type_names: frozenset[str],
) -> list[Violation]:
    """メンバー呼び出し名と既存型名の完全一致を検出する。"""

    violations: list[Violation] = []
    index = 0
    while index < len(tokens):
        member_name_index: Optional[int] = None
        if (
            tokens[index].text == "."
            and index + 2 < len(tokens)
            and tokens[index + 2].text == "("
        ):
            member_name_index = index + 1
        elif (
            tokens[index].text == "-"
            and index + 3 < len(tokens)
            and tokens[index + 1].text == ">"
            and tokens[index + 3].text == "("
        ):
            member_name_index = index + 2

        if member_name_index is not None:
            member_name = tokens[member_name_index]
            if member_name.text in declared_type_names:
                violations.append(
                    Violation(
                        path,
                        member_name.line,
                        member_name.column,
                        "ACS-R021",
                        f"メンバー呼び出し '{member_name.text}()' が既存型名と衝突している; "
                        "関数・メソッド名へ型 prefix を付けない",
                    )
                )
        index += 1
    return violations


def audit_tokens(
    path: Path,
    tokens: Sequence[Token],
    declared_type_names: Optional[frozenset[str]] = None,
) -> list[Violation]:
    violations: list[Violation] = []
    pending_template = False
    index = 0

    for token in tokens:
        if token.text in REMOVED_TYPES:
            violations.append(
                Violation(
                    path,
                    token.line,
                    token.column,
                    "ACS-NODE-001",
                    f"removed node type '{token.text}' is forbidden; use ANode/AComponent",
                )
            )

    while index < len(tokens):
        text = tokens[index].text
        if text == "template" and index + 1 < len(tokens) and tokens[index + 1].text == "<":
            index = _skip_balanced(tokens, index + 1, "<", ">")
            pending_template = True
            continue

        if text in {"class", "struct", "union", "enum"}:
            enum_declaration = text == "enum"
            # `enum class` の class token は enum 宣言の一部なので二重検査しない。
            if index > 0 and text in {"class", "struct"} and tokens[index - 1].text == "enum":
                index += 1
                continue
            # `template class TType<...>;` などは明示的 instantiation であり、
            # 新しい非 template 型を導入する宣言ではない。
            if (
                index > 0
                and text in {"class", "struct", "union"}
                and tokens[index - 1].text == "template"
            ):
                index += 1
                continue

            name, declaration_end = _declaration_name(tokens, index, enum_declaration)
            if name is not None:
                introduces_type_declaration = (
                    declaration_end < len(tokens)
                    and tokens[declaration_end].text in {":", "{", ";"}
                )
                if enum_declaration:
                    if ENUM_PREFIX.match(name.text) is None:
                        violations.append(
                            Violation(
                                path,
                                name.line,
                                name.column,
                                "ACS-R027",
                                f"enum 型 '{name.text}' は E prefix + PascalCase が必要",
                            )
                        )
                # function / variable template 内の elaborated type は非 template 型である。
                elif pending_template and introduces_type_declaration:
                    if TEMPLATE_TYPE_PREFIX.match(name.text) is None:
                        violations.append(
                            Violation(
                                path,
                                name.line,
                                name.column,
                                "ACS-R020b",
                                f"template 型 '{name.text}' は T prefix + PascalCase が必要",
                            )
                        )
                elif TYPE_PREFIX.match(name.text) is None:
                    violations.append(
                        Violation(
                            path,
                            name.line,
                            name.column,
                            "ACS-R020a",
                            f"型 '{name.text}' は A/C/F/I prefix + PascalCase が必要",
                        )
                    )
                pending_template = False
                index = max(index + 1, declaration_end)
                continue

        if pending_template and text in {";", "{"}:
            pending_template = False
        index += 1

    known_type_names = (
        collect_declared_type_names(tokens)
        if declared_type_names is None
        else declared_type_names
    )
    violations.extend(
        audit_enum_value_type_collisions(path, tokens, known_type_names)
    )
    violations.extend(
        audit_member_call_type_collisions(path, tokens, known_type_names)
    )
    return violations


def audit_file(
    path: Path,
    declared_type_names: Optional[frozenset[str]] = None,
) -> list[Violation]:
    try:
        source = path.read_text(encoding="utf-8-sig")
    except UnicodeDecodeError as error:
        return [
            Violation(path, 1, 1, "ACS-AUDIT-001", f"source is not valid UTF-8: {error}")
        ]

    violations: list[Violation] = []
    for include_path, line, column in extract_includes(source):
        normalized = include_path.lower()
        basename = normalized.rsplit("/", 1)[-1]
        if normalized in REMOVED_HEADERS or basename in REMOVED_HEADER_BASENAMES:
            violations.append(
                Violation(
                    path,
                    line,
                    column,
                    "ACS-NODE-002",
                    f"removed header '{include_path}' is forbidden; include "
                    "gameframework/ANode.h or gameframework/AComponent.h",
                )
            )
    violations.extend(
        audit_tokens(path, lex_cpp(source), declared_type_names)
    )
    return violations


def discover_files(root: Path, scopes: Iterable[str]) -> list[Path]:
    files: list[Path] = []
    for scope in scopes:
        scope_path = root / scope
        if not scope_path.exists():
            continue
        for path in scope_path.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in CPP_SUFFIXES:
                continue
            relative_parts = path.relative_to(root).parts
            if any(
                part in EXCLUDED_DIRECTORY_NAMES
                or part.startswith("cmake-build-")
                for part in relative_parts
            ):
                continue
            files.append(path)
    return sorted(set(files), key=lambda item: item.as_posix().lower())


def run_audit(root: Path, scopes: Iterable[str]) -> tuple[list[Path], list[Violation]]:
    files = discover_files(root, scopes)
    declared_type_names: set[str] = set()
    for path in files:
        try:
            source = path.read_text(encoding="utf-8-sig")
        except UnicodeDecodeError:
            continue
        declared_type_names.update(
            collect_declared_type_names(lex_cpp(source))
        )
    frozen_type_names = frozenset(declared_type_names)
    violations: list[Violation] = []
    for path in files:
        violations.extend(audit_file(path, frozen_type_names))
    violations.sort(key=lambda item: (item.path.as_posix().lower(), item.line, item.column))
    return files, violations


@dataclass(frozen=True)
class SelfTestCliResult:
    exit_code: int
    stdout: str
    stderr: str


def invoke_main_for_self_test(
    argv: Sequence[str], stdout_stream: Optional[io.StringIO] = None
) -> SelfTestCliResult:
    """main相当のCLI処理を隔離し、出力と終了値を回帰検証用に捕捉する。"""

    stdout = io.StringIO() if stdout_stream is None else stdout_stream
    stderr = io.StringIO()
    with redirect_stdout(stdout), redirect_stderr(stderr):
        try:
            exit_code = main(argv)
        except SystemExit as error:
            exit_code = error.code if isinstance(error.code, int) else 1
    return SelfTestCliResult(exit_code, stdout.getvalue(), stderr.getvalue())


def run_self_test() -> int:
    valid_source = r'''
// class BadComment {};
const char* text = "struct BadString {};";
const char* shader = R"HLSL(
struct VSIn { float3 Position : POSITION; };
class BadShader {};
)HLSL";
class FValue;
class AObject {};
class AManagedObject : public AObject {};
using FObject = AObject;
class CRenderer {};
class IReader { virtual void Read() = 0; };
struct FMethodTarget {};
template<typename T> class TArray {};
template<typename T> union TStorage {};
enum class EState : unsigned char { Ready, Fatal };
class fixture::BadQualified;
using HotReloadCallback = void (*)(void* user) noexcept;
using FHotReloadCallback = HotReloadCallback;
using RenderTarget = FValue;
typedef void (*JudgeCallback)(void* user) noexcept;
typedef JudgeCallback FJudgeCallback;
typedef JudgeCallback BeatEndCallback;
template<typename T> using CallbackList = TArray<T>;
template class TArray<int>;
template union TStorage<int>;
void UseStorage(union TStorage<int>* storage);
template<typename T> void UsePayload(struct FPayload* payload);
void UseMethodNames(FMethodTarget& value, FMethodTarget* pointer) {
    value.Format();
    pointer->Find();
    value.FPS();
    value.IASetVertexBuffers();
    FMethodTarget();
}
int continued = 0; \
#include "gameframework/Node2D.h"
// #include "gameframework/Node2D.h"
'''
    invalid_source = r'''
#include "gameframework/Node2D.h"
/* 複数行 block comment は前処理上の空白として扱う。
*/ #include "gameframework/Node2D.h"
class MissingPrefix {};
template<typename T> struct FWrongTemplate {};
enum class WrongEnum { Value };
class TNonTemplate {};
class FGood_bad {};
template<typename T> struct TGood_bad {};
enum class EGood_bad { Value };
void Use(FNode3D* node);
#/**/include/**/"gameframework/Node2D.h"
struct MissingElaborated* value;
void Read(class MissingParameter* value);
#\
include "gameframework/Node2D.h"
#inc\
lude "gameframework/Node2D.h"
void UseSpliced(FNode\
2D* node);
template<typename T> void UseBadPayload(struct TPayload* payload);
struct FEnumPayload {};
struct FAabb2 {};
enum class EBadValue : unsigned char { FEnumPayload, FAabb };
struct FMethodCollision {};
struct FReceiver {};
void InvokeBad(FReceiver& value, FReceiver* pointer) {
    value.FMethodCollision();
    pointer->FMethodCollision();
    FMethodCollision();
}
'''

    with tempfile.TemporaryDirectory(prefix="acs-conventions-") as temporary_directory:
        root = Path(temporary_directory)
        valid_path = root / "src" / "valid.h"
        invalid_path = root / "src" / "invalid.cpp"
        valid_path.parent.mkdir(parents=True)
        valid_path.write_text(valid_source, encoding="utf-8")
        invalid_path.write_text(invalid_source, encoding="utf-8")

        valid_violations = audit_file(valid_path)
        invalid_violations = audit_file(invalid_path)
        actual_rule_lines = [
            (violation.rule, violation.line) for violation in invalid_violations
        ]
        expected_rule_lines = [
            ("ACS-NODE-002", 2),
            ("ACS-NODE-002", 4),
            ("ACS-NODE-002", 13),
            ("ACS-NODE-002", 17),
            ("ACS-NODE-002", 19),
            ("ACS-NODE-001", 12),
            ("ACS-NODE-001", 20),
            ("ACS-R020a", 5),
            ("ACS-R020b", 6),
            ("ACS-R027", 7),
            ("ACS-R020a", 8),
            ("ACS-R020a", 9),
            ("ACS-R020b", 10),
            ("ACS-R027", 11),
            ("ACS-R020a", 14),
            ("ACS-R020a", 15),
            ("ACS-R020a", 22),
            ("ACS-R027", 25),
            ("ACS-R027", 25),
            ("ACS-R021", 29),
            ("ACS-R021", 30),
        ]

        if valid_violations:
            print("self-test failed: valid fixture produced violations", file=sys.stderr)
            for violation in valid_violations:
                print(violation.format(root), file=sys.stderr)
            return 1
        if sorted(actual_rule_lines) != sorted(expected_rule_lines):
            print(
                "self-test failed: expected "
                f"{sorted(expected_rule_lines)}, got {sorted(actual_rule_lines)}",
                file=sys.stderr,
            )
            for violation in invalid_violations:
                print(violation.format(root), file=sys.stderr)
            return 1

        zero_report = build_json_report(root, [valid_path], [])
        if (
            list(zero_report) != [
                "schema_version",
                "scanned_file_count",
                "violation_count",
                "violations",
            ]
            or zero_report["schema_version"] != JSON_SCHEMA_VERSION
            or zero_report["scanned_file_count"] != 1
            or zero_report["violation_count"] != 0
            or zero_report["violations"] != []
        ):
            print(
                f"self-test failed: invalid zero-violation JSON report: {zero_report}",
                file=sys.stderr,
            )
            return 1

        unicode_path = root / "src" / "日本語.cpp"
        ordered_path = root / "src" / "A.cpp"
        report_violations = [
            Violation(
                unicode_path,
                8,
                3,
                "ACS-R027",
                "日本語の違反メッセージ",
            ),
            Violation(ordered_path, 4, 2, "ACS-Z", "後の規則"),
            Violation(ordered_path, 4, 2, "ACS-A", "先の規則"),
        ]
        multiple_report = build_json_report(
            root,
            [unicode_path, ordered_path],
            report_violations,
        )
        report_entries = multiple_report["violations"]
        expected_order = [
            ("ACS-A", "src/A.cpp", 4, 2, "先の規則"),
            ("ACS-Z", "src/A.cpp", 4, 2, "後の規則"),
            ("ACS-R027", "src/日本語.cpp", 8, 3, "日本語の違反メッセージ"),
        ]
        actual_order = [
            (
                entry["rule"],
                entry["path"],
                entry["line"],
                entry["column"],
                entry["message"],
            )
            for entry in report_entries
        ]
        if (
            multiple_report["scanned_file_count"] != 2
            or multiple_report["violation_count"] != 3
            or actual_order != expected_order
            or any(
                list(entry) != ["rule", "path", "line", "column", "message"]
                for entry in report_entries
            )
        ):
            print(
                "self-test failed: invalid multi-violation JSON report "
                f"or ordering: {multiple_report}",
                file=sys.stderr,
            )
            return 1

        report_text = serialize_json_report(multiple_report)
        reversed_report_text = serialize_json_report(
            build_json_report(
                root,
                list(reversed([unicode_path, ordered_path])),
                list(reversed(report_violations)),
            )
        )
        if (
            report_text != reversed_report_text
            or json.loads(report_text) != multiple_report
            or "日本語" not in report_text
            or "\\u65e5" in report_text
        ):
            print(
                "self-test failed: JSON serialization is not deterministic UTF-8",
                file=sys.stderr,
            )
            return 1

        report_path = root / "audit-report.json"
        report_path.write_text("置換前", encoding="utf-8")
        write_json_report_atomic(report_path, report_text)
        if report_path.read_bytes() != report_text.encode("utf-8"):
            print(
                "self-test failed: atomic JSON output bytes differ",
                file=sys.stderr,
            )
            return 1

        directory_output_path = root / "report-directory"
        directory_output_path.mkdir()
        temporary_files_before = set(root.glob(".report-directory.*.tmp"))
        try:
            write_json_report_atomic(directory_output_path, report_text)
        except OSError:
            pass
        else:
            print(
                "self-test failed: invalid JSON output destination succeeded",
                file=sys.stderr,
            )
            return 1
        if set(root.glob(".report-directory.*.tmp")) != temporary_files_before:
            print(
                "self-test failed: failed JSON output left a temporary file",
                file=sys.stderr,
            )
            return 1

        cli_root = root / "cli"
        cli_valid_path = cli_root / "clean" / "valid.h"
        cli_invalid_path = cli_root / "invalid" / "invalid.h"
        cli_valid_path.parent.mkdir(parents=True)
        cli_invalid_path.parent.mkdir(parents=True)
        cli_valid_path.write_text("class FValid {};\n", encoding="utf-8")
        cli_invalid_path.write_text("class MissingPrefix {};\n", encoding="utf-8")

        human_success = invoke_main_for_self_test(
            ["--root", str(cli_root), "--scope", "clean"]
        )
        if (
            human_success.exit_code != 0
            or human_success.stdout
            != "ACS C++ conventions audit passed: 1 file(s)\n"
            or human_success.stderr
        ):
            print(
                f"self-test failed: invalid successful human CLI result: {human_success}",
                file=sys.stderr,
            )
            return 1

        human_failure = invoke_main_for_self_test(
            ["--root", str(cli_root), "--scope", "invalid"]
        )
        if (
            human_failure.exit_code != 1
            or human_failure.stdout
            or "[ACS-R020a]" not in human_failure.stderr
            or "1 violation(s) in 1 file(s)" not in human_failure.stderr
        ):
            print(
                f"self-test failed: invalid failing human CLI result: {human_failure}",
                file=sys.stderr,
            )
            return 1

        json_failure = invoke_main_for_self_test(
            [
                "--root",
                str(cli_root),
                "--scope",
                "invalid",
                "--format",
                "json",
            ]
        )
        try:
            json_failure_report = json.loads(json_failure.stdout)
        except json.JSONDecodeError:
            json_failure_report = None
        if (
            json_failure.exit_code != 1
            or json_failure.stderr
            or json_failure_report is None
            or json_failure_report["violation_count"] != 1
            or json_failure_report["violations"][0]["rule"] != "ACS-R020a"
        ):
            print(
                f"self-test failed: invalid failing JSON CLI result: {json_failure}",
                file=sys.stderr,
            )
            return 1

        cli_report_path = cli_root / "audit-report.json"
        json_success = invoke_main_for_self_test(
            [
                "--root",
                str(cli_root),
                "--scope",
                "clean",
                "--format",
                "json",
                "--json-output",
                str(cli_report_path),
            ]
        )
        if (
            json_success.exit_code != 0
            or json_success.stderr
            or not cli_report_path.is_file()
            or cli_report_path.read_text(encoding="utf-8") != json_success.stdout
        ):
            print(
                f"self-test failed: invalid successful JSON CLI result: {json_success}",
                file=sys.stderr,
            )
            return 1

        class FailingStdout(io.StringIO):
            """stdoutのclosed/reentrant相当エラーを決定的に発生させる。"""

            def __init__(self, error: Exception) -> None:
                super().__init__()
                self._error = error

            def write(self, value: str) -> int:
                del value
                raise self._error

        original_argv = sys.argv
        original_stdout = sys.stdout
        original_stderr = sys.stderr
        stdout_write_errors = (
            ValueError("I/O operation on closed file"),
            RuntimeError("synthetic reentrant stdout write"),
        )
        for stdout_write_error in stdout_write_errors:
            stdout_error_result = invoke_main_for_self_test(
                [
                    "--root",
                    str(cli_root),
                    "--scope",
                    "clean",
                    "--format",
                    "json",
                ],
                FailingStdout(stdout_write_error),
            )
            if (
                stdout_error_result.exit_code != 2
                or stdout_error_result.stdout
                or "report write failed: stdout:" not in stdout_error_result.stderr
                or "Traceback" in stdout_error_result.stderr
                or sys.argv is not original_argv
                or sys.stdout is not original_stdout
                or sys.stderr is not original_stderr
            ):
                print(
                    "self-test failed: stdout write error was not isolated cleanly: "
                    f"{type(stdout_write_error).__name__}: {stdout_error_result}",
                    file=sys.stderr,
                )
                return 1

        missing_parent_path = cli_root / "missing-parent" / "report.json"
        missing_parent_result = invoke_main_for_self_test(
            [
                "--root",
                str(cli_root),
                "--scope",
                "clean",
                "--format",
                "json",
                "--json-output",
                str(missing_parent_path),
            ]
        )
        if (
            missing_parent_result.exit_code != 2
            or missing_parent_result.stdout
            or "report write failed" not in missing_parent_result.stderr
            or "Traceback" in missing_parent_result.stderr
            or missing_parent_path.parent.exists()
        ):
            print(
                "self-test failed: missing parent was not a clean CLI write error: "
                f"{missing_parent_result}",
                file=sys.stderr,
            )
            return 1

        conflict_report_path = cli_root / "ignored-report.json"
        conflicting_options = invoke_main_for_self_test(
            [
                "--self-test",
                "--format",
                "json",
                "--json-output",
                str(conflict_report_path),
            ]
        )
        if (
            conflicting_options.exit_code != 2
            or conflicting_options.stdout
            or "--self-test cannot be combined" not in conflicting_options.stderr
            or conflict_report_path.exists()
        ):
            print(
                "self-test failed: conflicting self-test options were not rejected: "
                f"{conflicting_options}",
                file=sys.stderr,
            )
            return 1

        unicode_error_path = cli_root / "unicode-error.json"
        unicode_temporary_files = set(cli_root.glob(".unicode-error.json.*.tmp"))
        unicode_write_error = try_write_json_report(
            unicode_error_path, "invalid surrogate: \udcff"
        )
        if (
            not isinstance(unicode_write_error, UnicodeError)
            or unicode_error_path.exists()
            or set(cli_root.glob(".unicode-error.json.*.tmp"))
            != unicode_temporary_files
        ):
            print(
                "self-test failed: Unicode write error was not cleaned up: "
                f"{unicode_write_error}",
                file=sys.stderr,
            )
            return 1

        # symlinkを作成できる環境では、最終pathの拒否と循環親の終了値も固定する。
        symlink_target_path = cli_root / "symlink-target.json"
        symlink_output_path = cli_root / "symlink-report.json"
        symlink_target_path.write_text("変更禁止", encoding="utf-8")
        try:
            symlink_output_path.symlink_to(symlink_target_path)
        except (NotImplementedError, OSError):
            pass
        else:
            symlink_result = invoke_main_for_self_test(
                [
                    "--root",
                    str(cli_root),
                    "--scope",
                    "clean",
                    "--json-output",
                    str(symlink_output_path),
                ]
            )
            if (
                symlink_result.exit_code != 2
                or symlink_result.stdout
                or "symbolic link or reparse point" not in symlink_result.stderr
                or "Traceback" in symlink_result.stderr
                or symlink_target_path.read_text(encoding="utf-8") != "変更禁止"
                or not symlink_output_path.is_symlink()
            ):
                print(
                    f"self-test failed: unsafe symlink output result: {symlink_result}",
                    file=sys.stderr,
                )
                return 1
            symlink_output_path.unlink()

        cycle_first_path = cli_root / "cycle-first"
        cycle_second_path = cli_root / "cycle-second"
        try:
            cycle_first_path.symlink_to(cycle_second_path, target_is_directory=True)
            cycle_second_path.symlink_to(cycle_first_path, target_is_directory=True)
        except (NotImplementedError, OSError):
            if cycle_first_path.is_symlink():
                cycle_first_path.unlink()
            if cycle_second_path.is_symlink():
                cycle_second_path.unlink()
        else:
            cycle_result = invoke_main_for_self_test(
                [
                    "--root",
                    str(cli_root),
                    "--scope",
                    "clean",
                    "--json-output",
                    str(cycle_first_path / "report.json"),
                ]
            )
            if (
                cycle_result.exit_code != 2
                or cycle_result.stdout
                or "report write failed" not in cycle_result.stderr
                or "Traceback" in cycle_result.stderr
            ):
                print(
                    f"self-test failed: cyclic output path was not cleanly rejected: "
                    f"{cycle_result}",
                    file=sys.stderr,
                )
                return 1
            cycle_first_path.unlink()
            cycle_second_path.unlink()

    print("ACS C++ conventions audit self-test passed")
    return 0


def parse_arguments(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="ACS repository root (default: script parent)",
    )
    parser.add_argument(
        "--scope",
        action="append",
        dest="scopes",
        help="root-relative directory to scan; may be repeated",
    )
    parser.add_argument(
        "--format",
        choices=("human", "json"),
        default=None,
        help=(
            "標準出力形式。既定のhumanは従来互換、jsonはCI向けJSONのみを出力する"
        ),
    )
    parser.add_argument(
        "--json-output",
        type=Path,
        help=(
            "CI向けJSONをUTF-8で原子的に保存する。human標準出力と併用できる"
        ),
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help=(
            "repository を走査せず、using/typedef alias の意図的な除外を含む "
            "lexer/audit とJSONレポートの回帰 fixture を実行する"
        ),
    )
    arguments = parser.parse_args(argv)
    if arguments.self_test and (
        arguments.format is not None or arguments.json_output is not None
    ):
        parser.error("--self-test cannot be combined with --format or --json-output")
    if arguments.format is None:
        arguments.format = "human"
    return arguments


def main(argv: Optional[Sequence[str]] = None) -> int:
    configure_utf8_console()
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    if arguments.self_test:
        return run_self_test()

    root = arguments.root.resolve()
    scopes = tuple(arguments.scopes or DEFAULT_SCOPES)
    files, violations = run_audit(root, scopes)
    report_text = serialize_json_report(build_json_report(root, files, violations))

    if arguments.json_output is not None:
        write_error = try_write_json_report(arguments.json_output, report_text)
        if write_error is not None:
            print(
                "ACS C++ conventions audit report write failed: "
                f"{arguments.json_output}: {write_error}",
                file=sys.stderr,
            )
            return 2

    if arguments.format == "json":
        try:
            write_json_stdout(report_text)
        except (OSError, UnicodeError, RuntimeError, ValueError) as error:
            print(
                f"ACS C++ conventions audit report write failed: stdout: {error}",
                file=sys.stderr,
            )
            return 2
    else:
        for violation in violations:
            print(violation.format(root), file=sys.stderr)

        if violations:
            print(
                f"ACS C++ conventions audit failed: {len(violations)} violation(s) "
                f"in {len(files)} file(s)",
                file=sys.stderr,
            )
        else:
            print(f"ACS C++ conventions audit passed: {len(files)} file(s)")

    return 1 if violations else 0


if __name__ == "__main__":
    raise SystemExit(main())
