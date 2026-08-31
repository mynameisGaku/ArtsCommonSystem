# SPDX-License-Identifier: MIT

from __future__ import annotations

import hashlib
import html
import json
import re
import sys
import unicodedata
from collections import defaultdict
from pathlib import Path

from .model import (
    FFeatureMemberRecord,
    FFeatureRecord,
    FGlossaryRecord,
    FGuideRecord,
    FReferenceCatalog,
    FSymbolRecord,
    FTroubleshootingRecord,
)


JAPANESE_PATTERN = re.compile(r"[\u3040-\u30ff\u3400-\u9fff]")
MARKUP_PATTERN = re.compile(r"</?(?:b|code|small)\s*>")
PAIRED_MARKUP_PATTERN = re.compile(
    r"<(b|code|t|small)\s*>(.*?)</\1\s*>",
    flags=re.S,
)
BR_PATTERN = re.compile(r"<br\s*/?>", flags=re.I)
EXTERNAL_ENTITY_PATTERN = re.compile(
    r"Unity|Unreal(?: Engine)?|\bUE\b|Godot|\bRust\b|\bP4\b|"
    r"GoogleTest|Murmur3|enable_shared_from_this|std::[A-Za-z_]\w*|"
    r"Maya|Blender|Core\s+Keeper|Cinemachine|AAA\s*タイトル|DXLib|"
    r"VS\s*Code|\bUE5\b|Slay\s+the\s+Spire|Hearthstone|\bMtG\b|"
    r"DayZ|The\s+Long\s+Dark|Don'?t\s+Starve|Vampire\s+Survivors|Hades|"
    r"Bejeweled|Candy\s+Crush|OneDrive|Azure|OpenAI\s+Moderation|"
    r"Sentry|Crashpad|BugSnag"
)
ANALOGY_PATTERN = re.compile(r"相当|同様|風(?:モデル)?|的な感覚|最小版|に似た|代替|との比較|→")
CITATION_PATTERN = re.compile(r"CEDEC|GDC|SIGGRAPH|引用|出典|講演|論文")
PROCESS_PATTERN = re.compile(
    r"過去に|将来|暫定|今後(?:対応|実装|追加)|実装予定|移行中"
)
PHASE_TOKEN_PATTERN = re.compile(
    r"(?:、\s*)?(?:[（(]\s*)?\bPhase\s*\d+[A-Za-z]?\b(?:\s*[）)])?",
    flags=re.I,
)


KIND_LABELS = {
    "class": "クラス",
    "struct": "構造体",
    "union": "共用体",
    "enum": "列挙型",
    "enum class": "列挙型",
    "function": "関数",
    "method": "メンバー関数",
    "member-variable": "メンバー変数",
    "constant": "定数",
    "enum-value": "列挙値",
    "macro": "マクロ",
    "alias": "型alias",
}


def _legacy_parser():
    scripts_directory = Path(__file__).resolve().parents[1]
    if str(scripts_directory) not in sys.path:
        sys.path.insert(0, str(scripts_directory))
    import generate_reference  # type: ignore

    return generate_reference


def normalize_space(value: str) -> str:
    return re.sub(r"\s+", " ", value.strip())


def strip_rich_text(value: object) -> str:
    text = str(value or "")
    text = BR_PATTERN.sub(" ", text)
    while PAIRED_MARKUP_PATTERN.search(text):
        text = PAIRED_MARKUP_PATTERN.sub(lambda match: match.group(2), text)
    text = MARKUP_PATTERN.sub("", text)
    return normalize_space(html.unescape(text))


def sanitize_acs_prose(value: object) -> str:
    """ACS外の比較・出典・工程文を文単位で除き、現在のACS説明を残す。"""
    text = strip_rich_text(value)
    kept: list[str] = []
    for sentence in re.split(r"(?<=[。！？])\s*|\n+", text):
        had_phase = PHASE_TOKEN_PATTERN.search(sentence) is not None
        sentence = PHASE_TOKEN_PATTERN.sub("", sentence)
        sentence = sentence.strip()
        if not sentence:
            continue
        if had_phase and re.match(r"^の?(?:実装|工程|段階|対応)", sentence):
            continue
        if CITATION_PATTERN.search(sentence) or PROCESS_PATTERN.search(sentence):
            continue
        if EXTERNAL_ENTITY_PATTERN.search(sentence):
            continue
        kept.append(sentence)
    return normalize_space(" ".join(kept))


def japanese_text(value: object, fallback: str) -> str:
    text = sanitize_acs_prose(value)
    return text if JAPANESE_PATTERN.search(text) else fallback


def slug(value: str) -> str:
    normalized = unicodedata.normalize("NFKC", strip_rich_text(value)).lower()
    normalized = re.sub(r"[^a-z0-9\u3040-\u30ff\u3400-\u9fff]+", "-", normalized)
    normalized = normalized.strip("-")
    return normalized[:72] or "item"


def stable_id(*parts: object) -> str:
    material = "\x1f".join(normalize_space(str(part)) for part in parts)
    return hashlib.sha256(material.encode("utf-8")).hexdigest()[:20]


