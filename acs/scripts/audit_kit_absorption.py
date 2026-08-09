#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""ACS内へ独立したKit境界が再導入されていないかを検査する。"""

from __future__ import annotations

import argparse
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Callable, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
from audit_cpp_conventions import CPP_SUFFIXES, DEFAULT_SCOPES, EXCLUDED_DIRECTORY_NAMES, Token, _find_logical_line_end, _line_splice_length, _read_logical_directive, _remove_line_splices, extract_includes, lex_cpp


EXCLUDED_DIRECTORY_KEYS = frozenset(name.casefold() for name in EXCLUDED_DIRECTORY_NAMES)
KIT_HEADER_NAMES = frozenset({"kit.h"})
KIT_CPP_SUFFIXES = frozenset(set(CPP_SUFFIXES) | {".ccm", ".cppm", ".cxxm", ".mpp", ".mxx"})
ACS_MARKER_PATHS = (Path("engine/CMakeLists.txt"), Path("scripts/audit_cpp_conventions.py"))
ACS_REQUIRED_SCOPES = ("src", "tests")
RAW_STRING_START = re.compile(r'(?:u8|u|U|L)?R"([^\s()\\]{0,16})\(')
WINDOWS_REPARSE_ATTRIBUTE = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)


@dataclass(frozen=True)
class FViolation:
    """検出したKit境界の位置と理由を表す。"""

    rule: str
    path: str
    line: int
    column: int
    message: str


def _configure_utf8_console() -> None:
    """Windowsの既定文字コードに依存せず日本語診断を出力する。"""

    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="strict")


def _display_path(path: Path, root: Path) -> str:
    """symlinkを解決せず監査対象内の字句pathを返す。"""

    absolute_path = Path(os.path.abspath(os.fspath(path)))
    absolute_root = Path(os.path.abspath(os.fspath(root)))
    try:
        return absolute_path.relative_to(absolute_root).as_posix()
    except ValueError:
        return absolute_path.as_posix()


def _contains_kit_segment(parts: Sequence[str]) -> bool:
    """パスに独立したKit階層があるかを返す。"""

    return any(part.casefold() == "kit" for part in parts)


def _is_kit_include(include_path: str) -> bool:
    """Kit階層または旧Kit集約ヘッダーへのincludeかを返す。"""

    parts = tuple(part for part in include_path.replace("\\", "/").split("/") if part)
    return _contains_kit_segment(parts) or bool(parts and parts[-1].casefold() in KIT_HEADER_NAMES)


def _is_excluded_directory(name: str) -> bool:
    """生成物と外部依存のディレクトリを監査対象外にする。"""

    key = name.casefold()
    return key in EXCLUDED_DIRECTORY_KEYS or key.startswith("cmake-build-")


def _namespace_kit_positions(tokens: Sequence[Token]) -> set[tuple[int, int]]:
    """namespace名またはnamespace alias内のKit位置を返す。"""

    positions: set[tuple[int, int]] = set()
    for index, token in enumerate(tokens):
        if token.text != "namespace" or (index > 0 and tokens[index - 1].text == "using"):
            continue
        cursor = index + 1
        while cursor < len(tokens):
            current = tokens[cursor]
            if current.text in {"{", ";"}:
                break
            if current.text.casefold() == "kit":
                positions.add((current.line, current.column))
            cursor += 1
    return positions


def _using_namespace_kit_positions(tokens: Sequence[Token]) -> set[tuple[int, int]]:
    """using namespaceがKit境界を参照する位置を返す。"""

    positions: set[tuple[int, int]] = set()
    for index, token in enumerate(tokens):
        if token.text != "using" or index + 2 >= len(tokens) or tokens[index + 1].text != "namespace":
            continue
        cursor = index + 2
        while cursor < len(tokens) and tokens[cursor].text != ";":
            current = tokens[cursor]
            if current.text.casefold() == "kit":
                positions.add((current.line, current.column))
            cursor += 1
    return positions


