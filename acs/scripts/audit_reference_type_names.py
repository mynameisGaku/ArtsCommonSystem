#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""手書き API リファレンスの型名を現行 C++ 宣言と照合する。

``docs/reference/data/*.js`` の ``name`` / ``kind`` / ``header`` を読み、クラス・構造体・
列挙・インターフェース・テンプレートとして掲載された名前が ``src`` の実宣言と一致するか
確認する。型役割監査が指定するhard canonicalは正規名、kind、headerを固定し、
登録済み互換名の独立掲載を拒否する。delegate、callback、関数、macro、および
未登録のalias分類は意図的に対象外とする。
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
from audit_cpp_type_roles import (
    CANONICAL_OBJECT_AND_CLASS_TYPES,
    CANONICAL_SCALAR_ALIASES,
    LEGACY_COMPATIBILITY_ALIASES,
)


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
ALIAS_KIND_MARKER = "型エイリアス"
KNOWN_PREFIXES = "CFAEIT"
NAME_PATTERN = re.compile(r'\bname\s*:\s*"((?:\\.|[^"\\])*)"')
KIND_PATTERN = re.compile(r'\bkind\s*:\s*"((?:\\.|[^"\\])*)"')
HEADER_PATTERN = re.compile(r'\bheader\s*:\s*"((?:\\.|[^"\\])*)"')
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

# 型役割監査の正規契約からreference掲載先を導出する。
CANONICAL_SCALAR_REFERENCE_ENTRIES = {
    qualified_name.rsplit("::", 1)[-1]: (
        header.split("/", 1)[0] + ".js",
        header,
    )
    for qualified_name, (header, _) in CANONICAL_SCALAR_ALIASES.items()
}

# 管理objectと機能classの正規契約からreference掲載先を導出する。
CANONICAL_OBJECT_AND_CLASS_REFERENCE_ENTRIES = {
    qualified_name.rsplit("::", 1)[-1]: (
        header.split("/", 1)[0] + ".js",
        header,
    )
    for qualified_name, (header, kind, _) in CANONICAL_OBJECT_AND_CLASS_TYPES.items()
    if kind == "class"
}

# 全hard canonicalをkindとともに一意に検査する。
CANONICAL_REFERENCE_ENTRIES = {
    **{
        name: (expected_file, expected_header, "クラス")
        for name, (expected_file, expected_header) in (
            CANONICAL_OBJECT_AND_CLASS_REFERENCE_ENTRIES.items()
        )
    },
    **{
        name: (expected_file, expected_header, ALIAS_KIND_MARKER)
        for name, (expected_file, expected_header) in (
            CANONICAL_SCALAR_REFERENCE_ENTRIES.items()
        )
    },
}

# 旧名は対応する正規entry内の互換別名としてだけ掲載する。
LEGACY_REFERENCE_ALIASES = {
    legacy_name.rsplit("::", 1)[-1]: canonical_name.rsplit("::", 1)[-1]
    for legacy_name, canonical_name in LEGACY_COMPATIBILITY_ALIASES.items()
}

# 型役割監査に登録した互換名は独立entryへ掲載しない。
LEGACY_ALIAS_REFERENCE_NAMES = frozenset(
    qualified_name.rsplit("::", 1)[-1]
    for qualified_name in LEGACY_COMPATIBILITY_ALIASES
)

# 基盤最適化で追加した公開型のうち、参照欠落を継続監視する範囲。
REQUIRED_FOUNDATION_REFERENCE_TYPES = frozenset(
    {
        "EFileExtensionKind",
        "EFormatAspect",
        "EMessagePipePolicy",
        "ERhiPipelineBindDomain",
        "ETimerSchedulePolicy",
        "FAcpakReadDiagnostics",
        "FAssetPackReadRequest",
        "FAssetPathInterner",
        "FAssetPathInternerDiagnostics",
        "FAssetRegistryDiagnostics",
        "FFileSystemDiagnostics",
        "FFormatTraits",
        "FHierarchyVisibilityBatch",
        "FHierarchyWorldTransformBatch",
        "FInternedAssetPath",
        "FJobGraphDiagnostics",
        "FPipelineStateKey",
        "FRenderGraphAliasAssignment",
        "FRenderGraphAliasPlanSummary",
        "FRenderGraphResourceLifetime",
        "FRenderGraphTransientAliasPlanner",
        "FShaderParameterLayoutMetadata",
        "FStableStringKey",
        "FStringHasher",
        "FThreadPoolDiagnostics",
        "FTimerDiagnostics",
        "FTransformSoAInput",
        "TDescriptorSlotPool",
        "TInlineArray",
        "TPipelineStateKeyCache",
        "TRhiPipelineBindPolicy",
        "TTypedPoolAllocator",
    }
)


@dataclass(frozen=True)
class FReferenceEntry:
    """リファレンスに掲載された型候補。"""

    path: Path
    line: int
    display_name: str
    kind: str
    header: str
    body: str


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


@dataclass(frozen=True)
class FReferenceContractViolation:
    """hard canonicalと互換名のreference契約違反。"""

    tag: str
    name: str
    detail: str

    def format(self) -> str:
        """機械判定できるtag付き診断を返す。"""

        return f"[{self.tag}] {self.name}: {self.detail}"


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


def collect_js_entry_fields(source: str) -> list[tuple[int, str, str, str, str]]:
    """同じJS objectが直接持つname、kind、header、本文を収集する。"""

    objects: list[dict[str, object]] = []
    stack: list[dict[str, object]] = []
    position = 0
    line = 1
    while position < len(source):
        if source.startswith("//", position):
            newline = source.find("\n", position + 2)
            if newline < 0:
                break
            position = newline
            continue
        if source.startswith("/*", position):
            end = source.find("*/", position + 2)
            if end < 0:
                break
            line += source.count("\n", position, end + 2)
            position = end + 2
            continue

        character = source[position]
        if character in {'"', "'", "`"}:
            quote = character
            position += 1
            while position < len(source):
                if source[position] == "\\":
                    position += 2
                    continue
                if source[position] == quote:
                    position += 1
                    break
                if source[position] == "\n":
                    line += 1
                position += 1
            continue
        if character == "\n":
            line += 1
            position += 1
            continue
        if character == "{":
            stack.append(
                {
                    "line": 0,
                    "name": "",
                    "kind": "",
                    "header": "",
                    "start": position,
                    "body": "",
                }
            )
            position += 1
            continue
        if character == "}":
            if stack:
                closed = stack.pop()
                if closed["name"]:
                    closed["body"] = source[int(closed["start"]) : position + 1]
                    objects.append(closed)
            position += 1
            continue

        matched = False
        if stack:
            for field, pattern in (
                ("name", NAME_PATTERN),
                ("kind", KIND_PATTERN),
                ("header", HEADER_PATTERN),
            ):
                match = pattern.match(source, position)
                if match is None:
                    continue
                stack[-1][field] = decode_js_string(match.group(1))
                if field == "name":
                    stack[-1]["line"] = line
                line += source.count("\n", position, match.end())
                position = match.end()
                matched = True
                break
        if not matched:
            position += 1

    return sorted(
        (
            (
                int(item["line"]),
                str(item["name"]),
                str(item["kind"]),
                str(item["header"]),
                str(item["body"]),
            )
            for item in objects
        ),
        key=lambda item: item[0],
    )