def _kind_description(kind: str) -> str:
    return KIND_LABELS.get(kind, "API")


def _default_description(name: str, module: str, kind: str, parent: str = "") -> str:
    label = _kind_description(kind)
    if parent:
        return f"{parent} が提供する {name} {label}です。"
    return f"ACS の {module} モジュールで {name} を表す{label}です。"


def _qualified_name(namespace: str, name: str) -> str:
    return f"{namespace}::{name}" if namespace else name


def _extract_function_name(signature: str, owner_name: str) -> str:
    text = normalize_space(signature)
    operator_match = re.search(
        r"\b(operator\s*(?:\(\)|\[\]|new\[\]|delete\[\]|new|delete|"
        r"<=>|<<=|>>=|==|!=|<=|>=|&&|\|\||\+\+|--|->\*|->|"
        r"[+\-*/%<>=!&|^~,]+|[A-Za-z_]\w*(?:::\w+)*))\s*\(",
        text,
    )
    if operator_match:
        return normalize_space(operator_match.group(1))
    conversion_match = re.search(
        r"\boperator\s+((?:(?:const|volatile)\s+)*"
        r"[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*(?:\s*<[^(){};]+>)?"
        r"(?:\s+(?:const|volatile))?(?:\s*[*&]+)?)\s*\(",
        text,
    )
    if conversion_match:
        return "operator " + normalize_space(conversion_match.group(1))
    prefix = text.split("(", 1)[0].strip()
    destructor_match = re.search(r"(~[A-Za-z_]\w*)\s*$", prefix)
    if destructor_match:
        return destructor_match.group(1)
    identifier_match = re.search(r"([A-Za-z_]\w*)\s*$", prefix)
    if identifier_match:
        return identifier_match.group(1)
    return owner_name


def _extract_member_name(signature: str) -> str | None:
    text = normalize_space(signature).rstrip(";")
    text = re.sub(r"\s*=.*$", "", text)
    text = re.sub(r"\s*\{.*\}\s*$", "", text)
    text = re.sub(r"(?:\[[^\]]*\]\s*)+$", "", text)
    text = re.sub(r"\s*:\s*\d+\s*$", "", text)
    pointer_match = re.search(r"\(\s*[*&]+\s*([A-Za-z_]\w*)\s*\)", text)
    if pointer_match:
        return pointer_match.group(1)
    identifier_match = re.search(r"([A-Za-z_]\w*)\s*$", text)
    return identifier_match.group(1) if identifier_match else None


def _member_kind_and_name(signature: str) -> tuple[str, str | None]:
    text = normalize_space(signature)
    using_match = re.match(r"(?:template\s*<[^>]+>\s*)?using\s+([A-Za-z_]\w*)\s*=", text)
    if using_match:
        return "alias", using_match.group(1)
    typedef_match = re.match(r"typedef\s+.+?\s+([A-Za-z_]\w*)\s*;?$", text)
    if typedef_match:
        return "alias", typedef_match.group(1)
    kind = "constant" if _is_constant_signature(signature) else "member-variable"
    return kind, _extract_member_name(signature)


def _is_constant_signature(signature: str) -> bool:
    text = f" {normalize_space(signature)} "
    return (
        " constexpr " in text
        or " consteval " in text
        or " constinit " in text
        or bool(re.search(r"\bstatic\s+const\b", text))
    )


def _root_signature(declaration: object) -> str:
    kind = str(declaration.kind)
    explicit_signature = normalize_space(getattr(declaration, "signature", ""))
    if explicit_signature:
        return explicit_signature
    if kind == "function" and declaration.functions:
        return normalize_space(declaration.functions[0].signature)
    if kind == "macro" and declaration.members:
        return normalize_space(declaration.members[0].signature)
    if kind in {"enum", "enum class", "class", "struct", "union"}:
        template_prefix = ""
        try:
            lines = declaration.path.read_text(encoding="utf-8", errors="replace").splitlines()
            index = max(0, int(declaration.line) - 2)
            candidates: list[str] = []
            while index >= 0 and len(candidates) < 6:
                candidate = lines[index].strip()
                if not candidate:
                    if candidates:
                        break
                    index -= 1
                    continue
                candidates.insert(0, candidate)
                if candidate.startswith("template"):
                    break
                if not candidates[-1].startswith((">", "requires")):
                    break
                index -= 1
            joined = normalize_space(" ".join(candidates))
            if joined.startswith("template"):
                template_prefix = joined + " "
        except OSError:
            template_prefix = ""
        return f"{template_prefix}{kind} {declaration.name};"
    return str(declaration.name)


def _template_specialization(signature: str, name: str) -> str:
    """型名直後のbalancedなtemplate-idを表示用に返す。"""

    match = re.search(
        rf"(?<![A-Za-z0-9_]){re.escape(name)}(?![A-Za-z0-9_])",
        signature,
    )
    if not match:
        return ""
    start = match.end()
    while start < len(signature) and signature[start].isspace():
        start += 1
    if start >= len(signature) or signature[start] != "<":
        return ""
    depth = 0
    for index in range(start, len(signature)):
        character = signature[index]
        if character == "<":
            depth += 1
        elif character == ">":
            depth -= 1
            if depth == 0:
                return normalize_space(signature[start:index + 1])
    return ""