def _module_kit_positions(tokens: Sequence[Token]) -> set[tuple[int, int]]:
    """C++ moduleの宣言またはimport先がKit境界かを返す。"""

    positions: set[tuple[int, int]] = set()
    brace_depth = 0
    for index, token in enumerate(tokens):
        if token.text == "}":
            brace_depth = max(0, brace_depth - 1)
        if token.text not in {"import", "module"} or index + 1 >= len(tokens) or brace_depth != 0:
            if token.text == "{":
                brace_depth += 1
            continue
        declaration_start = index - 1 if index > 0 and tokens[index - 1].text == "export" else index
        if declaration_start > 0 and tokens[declaration_start - 1].text not in {";", "}"}:
            continue
        cursor = index + 1
        if tokens[cursor].text in {"<", ".", ";"}:
            continue
        while cursor < len(tokens) and tokens[cursor].text not in {";", "{", "}"}:
            component = tokens[cursor]
            if component.text.casefold() == "kit":
                positions.add((component.line, component.column))
            cursor += 1
    return positions


def _skip_quoted_source(source: str, start: int, quote: str) -> int:
    """escapeを考慮して文字列または文字literalの直後を返す。"""

    cursor = start + 1
    while cursor < len(source):
        if source[cursor] == "\\":
            cursor += 2
            continue
        if source[cursor] == quote:
            return cursor + 1
        cursor += 1
    return len(source)


def _skip_module_trivia(source: str, start: int) -> int:
    """module keywordとheader名の間にある空白とcommentを飛ばす。"""

    cursor = start
    while cursor < len(source):
        if source[cursor].isspace():
            cursor += 1
            continue
        if source.startswith("//", cursor):
            end = source.find("\n", cursor + 2)
            cursor = len(source) if end < 0 else end + 1
            continue
        if source.startswith("/*", cursor):
            end = source.find("*/", cursor + 2)
            cursor = len(source) if end < 0 else end + 2
            continue
        break
    return cursor


def _mask_preprocessor_directives(source: str) -> str:
    """長さと改行を保ったまま前処理directiveを空白へ置き換える。"""

    masked = list(source)
    cursor = 0
    only_whitespace_on_line = True
    while cursor < len(source):
        splice_length = _line_splice_length(source, cursor)
        if splice_length:
            cursor += splice_length
            continue
        raw_match = RAW_STRING_START.match(source, cursor)
        if raw_match is not None:
            terminator = ")" + raw_match.group(1) + '"'
            end = source.find(terminator, raw_match.end())
            cursor = len(source) if end < 0 else end + len(terminator)
            only_whitespace_on_line = False
            continue
        if source.startswith("//", cursor):
            cursor = _find_logical_line_end(source, cursor + 2)
            continue
        if source.startswith("/*", cursor):
            end = source.find("*/", cursor + 2)
            end = len(source) if end < 0 else end + 2
            if "\n" in source[cursor:end]:
                only_whitespace_on_line = True
            cursor = end
            continue
        if source[cursor] == "\n":
            only_whitespace_on_line = True
            cursor += 1
            continue
        if source[cursor] in {'"', "'"}:
            cursor = _skip_quoted_source(source, cursor, source[cursor])
            only_whitespace_on_line = False
            continue
        if source[cursor] == "#" and only_whitespace_on_line:
            _, _, end = _read_logical_directive(source, cursor)
            for index in range(cursor, end):
                if masked[index] not in {"\r", "\n"}:
                    masked[index] = " "
            cursor = end
            continue
        if not source[cursor].isspace():
            only_whitespace_on_line = False
        cursor += 1
    return "".join(masked)


