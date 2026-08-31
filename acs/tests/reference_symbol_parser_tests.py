# SPDX-License-Identifier: MIT

from __future__ import annotations

import re
import sys
import unittest
from collections import Counter
from pathlib import Path


ACS_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_ROOT = ACS_ROOT / "scripts"
FIXTURE_ROOT = ACS_ROOT / "tests" / "fixtures" / "reference_symbol_parser"
FIXTURE_HEADER = FIXTURE_ROOT / "src" / "parser" / "SymbolParserFixture.h"

COMPONENT_FIRST_APIS = {
    "ACameraComponent3D": "TrySetAuthoredState",
    "AFire2DComponent": "SetSize",
    "AFollow2DComponent": "target",
    "ALight2DComponent": "m_Radius",
    "AMeshComponent3D": "AMeshComponent3D",
    "APhysicsBody2D": "APhysicsBody2D",
    "APolygonRenderer2D": "kMaxVerts",
    "APrefabLink3DComponent": "SourcePath",
    "APrefabNodeIdentity3DComponent": "SourceNodeId",
    "APrimitiveRenderer2D": "EShape",
    "ARigidBody2D": "ARigidBody2D",
    "AShadowCaster2DComponent": "m_RadiusScale",
    "ASprite2DComponent": "ASprite2DComponent",
    "ASprite3DComponent": "TexturePath",
    "ASpriteAnimComponent": "ASpriteAnimComponent",
    "AStencilClip2DComponent": "kMaxTris",
    "ATilemapComponent": "ATilemapComponent",
    "ATrail2DComponent": "SetColor",
    "ATriggerComponent": "kAllLayers",
    "AWater2DComponent": "kMaxVerts",
    "AWaterSurface3DComponent": "shallowColor",
}

REQUIRED_FEATURE_MACROS = {
    "ACS_ASSERT",
    "ACS_ERR",
    "ACS_ERR_OS",
    "ACS_GAME_FIELD",
    "ACS_LOG",
    "ACS_SAFE_RELEASE",
    "ACS_THREAD_AFFINITY_CHECK",
    "ACS_THREAD_AFFINITY_FIELD",
    "ACS_THREAD_AFFINITY_RESET",
    "ACS_TRY",
    "ACS_TRY_ASSIGN",
    "EXPECT_EQ",
    "EXPECT_FALSE",
    "EXPECT_NE",
    "EXPECT_NEAR",
    "EXPECT_TRUE",
}

sys.path.insert(0, str(SCRIPTS_ROOT))

import generate_reference  # noqa: E402
from reference_site import catalog  # noqa: E402