def _root_route(symbol: FSymbolRecord) -> str:
    return f"symbols/{slug(symbol.module)}/{slug(symbol.name)}-{symbol.id[:8]}.html"


def _child_route(parent: FSymbolRecord, child: FSymbolRecord) -> str:
    parent_route = Path(parent.route)
    return (
        parent_route.parent
        / parent_route.stem
        / f"{slug(child.name)}-{child.id[:8]}.html"
    ).as_posix()


def _symbol_catalog(acs_root: Path) -> list[FSymbolRecord]:
    legacy = _legacy_parser()
    _, declarations = legacy.parse_all_headers(acs_root / "src")
    declarations = sorted(
        declarations,
        key=lambda item: (
            not bool(getattr(item, "is_definition", False)),
            bool(getattr(item, "owner_qualified_name", "") or getattr(item, "owner_signature", "")),
            str(item.path).casefold(),
            int(item.line),
        ),
    )
    symbols: list[FSymbolRecord] = []
    roots_by_identity: dict[tuple[str, ...], FSymbolRecord] = {}
    roots_by_semantic_name: dict[tuple[str, ...], list[FSymbolRecord]] = defaultdict(list)
    roots_by_qualified_name: dict[str, list[FSymbolRecord]] = defaultdict(list)
    roots_by_source_name: dict[tuple[str, str], list[FSymbolRecord]] = defaultdict(list)
    roots_by_source_signature: dict[tuple[str, str, str], list[FSymbolRecord]] = defaultdict(list)
    children_by_identity: dict[tuple[str, str, str, str], FSymbolRecord] = {}
    member_owners: dict[tuple[str, tuple[str, ...]], FSymbolRecord] = {}
    source_quality: dict[str, int] = {}
    access_quality: dict[str, int] = {}

    def add_source_lookup(source_path: str, scope_name: str, root: FSymbolRecord) -> None:
        name_key = (source_path, scope_name)
        signature_key = (source_path, scope_name, root.signature)
        if all(item.id != root.id for item in roots_by_source_name[name_key]):
            roots_by_source_name[name_key].append(root)
        if all(item.id != root.id for item in roots_by_source_signature[signature_key]):
            roots_by_source_signature[signature_key].append(root)

    def add_child(
        parent: FSymbolRecord,
        kind: str,
        name: str,
        signature: str,
        description: str,
        access: str,
        source_path: str,
        source_line: int,
    ) -> FSymbolRecord:
        normalized_signature = normalize_space(signature)
        identity = (parent.id, kind, name, normalized_signature)
        existing = children_by_identity.get(identity)
        if existing:
            if source_path not in existing.declaration_paths:
                existing.declaration_paths.append(source_path)
            fallback = _default_description(name, parent.module, kind, parent.name)
            if existing.description == fallback and description != fallback:
                existing.description = description
            return existing
        child_id = stable_id(*identity)
        separator = "." if parent.kind == "member-variable" else "::"
        child = FSymbolRecord(
            id=child_id,
            module=parent.module,
            kind=kind,
            name=name,
            qualified_name=f"{parent.qualified_name}{separator}{name}",
            signature=normalized_signature,
            description=description,
            access=access,
            source_path=source_path,
            source_line=source_line,
            parent_id=parent.id,
            declaration_paths=[source_path],
        )
        child.route = _child_route(parent, child)
        parent.child_ids.append(child.id)
        symbols.append(child)
        children_by_identity[identity] = child
        return child

    def member_owner(root: FSymbolRecord, owner_path: tuple[str, ...]) -> FSymbolRecord:
        if not owner_path:
            return root
        owner = member_owners.get((root.id, owner_path))
        if owner is None:
            raise ValueError(
                "入れ子fieldのownerがありません: "
                f"{root.qualified_name}::{'.'.join(owner_path)}"
            )
        return owner

    for declaration in declarations:
        source_path = declaration.path.relative_to(acs_root).as_posix()
        namespace = str(declaration.namespace or "")
        signature = _root_signature(declaration)
        specialization = _template_specialization(signature, str(declaration.name))
        parent: FSymbolRecord | None = None
        owner_signature = normalize_space(str(getattr(declaration, "owner_signature", "") or ""))
        if owner_signature:
            exact_parents = roots_by_source_signature.get((source_path, namespace, owner_signature), [])
            exact_parent_ids = {item.id for item in exact_parents if item.kind in {"class", "struct", "union"}}
            if len(exact_parent_ids) == 1:
                parent = next(item for item in exact_parents if item.id in exact_parent_ids)
        if parent is None and owner_signature:
            name_parents = [
                item for item in roots_by_source_name.get((source_path, namespace), [])
                if item.kind in {"class", "struct", "union"}
            ]
            if len({item.id for item in name_parents}) == 1:
                parent = name_parents[0]
        owner_qualified_name = normalize_space(
            str(getattr(declaration, "owner_qualified_name", "") or "")
        )
        if parent is None and owner_qualified_name:
            qualified_parents = [
                item
                for item in roots_by_qualified_name.get(owner_qualified_name, [])
                if item.kind in {"class", "struct", "union"}
            ]
            if len({item.id for item in qualified_parents}) == 1:
                parent = qualified_parents[0]

        declaration_kind = str(declaration.kind)
        semantic_kind = "record" if declaration_kind in {"class", "struct"} else declaration_kind
        semantic_identity = (
            semantic_kind,
            namespace,
            str(declaration.name),
            specialization,
            parent.id if parent else "",
        )
        identity_signature = "" if declaration_kind == "macro" else signature
        root_identity = (
            declaration_kind,
            namespace,
            str(declaration.name),
            identity_signature,
            parent.id if parent else "",
        )
        fallback = _default_description(str(declaration.name), str(declaration.module), str(declaration.kind))
        description = japanese_text(declaration.doc, fallback)
        root = roots_by_identity.get(root_identity)
        if (
            root is None
            and not bool(getattr(declaration, "is_definition", False))
            and declaration_kind in {"class", "struct", "union", "enum", "enum class"}
        ):
            semantic_candidates = roots_by_semantic_name.get(semantic_identity, [])
            if len({item.id for item in semantic_candidates}) == 1:
                root = semantic_candidates[0]
        quality = 2 if bool(getattr(declaration, "is_definition", False)) else 1
        declaration_access_quality = (
            2 if owner_signature else 1
        )
        if root is None:
            root_id = stable_id(*root_identity)
            display_name = f"{declaration.name}{specialization}"
            qualified_name = (
                f"{parent.qualified_name}::{display_name}"
                if parent
                else _qualified_name(namespace, display_name)
            )
            root = FSymbolRecord(
                id=root_id,
                module=str(declaration.module),
                kind=str(declaration.kind),
                name=str(declaration.name),
                qualified_name=qualified_name,
                signature=signature,
                description=description,
                access=str(getattr(declaration, "access", "public")),
                source_path=source_path,
                source_line=int(declaration.line),
                parent_id=parent.id if parent else None,
                declaration_paths=[source_path],
            )
            root.route = _root_route(root)
            if parent:
                root.route = _child_route(parent, root)
                parent.child_ids.append(root.id)
            symbols.append(root)
            roots_by_identity[root_identity] = root
            roots_by_semantic_name[semantic_identity].append(root)
            scope_name = _qualified_name(namespace, str(declaration.name))
            roots_by_qualified_name[scope_name].append(root)
            source_quality[root.id] = quality
            access_quality[root.id] = declaration_access_quality
        else:
            if source_path not in root.declaration_paths:
                root.declaration_paths.append(source_path)
            if root.description == fallback and description != fallback:
                root.description = description
            if quality > source_quality[root.id]:
                root.source_path = source_path
                root.source_line = int(declaration.line)
                source_quality[root.id] = quality
            if declaration_access_quality > access_quality[root.id]:
                root.access = str(getattr(declaration, "access", "public"))
                access_quality[root.id] = declaration_access_quality
        add_source_lookup(
            source_path,
            _qualified_name(namespace, str(declaration.name)),
            root,
        )

        if parent and root.parent_id is None:
            root.parent_id = parent.id
            root.route = _child_route(parent, root)
            if root.id not in parent.child_ids:
                parent.child_ids.append(root.id)

        if declaration.kind in {"class", "struct", "union"}:
            for member in declaration.members:
                kind, name = _member_kind_and_name(member.signature)
                name = str(getattr(member, "name", "") or name or "") or None
                if name is None:
                    raise ValueError(
                        "field名を抽出できません: "
                        f"{source_path}:{int(member.line or root.source_line)}: {member.signature}"
                    )
                owner_path = tuple(getattr(member, "owner_path", ()) or ())
                parent_symbol = member_owner(root, owner_path)
                child = add_child(
                    parent_symbol,
                    kind,
                    name,
                    member.signature,
                    japanese_text(
                        member.doc,
                        _default_description(name, parent_symbol.module, kind, parent_symbol.name),
                    ),
                    str(member.access),
                    source_path,
                    int(member.line or root.source_line),
                )
                member_owners[(root.id, owner_path + (name,))] = child

            for member in declaration.functions:
                name = _extract_function_name(member.signature, str(declaration.name))
                owner_path = tuple(getattr(member, "owner_path", ()) or ())
                parent_symbol = member_owner(root, owner_path)
                add_child(
                    parent_symbol,
                    "method",
                    name,
                    member.signature,
                    japanese_text(
                        member.doc,
                        _default_description(name, parent_symbol.module, "method", parent_symbol.name),
                    ),
                    str(member.access),
                    source_path,
                    int(member.line or root.source_line),
                )

        if declaration.kind in {"enum", "enum class"}:
            value_infos = list(getattr(declaration, "value_infos", []))
            if not value_infos:
                value_infos = [legacy.EnumValueInfo(str(value), str(value), "", root.source_line) for value in declaration.values]
            for value in value_infos:
                name = str(value.name)
                add_child(
                    root,
                    "enum-value",
                    name,
                    str(value.signature),
                    japanese_text(value.doc, _default_description(name, root.module, "enum-value", root.name)),
                    "public",
                    source_path,
                    int(value.line or root.source_line),
                )

    _append_global_aliases_and_constants(acs_root, symbols)
    _validate_symbols(symbols)
    return sorted(symbols, key=lambda item: (item.module.casefold(), item.qualified_name.casefold(), item.id))


