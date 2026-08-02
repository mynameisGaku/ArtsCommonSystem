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
from collections import Counter
from dataclasses import dataclass
import html
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Iterable, Sequence

from audit_cpp_conventions import CPP_SUFFIXES, lex_cpp
from audit_cpp_type_roles import (
    CANONICAL_SCALAR_ALIASES,
    LEGACY_COMPATIBILITY_ALIASES,
    MIGRATED_CANONICAL_OBJECT_AND_CLASS_TYPES,
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
MEMBER_SIG_PATTERN = re.compile(r'\bsig\s*:\s*"((?:\\.|[^"\\])*)"')
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
REFERENCE_DATA_FILE_OVERRIDES = {
    "app": "app.js",
    "editor_abi": "editor.js",
    "render": "render_core.js",
    "subsystem": "subsystem.js",
}
GAMEFRAMEWORK_HEADER_FILE_OVERRIDES = {
    "HierarchyVisibilityBatch.h": "gameframework_8.js",
    "HierarchyWorldTransformBatch.h": "gameframework_8.js",
    "LegacyKitEaseIdCodec.h": "gameframework_3.js",
    "OpenXrBridge.h": "openxr.js",
    "Reflect.h": "gameframework_8.js",
    "ReflectMethod.h": "gameframework_4.js",
    "ScriptHost.h": "gameframework_6.js",
    "Steering2D.h": "gameframework_5.js",
    "StudioWorkflow.h": "gameframework_7.js",
}

# 手書きreferenceで既に使っているclass分類だけを明示的に受理する。
REFERENCE_CLASS_KINDS = frozenset(
    {
        "クラス",
        "クラス / 関数",
        "クラス (static のみ)",
        "クラス(AEditorCommand 派生サンプル)",
        "クラス(AEditorPanel 派生)",
        "クラス / 構造体",
        "クラス(AAsset 派生)",
        "クラス(IAssetLoader 派生)",
        "クラス(panel)",
        "クラス(static ユーティリティ)",
        "クラス(サービス hub)",
        "クラス(サービス)",
        "クラス(抽象基底)",
        "クラス(static 関数群)",
        "クラス(静的)",
        "クラス（静的）",
        "基底クラス",
    }
)
REFERENCE_STRUCT_KINDS = frozenset({"構造体", "構造体(POD)"})
MODULE_ID_PATTERN = re.compile(
    r'ACS_REF\.modules\.push\(\{\s*id\s*:\s*"((?:\\.|[^"\\])*)"'
)

# Node vm内で各data fileを独立実行し、表示上activeなmodule/type/memberを返す。
NODE_REFERENCE_EVALUATOR = r"""
const fs = require("fs");
const path = require("path");
const vm = require("vm");
const root = process.argv[1];
const names = fs.readdirSync(root)
  .filter((name) => name.endsWith(".js") && name !== "_meta.js")
  .sort((left, right) => left < right ? -1 : left > right ? 1 : 0);
const records = [];
for (const name of names) {
  const context = { ACS_REF: { modules: [], glossary: {}, guide: [], troubleshooting: [] } };
  context.window = context;
  vm.createContext(context, { codeGeneration: { strings: false, wasm: false } });
  const source = fs.readFileSync(path.join(root, name), "utf8");
  new vm.Script(source, { filename: name }).runInContext(context, { timeout: 1000 });
  for (const module of context.ACS_REF.modules) {
    const entries = [
      ...(Array.isArray(module.types) ? module.types : []),
      ...(Array.isArray(module.items) ? module.items : []),
    ];
    records.push({
      file: name,
      module: typeof module.id === "string" ? module.id : "",
      entries: entries
        .filter((entry) => entry !== null && typeof entry === "object")
        .map((entry) => ({
          name: typeof entry.name === "string" ? entry.name : "",
          kind: typeof entry.kind === "string" ? entry.kind : "",
          header: typeof entry.header === "string" ? entry.header : "",
          members: Array.isArray(entry.members)
            ? entry.members
                .filter((member) => member !== null && typeof member === "object")
                .map((member) => typeof member.sig === "string" ? member.sig : "")
                .filter((signature) => signature.length !== 0)
            : [],
        })),
    });
  }
}
process.stdout.write(JSON.stringify(records));
"""


def reference_data_file_for_header(header: str) -> str:
    """header moduleから手書きreference data fileを一意に決める。"""

    module = header.split("/", 1)[0]
    header_name = header.rsplit("/", 1)[-1]
    if module == "gameframework":
        if "/tools/" in header:
            return "editor.js"
        override = GAMEFRAMEWORK_HEADER_FILE_OVERRIDES.get(header_name)
        if override is not None:
            return override
        stem = header_name.rsplit(".", 1)[0]
        if stem <= "CameraStack":
            return "gameframework_1.js"
        if stem <= "DevConsole":
            return "gameframework_2.js"
        if stem <= "HealthSystem":
            return "gameframework_3.js"
        if stem <= "NetSnapshot":
            return "gameframework_4.js"
        if stem <= "Progression":
            return "gameframework_5.js"
        if stem <= "SeasonPass":
            return "gameframework_6.js"
        if stem <= "TelemetryDirector":
            return "gameframework_7.js"
        return "gameframework_8.js"
    if module == "render" and header_name.startswith(("Diligent", "Dx12")):
        return "render_backend.js"
    return REFERENCE_DATA_FILE_OVERRIDES.get(module, f"{module}.js")

# 型役割監査の正規契約からreference掲載先を導出する。
CANONICAL_SCALAR_REFERENCE_ENTRIES = {
    qualified_name.rsplit("::", 1)[-1]: (
        reference_data_file_for_header(header),
        header,
        ALIAS_KIND_MARKER,
    )
    for qualified_name, (header, _) in CANONICAL_SCALAR_ALIASES.items()
}

# 移行registryのclass/struct契約からreference掲載先を導出する。
CANONICAL_MIGRATED_REFERENCE_ENTRIES = {
    qualified_name.rsplit("::", 1)[-1]: (
        reference_data_file_for_header(header),
        header,
        (
            "構造体"
            if kind == "struct"
            else "インターフェース" if prefix == "I" else "クラス"
        ),
    )
    for qualified_name, (header, kind, prefix) in (
        MIGRATED_CANONICAL_OBJECT_AND_CLASS_TYPES.items()
    )
    if kind in {"class", "struct"}
}

# Phase 1aで新設したgame寿命のscene描画資源は掲載欠落を許さない。
REQUIRED_SCENE_REFERENCE_ENTRIES = {
    "CSceneRenderResources": (
        "gameframework_6.js",
        "gameframework/SceneRenderResources.h",
        "クラス",
    ),
}

# 新設subsystem moduleで欠落を許さない公開value/enum/alias契約。
REQUIRED_SUBSYSTEM_PUBLIC_REFERENCE_ENTRIES = {
    "ESubsystemOwnerKind": (
        "subsystem.js",
        "subsystem/SubsystemOwner.h",
        "列挙",
    ),
    "ESubsystemScope": (
        "subsystem.js",
        "subsystem/SubsystemScope.h",
        "列挙",
    ),
    "ESubsystemTickPhase": (
        "subsystem.js",
        "subsystem/SubsystemTickPhase.h",
        "列挙",
    ),
    "FSubsystemCreateFn": (
        "subsystem.js",
        "subsystem/SubsystemFactory.h",
        ALIAS_KIND_MARKER,
    ),
    "FSubsystemFactory": (
        "subsystem.js",
        "subsystem/SubsystemFactory.h",
        "構造体",
    ),
    "FSubsystemFrameContext": (
        "subsystem.js",
        "subsystem/SubsystemFrameContext.h",
        "構造体",
    ),
    "FSubsystemOwner": (
        "subsystem.js",
        "subsystem/SubsystemOwner.h",
        "構造体",
    ),
}

# 新設7型でreferenceから欠落を許さないowner、scope、tick、factory契約。
REQUIRED_SUBSYSTEM_PUBLIC_MEMBER_SIGNATURES = {
    "ESubsystemOwnerKind": ("Unknown", "Application", "Game", "Scene"),
    "ESubsystemScope": ("Engine", "GameInstance", "World"),
    "ESubsystemTickPhase": ("None", "PreUpdate", "PostUpdate"),
    "FSubsystemCreateFn": (
        "using FSubsystemCreateFn = TUniquePtr<ASubsystem> (*)()",
    ),
    "FSubsystemFactory": (
        "const void* kind",
        "ESubsystemScope scope",
        "const char* name",
        "FSubsystemCreateFn create",
        "ESubsystemTickPhase phase",
        "i32 order",
        "bool IsValidSubsystemScope(ESubsystemScope)",
        "bool IsValidSubsystemTickPhase(ESubsystemTickPhase)",
        "bool IsValidSubsystemFactory(const FSubsystemFactory&)",
    ),
    "FSubsystemFrameContext": (
        "f32 scaled_delta_seconds",
        "f32 unscaled_delta_seconds",
        "u64 frame_number",
        "ESubsystemTickPhase phase",
    ),
    "FSubsystemOwner": ("void* pointer", "ESubsystemOwnerKind kind"),
}
if set(REQUIRED_SUBSYSTEM_PUBLIC_MEMBER_SIGNATURES) != set(
    REQUIRED_SUBSYSTEM_PUBLIC_REFERENCE_ENTRIES
):
    raise RuntimeError("subsystem public member contract must cover exactly 7 types")

# subsystem APIとして一体で欠落を許さない既存6型と新設7型。
_EXISTING_SUBSYSTEM_REFERENCE_NAMES = (
    "AAssetSubsystem",
    "ASubsystem",
    "ATimerSubsystem",
    "CSubsystemAutoRegister",
    "CSubsystemCollection",
    "CSubsystemRegistry",
)
REQUIRED_SUBSYSTEM_REFERENCE_ENTRIES = {
    **{
        name: CANONICAL_MIGRATED_REFERENCE_ENTRIES[name]
        for name in _EXISTING_SUBSYSTEM_REFERENCE_NAMES
    },
    **REQUIRED_SUBSYSTEM_PUBLIC_REFERENCE_ENTRIES,
}
if len(REQUIRED_SUBSYSTEM_REFERENCE_ENTRIES) != 13:
    raise RuntimeError("subsystem reference contract must contain exactly 13 types")

# 全hard canonicalをkindとともに一意に検査する。
CANONICAL_REFERENCE_ENTRIES = {
    **CANONICAL_MIGRATED_REFERENCE_ENTRIES,
    **CANONICAL_SCALAR_REFERENCE_ENTRIES,
    **REQUIRED_SUBSYSTEM_PUBLIC_REFERENCE_ENTRIES,
    **REQUIRED_SCENE_REFERENCE_ENTRIES,
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
LEGACY_REFERENCE_SIMPLE_TO_CANONICAL = dict(LEGACY_REFERENCE_ALIASES)

# 基盤最適化時の旧表記を保持し、現行migration registryで正規名へ解決する。
_REQUIRED_FOUNDATION_REFERENCE_TYPE_NAMES = frozenset(
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
REQUIRED_FOUNDATION_REFERENCE_TYPES = frozenset(
    LEGACY_REFERENCE_SIMPLE_TO_CANONICAL.get(name, name)
    for name in _REQUIRED_FOUNDATION_REFERENCE_TYPE_NAMES
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


def collect_static_reference_execution_contract(
    data_root: Path,
    entries: Sequence[FReferenceEntry],
) -> tuple[Counter[tuple[str, str]], Counter[tuple[object, ...]]]:
    """collectorが見たmodule/type/member集合を実行結果と比較できる形へする。"""

    module_ids_by_file: dict[str, tuple[str, ...]] = {}
    modules: Counter[tuple[str, str]] = Counter()
    for path in sorted(data_root.glob("*.js"), key=lambda item: item.name.casefold()):
        if path.name == "_meta.js":
            continue
        source = path.read_text(encoding="utf-8")
        module_ids = tuple(
            decode_js_string(match.group(1))
            for match in MODULE_ID_PATTERN.finditer(source)
        )
        module_ids_by_file[path.name] = module_ids
        modules.update((path.name, module_id) for module_id in module_ids)

    records: Counter[tuple[object, ...]] = Counter()
    for entry in entries:
        module_ids = module_ids_by_file.get(entry.path.name, ())
        module_id = module_ids[0] if len(module_ids) == 1 else "<ambiguous>"
        member_signatures = tuple(
            decode_js_string(match.group(1))
            for match in MEMBER_SIG_PATTERN.finditer(entry.body)
        )
        records[
            (
                entry.path.name,
                module_id,
                entry.display_name,
                entry.kind,
                entry.header,
                member_signatures,
            )
        ] += 1
    return modules, records


def collect_executed_reference_contract(
    data_root: Path,
    *,
    evaluator: str = NODE_REFERENCE_EVALUATOR,
    timeout_seconds: float = 30.0,
    node_executable: str | None = None,
    discover_node: bool = True,
) -> tuple[Counter[tuple[str, str]], Counter[tuple[object, ...]]]:
    """Node sandboxでactiveなmodule/type/member集合だけを収集する。"""

    # 通常実行ではPATHからNodeを解決し、self-testでは欠落分岐を直接固定する。
    node_path = shutil.which("node") if discover_node else node_executable
    if node_path is None:
        raise RuntimeError("Node.js executable is required for reference data audit")
    completed = subprocess.run(
        [node_path, "-e", evaluator, str(data_root)],
        capture_output=True,
        check=False,
        encoding="utf-8",
        errors="strict",
        timeout=timeout_seconds,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "reference data Node evaluation failed: "
            f"exit={completed.returncode}, stderr={completed.stderr.strip()}"
        )
    document = json.loads(completed.stdout)
    if not isinstance(document, list):
        raise ValueError("reference data Node evaluation returned a non-array document")

    modules: Counter[tuple[str, str]] = Counter()
    records: Counter[tuple[object, ...]] = Counter()
    for module_record in document:
        if not isinstance(module_record, dict) or set(module_record) != {
            "file",
            "module",
            "entries",
        }:
            raise ValueError("reference data Node module record has an invalid schema")
        file_name = module_record["file"]
        module_id = module_record["module"]
        executed_entries = module_record["entries"]
        if (
            not isinstance(file_name, str)
            or not isinstance(module_id, str)
            or not isinstance(executed_entries, list)
        ):
            raise ValueError("reference data Node module record has invalid values")
        modules[(file_name, module_id)] += 1
        for executed_entry in executed_entries:
            if not isinstance(executed_entry, dict) or set(executed_entry) != {
                "name",
                "kind",
                "header",
                "members",
            }:
                raise ValueError("reference data Node entry record has an invalid schema")
            name = executed_entry["name"]
            kind = executed_entry["kind"]
            header = executed_entry["header"]
            members = executed_entry["members"]
            if (
                not isinstance(name, str)
                or not isinstance(kind, str)
                or not isinstance(header, str)
                or not isinstance(members, list)
                or any(not isinstance(member, str) for member in members)
            ):
                raise ValueError("reference data Node entry record has invalid values")
            records[
                (
                    file_name,
                    module_id,
                    html.unescape(name),
                    html.unescape(kind),
                    html.unescape(header),
                    tuple(html.unescape(member) for member in members),
                )
            ] += 1
    return modules, records


def audit_active_reference_entries(
    data_root: Path,
    entries: Sequence[FReferenceEntry],
) -> list[FReferenceContractViolation]:
    """静的collectorとNode実行後のactiveなmodule/type/member集合を照合する。"""

    static_modules, static_records = collect_static_reference_execution_contract(
        data_root,
        entries,
    )
    executed_modules, executed_records = collect_executed_reference_contract(data_root)
    violations: list[FReferenceContractViolation] = []
    for file_name, module_id in sorted(set(static_modules) | set(executed_modules)):
        static_count = static_modules[(file_name, module_id)]
        executed_count = executed_modules[(file_name, module_id)]
        if static_count == executed_count:
            continue
        violations.append(
            FReferenceContractViolation(
                "reference-js-active-module",
                module_id or "<missing>",
                (
                    f"file={file_name}, collected_count={static_count}, "
                    f"executed_count={executed_count}"
                ),
            )
        )
    for record in sorted(set(static_records) | set(executed_records)):
        static_count = static_records[record]
        executed_count = executed_records[record]
        if static_count == executed_count:
            continue
        file_name, module_id, name, kind, header, members = record
        violations.append(
            FReferenceContractViolation(
                "reference-js-active-entry",
                str(name) or "<missing>",
                (
                    f"file={file_name}, module={module_id}, kind={kind}, "
                    f"header={header}, members={members!r}, "
                    f"collected_count={static_count}, executed_count={executed_count}"
                ),
            )
        )
    return sorted(violations, key=lambda item: (item.tag, item.name, item.detail))


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


def entry_direct_display_identifiers(entry: FReferenceEntry) -> Iterable[str]:
    """nested型のqualifierを除き、entryが直接掲載する識別子を返す。"""

    for part in entry.display_name.split("/"):
        stripped = part.strip()
        match = IDENTIFIER_PATTERN.match(stripped)
        if match is None or stripped[match.end() :].startswith("::"):
            continue
        yield match.group(0)


def normalized_header_paths(header: str) -> frozenset[str]:
    """複合header表記を個別のrepo相対pathへ展開する。"""

    parts = [part.strip() for part in header.split(" / ")]
    result: set[str] = set()
    base = ""
    for part in parts:
        if not part:
            continue
        if "/" in part:
            base = part.rsplit("/", 1)[0]
            result.add(part)
        elif base:
            result.add(f"{base}/{part}")
        else:
            result.add(part)
    return frozenset(result)


def reference_kind_matches(actual: str, expected: str) -> bool:
    """説明付きkindを保持したままhard canonicalの役割を照合する。"""

    if expected == "クラス":
        return actual in REFERENCE_CLASS_KINDS
    if expected == "構造体":
        return actual in REFERENCE_STRUCT_KINDS
    return actual == expected


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
        # 診断をscalar、subsystem公開型、移行型の契約ごとに区別する。
        if name in CANONICAL_SCALAR_REFERENCE_ENTRIES:
            tag_prefix = "reference-scalar-alias"
        elif name in REQUIRED_SUBSYSTEM_PUBLIC_REFERENCE_ENTRIES:
            tag_prefix = "reference-subsystem-public"
        elif name in REQUIRED_SCENE_REFERENCE_ENTRIES:
            tag_prefix = "reference-scene-resource"
        else:
            tag_prefix = "reference-migrated-type"
        matches = [
            entry
            for entry in entries
            if name in entry_direct_display_identifiers(entry)
        ]
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
        if expected_header not in normalized_header_paths(entry.header):
            violations.append(
                FReferenceContractViolation(
                    f"{tag_prefix}-header",
                    name,
                    f"expected={expected_header}, actual={entry.header or '<missing>'}",
                )
            )
        if not reference_kind_matches(entry.kind, expected_kind):
            violations.append(
                FReferenceContractViolation(
                    f"{tag_prefix}-kind",
                    name,
                    f"expected={expected_kind}, actual={entry.kind or '<missing>'}",
                )
            )
        expected_members = REQUIRED_SUBSYSTEM_PUBLIC_MEMBER_SIGNATURES.get(name)
        if expected_members is not None:
            actual_members = tuple(
                decode_js_string(match.group(1))
                for match in MEMBER_SIG_PATTERN.finditer(entry.body)
            )
            if actual_members != expected_members:
                violations.append(
                    FReferenceContractViolation(
                        "reference-subsystem-public-members",
                        name,
                        f"expected={expected_members!r}, actual={actual_members!r}",
                    )
                )

    for entry in entries:
        for name in entry_direct_display_identifiers(entry):
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
        canonical_contract = CANONICAL_REFERENCE_ENTRIES.get(canonical_name)
        if canonical_contract is None:
            violations.append(
                FReferenceContractViolation(
                    "reference-legacy-alias-contract",
                    legacy_name,
                    f"canonical={canonical_name} のhard type契約がありません。",
                )
            )
            continue
        # 保持するheader名の旧tokenはAPI構成上の正当な例外として除外する。
        expected_header = canonical_contract[1]
        normalized_sources = {
            path: source.replace(expected_header, "")
            for path, source in sources.items()
        }
        token_count = sum(
            len(token_pattern.findall(source))
            for source in normalized_sources.values()
        )
        canonical_entries = [
            entry
            for entry in entries
            if canonical_name in entry_direct_display_identifiers(entry)
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

    if not reference_kind_matches("クラス(静的)", "クラス"):
        print("registered reference class kind was rejected", file=sys.stderr)
        return 1
    if reference_kind_matches("偽クラス", "クラス"):
        print("unregistered reference class kind was accepted", file=sys.stderr)
        return 1
    if not reference_kind_matches("構造体(POD)", "構造体"):
        print("registered reference struct kind was rejected", file=sys.stderr)
        return 1
    if reference_kind_matches("偽構造体", "構造体"):
        print("unregistered reference struct kind was accepted", file=sys.stderr)
        return 1

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
        routing_cases = (
            ("app/Application.h", "app.js"),
            ("subsystem/Subsystem.h", "subsystem.js"),
            ("gameframework/ScriptHost.h", "gameframework_6.js"),
            ("gameframework/Reflect.h", "gameframework_8.js"),
            ("gameframework/OpenXrBridge.h", "openxr.js"),
            ("gameframework/tools/EditorCommand.h", "editor.js"),
        )
        for header, expected_file in routing_cases:
            actual_file = reference_data_file_for_header(header)
            if actual_file != expected_file:
                print(
                    "reference routing self-test failed: "
                    f"header={header}, expected={expected_file}, actual={actual_file}",
                    file=sys.stderr,
                )
                return 1

        # 静的collectorと実行時moduleの一致、およびNode失敗時のfail-closedを固定する。
        active_data_root = root / "active-reference-data"
        active_data_root.mkdir()
        active_path = active_data_root / "active.js"
        active_source = (
            "ACS_REF.modules.push({\n"
            '  id: "active",\n'
            "  types: [{\n"
            '    name: "CApplication", kind: "クラス", '
            'header: "app/Application.h",\n'
            '    members: [{ sig: "void Run()" }]\n'
            "  }]\n"
            "});\n"
        )
        active_path.write_text(active_source, encoding="utf-8")
        active_entries = collect_reference_entries(active_data_root)
        active_baseline = tuple(
            (violation.tag, violation.name, violation.detail)
            for violation in audit_active_reference_entries(
                active_data_root,
                active_entries,
            )
        )
        if active_baseline:
            print(
                "reference active-entry baseline did not pass: "
                f"{active_baseline!r}",
                file=sys.stderr,
            )
            return 1

        inactive_source = active_source.replace(
            "types: [{",
            "types: (false ? [{",
            1,
        ).replace("  }]\n});", "  }] : [])\n});", 1)
        active_path.write_text(inactive_source, encoding="utf-8")
        inactive_violations = tuple(
            (violation.tag, violation.name, violation.detail)
            for violation in audit_active_reference_entries(
                active_data_root,
                collect_reference_entries(active_data_root),
            )
        )
        expected_inactive_violations = (
            (
                "reference-js-active-entry",
                "CApplication",
                (
                    "file=active.js, module=active, kind=クラス, "
                    "header=app/Application.h, members=('void Run()',), "
                    "collected_count=1, executed_count=0"
                ),
            ),
        )
        if inactive_violations != expected_inactive_violations:
            print(
                "reference inactive-entry mutation failed: "
                f"expected={expected_inactive_violations!r}, "
                f"actual={inactive_violations!r}",
                file=sys.stderr,
            )
            return 1
        active_path.write_text(active_source, encoding="utf-8")

        try:
            collect_executed_reference_contract(
                active_data_root,
                node_executable=None,
                discover_node=False,
            )
        except RuntimeError as error:
            if str(error) != "Node.js executable is required for reference data audit":
                print(
                    f"reference missing-Node diagnostic changed: {error}",
                    file=sys.stderr,
                )
                return 1
        else:
            print("reference missing-Node mutation was accepted", file=sys.stderr)
            return 1

        # 実行中のNodeを使ってtimeout、schema、構文失敗を個別に固定する。
        self_test_node = shutil.which("node")
        if self_test_node is None:
            print("reference self-test requires Node.js", file=sys.stderr)
            return 1
        try:
            collect_executed_reference_contract(
                active_data_root,
                evaluator="while (true) {}",
                timeout_seconds=0.05,
                node_executable=self_test_node,
                discover_node=False,
            )
        except subprocess.TimeoutExpired as error:
            if error.timeout != 0.05:
                print(
                    f"reference Node timeout diagnostic changed: {error.timeout}",
                    file=sys.stderr,
                )
                return 1
        else:
            print("reference Node timeout mutation was accepted", file=sys.stderr)
            return 1

        try:
            collect_executed_reference_contract(
                active_data_root,
                evaluator='process.stdout.write("{}");',
                node_executable=self_test_node,
                discover_node=False,
            )
        except ValueError as error:
            if str(error) != "reference data Node evaluation returned a non-array document":
                print(
                    f"reference Node schema diagnostic changed: {error}",
                    file=sys.stderr,
                )
                return 1
        else:
            print("reference Node schema mutation was accepted", file=sys.stderr)
            return 1

        active_path.write_text("ACS_REF.modules.push({\n", encoding="utf-8")
        try:
            collect_executed_reference_contract(
                active_data_root,
                node_executable=self_test_node,
                discover_node=False,
            )
        except RuntimeError as error:
            error_text = str(error)
            if not error_text.startswith(
                "reference data Node evaluation failed: exit=1, stderr="
            ) or "SyntaxError" not in error_text:
                print(
                    f"reference Node syntax diagnostic changed: {error}",
                    file=sys.stderr,
                )
                return 1
        else:
            print("reference Node syntax mutation was accepted", file=sys.stderr)
            return 1
        active_path.write_text(active_source, encoding="utf-8")

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

        # 以降はregistry由来のglobal contractだけを検査し、一般名fixtureを分離する。
        data_path.write_text("", encoding="utf-8")
        event_path = data_root / "event.js"
        ecs_path = data_root / "ecs.js"
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

        canonical_to_legacy = {
            canonical_name: legacy_name
            for legacy_name, canonical_name in LEGACY_REFERENCE_ALIASES.items()
        }
        # 全旧名がclass/structを含むhard canonicalへ解決できることを先に固定する。
        missing_legacy_targets = sorted(
            set(canonical_to_legacy) - set(CANONICAL_REFERENCE_ENTRIES)
        )
        if missing_legacy_targets:
            print(
                "reference legacy target contract is incomplete: "
                f"{missing_legacy_targets!r}",
                file=sys.stderr,
            )
            return 1

        def canonical_migrated_entry(
            canonical_name: str,
            expected_header: str,
            expected_kind: str,
        ) -> str:
            """registry由来のcanonical class/struct fixtureを返す。"""

            legacy_name = canonical_to_legacy[canonical_name]
            return reference_entry(
                canonical_name,
                expected_kind,
                expected_header,
                (
                    '  members: [{ sig: "using '
                    f'{legacy_name} = {canonical_name}" }}]'
                ),
            )

        def subsystem_public_entry(
            canonical_name: str,
            expected_header: str,
            expected_kind: str,
        ) -> str:
            """新設subsystem公開型と必須memberのfixtureを返す。"""

            member_source = ", ".join(
                f'{{ sig: "{signature}" }}'
                for signature in REQUIRED_SUBSYSTEM_PUBLIC_MEMBER_SIGNATURES[
                    canonical_name
                ]
            )
            return reference_entry(
                canonical_name,
                expected_kind,
                expected_header,
                f"  members: [{member_source}]",
            )

        def required_scene_entry(
            canonical_name: str,
            expected_header: str,
            expected_kind: str,
        ) -> str:
            """scene描画資源の必須掲載fixtureを返す。"""

            return reference_entry(canonical_name, expected_kind, expected_header)

        hard_baseline_by_file: dict[str, str] = {}
        for canonical_name, (
            expected_file,
            expected_header,
            expected_kind,
        ) in sorted(CANONICAL_MIGRATED_REFERENCE_ENTRIES.items()):
            hard_baseline_by_file[expected_file] = (
                hard_baseline_by_file.get(expected_file, "")
                + canonical_migrated_entry(
                    canonical_name,
                    expected_header,
                    expected_kind,
                )
            )
        for canonical_name, (
            expected_file,
            expected_header,
            expected_kind,
        ) in sorted(REQUIRED_SUBSYSTEM_PUBLIC_REFERENCE_ENTRIES.items()):
            hard_baseline_by_file[expected_file] = (
                hard_baseline_by_file.get(expected_file, "")
                + subsystem_public_entry(
                    canonical_name,
                    expected_header,
                    expected_kind,
                )
            )
        for canonical_name, (
            expected_file,
            expected_header,
            expected_kind,
        ) in sorted(REQUIRED_SCENE_REFERENCE_ENTRIES.items()):
            hard_baseline_by_file[expected_file] = (
                hard_baseline_by_file.get(expected_file, "")
                + required_scene_entry(
                    canonical_name,
                    expected_header,
                    expected_kind,
                )
            )

        def write_contract_sources(event_source: str, ecs_source: str) -> None:
            """指定したreference fixtureを一意なbaseline配置へ戻す。"""

            wrong_path = data_root / "wrong.js"
            if wrong_path.exists():
                wrong_path.unlink()
            baseline_by_file = dict(hard_baseline_by_file)
            baseline_by_file["event.js"] = (
                baseline_by_file.get("event.js", "") + event_source
            )
            baseline_by_file["ecs.js"] = (
                baseline_by_file.get("ecs.js", "") + ecs_source
            )
            for file_name, source in sorted(baseline_by_file.items()):
                (data_root / file_name).write_text(source, encoding="utf-8")

        def check_contract(
            event_source: str, ecs_source: str
        ) -> tuple[tuple[str, str, str], ...]:
            """指定したreference fixtureのhard canonical契約違反を返す。"""

            write_contract_sources(event_source, ecs_source)
            return tuple(
                (violation.tag, violation.name, violation.detail)
                for violation in audit_canonical_type_references(
                    collect_reference_entries(data_root)
                )
            )

        baseline_contract_violations = check_contract(baseline_event, baseline_ecs)
        if baseline_contract_violations:
            print(
                "reference canonical type baseline did not pass: "
                f"{baseline_contract_violations!r}",
                file=sys.stderr,
            )
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

        # 全entry baselineとは別に、既存移行型はkindと配置の代表だけへ限定する。
        migrated_mutation_names = frozenset(
            {
                "ABtNode",
                "AObject",
                "ASubsystem",
                "CAudioEngine",
                "CApplication",
                "CMethodAutoRegister",
                "COpenXrBridgeStub",
                "CSubsystemAutoRegister",
                "CTypeAutoRegister",
                "IAllocator",
            }
        )
        # app/subsystemに分かれた既存6型と新設7型を一体で変異検査する。
        subsystem_public_mutation_names = frozenset(
            REQUIRED_SUBSYSTEM_REFERENCE_ENTRIES
        )
        hard_type_mutation_names = (
            migrated_mutation_names
            | subsystem_public_mutation_names
            | frozenset(REQUIRED_SCENE_REFERENCE_ENTRIES)
        )
        missing_hard_type_mutations = sorted(
            hard_type_mutation_names - set(CANONICAL_REFERENCE_ENTRIES)
        )
        if missing_hard_type_mutations:
            print(
                "reference hard-type mutation fixture is incomplete: "
                f"{missing_hard_type_mutations!r}",
                file=sys.stderr,
            )
            return 1
        hard_fixtures = tuple(
            (
                canonical_name,
                data_root / expected_file,
                (
                    canonical_migrated_entry(
                        canonical_name,
                        expected_header,
                        expected_kind,
                    )
                    if canonical_name in CANONICAL_MIGRATED_REFERENCE_ENTRIES
                    else (
                        subsystem_public_entry(
                            canonical_name,
                            expected_header,
                            expected_kind,
                        )
                        if canonical_name in REQUIRED_SUBSYSTEM_PUBLIC_REFERENCE_ENTRIES
                        else required_scene_entry(
                            canonical_name,
                            expected_header,
                            expected_kind,
                        )
                    )
                ),
                expected_header,
                expected_kind,
                (
                    "reference-subsystem-public"
                    if canonical_name in REQUIRED_SUBSYSTEM_PUBLIC_REFERENCE_ENTRIES
                    else (
                        "reference-scene-resource"
                        if canonical_name in REQUIRED_SCENE_REFERENCE_ENTRIES
                        else "reference-migrated-type"
                    )
                ),
            )
            for canonical_name, (
                expected_file,
                expected_header,
                expected_kind,
            ) in sorted(CANONICAL_REFERENCE_ENTRIES.items())
            if canonical_name in hard_type_mutation_names
        )
        for (
            canonical_name,
            canonical_path,
            canonical_entry,
            expected_header,
            expected_kind,
            diagnostic_prefix,
        ) in hard_fixtures:
            # 各mutationの前に正規fixtureへ戻す。
            write_contract_sources(baseline_event, baseline_ecs)
            canonical_source = canonical_path.read_text(encoding="utf-8")
            wrong_header = expected_header.replace(".h", "Wrong.h")
            # 正規kindと一致しない構文分類を選ぶ。
            wrong_kind = {
                "クラス": "構造体",
                "構造体": "クラス",
                "インターフェース": "構造体",
            }.get(expected_kind, "関数")
            mutations_for_type = (
                (
                    "missing",
                    canonical_source.replace(canonical_entry, "", 1),
                    canonical_path,
                    (
                        f"{diagnostic_prefix}-missing",
                        canonical_name,
                        "正規名の独立entryがありません。",
                    ),
                ),
                (
                    "duplicate",
                    canonical_source + canonical_entry,
                    canonical_path,
                    (
                        f"{diagnostic_prefix}-duplicate",
                        canonical_name,
                        "正規名の独立entryが重複しています: count=2",
                    ),
                ),
                (
                    "wrong header",
                    canonical_source.replace(
                        canonical_entry,
                        canonical_entry.replace(expected_header, wrong_header, 1),
                        1,
                    ),
                    canonical_path,
                    (
                        f"{diagnostic_prefix}-header",
                        canonical_name,
                        f"expected={expected_header}, actual={wrong_header}",
                    ),
                ),
                (
                    "wrong kind",
                    canonical_source.replace(
                        canonical_entry,
                        canonical_entry.replace(
                            f'kind: "{expected_kind}"',
                            f'kind: "{wrong_kind}"',
                            1,
                        ),
                        1,
                    ),
                    canonical_path,
                    (
                        f"{diagnostic_prefix}-kind",
                        canonical_name,
                        f"expected={expected_kind}, actual={wrong_kind}",
                    ),
                ),
            )
            if canonical_name in REQUIRED_SUBSYSTEM_PUBLIC_MEMBER_SIGNATURES:
                expected_members = REQUIRED_SUBSYSTEM_PUBLIC_MEMBER_SIGNATURES[
                    canonical_name
                ]
                mutated_members = ("WrongMember", *expected_members[1:])
                mutations_for_type += (
                    (
                        "wrong members",
                        canonical_source.replace(
                            canonical_entry,
                            canonical_entry.replace(
                                f'sig: "{expected_members[0]}"',
                                'sig: "WrongMember"',
                                1,
                            ),
                            1,
                        ),
                        canonical_path,
                        (
                            "reference-subsystem-public-members",
                            canonical_name,
                            (
                                f"expected={expected_members!r}, "
                                f"actual={mutated_members!r}"
                            ),
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
                        "reference hard-type mutation failed: "
                        f"name={canonical_name}, mutation={label}, "
                        f"expected={(expected_violation,)!r}, "
                        f"actual={actual_violations!r}",
                        file=sys.stderr,
                    )
                    return 1
                write_contract_sources(baseline_event, baseline_ecs)

            # 正規entryを別fileへ移してfile契約を検査する。
            canonical_source = canonical_path.read_text(encoding="utf-8")
            canonical_path.write_text(
                canonical_source.replace(canonical_entry, "", 1),
                encoding="utf-8",
            )
            wrong_path = data_root / "wrong.js"
            wrong_path.write_text(canonical_entry, encoding="utf-8")
            actual_file_violations = tuple(
                (violation.tag, violation.name, violation.detail)
                for violation in audit_canonical_type_references(
                    collect_reference_entries(data_root)
                )
            )
            expected_file_violation = (
                (
                    f"{diagnostic_prefix}-file",
                    canonical_name,
                    (
                        "expected="
                        f"{CANONICAL_REFERENCE_ENTRIES[canonical_name][0]}, "
                        "actual=wrong.js"
                    ),
                ),
            )
            if actual_file_violations != expected_file_violation:
                print(
                    "reference hard-type file mutation failed: "
                    f"name={canonical_name}, expected={expected_file_violation!r}, "
                    f"actual={actual_file_violations!r}",
                    file=sys.stderr,
                )
                return 1
            write_contract_sources(baseline_event, baseline_ecs)

        # 全旧名baselineとは別に、代表aliasだけで独立entry拒否を変異検査する。
        legacy_mutation_names = frozenset(
            {
                "EventTypeId",
                "FAllocator",
                "FApplication",
                "FAudioEngine",
                "FBtNode",
                "FMethodAutoRegister",
                "FObject",
                "FOpenXrBridgeStub",
                "FSubsystem",
                "FSubsystemAutoRegister",
                "FTypeAutoRegister",
            }
        )
        missing_legacy_mutations = sorted(
            legacy_mutation_names - LEGACY_ALIAS_REFERENCE_NAMES
        )
        if missing_legacy_mutations:
            print(
                "reference legacy mutation fixture is incomplete: "
                f"{missing_legacy_mutations!r}",
                file=sys.stderr,
            )
            return 1
        for legacy_name in sorted(legacy_mutation_names):
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

        write_contract_sources(baseline_event, baseline_ecs)
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

        for legacy_name in sorted(legacy_mutation_names):
            canonical_name = LEGACY_REFERENCE_ALIASES[legacy_name]
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

        write_contract_sources(baseline_event, baseline_ecs)

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

    try:
        active_reference_violations = audit_active_reference_entries(
            data_root,
            entries,
        )
    except (
        OSError,
        UnicodeError,
        RuntimeError,
        ValueError,
        subprocess.TimeoutExpired,
    ) as error:
        print(
            f"error: reference dataを安全に実行できません: {error}",
            file=sys.stderr,
        )
        return 2
    for violation in active_reference_violations:
        print(
            f"docs/reference/data: error: {violation.format()}",
            file=sys.stderr,
        )
    if active_reference_violations:
        print(
            "ACS reference active-entry audit failed: "
            f"{len(active_reference_violations)} violation(s)",
            file=sys.stderr,
        )
        return 1

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
        f"{len(REQUIRED_SUBSYSTEM_REFERENCE_ENTRIES)} subsystem API type(s), "
        f"{len(LEGACY_REFERENCE_ALIASES)} legacy source alias(es)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