def _extract_module_header_imports(source: str) -> list[tuple[str, int, int]]:
    """commentとliteralを除外してC++ header unit importを返す。"""

    source, source_positions = _remove_line_splices(source)
    imports: list[tuple[str, int, int]] = []
    cursor = 0
    while cursor < len(source):
        raw_match = RAW_STRING_START.match(source, cursor)
        if raw_match is not None:
            terminator = ")" + raw_match.group(1) + '"'
            end = source.find(terminator, raw_match.end())
            cursor = len(source) if end < 0 else end + len(terminator)
            continue
        if source.startswith("//", cursor):
            end = source.find("\n", cursor + 2)
            cursor = len(source) if end < 0 else end + 1
            continue
        if source.startswith("/*", cursor):
            end = source.find("*/", cursor + 2)
            cursor = len(source) if end < 0 else end + 2
            continue
        if source[cursor] in {'"', "'"}:
            cursor = _skip_quoted_source(source, cursor, source[cursor])
            continue
        if source[cursor].isalpha() or source[cursor] == "_":
            end = cursor + 1
            while end < len(source) and (source[end].isalnum() or source[end] == "_"):
                end += 1
            if source[cursor:end] == "import":
                header_open = _skip_module_trivia(source, end)
                if header_open < len(source) and source[header_open] in {'"', "<"}:
                    closing = '"' if source[header_open] == '"' else ">"
                    header_start = header_open + 1
                    header_end = source.find(closing, header_start)
                    if header_end >= 0:
                        line, column = source_positions[header_start]
                        imports.append((source[header_start:header_end], line, column))
                        cursor = header_end + 1
                        continue
            cursor = end
            continue
        cursor += 1
    return imports


def _access_failure(path: Path, root: Path, error: BaseException) -> FViolation:
    """読み取り失敗を機械可読な違反へ変換する。"""

    return FViolation(
        "ACS-KIT005",
        _display_path(path, root),
        1,
        1,
        f"C++境界の読み取りまたはディレクトリ走査に失敗しました。原因種別: {type(error).__name__}",
    )


def _audit_cpp_file(path: Path, root: Path) -> list[FViolation]:
    """一つのC++ファイルから旧名前空間と依存境界を検出する。"""

    relative = _display_path(path, root)
    try:
        source = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        return [_access_failure(path, root, error)]

    violations: list[FViolation] = []
    tokens = lex_cpp(source)
    module_source = _mask_preprocessor_directives(source)
    module_tokens = lex_cpp(module_source)
    namespace_positions = _namespace_kit_positions(tokens)
    using_namespace_positions = _using_namespace_kit_positions(tokens)
    for line, column in sorted(namespace_positions):
        violations.append(FViolation("ACS-KIT003", relative, line, column, "namespace名またはnamespace aliasにKitを使わず、責務に対応するACSモジュールへ統合してください。"))
    for line, column in sorted(using_namespace_positions):
        violations.append(FViolation("ACS-KIT002", relative, line, column, "using namespaceによる旧Kit参照をACSの正規APIへ置き換えてください。"))

    for index, token in enumerate(tokens):
        position = (token.line, token.column)
        if position in namespace_positions or position in using_namespace_positions:
            continue
        if token.text.casefold() == "kit" and index + 1 < len(tokens) and tokens[index + 1].text == "::":
            violations.append(FViolation("ACS-KIT002", relative, token.line, token.column, "旧kit名前空間の参照をACSの正規APIへ置き換えてください。"))

    for line, column in sorted(_module_kit_positions(module_tokens)):
        violations.append(FViolation("ACS-KIT004", relative, line, column, "独立したKit module宣言またはimportをACSモジュールの公開APIへ置き換えてください。"))

    for include_path, line, column in extract_includes(source):
        if _is_kit_include(include_path):
            violations.append(FViolation("ACS-KIT004", relative, line, column, "独立したKit includeをACSモジュールの公開ヘッダーへ置き換えてください。"))
    for header_path, line, column in _extract_module_header_imports(module_source):
        if _is_kit_include(header_path):
            violations.append(FViolation("ACS-KIT004", relative, line, column, "独立したKit header unit importをACSモジュールの公開ヘッダーへ置き換えてください。"))
    return violations