def _class_ranges(masked: str) -> list[tuple[int, int]]:
    legacy = _legacy_parser()
    return [
        (candidate.signature_start, candidate.close_index)
        for candidate in legacy.collect_type_candidates(masked)
    ]


def _inside_ranges(position: int, ranges: list[tuple[int, int]]) -> bool:
    return any(start <= position <= end for start, end in ranges)


def _namespace_constant_declarations(
    text: str,
    masked: str,
    ranges: list[tuple[int, int]],
    legacy: object,
) -> list[tuple[str, str, str, int]]:
    """namespace直下の定数宣言を関数本体と区別して抽出する。"""
    start_pattern = re.compile(
        r"(?m)^[ \t]*(?:template[ \t]*<[^;{}]+>[ \t\r\n]*)?"
        r"(?:(?:inline|static)\s+)*(?:constexpr|consteval|constinit)\b"
    )
    declarations: list[tuple[str, str, str, int]] = []
    for match in start_pattern.finditer(masked):
        position = match.start()
        if _inside_ranges(position, ranges) or not legacy.is_namespace_scope(text, position):
            continue

        signature_start = legacy._template_declaration_start(masked, position)
        cursor = position
        paren_depth = 0
        bracket_depth = 0
        brace_depth = 0
        function_body = False
        while cursor < len(masked):
            ch = masked[cursor]
            if ch == "(" and brace_depth == 0:
                paren_depth += 1
            elif ch == ")" and paren_depth > 0 and brace_depth == 0:
                paren_depth -= 1
            elif ch == "[" and brace_depth == 0:
                bracket_depth += 1
            elif ch == "]" and bracket_depth > 0 and brace_depth == 0:
                bracket_depth -= 1
            elif ch == "{" and paren_depth == bracket_depth == brace_depth == 0:
                prefix = masked[signature_start:cursor]
                if (
                    legacy._first_top_level(prefix, "=") < 0
                    and legacy._looks_like_function_declaration(prefix + ";")
                ):
                    function_body = True
                    break
                brace_depth = 1
            elif ch == "{" and brace_depth > 0:
                brace_depth += 1
            elif ch == "}" and brace_depth > 0:
                brace_depth -= 1
            elif ch == "}" and paren_depth == bracket_depth == brace_depth == 0:
                break
            elif ch == ";" and paren_depth == bracket_depth == brace_depth == 0:
                signature = normalize_space(text[signature_start:cursor + 1])
                if not legacy._looks_like_function_declaration(signature):
                    name = _extract_member_name(signature)
                    if name is not None:
                        declarations.append(("constant", name, signature, signature_start))
                break
            cursor += 1

        if function_body:
            continue
    return declarations


