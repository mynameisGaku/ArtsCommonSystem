#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""C++型の役割とA/F/I/T/E接頭辞の対応を監査する。"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
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
SCHEMA_VERSION = 1


@dataclass(frozen=True)
class FTypeDefinition:
    """意味分類に必要な一つの型定義。"""

    path: Path
    keyword: str
    name: str
    line: int
    column: int
    bases: tuple[str, ...]
    body: tuple[FToken, ...]
    is_template: bool


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


@dataclass(frozen=True)
class FScanResult:
    """監査対象と検出結果。"""

    root: Path
    files: tuple[Path, ...]
    definitions: tuple[FTypeDefinition, ...]
    violations: tuple[FViolation, ...]
    expected_prefix_counts: dict[str, int]


def _tokens(source: str) -> list[FToken]:
    """既存C++ lexerでコメント、文字列、raw文字列を除いたトークンを返す。"""

    return lex_cpp(source)


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


def _find_body_start(tokens: Sequence[FToken], start: int) -> Optional[int]:
    """型名の後ろから定義本体の開始位置を探す。"""

    angle_depth = 0
    parenthesis_depth = 0
    bracket_depth = 0
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


def _type_definitions(path: Path, tokens: Sequence[FToken]) -> tuple[FTypeDefinition, ...]:
    """C++トークンから本体を持つclass、struct、union、enumを集める。"""

    brace_pairs = _brace_pairs(tokens)
    definitions: list[FTypeDefinition] = []
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
        definitions.append(
            FTypeDefinition(
                path,
                text,
                name_token.text,
                name_token.line,
                name_token.column,
                _base_names(base_tokens),
                tuple(tokens[body_start + 1 : body_end]),
                pending_template and not enum_declaration,
            )
        )
        pending_template = False
        position += 1
    return tuple(definitions)


def _registered_object_names(tokens: Sequence[FToken]) -> frozenset[str]:
    """ACS_OBJECT系マクロの第一引数にある型名を集める。"""

    names: set[str] = set()
    for position, token in enumerate(tokens):
        if token.text not in OBJECT_MACROS or position + 1 >= len(tokens) or tokens[position + 1].text != "(":
            continue
        candidates: list[str] = []
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
            elif depth == 1 and IDENTIFIER.match(text):
                candidates.append(text)
            cursor += 1
        if candidates:
            names.add(candidates[-1])
    return frozenset(names)


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


def _method_names(statements: Sequence[Sequence[FToken]]) -> tuple[str, ...]:
    """メンバー関数らしい宣言から関数名を集める。"""

    names: set[str] = set()
    ignored = frozenset({"alignas", "decltype", "if", "noexcept", "requires", "sizeof", "static_assert"})
    for statement in statements:
        for position, token in enumerate(statement):
            if token.text != "(" or position == 0:
                continue
            candidate = statement[position - 1].text
            prefix = tuple(item.text for item in statement[:position])
            if (
                IDENTIFIER.match(candidate)
                and candidate not in ignored
                and "static" not in prefix
            ):
                names.add(candidate)
                break
    return tuple(sorted(names))


def _has_probable_data(statements: Sequence[Sequence[FToken]]) -> bool:
    """関数やaliasではないインスタンスデータ宣言がありそうかを返す。"""

    skipped_heads = frozenset({"class", "enum", "friend", "static_assert", "struct", "template", "typedef", "union", "using"})
    for statement in statements:
        texts = [token.text for token in statement]
        while len(texts) >= 2 and texts[0] in {"private", "protected", "public"} and texts[1] == ":":
            texts = texts[2:]
        if not texts or texts[0] in skipped_heads or "(" in texts:
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