def _reparse_directory_state(path: Path, root: Path) -> tuple[bool, FViolation | None]:
    """symlinkとWindows reparse directoryを追跡禁止として判定する。"""

    try:
        status = path.lstat()
    except OSError as error:
        return False, _access_failure(path, root, error)
    attributes = getattr(status, "st_file_attributes", 0)
    return stat.S_ISLNK(status.st_mode) or bool(attributes & WINDOWS_REPARSE_ATTRIBUTE), None


def _walk_scope(scope: Path, root: Path) -> tuple[list[Path], list[Path], list[FViolation]]:
    """除外ディレクトリを降りずにC++ファイルとKitディレクトリを返す。"""

    files: list[Path] = []
    kit_directories: list[Path] = []
    failures: list[FViolation] = []
    try:
        scope_status = scope.lstat()
    except FileNotFoundError:
        return files, kit_directories, failures
    except OSError as error:
        return files, kit_directories, [_access_failure(scope, root, error)]
    scope_attributes = getattr(scope_status, "st_file_attributes", 0)
    if stat.S_ISLNK(scope_status.st_mode) or bool(scope_attributes & WINDOWS_REPARSE_ATTRIBUTE):
        error = OSError(f"監査scopeのreparse directoryは追跡しません: {scope}")
        return files, kit_directories, [_access_failure(scope, root, error)]
    if not stat.S_ISDIR(scope_status.st_mode):
        error = NotADirectoryError(f"監査scopeがディレクトリではありません: {scope}")
        return files, kit_directories, [_access_failure(scope, root, error)]

    def record_walk_failure(error: OSError) -> None:
        failed_path = Path(os.fsdecode(error.filename)) if error.filename else scope
        failures.append(_access_failure(failed_path, root, error))

    for current_text, directory_names, file_names in os.walk(scope, topdown=True, onerror=record_walk_failure, followlinks=False):
        current = Path(current_text)
        retained_directories: list[str] = []
        for name in sorted(directory_names):
            if _is_excluded_directory(name):
                continue
            child = current / name
            if name.casefold() == "kit":
                kit_directories.append(child)
            is_reparse, failure = _reparse_directory_state(child, root)
            if failure is not None:
                failures.append(failure)
                continue
            if not is_reparse:
                retained_directories.append(name)
        directory_names[:] = retained_directories
        for file_name in sorted(file_names):
            path = current / file_name
            if path.suffix.casefold() in KIT_CPP_SUFFIXES:
                files.append(path)
    return files, kit_directories, failures


def scan_tree(root: Path) -> tuple[list[Path], list[FViolation]]:
    """ACSの標準scopeを走査し、再導入された旧Kit境界を返す。"""

    absolute_root = Path(os.path.abspath(os.fspath(root)))
    if not absolute_root.is_dir():
        raise FileNotFoundError(absolute_root)

    files: list[Path] = []
    kit_directories: list[Path] = []
    violations: list[FViolation] = []
    for scope_name in DEFAULT_SCOPES:
        scope_files, scope_kit_directories, scope_failures = _walk_scope(absolute_root / scope_name, absolute_root)
        files.extend(scope_files)
        kit_directories.extend(scope_kit_directories)
        violations.extend(scope_failures)

    violations.extend(
        FViolation("ACS-KIT001", _display_path(path, absolute_root), 1, 1, "独立したKitディレクトリを責務に対応するACSモジュールへ吸収してください。")
        for path in kit_directories
    )
    for path in files:
        violations.extend(_audit_cpp_file(path, absolute_root))
    files.sort(key=lambda path: _display_path(path, absolute_root).casefold())
    violations.sort(key=lambda item: (item.path.casefold(), item.line, item.column, item.rule))
    return files, violations


def _has_acs_root_contract(root: Path) -> bool:
    """ACS固有markerと必須scopeを持つrootかを返す。"""

    return all((root / marker).is_file() for marker in ACS_MARKER_PATHS) and all((root / scope).is_dir() for scope in ACS_REQUIRED_SCOPES)


