# SPDX-License-Identifier: MIT

from __future__ import annotations

import json
import sys
import tempfile
import unicodedata
import unittest
from pathlib import Path


ACS_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_ROOT = ACS_ROOT / "scripts"
REFERENCE_SOURCE_ROOT = ACS_ROOT / "docs" / "reference" / "source"

sys.path.insert(0, str(SCRIPTS_ROOT))

from reference_site.catalog import (  # noqa: E402
    _feature_catalog,
    _feature_header_paths,
    _glossary_catalog,
    _validate_feature_member_links,
    strip_rich_text,
)
from reference_site.model import (  # noqa: E402
    FGuideRecord,
    FReferenceCatalog,
    FSymbolRecord,
    FTroubleshootingRecord,
)
from reference_site.renderer import FReferenceRenderer  # noqa: E402


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _symbol(
    symbol_id: str,
    kind: str,
    name: str,
    signature: str,
    source_path: str,
    *,
    parent_id: str | None = None,
    source_line: int = 1,
) -> FSymbolRecord:
    qualified = f"acs::{name}" if parent_id is None else f"acs::FWidget::{name}"
    return FSymbolRecord(
        id=symbol_id,
        module="widget",
        kind=kind,
        name=name,
        qualified_name=qualified,
        signature=signature,
        description=f"{name} の説明です。",
        access="public",
        source_path=source_path,
        source_line=source_line,
        route=f"symbols/widget/{symbol_id}.html",
        parent_id=parent_id,
        declaration_paths=[source_path],
    )