def _expected_prefix(
    definition: FTypeDefinition,
    features: FTypeFeatures,
    managed_names: frozenset[str],
) -> tuple[str, tuple[str, ...]]:
    """型の意味的特徴から期待する接頭辞と根拠を返す。"""

    if definition.keyword == "enum":
        return "E", ("列挙型",)
    if definition.is_template:
        return "T", ("template型",)
    if definition.name in managed_names:
        return "A", ("FObject系またはACS_OBJECT登録",)

    stem = _role_stem(definition.name)
    value_named = _ends_with_word(stem, VALUE_WORDS)
    behavior_named = _ends_with_word(stem, BEHAVIOR_WORDS)
    lifecycle_methods = tuple(method for method in features.methods if method in LIFECYCLE_METHODS)
    service_named = behavior_named or bool(lifecycle_methods) or features.has_coordination
    if _is_interface(definition, features) and (
        definition.name.startswith("I") or not value_named and not service_named
    ):
        return "I", ("データを持たない仮想interface",)
    if stem.startswith("Scoped"):
        return "F", ("有効範囲内の資源を一つの値として管理",)
    if value_named and not service_named:
        evidence = ["値を表す型名"]
        if features.has_private_or_protected:
            evidence.append("値の内部表現を非公開")
        return "F", tuple(evidence)
    if definition.keyword in {"struct", "union"} and not service_named:
        return "F", ("公開データ中心",)

    evidence: list[str] = []
    if behavior_named:
        evidence.append("共有利用または処理を担うservice型名")
    if lifecycle_methods:
        evidence.append("状態や寿命を動かす操作: " + ", ".join(lifecycle_methods))
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
        evidence.append("具象の値・handle・service")
    return "F", tuple(evidence)


def _managed_names(
    definitions: Sequence[FTypeDefinition],
    registered_names: frozenset[str],
) -> frozenset[str]:
    """FObject継承とACS_OBJECT登録を推移的にたどる。"""

    managed = set(registered_names)
    changed = True
    while changed:
        changed = False
        for definition in definitions:
            if definition.name in managed:
                continue
            if any(
                base == "FObject"
                or base in managed
                or bool(re.match(r"^A[A-Z]", base))
                for base in definition.bases
            ):
                managed.add(definition.name)
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
        expected, evidence = _expected_prefix(definition, features, managed_names)
        counts[expected] = counts.get(expected, 0) + 1
        observed = definition.name[0] if ROLE_NAME.match(definition.name) else ""
        if observed != expected:
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


def _display_path(path: Path, root: Path) -> str:
    """root相対の表示用パスを返す。"""

    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def scan_tree(root: Path) -> FScanResult:
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
    registered_names: set[str] = set()
    for path in files:
        source = path.read_text(encoding="utf-8-sig")
        tokens = _tokens(source)
        definitions.extend(_type_definitions(path, tokens))
        registered_names.update(_registered_object_names(tokens))
    managed_names = _managed_names(definitions, frozenset(registered_names))
    violations, counts = _audit_definitions(definitions, managed_names)
    return FScanResult(resolved_root, files, tuple(definitions), violations, counts)


def build_json_report(result: FScanResult) -> dict[str, object]:
    """監査結果を機械可読な辞書へ変換する。"""

    by_rule: dict[str, int] = {}
    for violation in result.violations:
        by_rule[violation.rule] = by_rule.get(violation.rule, 0) + 1
    return {
        "schema_version": SCHEMA_VERSION,
        "root": result.root.as_posix(),
        "scanned_file_count": len(result.files),
        "type_definition_count": len(result.definitions),
        "expected_prefix_counts": result.expected_prefix_counts,
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
        f"files={len(result.files)} types={len(result.definitions)} "
        f"violations={len(result.violations)}"
    )
    return "\n".join(lines) + "\n"


def write_json(path: Path, report: dict[str, object]) -> None:
    """既存監査器の安全な保存経路でUTF-8 JSONを保存する。"""

    error = try_write_json_report(path, serialize_json_report(report))
    if error is not None:
        raise OSError(str(error)) from error