def _append_global_aliases_and_constants(acs_root: Path, symbols: list[FSymbolRecord]) -> None:
    legacy = _legacy_parser()
    src_root = acs_root / "src"
    existing = {
        (item.kind, item.qualified_name, normalize_space(item.signature))
        for item in symbols
        if item.parent_id is None
    }
    alias_pattern = re.compile(
        r"(?m)^[ \t]*(?:template[ \t]*<[^;{}]+>[ \t\r\n]*)?"
        r"using[ \t]+([A-Za-z_]\w*)[ \t]*=[ \t]*([^;]+);"
    )
    typedef_pattern = re.compile(
        r"(?m)^[ \t]*typedef[ \t]+([^;]+?)[ \t]+([A-Za-z_]\w*)[ \t]*;"
    )
    for path in sorted(
        candidate
        for candidate in src_root.rglob("*")
        if candidate.is_file() and candidate.suffix.lower() in {".h", ".hh", ".hpp", ".inl"}
    ):
        text = path.read_text(encoding="utf-8", errors="replace")
        masked = legacy.mask_cpp_non_code(text)
        ranges = _class_ranges(masked)
        module = path.relative_to(src_root).parts[0].lower()
        source_path = path.relative_to(acs_root).as_posix()
        declarations: list[tuple[str, str, str, int]] = []
        for match in alias_pattern.finditer(masked):
            if not _inside_ranges(match.start(), ranges) and legacy.is_namespace_scope(text, match.start()):
                declarations.append(("alias", match.group(1), normalize_space(match.group(0)), match.start(1)))
        for match in typedef_pattern.finditer(masked):
            if not _inside_ranges(match.start(), ranges) and legacy.is_namespace_scope(text, match.start()):
                declarations.append(("alias", match.group(2), normalize_space(match.group(0)), match.start(2)))
        declarations.extend(_namespace_constant_declarations(text, masked, ranges, legacy))

        for kind, name, signature, position in declarations:
            namespace = legacy.namespace_hint(text, position)
            qualified_name = _qualified_name(namespace, name)
            semantic_identity = (kind, qualified_name, signature)
            if semantic_identity in existing:
                continue
            symbol_id = stable_id(kind, namespace, name, signature)
            symbol = FSymbolRecord(
                id=symbol_id,
                module=module,
                kind=kind,
                name=name,
                qualified_name=qualified_name,
                signature=signature,
                description=_default_description(name, module, kind),
                access="public",
                source_path=source_path,
                source_line=legacy.line_number(text, position),
                declaration_paths=[source_path],
            )
            symbol.route = _root_route(symbol)
            symbols.append(symbol)
            existing.add(semantic_identity)