class ReferenceSymbolParserTests(unittest.TestCase):
    def _parse_fixture(self):
        return generate_reference.parse_header(FIXTURE_HEADER, FIXTURE_ROOT / "src")

    def _catalog_fixture(self):
        return catalog._symbol_catalog(FIXTURE_ROOT)

    def test_prose_sanitizer_preserves_cpp_tokens_and_acs_backends(self) -> None:
        self.assertEqual("TArray<T>", catalog.strip_rich_text("<code>TArray&lt;T&gt;</code>"))
        self.assertEqual("TArray<T>", catalog.strip_rich_text("TArray<T>"))
        self.assertEqual(
            "TResult<TSharedPtr<FAsset>>",
            catalog.strip_rich_text("TResult<TSharedPtr<FAsset>>"),
        )
        self.assertEqual("operator<=>", catalog.strip_rich_text("operator<=>"))
        self.assertEqual("FVec3::UnitY()", catalog.sanitize_acs_prose("FVec3::UnitY()"))
        self.assertEqual(
            "Diligent と OpenXR のACS接続を初期化する。",
            catalog.sanitize_acs_prose("Diligent と OpenXR のACS接続を初期化する。"),
        )
        self.assertEqual("", catalog.sanitize_acs_prose("Unity と同様の画面です。"))
        self.assertEqual("", catalog.sanitize_acs_prose("Phase 21b の実装です。"))
        self.assertEqual("", catalog.sanitize_acs_prose("GDC の講演を引用した方式です。"))

    def test_parser_preserves_declarations_without_function_bodies(self) -> None:
        info = self._parse_fixture()
        original_names = {"TBox", "FSlot", "FInside", "FAfter", "ClampValue", "ACS_REFERENCE_EXPECT"}
        actual = [
            (decl.kind, decl.name, decl.namespace, decl.line, decl.signature)
            for decl in info.declarations
            if decl.name in original_names
        ]
        expected = [
            ("class", "TBox", "acs::reference_fixture", 18, "template<class T> class TBox;"),
            ("struct", "FSlot", "acs::reference_fixture::TBox", 35, "struct FSlot;"),
            ("struct", "FInside", "acs::reference_fixture::detail", 80, "struct FInside;"),
            ("struct", "FAfter", "acs::reference_fixture", 85, "struct FAfter;"),
            (
                "function",
                "ClampValue",
                "acs::reference_fixture",
                89,
                "inline int ClampValue(int value, int minimum, int maximum) noexcept;",
            ),
            (
                "macro",
                "ACS_REFERENCE_EXPECT",
                "",
                107,
                "#define ACS_REFERENCE_EXPECT(expr) \\ do \\ { \\ Check(expr); \\ } while (false)",
            ),
        ]
        self.assertEqual(expected, actual)

        box = next(decl for decl in info.declarations if decl.name == "TBox")
        self.assertEqual(
            [
                ("public", "using FValue = T;", 21),
                ("public", "static constexpr int kCallLimit = MakeLimit();", 22),
                ("public", "static constexpr int kLambdaLimit = [] { return 64; }();", 23),
                ("public", "void (*OnValue)(T&) = nullptr;", 24),
            ],
            [(member.access, member.signature, member.line) for member in box.members],
        )
        self.assertEqual(
            [
                ("public", "TBox& operator=(const TBox&) = delete;", 26),
                ("public", "void Set(T value);", 27),
                ("public", "void Set(const T& value);", 28),
                ("public", "int operator()(const T& value) const noexcept;", 29),
                ("public", "class FForward* Forward() noexcept;", 30),
                ("public", "const char* Title() const noexcept;", 31),
                ("public", "void Bind(class FForward& value) noexcept;", 32),
            ],
            [(member.access, member.signature, member.line) for member in box.functions],
        )
        slot = next(decl for decl in info.declarations if decl.name == "FSlot")
        self.assertEqual("private", slot.access)

    def test_parser_handles_shift_anonymous_aggregate_and_qualified_definition(self) -> None:
        info = self._parse_fixture()
        by_name = {decl.name: decl for decl in info.declarations}

        shift = by_name["EShiftFlags"]
        self.assertEqual(["ShiftNone", "ShiftRead", "ShiftWrite"], shift.values)
        self.assertEqual(
            ["ShiftNone = 0u", "ShiftRead = 1u << 0", "ShiftWrite = 1u << 1"],
            [value.signature for value in shift.value_infos],
        )
        self.assertEqual([12, 13, 14], [value.line for value in shift.value_infos])

        function_names = {decl.name for decl in info.declarations if decl.kind == "function"}
        self.assertIn("ForEachValue", function_names)
        self.assertNotIn("Build", function_names)
        self.assertNotIn("submit", function_names)
        self.assertNotIn("FForward", {decl.name for decl in info.declarations if decl.namespace.endswith("TBox")})
        self.assertNotIn("NOMINMAX", {decl.name for decl in info.declarations})

    def test_parser_scans_inl_sources(self) -> None:
        headers, declarations = generate_reference.parse_all_headers(FIXTURE_ROOT / "src")
        self.assertIn("SymbolParserInline.inl", {header.path.name for header in headers})
        inline_value = next(declaration for declaration in declarations if declaration.name == "InlineValue")
        self.assertEqual("function", inline_value.kind)
        self.assertEqual("acs::reference_fixture", inline_value.namespace)
        self.assertEqual("inline int InlineValue(int value) noexcept;", inline_value.signature)

    def test_parser_separates_standalone_metadata_macros_from_declarations(self) -> None:
        body = """
/** 実行入口の説明。 */
ACS_FUNCTION(BlueprintCallable, Meta(")"))
void Invoke() noexcept;
ACS_ASSET_TYPE("FAnnotatedAsset")
FAnnotatedAsset() noexcept = default;
ACS_RTTI(FAnnotatedAsset, FBaseAsset)
static constexpr int kTypeVersion = 3;
"""

        members, functions = generate_reference.parse_class_body(body, "public")

        self.assertEqual(
            ["FAnnotatedAsset() noexcept = default;", "void Invoke() noexcept;"],
            sorted(item.signature for item in functions),
        )
        self.assertEqual(["static constexpr int kTypeVersion = 3;"], [item.signature for item in members])
        invoke = next(item for item in functions if item.signature.startswith("void Invoke"))
        self.assertEqual("実行入口の説明。", invoke.doc)
        self.assertTrue(
            all("ACS_" not in item.signature for item in [*members, *functions])
        )
        self.assertEqual(
            -1,
            generate_reference._standalone_class_metadata_macro_end(
                "ACS_FUNCTION(BlueprintCallable) void SameLine() noexcept;",
                0,
            ),
        )

    def test_production_component_and_macro_feature_members_resolve_to_declarations(self) -> None:
        macro_pattern = re.compile(
            r"^[ \t]*ACS_GAME_COMPONENT_KIND\(([^)]+)\)[ \t]*$",
            flags=re.MULTILINE,
        )
        occurrences: dict[str, tuple[Path, int]] = {}
        for path in sorted((ACS_ROOT / "src" / "gameframework").rglob("*.h")):
            source = path.read_text(encoding="utf-8", errors="replace")
            for match in macro_pattern.finditer(source):
                occurrences[match.group(1)] = (
                    path,
                    source.count("\n", 0, match.start()) + 1,
                )

        self.assertEqual(COMPONENT_FIRST_APIS.keys(), occurrences.keys())
        reference_catalog = catalog.build_catalog(
            ACS_ROOT,
            ACS_ROOT / "docs" / "reference" / "source",
        )
        symbols = reference_catalog.symbols
        symbol_by_id = {symbol.id: symbol for symbol in symbols}
        resolved_targets = []
        declaration_categories = set()

        for owner_name, expected_api_name in COMPONENT_FIRST_APIS.items():
            path, macro_line = occurrences[owner_name]
            source_path = path.relative_to(ACS_ROOT).as_posix().casefold()
            owners = [
                symbol
                for symbol in symbols
                if symbol.name == owner_name
                and symbol.kind in {"class", "struct"}
                and source_path in catalog._symbol_declaration_paths(symbol)
            ]
            self.assertEqual(1, len(owners), owner_name)
            owner = owners[0]
            direct_children = [symbol for symbol in symbols if symbol.parent_id == owner.id]
            first_line = min(symbol.source_line for symbol in direct_children if symbol.source_line > macro_line)
            first_api = [symbol for symbol in direct_children if symbol.source_line == first_line]
            self.assertEqual(1, len(first_api), owner_name)
            target = first_api[0]
            self.assertEqual(expected_api_name, target.name, owner_name)
            self.assertNotIn(
                "ACS_GAME_COMPONENT_KIND",
                {symbol_by_id[symbol_id].name for symbol_id in catalog._descendant_ids(owner, symbols)},
                owner_name,
            )

            header_symbols = [
                symbol
                for symbol in symbols
                if source_path in catalog._symbol_declaration_paths(symbol)
            ]
            target_ids, resolution = catalog._feature_member_targets(
                target.signature,
                header_symbols,
                catalog._descendant_ids(owner, symbols),
            )
            self.assertEqual("resolved", resolution, owner_name)
            self.assertEqual([target.id], target_ids, owner_name)
            resolved_targets.append(target.id)

            if target.kind == "method":
                declaration_categories.add("constructor" if target.name == owner_name else "function")
            elif target.kind == "member-variable":
                declaration_categories.add("member")
            elif target.kind in {"enum", "enum class"}:
                declaration_categories.add("enum")
            else:
                declaration_categories.add(target.kind)

        self.assertEqual(len(COMPONENT_FIRST_APIS), len(resolved_targets))
        self.assertEqual(len(COMPONENT_FIRST_APIS), len(set(resolved_targets)))
        self.assertEqual(
            {"constant", "constructor", "enum", "function", "member"},
            declaration_categories,
        )

        macro_members = []
        linked_macro_names = set()
        for feature in reference_catalog.features:
            for member in feature.members:
                macro_targets = [
                    symbol_by_id[symbol_id]
                    for symbol_id in member.target_ids
                    if symbol_by_id[symbol_id].kind == "macro"
                ]
                if not macro_targets:
                    continue
                self.assertEqual("resolved", member.resolution, f"{feature.title}: {member.signature}")
                macro_members.append((feature, member, macro_targets))
                for target in macro_targets:
                    linked_macro_names.add(target.name)
                    source_line = (ACS_ROOT / target.source_path).read_text(
                        encoding="utf-8",
                        errors="replace",
                    ).splitlines()[target.source_line - 1]
                    self.assertRegex(
                        source_line,
                        rf"^\s*#\s*define\s+{re.escape(target.name)}\b",
                        target.qualified_name,
                    )

        # 指定37項目に加え、同じfeatureのACS_GAME_REFLECTも正規macroとして検証する。
        requested_macro_members = [
            item
            for item in macro_members
            if item[1].signature != "ACS_GAME_REFLECT(T, fields...)"
        ]
        self.assertEqual(37, len(requested_macro_members))
        self.assertEqual(38, len(macro_members))
        self.assertTrue(REQUIRED_FEATURE_MACROS.issubset(linked_macro_names))

    def test_template_specialization_does_not_use_a_base_type_argument(self) -> None:
        self.assertEqual(
            "",
            catalog._template_specialization(
                "template<u32 N, u32... Indices> struct TMakeIndexSeq : "
                "TMakeIndexSeq<N - 1, N - 1, Indices...> {};",
                "TMakeIndexSeq",
            ),
        )
        self.assertEqual(
            "<0, Indices...>",
            catalog._template_specialization(
                "template<u32... Indices> struct TMakeIndexSeq<0, Indices...> {};",
                "TMakeIndexSeq",
            ),
        )

    def test_catalog_creates_one_page_per_symbol_and_member(self) -> None:
        symbols = self._catalog_fixture()
        self.assertEqual(66, len(symbols))
        self.assertEqual(
            Counter({
                "member-variable": 20,
                "method": 14,
                "struct": 11,
                "class": 7,
                "enum-value": 3,
                "function": 3,
                "constant": 3,
                "macro": 2,
                "enum": 1,
                "union": 1,
                "alias": 1,
            }),
            Counter(symbol.kind for symbol in symbols),
        )

        by_name = {symbol.name: symbol for symbol in symbols if symbol.name != "Set"}
        overloads = [symbol for symbol in symbols if symbol.name == "Set"]
        self.assertEqual(2, len(overloads))
        self.assertEqual(2, len({symbol.signature for symbol in overloads}))
        self.assertEqual(2, len({symbol.id for symbol in overloads}))
        self.assertEqual(2, len({symbol.route for symbol in overloads}))
        self.assertIn("= delete;", by_name["operator="].signature)
        self.assertEqual("method", by_name["operator()"].kind)
        self.assertEqual("alias", by_name["FValue"].kind)
        self.assertEqual("constant", by_name["kCallLimit"].kind)
        self.assertEqual("constant", by_name["kLambdaLimit"].kind)
        self.assertEqual("constant", by_name["IsReferenceValueV"].kind)
        self.assertEqual("member-variable", by_name["OnValue"].kind)
        self.assertEqual(by_name["TBox"].id, by_name["FSlot"].parent_id)
        self.assertEqual("private", by_name["FSlot"].access)
        self.assertEqual("acs::reference_fixture::FAfter", by_name["FAfter"].qualified_name)
        self.assertEqual(1, sum(symbol.qualified_name == "acs::reference_fixture::FForward" for symbol in symbols))
        self.assertEqual("member-variable", by_name["values"].kind)
        self.assertEqual("int values[2][3]{};", by_name["values"].signature)
        self.assertEqual({"first", "second"}, {name for name in by_name if name in {"first", "second"}})
        self.assertEqual(by_name["first"].id, by_name["x"].parent_id)
        self.assertEqual(by_name["second"].id, by_name["y"].parent_id)
        self.assertEqual("acs::reference_fixture::FAggregate::first.x", by_name["x"].qualified_name)
        self.assertEqual("acs::reference_fixture::FAggregate::second.y", by_name["y"].qualified_name)
        self.assertIn(Path(by_name["first"].route).stem, Path(by_name["x"].route).parts)
        self.assertEqual(by_name["FTaggedValue"].id, by_name["v"].parent_id)
        for name in ("b", "num", "str", "handle"):
            self.assertEqual(by_name["v"].id, by_name[name].parent_id)
            self.assertEqual(
                f"acs::reference_fixture::FTaggedValue::v.{name}",
                by_name[name].qualified_name,
            )
            self.assertIn(Path(by_name["v"].route).stem, Path(by_name[name].route).parts)
        self.assertEqual("class FForward* Forward() noexcept;", by_name["Forward"].signature)
        self.assertEqual("const char* Title() const noexcept;", by_name["Title"].signature)
        for name in ("Forward", "Title", "Bind"):
            for fragment in ("{", "}", "return"):
                self.assertNotIn(fragment, by_name[name].signature)
        self.assertEqual(
            {"ShiftNone", "ShiftRead", "ShiftWrite"},
            {symbol.name for symbol in symbols if symbol.kind == "enum-value"},
        )

        clamp_signature = by_name["ClampValue"].signature
        for fragment in ("{", "return", "SelectValue"):
            self.assertNotIn(fragment, clamp_signature)

        conversion = next(symbol for symbol in symbols if symbol.name == "operator const T&")
        self.assertEqual("method", conversion.kind)
        self.assertEqual(
            "acs::reference_fixture::FConversion::operator const T&",
            conversion.qualified_name,
        )
        self.assertNotIn("FLocalContext", {symbol.name for symbol in symbols})
        self.assertNotIn("FLocalLeak", {symbol.name for symbol in symbols})
        self.assertNotIn("FFriendOnly", {symbol.name for symbol in symbols})

        nested = [symbol for symbol in symbols if symbol.name == "FNested"]
        self.assertEqual(1, len(nested))
        self.assertEqual("class", nested[0].kind)
        self.assertEqual(by_name["FOwner"].id, nested[0].parent_id)
        self.assertEqual("private", nested[0].access)

        compact = by_name["compact"]
        self.assertEqual(154, compact.source_line)
        self.assertEqual(
            "union { bool compactFlag; int compactCount; } compact{};",
            compact.signature,
        )
        for name in ("compactFlag", "compactCount"):
            self.assertEqual(compact.id, by_name[name].parent_id)
            self.assertIn(f"compact.{name}", by_name[name].qualified_name)

        storage = by_name["storage"]
        storage_type = next(symbol for symbol in symbols if symbol.kind == "union" and symbol.name == "FStorage")
        stored_value = by_name["storedValue"]
        self.assertEqual(storage_type.id, stored_value.parent_id)
        self.assertNotEqual(storage.id, stored_value.parent_id)
        self.assertEqual("FStorage storage;", storage.signature)

        matrix_constructor = next(
            symbol
            for symbol in symbols
            if symbol.kind == "method" and symbol.name == "FMatrixLike"
        )
        identity_like = by_name["IdentityLike"]
        self.assertEqual("constexpr FMatrixLike() noexcept;", matrix_constructor.signature)
        self.assertEqual("static FMatrixLike IdentityLike() noexcept;", identity_like.signature)
        self.assertEqual(167, matrix_constructor.source_line)
        compact_constructor = next(
            symbol
            for symbol in symbols
            if symbol.kind == "method" and symbol.signature.startswith("FMatrixLike(int value)")
        )
        self.assertEqual("FMatrixLike(int value) noexcept;", compact_constructor.signature)
        self.assertEqual(170, compact_constructor.source_line)
        self.assertEqual(172, identity_like.source_line)

        self.assertEqual(1, sum(symbol.name == "FForward" and symbol.kind == "class" for symbol in symbols))
        self.assertEqual(1, sum(symbol.name == "ACS_REFERENCE_PLATFORM" for symbol in symbols))
        self.assertNotIn("ACS_DETAIL_REFERENCE_INTERNAL", {symbol.name for symbol in symbols})
        self.assertEqual(
            {
                "acs::reference_fixture::TCallable",
                "acs::reference_fixture::TCallable<void(Arguments...)>",
            },
            {symbol.qualified_name for symbol in symbols if symbol.name == "TCallable"},
        )
        self.assertEqual(
            {
                "acs::reference_fixture::TTraits",
                "acs::reference_fixture::TTraits<int>",
                "acs::reference_fixture::TTraits<float>",
            },
            {symbol.qualified_name for symbol in symbols if symbol.name == "TTraits"},
        )

        false_names = {
            "T", "MakeLimit", "SelectValue", "Check", "return", "do", "while",
            "submit", "member", "NOMINMAX", "FLocalContext", "FLocalLeak", "FFriendOnly",
        }
        self.assertTrue(false_names.isdisjoint(symbol.name for symbol in symbols))
        self.assertEqual(len(symbols), len({symbol.id for symbol in symbols}))
        self.assertEqual(len(symbols), len({symbol.route.casefold() for symbol in symbols}))


if __name__ == "__main__":
    unittest.main()
