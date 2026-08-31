# SPDX-License-Identifier: MIT

from __future__ import annotations

import json
import re
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path


ACS_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_ROOT = ACS_ROOT / "scripts"
FIXTURE_ROOT = ACS_ROOT / "tests" / "fixtures" / "reference_symbol_parser"

sys.path.insert(0, str(SCRIPTS_ROOT))

from generate_reference_site import add_assets, build_manifest, validate_output  # noqa: E402
from reference_site.catalog import build_catalog  # noqa: E402
from reference_site.renderer import FReferenceRenderer  # noqa: E402


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def _png_chunk(chunk_type: bytes, data: bytes) -> bytes:
    checksum = zlib.crc32(chunk_type + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + chunk_type + data + struct.pack(">I", checksum)


def _write_png(path: Path, width: int, height: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    scanlines = b"" if width < 1 or height < 1 else (b"\x00" + (b"\x00" * width)) * height
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", zlib.compress(scanlines))
        + _png_chunk(b"IEND", b"")
    )


class ReferenceSiteTests(unittest.TestCase):
    def _render_fixture(self, temporary_root: Path):
        source_root = temporary_root / "source"
        _write_json(
            source_root / "features" / "parser" / "fconversion.json",
            {
                "schema": 2,
                "module": {
                    "id": "parser",
                    "title": "parser",
                    "description": "ACS の宣言解析機能です。",
                },
                "feature": {
                    "name": "FConversion",
                    "kind": "クラス",
                    "header": "parser/SymbolParserFixture.h",
                    "summary": "世代付きハンドルを扱う宣言を確認します。",
                    "when": "変換演算子の宣言を確認するときに使います。",
                    "sample": "FConversion value;",
                },
            },
        )
        _write_json(
            source_root / "guides" / "01.json",
            {
                "schema": 2,
                "title": "ACS リファレンスの読み方",
                "blocks": [{"p": "機能名から対応する API を開きます。"}],
            },
        )
        _write_json(
            source_root / "glossary" / "generation-handle.json",
            {
                "schema": 2,
                "term": "世代付きハンドル",
                "definition": "再利用後の古い参照を世代番号で検出するハンドルです。",
            },
        )
        _write_json(
            source_root / "troubleshooting" / "001.json",
            {
                "schema": 2,
                "title": "API ページを開けない",
                "item": {
                    "q": "API ページを開けません。",
                    "a": "検索結果から同じ名前の宣言を確認します。",
                    "tags": ["参照"],
                },
            },
        )

        reference_catalog = build_catalog(FIXTURE_ROOT, source_root)
        docs_root = temporary_root / "docs"
        output_root = docs_root / "reference"
        image_root = docs_root / "media" / "captures" / "edited" / "editor"
        _write_png(image_root / "blueprint-editor.png", 37, 23)
        _write_png(image_root / "mobile" / "blueprint-event-flow.png", 19, 11)
        renderer = FReferenceRenderer(reference_catalog, docs_root)
        files = renderer.render_all()
        add_assets(files)
        files["manifest.json"] = build_manifest(reference_catalog, files)
        return reference_catalog, files, output_root, docs_root

    def test_each_feature_and_symbol_has_one_valid_page(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            catalog, files, output_root, docs_root = self._render_fixture(Path(temporary))

            self.assertEqual(len(files), len({route.casefold() for route in files}))
            for symbol in catalog.symbols:
                self.assertIn(symbol.route, files)
                self.assertTrue(symbol.route.endswith(".html"))
            for feature in catalog.features:
                self.assertIn(feature.route, files)
                self.assertIsNotNone(feature.symbol_id)

            expected_html_count = (
                5
                + len({symbol.module for symbol in catalog.symbols} | {feature.module for feature in catalog.features})
                + len(catalog.symbols)
                + len(catalog.features)
                + len(catalog.guides)
                + len(catalog.troubleshooting)
            )
            self.assertEqual(
                expected_html_count,
                sum(route.endswith(".html") for route in files),
            )
            validate_output(files, output_root, docs_root)

    def test_signature_links_tooltips_search_and_mobile_assets(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            catalog, files, _, _ = self._render_fixture(Path(temporary))
            by_name = {}
            for symbol in catalog.symbols:
                by_name.setdefault(symbol.name, []).append(symbol)

            bind = next(
                symbol
                for symbol in by_name["Bind"]
                if symbol.qualified_name.endswith("TBox::Bind")
            )
            bind_page = files[bind.route].decode("utf-8")
            self.assertIn("FForward", bind_page)
            self.assertRegex(bind_page, r'<a href="[^"]+">FForward</a>')

            feature = catalog.features[0]
            feature_page = files[feature.route].decode("utf-8")
            self.assertIn('class="term-trigger"', feature_page)
            self.assertIn('aria-describedby="term-tip-', feature_page)
            term_trigger = re.search(r'<button class="term-trigger"([^>]*)>', feature_page)
            self.assertIsNotNone(term_trigger)
            self.assertNotIn("aria-expanded", term_trigger.group(1))

            expected_navigation_labels = {
                "主要ナビゲーション",
                "モバイル用主要ナビゲーション",
                "リファレンス内の関連ページ",
                "パンくず",
            }
            for route, content in files.items():
                if not route.endswith(".html"):
                    continue
                page = content.decode("utf-8")
                unlabeled_navigation = re.findall(
                    r'<nav\b(?![^>]*\baria-label="[^"]+")[^>]*>',
                    page,
                )
                self.assertEqual([], unlabeled_navigation, route)
                labels = re.findall(r'<nav\b[^>]*\baria-label="([^"]+)"', page)
                self.assertEqual(len(labels), len(set(labels)), route)
            self.assertTrue(
                expected_navigation_labels.issubset(
                    set(re.findall(r'<nav\b[^>]*\baria-label="([^"]+)"', feature_page))
                )
            )
            self.assertIn('aria-label="このページ内の見出し"', feature_page)

            search_page = files["search.html"].decode("utf-8")
            self.assertIn("assets/js/search-index.js", search_page)
            self.assertNotIn("assets/js/search-index.js", bind_page)
            self.assertIn(
                '<form class="site-search" action="search.html" method="get" role="search" '
                'aria-label="ACSリファレンス全体を検索">',
                search_page,
            )
            self.assertIn(
                'role="search" aria-label="機能とAPIの検索結果を絞り込む"',
                search_page,
            )
            self.assertIn('<noscript><nav class="noscript-nav" aria-label="主要ナビゲーション">', search_page)
            self.assertIn("検索結果の絞り込みにはJavaScriptが必要です。", search_page)
            self.assertNotIn('class="right-rail"', search_page)
            search_index = files["assets/js/search-index.js"].decode("utf-8")
            self.assertIn("window.ACS_SEARCH_CONTEXTS=", search_index)
            self.assertIn("window.ACS_SEARCH_INDEX=", search_index)

            css = files["assets/css/reference.css"].decode("utf-8")
            self.assertIn("@media (max-width: 1199px)", css)
            self.assertIn("min-height: 44px", css)
            self.assertIn(".js .nav-toggle", css)
            self.assertIn(".noscript-nav", css)
            self.assertIn(".term-wrap.is-suppressed:hover .term-tooltip", css)
            script = files["assets/js/reference.js"].decode("utf-8")
            self.assertIn("data-mobile-drawer", script)
            self.assertIn("Escape", script)
            self.assertIn('root.classList.add("js")', script)
            self.assertIn('matchMedia("(min-width: 1200px)")', script)
            self.assertNotIn('matchMedia("(min-width: 1100px)")', script)
            self.assertIn("focusWasInsideDrawer", script)
            self.assertIn("visibleFallback.focus()", script)
            self.assertIn('classList.add("is-suppressed")', script)
            self.assertNotIn('button.setAttribute("aria-expanded"', script)
            self.assertIn("restoreMoreFocus", script)
            self.assertIn("firstAdded.focus()", script)
            self.assertIn('className = "search-more"', script)
            self.assertIn("appendBatch", script)
            self.assertIn("requestIdleCallback", script)

    def test_index_uses_ihdr_dimensions_and_mobile_picture(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            catalog, _, _, docs_root = self._render_fixture(Path(temporary))
            image_root = docs_root / "media" / "captures" / "edited" / "editor"
            _write_png(image_root / "blueprint-editor.png", 83, 47)
            _write_png(image_root / "mobile" / "blueprint-event-flow.png", 41, 17)

            page = FReferenceRenderer(catalog, docs_root).render_index()

            self.assertIn(
                '<a class="reference-image-link" '
                'href="../media/captures/edited/editor/blueprint-editor.png"',
                page,
            )
            self.assertIn("<picture>", page)
            self.assertIn(
                '<source media="(max-width: 700px)" '
                'srcset="../media/captures/edited/editor/mobile/blueprint-event-flow.png" '
                'width="41" height="17">',
                page,
            )
            self.assertIn(
                '<img src="../media/captures/edited/editor/blueprint-editor.png" '
                'alt="ACS BlueprintでEvent Graphを編集している画面" loading="lazy" '
                'width="83" height="47">',
                page,
            )

    def test_index_rejects_missing_or_non_png_top_images(self) -> None:
        cases = (
            (
                "blueprint-editor.png",
                "トップページのACS Blueprint全体画像が見つかりません",
                "missing",
            ),
            (
                "mobile/blueprint-event-flow.png",
                "トップページのACS Blueprintモバイル画像が見つかりません",
                "missing",
            ),
            (
                "blueprint-editor.png",
                "トップページのACS Blueprint全体画像はPNG画像ではありません",
                "not-png",
            ),
        )
        for relative_path, expected_error, mutation in cases:
            with self.subTest(relative_path=relative_path, mutation=mutation):
                with tempfile.TemporaryDirectory() as temporary:
                    catalog, _, _, docs_root = self._render_fixture(Path(temporary))
                    image_path = (
                        docs_root / "media" / "captures" / "edited" / "editor" / relative_path
                    )
                    if mutation == "missing":
                        image_path.unlink()
                    else:
                        image_path.write_bytes(b"ACS")

                    with self.assertRaisesRegex(ValueError, expected_error):
                        FReferenceRenderer(catalog, docs_root).render_index()

    def test_index_rejects_invalid_png_dimensions(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            catalog, _, _, docs_root = self._render_fixture(Path(temporary))
            mobile_image = (
                docs_root
                / "media"
                / "captures"
                / "edited"
                / "editor"
                / "mobile"
                / "blueprint-event-flow.png"
            )
            _write_png(mobile_image, 0, 17)

            with self.assertRaisesRegex(
                ValueError,
                "トップページのACS Blueprintモバイル画像のPNG IHDRの寸法が不正です",
            ):
                FReferenceRenderer(catalog, docs_root).render_index()

    def test_rendered_text_files_have_no_trailing_whitespace(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            _, files, _, _ = self._render_fixture(Path(temporary))
            failures: list[str] = []
            for route, content in sorted(files.items()):
                if Path(route).suffix not in {".css", ".html", ".js", ".json"}:
                    continue
                for line_number, line in enumerate(content.decode("utf-8").splitlines(), 1):
                    if line != line.rstrip(" \t"):
                        failures.append(f"{route}:{line_number}")
            self.assertEqual([], failures)

    def test_validator_rejects_a_missing_internal_link(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            _, files, output_root, docs_root = self._render_fixture(Path(temporary))
            files["index.html"] = files["index.html"].replace(
                b"</body>",
                '<a href="missing.html">存在しないページ</a></body>'.encode("utf-8"),
            )
            with self.assertRaisesRegex(ValueError, "リンク先がありません"):
                validate_output(files, output_root, docs_root)


if __name__ == "__main__":
    unittest.main()