def _validate_symbols(symbols: list[FSymbolRecord]) -> None:
    ids: dict[str, FSymbolRecord] = {}
    routes: dict[str, str] = {}
    for symbol in symbols:
        if symbol.id in ids:
            previous_symbol = ids[symbol.id]
            raise ValueError(
                "symbol ID が重複しています: "
                f"{symbol.id} / {previous_symbol.qualified_name} [{previous_symbol.signature}] / "
                f"{symbol.qualified_name} [{symbol.signature}]"
            )
        ids[symbol.id] = symbol
        route_key = symbol.route.casefold()
        previous = routes.get(route_key)
        if previous:
            raise ValueError(f"大文字小文字を区別しない経路が衝突しています: {previous} / {symbol.route}")
        routes[route_key] = symbol.route
    for symbol in symbols:
        if symbol.parent_id and symbol.parent_id not in ids:
            raise ValueError(f"親 symbol がありません: {symbol.qualified_name}")


def _load_json_files(directory: Path) -> list[tuple[Path, dict[str, object]]]:
    result: list[tuple[Path, dict[str, object]]] = []
    if not directory.exists():
        return result
    for path in sorted(directory.rglob("*.json")):
        result.append((path, json.loads(path.read_text(encoding="utf-8"))))
    return result


FEATURE_LINK_KINDS = frozenset({
    "class",
    "struct",
    "union",
    "enum",
    "enum class",
    "alias",
    "function",
    "method",
    "member-variable",
    "constant",
    "enum-value",
    "macro",
})
FEATURE_OWNER_KINDS = frozenset({"class", "struct", "union", "enum", "enum class"})


def _normalized_source_path(value: str) -> str:
    parts = [part for part in value.replace("\\", "/").strip().lstrip("/").split("/") if part not in {"", "."}]
    if ".." in parts:
        return ""
    normalized = "/".join(parts)
    if not normalized:
        return ""
    return normalized if normalized.startswith("src/") else f"src/{normalized}"


def _feature_header_paths(value: object) -> list[str]:
    text = BR_PATTERN.sub(",", html.unescape(str(value or "")))
    result: list[str] = []
    previous_directory = ""
    for raw_part in re.split(r"\s+/\s+|[,;|\r\n]+", text):
        part = strip_rich_text(raw_part).replace("\\", "/").strip().strip("<>")
        if not part:
            continue
        if "/" not in part and previous_directory:
            part = f"{previous_directory}/{part}"
        normalized = _normalized_source_path(part)
        if not normalized:
            continue
        previous_directory = normalized.removeprefix("src/").rsplit("/", 1)[0]
        if normalized.casefold() not in {item.casefold() for item in result}:
            result.append(normalized)
    return result


def _symbol_declaration_paths(symbol: FSymbolRecord) -> set[str]:
    paths = symbol.declaration_paths or [symbol.source_path]
    return {
        normalized.casefold()
        for path in paths
        if (normalized := _normalized_source_path(path))
    }


def _symbol_name_spans(signature: str, name: str) -> list[tuple[int, int]]:
    if not name:
        return []
    if re.fullmatch(r"[A-Za-z_]\w*", name):
        pattern = rf"(?<![A-Za-z0-9_]){re.escape(name)}(?![A-Za-z0-9_])"
        return [(match.start(), match.end()) for match in re.finditer(pattern, signature)]

    flexible = r"\s+".join(re.escape(part) for part in normalize_space(name).split(" "))
    spans = [(match.start(), match.end()) for match in re.finditer(flexible, signature)]
    if spans or not name.startswith("operator") or "operator" not in signature:
        return spans

    suffix = name.removeprefix("operator").strip()
    if not suffix or re.search(r"[A-Za-z0-9_]", suffix):
        return []
    abbreviated = re.compile(rf"[/、]\s*({re.escape(suffix)})")
    return [(match.start(1), match.end(1)) for match in abbreviated.finditer(signature)]


def _feature_api_symbols(title: str, header_symbols: list[FSymbolRecord]) -> list[FSymbolRecord]:
    grouped: dict[str, list[FSymbolRecord]] = defaultdict(list)
    first_position: dict[str, int] = {}
    for symbol in header_symbols:
        spans = _symbol_name_spans(title, symbol.name)
        if not spans:
            continue
        grouped[symbol.name].append(symbol)
        first_position[symbol.name] = min(first_position.get(symbol.name, spans[0][0]), spans[0][0])

    result: list[FSymbolRecord] = []
    for name in sorted(grouped, key=lambda item: (first_position[item], item.casefold())):
        candidates = list({symbol.id: symbol for symbol in grouped[name]}.values())
        typed = [symbol for symbol in candidates if symbol.kind in FEATURE_OWNER_KINDS]
        if len(typed) > 1 and re.search(rf"(?<![A-Za-z0-9_]){re.escape(name)}\s*<", title):
            primary_templates = [
                symbol
                for symbol in typed
                if not _template_specialization(symbol.signature, symbol.name)
            ]
            if len(primary_templates) == 1:
                typed = primary_templates
        roots = [symbol for symbol in candidates if symbol.parent_id is None]
        selected = typed or roots or candidates
        result.extend(sorted(
            selected,
            key=lambda item: (item.source_line, item.qualified_name.casefold(), item.signature, item.id),
        ))
    return result


