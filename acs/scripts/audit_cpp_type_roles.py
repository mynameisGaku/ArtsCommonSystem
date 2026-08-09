#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""C++型の役割とA/C/F/I/T/E接頭辞の対応を監査する。"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import tempfile
from typing import Optional, Sequence

from audit_cpp_conventions import (
    CPP_SUFFIXES,
    EXCLUDED_DIRECTORY_NAMES,
    Token as FToken,
    _declaration_name,
    _skip_balanced,
    lex_cpp,
    serialize_json_report,
    try_write_json_report,
)


IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
ROLE_NAME = re.compile(r"^[ACEFIT][A-Z0-9][A-Za-z0-9]*$")
OBJECT_MACROS = frozenset({"ACS_OBJECT", "ACS_REGISTER_OBJECT"})

# module単位の走査でも所有objectの継承関係を判定する正規基底。
CANONICAL_EXTERNAL_MANAGED_BASES = frozenset(
    {
        "acs::AObject",
        "acs::game::AComponent",
        "acs::game::ANode",
        "acs::game::AScene",
    }
)
LEGACY_IDENTITY_MACRO_ARGUMENTS = {
    "ACS_GAME_SUBSYSTEM_KIND": frozenset({0}),
    "ACS_REGISTER_ASSET": frozenset({0}),
    "ACS_REGISTER_SCENE": frozenset({0}),
    "ACS_REGISTER_SYSTEM": frozenset({0}),
    "ACS_RTTI": frozenset({0, 1}),
    "ACS_RTTI_ROOT": frozenset({0}),
    "ACS_SUBSYSTEM_KIND": frozenset({0}),
}
ACCESS_WORDS = frozenset({"public", "private", "protected", "virtual"})
VALUE_WORDS = frozenset(
    {
        "Array",
        "Asset",
        "Bounds",
        "Buffer",
        "Color",
        "Command",
        "Config",
        "Date",
        "Desc",
        "Descriptor",
        "Duration",
        "Entry",
        "Event",
        "Flags",
        "Frame",
        "Future",
        "Guid",
        "Handle",
        "Hash",
        "Id",
        "Info",
        "Key",
        "Map",
        "Matrix",
        "Node",
        "Options",
        "Packet",
        "Pair",
        "Path",
        "Payload",
        "Point",
        "Quaternion",
        "Range",
        "Record",
        "Rect",
        "Result",
        "Set",
        "Size",
        "Snapshot",
        "Span",
        "State",
        "String",
        "Transform",
        "Tuple",
        "Value",
        "Vector",
        "Version",
        "View",
    }
)
BEHAVIOR_WORDS = frozenset(
    {
        "App",
        "Allocator",
        "Application",
        "Backend",
        "Broker",
        "Cache",
        "Clock",
        "Controller",
        "Device",
        "Director",
        "Dispatcher",
        "Engine",
        "Executor",
        "Factory",
        "Loader",
        "Logger",
        "Manager",
        "Player",
        "Pool",
        "Reader",
        "Register",
        "Registry",
        "Renderer",
        "Runtime",
        "Scheduler",
        "Server",
        "Service",
        "Session",
        "Sink",
        "Source",
        "Store",
        "Stream",
        "Subsystem",
        "System",
        "Thread",
        "Tracker",
        "ViewModel",
        "Vm",
        "Watcher",
        "Writer",
    }
)
LIFECYCLE_METHODS = frozenset(
    {
        "Acquire",
        "Allocate",
        "Close",
        "Create",
        "Destroy",
        "Execute",
        "Free",
        "Initialize",
        "Load",
        "Open",
        "Register",
        "Release",
        "Run",
        "Save",
        "Shutdown",
        "Start",
        "Stop",
        "Tick",
        "Unregister",
        "Update",
    }
)
OBVIOUS_BEHAVIOR_METHODS = frozenset({"Draw", "Render"})
OWNERSHIP_TOKENS = frozenset(
    {
        "shared_ptr",
        "TObjectPtr",
        "TSharedPtr",
        "TUniquePtr",
        "unique_ptr",
    }
)
COORDINATION_TOKENS = frozenset(
    {
        "atomic",
        "condition_variable",
        "FThread",
        "future",
        "jthread",
        "mutex",
        "promise",
        "thread",
    }
)
SCALAR_TYPE_NAMES = frozenset(
    {
        "__int8",
        "__int16",
        "__int32",
        "__int64",
        "bool",
        "byte",
        "c8",
        "c16",
        "c32",
        "char",
        "char8_t",
        "char16_t",
        "char32_t",
        "double",
        "f32",
        "f64",
        "float",
        "i8",
        "i16",
        "i32",
        "i64",
        "int",
        "int8_t",
        "int16_t",
        "int32_t",
        "int64_t",
        "intptr_t",
        "iptr",
        "isize",
        "long",
        "ptrdiff_t",
        "short",
        "signed",
        "size_t",
        "u8",
        "u16",
        "u32",
        "u64",
        "uint8_t",
        "uint16_t",
        "uint32_t",
        "uint64_t",
        "uintptr_t",
        "unsigned",
        "uptr",
        "usize",
        "wchar_t",
    }
)
DETAIL_NAMESPACE_NAMES = frozenset({"detail", "internal", "private"})
PUBLIC_HEADER_SUFFIXES = frozenset({".h", ".hh", ".hpp", ".hxx", ".inl"})
TYPE_ROLE_MIGRATION_SCHEMA_VERSION = 2
DEFAULT_TYPE_ROLE_MIGRATION_ENTRY_COUNT = 333
DEFAULT_TYPE_ROLE_MIGRATION_SEMANTIC_SHA256 = "8054D56AB0F21BF0D98B72E02A486EA98578A110E6D37BF36B5CAF425DF29945"
DEFAULT_TYPE_ROLE_MIGRATIONS = (
    Path(os.path.abspath(__file__)).parent / "data" / "cpp_type_role_migrations.json"
)


def _load_registered_type_role_migrations(
    path: Path = DEFAULT_TYPE_ROLE_MIGRATIONS,
    verify_default_baseline: bool = True,
) -> tuple[tuple[Optional[str], str, str, Optional[str], str, str], ...]:
    """正規型と一時的な旧名の固定契約を読み込む。"""

    # 監査器と同じ場所に置いた追跡済みregistryだけを読む。
    registry_stat = path.lstat()
    reparse_attribute = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    file_attributes = getattr(registry_stat, "st_file_attributes", 0)
    if (
        not stat.S_ISREG(registry_stat.st_mode)
        or stat.S_ISLNK(registry_stat.st_mode)
        or bool(file_attributes & reparse_attribute)
    ):
        raise ValueError("type role migration registry must be a regular non-reparse file")
    payload = path.read_bytes()
    if payload.startswith(b"\xef\xbb\xbf") or b"\r" in payload or not payload.endswith(b"\n"):
        raise ValueError("type role migration registry must be UTF-8 without BOM and use LF")

    # 同じkeyを後勝ちにせず、その場で拒否する。
    def reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate type role migration key: {key}")
            result[key] = value
        return result

    document = json.loads(payload.decode("utf-8"), object_pairs_hook=reject_duplicate_keys)
    if not isinstance(document, dict) or set(document) != {"schema_version", "entries"}:
        raise ValueError("type role migration registry has an invalid root schema")
    if (
        type(document["schema_version"]) is not int
        or document["schema_version"] != TYPE_ROLE_MIGRATION_SCHEMA_VERSION
    ):
        raise ValueError("type role migration registry has an unsupported schema version")
    raw_entries = document["entries"]
    if not isinstance(raw_entries, list):
        raise ValueError("type role migration entries must be an array")
    # sourceとregistryを同時に削除しても通らない独立baselineを固定する。
    semantic_payload = json.dumps(
        raw_entries,
        ensure_ascii=False,
        separators=(",", ":"),
        allow_nan=False,
    ).encode("utf-8")
    semantic_sha256 = hashlib.sha256(semantic_payload).hexdigest().upper()
    if verify_default_baseline and (
        len(raw_entries) != DEFAULT_TYPE_ROLE_MIGRATION_ENTRY_COUNT
        or semantic_sha256 != DEFAULT_TYPE_ROLE_MIGRATION_SEMANTIC_SHA256
    ):
        raise ValueError("type role migration registry does not match the fixed baseline")

    # legacy、canonical、定義path、互換alias path、宣言種別、prefixの順で保持する。
    entries: list[tuple[Optional[str], str, str, Optional[str], str, str]] = []
    expected_fields = {"path", "legacy", "canonical", "legacy_path", "kind", "prefix"}
    qualified_name = re.compile(r"^[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*$")

    def validate_public_header_path(value: object, field_name: str, index: int) -> str:
        """registry内のrepo相対公開header pathを正規化せず検証する。"""

        # 入力表記と完全一致するPOSIX pathだけを許可する。
        normalized_path = PurePosixPath(value) if isinstance(value, str) else None
        if (
            not isinstance(value, str)
            or not value
            or normalized_path is None
            or normalized_path.is_absolute()
            or normalized_path.as_posix() != value
            or any(part in {"", ".", ".."} for part in normalized_path.parts)
            or "\\" in value
            or ":" in value
            or any(ord(character) < 0x20 or ord(character) == 0x7F for character in value)
            or normalized_path.suffix not in PUBLIC_HEADER_SUFFIXES
        ):
            raise ValueError(
                f"type role migration entries[{index}] has an invalid {field_name}"
            )
        return value

    for index, raw_entry in enumerate(raw_entries):
        if not isinstance(raw_entry, dict) or set(raw_entry) != expected_fields:
            raise ValueError(f"type role migration entries[{index}] has an invalid schema")
        # 正規型の定義先と旧名aliasの公開先を別々に保持する。
        path = validate_public_header_path(raw_entry["path"], "path", index)
        legacy = raw_entry["legacy"]
        canonical = raw_entry["canonical"]
        legacy_path_value = raw_entry["legacy_path"]
        kind = raw_entry["kind"]
        prefix = raw_entry["prefix"]
        if legacy is not None and (
            not isinstance(legacy, str) or qualified_name.fullmatch(legacy) is None
        ):
            raise ValueError(f"type role migration entries[{index}] has an invalid legacy name")
        if legacy is None:
            if legacy_path_value is not None:
                raise ValueError(
                    f"type role migration entries[{index}] cannot have legacy_path without legacy"
                )
            legacy_path = None
        else:
            if legacy_path_value is None:
                raise ValueError(
                    f"type role migration entries[{index}] requires legacy_path for legacy"
                )
            legacy_path = validate_public_header_path(
                legacy_path_value,
                "legacy_path",
                index,
            )
        if not isinstance(canonical, str) or qualified_name.fullmatch(canonical) is None:
            raise ValueError(f"type role migration entries[{index}] has an invalid canonical name")
        if kind not in {"class", "struct"} or prefix not in {"A", "C", "F", "I"}:
            raise ValueError(f"type role migration entries[{index}] has an invalid role")
        if canonical.rsplit("::", 1)[-1][0] != prefix:
            raise ValueError(f"type role migration entries[{index}] prefix does not match canonical name")
        if legacy == canonical:
            raise ValueError(f"type role migration entries[{index}] cannot alias a type to itself")
        entries.append((legacy, canonical, path, legacy_path, kind, prefix))

    if entries != sorted(entries, key=lambda entry: (entry[2], entry[0] or "")):
        raise ValueError("type role migration entries must use ordinal path and legacy-name order")
    legacy_names = [entry[0] for entry in entries if entry[0] is not None]
    if len(set(legacy_names)) != len(legacy_names):
        raise ValueError("type role migration legacy names must be unique")
    if len({entry[1] for entry in entries}) != len(entries):
        raise ValueError("type role migration canonical names must be unique")
    return tuple(entries)


REGISTERED_TYPE_ROLE_MIGRATIONS = _load_registered_type_role_migrations()
# 登録済み旧名だけで宣言した所有objectも、正規基底と同じ管理対象として解決する。
# 互換入口の維持を検査するcompile testは旧綴りを基底に書くため、綴りの違いで役割を変えない。
EXTERNAL_MANAGED_BASES = CANONICAL_EXTERNAL_MANAGED_BASES | frozenset(
    legacy
    for legacy, canonical, _, _, _, _ in REGISTERED_TYPE_ROLE_MIGRATIONS
    if legacy is not None and canonical in CANONICAL_EXTERNAL_MANAGED_BASES
)
LEGACY_COMPATIBILITY_ALIASES = {
    legacy: canonical for legacy, canonical, _, _, _, _ in REGISTERED_TYPE_ROLE_MIGRATIONS
    if legacy is not None
}
LEGACY_COMPATIBILITY_ALIASES.update({
    "acs::ComponentSignatureId": "acs::FComponentSignatureId",
    "acs::ComponentTypeId": "acs::FComponentTypeId",
    "acs::EventTypeId": "acs::FEventTypeId",
})
LEGACY_COMPATIBILITY_PATHS = {
    legacy: legacy_path
    for legacy, _, _, legacy_path, _, _ in REGISTERED_TYPE_ROLE_MIGRATIONS
    if legacy is not None and legacy_path is not None
}
LEGACY_COMPATIBILITY_PATHS.update({
    "acs::ComponentSignatureId": "ecs/ComponentId.h",
    "acs::ComponentTypeId": "ecs/ComponentId.h",
    "acs::EventTypeId": "event/EventTypeId.h",
})
# 互換headerが公開する正規化済み型のqualified re-export契約。
LEGACY_COMPATIBILITY_REEXPORTS = frozenset({
    ("gameframework/Subsystem.h", ("acs", "game"), "acs::FSubsystem"),
    (
        "gameframework/SubsystemCollection.h",
        ("acs", "game"),
        "acs::FSubsystemCollection",
    ),
    (
        "gameframework/SubsystemRegistry.h",
        ("acs", "game"),
        "acs::FSubsystemAutoRegister",
    ),
    (
        "gameframework/SubsystemRegistry.h",
        ("acs", "game"),
        "acs::FSubsystemRegistry",
    ),
})
CANONICAL_OBJECT_AND_CLASS_TYPES = {
    canonical: (path, kind, prefix)
    for _, canonical, path, _, kind, prefix in REGISTERED_TYPE_ROLE_MIGRATIONS
}
MIGRATED_CANONICAL_OBJECT_AND_CLASS_TYPES = {
    canonical: (path, kind, prefix)
    for legacy, canonical, path, _, kind, prefix in REGISTERED_TYPE_ROLE_MIGRATIONS
    if legacy is not None
}
FOUNDATION_PRIMITIVE_ALIASES = {
    "byte": ("unsigned", "char"),
    "c8": ("char",),
    "c16": ("char16_t",),
    "c32": ("char32_t",),
    "f32": ("float",),
    "f64": ("double",),
    "i8": ("::", "int8_t"),
    "i16": ("::", "int16_t"),
    "i32": ("::", "int32_t"),
    "i64": ("::", "int64_t"),
    "iptr": ("::", "intptr_t"),
    "isize": ("::", "ptrdiff_t"),
    "u8": ("::", "uint8_t"),
    "u16": ("::", "uint16_t"),
    "u32": ("::", "uint32_t"),
    "u64": ("::", "uint64_t"),
    "uptr": ("::", "uintptr_t"),
    "usize": ("::", "size_t"),
}
CANONICAL_SCALAR_ALIASES = {
    "acs::FComponentSignatureId": ("ecs/ComponentId.h", ("u64",)),
    "acs::FComponentTypeId": ("ecs/ComponentId.h", ("u32",)),
    "acs::FEventTypeId": ("event/EventTypeId.h", ("u32",)),
}
PREMIGRATION_SCALAR_ALIASES = {
    "acs::ComponentSignatureId": ("ecs/ComponentId.h", ("u64",)),
    "acs::ComponentTypeId": ("ecs/ComponentId.h", ("u32",)),
    "acs::EventTypeId": ("event/EventTypeId.h", ("u32",)),
}
DEBT_SCHEMA_VERSION = 1
SCHEMA_VERSION = 3
DEBT_WAVE = "acf-role-review-wave-1"
DEFAULT_DEBT_ENTRY_COUNT = 0
DEFAULT_DEBT_SEMANTIC_SHA256 = "4F53CDA18C2BAA0C0354BB5F9A3ECBE5ED12AB4D8E11BA873C2F11161202B945"
DEFAULT_MIGRATION_DEBT = Path(os.path.abspath(__file__)).parent / "data" / "cpp_type_role_migration_debt.json"


@dataclass(frozen=True)
class FTypeDefinition:
    """意味分類に必要な一つの型定義。"""

    path: Path
    keyword: str
    name: str
    line: int
    column: int
    bases: tuple[str, ...]
    base_references: tuple[str, ...]
    body: tuple[FToken, ...]
    is_template: bool
    scope: tuple[str, ...]
    source_relative_path: Optional[str]
    has_local_scope: bool
    is_nested_type: bool
    visible_managed_references: tuple[tuple[str, str], ...]
    declared_managed_reference_scopes: tuple[tuple[str, tuple[str, ...]], ...]

    @property
    def qualified_name(self) -> str:
        """名前空間と外側の型を含む完全修飾名を返す。"""

        return "::".join((*self.scope, self.name))


@dataclass(frozen=True)
class FManagedVisibilityDeclaration:
    """名前空間scopeで宣言された名前と、直接解決できた管理基底を保持する。"""

    position: int
    owner_scope: tuple[str, ...]
    declared_name: Optional[str]
    managed_references: tuple[tuple[str, str], ...]
    namespace_target: Optional[tuple[str, ...]]


@dataclass(frozen=True)
class FTypeFeatures:
    """型から保守的に読み取った役割の根拠。"""

    has_private_or_protected: bool
    has_probable_data: bool
    has_pure_virtual: bool
    has_virtual: bool
    has_destructor: bool
    has_deleted_member: bool
    has_ownership: bool
    has_coordination: bool
    methods: tuple[str, ...]


@dataclass(frozen=True)
class FTypeAlias:
    """名前空間直下にある公開型alias。"""

    path: Path
    name: str
    line: int
    column: int
    namespace: tuple[str, ...]
    target: tuple[str, ...]
    is_template: bool
    declaration_kind: str
    source_relative_path: Optional[str]
    has_attributes: bool


@dataclass(frozen=True)
class FViolation:
    """一件の型役割違反。"""

    rule: str
    path: Path
    line: int
    column: int
    type_name: str
    expected_prefix: str
    message: str
    evidence: tuple[str, ...]
    qualified_type: Optional[str] = None
    role_reason: Optional[str] = None


@dataclass(frozen=True)
class FTypeRoleDebt:
    """レビュー未完了の一件の型役割移行debt。"""

    path: str
    qualified_type: str
    current_prefix: str
    status: str
    candidate_prefix: Optional[str]
    expected: None
    review_required: bool
    reason: str
    wave: str


@dataclass(frozen=True)
class FScanResult:
    """監査対象と検出結果。"""

    root: Path
    files: tuple[Path, ...]
    definitions: tuple[FTypeDefinition, ...]
    aliases: tuple[FTypeAlias, ...]
    violations: tuple[FViolation, ...]
    expected_prefix_counts: dict[str, int]
    migration_debt: tuple[FTypeRoleDebt, ...] = ()
    matched_migration_debt: tuple[FTypeRoleDebt, ...] = ()


DEBT_ENTRY_FIELDS = frozenset(
    {
        "path",
        "qualified_type",
        "current_prefix",
        "status",
        "candidate_prefix",
        "expected",
        "review_required",
        "reason",
        "wave",
    }
)


def _debt_sort_key(entry: FTypeRoleDebt) -> tuple[str, str]:
    """debt台帳の正規順序keyを返す。"""

    return entry.path, entry.qualified_type


def _validated_debt_path(value: object, index: int) -> str:
    """source相対POSIX pathを検証して返す。"""

    if (
        not isinstance(value, str)
        or not value
        or "\\" in value
        or ":" in value
        or _contains_control_character(value)
    ):
        raise ValueError(f"migration debt entries[{index}].pathがsource相対POSIX pathではありません")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts) or path.as_posix() != value:
        raise ValueError(f"migration debt entries[{index}].pathがsource tree外を示します")
    if PurePosixPath(value).suffix.casefold() not in PUBLIC_HEADER_SUFFIXES:
        raise ValueError(f"migration debt entries[{index}].pathは公開headerではありません")
    return value


def _contains_control_character(value: str) -> bool:
    """JSON stringにC0制御文字またはDELが含まれるか返す。"""

    return any(ord(character) < 0x20 or ord(character) == 0x7F for character in value)


def _reject_duplicate_json_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    """JSON objectの同名keyを全階層で拒否して辞書を返す。"""

    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"migration debt JSONに重複keyがあります: {key}")
        result[key] = value
    return result


def _reject_nonfinite_json_constant(value: str) -> object:
    """JSON規格外のNaNとInfinityを拒否する。"""

    raise ValueError(f"migration debt JSONに非有限値があります: {value}")


def _lexical_absolute_path(path: Path) -> Path:
    """symlinkやjunctionを解決せず絶対pathへ正規化する。"""

    return Path(os.path.abspath(os.fspath(path)))


def _is_reparse_path(stat_result: os.stat_result) -> bool:
    """lstat結果がsymlinkまたはWindows reparse pointかを返す。"""

    reparse_attribute = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    file_attributes = getattr(stat_result, "st_file_attributes", 0)
    return stat.S_ISLNK(stat_result.st_mode) or bool(file_attributes & reparse_attribute)


def _read_canonical_migration_debt(path: Path) -> tuple[Path, str]:
    """通常fileのcanonical UTF-8/LF bytesだけを読み、絶対pathとtextを返す。"""

    absolute_path = _lexical_absolute_path(path)
    current_path = Path(absolute_path.anchor)
    components = [current_path]
    for part in absolute_path.parts[1:]:
        current_path /= part
        components.append(current_path)
    for index, component in enumerate(components):
        try:
            stat_result = component.lstat()
        except OSError as error:
            raise ValueError(f"migration debt pathを確認できません: {component}") from error
        if _is_reparse_path(stat_result):
            raise ValueError(
                "migration debt pathにsymlink、junction、reparse pointを含められません: "
                f"{component}"
            )
        is_final = index + 1 == len(components)
        if is_final and not stat.S_ISREG(stat_result.st_mode):
            raise ValueError(f"migration debtは通常fileである必要があります: {component}")
        if not is_final and not stat.S_ISDIR(stat_result.st_mode):
            raise ValueError(f"migration debtの親pathはdirectoryである必要があります: {component}")

    payload = absolute_path.read_bytes()
    if payload.startswith(b"\xef\xbb\xbf"):
        raise ValueError("migration debt UTF-8にBOMを付けられません")
    if b"\r" in payload:
        raise ValueError("migration debtの改行はLFだけを使用する必要があります")
    if not payload.endswith(b"\n"):
        raise ValueError("migration debtは一つの最終LFで終わる必要があります")
    if payload.rstrip(b" \t\n") + b"\n" != payload:
        raise ValueError("migration debtの最終LFは一つだけである必要があります")
    try:
        source = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError("migration debtは厳密なUTF-8である必要があります") from error
    return absolute_path, source


def _load_migration_debt(path: Path) -> tuple[FTypeRoleDebt, ...]:
    """物理byte契約と厳密なschema、順序、重複を検証してdebt台帳を読む。"""

    absolute_path, source = _read_canonical_migration_debt(path)
    document = json.loads(
        source,
        object_pairs_hook=_reject_duplicate_json_keys,
        parse_constant=_reject_nonfinite_json_constant,
        strict=True,
    )
    if not isinstance(document, dict) or set(document) != {"schema_version", "entries"}:
        raise ValueError("migration debtはschema_versionとentriesだけを持つobjectである必要があります")
    if type(document["schema_version"]) is not int or document["schema_version"] != DEBT_SCHEMA_VERSION:
        raise ValueError(f"migration debt schema_versionは{DEBT_SCHEMA_VERSION}である必要があります")
    raw_entries = document["entries"]
    if not isinstance(raw_entries, list):
        raise ValueError("migration debt entriesはarrayである必要があります")
    entries: list[FTypeRoleDebt] = []
    for index, raw_entry in enumerate(raw_entries):
        if not isinstance(raw_entry, dict) or set(raw_entry) != DEBT_ENTRY_FIELDS:
            raise ValueError(f"migration debt entries[{index}]のfield集合が固定schemaと一致しません")
        entry_path = _validated_debt_path(raw_entry["path"], index)
        qualified_type = raw_entry["qualified_type"]
        if not isinstance(qualified_type, str) or re.fullmatch(r"[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*", qualified_type) is None:
            raise ValueError(f"migration debt entries[{index}].qualified_typeが完全修飾型名ではありません")
        current_prefix = raw_entry["current_prefix"]
        if current_prefix not in {"A", "C", "F", "I", "T", "E"}:
            raise ValueError(f"migration debt entries[{index}].current_prefixが役割接頭辞ではありません")
        status = raw_entry["status"]
        candidate_prefix = raw_entry["candidate_prefix"]
        if status not in {"candidate", "manual"}:
            raise ValueError(f"migration debt entries[{index}].statusがcandidate/manualではありません")
        if candidate_prefix is not None and candidate_prefix not in {"A", "C", "F", "I", "T", "E"}:
            raise ValueError(f"migration debt entries[{index}].candidate_prefixが役割接頭辞ではありません")
        if status == "candidate" and candidate_prefix is None:
            raise ValueError(f"migration debt entries[{index}]のcandidate statusにはcandidate_prefixが必要です")
        if raw_entry["expected"] is not None:
            raise ValueError(f"migration debt entries[{index}].expectedはreview完了までnull固定です")
        if raw_entry["review_required"] is not True:
            raise ValueError(f"migration debt entries[{index}].review_requiredはtrue固定です")
        reason = raw_entry["reason"]
        wave = raw_entry["wave"]
        if (
            not isinstance(reason, str)
            or not reason.strip()
            or reason != reason.strip()
            or _contains_control_character(reason)
        ):
            raise ValueError(f"migration debt entries[{index}].reasonが空または非正規です")
        if (
            not isinstance(wave, str)
            or not wave.strip()
            or wave != wave.strip()
            or _contains_control_character(wave)
        ):
            raise ValueError(f"migration debt entries[{index}].waveが空または非正規です")
        entries.append(
            FTypeRoleDebt(
                entry_path,
                qualified_type,
                current_prefix,
                status,
                candidate_prefix,
                None,
                True,
                reason,
                wave,
            )
        )
    keys = [(entry.path, entry.qualified_type) for entry in entries]
    if len(set(keys)) != len(keys):
        raise ValueError("migration debtにpath + qualified_typeの重複があります")
    if entries != sorted(entries, key=_debt_sort_key):
        raise ValueError("migration debt entriesがpath + qualified_typeの正規順ではありません")
    result = tuple(entries)
    if os.path.normcase(os.fspath(absolute_path)) == os.path.normcase(
        os.fspath(_lexical_absolute_path(DEFAULT_MIGRATION_DEBT))
    ):
        _verify_default_debt_baseline(result)
    return result