class ReferenceFeatureMemberTests(unittest.TestCase):
    def test_feature_headers_split_spaced_slashes_without_breaking_paths(self) -> None:
        self.assertEqual(
            ["src/foundation/LogSinkHandle.h", "src/foundation/LogSinkSubscription.h"],
            _feature_header_paths(
                "foundation/LogSinkHandle.h / foundation/LogSinkSubscription.h"
            ),
        )
        self.assertEqual(
            ["src/memory/LinearAllocator.h", "src/memory/PoolAllocator.h"],
            _feature_header_paths("memory/LinearAllocator.h / PoolAllocator.h"),
        )

    def _symbols(self) -> list[FSymbolRecord]:
        header = "src/widget/Widget.h"
        return [
            _symbol("owner", "class", "FWidget", "template<class T> class FWidget;", header),
            _symbol("tarray", "class", "TArray", "template<class T> class TArray;", header, source_line=2),
            _symbol("config", "struct", "FConfig", "struct FConfig;", header, source_line=3),
            _symbol("other-owner", "class", "FOther", "class FOther;", header, source_line=4),
            _symbol("reset", "method", "Reset", "FConfig Reset() noexcept;", header, parent_id="owner", source_line=10),
            _symbol("value", "member-variable", "Value", "int Value = 0;", header, parent_id="owner", source_line=11),
            _symbol("set-value", "method", "Set", "void Set(int value);", header, parent_id="owner", source_line=12),
            _symbol("set-ref", "method", "Set", "void Set(const int& value);", header, parent_id="owner", source_line=13),
            _symbol("other-set", "method", "Set", "void Set(float value);", header, parent_id="other-owner", source_line=14),
            _symbol("limit", "constant", "kLimit", "static constexpr int kLimit = 8;", header, parent_id="owner", source_line=15),
            _symbol("mode", "enum class", "EMode", "enum class EMode;", header, source_line=20),
            FSymbolRecord(
                id="fast",
                module="widget",
                kind="enum-value",
                name="Fast",
                qualified_name="acs::EMode::Fast",
                signature="Fast = 1",
                description="高速設定です。",
                access="public",
                source_path=header,
                source_line=21,
                route="symbols/widget/mode/fast.html",
                parent_id="mode",
                declaration_paths=[header],
            ),
            _symbol("foreign-reset", "function", "Reset", "void Reset();", "src/widget/Other.h", source_line=30),
            _symbol("elsewhere", "function", "OnlyElsewhere", "void OnlyElsewhere();", "src/widget/Other.h", source_line=31),
            _symbol("get-thing", "function", "GetThing", "int GetThing();", header, source_line=32),
            _symbol("parse-char", "function", "ParseJson", "void ParseJson(const char* text);", header, source_line=33),
            _symbol("parse-view", "function", "ParseJson", "void ParseJson(FStringView text);", header, source_line=34),
        ]

    def test_members_preserve_source_order_and_render_safe_links(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source_root = Path(temporary) / "source"
            _write_json(
                source_root / "features" / "widget" / "fwidget.json",
                {
                    "schema": 2,
                    "module": {"id": "widget"},
                    "feature": {
                        "name": "FWidget<T>",
                        "kind": "クラス",
                        "header": "widget/Widget.h",
                        "summary": "値を管理する機能です。",
                        "note": "機能全体の補足です。",
                        "members": [
                            {
                                "sig": "FConfig Reset() / int Value",
                                "desc": "値と設定を取得します。",
                                "ret": "現在の設定を返します。",
                                "when": "状態を確認するときに使います。",
                                "sample": "FConfig config = widget.Reset();",
                                "note": "取得した設定は値として保持します。",
                            },
                            {"sig": "void Set(value)", "desc": "値を設定します。"},
                            {"sig": "kLimit / EMode::Fast", "desc": "上限と設定値です。"},
                            {"sig": "void Missing()", "desc": "未解決の宣言です。"},
                        ],
                    },
                },
            )

            features = _feature_catalog(source_root, self._symbols())
            self.assertEqual(1, len(features))
            feature = features[0]
            self.assertEqual("owner", feature.symbol_id)
            self.assertEqual(["owner"], feature.symbol_ids)
            self.assertEqual("", feature.usage)
            self.assertEqual("機能全体の補足です。", feature.note)
            self.assertEqual(
                [
                    "FConfig Reset() / int Value",
                    "void Set(value)",
                    "kLimit / EMode::Fast",
                    "void Missing()",
                ],
                [member.signature for member in feature.members],
            )
            first = feature.members[0]
            self.assertEqual("値と設定を取得します。", first.description)
            self.assertEqual("現在の設定を返します。", first.returns)
            self.assertEqual("状態を確認するときに使います。", first.usage)
            self.assertEqual("FConfig config = widget.Reset();", first.sample)
            self.assertEqual("取得した設定は値として保持します。", first.note)
            self.assertEqual(["config", "reset", "value"], first.target_ids)
            self.assertEqual(["set-value", "set-ref"], feature.members[1].target_ids)
            self.assertEqual("overload", feature.members[1].resolution)
            self.assertEqual(["limit", "mode", "fast"], feature.members[2].target_ids)
            self.assertEqual([], feature.members[3].target_ids)
            self.assertEqual("unresolved", feature.members[3].resolution)
            self.assertNotIn("foreign-reset", first.target_ids)
            with self.assertRaisesRegex(ValueError, "個別APIページへ解決できない"):
                _validate_feature_member_links(features)

            catalog = FReferenceCatalog(
                symbols=self._symbols(),
                features=features,
                glossary=[],
                guides=[FGuideRecord(
                    id="guide",
                    title="検索ガイド",
                    blocks=[{"p": "ガイド固有の確認手順です。"}],
                    source_file="guides/search.json",
                    route="guides/search.html",
                )],
                troubleshooting=[FTroubleshootingRecord(
                    id="trouble",
                    title="検索できない",
                    item={"cause": "索引が古い状態です。", "fix": "索引を再生成します。"},
                    source_file="troubleshooting/search.json",
                    route="troubleshooting/search.html",
                )],
            )
            renderer = FReferenceRenderer(catalog, Path(temporary) / "docs")
            page = renderer.render_feature(feature)
            self.assertNotIn('id="usage"', page)
            self.assertIn('id="note"', page)
            self.assertEqual(4, page.count("API項目 ") // 2)
            self.assertIn("overload候補", page)
            self.assertIn("対応するAPIを特定できない", page)
            for route in (
                "../../symbols/widget/config.html",
                "../../symbols/widget/reset.html",
                "../../symbols/widget/value.html",
                "../../symbols/widget/set-value.html",
                "../../symbols/widget/set-ref.html",
                "../../symbols/widget/limit.html",
                "../../symbols/widget/mode.html",
                "../../symbols/widget/mode/fast.html",
            ):
                self.assertIn(f'href="{route}"', page)
            field_positions = [
                page.index("値と設定を取得します。"),
                page.index("現在の設定を返します。"),
                page.index("状態を確認するときに使います。"),
                page.index("FConfig config = widget.Reset();"),
                page.index("取得した設定は値として保持します。"),
            ]
            self.assertEqual(field_positions, sorted(field_positions))

            search_records = {record["title"]: record for record in renderer.search_index()}
            feature_search = search_records["FWidget<T>"]
            for expected in (
                "値を管理する機能です。",
                "機能全体の補足です。",
                "FConfig Reset() / int Value",
                "値と設定を取得します。",
                "現在の設定を返します。",
                "状態を確認するときに使います。",
                "取得した設定は値として保持します。",
                "FConfig config = widget.Reset();",
            ):
                self.assertIn(expected, feature_search["signature"] + " " + feature_search["text"])
            self.assertIn("ガイド固有の確認手順です。", search_records["検索ガイド"]["text"])
            self.assertIn("索引が古い状態です。", search_records["検索できない"]["text"])
            self.assertIn("索引を再生成します。", search_records["検索できない"]["text"])

    def test_root_links_use_only_declared_headers_and_keep_overloads(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source_root = Path(temporary) / "source"
            feature_base = {
                "schema": 2,
                "module": {"id": "widget"},
            }
            _write_json(
                source_root / "features" / "widget" / "get-thing.json",
                feature_base | {"feature": {
                    "name": "GetThing()",
                    "kind": "関数",
                    "header": "widget/Widget.h",
                    "summary": "値を取得します。",
                }},
            )
            _write_json(
                source_root / "features" / "widget" / "parse-json.json",
                feature_base | {"feature": {
                    "name": "ParseJson",
                    "kind": "関数",
                    "header": "widget/Widget.h",
                    "summary": "JSONを解析します。",
                }},
            )
            _write_json(
                source_root / "features" / "widget" / "tarray.json",
                feature_base | {"feature": {
                    "name": "TArray<T>",
                    "kind": "クラス",
                    "header": "widget/Widget.h",
                    "summary": "要素を保持します。",
                }},
            )
            _write_json(
                source_root / "features" / "widget" / "elsewhere.json",
                feature_base | {"feature": {
                    "name": "OnlyElsewhere()",
                    "kind": "関数",
                    "header": "widget/Missing.h",
                    "summary": "指定headerに宣言がない機能です。",
                }},
            )

            features = {feature.title: feature for feature in _feature_catalog(source_root, self._symbols())}
            self.assertEqual("get-thing", features["GetThing()"].symbol_id)
            self.assertEqual(["get-thing"], features["GetThing()"].symbol_ids)
            self.assertEqual("tarray", features["TArray<T>"].symbol_id)
            self.assertEqual(["tarray"], features["TArray<T>"].symbol_ids)
            self.assertIsNone(features["ParseJson"].symbol_id)
            self.assertEqual(["parse-char", "parse-view"], features["ParseJson"].symbol_ids)
            self.assertIsNone(features["OnlyElsewhere()"].symbol_id)
            self.assertEqual([], features["OnlyElsewhere()"].symbol_ids)

            catalog = FReferenceCatalog(
                symbols=self._symbols(),
                features=list(features.values()),
                glossary=[],
                guides=[],
                troubleshooting=[],
            )
            page = FReferenceRenderer(catalog, Path(temporary) / "docs").render_feature(features["ParseJson"])
            self.assertIn("対応するAPI候補", page)
            self.assertIn("void ParseJson(const char* text);", page)
            self.assertIn("void ParseJson(FStringView text);", page)
            self.assertIn('href="../../symbols/widget/parse-char.html"', page)
            self.assertIn('href="../../symbols/widget/parse-view.html"', page)

    def test_template_feature_prefers_the_primary_template(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source_root = Path(temporary) / "source"
            _write_json(
                source_root / "features" / "widget" / "tchoice.json",
                {
                    "schema": 2,
                    "module": {"id": "widget"},
                    "feature": {
                        "name": "TChoice<T>",
                        "kind": "テンプレート",
                        "header": "widget/Choice.h",
                        "summary": "選択値を保持します。",
                    },
                },
            )
            header = "src/widget/Choice.h"
            symbols = [
                _symbol(
                    "primary",
                    "class",
                    "TChoice",
                    "template<typename T> class TChoice;",
                    header,
                ),
                _symbol(
                    "specialization",
                    "class",
                    "TChoice",
                    "template<> class TChoice<int>;",
                    header,
                    source_line=2,
                ),
            ]
            feature = _feature_catalog(source_root, symbols)[0]
            self.assertEqual("primary", feature.symbol_id)
            self.assertEqual(["primary"], feature.symbol_ids)

    def test_glossary_is_a_one_to_one_catalog_of_source_json(self) -> None:
        records = _glossary_catalog(REFERENCE_SOURCE_ROOT)
        source_files = list((REFERENCE_SOURCE_ROOT / "glossary").glob("*.json"))
        self.assertEqual(669, len(source_files))
        self.assertEqual(669, len(records))
        self.assertEqual(
            669,
            len({unicodedata.normalize("NFKC", record.term).casefold() for record in records}),
        )
        self.assertTrue(all(record.source_file.startswith("glossary/") for record in records))
        self.assertNotIn("glossary/canonical", {record.source_file for record in records})
        by_source = {record.source_file: record for record in records}
        for path in source_files:
            source_file = path.relative_to(REFERENCE_SOURCE_ROOT).as_posix()
            source = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(strip_rich_text(source["term"]), by_source[source_file].term)
            self.assertEqual(strip_rich_text(source["definition"]), by_source[source_file].definition)

    def test_glossary_duplicate_reports_both_source_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source_root = Path(temporary) / "source"
            _write_json(
                source_root / "glossary" / "fullwidth.json",
                {"schema": 2, "term": "ＡＣＳ用語", "definition": "全角表記の用語です。"},
            )
            _write_json(
                source_root / "glossary" / "ascii.json",
                {"schema": 2, "term": "acs用語", "definition": "ASCII表記の用語です。"},
            )
            with self.assertRaises(ValueError) as error:
                _glossary_catalog(source_root)
            message = str(error.exception)
            self.assertIn("glossary/fullwidth.json", message)
            self.assertIn("glossary/ascii.json", message)


if __name__ == "__main__":
    unittest.main()