def _descendant_ids(owner: FSymbolRecord | None, symbols: list[FSymbolRecord]) -> set[str]:
    if owner is None:
        return set()
    children: dict[str, list[str]] = defaultdict(list)
    for symbol in symbols:
        if symbol.parent_id:
            children[symbol.parent_id].append(symbol.id)
    result = {owner.id}
    pending = [owner.id]
    while pending:
        parent_id = pending.pop()
        for child_id in children.get(parent_id, []):
            if child_id not in result:
                result.add(child_id)
                pending.append(child_id)
    return result


def _signature_identity(value: str) -> str:
    return normalize_space(value).removesuffix(";").strip()


def _feature_member_targets(
    signature: str,
    candidates: list[FSymbolRecord],
    owner_ids: set[str],
) -> tuple[list[str], str]:
    eligible = [symbol for symbol in candidates if symbol.kind in FEATURE_LINK_KINDS]
    exact = [
        symbol
        for symbol in eligible
        if _signature_identity(symbol.signature) == _signature_identity(signature)
    ]
    exact_by_name: dict[str, list[FSymbolRecord]] = defaultdict(list)
    for symbol in exact:
        exact_by_name[symbol.name].append(symbol)

    groups: list[tuple[int, str, list[FSymbolRecord]]] = []
    for name in sorted({symbol.name for symbol in eligible}, key=lambda item: (-len(item), item.casefold())):
        spans = _symbol_name_spans(signature, name)
        if not spans:
            continue
        named = [symbol for symbol in eligible if symbol.name == name]
        owned = [symbol for symbol in named if symbol.id in owner_ids]
        selected = owned or named
        exact_named = exact_by_name.get(name, [])
        exact_owned = [symbol for symbol in exact_named if symbol.id in owner_ids]
        if exact_owned:
            selected = exact_owned
        elif exact_named:
            selected = exact_named
        selected = sorted(
            {symbol.id: symbol for symbol in selected}.values(),
            key=lambda item: (item.source_line, item.qualified_name.casefold(), item.signature, item.id),
        )
        groups.append((spans[0][0], name, selected))

    if not groups and exact:
        for name, records in exact_by_name.items():
            groups.append((0, name, records))
    groups.sort(key=lambda item: (item[0], item[1].casefold()))

    target_ids: list[str] = []
    has_overload = False
    for _, _, records in groups:
        has_overload = has_overload or len(records) > 1
        for symbol in records:
            if symbol.id not in target_ids:
                target_ids.append(symbol.id)
    if not target_ids:
        return [], "unresolved"
    return target_ids, "overload" if has_overload else "resolved"


def _feature_members(
    feature_id: str,
    value: object,
    candidates: list[FSymbolRecord],
    owner_ids: set[str],
) -> list[FFeatureMemberRecord]:
    if not isinstance(value, list):
        return []
    result: list[FFeatureMemberRecord] = []
    for source_index, raw_member in enumerate(value):
        if not isinstance(raw_member, dict):
            continue
        signature = strip_rich_text(raw_member.get("sig", ""))
        target_ids, resolution = _feature_member_targets(signature, candidates, owner_ids)
        result.append(FFeatureMemberRecord(
            id=stable_id(feature_id, source_index, signature),
            signature=signature,
            description=strip_rich_text(raw_member.get("desc", "")),
            returns=strip_rich_text(raw_member.get("ret", "")),
            usage=strip_rich_text(raw_member.get("when", "")),
            sample=str(raw_member.get("sample", "") or ""),
            note=strip_rich_text(raw_member.get("note", "")),
            source_index=source_index,
            target_ids=target_ids,
            resolution=resolution,
        ))
    return result