def run_self_test() -> bool:
    """役割分類、旧C service、字句除外、JSONを一時fixtureで確認する。"""

    valid_source = r'''
// class FCommentRegistry { public: void Register(); };
const char* Text = "class AStringUtility {};";
const char* Shader = R"code(struct CShaderManager { private: int Value; };)code";
class FObject;
class AActor : public FObject { public: virtual ~AActor() = default; private: int State; };
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
class FAllocator { public: virtual void* Allocate(int Size) = 0; };
class FLuaVm { public: virtual void Shutdown() = 0; };
class FInlineService {
public:
    virtual void Run() = 0;
    int Count() const { return m_Count; }
private:
    int m_Count;
};
class FMessageBroker { public: void Clear(); private: int ChannelCount; };
class FTimerManager { public: void Tick(); private: int TimerCount; };
class FRegistry { public: void Register(); private: int Count; };
struct FConfig { int Width; int Height; };
class FString { public: int Size() const; private: char* Data; };
class FAsset { public: virtual ~FAsset() = default; private: int Payload; };
class FAssetFuture { private: TSharedPtr<FState> State; };
struct FScopedSession { void Release(); ~FScopedSession(); void* Handle; };
struct FHidden { private: int Value; };
template<typename T> class TBox { private: T Value; };
enum class EState : unsigned { Ready, Stopped };
using SimpleDelegate = void(*)();
using CCallback = void(*)();
template<typename T> using CallbackList = T;
'''
    invalid_source = r'''
class FObject;
class AUtilityManager { public: void Tick(); private: int State; };
class CMessageBroker { public: void Clear(); private: int Count; };
struct CTimerManager { private: int State; public: void Tick(); };
class COptions { private: int Value; };
class MessageBroker { public: void Clear(); };
class URegistry { public: void Register(); };
class Readable { public: virtual void Read() = 0; };
class Entity : public FObject {};
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
struct AEntity : public FObject {};
'''
    with tempfile.TemporaryDirectory(prefix="acs-type-role-") as directory:
        root = Path(directory)
        (root / "valid.h").write_text(valid_source, encoding="utf-8")
        (root / "invalid.h").write_text(invalid_source, encoding="utf-8")

        valid_result = scan_tree(root / "valid.h")
        definition_names = {definition.name for definition in valid_result.definitions}
        if (
            valid_result.violations
            or len(valid_result.definitions) != 22
            or valid_result.expected_prefix_counts
            != {"A": 3, "E": 1, "F": 13, "I": 4, "T": 1}
            or not {
                "EState",
                "FAllocator",
                "FInlineService",
                "FLuaVm",
                "FMessageBroker",
                "FTimerManager",
            }.issubset(definition_names)
        ):
            print(
                "type role self-test failed: "
                f"valid fixture={valid_result.violations} "
                f"types={len(valid_result.definitions)} "
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
            ("ACS-R020c", "AUtilityManager", "F"),
            ("ACS-R020c", "CMessageBroker", "F"),
            ("ACS-R020c", "CTimerManager", "F"),
            ("ACS-R020c", "COptions", "F"),
            ("ACS-R020c", "MessageBroker", "F"),
            ("ACS-R020c", "URegistry", "F"),
            ("ACS-R020c", "Readable", "I"),
            ("ACS-R020c", "Entity", "A"),
            ("ACS-R020c", "Box", "T"),
            ("ACS-R020c", "State", "E"),
            ("ACS-R020c", "CLuaVm", "F"),
            ("ACS-R020c", "FAbstract", "I"),
            ("ACS-R020c", "IRecord", "F"),
            ("ACS-R020c", "FRegistered", "A"),
            ("ACS-R020c", "FBox", "T"),
            ("ACS-R020c", "FState", "E"),
            ("ACS-R020c", "TConcrete", "F"),
            ("ACS-R020c", "IConcrete", "F"),
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
            or report["violations_by_rule"] != {"ACS-R020c": len(expected)}
        ):
            print(f"type role JSON self-test failed: {report}", file=sys.stderr)
            return False
        report_path = root / "type-role-report.json"
        write_json(report_path, report)
        if json.loads(report_path.read_text(encoding="utf-8")) != report:
            print("type role JSON write self-test failed", file=sys.stderr)
            return False
    print(f"cpp_type_role_self_test=ok violations={len(expected)}")
    return True


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    """コマンドライン引数を解析する。"""

    parser = argparse.ArgumentParser(description="C++型の役割とA/F/I/T/E接頭辞を監査します。")
    parser.add_argument("--root", type=Path, help="監査するC++ツリー")
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
        result = scan_tree(arguments.root)
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