def _scan_root(root: Path) -> tuple[list[Path], list[FViolation]]:
    """存在しないrootと誤rootを構造化違反として返す。"""

    absolute_root = Path(os.path.abspath(os.fspath(root)))
    if not absolute_root.is_dir() or not _has_acs_root_contract(absolute_root):
        return [], [FViolation("ACS-KIT000", absolute_root.as_posix(), 1, 1, "指定先はACS markerと必須scopeを持つrootではありません。")]
    try:
        return scan_tree(absolute_root)
    except (FileNotFoundError, OSError) as error:
        return [], [FViolation("ACS-KIT000", absolute_root.as_posix(), 1, 1, f"指定したACSルートを走査できませんでした。原因種別: {type(error).__name__}")]


def _report_payload(root: Path, files: Sequence[Path], violations: Sequence[FViolation]) -> dict[str, object]:
    """機械可読な監査結果を構築する。"""

    return {
        "schema_version": 3,
        "root": Path(os.path.abspath(os.fspath(root))).as_posix(),
        "status": "fail" if violations else "pass",
        "scanned_file_count": len(files),
        "violation_count": len(violations),
        "violations": [asdict(item) for item in violations],
    }


def _write_json(path: Path, payload: dict[str, object], replace_operation: Callable[[Path, Path], object] = os.replace) -> None:
    """JSONを途中状態を公開せず保存する。"""

    destination = Path(os.path.abspath(os.fspath(path)))
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n", dir=destination.parent, prefix=f".{destination.name}.", suffix=".tmp", delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(payload, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
        replace_operation(temporary, destination)
        temporary = None
    except BaseException:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
        raise


def _create_directory_reparse(link: Path, target: Path) -> bool:
    """self-test用のjunctionまたはdirectory symlinkを作る。"""

    if os.name == "nt":
        result = subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(target)], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return result.returncode == 0
    try:
        link.symlink_to(target, target_is_directory=True)
        return True
    except OSError:
        return False


def _remove_directory_reparse(path: Path) -> None:
    """self-test用のjunctionまたはdirectory symlinkだけを削除する。"""

    try:
        if os.name == "nt":
            os.rmdir(path)
        else:
            path.unlink()
    except FileNotFoundError:
        pass