def collect_reference_entries(data_root: Path) -> list[FReferenceEntry]:
    """手書き data file から型掲載 entry を収集する。"""

    entries: list[FReferenceEntry] = []
    for path in sorted(data_root.glob("*.js"), key=lambda item: item.name.casefold()):
        if path.name == "_meta.js":
            continue
        source = path.read_text(encoding="utf-8")
        for line, display_name, kind, header, body in collect_js_entry_fields(source):
            entries.append(
                FReferenceEntry(
                    path=path,
                    line=line,
                    display_name=display_name,
                    kind=kind,
                    header=header,
                    body=body,
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


def entry_display_identifiers(entry: FReferenceEntry) -> Iterable[str]:
    """entry名に直接書かれた識別子をkindや関数表記に依存せず返す。"""

    for part in entry.display_name.split("/"):
        match = IDENTIFIER_PATTERN.match(part.strip())
        if match is not None:
            yield match.group(0)


def candidate_names(name: str, declared: set[str]) -> list[str]:
    """prefix の追加・置換だけで到達する現行宣言候補を返す。"""

    # Cから始まる未接頭辞名もあるため、元名と接頭辞除去名の両方を候補化する。
    suffixes = {name}
    if (
        len(name) >= 2
        and name[0] in KNOWN_PREFIXES
        and ("A" <= name[1] <= "Z" or name[1].isdigit())
    ):
        suffixes = {name[1:]}
    exact_candidates = {
        prefix + suffix for prefix in KNOWN_PREFIXES for suffix in suffixes
    }
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
        if ALIAS_KIND_MARKER in entry.kind or not any(
            marker in entry.kind for marker in TYPE_KIND_MARKERS
        ):
            continue
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


def audit_canonical_type_references(
    entries: Sequence[FReferenceEntry],
) -> list[FReferenceContractViolation]:
    """hard canonicalの掲載と互換名の独立掲載禁止を検証する。"""

    violations: list[FReferenceContractViolation] = []
    for name, (expected_file, expected_header, expected_kind) in sorted(
        CANONICAL_REFERENCE_ENTRIES.items()
    ):
        # 診断を既存scalar契約とobject/class契約で区別する。
        tag_prefix = (
            "reference-scalar-alias"
            if expected_kind == ALIAS_KIND_MARKER
            else "reference-object-class"
        )
        matches = [entry for entry in entries if entry.display_name == name]
        if not matches:
            violations.append(
                FReferenceContractViolation(
                    f"{tag_prefix}-missing",
                    name,
                    "正規名の独立entryがありません。",
                )
            )
            continue
        if len(matches) > 1:
            violations.append(
                FReferenceContractViolation(
                    f"{tag_prefix}-duplicate",
                    name,
                    f"正規名の独立entryが重複しています: count={len(matches)}",
                )
            )
            continue
        entry = matches[0]
        if entry.path.name != expected_file:
            violations.append(
                FReferenceContractViolation(
                    f"{tag_prefix}-file",
                    name,
                    f"expected={expected_file}, actual={entry.path.name}",
                )
            )
        if entry.header != expected_header:
            violations.append(
                FReferenceContractViolation(
                    f"{tag_prefix}-header",
                    name,
                    f"expected={expected_header}, actual={entry.header or '<missing>'}",
                )
            )
        if entry.kind != expected_kind:
            violations.append(
                FReferenceContractViolation(
                    f"{tag_prefix}-kind",
                    name,
                    f"expected={expected_kind}, actual={entry.kind or '<missing>'}",
                )
            )

    for entry in entries:
        for name in entry_display_identifiers(entry):
            if name not in LEGACY_ALIAS_REFERENCE_NAMES:
                continue
            violations.append(
                FReferenceContractViolation(
                    "reference-legacy-alias-entry",
                    name,
                    "正規entryの本文で互換名として説明してください。",
                )
            )
    return sorted(violations, key=lambda item: (item.tag, item.name, item.detail))


def audit_legacy_alias_reference_tokens(
    data_root: Path,
    entries: Sequence[FReferenceEntry],
) -> list[FReferenceContractViolation]:
    """旧名が正規entry内のexact using以外へ再流入していないか検証する。"""

    # 全reference本文を対象にし、sample、crossref、glossaryも監査する。
    sources = {
        path: path.read_text(encoding="utf-8")
        for path in sorted(data_root.glob("*.js"), key=lambda item: item.name.casefold())
    }
    violations: list[FReferenceContractViolation] = []
    for legacy_name, canonical_name in sorted(LEGACY_REFERENCE_ALIASES.items()):
        exact_alias = f"using {legacy_name} = {canonical_name}"
        token_pattern = re.compile(rf"\b{re.escape(legacy_name)}\b")
        # 保持するheader名の旧tokenはAPI構成上の正当な例外として除外する。
        expected_header = CANONICAL_REFERENCE_ENTRIES[canonical_name][1]
        normalized_sources = {
            path: source.replace(expected_header, "")
            for path, source in sources.items()
        }
        token_count = sum(
            len(token_pattern.findall(source))
            for source in normalized_sources.values()
        )
        canonical_entries = [
            entry for entry in entries if entry.display_name == canonical_name
        ]
        alias_count = sum(
            entry.body.count(exact_alias) for entry in canonical_entries
        )
        if token_count != 1 or alias_count != 1:
            violations.append(
                FReferenceContractViolation(
                    "reference-legacy-alias-token",
                    legacy_name,
                    (
                        f"expected={exact_alias}, token_count={token_count}, "
                        f"canonical_entry_count={alias_count}"
                    ),
                )
            )
    return sorted(violations, key=lambda item: (item.tag, item.name, item.detail))


def audit_required_references(
    required: frozenset[str],
    declared: set[str],
    entries: Sequence[FReferenceEntry],
) -> tuple[list[str], list[str]]:
    """限定した公開型の宣言消失と参照欠落を決定的順序で返す。"""

    referenced = {
        name
        for entry in entries
        for name in entry_identifiers(entry)
    }
    undeclared = sorted(required - declared)
    undocumented = sorted((required & declared) - referenced)
    return undeclared, undocumented


def run_self_test() -> int:
    """旧名検出、正名通過、hard canonical・互換alias契約を検証する。"""

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

        def reference_entry(
            name: str, kind: str, header: str, body: str = ""
        ) -> str:
            """一つのreference objectを作る。"""

            body_text = f",\n{body}" if body else ""
            return (
                "{\n"
                f'  name: "{name}", kind: "{kind}", header: "{header}"'
                f"{body_text}\n"
                "},\n"
            )

        data_path = data_root / "sample.js"
        data_path.write_text(
            reference_entry("World", "クラス", "World.h")
            + reference_entry("CpuFeatures / Cpu()", "構造体・関数", "Cpu.h")
            + reference_entry("Mode", "列挙", "Mode.h")
            + reference_entry(
                "CompletionCallback", "型エイリアス", "Callback.h"
            ),
            encoding="utf-8",
        )

        declared = collect_declared_types(source_root)
        if "TResult" not in declared:
            print(
                "reference audit did not recognize an attributed class declaration",
                file=sys.stderr,
            )
            return 1
        candidate_cases = (
            ("ImGuiCtx", {"FImGuiCtx"}, ["FImGuiCtx"]),
            ("TestCase", {"FTestCase"}, ["FTestCase"]),
            ("FWorld", {"CWorld", "FFWorld"}, ["CWorld"]),
        )
        for old_name, candidate_declarations, expected_candidates in candidate_cases:
            actual_candidates = candidate_names(old_name, candidate_declarations)
            if actual_candidates != expected_candidates:
                print(
                    "reference candidate-name self-test failed: "
                    f"name={old_name}, expected={expected_candidates!r}, "
                    f"actual={actual_candidates!r}",
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
            reference_entry("FWorld", "クラス", "World.h")
            + reference_entry("FCpuFeatures / Cpu()", "構造体・関数", "Cpu.h")
            + reference_entry("EMode", "列挙", "Mode.h"),
            encoding="utf-8",
        )
        corrected = audit(
            collect_declared_types(source_root),
            collect_reference_entries(data_root),
        )
        if corrected:
            print("reference audit corrected fixture did not pass", file=sys.stderr)
            return 1

        required = frozenset({"FWorld", "FCpuFeatures", "TResult", "FMissing"})
        undeclared, undocumented = audit_required_references(
            required,
            collect_declared_types(source_root),
            collect_reference_entries(data_root),
        )
        if undeclared != ["FMissing"] or undocumented != ["TResult"]:
            print(
                "reference completeness self-test failed: "
                f"undeclared={undeclared!r}, undocumented={undocumented!r}",
                file=sys.stderr,
            )
            return 1

        event_path = data_root / "event.js"
        ecs_path = data_root / "ecs.js"
        audio_path = data_root / "audio.js"
        memory_path = data_root / "memory.js"
        scripting_path = data_root / "scripting.js"
        baseline_audio = reference_entry(
            "CAudioEngine",
            "クラス",
            "audio/AudioEngine.h",
            '  members: [{ sig: "using FAudioEngine = CAudioEngine" }]',
        )
        baseline_memory = reference_entry(
            "AObject",
            "クラス",
            "memory/AObject.h",
            '  members: [{ sig: "using FObject = AObject" }]',
        )
        baseline_scripting = reference_entry(
            "CLuaVm",
            "クラス",
            "scripting/LuaVm.h",
            '  members: [{ sig: "using FLuaVm = CLuaVm" }]',
        )
        baseline_event_classes = (
            reference_entry(
                "CMessageBroker",
                "クラス",
                "event/MessageBroker.h",
                '  members: [{ sig: "using FMessageBroker = CMessageBroker" }]',
            )
            + reference_entry(
                "CTimerManager",
                "クラス",
                "event/TimerManager.h",
                '  members: [{ sig: "using FTimerManager = CTimerManager" }]',
            )
        )
        baseline_event = (
            reference_entry(
                "FEventTypeId",
                "型エイリアス",
                "event/EventTypeId.h",
                '  members: [{ sig: "using EventTypeId = FEventTypeId" }]',
            )
            + reference_entry(
                "EventTypeIdCallback",
                "型エイリアス",
                "event/EventTypeId.h",
            )
        )
        baseline_ecs = (
            reference_entry(
                "FComponentTypeId",
                "型エイリアス",
                "ecs/ComponentId.h",
                '  members: [{ sig: "using ComponentTypeId = FComponentTypeId" }]',
            )
            + reference_entry(
                "FComponentSignatureId",
                "型エイリアス",
                "ecs/ComponentId.h",
                '  members: [{ sig: "using ComponentSignatureId = '
                'FComponentSignatureId" }]',
            )
            + reference_entry("AssetType", "型エイリアス", "asset/Asset.h")
            + reference_entry("TRc&lt;T&gt;", "型エイリアス", "memory/Rc.h")
        )

        def check_contract(
            event_source: str, ecs_source: str
        ) -> tuple[tuple[str, str, str], ...]:
            """指定したreference fixtureのhard canonical契約違反を返す。"""

            audio_path.write_text(baseline_audio, encoding="utf-8")
            memory_path.write_text(baseline_memory, encoding="utf-8")
            scripting_path.write_text(baseline_scripting, encoding="utf-8")
            event_path.write_text(
                baseline_event_classes + event_source,
                encoding="utf-8",
            )
            ecs_path.write_text(ecs_source, encoding="utf-8")
            return tuple(
                (violation.tag, violation.name, violation.detail)
                for violation in audit_canonical_type_references(
                    collect_reference_entries(data_root)
                )
            )

        if check_contract(baseline_event, baseline_ecs):
            print("reference canonical type baseline did not pass", file=sys.stderr)
            return 1

        mutations: tuple[
            tuple[str, str, str, tuple[tuple[str, str, str], ...]], ...
        ] = (
            (
                "missing canonical",
                baseline_event,
                baseline_ecs.replace(
                    reference_entry(
                        "FComponentSignatureId",
                        "型エイリアス",
                        "ecs/ComponentId.h",
                        '  members: [{ sig: "using ComponentSignatureId = '
                        'FComponentSignatureId" }]',
                    ),
                    "",
                ),
                (
                    (
                        "reference-scalar-alias-missing",
                        "FComponentSignatureId",
                        "正規名の独立entryがありません。",
                    ),
                ),
            ),
            (
                "wrong header",
                baseline_event.replace(
                    "event/EventTypeId.h", "event/MessageBroker.h", 1
                ),
                baseline_ecs,
                (
                    (
                        "reference-scalar-alias-header",
                        "FEventTypeId",
                        "expected=event/EventTypeId.h, actual=event/MessageBroker.h",
                    ),
                ),
            ),
            (
                "duplicate canonical",
                baseline_event
                + reference_entry(
                    "FEventTypeId", "型エイリアス", "event/EventTypeId.h"
                ),
                baseline_ecs,
                (
                    (
                        "reference-scalar-alias-duplicate",
                        "FEventTypeId",
                        "正規名の独立entryが重複しています: count=2",
                    ),
                ),
            ),
            (
                "legacy composite",
                baseline_event,
                baseline_ecs
                + reference_entry(
                    "ComponentTypeId / GetComponentTypeId&lt;T&gt;()",
                    "型エイリアス / 関数テンプレート",
                    "ecs/ComponentId.h",
                ),
                (
                    (
                        "reference-legacy-alias-entry",
                        "ComponentTypeId",
                        "正規entryの本文で互換名として説明してください。",
                    ),
                ),
            ),
            (
                "entry-local header",
                baseline_event.replace(
                    ', header: "event/EventTypeId.h"', "", 1
                ),
                baseline_ecs,
                (
                    (
                        "reference-scalar-alias-header",
                        "FEventTypeId",
                        "expected=event/EventTypeId.h, actual=<missing>",
                    ),
                ),
            ),
            (
                "wrong data file",
                baseline_event.replace(
                    reference_entry(
                        "FEventTypeId",
                        "型エイリアス",
                        "event/EventTypeId.h",
                        '  members: [{ sig: "using EventTypeId = FEventTypeId" }]',
                    ),
                    "",
                ),
                baseline_ecs
                + reference_entry(
                    "FEventTypeId", "型エイリアス", "event/EventTypeId.h"
                ),
                (
                    (
                        "reference-scalar-alias-file",
                        "FEventTypeId",
                        "expected=event.js, actual=ecs.js",
                    ),
                ),
            ),
            (
                "wrong kind",
                baseline_event.replace("型エイリアス", "関数", 1),
                baseline_ecs,
                (
                    (
                        "reference-scalar-alias-kind",
                        "FEventTypeId",
                        "expected=型エイリアス, actual=関数",
                    ),
                ),
            ),
            (
                "duplicate legacy diagnostics",
                baseline_event
                + reference_entry("EventTypeId", "関数", "compat/Legacy.h")
                + reference_entry("EventTypeId", "関数", "compat/Legacy.h"),
                baseline_ecs,
                (
                    (
                        "reference-legacy-alias-entry",
                        "EventTypeId",
                        "正規entryの本文で互換名として説明してください。",
                    ),
                    (
                        "reference-legacy-alias-entry",
                        "EventTypeId",
                        "正規entryの本文で互換名として説明してください。",
                    ),
                ),
            ),
            (
                "ordered distinct diagnostics",
                baseline_event
                + reference_entry("EventTypeId", "関数", "compat/Legacy.h"),
                baseline_ecs.replace(
                    reference_entry(
                        "FComponentSignatureId",
                        "型エイリアス",
                        "ecs/ComponentId.h",
                        '  members: [{ sig: "using ComponentSignatureId = '
                        'FComponentSignatureId" }]',
                    ),
                    "",
                ),
                (
                    (
                        "reference-legacy-alias-entry",
                        "EventTypeId",
                        "正規entryの本文で互換名として説明してください。",
                    ),
                    (
                        "reference-scalar-alias-missing",
                        "FComponentSignatureId",
                        "正規名の独立entryがありません。",
                    ),
                ),
            ),
        )
        for label, event_source, ecs_source, expected_violations in mutations:
            actual_violations = check_contract(event_source, ecs_source)
            if actual_violations != expected_violations:
                print(
                    f"reference canonical type mutation failed ({label}): "
                    f"expected={expected_violations!r}, "
                    f"actual={actual_violations!r}",
                    file=sys.stderr,
                )
                return 1

        for canonical_name in sorted(CANONICAL_SCALAR_REFERENCE_ENTRIES):
            for invalid_kind in ("型エイリアス / 関数", "偽型エイリアス"):
                old_field = (
                    f'name: "{canonical_name}", kind: "型エイリアス"'
                )
                new_field = f'name: "{canonical_name}", kind: "{invalid_kind}"'
                event_source = baseline_event
                ecs_source = baseline_ecs
                if canonical_name == "FEventTypeId":
                    event_source = event_source.replace(old_field, new_field, 1)
                else:
                    ecs_source = ecs_source.replace(old_field, new_field, 1)
                actual_violations = check_contract(event_source, ecs_source)
                expected_violations = (
                    (
                        "reference-scalar-alias-kind",
                        canonical_name,
                        f"expected=型エイリアス, actual={invalid_kind}",
                    ),
                )
                if actual_violations != expected_violations:
                    print(
                        "reference scalar alias kind mutation failed: "
                        f"name={canonical_name}, kind={invalid_kind}, "
                        f"expected={expected_violations!r}, "
                        f"actual={actual_violations!r}",
                        file=sys.stderr,
                    )
                    return 1

        hard_fixtures = (
            (
                "AObject",
                memory_path,
                baseline_memory,
                "memory/AObject.h",
            ),
            (
                "CAudioEngine",
                audio_path,
                baseline_audio,
                "audio/AudioEngine.h",
            ),
            (
                "CLuaVm",
                scripting_path,
                baseline_scripting,
                "scripting/LuaVm.h",
            ),
            (
                "CMessageBroker",
                event_path,
                reference_entry(
                    "CMessageBroker",
                    "クラス",
                    "event/MessageBroker.h",
                    '  members: [{ sig: "using FMessageBroker = CMessageBroker" }]',
                ),
                "event/MessageBroker.h",
            ),
            (
                "CTimerManager",
                event_path,
                reference_entry(
                    "CTimerManager",
                    "クラス",
                    "event/TimerManager.h",
                    '  members: [{ sig: "using FTimerManager = CTimerManager" }]',
                ),
                "event/TimerManager.h",
            ),
        )
        for canonical_name, canonical_path, canonical_entry, expected_header in hard_fixtures:
            # 各mutationの前に正規fixtureへ戻す。
            if check_contract(baseline_event, baseline_ecs):
                print("reference object/class fixture restore failed", file=sys.stderr)
                return 1
            canonical_source = canonical_path.read_text(encoding="utf-8")
            wrong_header = expected_header.replace(".h", "Wrong.h")
            mutations_for_type = (
                (
                    "missing",
                    canonical_source.replace(canonical_entry, "", 1),
                    canonical_path,
                    (
                        "reference-object-class-missing",
                        canonical_name,
                        "正規名の独立entryがありません。",
                    ),
                ),
                (
                    "duplicate",
                    canonical_source + canonical_entry,
                    canonical_path,
                    (
                        "reference-object-class-duplicate",
                        canonical_name,
                        "正規名の独立entryが重複しています: count=2",
                    ),
                ),
                (
                    "wrong header",
                    canonical_source.replace(expected_header, wrong_header, 1),
                    canonical_path,
                    (
                        "reference-object-class-header",
                        canonical_name,
                        f"expected={expected_header}, actual={wrong_header}",
                    ),
                ),
                (
                    "wrong kind",
                    canonical_source.replace(
                        canonical_entry,
                        canonical_entry.replace(
                            'kind: "クラス"', 'kind: "構造体"', 1
                        ),
                        1,
                    ),
                    canonical_path,
                    (
                        "reference-object-class-kind",
                        canonical_name,
                        "expected=クラス, actual=構造体",
                    ),
                ),
            )
            for label, mutated_source, mutated_path, expected_violation in mutations_for_type:
                mutated_path.write_text(mutated_source, encoding="utf-8")
                actual_violations = tuple(
                    (violation.tag, violation.name, violation.detail)
                    for violation in audit_canonical_type_references(
                        collect_reference_entries(data_root)
                    )
                )
                if actual_violations != (expected_violation,):
                    print(
                        "reference object/class mutation failed: "
                        f"name={canonical_name}, mutation={label}, "
                        f"expected={(expected_violation,)!r}, "
                        f"actual={actual_violations!r}",
                        file=sys.stderr,
                    )
                    return 1
                if check_contract(baseline_event, baseline_ecs):
                    print("reference object/class fixture restore failed", file=sys.stderr)
                    return 1

            # 正規entryを別fileへ移してfile契約を検査する。
            canonical_source = canonical_path.read_text(encoding="utf-8")
            canonical_path.write_text(
                canonical_source.replace(canonical_entry, "", 1),
                encoding="utf-8",
            )
            ecs_path.write_text(
                baseline_ecs + canonical_entry,
                encoding="utf-8",
            )
            actual_file_violations = tuple(
                (violation.tag, violation.name, violation.detail)
                for violation in audit_canonical_type_references(
                    collect_reference_entries(data_root)
                )
            )
            expected_file_violation = (
                (
                    "reference-object-class-file",
                    canonical_name,
                    (
                        "expected="
                        f"{CANONICAL_OBJECT_AND_CLASS_REFERENCE_ENTRIES[canonical_name][0]}, "
                        "actual=ecs.js"
                    ),
                ),
            )
            if actual_file_violations != expected_file_violation:
                print(
                    "reference object/class file mutation failed: "
                    f"name={canonical_name}, expected={expected_file_violation!r}, "
                    f"actual={actual_file_violations!r}",
                    file=sys.stderr,
                )
                return 1
            if check_contract(baseline_event, baseline_ecs):
                print("reference object/class fixture restore failed", file=sys.stderr)
                return 1

        for legacy_name in sorted(LEGACY_ALIAS_REFERENCE_NAMES):
            legacy_source = (
                baseline_event
                + reference_entry(legacy_name, "関数", "compat/Legacy.h")
            )
            actual_violations = check_contract(legacy_source, baseline_ecs)
            expected_violations = (
                (
                    "reference-legacy-alias-entry",
                    legacy_name,
                    "正規entryの本文で互換名として説明してください。",
                ),
            )
            if actual_violations != expected_violations:
                print(
                    "reference legacy alias mutation failed: "
                    f"name={legacy_name}, expected={expected_violations!r}, "
                    f"actual={actual_violations!r}",
                    file=sys.stderr,
                )
                return 1

        if check_contract(baseline_event, baseline_ecs):
            print("reference alias token fixture restore failed", file=sys.stderr)
            return 1
        token_baseline = tuple(
            (violation.tag, violation.name, violation.detail)
            for violation in audit_legacy_alias_reference_tokens(
                data_root,
                collect_reference_entries(data_root),
            )
        )
        if token_baseline:
            print(
                f"reference alias token baseline failed: {token_baseline!r}",
                file=sys.stderr,
            )
            return 1

        for legacy_name, canonical_name in sorted(LEGACY_REFERENCE_ALIASES.items()):
            # sampleやglossary相当の本文へ旧名が再流入するmutationを固定する。
            sample_path = data_root / "sample.js"
            original_sample = sample_path.read_text(encoding="utf-8")
            sample_path.write_text(
                original_sample + f'const legacyText = "{legacy_name}";\n',
                encoding="utf-8",
            )
            actual_token_violations = tuple(
                (violation.tag, violation.name, violation.detail)
                for violation in audit_legacy_alias_reference_tokens(
                    data_root,
                    collect_reference_entries(data_root),
                )
            )
            expected_token_violations = (
                (
                    "reference-legacy-alias-token",
                    legacy_name,
                    (
                        f"expected=using {legacy_name} = {canonical_name}, "
                        "token_count=2, canonical_entry_count=1"
                    ),
                ),
            )
            if actual_token_violations != expected_token_violations:
                print(
                    "reference legacy token mutation failed: "
                    f"name={legacy_name}, expected={expected_token_violations!r}, "
                    f"actual={actual_token_violations!r}",
                    file=sys.stderr,
                )
                return 1
            sample_path.write_text(original_sample, encoding="utf-8")

            # exact usingの参照先を変えた場合も互換説明として認めない。
            exact_alias = f"using {legacy_name} = {canonical_name}"
            alias_paths = [
                path
                for path in sorted(data_root.glob("*.js"))
                if exact_alias in path.read_text(encoding="utf-8")
            ]
            if len(alias_paths) != 1:
                print(
                    "reference legacy alias fixture is not unique: "
                    f"name={legacy_name}, paths={alias_paths!r}",
                    file=sys.stderr,
                )
                return 1
            alias_path = alias_paths[0]
            alias_source = alias_path.read_text(encoding="utf-8")
            alias_path.write_text(
                alias_source.replace(
                    exact_alias,
                    f"using {legacy_name} = WrongCanonical",
                    1,
                ),
                encoding="utf-8",
            )
            actual_target_violations = tuple(
                (violation.tag, violation.name, violation.detail)
                for violation in audit_legacy_alias_reference_tokens(
                    data_root,
                    collect_reference_entries(data_root),
                )
            )
            expected_target_violations = (
                (
                    "reference-legacy-alias-token",
                    legacy_name,
                    (
                        f"expected={exact_alias}, token_count=1, "
                        "canonical_entry_count=0"
                    ),
                ),
            )
            if actual_target_violations != expected_target_violations:
                print(
                    "reference legacy alias target mutation failed: "
                    f"name={legacy_name}, expected={expected_target_violations!r}, "
                    f"actual={actual_target_violations!r}",
                    file=sys.stderr,
                )
                return 1
            alias_path.write_text(alias_source, encoding="utf-8")

        if check_contract(baseline_event, baseline_ecs):
            print("reference canonical type fixture restore failed", file=sys.stderr)
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

    canonical_violations = audit_canonical_type_references(entries)
    for violation in canonical_violations:
        print(
            f"docs/reference/data: error: {violation.format()}",
            file=sys.stderr,
        )
    if canonical_violations:
        print(
            "ACS reference canonical-type audit failed: "
            f"{len(canonical_violations)} violation(s)",
            file=sys.stderr,
        )
        return 1

    try:
        legacy_token_violations = audit_legacy_alias_reference_tokens(
            data_root,
            entries,
        )
    except (OSError, UnicodeError) as error:
        print(f"error: reference alias contract を読めません: {error}", file=sys.stderr)
        return 2
    for violation in legacy_token_violations:
        print(
            f"docs/reference/data: error: {violation.format()}",
            file=sys.stderr,
        )
    if legacy_token_violations:
        print(
            "ACS reference legacy-alias audit failed: "
            f"{len(legacy_token_violations)} violation(s)",
            file=sys.stderr,
        )
        return 1

    undeclared, undocumented = audit_required_references(
        REQUIRED_FOUNDATION_REFERENCE_TYPES,
        declared,
        entries,
    )
    for name in undeclared:
        print(
            "docs/reference/data: error: "
            f"[reference-required-type] {name} は公開型一覧に残っていますが宣言がありません"
        )
    for name in undocumented:
        print(
            "docs/reference/data: error: "
            f"[reference-required-type] {name} の型リファレンスがありません"
        )
    if undeclared or undocumented:
        print(
            "ACS reference completeness audit failed: "
            f"{len(undeclared)} undeclared, {len(undocumented)} undocumented",
            file=sys.stderr,
        )
        return 1

    print(
        "ACS reference type-name audit passed: "
        f"{len(entries)} entry(s), {len(declared)} declaration(s), "
        f"{len(REQUIRED_FOUNDATION_REFERENCE_TYPES)} required foundation type(s), "
        f"{len(CANONICAL_REFERENCE_ENTRIES)} canonical hard type(s), "
        f"{len(LEGACY_REFERENCE_ALIASES)} legacy source alias(es)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