def _debt_semantic_sha256(entries: Sequence[FTypeRoleDebt]) -> str:
    """repo baseline用に固定順array rowsの正規JSON SHA-256を返す。"""

    rows = [
        [
            entry.path,
            entry.qualified_type,
            entry.current_prefix,
            entry.status,
            entry.candidate_prefix,
            entry.expected,
            entry.review_required,
            entry.reason,
            entry.wave,
        ]
        for entry in entries
    ]
    payload = json.dumps(
        rows,
        ensure_ascii=False,
        separators=(",", ":"),
        allow_nan=False,
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest().upper()


def _verify_default_debt_baseline(entries: Sequence[FTypeRoleDebt]) -> None:
    """review済みのdefault debt baselineをsemantic hashで完全freezeする。"""

    semantic_sha = _debt_semantic_sha256(entries)
    if len(entries) != DEFAULT_DEBT_ENTRY_COUNT or semantic_sha != DEFAULT_DEBT_SEMANTIC_SHA256:
        raise ValueError(
            "default migration debt baselineがreview済みの固定値と一致しません: "
            f"entries={len(entries)} semantic_sha256={semantic_sha}"
        )


def _tokens(source: str) -> list[FToken]:
    """明示的な#if 0と非トークン領域を除いたC++ tokenを返す。"""

    return lex_cpp(_without_if_zero_regions(source))


def _blank_source_line(line: str) -> str:
    """lexerの行番を保ったまま1行の内容を空白にする。"""

    return "".join(character if character in {"\r", "\n"} else " " for character in line)


def _is_zero_preprocessor_expression(expression: str) -> bool:
    """preprocessor条件が明示的な定数0だけか返す。"""

    return re.fullmatch(
        r"\s*\(?\s*0\s*\)?\s*(?://.*|/\*.*\*/\s*)?",
        expression,
    ) is not None


def _without_if_zero_regions(source: str) -> str:
    """`#if 0`で確実に無効なbranchを改行位置を保って除外する。"""

    parent_disabled_stack: list[bool] = []
    disabled = False
    output: list[str] = []
    directive_pattern = re.compile(
        r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$"
    )
    for line in source.splitlines(keepends=True):
        directive = directive_pattern.match(line)
        if directive is None:
            output.append(_blank_source_line(line) if disabled else line)
            continue
        command, expression = directive.groups()
        if command in {"if", "ifdef", "ifndef"}:
            parent_disabled_stack.append(disabled)
            disabled = disabled or (
                command == "if" and _is_zero_preprocessor_expression(expression)
            )
        elif command == "elif" and parent_disabled_stack:
            parent_disabled = parent_disabled_stack[-1]
            disabled = parent_disabled or _is_zero_preprocessor_expression(expression)
        elif command == "else" and parent_disabled_stack:
            disabled = parent_disabled_stack[-1]
        elif command == "endif" and parent_disabled_stack:
            disabled = parent_disabled_stack.pop()
        output.append(_blank_source_line(line))
    return "".join(output)


def _brace_pairs(tokens: Sequence[FToken]) -> dict[int, int]:
    """波括弧の開始位置から終了位置への対応を返す。"""

    stack: list[int] = []
    pairs: dict[int, int] = {}
    for position, token in enumerate(tokens):
        if token.text == "{":
            stack.append(position)
        elif token.text == "}" and stack:
            pairs[stack.pop()] = position
    return pairs


def _base_names(tokens: Sequence[FToken]) -> tuple[str, ...]:
    """基底型リストから修飾を除いた型名を返す。"""

    segments: list[list[FToken]] = [[]]
    angle_depth = 0
    parenthesis_depth = 0
    for token in tokens:
        if token.text == "<":
            angle_depth += 1
        elif token.text == ">" and angle_depth > 0:
            angle_depth -= 1
        elif token.text == "(":
            parenthesis_depth += 1
        elif token.text == ")" and parenthesis_depth > 0:
            parenthesis_depth -= 1
        if token.text == "," and angle_depth == 0 and parenthesis_depth == 0:
            segments.append([])
        else:
            segments[-1].append(token)

    names: list[str] = []
    for segment in segments:
        before_template: list[FToken] = []
        for token in segment:
            if token.text == "<":
                break
            before_template.append(token)
        candidates = [
            token.text
            for token in before_template
            if IDENTIFIER.match(token.text) and token.text not in ACCESS_WORDS
        ]
        if candidates:
            names.append(candidates[-1])
    return tuple(names)


def _base_references(tokens: Sequence[FToken]) -> tuple[str, ...]:
    """基底listから単純名または名前空間修飾名を返す。"""

    segments: list[list[FToken]] = [[]]
    angle_depth = 0
    parenthesis_depth = 0
    for token in tokens:
        if token.text == "<":
            angle_depth += 1
        elif token.text == ">" and angle_depth > 0:
            angle_depth -= 1
        elif token.text == "(":
            parenthesis_depth += 1
        elif token.text == ")" and parenthesis_depth > 0:
            parenthesis_depth -= 1
        if token.text == "," and angle_depth == 0 and parenthesis_depth == 0:
            segments.append([])
        else:
            segments[-1].append(token)

    references: list[str] = []
    for segment in segments:
        before_template: list[FToken] = []
        for token in segment:
            if token.text == "<":
                break
            before_template.append(token)
        last_name = next(
            (
                position
                for position in range(len(before_template) - 1, -1, -1)
                if IDENTIFIER.match(before_template[position].text)
                and before_template[position].text not in ACCESS_WORDS
            ),
            None,
        )
        if last_name is None:
            continue
        names = [before_template[last_name].text]
        cursor = last_name - 1
        while (
            cursor >= 1
            and before_template[cursor].text == "::"
            and IDENTIFIER.match(before_template[cursor - 1].text)
            and before_template[cursor - 1].text not in ACCESS_WORDS
        ):
            names.insert(0, before_template[cursor - 1].text)
            cursor -= 2
        reference = "::".join(names)
        if cursor >= 0 and before_template[cursor].text == "::":
            reference = f"::{reference}"
        references.append(reference)
    return tuple(references)


def _find_body_start(tokens: Sequence[FToken], start: int) -> Optional[int]:
    """型名の後ろから定義本体の開始位置を探す。"""

    angle_depth = 0
    parenthesis_depth = 0
    bracket_depth = 0
    brace_depth = 0
    for position in range(start, len(tokens)):
        text = tokens[position].text
        if text == "<":
            angle_depth += 1
        elif text == ">" and angle_depth > 0:
            angle_depth -= 1
        elif text == "(":
            parenthesis_depth += 1
        elif text == ")" and parenthesis_depth > 0:
            parenthesis_depth -= 1
        elif text == "[":
            bracket_depth += 1
        elif text == "]" and bracket_depth > 0:
            bracket_depth -= 1
        elif text == "{" and angle_depth == 0 and parenthesis_depth == 0 and bracket_depth == 0:
            return position
        elif text == ";" and angle_depth == 0 and parenthesis_depth == 0 and bracket_depth == 0:
            return None
    return None


def _definition_scope(
    tokens: Sequence[FToken],
    type_position: int,
    type_bodies: dict[int, str],
) -> tuple[tuple[str, ...], bool, bool]:
    """型位置を囲む名前空間・型名と、関数などの局所scope有無を返す。"""

    contexts: list[tuple[str, tuple[str, ...]]] = []
    for position, token in enumerate(tokens[:type_position]):
        if token.text == "{":
            enclosing_type = type_bodies.get(position)
            if enclosing_type is not None:
                contexts.append(("type", (enclosing_type,)))
                continue
            namespace = _namespace_for_brace(tokens, position)
            if namespace is not None:
                contexts.append(("namespace", namespace))
                continue
            contexts.append(("local", ()))
        elif token.text == "}" and contexts:
            contexts.pop()
    scope = tuple(
        name
        for kind, names in contexts
        if kind in {"namespace", "type"}
        for name in names
        if name != "__linkage__"
    )
    return (
        scope,
        any(kind == "local" for kind, _ in contexts),
        any(kind == "type" for kind, _ in contexts),
    )


def _qualified_using_reference(
    tokens: Sequence[FToken],
) -> Optional[tuple[bool, tuple[str, ...]]]:
    """対象が単純な修飾名なら、global指定と構成要素を返す。"""

    position = 0
    absolute = position < len(tokens) and tokens[position].text == "::"
    if absolute:
        position += 1
    names: list[str] = []
    while position < len(tokens):
        if not IDENTIFIER.match(tokens[position].text):
            return None
        names.append(tokens[position].text)
        position += 1
        if position == len(tokens):
            break
        if tokens[position].text != "::":
            return None
        position += 1
    return (absolute, tuple(names)) if names else None


def _managed_base_visibility_by_definition(
    tokens: Sequence[FToken],
    type_bodies: dict[int, str],
    body_starts: frozenset[int],
) -> dict[
    int,
    tuple[
        tuple[tuple[str, str], ...],
        tuple[tuple[str, tuple[str, ...]], ...],
    ],
]:
    """各型定義より前に同じ名前空間で可視になった管理基底を返す。"""

    external_names = {
        tuple(name.split("::")): name for name in EXTERNAL_MANAGED_BASES
    }
    external_namespaces = frozenset(name[:-1] for name in external_names)
    external_types = frozenset(external_names)
    contexts: list[tuple[str, tuple[str, ...]]] = []
    declarations: list[FManagedVisibilityDeclaration] = []
    visibility: dict[
        int,
        tuple[
            tuple[tuple[str, str], ...],
            tuple[tuple[str, tuple[str, ...]], ...],
        ],
    ] = {}

    def current_namespace() -> tuple[str, ...]:
        return tuple(
            name
            for kind, names in contexts
            if kind == "namespace"
            for name in names
            if name != "__linkage__"
        )

    def is_namespace_scope() -> bool:
        return all(
            kind in {"namespace", "inline", "anonymous", "linkage"}
            for kind, _ in contexts
        )

    def is_inline_namespace_brace(brace_position: int) -> bool:
        """波括弧がinline namespaceを開始する場合だけtrueを返す。"""

        statement_start = brace_position - 1
        while (
            statement_start >= 0
            and tokens[statement_start].text not in {";", "{", "}"}
        ):
            statement_start -= 1
        segment = tokens[statement_start + 1 : brace_position]
        return any(item.text == "namespace" for item in segment) and any(
            item.text == "inline" for item in segment
        )

    def namespace_imports(
        resolved_namespace: tuple[str, ...],
        visible_prefix: tuple[str, ...] = (),
    ) -> tuple[tuple[str, str], ...]:
        """外部名前空間から見える基底参照と完全修飾名を作る。"""

        return tuple(
            sorted(
                (
                    "::".join(
                        (*visible_prefix, *parts[len(resolved_namespace) :])
                    ),
                    qualified,
                )
                for parts, qualified in external_names.items()
                if parts[: len(resolved_namespace)] == resolved_namespace
            )
        )

    def resolve_namespace_reference(
        scope: tuple[str, ...],
        position: int,
        reference: tuple[bool, tuple[str, ...]],
    ) -> Optional[tuple[str, ...]]:
        """namespace参照をabsolute指定またはC++の親scope順で解決する。"""

        absolute, names = reference
        if absolute:
            return names if names in external_namespaces else None
        for depth in range(len(scope), -1, -1):
            owner_scope = scope[:depth]
            nearest = tuple(
                declaration
                for declaration in declarations
                if declaration.position < position
                and declaration.declared_name == names[0]
                and declaration.owner_scope == owner_scope
            )
            if nearest:
                targets = frozenset(
                    declaration.namespace_target for declaration in nearest
                )
                if len(targets) != 1 or None in targets:
                    return None
                target = next(iter(targets))
                candidate = (*target, *names[1:])
                return candidate if candidate in external_namespaces else None
            candidate = (*owner_scope, *names)
            if candidate in external_namespaces:
                return candidate
        return None

    def resolve_type_reference(
        scope: tuple[str, ...],
        position: int,
        reference: tuple[bool, tuple[str, ...]],
    ) -> Optional[tuple[str, ...]]:
        """型参照をnamespace aliasと同じscope規則で外部管理基底へ解決する。"""

        absolute, names = reference
        if absolute:
            return names if names in external_types else None
        if len(names) > 1:
            namespace_target = resolve_namespace_reference(
                scope,
                position,
                (False, names[:-1]),
            )
            if namespace_target is None:
                return None
            candidate = (*namespace_target, names[-1])
            return candidate if candidate in external_types else None
        for depth in range(len(scope), -1, -1):
            owner_scope = scope[:depth]
            nearest = tuple(
                declaration
                for declaration in declarations
                if declaration.position < position
                and declaration.declared_name == names[0]
                and declaration.owner_scope == owner_scope
            )
            if nearest:
                targets = frozenset(
                    target
                    for declaration in nearest
                    for visible_name, target in declaration.managed_references
                    if visible_name == names[0]
                )
                if len(targets) == 1:
                    return tuple(next(iter(targets)).split("::"))
                return None
            candidate = (*owner_scope, *names)
            if candidate in external_types:
                return candidate
        return None

    def append_named_declaration(
        position: int,
        owner_scope: tuple[str, ...],
        name: str,
        resolved_type: Optional[tuple[str, ...]] = None,
        namespace_target: Optional[tuple[str, ...]] = None,
    ) -> None:
        """宣言名を必ず記録し、管理対象へ解決できた場合だけ対応を付ける。"""

        managed_references = (
            ((name, external_names[resolved_type]),)
            if resolved_type is not None
            else namespace_imports(namespace_target, (name,))
            if namespace_target is not None
            else ()
        )
        declarations.append(
            FManagedVisibilityDeclaration(
                position,
                owner_scope,
                name,
                managed_references,
                namespace_target,
            )
        )

    for position, token in enumerate(tokens):
        if position in body_starts:
            definition_scope = current_namespace()
            active_records = tuple(
                record
                for record in declarations
                if record.position < position
                and definition_scope[: len(record.owner_scope)]
                == record.owner_scope
            )
            records_by_name: dict[
                str,
                list[FManagedVisibilityDeclaration],
            ] = {}
            for record in active_records:
                if record.declared_name is not None:
                    records_by_name.setdefault(record.declared_name, []).append(record)
            visible: set[tuple[str, str]] = set()
            declared_names = frozenset(records_by_name)
            declared_scopes: list[tuple[str, tuple[str, ...]]] = []
            for declared_name, records in records_by_name.items():
                nearest_depth = max(len(record.owner_scope) for record in records)
                nearest = tuple(
                    record
                    for record in records
                    if len(record.owner_scope) == nearest_depth
                )
                declared_scopes.append((declared_name, nearest[0].owner_scope))
                if all(record.managed_references for record in nearest):
                    visible.update(
                        managed_reference
                        for record in nearest
                        for managed_reference in record.managed_references
                    )
            visible.update(
                managed_reference
                for record in active_records
                if record.declared_name is None
                for managed_reference in record.managed_references
                if managed_reference[0].split("::", 1)[0] not in declared_names
            )
            visibility[position] = (
                tuple(sorted(visible)),
                tuple(sorted(declared_scopes)),
            )
            if is_namespace_scope():
                append_named_declaration(
                    position,
                    current_namespace(),
                    type_bodies[position],
                )

        if token.text == "using" and is_namespace_scope():
            end = position + 1
            while end < len(tokens) and tokens[end].text not in {";", "{", "}"}:
                end += 1
            if end < len(tokens) and tokens[end].text == ";":
                owner_scope = current_namespace()
                payload = tokens[position + 1 : end]
                if payload and payload[0].text == "namespace":
                    reference = _qualified_using_reference(payload[1:])
                    resolved_namespace = (
                        resolve_namespace_reference(
                            owner_scope,
                            position,
                            reference,
                        )
                        if reference is not None
                        else None
                    )
                    if resolved_namespace is not None:
                        declarations.append(
                            FManagedVisibilityDeclaration(
                                position,
                                owner_scope,
                                None,
                                namespace_imports(resolved_namespace),
                                None,
                            )
                        )
                else:
                    equals_position = (
                        _skip_attributes(payload, 1)
                        if payload and IDENTIFIER.match(payload[0].text)
                        else len(payload)
                    )
                    if (
                        equals_position < len(payload)
                        and payload[equals_position].text == "="
                    ):
                        alias_name = payload[0].text
                        reference = _qualified_using_reference(
                            payload[equals_position + 1 :]
                        )
                        resolved_type = (
                            resolve_type_reference(owner_scope, position, reference)
                            if reference is not None
                            else None
                        )
                        append_named_declaration(
                            position,
                            owner_scope,
                            alias_name,
                            resolved_type,
                        )
                    else:
                        reference = _qualified_using_reference(payload)
                        resolved_type = (
                            resolve_type_reference(
                                owner_scope,
                                position,
                                reference,
                            )
                            if reference is not None
                            else None
                        )
                        if reference is not None:
                            append_named_declaration(
                                position,
                                owner_scope,
                                reference[1][-1],
                                resolved_type,
                            )

        if token.text == "typedef" and is_namespace_scope():
            end = position + 1
            while end < len(tokens) and tokens[end].text not in {";", "{", "}"}:
                end += 1
            if end < len(tokens) and tokens[end].text == ";":
                owner_scope = current_namespace()
                payload = tokens[position + 1 : end]
                name_position = _typedef_name_position(payload)
                if name_position is not None:
                    alias_name = payload[name_position].text
                    reference = _qualified_using_reference(payload[:name_position])
                    resolved_type = (
                        resolve_type_reference(owner_scope, position, reference)
                        if reference is not None
                        else None
                    )
                    append_named_declaration(
                        position,
                        owner_scope,
                        alias_name,
                        resolved_type,
                    )

        if token.text == "namespace" and is_namespace_scope():
            end = position + 1
            while end < len(tokens) and tokens[end].text not in {";", "{", "}"}:
                end += 1
            if (
                end < len(tokens)
                and tokens[end].text == ";"
                and position + 3 < end
                and IDENTIFIER.match(tokens[position + 1].text)
                and tokens[position + 2].text == "="
            ):
                owner_scope = current_namespace()
                alias_name = tokens[position + 1].text
                reference = _qualified_using_reference(tokens[position + 3 : end])
                resolved_namespace = (
                    resolve_namespace_reference(
                        owner_scope,
                        position,
                        reference,
                    )
                    if reference is not None
                    else None
                )
                append_named_declaration(
                    position,
                    owner_scope,
                    alias_name,
                    namespace_target=resolved_namespace,
                )
            elif end < len(tokens) and tokens[end].text == "{":
                owner_scope = current_namespace()
                names = tuple(
                    item.text
                    for item in tokens[position + 1 : end]
                    if IDENTIFIER.match(item.text) and item.text != "inline"
                )
                if names:
                    declaration_scope = owner_scope
                    for name in names:
                        direct_target = (*declaration_scope, name)
                        if direct_target not in external_namespaces:
                            append_named_declaration(
                                position,
                                declaration_scope,
                                name,
                            )
                        declaration_scope = direct_target

        if token.text == "{":
            enclosing_type = type_bodies.get(position)
            if enclosing_type is not None:
                contexts.append(("type", (enclosing_type,)))
                continue
            namespace = _namespace_for_brace(tokens, position)
            if namespace == ("__linkage__",):
                contexts.append(("linkage", namespace))
            elif namespace:
                contexts.append(
                    (
                        "inline" if is_inline_namespace_brace(position) else "namespace",
                        namespace,
                    )
                )
            elif namespace is not None:
                contexts.append(("anonymous", ()))
            else:
                contexts.append(("local", ()))
        elif token.text == "}" and contexts:
            contexts.pop()
    return visibility


def _type_definitions(
    path: Path,
    tokens: Sequence[FToken],
    source_root: Optional[Path],
) -> tuple[FTypeDefinition, ...]:
    """C++トークンから本体を持つclass、struct、union、enumを集める。"""

    brace_pairs = _brace_pairs(tokens)
    parsed: list[
        tuple[str, FToken, tuple[str, ...], tuple[str, ...], tuple[FToken, ...], bool, int]
    ] = []
    pending_template = False
    position = 0
    while position < len(tokens):
        text = tokens[position].text
        if text == "template" and position + 1 < len(tokens) and tokens[position + 1].text == "<":
            position = _skip_balanced(tokens, position + 1, "<", ">")
            pending_template = True
            continue
        if text not in {"class", "struct", "union", "enum"}:
            if pending_template and text in {";", "{"}:
                pending_template = False
            position += 1
            continue
        if position > 0 and text in {"class", "struct"} and tokens[position - 1].text == "enum":
            position += 1
            continue
        if position > 0 and text != "enum" and tokens[position - 1].text in {"friend", "template"}:
            position += 1
            continue

        enum_declaration = text == "enum"
        name_token, declaration_end = _declaration_name(tokens, position, enum_declaration)
        if (
            name_token is None
            or declaration_end >= len(tokens)
            or tokens[declaration_end].text not in {":", "{"}
        ):
            position += 1
            continue
        body_start = _find_body_start(tokens, declaration_end)
        if body_start is None or body_start not in brace_pairs:
            position += 1
            continue
        base_tokens = tokens[declaration_end + 1 : body_start] if not enum_declaration and tokens[declaration_end].text == ":" else ()
        body_end = brace_pairs[body_start]
        parsed.append(
            (
                text,
                name_token,
                _base_names(base_tokens),
                _base_references(base_tokens),
                tuple(tokens[body_start + 1 : body_end]),
                pending_template and not enum_declaration,
                body_start,
            )
        )
        pending_template = False
        position += 1
    source_relative_path: Optional[str] = None
    if source_root is not None:
        try:
            source_relative_path = path.relative_to(source_root).as_posix()
        except ValueError:
            source_relative_path = None
    type_bodies = {
        body_start: name_token.text
        for _, name_token, _, _, _, _, body_start in parsed
    }
    managed_visibility = _managed_base_visibility_by_definition(
        tokens,
        type_bodies,
        frozenset(type_bodies),
    )
    definitions: list[FTypeDefinition] = []
    for keyword, name_token, bases, base_references, body, is_template, body_start in parsed:
        scope, has_local_scope, is_nested_type = _definition_scope(tokens, body_start, type_bodies)
        visible_references, declared_scopes = managed_visibility.get(
            body_start,
            ((), ()),
        )
        definitions.append(
            FTypeDefinition(
                path,
                keyword,
                name_token.text,
                name_token.line,
                name_token.column,
                bases,
                base_references,
                body,
                is_template,
                scope,
                source_relative_path,
                has_local_scope,
                is_nested_type,
                visible_references,
                declared_scopes,
            )
        )
    return tuple(definitions)


def _namespace_for_brace(tokens: Sequence[FToken], brace_position: int) -> Optional[tuple[str, ...]]:
    """波括弧が開始する名前空間を返し、通常のblockならNoneを返す。"""

    statement_start = brace_position - 1
    while statement_start >= 0 and tokens[statement_start].text not in {";", "{", "}"}:
        statement_start -= 1
    segment = tokens[statement_start + 1 : brace_position]
    if tuple(token.text for token in segment) == ("extern",):
        return ("__linkage__",)
    namespace_position = next(
        (position for position, token in enumerate(segment) if token.text == "namespace"),
        None,
    )
    if namespace_position is None:
        return None
    names = tuple(
        token.text
        for token in segment[namespace_position + 1 :]
        if IDENTIFIER.match(token.text) and token.text != "inline"
    )
    return names


def _is_public_namespace_context(contexts: Sequence[Optional[tuple[str, ...]]]) -> bool:
    """現在位置がACSの公開名前空間直下かを返す。"""

    if not contexts or any(context is None or not context for context in contexts):
        return False
    names = tuple(name for context in contexts for name in context or () if name != "__linkage__")
    if not names or names[0] != "acs":
        return False
    return not any(
        name.casefold() in DETAIL_NAMESPACE_NAMES
        or name.casefold().endswith("_detail")
        or name.casefold().endswith("_internal")
        for name in names
    )


def _template_precedes_alias(tokens: Sequence[FToken], alias_position: int) -> bool:
    """同じ宣言の先頭にtemplate指定があるかを返す。"""

    statement_start = alias_position - 1
    while statement_start >= 0 and tokens[statement_start].text not in {";", "{", "}"}:
        statement_start -= 1
    return any(token.text == "template" for token in tokens[statement_start + 1 : alias_position])


def _alias_target_end(tokens: Sequence[FToken], start: int) -> Optional[int]:
    """型alias右辺を終えるセミコロン位置を返す。"""

    angle_depth = 0
    parenthesis_depth = 0
    bracket_depth = 0
    brace_depth = 0
    for position in range(start, len(tokens)):
        text = tokens[position].text
        if text == "<":
            angle_depth += 1
        elif text == ">" and angle_depth > 0:
            angle_depth -= 1
        elif text == "(":
            parenthesis_depth += 1
        elif text == ")" and parenthesis_depth > 0:
            parenthesis_depth -= 1
        elif text == "[":
            bracket_depth += 1
        elif text == "]" and bracket_depth > 0:
            bracket_depth -= 1
        elif text == "{":
            brace_depth += 1
        elif text == "}" and brace_depth > 0:
            brace_depth -= 1
        elif text == ";" and angle_depth == 0 and parenthesis_depth == 0 and bracket_depth == 0 and brace_depth == 0:
            return position
    return None


def _skip_attributes(tokens: Sequence[FToken], position: int) -> int:
    """型alias名の直後にある属性を飛ばして次のtoken位置を返す。"""

    while position + 1 < len(tokens) and tokens[position].text == "[" and tokens[position + 1].text == "[":
        depth = 0
        while position < len(tokens):
            if tokens[position].text == "[":
                depth += 1
            elif tokens[position].text == "]":
                depth -= 1
                if depth == 0:
                    position += 1
                    break
            position += 1
    return position


def _typedef_name_position(declaration: Sequence[FToken]) -> Optional[int]:
    """typedef宣言から公開alias名のtoken位置を返す。"""

    attribute_positions: set[int] = set()
    position = 0
    while position + 1 < len(declaration):
        if declaration[position].text != "[" or declaration[position + 1].text != "[":
            position += 1
            continue
        depth = 0
        while position < len(declaration):
            attribute_positions.add(position)
            if declaration[position].text == "[":
                depth += 1
            elif declaration[position].text == "]":
                depth -= 1
                if depth == 0:
                    position += 1
                    break
            position += 1
    pointer_positions = [position for position, token in enumerate(declaration) if token.text == "*" and position not in attribute_positions]
    if pointer_positions:
        start = pointer_positions[-1] + 1
        return next(
            (position for position in range(start, len(declaration)) if position not in attribute_positions and IDENTIFIER.match(declaration[position].text)),
            None,
        )
    return next(
        (position for position in range(len(declaration) - 1, -1, -1) if position not in attribute_positions and IDENTIFIER.match(declaration[position].text)),
        None,
    )


def _namespace_aliases(path: Path, tokens: Sequence[FToken], source_root: Optional[Path]) -> tuple[FTypeAlias, ...]:
    """ACSの公開名前空間直下にある型aliasを集める。"""

    if path.suffix.casefold() not in PUBLIC_HEADER_SUFFIXES:
        return ()
    source_relative_path: Optional[str] = None
    if source_root is not None:
        try:
            source_relative_path = path.relative_to(source_root).as_posix()
        except ValueError:
            source_relative_path = None
    aliases: list[FTypeAlias] = []
    contexts: list[Optional[tuple[str, ...]]] = []
    position = 0
    while position < len(tokens):
        text = tokens[position].text
        if text == "{":
            contexts.append(_namespace_for_brace(tokens, position))
            position += 1
            continue
        if text == "}":
            if contexts:
                contexts.pop()
            position += 1
            continue
        if text not in {"typedef", "using"} or not _is_public_namespace_context(contexts):
            position += 1
            continue
        if text == "using":
            if position + 2 >= len(tokens) or not IDENTIFIER.match(tokens[position + 1].text):
                position += 1
                continue
            equals_position = _skip_attributes(tokens, position + 2)
            has_attributes = equals_position != position + 2
            if equals_position >= len(tokens) or tokens[equals_position].text != "=":
                position += 1
                continue
            target_start = equals_position + 1
            target_end = _alias_target_end(tokens, target_start)
            if target_end is None:
                position += 1
                continue
            name_token = tokens[position + 1]
            target = tuple(token.text for token in tokens[target_start:target_end])
        else:
            has_attributes = False
            target_start = position + 1
            target_end = _alias_target_end(tokens, target_start)
            if target_end is None:
                position += 1
                continue
            declaration = tokens[target_start:target_end]
            name_position = _typedef_name_position(declaration)
            if name_position is None:
                position = target_end + 1
                continue
            name_token = declaration[name_position]
            target = tuple(token.text for token in declaration[:name_position])
        namespace = tuple(name for context in contexts for name in context or () if name != "__linkage__")
        aliases.append(
            FTypeAlias(
                path,
                name_token.text,
                name_token.line,
                name_token.column,
                namespace,
                target,
                _template_precedes_alias(tokens, position),
                text,
                source_relative_path,
                has_attributes,
            )
        )
        position = target_end + 1
    return tuple(aliases)


def _qualified_alias_name(alias: FTypeAlias) -> str:
    """型aliasの完全修飾名を返す。"""

    return "::".join((*alias.namespace, alias.name))


def _qualified_alias_target(alias: FTypeAlias) -> Optional[str]:
    """単一型名だけから成るalias右辺を完全修飾して返す。"""

    if not alias.target:
        return None
    absolute = alias.target[0] == "::"
    cursor = 1 if absolute else 0
    names: list[str] = []
    expect_name = True
    while cursor < len(alias.target):
        text = alias.target[cursor]
        if expect_name:
            if not IDENTIFIER.match(text):
                return None
            names.append(text)
        elif text != "::":
            return None
        expect_name = not expect_name
        cursor += 1
    if not names or expect_name:
        return None
    if len(names) == 1:
        if absolute:
            return "::" + names[0]
        return "::".join((*alias.namespace, names[0]))
    if absolute or names[0] == "acs":
        return "::".join(names)
    return "::".join((*alias.namespace, *names))


def _alias_target_candidates(alias: FTypeAlias) -> tuple[str, ...]:
    """C++の親名前空間探索順にalias右辺の候補名を返す。"""

    if not alias.target:
        return ()
    target = list(alias.target)
    while target and target[0] in {"const", "volatile"}:
        target.pop(0)
    while target and target[-1] in {"const", "volatile"}:
        target.pop()
    if not target:
        return ()
    absolute = target[0] == "::"
    cursor = 1 if absolute else 0
    names: list[str] = []
    expect_name = True
    while cursor < len(target):
        text = target[cursor]
        if expect_name:
            if not IDENTIFIER.match(text):
                return ()
            names.append(text)
        elif text != "::":
            return ()
        expect_name = not expect_name
        cursor += 1
    if not names or expect_name:
        return ()
    if absolute:
        return ("::".join(names),)
    return tuple(
        "::".join((*alias.namespace[:depth], *names))
        for depth in range(len(alias.namespace), -1, -1)
    )


def _direct_scalar_target(alias: FTypeAlias) -> Optional[str]:
    """alias右辺が直接示すscalar型名を返す。"""

    if alias.is_template or any(text in {"(", ")", "*", "&", "[", "]", "<", ">"} for text in alias.target):
        return None
    names = tuple(text for text in alias.target if IDENTIFIER.match(text) and text not in {"const", "volatile"})
    if names and names[-1] in SCALAR_TYPE_NAMES:
        return names[-1]
    return None


def _scalar_alias_names(aliases: Sequence[FTypeAlias]) -> frozenset[str]:
    """直接または別alias経由でscalarを示す完全修飾名を返す。"""

    scalar_names = {
        _qualified_alias_name(alias)
        for alias in aliases
        if _direct_scalar_target(alias) is not None
    }
    changed = True
    while changed:
        changed = False
        for alias in aliases:
            if alias.is_template or _qualified_alias_name(alias) in scalar_names:
                continue
            if any(target in scalar_names for target in _alias_target_candidates(alias)):
                scalar_names.add(_qualified_alias_name(alias))
                changed = True
    return frozenset(scalar_names)


def _callback_alias_names(aliases: Sequence[FTypeAlias]) -> frozenset[str]:
    """直接または別alias経由で関数pointerを示す完全修飾名を返す。"""

    callback_names = {
        _qualified_alias_name(alias)
        for alias in aliases
        if "*" in alias.target and "(" in alias.target and alias.declaration_kind == "using"
    }
    changed = True
    while changed:
        changed = False
        for alias in aliases:
            if alias.is_template or _qualified_alias_name(alias) in callback_names:
                continue
            if any(target in callback_names for target in _alias_target_candidates(alias)):
                callback_names.add(_qualified_alias_name(alias))
                changed = True
    return frozenset(callback_names)


def _source_tree_root(root: Path) -> Optional[Path]:
    """監査rootを含む既知のsource tree rootを返す。"""

    candidate = root if root.is_dir() else root.parent
    for parent in (candidate, *candidate.parents):
        if parent.name.casefold() == "src":
            return parent
    return None


def _is_foundation_primitive_alias(alias: FTypeAlias) -> bool:
    """基盤で定義する小文字primitive aliasと完全一致するかを返す。"""

    return (
        alias.source_relative_path == "foundation/Types.h"
        and alias.namespace == ("acs",)
        and alias.declaration_kind == "using"
        and FOUNDATION_PRIMITIVE_ALIASES.get(alias.name) == alias.target
        and not alias.has_attributes
    )


def _is_deferred_scalar_alias(alias: FTypeAlias) -> bool:
    """次waveへ固定したAsset型番号aliasと完全一致するかを返す。"""

    return (
        alias.source_relative_path == "asset/Asset.h"
        and _qualified_alias_name(alias) == "acs::AssetType"
        and alias.target == ("u32",)
        and alias.declaration_kind == "using"
        and not alias.has_attributes
    )


def _is_exact_legacy_alias(alias: FTypeAlias) -> bool:
    """登録済み互換aliasが名前、場所、向きの固定契約と一致するかを返す。"""

    qualified_name = _qualified_alias_name(alias)
    expected_target = LEGACY_COMPATIBILITY_ALIASES.get(qualified_name)
    expected_names = expected_target.split("::") if expected_target is not None else []
    absolute_target: list[str] = ["::"]
    for name_index, name in enumerate(expected_names):
        if name_index != 0:
            absolute_target.append("::")
        absolute_target.append(name)
    valid_target_tokens = {(expected_names[-1],), tuple(absolute_target)} if expected_names else set()
    return (
        alias.target in valid_target_tokens
        and LEGACY_COMPATIBILITY_PATHS.get(qualified_name) == alias.source_relative_path
        and alias.declaration_kind == "using"
        and not alias.is_template
        and not alias.has_attributes
    )


def _is_exact_premigration_alias(alias: FTypeAlias) -> bool:
    """第一wave前の三つのscalar alias宣言と完全一致するかを返す。"""

    contract = PREMIGRATION_SCALAR_ALIASES.get(_qualified_alias_name(alias))
    return contract is not None and alias.source_relative_path == contract[0] and alias.target == contract[1] and alias.declaration_kind == "using" and not alias.is_template and not alias.has_attributes


def _audit_aliases(aliases: Sequence[FTypeAlias], enforce_exact_contracts: bool) -> tuple[FViolation, ...]:
    """公開scalar aliasと一時互換aliasの接頭辞・向きを検査する。"""

    scalar_names = _scalar_alias_names(aliases)
    callback_names = _callback_alias_names(aliases)
    violations: list[FViolation] = []
    for alias in aliases:
        qualified_name = _qualified_alias_name(alias)
        qualified_target = _qualified_alias_target(alias)
        canonical_contract = CANONICAL_SCALAR_ALIASES.get(qualified_name) if enforce_exact_contracts else None
        if canonical_contract is not None:
            expected_path, expected_target_tokens = canonical_contract
            if (
                alias.source_relative_path != expected_path
                or alias.target != expected_target_tokens
                or alias.declaration_kind != "using"
                or alias.is_template
                or alias.has_attributes
            ):
                violations.append(
                    FViolation(
                        "ACS-R020d",
                        alias.path,
                        alias.line,
                        alias.column,
                        alias.name,
                        "F",
                        "正規scalar aliasの実体が固定契約と一致しません。",
                        (f"期待する宣言: {expected_path} -> {' '.join(expected_target_tokens)}",),
                    )
                )
            continue
        expected_target = LEGACY_COMPATIBILITY_ALIASES.get(qualified_name)
        if expected_target is not None:
            expected_path = LEGACY_COMPATIBILITY_PATHS[qualified_name]
            expected_prefix = expected_target.rsplit("::", 1)[-1][0]
            if not _is_exact_legacy_alias(alias):
                violations.append(
                    FViolation(
                        "ACS-R020d",
                        alias.path,
                        alias.line,
                        alias.column,
                        alias.name,
                        expected_prefix,
                        "一時互換aliasの正規型が契約と一致しません。",
                        (f"期待する宣言: {expected_path} -> {expected_target}",),
                    )
                )
            continue
        if qualified_name == "acs::AssetType":
            if not _is_deferred_scalar_alias(alias):
                violations.append(
                    FViolation(
                        "ACS-R020d",
                        alias.path,
                        alias.line,
                        alias.column,
                        alias.name,
                        "F",
                        "次waveへ保留したAsset型番号aliasが固定契約と一致しません。",
                        ("期待する宣言: asset/Asset.h -> using AssetType = u32",),
                    )
                )
            continue
        if alias.name in FOUNDATION_PRIMITIVE_ALIASES:
            if not _is_foundation_primitive_alias(alias):
                violations.append(
                    FViolation(
                        "ACS-R020d",
                        alias.path,
                        alias.line,
                        alias.column,
                        alias.name,
                        "F",
                        "primitive aliasが基盤の固定宣言と一致しません。",
                        ("foundation/Types.hの正規宣言だけを許可する",),
                    )
                )
            continue
        if alias.declaration_kind == "typedef":
            violations.append(
                FViolation(
                    "ACS-R020d",
                    alias.path,
                    alias.line,
                    alias.column,
                    alias.name,
                    "F",
                    "公開名前空間へtypedefを追加できません。",
                    ("新規公開aliasはusingで宣言する",),
                )
            )
            continue
        if alias.is_template or qualified_name in callback_names:
            continue
        if qualified_name not in scalar_names or _is_foundation_primitive_alias(alias) or _is_deferred_scalar_alias(alias):
            continue
        if not re.match(r"^F[A-Z0-9]", alias.name):
            violations.append(
                FViolation(
                    "ACS-R020d",
                    alias.path,
                    alias.line,
                    alias.column,
                    alias.name,
                    "F",
                    "公開scalar value aliasにはF接頭辞が必要です。",
                    ("名前空間直下の値alias",),
                )
            )
    return tuple(violations)


def _is_full_source_scan(root: Path) -> bool:
    """必須alias契約を検査する製品source全走査かを返す。"""

    return root.is_dir() and root.name.casefold() == "src"


def _audit_required_alias_presence(root: Path, aliases: Sequence[FTypeAlias]) -> tuple[FViolation, ...]:
    """全source走査で必須互換alias八件と正規scalar alias三件の存在を検査する。"""

    if not _is_full_source_scan(root):
        return ()
    violations: list[FViolation] = []
    for qualified_name, expected_path in sorted(LEGACY_COMPATIBILITY_PATHS.items()):
        matches = [alias for alias in aliases if _qualified_alias_name(alias) == qualified_name]
        if len(matches) == 1:
            continue
        diagnostic_path = root / expected_path
        violations.append(
            FViolation(
                "ACS-R020d",
                diagnostic_path,
                1,
                1,
                qualified_name.rsplit("::", 1)[-1],
                LEGACY_COMPATIBILITY_ALIASES[qualified_name].rsplit("::", 1)[-1][0],
                "必須の一時互換aliasは正確に一件だけ必要です。",
                (f"検出数: {len(matches)}",),
            )
        )
    for canonical_name, (expected_path, _) in sorted(CANONICAL_SCALAR_ALIASES.items()):
        legacy_name = next(name for name, target in LEGACY_COMPATIBILITY_ALIASES.items() if target == canonical_name)
        legacy_matches = [alias for alias in aliases if _qualified_alias_name(alias) == legacy_name]
        if len(legacy_matches) == 1 and _is_exact_premigration_alias(legacy_matches[0]):
            continue
        matches = [alias for alias in aliases if _qualified_alias_name(alias) == canonical_name]
        if len(matches) == 1:
            continue
        violations.append(
            FViolation(
                "ACS-R020d",
                root / expected_path,
                1,
                1,
                canonical_name.rsplit("::", 1)[-1],
                canonical_name.rsplit("::", 1)[-1][0],
                "必須の正規scalar aliasは正確に一件だけ必要です。",
                (f"検出数: {len(matches)}",),
            )
        )
    return tuple(violations)


def _audit_required_canonical_definitions(
    root: Path,
    definitions: Sequence[FTypeDefinition],
) -> tuple[FViolation, ...]:
    """AObjectとC4の正規定義が名前、場所、宣言種別の固定契約と一致するか検査する。"""

    if not _is_full_source_scan(root):
        return ()
    violations: list[FViolation] = []
    for qualified_name, (expected_path, expected_keyword, expected_prefix) in sorted(
        CANONICAL_OBJECT_AND_CLASS_TYPES.items()
    ):
        matches = [definition for definition in definitions if definition.qualified_name == qualified_name]
        valid = (
            len(matches) == 1
            and matches[0].source_relative_path == expected_path
            and matches[0].keyword == expected_keyword
            and not matches[0].is_template
            and not matches[0].has_local_scope
        )
        if valid:
            continue
        diagnostic = matches[0] if matches else None
        violations.append(
            FViolation(
                "ACS-R020d",
                diagnostic.path if diagnostic is not None else root / expected_path,
                diagnostic.line if diagnostic is not None else 1,
                diagnostic.column if diagnostic is not None else 1,
                qualified_name.rsplit("::", 1)[-1],
                expected_prefix,
                "正規object/class定義が固定契約と一致しません。",
                (
                    f"期待する定義: {expected_path} -> {expected_keyword} {qualified_name}",
                    f"検出数: {len(matches)}",
                ),
                qualified_name,
                "hard-canonical",
            )
        )
    canonical_targets = frozenset(CANONICAL_OBJECT_AND_CLASS_TYPES)
    for legacy_name, canonical_name in sorted(LEGACY_COMPATIBILITY_ALIASES.items()):
        if canonical_name not in canonical_targets:
            continue
        for definition in definitions:
            if definition.qualified_name != legacy_name:
                continue
            violations.append(
                FViolation(
                    "ACS-R020d",
                    definition.path,
                    definition.line,
                    definition.column,
                    definition.name,
                    canonical_name.rsplit("::", 1)[-1][0],
                    "旧互換名を独立した型として再定義できません。",
                    (f"正規名を使用する: {canonical_name}",),
                    definition.qualified_name,
                    "legacy-definition",
                )
            )
    return tuple(violations)


def _audit_legacy_alias_uses(
    path: Path,
    tokens: Sequence[FToken],
    aliases: Sequence[FTypeAlias],
    macro_definition_lines: frozenset[int],
) -> tuple[FViolation, ...]:
    """一時互換名がalias宣言以外の製品sourceへ再流入していないかを検査する。"""

    declared_positions = {
        (alias.path, alias.name, alias.line, alias.column)
        for alias in aliases
        if _qualified_alias_name(alias) in LEGACY_COMPATIBILITY_ALIASES
    }
    declared_names = {
        alias.name
        for alias in aliases
        if _is_exact_legacy_alias(alias)
    }
    legacy_names = {
        qualified_name.rsplit("::", 1)[-1]: qualified_target.rsplit("::", 1)[-1]
        for qualified_name, qualified_target in LEGACY_COMPATIBILITY_ALIASES.items()
        if qualified_name.rsplit("::", 1)[-1] in declared_names
    }
    allowed_positions = {
        *_legacy_using_declaration_positions(
            path,
            tokens,
            frozenset(legacy_names),
        ),
        *_legacy_identity_macro_positions(
            tokens,
            frozenset(legacy_names),
            macro_definition_lines,
        ),
    }
    violations: list[FViolation] = []
    for position, token in enumerate(tokens):
        canonical_name = legacy_names.get(token.text)
        if (
            canonical_name is None
            or (path, token.text, token.line, token.column) in declared_positions
            or (token.text, token.line, token.column) in allowed_positions
        ):
            continue
        violations.append(
            FViolation(
                "ACS-R020e",
                path,
                token.line,
                token.column,
                token.text,
                canonical_name[0],
                "一時互換名を通常の製品sourceで使用できません。",
                (f"正規名を使用する: {canonical_name}",),
            )
        )
    return tuple(violations)


def _legacy_using_declaration_positions(
    path: Path,
    tokens: Sequence[FToken],
    legacy_names: frozenset[str],
) -> frozenset[tuple[str, int, int]]:
    """登録済みheaderとnamespace内のqualified re-exportだけを許可する。"""

    allowed: set[tuple[str, int, int]] = set()
    source_root = _source_tree_root(path)
    if source_root is None:
        return frozenset()
    try:
        relative_path = path.relative_to(source_root).as_posix()
    except ValueError:
        return frozenset()
    for position, token in enumerate(tokens):
        if token.text != "using":
            continue
        cursor = position + 1
        declaration: list[FToken] = []
        while cursor < len(tokens) and tokens[cursor].text != ";":
            if tokens[cursor].text in {"=", "{", "}"}:
                declaration = []
                break
            declaration.append(tokens[cursor])
            cursor += 1
        if not declaration or cursor >= len(tokens):
            continue
        name_token = declaration[-1]
        if name_token.text not in legacy_names or "::" not in {
            item.text for item in declaration
        }:
            continue
        scope, local_scope, type_scope = _definition_scope(tokens, position, {})
        if local_scope or type_scope:
            continue
        argument = "".join(item.text for item in declaration)
        absolute = argument.startswith("::")
        argument = argument.lstrip(":")
        resolved_name = argument if absolute else "::".join((*scope, argument))
        if (relative_path, scope, resolved_name) in LEGACY_COMPATIBILITY_REEXPORTS:
            allowed.add((name_token.text, name_token.line, name_token.column))
            continue
        expected_path = LEGACY_COMPATIBILITY_PATHS.get(resolved_name)
        if expected_path != relative_path:
            continue
        segments = resolved_name.split("::")
        if segments[:2] == ["acs", "game"]:
            expected_scope = ("acs",)
        elif segments[:1] == ["acs"] and relative_path.startswith("gameframework/"):
            expected_scope = ("acs", "game")
        else:
            continue
        if scope == expected_scope:
            allowed.add((name_token.text, name_token.line, name_token.column))
    return frozenset(allowed)


def _legacy_identity_macro_positions(
    tokens: Sequence[FToken],
    legacy_names: frozenset[str],
    macro_definition_lines: frozenset[int],
) -> frozenset[tuple[str, int, int]]:
    """保存済み実行時IDを維持するmacro引数内の登録済み旧名だけを許可する。"""

    allowed: set[tuple[str, int, int]] = set()
    for position, token in enumerate(tokens):
        allowed_argument_indexes = LEGACY_IDENTITY_MACRO_ARGUMENTS.get(token.text)
        if (
            allowed_argument_indexes is None
            or token.line in macro_definition_lines
            or position + 1 >= len(tokens)
            or tokens[position + 1].text != "("
        ):
            continue
        depth = 1
        cursor = position + 2
        arguments: list[list[FToken]] = [[]]
        while cursor < len(tokens) and depth > 0:
            argument_token = tokens[cursor]
            if argument_token.text == "(":
                depth += 1
            elif argument_token.text == ")":
                depth -= 1
            if depth == 1 and argument_token.text == ",":
                arguments.append([])
            elif depth > 0:
                arguments[-1].append(argument_token)
            cursor += 1
        if depth != 0:
            continue
        for argument_index in sorted(allowed_argument_indexes):
            if argument_index >= len(arguments):
                continue
            argument = arguments[argument_index]
            absolute = bool(argument and argument[0].text == "::")
            while argument and argument[0].text == "::":
                argument = argument[1:]
            if (
                not argument
                or any(
                    not IDENTIFIER.match(item.text)
                    if item_index % 2 == 0
                    else item.text != "::"
                    for item_index, item in enumerate(argument)
                )
            ):
                continue
            qualified_argument = "".join(item.text for item in argument)
            if absolute and "::" not in qualified_argument:
                continue
            matches = tuple(
                qualified_name
                for qualified_name in LEGACY_COMPATIBILITY_ALIASES
                if qualified_name == qualified_argument
                or (
                    not absolute and "::" not in qualified_argument
                    and qualified_name.rsplit("::", 1)[-1] == qualified_argument
                )
            )
            if len(matches) == 1 and argument[-1].text in legacy_names:
                allowed.add(
                    (
                        argument[-1].text,
                        argument[-1].line,
                        argument[-1].column,
                    )
                )
    return frozenset(allowed)


def _macro_definition_lines(source: str) -> frozenset[int]:
    """`#define`本体に含まれる物理行番号を返す。"""

    lines = source.splitlines()
    definition_lines: set[int] = set()
    position = 0
    while position < len(lines):
        if re.match(r"^\s*#\s*define\b", lines[position]) is None:
            position += 1
            continue
        while position < len(lines):
            definition_lines.add(position + 1)
            continued = lines[position].rstrip().endswith("\\")
            position += 1
            if not continued:
                break
    return frozenset(definition_lines)


def _registered_object_references(
    tokens: Sequence[FToken],
    macro_definition_lines: frozenset[int],
) -> frozenset[tuple[tuple[str, ...], str]]:
    """ACS_OBJECT系の実呼び出しscopeと第一引数の型参照を集める。"""

    references: set[tuple[tuple[str, ...], str]] = set()
    for position, token in enumerate(tokens):
        if (
            token.line in macro_definition_lines
            or token.text not in OBJECT_MACROS
            or position + 1 >= len(tokens)
            or tokens[position + 1].text != "("
        ):
            continue
        argument: list[str] = []
        depth = 1
        cursor = position + 2
        while cursor < len(tokens) and depth > 0:
            text = tokens[cursor].text
            if text == "(":
                depth += 1
            elif text == ")":
                depth -= 1
            elif text == "," and depth == 1:
                break
            elif depth == 1:
                argument.append(text)
            cursor += 1
        while argument and argument[0] == "::":
            argument.pop(0)
        if (
            not argument
            or any(
                not IDENTIFIER.match(text) if index % 2 == 0 else text != "::"
                for index, text in enumerate(argument)
            )
        ):
            continue
        scope, _, _ = _definition_scope(tokens, position, {})
        references.add((scope, "".join(argument)))
    return frozenset(references)


def _top_level_statements(body: Sequence[FToken]) -> tuple[tuple[FToken, ...], ...]:
    """型本体の直下にあるセミコロン単位の宣言を返す。"""

    statements: list[tuple[FToken, ...]] = []
    current: list[FToken] = []
    brace_depth = 0
    for position, token in enumerate(body):
        if token.text == "{":
            brace_depth += 1
        elif token.text == "}" and brace_depth > 0:
            brace_depth -= 1
        if brace_depth == 0:
            current.append(token)
            if token.text == ";":
                statements.append(tuple(current))
                current = []
            elif token.text == "}" and "(" in tuple(item.text for item in current):
                next_text = body[position + 1].text if position + 1 < len(body) else ""
                if next_text not in {";", ",", "{"}:
                    statements.append(tuple(current))
                    current = []
    if current:
        statements.append(tuple(current))
    return tuple(statements)


def _statement_method_name(statement: Sequence[FToken]) -> Optional[str]:
    """initializer呼び出しと関数pointer fieldを除いたメンバー関数名を返す。"""

    ignored = frozenset({"alignas", "decltype", "if", "noexcept", "requires", "sizeof", "static_assert"})
    for position, token in enumerate(statement):
        if token.text != "(" or position == 0:
            continue
        candidate = statement[position - 1].text
        prefix = tuple(item.text for item in statement[:position])
        if "operator" in prefix:
            return "operator"
        if (
            IDENTIFIER.match(candidate)
            and candidate not in ignored
            and "static" not in prefix
            and "=" not in prefix
            and not (position >= 2 and statement[position - 2].text == "::")
            and not (
                position + 1 < len(statement)
                and statement[position + 1].text == "*"
            )
        ):
            return candidate
    return None


def _method_names(statements: Sequence[Sequence[FToken]]) -> tuple[str, ...]:
    """メンバー関数らしい宣言から関数名を集める。"""

    return tuple(
        sorted(
            {
                name
                for statement in statements
                if (name := _statement_method_name(statement)) is not None
            }
        )
    )


def _has_probable_data(statements: Sequence[Sequence[FToken]]) -> bool:
    """関数やaliasではないインスタンスデータ宣言がありそうかを返す。"""

    skipped_heads = frozenset({"class", "enum", "friend", "static_assert", "struct", "template", "typedef", "union", "using"})
    for statement in statements:
        texts = [token.text for token in statement]
        while len(texts) >= 2 and texts[0] in {"private", "protected", "public"} and texts[1] == ":":
            texts = texts[2:]
        if (
            not texts
            or texts[0] in skipped_heads
            or _statement_method_name(statement) is not None
        ):
            continue
        if "static" in texts or texts == [";"]:
            continue
        if any(IDENTIFIER.match(text) for text in texts):
            return True
    return False


def _features(definition: FTypeDefinition) -> FTypeFeatures:
    """型本体から役割分類に使う特徴を取り出す。"""

    statements = _top_level_statements(definition.body)
    body_text = tuple(
        token.text for statement in statements for token in statement
    )
    methods = _method_names(statements)
    has_pure_virtual = any(
        "virtual" in tuple(token.text for token in statement)
        and any(
            statement[position].text == "=" and position + 1 < len(statement) and statement[position + 1].text == "0"
            for position in range(len(statement))
        )
        for statement in statements
    )
    return FTypeFeatures(
        has_private_or_protected=any(
            body_text[position] in {"private", "protected"}
            and position + 1 < len(body_text)
            and body_text[position + 1] == ":"
            for position in range(len(body_text))
        ),
        has_probable_data=_has_probable_data(statements),
        has_pure_virtual=has_pure_virtual,
        has_virtual="virtual" in body_text,
        has_destructor=any(
            body_text[position] == "~"
            and position + 1 < len(body_text)
            and body_text[position + 1] == definition.name
            for position in range(len(body_text))
        ),
        has_deleted_member=any(
            body_text[position] == "="
            and position + 1 < len(body_text)
            and body_text[position + 1] == "delete"
            for position in range(len(body_text))
        ),
        has_ownership=any(text in OWNERSHIP_TOKENS for text in body_text),
        has_coordination=any(text in COORDINATION_TOKENS for text in body_text),
        methods=methods,
    )


def _role_stem(name: str) -> str:
    """型接頭辞を除いた役割判定用の名前を返す。"""

    return name[1:] if ROLE_NAME.match(name) else name


def _ends_with_word(name: str, words: frozenset[str]) -> bool:
    """型名が役割語で終わるかを返す。"""

    return any(name == word or name.endswith(word) for word in words)


def _is_interface(definition: FTypeDefinition, features: FTypeFeatures) -> bool:
    """データを持たず、仮想操作または仮想破棄だけを公開する型かを判定する。"""

    if features.has_probable_data:
        return False
    return features.has_pure_virtual or (
        definition.name.startswith("I") and features.has_virtual
    )


def _is_public_role_definition(definition: FTypeDefinition) -> bool:
    """公開role inventoryに含める名前空間直下のheader型かを返す。"""

    if (
        definition.path.suffix.casefold() not in PUBLIC_HEADER_SUFFIXES
        or definition.keyword not in {"class", "struct"}
        or definition.has_local_scope
        or definition.is_nested_type
        or not definition.scope
        or definition.scope[0] != "acs"
    ):
        return False
    return not any(
        name.casefold() in DETAIL_NAMESPACE_NAMES
        or name.casefold().endswith("_detail")
        or name.casefold().endswith("_internal")
        for name in definition.scope
    )


def _expected_prefix(
    definition: FTypeDefinition,
    features: FTypeFeatures,
    managed_names: frozenset[str],
) -> tuple[str, tuple[str, ...], str]:
    """型の意味的特徴から期待する接頭辞と根拠を返す。"""

    if definition.keyword == "enum":
        return "E", ("列挙型",), "enum"
    if definition.is_template:
        return "T", ("template型",), "template"
    if definition.qualified_name in managed_names:
        return "A", ("AObject継承またはACS_OBJECT登録",), "managed-object"

    stem = _role_stem(definition.name)
    value_named = _ends_with_word(stem, VALUE_WORDS)
    behavior_named = _ends_with_word(stem, BEHAVIOR_WORDS)
    lifecycle_methods = tuple(method for method in features.methods if method in LIFECYCLE_METHODS)
    behavior_methods = tuple(
        method for method in features.methods if method in OBVIOUS_BEHAVIOR_METHODS
    )
    service_named = (
        behavior_named
        or bool(lifecycle_methods)
        or bool(behavior_methods)
        or features.has_coordination
    )
    if _is_interface(definition, features) and (
        definition.name.startswith("I") or not value_named and not service_named
    ):
        return "I", ("データを持たない仮想interface",), "interface"
    if stem.startswith("Scoped"):
        return "F", ("有効範囲内の資源を一つの値として管理",), "scoped-value"
    if (
        definition.keyword == "class"
        and features.has_probable_data
        and not behavior_named
        and not features.methods
        and not features.has_virtual
        and not features.has_destructor
        and not features.has_deleted_member
        and not features.has_ownership
        and not features.has_coordination
    ):
        return "F", ("操作を持たないデータ中心class",), "data-only"
    if value_named and not service_named:
        evidence = ["値を表す型名"]
        if features.has_private_or_protected:
            evidence.append("値の内部表現を非公開")
        return "F", tuple(evidence), "named-value"
    if definition.keyword in {"struct", "union"} and not service_named:
        return "F", ("公開データ中心",), "data-only"

    evidence: list[str] = []
    if behavior_named:
        evidence.append("共有利用または処理を担うservice型名")
    if lifecycle_methods:
        evidence.append("状態や寿命を動かす操作: " + ", ".join(lifecycle_methods))
    if behavior_methods:
        evidence.append("明白な処理操作: " + ", ".join(behavior_methods))
    if features.has_virtual:
        evidence.append("仮想動作は補助根拠")
    if features.has_coordination:
        evidence.append("非同期または共有状態を協調")
    if features.has_ownership:
        evidence.append("所有先を持つ")
    if features.has_deleted_member:
        evidence.append("コピーまたは移動の制限は補助根拠")
    if features.has_destructor:
        evidence.append("破棄処理は補助根拠")
    if features.has_private_or_protected:
        evidence.append("非公開状態は補助根拠")
    if not evidence:
        evidence.append("具象の機能を持つclass")
    return "C", tuple(evidence), "functional-evidence" if service_named else "manual-review"


def _managed_names(
    definitions: Sequence[FTypeDefinition],
    registered_references: frozenset[tuple[tuple[str, ...], str]],
) -> frozenset[str]:
    """acs::AObject実継承と実際のACS_OBJECT登録を修飾名でたどる。"""

    qualified_names = frozenset(
        {definition.qualified_name for definition in definitions}
        | set(EXTERNAL_MANAGED_BASES)
    )
    managed = set(EXTERNAL_MANAGED_BASES)
    for scope, reference in registered_references:
        resolved = _resolve_qualified_reference(
            scope,
            reference,
            qualified_names,
            f"ACS_OBJECT登録: {'::'.join(scope) or '<global>'}",
        )
        if resolved is None:
            raise ValueError(
                f"ACS_OBJECT登録型を解決できません: {'::'.join(scope) or '<global>'} -> {reference}"
            )
        managed.add(resolved)
    resolved_bases = {
        definition.qualified_name: tuple(
            resolved
            for reference in definition.base_references
            if (
                resolved := _resolve_base_reference(
                    definition,
                    reference,
                    qualified_names,
                )
            )
            is not None
        )
        for definition in definitions
    }
    changed = True
    while changed:
        changed = False
        for definition in definitions:
            if definition.qualified_name in managed:
                continue
            if any(base in managed for base in resolved_bases[definition.qualified_name]):
                managed.add(definition.qualified_name)
                changed = True
    return frozenset(managed)


def _audit_definitions(
    definitions: Sequence[FTypeDefinition],
    managed_names: frozenset[str],
) -> tuple[tuple[FViolation, ...], dict[str, int]]:
    """型定義の意味に対応する接頭辞を検査する。"""

    violations: list[FViolation] = []
    counts: dict[str, int] = {}
    for definition in definitions:
        features = _features(definition)
        hard_contract = CANONICAL_OBJECT_AND_CLASS_TYPES.get(definition.qualified_name)
        if hard_contract is None:
            expected, evidence, role_reason = _expected_prefix(
                definition,
                features,
                managed_names,
            )
        else:
            expected = hard_contract[2]
            evidence = ("レビュー済みのhard canonical型役割",)
            role_reason = "hard-canonical"
        counts[expected] = counts.get(expected, 0) + 1
        observed = definition.name[0] if ROLE_NAME.match(definition.name) else ""
        if observed != expected:
            if (
                expected == "C"
                and definition.source_relative_path is not None
                and not _is_public_role_definition(definition)
            ):
                continue
            violations.append(
                FViolation(
                    "ACS-R020c",
                    definition.path,
                    definition.line,
                    definition.column,
                    definition.name,
                    expected,
                    f"型の役割は{expected}接頭辞に対応します。",
                    evidence,
                    definition.qualified_name,
                    role_reason,
                )
            )

    violations.sort(
        key=lambda item: (
            item.path.as_posix().casefold(),
            item.line,
            item.column,
            item.rule,
            item.type_name,
        )
    )
    return tuple(violations), dict(sorted(counts.items()))


def _debt_entries_for_scan(
    root: Path,
    source_root: Optional[Path],
    entries: Sequence[FTypeRoleDebt],
) -> tuple[FTypeRoleDebt, ...]:
    """現在の全体またはmodule走査に属するdebt entryだけを返す。"""

    if source_root is None:
        return ()
    try:
        relative_root = root.relative_to(source_root)
    except ValueError:
        return ()
    if relative_root == Path("."):
        return tuple(entries)
    relative = relative_root.as_posix()
    if root.is_file():
        return tuple(entry for entry in entries if entry.path == relative)
    prefix = relative.rstrip("/") + "/"
    return tuple(entry for entry in entries if entry.path.startswith(prefix))


def _debt_diagnostic(
    root: Path,
    entry: FTypeRoleDebt,
    definition: Optional[FTypeDefinition],
    message: str,
    evidence: tuple[str, ...],
) -> FViolation:
    """debt台帳とsourceのずれを示す診断を作る。"""

    return FViolation(
        "ACS-R020f",
        definition.path if definition is not None else root / entry.path,
        definition.line if definition is not None else 1,
        definition.column if definition is not None else 1,
        entry.qualified_type.rsplit("::", 1)[-1],
        entry.candidate_prefix or "",
        message,
        evidence,
        entry.qualified_type,
        "migration-debt-contract",
    )


def _resolve_qualified_reference(
    scope: tuple[str, ...],
    reference: str,
    qualified_names: frozenset[str],
    context: str,
    allow_unique_leaf: bool = True,
    minimum_scope_depth: int = 0,
) -> Optional[str]:
    """C++の親scope探索順で型参照を一つの完全修飾名へ解決する。"""

    absolute = reference.startswith("::")
    names = tuple(part for part in reference.split("::") if part)
    if not names:
        return None
    if absolute:
        candidate = "::".join(names)
        return candidate if candidate in qualified_names else None
    for depth in range(len(scope), minimum_scope_depth - 1, -1):
        candidate = "::".join((*scope[:depth], *names))
        if candidate in qualified_names:
            return candidate
    if not allow_unique_leaf:
        return None
    leaf_matches = sorted(
        name for name in qualified_names if name.rsplit("::", 1)[-1] == names[-1]
    )
    if len(leaf_matches) == 1:
        return leaf_matches[0]
    if len(leaf_matches) > 1:
        raise ValueError(
            f"型参照が複数候補へ解決されます: {context} -> {reference}: {leaf_matches}"
        )
    return None


def _resolve_base_reference(
    definition: FTypeDefinition,
    reference: str,
    qualified_names: frozenset[str],
) -> Optional[str]:
    """definitionのscopeから基底型参照を完全修飾名へ解決する。"""

    unqualified_reference = reference[2:] if reference.startswith("::") else reference
    first_name = unqualified_reference.split("::", 1)[0]
    visible_matches = tuple(
        qualified_name
        for visible_reference, qualified_name in definition.visible_managed_references
        if visible_reference == reference
    )
    declaration_scopes = tuple(
        owner_scope
        for declared_name, owner_scope in definition.declared_managed_reference_scopes
        if declared_name == first_name
    )
    has_visible_declaration = not reference.startswith("::") and bool(
        declaration_scopes
    )
    if visible_matches:
        if len(visible_matches) == 1:
            return visible_matches[0]
        raise ValueError(
            "using で可視な管理基底が一意に解決されません: "
            f"{definition.qualified_name} -> {reference}: {visible_matches}"
        )
    lexical = _resolve_qualified_reference(
        definition.scope,
        reference,
        qualified_names,
        definition.qualified_name,
        False,
        len(declaration_scopes[0]) if has_visible_declaration else 0,
    )
    if lexical is not None:
        return lexical
    if has_visible_declaration:
        return None
    external_leaf_names = frozenset(
        name.rsplit("::", 1)[-1] for name in EXTERNAL_MANAGED_BASES
    )
    if reference.rsplit("::", 1)[-1] in external_leaf_names:
        return None
    fallback = _resolve_qualified_reference(
        definition.scope,
        reference,
        qualified_names,
        definition.qualified_name,
    )
    return None if fallback in EXTERNAL_MANAGED_BASES else fallback


def _asset_family_names(
    definitions: Sequence[FTypeDefinition],
    aliases: Sequence[FTypeAlias],
) -> frozenset[str]:
    """FAssetからaliasを含めて直接・間接に派生する型を閉包化する。"""

    alias_names = frozenset(
        _qualified_alias_name(alias) for alias in aliases if not alias.is_template
    )
    qualified_names = frozenset(
        {definition.qualified_name for definition in definitions}
        | set(alias_names)
        | {"acs::FAsset"}
    )
    alias_targets: dict[str, Optional[str]] = {}
    for alias in aliases:
        if alias.is_template:
            continue
        target = next(
            (
                candidate
                for candidate in _alias_target_candidates(alias)
                if candidate in qualified_names
            ),
            None,
        )
        alias_targets[_qualified_alias_name(alias)] = target
    resolved_bases: dict[str, tuple[str, ...]] = {}
    for definition in definitions:
        bases: list[str] = []
        for reference in definition.base_references:
            resolved = _resolve_base_reference(definition, reference, qualified_names)
            if resolved is None:
                if reference.rsplit("::", 1)[-1].endswith("Asset"):
                    raise ValueError(
                        f"Asset派生候補の基底型を解決できません: {definition.qualified_name} -> {reference}"
                    )
                continue
            if (
                resolved in alias_names
                and alias_targets.get(resolved) is None
                and _is_public_role_definition(definition)
                and definition.name.startswith("F")
            ):
                raise ValueError(
                    "公開F型のalias基底を解決できません: "
                    f"{definition.qualified_name} -> {reference} -> {resolved}"
                )
            bases.append(resolved)
        resolved_bases[definition.qualified_name] = tuple(bases)

    family = {"acs::FAsset"}
    changed = True
    while changed:
        changed = False
        for alias_name, target in alias_targets.items():
            if alias_name in family or target not in family:
                continue
            family.add(alias_name)
            changed = True
        for qualified_name, bases in resolved_bases.items():
            if qualified_name in family or not any(base in family for base in bases):
                continue
            family.add(qualified_name)
            changed = True
    return frozenset(family)


def _migration_debt_assessment(
    definition: FTypeDefinition,
    managed_names: frozenset[str],
    asset_family: frozenset[str],
) -> Optional[FTypeRoleDebt]:
    """公開F型がcandidate/manual reviewを要する場合に再構成したdebtを返す。"""

    if (
        not _is_public_role_definition(definition)
        or definition.is_template
        or definition.qualified_name in managed_names
        or not definition.name.startswith("F")
        or definition.source_relative_path is None
    ):
        return None
    features = _features(definition)
    stem = _role_stem(definition.name)
    behavior_named = _ends_with_word(stem, BEHAVIOR_WORDS)
    lifecycle = bool(set(features.methods) & LIFECYCLE_METHODS)
    behavior_method = bool(set(features.methods) & OBVIOUS_BEHAVIOR_METHODS)
    coordination = features.has_coordination
    value_named = _ends_with_word(stem, VALUE_WORDS)
    established_service_evidence = behavior_named or lifecycle or coordination
    service_evidence = established_service_evidence or behavior_method

    status = "manual"
    candidate_prefix: Optional[str] = None
    reason_codes: list[str] = []
    if definition.qualified_name in asset_family:
        reason_codes.append("owned_polymorphic_family")
    elif definition.keyword == "struct":
        if not service_evidence:
            return None
        reason_codes.append("struct_behavior_conflict")
    elif stem.startswith("Scoped"):
        return None
    elif features.has_pure_virtual:
        if not behavior_named and not lifecycle and not coordination and not value_named:
            reason_codes.append("class_role_unresolved")
        reason_codes.append("pure_virtual_role_conflict")
        if value_named and service_evidence:
            reason_codes.append("value_behavior_conflict")
    elif value_named:
        if not service_evidence:
            return None
        reason_codes.append("value_behavior_conflict")
    elif service_evidence:
        status = "candidate"
        candidate_prefix = "C"
        if behavior_named:
            reason_codes.append("behavior_suffix")
        if lifecycle:
            reason_codes.append("lifecycle_method")
        if behavior_method and not established_service_evidence:
            reason_codes.append("behavior_method")
        if coordination:
            reason_codes.append("coordination_state")
    else:
        reason_codes.append("class_role_unresolved")

    return FTypeRoleDebt(
        definition.source_relative_path,
        definition.qualified_name,
        "F",
        status,
        candidate_prefix,
        None,
        True,
        "+".join(reason_codes),
        DEBT_WAVE,
    )


def _collect_migration_debt(
    definitions: Sequence[FTypeDefinition],
    managed_names: frozenset[str],
    aliases: Sequence[FTypeAlias],
) -> tuple[FTypeRoleDebt, ...]:
    """public collectorからcandidate/manual categoryをraw診断に依存せず再構成する。"""

    asset_family = _asset_family_names(definitions, aliases)
    entries: list[FTypeRoleDebt] = []
    for definition in definitions:
        if definition.qualified_name in CANONICAL_OBJECT_AND_CLASS_TYPES:
            continue
        entry = _migration_debt_assessment(definition, managed_names, asset_family)
        if entry is not None:
            entries.append(entry)
    return tuple(sorted(entries, key=_debt_sort_key))


def _reconcile_migration_debt(
    root: Path,
    source_root: Optional[Path],
    definitions: Sequence[FTypeDefinition],
    managed_names: frozenset[str],
    aliases: Sequence[FTypeAlias],
    type_violations: Sequence[FViolation],
    entries: Sequence[FTypeRoleDebt],
) -> tuple[tuple[FViolation, ...], tuple[FTypeRoleDebt, ...], tuple[FTypeRoleDebt, ...]]:
    """public collectorをexact debtと照合し、未登録・stale・driftをfail-closedにする。"""

    selected_entries = _debt_entries_for_scan(root, source_root, entries)
    collected_entries = _debt_entries_for_scan(
        root,
        source_root,
        _collect_migration_debt(definitions, managed_names, aliases),
    )
    definitions_by_key: dict[tuple[str, str], list[FTypeDefinition]] = {}
    for definition in definitions:
        if definition.source_relative_path is None:
            continue
        definitions_by_key.setdefault(
            (definition.source_relative_path, definition.qualified_name), []
        ).append(definition)
    matched_keys: set[tuple[str, str]] = set()
    matched_entries: list[FTypeRoleDebt] = []
    debt_violations: list[FViolation] = []
    manifest_by_key = {
        (entry.path, entry.qualified_type): entry for entry in selected_entries
    }
    collected_by_key: dict[tuple[str, str], list[FTypeRoleDebt]] = {}
    for entry in collected_entries:
        collected_by_key.setdefault((entry.path, entry.qualified_type), []).append(entry)
    for key in sorted(set(manifest_by_key) | set(collected_by_key)):
        manifest_entry = manifest_by_key.get(key)
        collected_matches = collected_by_key.get(key, [])
        collected_entry = collected_matches[0] if collected_matches else None
        definitions_at_key = definitions_by_key.get(key, [])
        definition = definitions_at_key[0] if definitions_at_key else None
        diagnostic_entry = manifest_entry or collected_entry
        if diagnostic_entry is None:
            continue
        if len(collected_matches) > 1 or len(definitions_at_key) > 1:
            debt_violations.append(
                _debt_diagnostic(
                    root,
                    diagnostic_entry,
                    definition,
                    "public collectorでmigration debt型が重複しています。",
                    (
                        f"collector検出数: {len(collected_matches)}",
                        f"型定義数: {len(definitions_at_key)}",
                    ),
                )
            )
            continue
        if manifest_entry is None and collected_entry is not None:
            debt_violations.append(
                _debt_diagnostic(
                    root,
                    collected_entry,
                    definition,
                    "public collectorが未登録のcandidate/manual migration debtを検出しました。",
                    (
                        f"status: {collected_entry.status}",
                        f"reason: {collected_entry.reason}",
                    ),
                )
            )
            continue
        if collected_entry is None and manifest_entry is not None:
            debt_violations.append(
                _debt_diagnostic(
                    root,
                    manifest_entry,
                    definition,
                    "migration debtがpublic collectorから消失、移動、または分類変更されています。",
                    (f"台帳status: {manifest_entry.status}", f"台帳reason: {manifest_entry.reason}"),
                )
            )
            continue
        if manifest_entry != collected_entry:
            debt_violations.append(
                _debt_diagnostic(
                    root,
                    manifest_entry,
                    definition,
                    "migration debtのstatus、候補、理由、またはwaveがpublic collectorと一致しません。",
                    (
                        f"台帳: {manifest_entry}",
                        f"collector: {collected_entry}",
                    ),
                )
            )
            continue
        matched_keys.add(key)
        matched_entries.append(manifest_entry)

    remaining_violations: list[FViolation] = []
    for violation in type_violations:
        if violation.qualified_type is None:
            remaining_violations.append(violation)
            continue
        definition = next(
            (
                candidate
                for candidate in definitions
                if candidate.path == violation.path
                and candidate.line == violation.line
                and candidate.column == violation.column
            ),
            None,
        )
        key = (
            definition.source_relative_path if definition is not None else "",
            violation.qualified_type,
        )
        if key not in matched_keys:
            remaining_violations.append(violation)
    return (
        tuple((*remaining_violations, *debt_violations)),
        selected_entries,
        tuple(sorted(matched_entries, key=_debt_sort_key)),
    )


def _display_path(path: Path, root: Path) -> str:
    """root相対の表示用パスを返す。"""

    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def scan_tree(
    root: Path,
    migration_debt_path: Optional[Path] = None,
) -> FScanResult:
    """指定ツリーを走査して型の意味と表記を照合する。"""

    resolved_root = root.resolve()
    if resolved_root.is_file():
        files = (resolved_root,) if resolved_root.suffix.lower() in CPP_SUFFIXES else ()
    else:
        files = tuple(
            sorted(
                (
                    path
                    for path in resolved_root.rglob("*")
                    if path.is_file()
                    and path.suffix.lower() in CPP_SUFFIXES
                    and not any(part in EXCLUDED_DIRECTORY_NAMES for part in path.parts)
                ),
                key=lambda path: path.as_posix().casefold(),
            )
        )
    definitions: list[FTypeDefinition] = []
    aliases: list[FTypeAlias] = []
    tokenized_files: list[
        tuple[Path, tuple[FToken, ...], frozenset[int]]
    ] = []
    registered_references: set[tuple[tuple[str, ...], str]] = set()
    source_root = _source_tree_root(resolved_root)
    migration_debt = (
        _load_migration_debt(migration_debt_path)
        if migration_debt_path is not None
        else ()
    )
    for path in files:
        source = path.read_text(encoding="utf-8-sig")
        tokens = _tokens(source)
        legacy_use_tokens = lex_cpp(source)
        file_aliases = _namespace_aliases(path, tokens, source_root)
        macro_definition_lines = _macro_definition_lines(source)
        tokenized_files.append(
            (path, tuple(legacy_use_tokens), macro_definition_lines)
        )
        definitions.extend(_type_definitions(path, tokens, source_root))
        aliases.extend(file_aliases)
        registered_references.update(
            _registered_object_references(tokens, macro_definition_lines)
        )
    managed_names = _managed_names(definitions, frozenset(registered_references))
    type_violations, counts = _audit_definitions(definitions, managed_names)
    type_violations, selected_debt, matched_debt = _reconcile_migration_debt(
        resolved_root,
        source_root,
        definitions,
        managed_names,
        aliases,
        type_violations,
        migration_debt,
    )
    alias_use_violations = tuple(
        violation
        for path, tokens, macro_definition_lines in tokenized_files
        for violation in _audit_legacy_alias_uses(
            path,
            tokens,
            aliases,
            macro_definition_lines,
        )
    )
    violations = tuple(
        sorted(
            (
                *type_violations,
                *_audit_aliases(aliases, _is_full_source_scan(resolved_root)),
                *_audit_required_alias_presence(resolved_root, aliases),
                *_audit_required_canonical_definitions(resolved_root, definitions),
                *alias_use_violations,
            ),
            key=lambda item: (
                item.path.as_posix().casefold(),
                item.line,
                item.column,
                item.rule,
                item.type_name,
            ),
        )
    )
    return FScanResult(
        resolved_root,
        files,
        tuple(definitions),
        tuple(aliases),
        violations,
        counts,
        selected_debt,
        matched_debt,
    )


def build_json_report(result: FScanResult) -> dict[str, object]:
    """監査結果を機械可読な辞書へ変換する。"""

    by_rule: dict[str, int] = {}
    for violation in result.violations:
        by_rule[violation.rule] = by_rule.get(violation.rule, 0) + 1
    debt_by_status: dict[str, int] = {}
    debt_by_wave: dict[str, int] = {}
    for entry in result.migration_debt:
        debt_by_status[entry.status] = debt_by_status.get(entry.status, 0) + 1
        debt_by_wave[entry.wave] = debt_by_wave.get(entry.wave, 0) + 1
    return {
        "schema_version": SCHEMA_VERSION,
        "root": result.root.as_posix(),
        "scanned_file_count": len(result.files),
        "type_definition_count": len(result.definitions),
        "namespace_alias_count": len(result.aliases),
        "expected_prefix_counts": result.expected_prefix_counts,
        "migration_debt_count": len(result.migration_debt),
        "matched_migration_debt_count": len(result.matched_migration_debt),
        "migration_debt_by_status": dict(sorted(debt_by_status.items())),
        "migration_debt_by_wave": dict(sorted(debt_by_wave.items())),
        "scope_note": "violation_count=0はhard canonicalとdebt非増加を示し、全型のrole review完了を意味しません。",
        "violation_count": len(result.violations),
        "violations_by_rule": dict(sorted(by_rule.items())),
        "violations": [
            {
                "rule": item.rule,
                "path": _display_path(item.path, result.root),
                "line": item.line,
                "column": item.column,
                "type_name": item.type_name,
                "expected_prefix": item.expected_prefix,
                "message": item.message,
                "evidence": list(item.evidence),
                "qualified_type": item.qualified_type,
                "role_reason": item.role_reason,
            }
            for item in result.violations
        ],
    }


def format_human_report(result: FScanResult, max_diagnostics: int) -> str:
    """人が確認しやすい診断と集計を返す。"""

    lines: list[str] = []
    for item in result.violations[:max_diagnostics]:
        evidence = "、".join(item.evidence)
        lines.append(
            f"{_display_path(item.path, result.root)}:{item.line}:{item.column}: "
            f"error: [{item.rule}] {item.type_name}: {item.message} 根拠: {evidence}"
        )
    omitted = len(result.violations) - min(len(result.violations), max_diagnostics)
    if omitted:
        lines.append(f"... 残り {omitted} 件はJSON出力で確認できます")
    lines.append(
        "ACS C++ 型役割監査: "
        f"files={len(result.files)} types={len(result.definitions)} aliases={len(result.aliases)} "
        f"debt={len(result.migration_debt)} matched_debt={len(result.matched_migration_debt)} "
        f"violations={len(result.violations)}"
    )
    lines.append("注: violations=0はhard canonicalとdebt非増加を示し、全型のrole review完了を意味しません。")
    return "\n".join(lines) + "\n"


def write_json(path: Path, report: dict[str, object]) -> None:
    """既存監査器の安全な保存経路でUTF-8 JSONを保存する。"""

    error = try_write_json_report(path, serialize_json_report(report))
    if error is not None:
        raise OSError(str(error)) from error


def run_self_test() -> bool:
    """A/C/F分類、hard canonical、debt、字句除外、JSONを一時fixtureで確認する。"""

    valid_source = r'''
// class FCommentRegistry { public: void Register(); };
const char* Text = "class AStringUtility {};";
const char* Shader = R"code(struct CShaderManager { private: int Value; };)code";
class AObject;
class AActor : public acs::AObject { public: virtual ~AActor() = default; private: int State; };
class AActorChild : public AActor { public: ~AActorChild() override = default; };
class ARegistered { public: virtual ~ARegistered() = default; };
ACS_OBJECT(ARegistered)
class IReader { public: virtual ~IReader() = default; virtual void Read() = 0; };
class IMarker { public: virtual ~IMarker() = default; };
struct ICallback { virtual ~ICallback() = default; virtual void Invoke() = 0; };
class IBackend {
public:
    struct FRequest { int Id; };
    virtual ~IBackend() = default;
    virtual void Submit(const FRequest& Request) = 0;
};
class CAllocator { public: void* Allocate(int Size); };
class CLuaVm { public: virtual void Shutdown() = 0; };
class CInlineService {
public:
    virtual void Run() = 0;
    int Count() const { return m_Count; }
private:
    int m_Count;
};
class CMessageBroker { public: void Clear(); private: int ChannelCount; };
class CTimerManager { public: void Tick(); private: int TimerCount; };
class CRegistry { public: void Register(); private: int Count; };
struct FConfig { int Width; int Height; };
class FString { public: int Size() const; private: char* Data; };
struct FValueRecord { int Payload; };
class FAssetFuture { private: TSharedPtr<FState> State; };
struct FScopedSession { void Release(); ~FScopedSession(); void* Handle; };
struct FHidden { private: int Value; };
template<typename T> class TBox { private: T Value; };
enum class EState : unsigned { Ready, Stopped };
namespace acs {
using FEventTypeId = u32;
using FComponentTypeId = u32;
using FComponentSignatureId = u64;
using RawCallback = void(*)(void*, u32) noexcept;
using EventCallback = RawCallback;
using CCallback = void(*)();
using CCallbackChain = CCallback;
template<typename T> using AliasList = T;
template<typename T> using CBox = T;
namespace detail { using HiddenTypeId = u32; }
struct FOwner { using EventId = u32; };
void LocalAliasFixture() { using LocalTypeId = u32; }
extern "C" { using FLinkageTypeId = u32; }
}
namespace { using AnonymousTypeId = u32; }
'''
    invalid_source = r'''
class AObject;
class AUtilityManager { public: void Tick(); private: int State; };
class CMessageBroker { public: void Clear(); private: int Count; };
struct CTimerManager { private: int State; public: void Tick(); };
class COptions { private: int Value; };
class MessageBroker { public: void Clear(); };
class URegistry { public: void Register(); };
class Readable { public: virtual void Read() = 0; };
class Entity : public acs::AObject {};
template<typename T> class Box { T Value; };
enum class State { Ready };
class CLuaVm { public: virtual void Shutdown() = 0; };
class FAbstract { public: virtual void Read() = 0; };
struct IRecord { int Value; };
class FRegistered {};
ACS_OBJECT(FRegistered)
template<typename T> class FBox { T Value; };
enum class FState { Ready };
class TConcrete { public: void Run(); };
class IConcrete { private: int Value; };
struct AEntity : public acs::AObject {};
namespace acs {
using EventTypeId = u32;
using ComponentTypeId = u32;
using ComponentSignatureId = u64;
using CMessageBroker = FTimerManager;
using CUnexpectedService = FMessageBroker;
EventTypeId LegacyEventType = 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="acs-type-role-") as directory:
        root = Path(directory)
        registry_root = root / "registry"
        registry_root.mkdir()

        # registry単体のschemaと独立baselineを製品sourceとは別に固定する。
        default_registry_document = json.loads(DEFAULT_TYPE_ROLE_MIGRATIONS.read_text(encoding="utf-8"))

        def write_registry_fixture(name: str, document: object) -> Path:
            """指定したregistry文書をcanonical JSON fixtureとして保存する。"""

            fixture_path = registry_root / name
            fixture_path.write_bytes(
                (json.dumps(document, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
            )
            return fixture_path

        copied_registry_path = write_registry_fixture("copied.json", default_registry_document)
        if (
            _load_registered_type_role_migrations(copied_registry_path, False)
            != REGISTERED_TYPE_ROLE_MIGRATIONS
        ):
            print("type role migration registry copy self-test failed", file=sys.stderr)
            return False

        reviewed_registry_document = json.loads(json.dumps(default_registry_document))
        reviewed_registry_document["entries"].append(
            {
                "path": "z/ReviewedValue.h",
                "legacy": None,
                "legacy_path": None,
                "canonical": "acs::FReviewedValue",
                "kind": "struct",
                "prefix": "F",
            }
        )
        reviewed_entries = _load_registered_type_role_migrations(
            write_registry_fixture("reviewed-value.json", reviewed_registry_document),
            False,
        )
        if reviewed_entries[-1] != (
            None,
            "acs::FReviewedValue",
            "z/ReviewedValue.h",
            None,
            "struct",
            "F",
        ):
            print("type role reviewed keep decision self-test failed", file=sys.stderr)
            return False

        split_registry_document = json.loads(json.dumps(default_registry_document))
        split_registry_document["entries"].append(
            {
                "path": "z/SplitCanonical.h",
                "legacy": "acs::FLegacySplit",
                "legacy_path": "z/SplitForward.h",
                "canonical": "acs::CSplitCanonical",
                "kind": "class",
                "prefix": "C",
            }
        )
        split_entries = _load_registered_type_role_migrations(
            write_registry_fixture("split-paths.json", split_registry_document),
            False,
        )
        split_entry = (
            "acs::FLegacySplit",
            "acs::CSplitCanonical",
            "z/SplitCanonical.h",
            "z/SplitForward.h",
            "class",
            "C",
        )
        if split_entries[-1] != split_entry:
            print("type role split path registry self-test failed", file=sys.stderr)
            return False

        self_alias_registry_document = json.loads(json.dumps(default_registry_document))
        self_alias_registry_document["entries"][0]["legacy"] = (
            self_alias_registry_document["entries"][0]["canonical"]
        )
        try:
            _load_registered_type_role_migrations(
                write_registry_fixture("self-alias.json", self_alias_registry_document),
                False,
            )
        except ValueError:
            pass
        else:
            print("type role migration self-alias self-test failed", file=sys.stderr)
            return False

        invalid_registry_paths = (
            "",
            ".",
            "./audio/AudioEngine.h",
            "audio\\AudioEngine.h",
            "C:/audio/AudioEngine.h",
            "audio/../AudioEngine.h",
            "audio/AudioEngine.cpp",
        )
        for case_index, invalid_path in enumerate(invalid_registry_paths):
            invalid_document = json.loads(json.dumps(default_registry_document))
            invalid_document["entries"][0]["path"] = invalid_path
            invalid_registry_path = write_registry_fixture(
                f"invalid-path-{case_index}.json",
                invalid_document,
            )
            try:
                _load_registered_type_role_migrations(invalid_registry_path, False)
            except ValueError:
                pass
            else:
                print(
                    f"type role migration invalid path self-test failed: {invalid_path!r}",
                    file=sys.stderr,
                )
                return False

        # 旧名aliasの公開先にも定義pathと同じ正規化契約を適用する。
        for case_index, invalid_path in enumerate(invalid_registry_paths):
            invalid_document = json.loads(json.dumps(default_registry_document))
            invalid_document["entries"][0]["legacy_path"] = invalid_path
            invalid_registry_path = write_registry_fixture(
                f"invalid-legacy-path-{case_index}.json",
                invalid_document,
            )
            try:
                _load_registered_type_role_migrations(invalid_registry_path, False)
            except ValueError:
                pass
            else:
                print(
                    "type role migration invalid legacy_path self-test failed: "
                    f"{invalid_path!r}",
                    file=sys.stderr,
                )
                return False

        missing_legacy_path_document = json.loads(json.dumps(default_registry_document))
        missing_legacy_path_document["entries"][0].pop("legacy_path")
        try:
            _load_registered_type_role_migrations(
                write_registry_fixture(
                    "missing-legacy-path.json",
                    missing_legacy_path_document,
                ),
                False,
            )
        except ValueError:
            pass
        else:
            print("type role migration missing legacy_path self-test failed", file=sys.stderr)
            return False

        legacy_without_path_document = json.loads(json.dumps(default_registry_document))
        legacy_without_path_document["entries"][0]["legacy_path"] = None
        try:
            _load_registered_type_role_migrations(
                write_registry_fixture(
                    "legacy-without-path.json",
                    legacy_without_path_document,
                ),
                False,
            )
        except ValueError:
            pass
        else:
            print("type role migration legacy without path self-test failed", file=sys.stderr)
            return False

        path_without_legacy_document = json.loads(json.dumps(default_registry_document))
        path_without_legacy_document["entries"][0]["legacy"] = None
        try:
            _load_registered_type_role_migrations(
                write_registry_fixture(
                    "path-without-legacy.json",
                    path_without_legacy_document,
                ),
                False,
            )
        except ValueError:
            pass
        else:
            print("type role migration path without legacy self-test failed", file=sys.stderr)
            return False

        bool_schema_document = json.loads(json.dumps(default_registry_document))
        bool_schema_document["schema_version"] = True
        try:
            _load_registered_type_role_migrations(
                write_registry_fixture("bool-schema.json", bool_schema_document),
                False,
            )
        except ValueError:
            pass
        else:
            print("type role migration bool schema self-test failed", file=sys.stderr)
            return False

        duplicate_key_path = registry_root / "duplicate-key.json"
        duplicate_key_path.write_bytes(
            b'{"schema_version":2,"schema_version":2,"entries":[]}\n'
        )
        try:
            _load_registered_type_role_migrations(duplicate_key_path, False)
        except ValueError:
            pass
        else:
            print("type role migration duplicate key self-test failed", file=sys.stderr)
            return False

        deleted_registry_document = json.loads(json.dumps(default_registry_document))
        deleted_registry_document["entries"].pop()
        try:
            _load_registered_type_role_migrations(
                write_registry_fixture("coordinated-delete.json", deleted_registry_document),
                True,
            )
        except ValueError:
            pass
        else:
            print("type role migration fixed baseline self-test failed", file=sys.stderr)
            return False

        (root / "valid.h").write_text(valid_source, encoding="utf-8")
        (root / "invalid.h").write_text(invalid_source, encoding="utf-8")

        valid_result = scan_tree(root / "valid.h")
        definition_names = {definition.name for definition in valid_result.definitions}
        if (
            valid_result.violations
            or len(valid_result.definitions) != 23
            or valid_result.expected_prefix_counts
            != {"A": 3, "C": 6, "E": 1, "F": 8, "I": 4, "T": 1}
            or not {
                "EState",
                "CAllocator",
                "CInlineService",
                "CLuaVm",
                "CMessageBroker",
                "CTimerManager",
            }.issubset(definition_names)
            or len(valid_result.aliases) != 10
        ):
            print(
                "type role self-test failed: "
                f"valid fixture={valid_result.violations} "
                f"types={len(valid_result.definitions)} "
                f"aliases={len(valid_result.aliases)} "
                f"counts={valid_result.expected_prefix_counts}",
                file=sys.stderr,
            )
            return False

        invalid_result = scan_tree(root / "invalid.h")
        actual = [
            (item.rule, item.type_name, item.expected_prefix)
            for item in invalid_result.violations
        ]
        expected = [
            ("ACS-R020c", "AUtilityManager", "C"),
            ("ACS-R020c", "COptions", "F"),
            ("ACS-R020c", "MessageBroker", "C"),
            ("ACS-R020c", "URegistry", "C"),
            ("ACS-R020c", "Readable", "I"),
            ("ACS-R020c", "Entity", "A"),
            ("ACS-R020c", "Box", "T"),
            ("ACS-R020c", "State", "E"),
            ("ACS-R020c", "FAbstract", "I"),
            ("ACS-R020c", "IRecord", "F"),
            ("ACS-R020c", "FRegistered", "A"),
            ("ACS-R020c", "FBox", "T"),
            ("ACS-R020c", "FState", "E"),
            ("ACS-R020c", "TConcrete", "C"),
            ("ACS-R020c", "IConcrete", "F"),
            ("ACS-R020d", "EventTypeId", "F"),
            ("ACS-R020d", "ComponentTypeId", "F"),
            ("ACS-R020d", "ComponentSignatureId", "F"),
        ]
        if actual != expected:
            print(
                f"type role self-test failed: expected={expected} actual={actual}",
                file=sys.stderr,
            )
            return False
        report = build_json_report(invalid_result)
        if (
            report["schema_version"] != SCHEMA_VERSION
            or report["violation_count"] != len(expected)
            or report["namespace_alias_count"] != 5
            or report["violations_by_rule"] != {"ACS-R020c": 15, "ACS-R020d": 3}
        ):
            print(f"type role JSON self-test failed: {report}", file=sys.stderr)
            return False
        report_path = root / "type-role-report.json"
        write_json(report_path, report)
        if json.loads(report_path.read_text(encoding="utf-8")) != report:
            print("type role JSON write self-test failed", file=sys.stderr)
            return False

        macro_definition_path = root / "macro-definition.h"
        macro_definition_path.write_text(
            "#define ACS_OBJECT(Type) static_assert(true)\nclass Type { public: void Run(); };",
            encoding="utf-8",
        )
        macro_definition_violations = scan_tree(macro_definition_path).violations
        if [
            (item.rule, item.type_name, item.expected_prefix)
            for item in macro_definition_violations
        ] != [("ACS-R020c", "Type", "C")]:
            print(
                f"type role macro definition registration self-test failed: {macro_definition_violations}",
                file=sys.stderr,
            )
            return False

        conditional_registration_path = root / "conditional-registration.h"
        conditional_registration_path.write_text(
            "namespace acs { class AObject {}; class AConditional { public: void Run(); };\n"
            "#if 0\nACS_OBJECT(AConditional)\n#endif\n}",
            encoding="utf-8",
        )
        conditional_registration_violations = scan_tree(
            conditional_registration_path
        ).violations
        if [
            (item.rule, item.qualified_type, item.expected_prefix)
            for item in conditional_registration_violations
        ] != [("ACS-R020c", "acs::AConditional", "C")]:
            print(
                "type role disabled registration self-test failed: "
                f"{conditional_registration_violations}",
                file=sys.stderr,
            )
            return False

        qualified_a_path = root / "qualified-a-graph.h"
        qualified_a_path.write_text(
            "namespace acs { class AObject {}; class AReal : public AObject {}; "
            "namespace fake { class AObject {}; class AThing : public AObject {}; } "
            "namespace one { class ARegistered {}; ACS_OBJECT(ARegistered) } "
            "namespace two { class ARegistered {}; } }",
            encoding="utf-8",
        )
        qualified_a_violations = scan_tree(qualified_a_path).violations
        if [
            (item.rule, item.qualified_type, item.expected_prefix)
            for item in qualified_a_violations
        ] != [
            ("ACS-R020c", "acs::fake::AObject", "C"),
            ("ACS-R020c", "acs::fake::AThing", "C"),
            ("ACS-R020c", "acs::two::ARegistered", "C"),
        ]:
            print(
                f"type role qualified A graph self-test failed: {qualified_a_violations}",
                file=sys.stderr,
            )
            return False

        external_managed_path = root / "external-managed-bases.cpp"
        external_managed_path.write_text(
            "namespace vendor { struct FNode {}; class ANode { public: void Run(); }; "
            "namespace game { struct FNode {}; } "
            "namespace acs::game { using ANode = ::vendor::FNode; } } "
            "namespace acs::fake { using ANode = ::vendor::FNode; } "
            "namespace fixture { "
            "class ASceneProbe : public acs::game::AScene {}; "
            "class ASceneDerivedProbe : public acs::game::AScene {}; "
            "class ANodeProbe : public acs::game::ANode {}; "
            "class AComponentProbe : public acs::game::AComponent {}; "
            "class AFalseProbe : public acs::fake::ANode { public: void Run(); }; "
            "} "
            "namespace using_namespace { using namespace acs::game; "
            "class AVisibleNodeProbe : public ANode {}; "
            "namespace child { class AVisibleSceneProbe : public AScene {}; } } "
            "namespace using_declaration { using acs::game::AComponent; "
            "class AVisibleComponentProbe : public AComponent {}; } "
            "namespace using_parent_namespace { using namespace acs; "
            "class ARelativeQualifiedProbe : public game::ANode {}; } "
            "namespace reopened { using acs::game::ANode; } "
            "namespace reopened { class AReopenedProbe : public ANode {}; } "
            "namespace foreign { class AForeignProbe : public ANode { public: void Run(); }; } "
            "class AGlobalProbe : public ANode { public: void Run(); }; "
            "namespace after { class AAfterProbe : public ANode { public: void Run(); }; "
            "using acs::game::ANode; } "
            "namespace sibling_source { using acs::game::ANode; } "
            "namespace sibling_target { class ASiblingProbe : public ANode { public: void Run(); }; } "
            "namespace inner_scope { namespace child { using acs::game::ANode; "
            "class AInnerProbe : public ANode {}; } "
            "class AOuterProbe : public ANode { public: void Run(); }; } "
            "namespace { using acs::game::ANode; } "
            "namespace anonymous_target { "
            "class AAnonymousLeakProbe : public ANode { public: void Run(); }; } "
            "namespace alias_reopened { namespace ag = acs::game; } "
            "namespace alias_reopened { namespace child { "
            "class AAliasReopenedProbe : public ag::ANode {}; } } "
            "namespace alias_after { "
            "class AAliasAfterProbe : public ag::ANode { public: void Run(); }; "
            "namespace ag = acs::game; } "
            "namespace alias_sibling_source { namespace ag = acs::game; } "
            "namespace alias_sibling_target { "
            "class AAliasSiblingProbe : public ag::ANode { public: void Run(); }; } "
            "namespace alias_inner_scope { namespace child { namespace ag = acs::game; } "
            "class AAliasOuterProbe : public ag::ANode { public: void Run(); }; } "
            "namespace { namespace hidden_ag = acs::game; } "
            "namespace alias_anonymous_target { "
            "class AAliasAnonymousProbe : public hidden_ag::ANode { public: void Run(); }; } "
            "namespace unknown_ag = vendor::game; "
            "class AUnknownAliasProbe : public unknown_ag::ANode { public: void Run(); }; "
            "namespace alias_chain { namespace root = acs; "
            "namespace game_alias = root::game; "
            "class AAliasChainProbe : public game_alias::ANode { public: void Run(); }; } "
            "namespace alias_cycle { namespace left = right; namespace right = left; "
            "class AAliasCycleProbe : public left::ANode { public: void Run(); }; } "
            "namespace alias_shadow { namespace ag = acs::game; "
            "namespace inherited { class AInheritedAliasProbe : public ag::ANode {}; } "
            "namespace child { namespace ag = vendor; "
            "class AShadowAlias : public ag::ANode { public: void Run(); }; } } "
            "namespace using_shadow { using acs::game::ANode; "
            "namespace inherited { class AInheritedUsingProbe : public ANode {}; } "
            "namespace child { using vendor::ANode; "
            "class AShadowUsing : public ANode { public: void Run(); }; } } "
            "namespace outer { namespace ag = acs::game; using acs::game::ANode; "
            "namespace inherited { class AInheritedAlias : public ag::ANode {}; "
            "class AInheritedUsing : public ANode {}; } "
            "namespace child_alias { namespace ag = vendor; "
            "class AShadowAlias : public ag::ANode { public: void Run(); }; } "
            "namespace child_using { using vendor::ANode; "
            "class AShadowUsing : public ANode { public: void Run(); }; } } "
            "namespace local_type_shadow { using ::acs::game::ANode; "
            "namespace child { class ANode { public: void Run(); }; "
            "class ALocalTypeShadow : public ANode { public: void Run(); }; } } "
            "namespace local_struct_shadow { using ::acs::game::ANode; "
            "namespace child { struct ANode { void Run(); }; "
            "class ALocalStructShadow : public ANode { public: void Run(); }; } } "
            "namespace local_union_shadow { using ::acs::game::ANode; "
            "namespace child { union ANode { int Value; void Run(); }; "
            "class ALocalUnionShadow : public ANode { public: void Run(); }; } } "
            "namespace local_enum_shadow { using ::acs::game::ANode; "
            "namespace child { enum class ANode { Value }; "
            "class ALocalEnumShadow : public ANode { public: void Run(); }; } } "
            "namespace local_after_shadow { using ::acs::game::ANode; "
            "namespace child { class ABeforeShadow : public ANode {}; "
            "struct ANode { int Value; }; } } "
            "namespace local_sibling_shadow { using ::acs::game::ANode; "
            "namespace source { struct ANode { int Value; }; } "
            "namespace target { class ASiblingShadow : public ANode {}; } } "
            "namespace qualified_class_shadow { namespace ag = acs::game; "
            "namespace child { class ag { public: using ANode = ::vendor::FNode; }; "
            "class AQualifiedClassShadow : public ag::ANode { public: void Run(); }; } } "
            "namespace qualified_struct_shadow { namespace ag = acs::game; "
            "namespace child { struct ag { using ANode = ::vendor::FNode; }; "
            "class AQualifiedStructShadow : public ag::ANode { public: void Run(); }; } } "
            "namespace qualified_union_shadow { namespace ag = acs::game; "
            "namespace child { union ag { using ANode = ::vendor::FNode; }; "
            "class AQualifiedUnionShadow : public ag::ANode { public: void Run(); }; } } "
            "namespace qualified_after_shadow { namespace ag = acs::game; "
            "namespace child { class ABeforeShadow : public ag::ANode {}; "
            "struct ag { using ANode = ::vendor::FNode; }; } } "
            "namespace qualified_sibling_shadow { namespace ag = acs::game; "
            "namespace source { struct ag { using ANode = ::vendor::FNode; }; } "
            "namespace target { class ASiblingShadow : public ag::ANode {}; } } "
            "namespace directive_class_shadow { using namespace ::acs::game; "
            "namespace child { class ANode { public: void Run(); }; "
            "class ADirectiveClassShadow : public ANode { public: void Run(); }; } } "
            "namespace directive_after_shadow { using namespace ::acs::game; "
            "namespace child { class ABeforeShadow : public ANode {}; "
            "struct ANode { int Value; }; } } "
            "namespace parent_alias_class_shadow { using ANode = ::acs::game::ANode; "
            "namespace child { class ANode { public: void Run(); }; "
            "class AParentAliasClassShadow : public ANode { public: void Run(); }; } } "
            "namespace parent_typedef_class_shadow { typedef ::acs::game::ANode ANode; "
            "namespace child { class ANode { public: void Run(); }; "
            "class AParentTypedefClassShadow : public ANode { public: void Run(); }; } } "
            "namespace type_alias_positive { using Base = acs::game::ANode; "
            "class ATypeAliasProbe : public Base {}; } "
            "namespace typedef_positive { typedef acs::game::ANode Base; "
            "class ATypedefProbe : public Base {}; } "
            "namespace type_alias_shadow { using acs::game::ANode; "
            "namespace child { using ANode = vendor::ANode; "
            "class AShadowTypeAlias : public ANode { public: void Run(); }; } } "
            "namespace typedef_shadow { using acs::game::ANode; "
            "namespace child { typedef vendor::ANode ANode; "
            "class AShadowTypedef : public ANode { public: void Run(); }; } } "
            "namespace attributed_alias_positive { "
            "using Base [[deprecated]] = ::acs::game::ANode; "
            "class AAttributedAlias : public Base {}; } "
            "namespace attributed_typedef_positive { "
            "typedef ::acs::game::ANode Base [[deprecated]]; "
            "class AAttributedTypedef : public Base {}; } "
            "namespace attributed_alias_shadow { using acs::game::ANode; "
            "namespace child { using ANode [[deprecated]] = vendor::ANode; "
            "class AAttributedAliasShadow : public ANode { public: void Run(); }; } } "
            "namespace attributed_typedef_shadow { using acs::game::ANode; "
            "namespace child { typedef vendor::ANode ANode [[deprecated]]; "
            "class AAttributedTypedefShadow : public ANode { public: void Run(); }; } } "
            "namespace type_alias_after { "
            "class ATypeAliasAfter : public Base { public: void Run(); }; "
            "using Base = acs::game::ANode; } "
            "namespace type_alias_sibling_source { using Base = acs::game::ANode; } "
            "namespace type_alias_sibling_target { "
            "class ATypeAliasSibling : public Base { public: void Run(); }; } "
            "namespace typedef_after { "
            "class ATypedefAfter : public Base { public: void Run(); }; "
            "typedef acs::game::ANode Base; } "
            "namespace typedef_sibling_source { typedef acs::game::ANode Base; } "
            "namespace typedef_sibling_target { "
            "class ATypedefSibling : public Base { public: void Run(); }; } "
            "namespace chain_using_namespace { namespace root = acs; "
            "using namespace root::game; class AChainNamespace : public ANode {}; } "
            "namespace chain_using_declaration { namespace root = acs; "
            "using root::game::ANode; class AChainDeclaration : public ANode {}; } "
            "namespace inline_scope { inline namespace v1 { namespace ag = acs::game; "
            "using acs::game::AComponent; } namespace child { "
            "class AInlineAlias : public ag::ANode {}; "
            "class AInlineUsing : public AComponent {}; } } "
            "namespace inline_parent { namespace ag = acs::game; "
            "using acs::game::AComponent; inline namespace v1 { "
            "class AInlineParentAlias : public ag::ANode {}; "
            "class AInlineParentUsing : public AComponent {}; } } "
            "namespace { namespace repeated_ag = acs::game; } "
            "namespace { using acs::game::AComponent; } "
            "namespace anonymous_reopen_target { "
            "class AAnonymousAlias : public repeated_ag::ANode {}; "
            "class AAnonymousUsing : public AComponent {}; } "
            "namespace vendor::relative_lookup { "
            "using acs::game::ANode; class ARelativeUsing : public ANode { public: void Run(); }; "
            "namespace ag = acs::game; class ARelativeAlias : public ag::ANode { public: void Run(); }; "
            "using RelativeBase = acs::game::ANode; "
            "class ARelativeTypeAlias : public RelativeBase { public: void Run(); }; "
            "typedef acs::game::ANode RelativeTypedef; "
            "class ARelativeTypedef : public RelativeTypedef { public: void Run(); }; "
            "class ARelativeDirect : public acs::game::ANode { public: void Run(); }; } "
            "namespace vendor::absolute_lookup { "
            "using ::acs::game::ANode; class AAbsoluteUsing : public ANode {}; "
            "namespace absolute_ag = ::acs::game; class AAbsoluteAlias : public absolute_ag::ANode {}; "
            "using AbsoluteBase = ::acs::game::ANode; class AAbsoluteTypeAlias : public AbsoluteBase {}; "
            "typedef ::acs::game::ANode AbsoluteTypedef; class AAbsoluteTypedef : public AbsoluteTypedef {}; "
            "class AAbsoluteDirect : public ::acs::game::ANode {}; } "
            "using namespace acs::game; "
            "namespace global_using { class AGlobalUsingProbe : public ANode {}; } "
            "namespace direct_ag = acs::game; "
            "namespace direct_alias_positive { "
            "class ADirectAliasProbe : public direct_ag::ANode {}; } "
            "namespace acs_alias = acs; "
            "namespace parent_alias_positive { "
            "class AParentAliasProbe : public acs_alias::game::ANode {}; }",
            encoding="utf-8",
        )
        external_managed_violations = scan_tree(external_managed_path).violations
        if [
            (item.rule, item.qualified_type, item.expected_prefix)
            for item in external_managed_violations
        ] != [
            ("ACS-R020c", "vendor::ANode", "C"),
            ("ACS-R020c", "fixture::AFalseProbe", "C"),
            ("ACS-R020c", "foreign::AForeignProbe", "C"),
            ("ACS-R020c", "AGlobalProbe", "C"),
            ("ACS-R020c", "after::AAfterProbe", "C"),
            ("ACS-R020c", "sibling_target::ASiblingProbe", "C"),
            ("ACS-R020c", "inner_scope::AOuterProbe", "C"),
            ("ACS-R020c", "alias_after::AAliasAfterProbe", "C"),
            ("ACS-R020c", "alias_sibling_target::AAliasSiblingProbe", "C"),
            ("ACS-R020c", "alias_inner_scope::AAliasOuterProbe", "C"),
            ("ACS-R020c", "AUnknownAliasProbe", "C"),
            ("ACS-R020c", "alias_cycle::AAliasCycleProbe", "C"),
            ("ACS-R020c", "alias_shadow::child::AShadowAlias", "C"),
            ("ACS-R020c", "using_shadow::child::AShadowUsing", "C"),
            ("ACS-R020c", "outer::child_alias::AShadowAlias", "C"),
            ("ACS-R020c", "outer::child_using::AShadowUsing", "C"),
            ("ACS-R020c", "local_type_shadow::child::ANode", "C"),
            ("ACS-R020c", "local_type_shadow::child::ALocalTypeShadow", "C"),
            ("ACS-R020c", "local_struct_shadow::child::ANode", "C"),
            ("ACS-R020c", "local_struct_shadow::child::ALocalStructShadow", "C"),
            ("ACS-R020c", "local_union_shadow::child::ANode", "C"),
            ("ACS-R020c", "local_union_shadow::child::ALocalUnionShadow", "C"),
            ("ACS-R020c", "local_enum_shadow::child::ANode", "E"),
            ("ACS-R020c", "local_enum_shadow::child::ALocalEnumShadow", "C"),
            ("ACS-R020c", "local_after_shadow::child::ANode", "F"),
            ("ACS-R020c", "local_sibling_shadow::source::ANode", "F"),
            ("ACS-R020c", "qualified_class_shadow::child::ag", "C"),
            ("ACS-R020c", "qualified_class_shadow::child::AQualifiedClassShadow", "C"),
            ("ACS-R020c", "qualified_struct_shadow::child::ag", "F"),
            ("ACS-R020c", "qualified_struct_shadow::child::AQualifiedStructShadow", "C"),
            ("ACS-R020c", "qualified_union_shadow::child::ag", "F"),
            ("ACS-R020c", "qualified_union_shadow::child::AQualifiedUnionShadow", "C"),
            ("ACS-R020c", "qualified_after_shadow::child::ag", "F"),
            ("ACS-R020c", "qualified_sibling_shadow::source::ag", "F"),
            ("ACS-R020c", "directive_class_shadow::child::ANode", "C"),
            ("ACS-R020c", "directive_class_shadow::child::ADirectiveClassShadow", "C"),
            ("ACS-R020c", "directive_after_shadow::child::ANode", "F"),
            ("ACS-R020c", "parent_alias_class_shadow::child::ANode", "C"),
            ("ACS-R020c", "parent_alias_class_shadow::child::AParentAliasClassShadow", "C"),
            ("ACS-R020c", "parent_typedef_class_shadow::child::ANode", "C"),
            ("ACS-R020c", "parent_typedef_class_shadow::child::AParentTypedefClassShadow", "C"),
            ("ACS-R020c", "type_alias_shadow::child::AShadowTypeAlias", "C"),
            ("ACS-R020c", "typedef_shadow::child::AShadowTypedef", "C"),
            ("ACS-R020c", "attributed_alias_shadow::child::AAttributedAliasShadow", "C"),
            ("ACS-R020c", "attributed_typedef_shadow::child::AAttributedTypedefShadow", "C"),
            ("ACS-R020c", "type_alias_after::ATypeAliasAfter", "C"),
            ("ACS-R020c", "type_alias_sibling_target::ATypeAliasSibling", "C"),
            ("ACS-R020c", "typedef_after::ATypedefAfter", "C"),
            ("ACS-R020c", "typedef_sibling_target::ATypedefSibling", "C"),
            ("ACS-R020c", "vendor::relative_lookup::ARelativeUsing", "C"),
            ("ACS-R020c", "vendor::relative_lookup::ARelativeAlias", "C"),
            ("ACS-R020c", "vendor::relative_lookup::ARelativeTypeAlias", "C"),
            ("ACS-R020c", "vendor::relative_lookup::ARelativeTypedef", "C"),
            ("ACS-R020c", "vendor::relative_lookup::ARelativeDirect", "C"),
        ]:
            print(
                "type role external managed base self-test failed: "
                f"{external_managed_violations}",
                file=sys.stderr,
            )
            return False

        legacy_managed_path = root / "legacy-managed-bases.cpp"
        legacy_managed_path.write_text(
            "namespace vendor { class FScene { public: int Value; }; }\n"
            "namespace legacy_managed {\n"
            "class ALegacyObjectProbe : public acs::FObject {};\n"
            "class FLegacyObjectProbe : public acs::FObject {};\n"
            "class FLegacySceneProbe : public acs::game::FScene {};\n"
            "class AUnregisteredLegacyProbe : public vendor::FScene { public: void Run(); };\n"
            "}",
            encoding="utf-8",
        )
        legacy_managed_violations = scan_tree(legacy_managed_path).violations
        if [
            (item.rule, item.qualified_type, item.expected_prefix)
            for item in legacy_managed_violations
        ] != [
            ("ACS-R020c", "legacy_managed::FLegacyObjectProbe", "A"),
            ("ACS-R020c", "legacy_managed::FLegacySceneProbe", "A"),
            ("ACS-R020c", "legacy_managed::AUnregisteredLegacyProbe", "C"),
        ]:
            print(
                "type role registered legacy managed base self-test failed: "
                f"{legacy_managed_violations}",
                file=sys.stderr,
            )
            return False

        role_boundary_path = root / "role-boundary.h"
        role_boundary_path.write_text(
            "namespace acs { class CWidget { public: int Value; }; "
            "struct FWidget { void Draw(); }; "
            "struct FHandle { int Id; bool IsValid() const; }; }",
            encoding="utf-8",
        )
        role_boundary_violations = scan_tree(role_boundary_path).violations
        if [
            (item.rule, item.qualified_type, item.expected_prefix)
            for item in role_boundary_violations
        ] != [
            ("ACS-R020c", "acs::CWidget", "F"),
            ("ACS-R020c", "acs::FWidget", "C"),
        ]:
            print(
                f"type role C/F boundary self-test failed: {role_boundary_violations}",
                file=sys.stderr,
            )
            return False

        behavior_name_path = root / "behavior-role-names.h"
        behavior_name_path.write_text(
            "namespace fixture { "
            "struct CExampleApp { static int Run(); }; "
            "class CPlayerVm { public: int Value; }; "
            "class CPlayerViewModel { public: int Value; }; "
            "}",
            encoding="utf-8",
        )
        behavior_name_violations = scan_tree(behavior_name_path).violations
        if behavior_name_violations:
            print(
                "type role behavior name self-test failed: "
                f"{behavior_name_violations}",
                file=sys.stderr,
            )
            return False

        audio_semantic_path = root / "audio-engine-semantic.h"
        audio_semantic_path.write_text(
            "namespace acs { class CAudioEngine { public: bool Init(); void Shutdown(); "
            "void Play(); void Stop(); private: int ActiveVoiceCount; }; }",
            encoding="utf-8",
        )
        audio_semantic_result = scan_tree(audio_semantic_path)
        if len(audio_semantic_result.definitions) != 1:
            print(
                f"type role CAudioEngine semantic definition self-test failed: {audio_semantic_result}",
                file=sys.stderr,
            )
            return False
        audio_definition = audio_semantic_result.definitions[0]
        audio_features = _features(audio_definition)
        audio_role = _expected_prefix(audio_definition, audio_features, frozenset())
        if (
            audio_semantic_result.violations
            or audio_semantic_result.expected_prefix_counts != {"C": 1}
            or audio_role[0] != "C"
            or audio_role[2] != "functional-evidence"
            or not {"Init", "Play", "Shutdown", "Stop"}.issubset(audio_features.methods)
            or "状態や寿命を動かす操作: Shutdown, Stop" not in audio_role[1]
        ):
            print(
                "type role CAudioEngine semantic classification self-test failed: "
                f"result={audio_semantic_result} role={audio_role}",
                file=sys.stderr,
            )
            return False

        default_debt_entries = _load_migration_debt(DEFAULT_MIGRATION_DEBT)
        collision_common = (
            "fixture/Collision.h",
            "acs::FCollision",
            "F",
            "manual",
            None,
            None,
            True,
        )
        collision_left = FTypeRoleDebt(
            *collision_common,
            "reason\x1fsegment",
            "wave",
        )
        collision_right = FTypeRoleDebt(
            *collision_common,
            "reason",
            "segment\x1fwave",
        )
        if _debt_semantic_sha256((collision_left,)) == _debt_semantic_sha256((collision_right,)):
            print("type role canonical JSON semantic hash collision self-test failed", file=sys.stderr)
            return False
        coordinated_addition = tuple(
            sorted(
                (
                    *default_debt_entries,
                    FTypeRoleDebt(
                        "z/NewDebt.h",
                        "acs::FNewDebt",
                        "F",
                        "manual",
                        None,
                        None,
                        True,
                        "class_role_unresolved",
                        DEBT_WAVE,
                    ),
                ),
                key=_debt_sort_key,
            )
        )
        default_source_root = Path(__file__).resolve().parent.parent / "src"
        default_source_result = scan_tree(default_source_root, DEFAULT_MIGRATION_DEBT)
        if default_source_result.violations:
            print(
                f"type role default source fixture self-test failed: {default_source_result.violations}",
                file=sys.stderr,
            )
            return False
        default_definitions = default_source_result.definitions
        addition_source_root = root / "coordinated-addition" / "src"
        addition_path = addition_source_root / "z" / "NewDebt.h"
        addition_path.parent.mkdir(parents=True)
        addition_source = "namespace acs { class FNewDebt {}; }"
        addition_path.write_text(addition_source, encoding="utf-8")
        addition_definitions = _type_definitions(
            addition_path,
            _tokens(addition_source),
            addition_source_root,
        )
        if len(addition_definitions) != 1:
            print("type role coordinated addition fixture selection failed", file=sys.stderr)
            return False
        addition_mutation_definitions = (*default_definitions, *addition_definitions)
        addition_managed_names = _managed_names(
            addition_mutation_definitions,
            frozenset(),
        )
        addition_raw_violations, _ = _audit_definitions(
            addition_mutation_definitions,
            addition_managed_names,
        )
        reconciled, selected, matched = _reconcile_migration_debt(
            default_source_root,
            default_source_root,
            addition_mutation_definitions,
            addition_managed_names,
            default_source_result.aliases,
            addition_raw_violations,
            coordinated_addition,
        )
        if (
            reconciled
            or selected != coordinated_addition
            or matched != coordinated_addition
        ):
            print(
                "type role coordinated source/manifest addition self-test failed: "
                f"violations={reconciled}",
                file=sys.stderr,
            )
            return False
        try:
            _verify_default_debt_baseline(coordinated_addition)
        except ValueError:
            pass
        else:
            print("type role default debt coordinated addition self-test failed", file=sys.stderr)
            return False

        # deletionとmoveは直後の2件だけのsynthetic debt fixtureで独立に固定する。

        debt_root = root / "debt-contract"
        debt_source_root = debt_root / "src"
        debt_feature_root = debt_source_root / "feature"
        debt_feature_root.mkdir(parents=True)
        debt_source_path = debt_feature_root / "Types.h"
        debt_source_path.write_text(
            "namespace acs { class FUnresolved { public: void Observe(); }; class FWorker { public: void Run(); }; }",
            encoding="utf-8",
        )
        debt_entries = [
            {
                "path": "feature/Types.h",
                "qualified_type": "acs::FUnresolved",
                "current_prefix": "F",
                "status": "manual",
                "candidate_prefix": None,
                "expected": None,
                "review_required": True,
                "reason": "class_role_unresolved",
                "wave": DEBT_WAVE,
            },
            {
                "path": "feature/Types.h",
                "qualified_type": "acs::FWorker",
                "current_prefix": "F",
                "status": "candidate",
                "candidate_prefix": "C",
                "expected": None,
                "review_required": True,
                "reason": "lifecycle_method",
                "wave": DEBT_WAVE,
            },
        ]

        def write_debt(name: str, entries: list[dict[str, object]]) -> Path:
            """debt台帳の変異fixtureをUTF-8 JSONで保存する。"""

            path = debt_root / f"{name}.json"
            path.write_bytes(
                (
                    json.dumps(
                        {"schema_version": DEBT_SCHEMA_VERSION, "entries": entries},
                        ensure_ascii=False,
                        indent=2,
                    )
                    + "\n"
                ).encode("utf-8")
            )
            return path

        def debt_rejection_matches(path: Path, expected_message: str) -> bool:
            """debt台帳が期待した契約段階で拒否されたかを返す。"""

            try:
                _load_migration_debt(path)
            except ValueError as error:
                return expected_message in str(error)
            return False

        valid_debt_path = write_debt("valid-debt", debt_entries)
        valid_debt_result = scan_tree(debt_feature_root, valid_debt_path)
        if (
            valid_debt_result.violations
            or len(valid_debt_result.migration_debt) != 2
            or len(valid_debt_result.matched_migration_debt) != 2
        ):
            print(f"type role debt match self-test failed: {valid_debt_result}", file=sys.stderr)
            return False

        valid_debt_bytes = valid_debt_path.read_bytes()
        physical_debt_mutations = (
            ("bom", b"\xef\xbb\xbf" + valid_debt_bytes, "UTF-8にBOM"),
            ("crlf", valid_debt_bytes.replace(b"\n", b"\r\n"), "改行はLFだけ"),
            ("cr", valid_debt_bytes.replace(b"\n", b"\r"), "改行はLFだけ"),
            ("missing-final-lf", valid_debt_bytes[:-1], "一つの最終LFで終わる"),
            ("excess-final-lf", valid_debt_bytes + b"\n", "最終LFは一つだけ"),
            ("spaced-excess-final-lf", valid_debt_bytes + b" \n", "最終LFは一つだけ"),
            ("invalid-utf8", valid_debt_bytes[:-1] + b"\xff\n", "厳密なUTF-8"),
        )
        for mutation_name, mutation_payload, expected_message in physical_debt_mutations:
            mutation_path = debt_root / f"physical-{mutation_name}.json"
            mutation_path.write_bytes(mutation_payload)
            if not debt_rejection_matches(mutation_path, expected_message):
                print(
                    f"type role physical debt contract self-test failed: {mutation_name}",
                    file=sys.stderr,
                )
                return False
        if not debt_rejection_matches(debt_root, "通常file"):
            print("type role non-regular debt path self-test failed", file=sys.stderr)
            return False

        schema_marker = b'"schema_version": 1'
        schema_debt_mutations = (
            ("bool", valid_debt_bytes.replace(schema_marker, b'"schema_version": true', 1), "schema_versionは1"),
            ("float", valid_debt_bytes.replace(schema_marker, b'"schema_version": 1.0', 1), "schema_versionは1"),
            ("nonfinite", valid_debt_bytes.replace(schema_marker, b'"schema_version": NaN', 1), "非有限値"),
        )
        for mutation_name, mutation_payload, expected_message in schema_debt_mutations:
            if mutation_payload == valid_debt_bytes:
                print(
                    f"type role schema mutation fixture replacement failed: {mutation_name}",
                    file=sys.stderr,
                )
                return False
            mutation_path = debt_root / f"schema-{mutation_name}.json"
            mutation_path.write_bytes(mutation_payload)
            if not debt_rejection_matches(mutation_path, expected_message):
                print(
                    f"type role schema version type self-test failed: {mutation_name}",
                    file=sys.stderr,
                )
                return False

        external_temp_path: Optional[Path] = None
        with tempfile.TemporaryDirectory(prefix="acs-type-role-debt-external-") as external_directory:
            external_root = Path(external_directory)
            external_temp_path = external_root
            external_debt_path = external_root / "external-debt.json"
            external_sentinel_path = external_root / "external-sentinel.bin"
            external_debt_path.write_bytes(valid_debt_bytes)
            external_sentinel = b"ACS_TYPE_ROLE_EXTERNAL_SENTINEL\x00\xff"
            external_sentinel_path.write_bytes(external_sentinel)
            link_artifacts = (
                debt_root / "external-file-link.json",
                debt_root / "external-directory-link",
                debt_root / "external-junction",
            )

            symlink_cases = (
                (link_artifacts[0], external_debt_path, False),
                (link_artifacts[1], external_root, True),
            )
            reparse_case_count = 0
            for link_path, target_path, is_directory in symlink_cases:
                try:
                    link_path.symlink_to(target_path, target_is_directory=is_directory)
                except (NotImplementedError, OSError):
                    continue
                reparse_case_count += 1
                linked_debt_path = link_path / external_debt_path.name if is_directory else link_path
                try:
                    rejected = debt_rejection_matches(linked_debt_path, "reparse point")
                finally:
                    if is_directory and sys.platform == "win32":
                        os.rmdir(link_path)
                    else:
                        link_path.unlink()
                if not rejected:
                    print(
                        f"type role symlink debt path self-test failed: {link_path.name}",
                        file=sys.stderr,
                    )
                    return False

            if sys.platform == "win32":
                junction_path = link_artifacts[2]
                junction_result = subprocess.run(
                    ["cmd.exe", "/d", "/c", "mklink", "/J", str(junction_path), str(external_root)],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    check=False,
                )
                if junction_result.returncode == 0:
                    reparse_case_count += 1
                    try:
                        rejected = debt_rejection_matches(
                            junction_path / external_debt_path.name,
                            "reparse point",
                        )
                    finally:
                        os.rmdir(junction_path)
                    if not rejected:
                        print("type role junction debt path self-test failed", file=sys.stderr)
                        return False

            if reparse_case_count == 0:
                print("type role reparse debt path coverage self-test failed", file=sys.stderr)
                return False
            if (
                external_debt_path.read_bytes() != valid_debt_bytes
                or external_sentinel_path.read_bytes() != external_sentinel
                or any(path.exists() or path.is_symlink() for path in link_artifacts)
            ):
                print("type role external debt sentinel/cleanup self-test failed", file=sys.stderr)
                return False
        if external_temp_path is None or external_temp_path.exists():
            print("type role external debt temporary cleanup self-test failed", file=sys.stderr)
            return False

        deletion_result = scan_tree(debt_feature_root, write_debt("deleted-debt", debt_entries[:1]))
        if [(item.rule, item.qualified_type) for item in deletion_result.violations] != [
            ("ACS-R020c", "acs::FWorker"),
            ("ACS-R020f", "acs::FWorker"),
        ]:
            print(f"type role debt deletion self-test failed: {deletion_result.violations}", file=sys.stderr)
            return False
        added_entries = [
            *debt_entries,
            {
                "path": "feature/Types.h",
                "qualified_type": "acs::FZombie",
                "current_prefix": "F",
                "status": "manual",
                "candidate_prefix": None,
                "expected": None,
                "review_required": True,
                "reason": "class_role_unresolved",
                "wave": DEBT_WAVE,
            },
        ]
        added_entries.sort(key=lambda entry: (str(entry["path"]).casefold(), str(entry["qualified_type"])))
        addition_result = scan_tree(debt_feature_root, write_debt("added-debt", added_entries))
        if [(item.rule, item.qualified_type) for item in addition_result.violations] != [
            ("ACS-R020f", "acs::FZombie")
        ]:
            print(f"type role debt addition self-test failed: {addition_result.violations}", file=sys.stderr)
            return False
        moved_entries = [dict(entry) for entry in debt_entries]
        moved_entries[1]["path"] = "feature/Moved.h"
        moved_entries.sort(key=lambda entry: (str(entry["path"]).casefold(), str(entry["qualified_type"])))
        moved_result = scan_tree(debt_feature_root, write_debt("moved-debt", moved_entries))
        if {item.rule for item in moved_result.violations} != {"ACS-R020c", "ACS-R020f"}:
            print(f"type role debt move self-test failed: {moved_result.violations}", file=sys.stderr)
            return False
        drift_entries = [dict(entry) for entry in debt_entries]
        drift_entries[1]["candidate_prefix"] = "F"
        drift_result = scan_tree(debt_feature_root, write_debt("drift-debt", drift_entries))
        if [(item.rule, item.qualified_type) for item in drift_result.violations] != [
            ("ACS-R020c", "acs::FWorker"),
            ("ACS-R020f", "acs::FWorker"),
        ]:
            print(f"type role debt candidate drift self-test failed: {drift_result.violations}", file=sys.stderr)
            return False
        malformed_debts = (
            ("duplicate", [*debt_entries, dict(debt_entries[-1])]),
            ("unsorted", list(reversed(debt_entries))),
            (
                "reason-control",
                [
                    {
                        **debt_entries[0],
                        "reason": "class_role_unresolved\x1fshifted",
                    }
                ],
            ),
            (
                "wave-control",
                [
                    {
                        **debt_entries[0],
                        "wave": f"{DEBT_WAVE}\nshifted",
                    }
                ],
            ),
            (
                "traversal",
                [
                    {
                        **debt_entries[0],
                        "path": "../Types.h",
                    }
                ],
            ),
        )
        for mutation_name, malformed_entries in malformed_debts:
            malformed_path = write_debt(f"malformed-{mutation_name}", malformed_entries)
            try:
                scan_tree(debt_feature_root, malformed_path)
            except ValueError:
                continue
            print(f"type role malformed debt self-test failed: {mutation_name}", file=sys.stderr)
            return False

        valid_debt_text = valid_debt_path.read_text(encoding="utf-8")
        duplicate_key_sources = (
            (
                "top",
                valid_debt_text.replace(
                    '"schema_version": 1,',
                    '"schema_version": 1,\n  "schema_version": 1,',
                    1,
                ),
            ),
            (
                "entry",
                valid_debt_text.replace(
                    '"path": "feature/Types.h",',
                    '"path": "feature/Types.h",\n      "path": "feature/Types.h",',
                    1,
                ),
            ),
            (
                "reason",
                valid_debt_text.replace(
                    '"reason": "class_role_unresolved",',
                    '"reason": "class_role_unresolved",\n      "reason": "changed",',
                    1,
                ),
            ),
        )
        for mutation_name, source in duplicate_key_sources:
            duplicate_key_path = debt_root / f"duplicate-key-{mutation_name}.json"
            duplicate_key_path.write_bytes(source.encode("utf-8"))
            if not debt_rejection_matches(duplicate_key_path, "重複key"):
                print(f"type role duplicate JSON key self-test failed: {mutation_name}", file=sys.stderr)
                return False

        new_manual_path = debt_feature_root / "NewManual.h"
        new_manual_path.write_text(
            "namespace acs { class FNewAsset : public FAsset {}; class FQualifiedAsset : public acs::FAsset {}; class FNewAudioAsset : public FAsset {}; class FSpecialAudioAsset : public FNewAudioAsset {}; struct FScopedBlob : public FAsset {}; struct FConcreteAsset : public FAsset { int Id; }; using FAssetRoot = FAsset; using FAssetRoot2 = FAssetRoot; struct FAliasAsset : public FAssetRoot2 { int Id; }; class FNewNode { public: virtual void Visit() = 0; }; }",
            encoding="utf-8",
        )
        new_manual_result = scan_tree(debt_feature_root, valid_debt_path)
        if [
            (item.rule, item.qualified_type, item.evidence[-1])
            for item in new_manual_result.violations
        ] != [
            ("ACS-R020f", "acs::FNewAsset", "reason: owned_polymorphic_family"),
            ("ACS-R020f", "acs::FQualifiedAsset", "reason: owned_polymorphic_family"),
            ("ACS-R020f", "acs::FNewAudioAsset", "reason: owned_polymorphic_family"),
            ("ACS-R020f", "acs::FSpecialAudioAsset", "reason: owned_polymorphic_family"),
            ("ACS-R020f", "acs::FScopedBlob", "reason: owned_polymorphic_family"),
            ("ACS-R020f", "acs::FConcreteAsset", "reason: owned_polymorphic_family"),
            ("ACS-R020f", "acs::FAliasAsset", "reason: owned_polymorphic_family"),
            ("ACS-R020f", "acs::FNewNode", "reason: pure_virtual_role_conflict"),
        ]:
            print(
                f"type role new manual category self-test failed: {new_manual_result.violations}",
                file=sys.stderr,
            )
            return False

        asset_resolution_cases = (
            (
                "ambiguous",
                "namespace one { class FExternalAsset {}; } namespace two { class FExternalAsset {}; } namespace acs { class FBrokenAsset : public FExternalAsset {}; }",
            ),
            (
                "unresolved",
                "namespace acs { class FBrokenAsset : public FMissingAsset {}; }",
            ),
            (
                "unresolved-alias",
                "namespace acs { using FRoot = FMissingBase; struct FData : public FRoot { int Id; }; }",
            ),
        )
        for case_name, source in asset_resolution_cases:
            case_root = debt_source_root / case_name
            case_root.mkdir()
            (case_root / "Types.h").write_text(source, encoding="utf-8")
            try:
                scan_tree(case_root, valid_debt_path)
            except ValueError:
                continue
            print(f"type role Asset base resolution self-test failed: {case_name}", file=sys.stderr)
            return False

        deferred_root = root / "deferred" / "src" / "asset"
        deferred_root.mkdir(parents=True)
        (deferred_root / "Asset.h").write_text("namespace acs { using AssetType = u32; }", encoding="utf-8")
        if scan_tree(deferred_root / "Asset.h").violations:
            print("type role deferred alias self-test failed", file=sys.stderr)
            return False
        deferred_mutations = (
            (root / "deferred-path" / "src" / "event" / "Asset.h", "namespace acs { using AssetType = u32; }"),
            (root / "deferred-name" / "src" / "asset" / "Asset.h", "namespace acs { using AssetKind = u32; }"),
            (root / "deferred-target" / "src" / "asset" / "Asset.h", "namespace acs { using AssetType = u64; }"),
        )
        for mutation_path, mutation_source in deferred_mutations:
            mutation_path.parent.mkdir(parents=True)
            mutation_path.write_text(mutation_source, encoding="utf-8")
            if not scan_tree(mutation_path).violations:
                print(f"type role deferred alias mutation self-test failed: {mutation_path}", file=sys.stderr)
                return False
        foundation_sources = (
            ("valid", "foundation/Types.h", "namespace acs { using u32 = ::uint32_t; }", False),
            ("path", "event/Types.h", "namespace acs { using u32 = ::uint32_t; }", True),
            ("name", "foundation/Types.h", "namespace acs { using u33 = ::uint32_t; }", True),
            ("target", "foundation/Types.h", "namespace acs { using u32 = ::uint64_t; }", True),
            ("attribute", "foundation/Types.h", "namespace acs { using u32 [[deprecated]] = ::uint32_t; }", True),
        )
        for mutation_name, relative_path, source, expects_violation in foundation_sources:
            mutation_path = root / f"foundation-{mutation_name}" / "src" / relative_path
            mutation_path.parent.mkdir(parents=True)
            mutation_path.write_text(source, encoding="utf-8")
            mutation_violations = scan_tree(mutation_path).violations
            if bool(mutation_violations) != expects_violation:
                print(f"type role foundation primitive mutation self-test failed: {mutation_name}: {mutation_violations}", file=sys.stderr)
                return False
        typedef_path = root / "typedef" / "src" / "event" / "EventTypeId.h"
        typedef_path.parent.mkdir(parents=True)
        typedef_path.write_text("namespace acs { typedef u32 EventTypeId; }", encoding="utf-8")
        if not scan_tree(typedef_path).violations:
            print("type role typedef alias mutation self-test failed", file=sys.stderr)
            return False
        alias_mutations = (
            "namespace acs { using EventTypeId [[deprecated]] = u32; }",
            "namespace acs { using EventTypeId = const FEventTypeId; }",
            "namespace acs { using EventTypeId = ::FEventTypeId; }",
            "namespace acs { using EventTypeId = acs::FEventTypeId; }",
            "namespace acs { using event_id = u32; }",
        )
        for mutation_index, mutation_source in enumerate(alias_mutations):
            mutation_path = root / f"alias-mutation-{mutation_index}" / "src" / "event" / "EventTypeId.h"
            mutation_path.parent.mkdir(parents=True)
            mutation_path.write_text(mutation_source, encoding="utf-8")
            if not scan_tree(mutation_path).violations:
                print(f"type role alias mutation self-test failed: {mutation_index}", file=sys.stderr)
                return False
        callback_typedef_path = root / "callback-typedef" / "src" / "event" / "Callback.h"
        callback_typedef_path.parent.mkdir(parents=True)
        callback_typedef_path.write_text("namespace acs { typedef void(*CCallback)(); }", encoding="utf-8")
        if not scan_tree(callback_typedef_path).violations:
            print("type role callback typedef mutation self-test failed", file=sys.stderr)
            return False
        classification_path = root / "classification.h"
        classification_path.write_text(
            "namespace acs { using EventTypeId=u32; using ComponentTypeId=u32; using ComponentSignatureId=u64; template<class TValue> using OptionalAlias=TValue; using CompletionCallback=void(*)(int); }",
            encoding="utf-8",
        )
        classification_violations = scan_tree(classification_path).violations
        expected_classification = {
            ("ACS-R020d", "EventTypeId", "F"),
            ("ACS-R020d", "ComponentTypeId", "F"),
            ("ACS-R020d", "ComponentSignatureId", "F"),
        }
        actual_classification = [
            (item.rule, item.type_name, item.expected_prefix)
            for item in classification_violations
        ]
        excluded_classification_counts = [
            sum(item.type_name == type_name for item in classification_violations)
            for type_name in ("OptionalAlias", "CompletionCallback")
        ]
        if (
            len(actual_classification) != len(expected_classification)
            or set(actual_classification) != expected_classification
            or excluded_classification_counts != [0, 0]
        ):
            print(f"type role first wave classification self-test failed: {classification_violations}", file=sys.stderr)
            return False
        parent_scalar_path = root / "parent-scalar.h"
        parent_scalar_path.write_text("namespace acs { using FBaseScalar=u32; namespace feature { using BadId=FBaseScalar; } }", encoding="utf-8")
        parent_scalar_violations = scan_tree(parent_scalar_path).violations
        if [(item.rule, item.type_name) for item in parent_scalar_violations] != [("ACS-R020d", "BadId")]:
            print(f"type role parent scalar self-test failed: {parent_scalar_violations}", file=sys.stderr)
            return False
        cv_scalar_path = root / "cv-scalar.h"
        cv_scalar_path.write_text("namespace acs { using FBaseScalar=u32; namespace feature { using BadId=const FBaseScalar; } }", encoding="utf-8")
        cv_scalar_violations = scan_tree(cv_scalar_path).violations
        if [(item.rule, item.type_name) for item in cv_scalar_violations] != [("ACS-R020d", "BadId")]:
            print(f"type role cv scalar self-test failed: {cv_scalar_violations}", file=sys.stderr)
            return False
        msvc_scalar_path = root / "msvc-scalar.h"
        msvc_scalar_path.write_text("namespace acs { using NativeWidth=__int64; using NativeMask=unsigned __int64; }", encoding="utf-8")
        msvc_scalar_violations = scan_tree(msvc_scalar_path).violations
        if [(item.rule, item.type_name) for item in msvc_scalar_violations] != [("ACS-R020d", "NativeWidth"), ("ACS-R020d", "NativeMask")]:
            print(f"type role MSVC scalar self-test failed: {msvc_scalar_violations}", file=sys.stderr)
            return False
        parent_callback_path = root / "parent-callback.h"
        parent_callback_path.write_text("namespace acs { using CCallback=void(*)(); namespace feature { using CCallbackChain=CCallback; } }", encoding="utf-8")
        if scan_tree(parent_callback_path).violations:
            print("type role parent callback self-test failed", file=sys.stderr)
            return False
        extern_function_path = root / "extern-function.h"
        extern_function_path.write_text("namespace acs { extern int ExternalFunction() { using LocalTypeId=u32; return 0; } }", encoding="utf-8")
        extern_function_result = scan_tree(extern_function_path)
        if extern_function_result.aliases or extern_function_result.violations:
            print(f"type role extern function exclusion self-test failed: {extern_function_result}", file=sys.stderr)
            return False
        balanced_typedef_path = root / "balanced-typedef.h"
        balanced_typedef_path.write_text("namespace acs { typedef struct { int Field; } LegacyStruct; using BadId=u32; }", encoding="utf-8")
        balanced_typedef_violations = scan_tree(balanced_typedef_path).violations
        if [(item.rule, item.type_name) for item in balanced_typedef_violations] != [("ACS-R020d", "LegacyStruct"), ("ACS-R020d", "BadId")]:
            print(f"type role balanced typedef self-test failed: {balanced_typedef_violations}", file=sys.stderr)
            return False
        attributed_typedef_path = root / "attributed-typedef.h"
        attributed_typedef_path.write_text("namespace acs { typedef u32 BadId [[deprecated]]; }", encoding="utf-8")
        attributed_typedef_violations = scan_tree(attributed_typedef_path).violations
        if [(item.rule, item.type_name) for item in attributed_typedef_violations] != [("ACS-R020d", "BadId")]:
            print(f"type role attributed typedef self-test failed: {attributed_typedef_violations}", file=sys.stderr)
            return False
        multiple_typedef_path = root / "multiple-typedef.h"
        multiple_typedef_path.write_text("namespace acs { typedef u32 BadA, BadB; }", encoding="utf-8")
        multiple_typedef_violations = scan_tree(multiple_typedef_path).violations
        if [(item.rule, item.type_name) for item in multiple_typedef_violations] != [("ACS-R020d", "BadB")]:
            print(f"type role multiple typedef self-test failed: {multiple_typedef_violations}", file=sys.stderr)
            return False
        implementation_path = root / "implementation.cpp"
        implementation_path.write_text("namespace acs { using ImplementationTypeId = u32; }", encoding="utf-8")
        implementation_result = scan_tree(implementation_path)
        if implementation_result.aliases or implementation_result.violations:
            print("type role implementation alias exclusion self-test failed", file=sys.stderr)
            return False
        usage_root = root / "legacy-use"
        usage_header = usage_root / "src" / "event" / "EventTypeId.h"
        usage_header.parent.mkdir(parents=True)
        usage_header.write_text(
            "namespace acs { using FEventTypeId = u32; using EventTypeId = FEventTypeId; EventTypeId Value = 0; }",
            encoding="utf-8",
        )
        usage_violations = scan_tree(usage_header).violations
        if [(item.rule, item.type_name) for item in usage_violations] != [("ACS-R020e", "EventTypeId")]:
            print(f"type role legacy use self-test failed: {usage_violations}", file=sys.stderr)
            return False
        presence_sources = {
            "ecs/ComponentId.h": "namespace acs { using FComponentTypeId=u32; using ComponentTypeId=FComponentTypeId; using FComponentSignatureId=u64; using ComponentSignatureId=FComponentSignatureId; }",
            "event/EventTypeId.h": "namespace acs { using FEventTypeId=u32; using EventTypeId=FEventTypeId; }",
        }
        for legacy, canonical, expected_path, legacy_path, kind, _ in REGISTERED_TYPE_ROLE_MIGRATIONS:
            # 正規型の定義はcanonical pathだけへ置く。
            namespace = canonical.rsplit("::", 1)[0]
            canonical_name = canonical.rsplit("::", 1)[-1]
            declaration = f"namespace {namespace} {{ {kind} {canonical_name} {{}}; }}"
            presence_sources[expected_path] = (
                f"{presence_sources[expected_path]}\n{declaration}"
                if expected_path in presence_sources
                else declaration
            )
            if legacy is not None and legacy_path is not None:
                # 旧名aliasは循環しない公開forward headerへ分離できる。
                legacy_namespace = legacy.rsplit("::", 1)[0]
                legacy_name = legacy.rsplit("::", 1)[-1]
                declaration = (
                    f"namespace {legacy_namespace} {{ "
                    f"using {legacy_name}={canonical_name}; }}"
                )
                presence_sources[legacy_path] = (
                    f"{presence_sources[legacy_path]}\n{declaration}"
                    if legacy_path in presence_sources
                    else declaration
                )

        def write_presence_tree(name: str, sources: dict[str, str]) -> Path:
            """必須aliasの変異を独立したsource treeへ保存する。"""

            tree = root / name / "src"
            for relative_path, source in sources.items():
                fixture_path = tree / relative_path
                fixture_path.parent.mkdir(parents=True, exist_ok=True)
                fixture_path.write_text(source, encoding="utf-8")
            return tree

        presence_root = write_presence_tree("presence", presence_sources)
        if scan_tree(presence_root).violations:
            print("type role compatibility presence self-test failed", file=sys.stderr)
            return False

        # 全335件のpresenceは直前の一括fixtureで固定し、同じfilesystem変異の反復は
        # role・同居/分離path・scalarを横断する代表契約へ限定する。
        legacy_contract_names = frozenset(
            {
                "acs::ComponentSignatureId",
                "acs::ComponentTypeId",
                "acs::EventTypeId",
                "acs::FAudioEngine",
                "acs::FAllocator",
                "acs::scripting::FLuaVm",
                "acs::FMessageBroker",
                "acs::FObject",
                "acs::FTimerManager",
                "acs::game::FGame",
                "acs::game::FScene",
            }
        )
        canonical_contract_names = frozenset(
            {
                "acs::AObject",
                "acs::CAudioEngine",
                "acs::scripting::CLuaVm",
                "acs::CMessageBroker",
                "acs::CTimerManager",
                "acs::IAllocator",
                "acs::FSubsystemOwner",
                "acs::game::AScene",
                "acs::game::CGame",
                "acs::game::FRenderContext",
            }
        )
        missing_legacy_contracts = legacy_contract_names.difference(
            LEGACY_COMPATIBILITY_ALIASES
        )
        missing_canonical_contracts = canonical_contract_names.difference(
            CANONICAL_OBJECT_AND_CLASS_TYPES
        )
        if missing_legacy_contracts or missing_canonical_contracts:
            print(
                "type role bounded mutation contract registration failed: "
                f"legacy={sorted(missing_legacy_contracts)} "
                f"canonical={sorted(missing_canonical_contracts)}",
                file=sys.stderr,
            )
            return False

        # loaderが返した分離path契約を実際のpresence監査へ接続する。
        split_legacy, split_canonical, split_path, split_legacy_path, split_kind, split_prefix = (
            split_entry
        )
        if split_legacy is None or split_legacy_path is None:
            print("type role split path fixture pairing failed", file=sys.stderr)
            return False
        split_sources = dict(presence_sources)
        split_sources[split_path] = (
            f"namespace acs {{ {split_kind} {split_canonical.rsplit('::', 1)[-1]} {{}}; }}"
        )
        split_sources[split_legacy_path] = (
            "namespace acs { class CSplitCanonical; using FLegacySplit=CSplitCanonical; }"
        )
        LEGACY_COMPATIBILITY_ALIASES[split_legacy] = split_canonical
        LEGACY_COMPATIBILITY_PATHS[split_legacy] = split_legacy_path
        CANONICAL_OBJECT_AND_CLASS_TYPES[split_canonical] = (
            split_path,
            split_kind,
            split_prefix,
        )
        MIGRATED_CANONICAL_OBJECT_AND_CLASS_TYPES[split_canonical] = (
            split_path,
            split_kind,
            split_prefix,
        )
        try:
            split_root = write_presence_tree("split-presence", split_sources)
            if scan_tree(split_root).violations:
                print("type role split path presence self-test failed", file=sys.stderr)
                return False
            wrong_split_sources = dict(split_sources)
            wrong_split_sources["z/WrongSplitForward.h"] = wrong_split_sources.pop(
                split_legacy_path
            )
            wrong_split_root = write_presence_tree("wrong-split-presence", wrong_split_sources)
            wrong_split_violations = scan_tree(wrong_split_root).violations
            if [
                (item.rule, item.type_name)
                for item in wrong_split_violations
                if item.type_name == "FLegacySplit"
            ] != [("ACS-R020d", "FLegacySplit")]:
                print(
                    "type role wrong legacy_path presence self-test failed: "
                    f"{wrong_split_violations}",
                    file=sys.stderr,
                )
                return False
        finally:
            LEGACY_COMPATIBILITY_ALIASES.pop(split_legacy, None)
            LEGACY_COMPATIBILITY_PATHS.pop(split_legacy, None)
            CANONICAL_OBJECT_AND_CLASS_TYPES.pop(split_canonical, None)
            MIGRATED_CANONICAL_OBJECT_AND_CLASS_TYPES.pop(split_canonical, None)

        def scan_presence_mutation(sources: dict[str, str]) -> FScanResult:
            """一つの共有treeへ変異を適用し、走査後に必ず基準内容へ戻す。"""

            changed_paths = tuple(
                sorted(
                    {
                        *(
                            path
                            for path, source in sources.items()
                            if presence_sources.get(path) != source
                        ),
                        *(
                            path
                            for path in presence_sources
                            if path not in sources
                        ),
                    }
                )
            )
            try:
                for relative_path in changed_paths:
                    fixture_path = presence_root / relative_path
                    if relative_path in sources:
                        fixture_path.parent.mkdir(parents=True, exist_ok=True)
                        fixture_path.write_text(sources[relative_path], encoding="utf-8")
                    elif fixture_path.exists():
                        fixture_path.unlink()
                return scan_tree(presence_root)
            finally:
                for relative_path in changed_paths:
                    fixture_path = presence_root / relative_path
                    if relative_path in presence_sources:
                        fixture_path.parent.mkdir(parents=True, exist_ok=True)
                        fixture_path.write_text(
                            presence_sources[relative_path],
                            encoding="utf-8",
                        )
                    elif fixture_path.exists():
                        fixture_path.unlink()
                    parent = fixture_path.parent
                    while parent != presence_root and parent.exists():
                        try:
                            parent.rmdir()
                        except OSError:
                            break
                        parent = parent.parent
        reexport_sources = dict(presence_sources)
        reexport_sources["gameframework/Forward.h"] = (
            f"{reexport_sources['gameframework/Forward.h']}\n"
            "namespace acs { using game::FGame; }"
        )
        reexport_sources["gameframework/Subsystem.h"] = (
            "namespace acs { namespace game { using ::acs::FSubsystem; } }"
        )
        reexport_sources["gameframework/SubsystemCollection.h"] = (
            "namespace acs { namespace game { using ::acs::FSubsystemCollection; } }"
        )
        reexport_sources["gameframework/SubsystemRegistry.h"] = (
            "namespace acs { namespace game { "
            "using ::acs::FSubsystemAutoRegister; "
            "using ::acs::FSubsystemRegistry; } }"
        )
        if scan_presence_mutation(reexport_sources).violations:
            print("type role legacy qualified re-export self-test failed", file=sys.stderr)
            return False
        wrong_reexport_sources = dict(reexport_sources)
        wrong_reexport_sources["gameframework/WrongLegacyReexport.h"] = (
            "namespace rogue { using acs::game::FGame; }"
        )
        wrong_reexport_sources["gameframework/WrongSubsystemReexport.h"] = (
            "namespace acs { namespace game { using ::acs::FSubsystem; } }"
        )
        wrong_reexport_violations = scan_presence_mutation(
            wrong_reexport_sources
        ).violations
        if [
            (item.rule, item.type_name) for item in wrong_reexport_violations
        ] != [
            ("ACS-R020e", "FGame"),
            ("ACS-R020e", "FSubsystem"),
        ]:
            print(
                "type role wrong legacy re-export self-test failed: "
                f"{wrong_reexport_violations}",
                file=sys.stderr,
            )
            return False
        identity_sources = dict(presence_sources)
        identity_sources["gameframework/LegacyIdentity.cpp"] = (
            "ACS_REGISTER_SYSTEM(FGame)\n"
            "ACS_REGISTER_SYSTEM(::acs::game::FGame)"
        )
        if scan_presence_mutation(identity_sources).violations:
            print("type role legacy identity macro self-test failed", file=sys.stderr)
            return False
        unrelated_macro_sources = dict(presence_sources)
        unrelated_macro_sources["gameframework/LegacyUseMacro.cpp"] = (
            "ACS_UNRELATED(FGame)"
        )
        unrelated_macro_violations = scan_presence_mutation(
            unrelated_macro_sources
        ).violations
        if [
            (item.rule, item.type_name) for item in unrelated_macro_violations
        ] != [("ACS-R020e", "FGame")]:
            print(
                "type role unrelated legacy macro self-test failed: "
                f"{unrelated_macro_violations}",
                file=sys.stderr,
            )
            return False
        absolute_identity_sources = dict(presence_sources)
        absolute_identity_sources["gameframework/AbsoluteLegacyIdentity.cpp"] = (
            "ACS_REGISTER_SYSTEM(::FGame)\n"
            "ACS_REGISTER_SYSTEM(rogue::FGame)"
        )
        absolute_identity_violations = scan_presence_mutation(
            absolute_identity_sources
        ).violations
        if [
            (item.rule, item.type_name)
            for item in absolute_identity_violations
        ] != [
            ("ACS-R020e", "FGame"),
            ("ACS-R020e", "FGame"),
        ]:
            print(
                "type role absolute legacy identity self-test failed: "
                f"{absolute_identity_violations}",
                file=sys.stderr,
            )
            return False
        nested_identity_sources = dict(presence_sources)
        nested_identity_sources["gameframework/NestedLegacyIdentity.cpp"] = (
            "ACS_REGISTER_SYSTEM(Wrapper<FGame>)"
        )
        nested_identity_violations = scan_presence_mutation(
            nested_identity_sources
        ).violations
        if [
            (item.rule, item.type_name) for item in nested_identity_violations
        ] != [("ACS-R020e", "FGame")]:
            print(
                "type role nested legacy identity self-test failed: "
                f"{nested_identity_violations}",
                file=sys.stderr,
            )
            return False
        wrong_identity_argument_sources = dict(presence_sources)
        wrong_identity_argument_sources["gameframework/WrongLegacyIdentity.cpp"] = (
            "ACS_REGISTER_SYSTEM(CGame, sizeof(FGame))"
        )
        wrong_identity_argument_sources["gameframework/UnbalancedLegacyIdentity.cpp"] = (
            "#if 0\nACS_REGISTER_SYSTEM(FGame\n#endif"
        )
        wrong_identity_argument_violations = scan_presence_mutation(
            wrong_identity_argument_sources
        ).violations
        if [
            (item.rule, item.type_name)
            for item in wrong_identity_argument_violations
        ] != [
            ("ACS-R020e", "FGame"),
            ("ACS-R020e", "FGame"),
        ]:
            print(
                "type role wrong legacy identity argument self-test failed: "
                f"{wrong_identity_argument_violations}",
                file=sys.stderr,
            )
            return False
        fake_directive_sources = dict(presence_sources)
        fake_directive_sources["gameframework/FakeDirective.cpp"] = (
            "/* #if 0 */\n"
            "R\"tag(#endif)tag\";\n"
            "namespace acs::game { FGame* ActiveLegacy = nullptr; }"
        )
        fake_directive_violations = scan_presence_mutation(
            fake_directive_sources
        ).violations
        if [
            (item.rule, item.type_name) for item in fake_directive_violations
        ] != [("ACS-R020e", "FGame")]:
            print(
                "type role fake preprocessor directive self-test failed: "
                f"{fake_directive_violations}",
                file=sys.stderr,
            )
            return False
        conditional_alias_sources = dict(presence_sources)
        conditional_alias_sources["audio/AudioEngine.h"] = presence_sources["audio/AudioEngine.h"].replace(
            "using FAudioEngine=CAudioEngine;",
            "#if SOME_FLAG\n"
            "using FAudioEngine=CAudioEngine;\n"
            "#else\n"
            "using FAudioEngine=CAudioEngine;\n"
            "#endif\n"
        )
        conditional_alias_violations = scan_presence_mutation(
            conditional_alias_sources
        ).violations
        if [(item.rule, item.type_name) for item in conditional_alias_violations] != [
            ("ACS-R020d", "FAudioEngine")
        ]:
            print(
                "type role conditional compatibility raw scan self-test failed: "
                f"{conditional_alias_violations}",
                file=sys.stderr,
            )
            return False
        for case_index, (qualified_name, canonical_name) in enumerate(
            (
                item
                for item in sorted(LEGACY_COMPATIBILITY_ALIASES.items())
                if item[0] in legacy_contract_names
            )
        ):
            alias_name = qualified_name.rsplit("::", 1)[-1]
            target_name = canonical_name.rsplit("::", 1)[-1]
            expected_path = LEGACY_COMPATIBILITY_PATHS[qualified_name]
            absolute_sources = dict(presence_sources)
            original_source = absolute_sources[expected_path]
            absolute_source = re.sub(
                rf"using\s+{re.escape(alias_name)}\s*=\s*{re.escape(target_name)}\s*;",
                f"using {alias_name}=::{canonical_name};",
                original_source,
                count=1,
            )
            if absolute_source == original_source:
                print(f"type role absolute compatibility fixture replacement failed: {qualified_name}", file=sys.stderr)
                return False
            absolute_sources[expected_path] = absolute_source
            if scan_presence_mutation(absolute_sources).violations:
                print(f"type role absolute compatibility target self-test failed: {qualified_name}", file=sys.stderr)
                return False
        missing_audio_sources = dict(presence_sources)
        missing_audio_sources.pop("audio/AudioEngine.h")
        missing_audio_violations = scan_presence_mutation(missing_audio_sources).violations
        if [(item.rule, item.type_name) for item in missing_audio_violations] != [
            ("ACS-R020d", "CAudioEngine"),
            ("ACS-R020d", "FAudioEngine"),
        ]:
            print(f"type role missing module directory self-test failed: {missing_audio_violations}", file=sys.stderr)
            return False
        nested_source_sources = dict(presence_sources)
        nested_source_sources["foo/src/foundation/Types.h"] = "namespace acs { using u32 = ::uint32_t; }"
        nested_source_violations = scan_presence_mutation(nested_source_sources).violations
        if [(item.rule, item.type_name) for item in nested_source_violations] != [("ACS-R020d", "u32")]:
            print(f"type role nested source path self-test failed: {nested_source_violations}", file=sys.stderr)
            return False
        preprocessor_sources = {relative_path: f"#if 0\n{source}\n#endif\n" for relative_path, source in presence_sources.items()}
        preprocessor_violations = scan_presence_mutation(preprocessor_sources).violations
        expected_disabled_names = {
            *(name.rsplit("::", 1)[-1] for name in LEGACY_COMPATIBILITY_ALIASES),
            *(name.rsplit("::", 1)[-1] for name in CANONICAL_OBJECT_AND_CLASS_TYPES),
            *(name.rsplit("::", 1)[-1] for name in CANONICAL_SCALAR_ALIASES),
        }
        disabled_names = [
            item.type_name for item in preprocessor_violations if item.rule == "ACS-R020d"
        ]
        if set(disabled_names) != expected_disabled_names or len(disabled_names) != len(
            expected_disabled_names
        ):
            print(
                f"type role preprocessor disabled contract self-test failed: {preprocessor_violations}",
                file=sys.stderr,
            )
            return False
        premigration_sources = dict(presence_sources)
        premigration_sources["ecs/ComponentId.h"] = "namespace acs { using ComponentTypeId=u32; using ComponentSignatureId=u64; }"
        premigration_sources["event/EventTypeId.h"] = "namespace acs { using EventTypeId=u32; }"
        premigration_violations = scan_presence_mutation(premigration_sources).violations
        if [(item.rule, item.type_name) for item in premigration_violations] != [
            ("ACS-R020d", "ComponentTypeId"),
            ("ACS-R020d", "ComponentSignatureId"),
            ("ACS-R020d", "EventTypeId"),
        ]:
            print(f"type role premigration classification self-test failed: {premigration_violations}", file=sys.stderr)
            return False
        coordinated_sources = dict(presence_sources)
        coordinated_sources["event/EventTypeId.h"] = "namespace acs {}"
        coordinated_violations = scan_presence_mutation(coordinated_sources).violations
        if [(item.rule, item.type_name) for item in coordinated_violations] != [("ACS-R020d", "EventTypeId"), ("ACS-R020d", "FEventTypeId")]:
            print(f"type role coordinated delete self-test failed: {coordinated_violations}", file=sys.stderr)
            return False
        canonical_attribute_sources = dict(presence_sources)
        canonical_attribute_sources["event/EventTypeId.h"] = canonical_attribute_sources["event/EventTypeId.h"].replace("using FEventTypeId=u32;", "using FEventTypeId [[deprecated]]=u32;")
        canonical_attribute_violations = scan_presence_mutation(
            canonical_attribute_sources
        ).violations
        if [(item.rule, item.type_name) for item in canonical_attribute_violations] != [("ACS-R020d", "FEventTypeId")]:
            print(f"type role canonical attribute self-test failed: {canonical_attribute_violations}", file=sys.stderr)
            return False
        legacy_attribute_sources = dict(presence_sources)
        legacy_attribute_sources["event/EventTypeId.h"] = legacy_attribute_sources["event/EventTypeId.h"].replace("using EventTypeId=FEventTypeId;", "using EventTypeId [[deprecated]]=FEventTypeId;")
        legacy_attribute_violations = scan_presence_mutation(legacy_attribute_sources).violations
        if [(item.rule, item.type_name) for item in legacy_attribute_violations] != [("ACS-R020d", "EventTypeId")]:
            print(f"type role legacy attribute self-test failed: {legacy_attribute_violations}", file=sys.stderr)
            return False
        inner_linkage_sources = dict(presence_sources)
        inner_linkage_sources["event/EventTypeId.h"] = "namespace acs { extern \"C\" { using FEventTypeId=u32; using EventTypeId=FEventTypeId; } }"
        if scan_presence_mutation(inner_linkage_sources).violations:
            print("type role inner linkage self-test failed", file=sys.stderr)
            return False
        outer_linkage_sources = dict(presence_sources)
        outer_linkage_sources["event/EventTypeId.h"] = "extern \"C\" { namespace acs { using FEventTypeId=u32; using EventTypeId=FEventTypeId; } }"
        if scan_presence_mutation(outer_linkage_sources).violations:
            print("type role outer linkage self-test failed", file=sys.stderr)
            return False

        legacy_declarations = {
            "acs::ComponentSignatureId": ("ecs/ComponentId.h", "using ComponentSignatureId=FComponentSignatureId;", "using ComponentSignatureId=FComponentTypeId;"),
            "acs::ComponentTypeId": ("ecs/ComponentId.h", "using ComponentTypeId=FComponentTypeId;", "using ComponentTypeId=FComponentSignatureId;"),
            "acs::EventTypeId": ("event/EventTypeId.h", "using EventTypeId=FEventTypeId;", "using EventTypeId=FComponentTypeId;"),
        }
        registered_canonical_names = [entry[1] for entry in REGISTERED_TYPE_ROLE_MIGRATIONS]
        for legacy, canonical, _, legacy_path, _, _ in REGISTERED_TYPE_ROLE_MIGRATIONS:
            if legacy is None or legacy not in legacy_contract_names:
                continue
            if legacy_path is None:
                print(f"type role legacy path pairing failed: {legacy}", file=sys.stderr)
                return False
            # target swapは別の登録済み正規型へ向け、単なる未宣言名にしない。
            wrong_target = next(name for name in registered_canonical_names if name != canonical)
            legacy_name = legacy.rsplit("::", 1)[-1]
            canonical_name = canonical.rsplit("::", 1)[-1]
            legacy_declarations[legacy] = (
                legacy_path,
                f"using {legacy_name}={canonical_name};",
                f"using {legacy_name}=::{wrong_target};",
            )
        for case_index, (qualified_name, (expected_path, declaration, wrong_declaration)) in enumerate(sorted(legacy_declarations.items())):
            alias_name = qualified_name.rsplit("::", 1)[-1]
            namespace = qualified_name.rsplit("::", 1)[0]
            deletion_sources = dict(presence_sources)
            deletion_sources[expected_path] = deletion_sources[expected_path].replace(declaration, "")
            duplicate_sources = dict(presence_sources)
            duplicate_sources[expected_path] += f"\nnamespace {namespace} {{ {declaration} }}"
            moved_sources = dict(deletion_sources)
            moved_sources[f"event/MovedLegacy{case_index}.h"] = f"namespace {namespace} {{ {declaration} }}"
            target_sources = dict(presence_sources)
            target_sources[expected_path] = target_sources[expected_path].replace(declaration, wrong_declaration)
            relative_target_sources = dict(presence_sources)
            canonical_target = LEGACY_COMPATIBILITY_ALIASES[qualified_name]
            relative_declaration = declaration.replace(canonical_target.rsplit("::", 1)[-1], canonical_target)
            relative_target_sources[expected_path] = relative_target_sources[expected_path].replace(declaration, relative_declaration)
            for mutation_name, mutation_sources in (
                ("delete", deletion_sources),
                ("duplicate", duplicate_sources),
                ("move", moved_sources),
                ("target", target_sources),
                ("relative-target", relative_target_sources),
            ):
                mutation_violations = scan_presence_mutation(mutation_sources).violations
                if [(item.rule, item.type_name) for item in mutation_violations] != [("ACS-R020d", alias_name)]:
                    print(f"type role legacy contract mutation failed: {qualified_name}/{mutation_name}: {mutation_violations}", file=sys.stderr)
                    return False

        canonical_type_declarations = {
            canonical: (
                expected_path,
                f"{kind} {canonical.rsplit('::', 1)[-1]} {{}};",
                f"{'struct' if kind == 'class' else 'class'} {canonical.rsplit('::', 1)[-1]} {{}};",
            )
            for _, canonical, expected_path, _, kind, _ in REGISTERED_TYPE_ROLE_MIGRATIONS
            if canonical in canonical_contract_names
        }
        for case_index, (qualified_name, (expected_path, declaration, wrong_declaration)) in enumerate(sorted(canonical_type_declarations.items())):
            namespace = qualified_name.rsplit("::", 1)[0]
            deletion_sources = dict(presence_sources)
            deletion_sources[expected_path] = deletion_sources[expected_path].replace(declaration, "")
            duplicate_sources = dict(presence_sources)
            duplicate_sources[expected_path] += f"\nnamespace {namespace} {{ {declaration} }}"
            moved_sources = dict(deletion_sources)
            moved_sources[f"event/MovedCanonicalType{case_index}.h"] = f"namespace {namespace} {{ {declaration} }}"
            keyword_sources = dict(presence_sources)
            keyword_sources[expected_path] = keyword_sources[expected_path].replace(declaration, wrong_declaration)
            for mutation_name, mutation_sources in (
                ("delete", deletion_sources),
                ("duplicate", duplicate_sources),
                ("move", moved_sources),
                ("keyword", keyword_sources),
            ):
                mutation_violations = scan_presence_mutation(mutation_sources).violations
                hard_contract = [
                    item
                    for item in mutation_violations
                    if item.rule == "ACS-R020d"
                    and item.role_reason == "hard-canonical"
                    and item.qualified_type == qualified_name
                ]
                if len(hard_contract) != 1:
                    print(
                        f"type role canonical type mutation failed: {qualified_name}/{mutation_name}: {mutation_violations}",
                        file=sys.stderr,
                    )
                    return False

            legacy_name = next(
                (
                    name
                    for name, target in LEGACY_COMPATIBILITY_ALIASES.items()
                    if target == qualified_name
                ),
                None,
            )
            if legacy_name is None:
                continue
            legacy_definition_sources = dict(presence_sources)
            legacy_definition_sources[expected_path] += (
                f"\nnamespace {namespace} {{ class {legacy_name.rsplit('::', 1)[-1]} {{}}; }}"
            )
            legacy_definition_violations = scan_presence_mutation(
                legacy_definition_sources
            ).violations
            if len(
                [
                    item
                    for item in legacy_definition_violations
                    if item.rule == "ACS-R020d"
                    and item.role_reason == "legacy-definition"
                    and item.qualified_type == legacy_name
                ]
            ) != 1:
                print(
                    f"type role legacy definition mutation failed: {legacy_name}: {legacy_definition_violations}",
                    file=sys.stderr,
                )
                return False

        canonical_declarations = {
            "acs::FComponentSignatureId": ("ecs/ComponentId.h", "using FComponentSignatureId=u64;", "using FComponentSignatureId=u32;"),
            "acs::FComponentTypeId": ("ecs/ComponentId.h", "using FComponentTypeId=u32;", "using FComponentTypeId=u64;"),
            "acs::FEventTypeId": ("event/EventTypeId.h", "using FEventTypeId=u32;", "using FEventTypeId=u64;"),
        }
        for case_index, (qualified_name, (expected_path, declaration, wrong_declaration)) in enumerate(sorted(canonical_declarations.items())):
            alias_name = qualified_name.rsplit("::", 1)[-1]
            deletion_sources = dict(presence_sources)
            deletion_sources[expected_path] = deletion_sources[expected_path].replace(declaration, "")
            duplicate_sources = dict(presence_sources)
            duplicate_sources[expected_path] += f"\nnamespace acs {{ {declaration} }}"
            moved_sources = dict(deletion_sources)
            moved_sources[f"event/MovedCanonical{case_index}.h"] = f"namespace acs {{ {declaration} }}"
            target_sources = dict(presence_sources)
            target_sources[expected_path] = target_sources[expected_path].replace(declaration, wrong_declaration)
            for mutation_name, mutation_sources in (
                ("delete", deletion_sources),
                ("duplicate", duplicate_sources),
                ("move", moved_sources),
                ("target", target_sources),
            ):
                mutation_violations = scan_presence_mutation(mutation_sources).violations
                if [(item.rule, item.type_name) for item in mutation_violations] != [("ACS-R020d", alias_name)]:
                    print(f"type role canonical contract mutation failed: {qualified_name}/{mutation_name}: {mutation_violations}", file=sys.stderr)
                    return False

        required_bypass_sources = dict(presence_sources)
        required_bypass_sources["event/EventTypeId.h"] = required_bypass_sources["event/EventTypeId.h"].replace(
            "using EventTypeId=FEventTypeId;",
            "template<class T> using EventTypeId=FEventTypeId;",
        )
        bypass_violations = scan_presence_mutation(required_bypass_sources).violations
        if [(item.rule, item.type_name) for item in bypass_violations] != [("ACS-R020d", "EventTypeId")]:
            print(f"type role required template bypass self-test failed: {bypass_violations}", file=sys.stderr)
            return False
        required_bypass_sources = dict(presence_sources)
        required_bypass_sources["event/MessageBroker.h"] = required_bypass_sources["event/MessageBroker.h"].replace(
            "using FMessageBroker=CMessageBroker;",
            "using FMessageBroker=void(*)();",
        )
        bypass_violations = scan_presence_mutation(required_bypass_sources).violations
        if [(item.rule, item.type_name) for item in bypass_violations] != [("ACS-R020d", "FMessageBroker")]:
            print(f"type role required callback bypass self-test failed: {bypass_violations}", file=sys.stderr)
            return False

        for case_index, qualified_name in enumerate(sorted(legacy_contract_names)):
            alias_name = qualified_name.rsplit("::", 1)[-1]
            namespace = qualified_name.rsplit("::", 1)[0]
            reentry_sources = dict(presence_sources)
            reentry_sources[f"event/LegacyUse{case_index}.cpp"] = f"namespace {namespace} {{ {alias_name}* LegacyValue = nullptr; }}"
            reentry_violations = scan_presence_mutation(reentry_sources).violations
            if [(item.rule, item.type_name) for item in reentry_violations] != [("ACS-R020e", alias_name)]:
                print(f"type role legacy reentry self-test failed: {qualified_name}: {reentry_violations}", file=sys.stderr)
                return False

        for case_index, qualified_name in enumerate(sorted(legacy_contract_names)):
            alias_name = qualified_name.rsplit("::", 1)[-1]
            namespace = qualified_name.rsplit("::", 1)[0]
            forward_sources = dict(presence_sources)
            forward_sources[f"event/LegacyForward{case_index}.cpp"] = f"namespace {namespace} {{ class {alias_name}; }}"
            forward_violations = scan_presence_mutation(forward_sources).violations
            if [(item.rule, item.type_name) for item in forward_violations] != [("ACS-R020e", alias_name)]:
                print(f"type role legacy forward self-test failed: {qualified_name}: {forward_violations}", file=sys.stderr)
                return False
    print(f"cpp_type_role_self_test=ok violations={len(expected)}")
    return True


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    """コマンドライン引数を解析する。"""

    parser = argparse.ArgumentParser(description="C++型の役割とA/C/F/I/T/E接頭辞を監査します。")
    parser.add_argument("--root", type=Path, help="監査するC++ツリー")
    parser.add_argument(
        "--migration-debt",
        type=Path,
        default=DEFAULT_MIGRATION_DEBT,
        help="未完了の型役割reviewを固定するexact debt JSON",
    )
    parser.add_argument("--format", choices=("human", "json"), default="human")
    parser.add_argument("--json-output", type=Path, help="全結果を保存するJSONファイル")
    parser.add_argument("--max-diagnostics", type=int, default=200)
    parser.add_argument("--quiet", action="store_true", help="結果を画面へ出さず終了値とJSONだけを返す")
    parser.add_argument("--self-test", action="store_true")
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    """監査を実行し、違反があれば終了値1を返す。"""

    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="replace")
    arguments = parse_args(argv)
    if arguments.self_test:
        return 0 if run_self_test() else 1
    if arguments.root is None or not arguments.root.exists():
        print("監査rootが見つかりません", file=sys.stderr)
        return 2
    try:
        result = scan_tree(arguments.root, arguments.migration_debt)
        report = build_json_report(result)
        if arguments.json_output is not None:
            write_json(arguments.json_output, report)
        if not arguments.quiet:
            if arguments.format == "json":
                json.dump(report, sys.stdout, ensure_ascii=False, indent=2)
                sys.stdout.write("\n")
            else:
                sys.stdout.write(
                    format_human_report(result, max(0, arguments.max_diagnostics))
                )
    except (OSError, UnicodeError, ValueError) as error:
        print(f"監査に失敗しました: {error}", file=sys.stderr)
        return 2
    return 1 if result.violations else 0


if __name__ == "__main__":
    raise SystemExit(main())