def _feature_catalog(source_root: Path, symbols: list[FSymbolRecord]) -> list[FFeatureRecord]:
    result: list[FFeatureRecord] = []
    for path, data in _load_json_files(source_root / "features"):
        module_data = data.get("module", {}) if isinstance(data.get("module"), dict) else {}
        feature = data.get("feature", {}) if isinstance(data.get("feature"), dict) else {}
        module = slug(str(module_data.get("id", path.parent.name)))
        title = strip_rich_text(feature.get("name", path.stem))
        raw_header = feature.get("header", "")
        header = strip_rich_text(raw_header).replace("\\", "/").lstrip("/")
        headers = {item.casefold() for item in _feature_header_paths(raw_header)}
        header_symbols = [
            symbol
            for symbol in symbols
            if headers.intersection(_symbol_declaration_paths(symbol))
        ]
        feature_symbols = _feature_api_symbols(title, header_symbols)
        feature_owner_types = [
            symbol for symbol in feature_symbols if symbol.kind in FEATURE_OWNER_KINDS
        ]
        owner = (
            feature_owner_types[0]
            if len(feature_owner_types) == 1
            else feature_symbols[0] if len(feature_symbols) == 1 else None
        )
        owner_ids = _descendant_ids(owner, symbols)
        candidates = list({
            symbol.id: symbol
            for symbol in [*header_symbols, *(symbol for symbol in symbols if symbol.id in owner_ids)]
        }.values())
        feature_id = stable_id(module, title, path.relative_to(source_root).as_posix())
        route = f"features/{module}/{slug(title)}-{feature_id[:8]}.html"
        result.append(FFeatureRecord(
            id=feature_id,
            module=module,
            title=title,
            kind=strip_rich_text(feature.get("kind", "機能")) or "機能",
            header=header,
            summary=japanese_text(feature.get("summary", ""), f"ACS の {title} 機能です。"),
            usage=sanitize_acs_prose(feature.get("when", "")),
            sample=str(feature.get("sample", "") or ""),
            source_file=path.relative_to(source_root).as_posix(),
            route=route,
            note=strip_rich_text(feature.get("note", "")),
            symbol_id=feature_symbols[0].id if len(feature_symbols) == 1 else None,
            symbol_ids=[symbol.id for symbol in feature_symbols],
            members=_feature_members(feature_id, feature.get("members", []), candidates, owner_ids),
        ))
    return sorted(result, key=lambda item: (item.module.casefold(), item.title.casefold(), item.id))


def _glossary_catalog(source_root: Path) -> list[FGlossaryRecord]:
    merged: dict[str, FGlossaryRecord] = {}
    for path, data in _load_json_files(source_root / "glossary"):
        term = strip_rich_text(data.get("term", path.stem))
        definition = strip_rich_text(data.get("definition", ""))
        record_id = stable_id("glossary", term)
        source_file = path.relative_to(source_root).as_posix()
        normalized_term = unicodedata.normalize("NFKC", term).casefold()
        previous = merged.get(normalized_term)
        if previous is not None:
            raise ValueError(
                "NFKCとcasefoldで同一になる用語が重複しています: "
                f"{previous.source_file} / {source_file}"
            )
        merged[normalized_term] = FGlossaryRecord(
            id=record_id,
            term=term,
            definition=definition,
            source_file=source_file,
        )
    return sorted(merged.values(), key=lambda item: (item.term.casefold(), item.id))


def _guide_catalog(source_root: Path) -> list[FGuideRecord]:
    result: list[FGuideRecord] = []
    for path, data in _load_json_files(source_root / "guides"):
        title = strip_rich_text(data.get("title", path.stem))
        guide_id = stable_id("guide", title, path.name)
        blocks = data.get("blocks", [])
        result.append(FGuideRecord(
            id=guide_id,
            title=title,
            blocks=list(blocks) if isinstance(blocks, list) else [],
            source_file=path.relative_to(source_root).as_posix(),
            route=f"guides/{slug(title)}-{guide_id[:8]}.html",
        ))
    return sorted(result, key=lambda item: item.source_file)


def _troubleshooting_catalog(source_root: Path) -> list[FTroubleshootingRecord]:
    result: list[FTroubleshootingRecord] = []
    for path, data in _load_json_files(source_root / "troubleshooting"):
        title = strip_rich_text(data.get("title", path.stem))
        record_id = stable_id("troubleshooting", title, path.name)
        item = data.get("item", {})
        result.append(FTroubleshootingRecord(
            id=record_id,
            title=title,
            item=dict(item) if isinstance(item, dict) else {},
            source_file=path.relative_to(source_root).as_posix(),
            route=f"troubleshooting/{slug(title)}-{record_id[:8]}.html",
        ))
    return sorted(result, key=lambda item: item.source_file)


def _validate_feature_member_links(features: list[FFeatureRecord]) -> None:
    """正本に列挙した各API項目が個別symbolへ解決することを保証する。"""
    unresolved = [
        (
            feature.source_file,
            member.source_index,
            member.signature,
        )
        for feature in features
        for member in feature.members
        if not member.target_ids
    ]
    if not unresolved:
        return
    preview = "\n".join(
        f"{source_file} /feature/members/{source_index}/sig: {signature}"
        for source_file, source_index, signature in unresolved[:80]
    )
    suffix = f"\nほか {len(unresolved) - 80} 件" if len(unresolved) > 80 else ""
    raise ValueError(f"個別APIページへ解決できないfeature memberがあります。\n{preview}{suffix}")


def build_catalog(acs_root: Path, source_root: Path) -> FReferenceCatalog:
    symbols = _symbol_catalog(acs_root)
    features = _feature_catalog(source_root, symbols)
    _validate_feature_member_links(features)
    return FReferenceCatalog(
        symbols=symbols,
        features=features,
        glossary=_glossary_catalog(source_root),
        guides=_guide_catalog(source_root),
        troubleshooting=_troubleshooting_catalog(source_root),
    )