def _run_self_test() -> bool:
    """scope、reparse、module、読み取り失敗、原子的JSON保存を確認する。"""

    with tempfile.TemporaryDirectory(prefix="acs-kit-absorption-audit-") as directory, tempfile.TemporaryDirectory(prefix="acs-kit-absorption-outside-") as outside_directory:
        root = Path(directory)
        outside = Path(outside_directory)
        (root / "src").mkdir()
        (root / "tests").mkdir()
        (root / "tools" / "Kit").mkdir(parents=True)
        (root / "editor" / "Kit").mkdir(parents=True)
        (root / "src" / "ThirdParty" / "Kit").mkdir(parents=True)
        (root / "src" / "Intermediate" / "Kit").mkdir(parents=True)
        (root / "src" / "cmake-build-debug" / "Kit").mkdir(parents=True)

        good_source = 'const char* Text = "kit::Ignored"; // import kit;\nconst char* Raw = R"tag(\nimport "Kit.h";\nnamespace acs::kit {}\n)tag";\nstruct FMemberValue { int kit; };\nvoid UseMemberNames() { FMemberValue module{}; FMemberValue import{}; int value = module.kit + import.kit; }\n'
        (root / "src" / "Good.cpp").write_text(good_source, encoding="utf-8")
        bad_source = r'#include "Kit\Timer.h"' + "\nnamespace acs::kit {}\nvoid Use() { kit::Timer value; }\n"
        (root / "src" / "Bad.cpp").write_text(bad_source, encoding="utf-8")
        (root / "src" / "BadHeader.cpp").write_text('#include "Kit.h"\n', encoding="utf-8")
        (root / "src" / "Module.ixx").write_text("export module kit.audit;\nimport kit;\nexport import kit.foo;\n", encoding="utf-8")
        module_source = 'export module acs.kit.bridge;\nimport acs.kit;\nimport "Kit.h";\nimport <Kit/Timer.h>;\n'
        (root / "src" / "Module.cppm").write_text(module_source, encoding="utf-8")
        splice_source = 'export module fixture.splice;\nimpor\\\nt "Kit.h";\nimport "Ki\\\nt.h";\n// ignored \\\nimport "Kit.h";\n'
        (root / "src" / "Splice.cppm").write_text(splice_source, encoding="utf-8")
        fragment_source = 'module;\n#define ACS_FRAGMENT 1\n#define ACS_FAKE export module kit.fake\n#include "Foundation/Config.h"\nexport module kit.fragment;\n'
        (root / "src" / "GlobalFragment.cppm").write_text(fragment_source, encoding="utf-8")
        (root / "src" / "Unreadable.cpp").write_bytes(b"\xff\xfe\x00")
        (root / "editor" / "Alias.cpp").write_text("namespace Legacy = kit;\nusing namespace acs::kit;\n", encoding="utf-8")
        (root / "editor" / "Kit" / "Module.cmake").write_text("# C++ファイルがなくてもディレクトリを検出する。\n", encoding="utf-8")
        (root / "tools" / "Kit" / "Tool.cpp").write_text("namespace acs {}\n", encoding="utf-8")
        ignored_source = '#include "Kit.h"\nnamespace kit {}\n'
        for excluded in ("ThirdParty", "Intermediate", "cmake-build-debug"):
            (root / "src" / excluded / "Kit" / "Ignored.cpp").write_text(ignored_source, encoding="utf-8")

        (outside / "Kit").mkdir()
        (outside / "Escaped.cpp").write_text("void Escape() { kit::Timer value; }\n", encoding="utf-8")
        kit_reparse = root / "tests" / "Kit"
        ancestor_reparse = root / "src" / "ExternalBridge"
        if not _create_directory_reparse(kit_reparse, outside) or not _create_directory_reparse(ancestor_reparse, outside):
            _remove_directory_reparse(kit_reparse)
            _remove_directory_reparse(ancestor_reparse)
            print("kit_absorption_audit_self_test failed: directory reparse fixture could not be created", file=sys.stderr)
            return False

        try:
            files, violations = scan_tree(root)
        finally:
            _remove_directory_reparse(kit_reparse)
            _remove_directory_reparse(ancestor_reparse)

        rule_counts = {rule: sum(item.rule == rule for item in violations) for rule in ("ACS-KIT001", "ACS-KIT002", "ACS-KIT003", "ACS-KIT004", "ACS-KIT005")}
        expected_counts = {"ACS-KIT001": 3, "ACS-KIT002": 2, "ACS-KIT003": 2, "ACS-KIT004": 12, "ACS-KIT005": 1}
        if rule_counts != expected_counts or len(files) != 10:
            print(f"kit_absorption_audit_self_test failed: counts={rule_counts} files={len(files)}", file=sys.stderr)
            return False
        splice_positions = {(item.line, item.column) for item in violations if item.path == "src/Splice.cppm" and item.rule == "ACS-KIT004"}
        if splice_positions != {(3, 4), (4, 9)}:
            print(f"kit_absorption_audit_self_test failed: phase-2 splice positions={sorted(splice_positions)}", file=sys.stderr)
            return False
        fragment_positions = {(item.line, item.column) for item in violations if item.path == "src/GlobalFragment.cppm" and item.rule == "ACS-KIT004"}
        if fragment_positions != {(5, 15)}:
            print(f"kit_absorption_audit_self_test failed: global module fragment positions={sorted(fragment_positions)}", file=sys.stderr)
            return False
        kit_paths = {item.path for item in violations if item.rule == "ACS-KIT001"}
        if "tests/Kit" not in kit_paths or any("ExternalBridge" in item.path or outside.as_posix() in item.path for item in violations):
            print(f"kit_absorption_audit_self_test failed: lexical reparse path or pruning was lost: paths={sorted(kit_paths)}", file=sys.stderr)
            return False
        if any(any(name in item.path for name in ("ThirdParty", "Intermediate", "cmake-build-debug")) for item in violations):
            print("kit_absorption_audit_self_test failed: excluded directory was scanned", file=sys.stderr)
            return False

        payload = _report_payload(root, files, violations)
        json_path = root / "Saved" / "kit-absorption.json"
        _write_json(json_path, payload)
        restored = json.loads(json_path.read_text(encoding="utf-8"))
        if restored["violation_count"] != sum(expected_counts.values()) or not any(item["rule"] == "ACS-KIT005" for item in restored["violations"]):
            print("kit_absorption_audit_self_test failed: structured read failure was not preserved", file=sys.stderr)
            return False

        stable_json = json_path.read_bytes()
        try:
            _write_json(json_path, {"invalid": object()})
        except TypeError:
            pass
        else:
            print("kit_absorption_audit_self_test failed: JSON dump failure was accepted", file=sys.stderr)
            return False

        def reject_replace(source: Path, destination: Path) -> object:
            raise PermissionError(f"replace rejected: {source} -> {destination}")

        try:
            _write_json(json_path, payload, reject_replace)
        except PermissionError:
            pass
        else:
            print("kit_absorption_audit_self_test failed: JSON replace failure was accepted", file=sys.stderr)
            return False
        temporary_files = tuple(json_path.parent.glob(f".{json_path.name}.*.tmp"))
        if json_path.read_bytes() != stable_json or temporary_files:
            print(f"kit_absorption_audit_self_test failed: atomic JSON destination changed or temporary files remain: temp={len(temporary_files)}", file=sys.stderr)
            return False

        missing_files, missing_violations = _scan_root(root / "missing")
        empty_root = root / "empty-root"
        empty_root.mkdir()
        empty_files, empty_violations = _scan_root(empty_root)
        wrong_root = root / "wrong-root"
        (wrong_root / "src").mkdir(parents=True)
        (wrong_root / "tests").mkdir()
        wrong_files, wrong_violations = _scan_root(wrong_root)
        root_failures = (missing_files, missing_violations, empty_files, empty_violations, wrong_files, wrong_violations)
        if any(root_failures[index] for index in (0, 2, 4)) or any(len(root_failures[index]) != 1 or root_failures[index][0].rule != "ACS-KIT000" for index in (1, 3, 5)):
            print("kit_absorption_audit_self_test failed: missing, empty, or wrong root was accepted", file=sys.stderr)
            return False

    print(f"kit_absorption_audit_self_test=ok files=10 violations={sum(expected_counts.values())} reparse=2")
    return True


def main() -> int:
    """引数に従ってACS内部のKit境界を監査する。"""

    _configure_utf8_console()
    parser = argparse.ArgumentParser(description="ACS内のC++境界と独立Kitディレクトリに旧Kit依存が再導入されていないかを検証します。")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1], help="ACSツリーのルート")
    parser.add_argument("--format", choices=("human", "json"), default="human", help="標準出力の形式")
    parser.add_argument("--json-output", type=Path, help="機械可読な結果の保存先")
    parser.add_argument("--self-test", action="store_true", help="一時fixtureで監査器を検証")
    args = parser.parse_args()
    if args.self_test:
        return 0 if _run_self_test() else 1

    absolute_root = Path(os.path.abspath(os.fspath(args.root)))
    files, violations = _scan_root(absolute_root)
    payload = _report_payload(absolute_root, files, violations)
    if args.json_output is not None:
        _write_json(args.json_output, payload)
    if args.format == "json":
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    else:
        for item in violations:
            print(f"{item.path}:{item.line}:{item.column}: error: [{item.rule}] {item.message}")
        print(f"ACS Kit absorption audit: files={len(files)} violations={len(violations)}")
    return 1 if violations else 0


if __name__ == "__main__":
    raise SystemExit(main())
