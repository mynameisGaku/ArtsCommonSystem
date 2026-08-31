# SPDX-License-Identifier: MIT

from __future__ import annotations

import html
import json
import posixpath
import re
import struct
import zlib
from collections import defaultdict
from pathlib import Path, PurePosixPath
from typing import Iterable

from .catalog import KIND_LABELS, _symbol_name_spans, sanitize_acs_prose, slug, strip_rich_text
from .model import (
    FFeatureMemberRecord,
    FFeatureRecord,
    FGlossaryRecord,
    FGuideRecord,
    FReferenceCatalog,
    FSymbolRecord,
    FTroubleshootingRecord,
)


ROOT_PAGES = {
    "index": "index.html",
    "guide": "guide.html",
    "search": "search.html",
    "glossary": "glossary.html",
    "troubleshooting": "troubleshooting.html",
}

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
PNG_IHDR_DATA_LENGTH = 13
PNG_MAX_DIMENSION = (1 << 31) - 1


def escape(value: object) -> str:
    return html.escape(str(value or ""), quote=True)


def relative_href(current_route: str, target_route: str) -> str:
    current_directory = posixpath.dirname(current_route) or "."
    return posixpath.relpath(target_route, current_directory)


def kind_label(kind: str) -> str:
    return KIND_LABELS.get(kind, kind or "API")


def searchable_text(value: object) -> str:
    if isinstance(value, dict):
        return " ".join(filter(None, (searchable_text(item) for item in value.values())))
    if isinstance(value, (list, tuple)):
        return " ".join(filter(None, (searchable_text(item) for item in value)))
    return strip_rich_text(value)


def read_png_dimensions(path: Path, image_label: str) -> tuple[int, int]:
    """PNGのIHDRを検証し、画像の実寸を返す。"""

    try:
        with path.open("rb") as image_file:
            header = image_file.read(33)
    except FileNotFoundError:
        raise ValueError(f"{image_label}が見つかりません: {path}") from None
    except OSError as error:
        raise ValueError(f"{image_label}を読み込めません: {path} ({error})") from error

    if len(header) < len(PNG_SIGNATURE) or header[:8] != PNG_SIGNATURE:
        raise ValueError(f"{image_label}はPNG画像ではありません: {path}")
    if len(header) < 33:
        raise ValueError(f"{image_label}のPNG IHDRが不完全です: {path}")

    chunk_length = struct.unpack(">I", header[8:12])[0]
    chunk_type = header[12:16]
    if chunk_length != PNG_IHDR_DATA_LENGTH or chunk_type != b"IHDR":
        raise ValueError(f"{image_label}のPNG IHDRが不正です: {path}")

    expected_crc = struct.unpack(">I", header[29:33])[0]
    actual_crc = zlib.crc32(header[12:29]) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        raise ValueError(f"{image_label}のPNG IHDRが破損しています: {path}")

    width, height = struct.unpack(">II", header[16:24])
    if not (1 <= width <= PNG_MAX_DIMENSION and 1 <= height <= PNG_MAX_DIMENSION):
        raise ValueError(
            f"{image_label}のPNG IHDRの寸法が不正です: {path} "
            f"(width={width}, height={height})"
        )
    return width, height


class FReferenceRenderer:
    """ACS の catalog から静的HTML一式を生成する。"""

    def __init__(self, catalog: FReferenceCatalog, docs_root: Path) -> None:
        self.catalog = catalog
        self.docs_root = docs_root
        self.symbol_by_id = {symbol.id: symbol for symbol in catalog.symbols}
        self.children_by_parent: dict[str, list[FSymbolRecord]] = defaultdict(list)
        self.root_symbols_by_module: dict[str, list[FSymbolRecord]] = defaultdict(list)
        self.features_by_module: dict[str, list[FFeatureRecord]] = defaultdict(list)
        for symbol in catalog.symbols:
            if symbol.parent_id:
                self.children_by_parent[symbol.parent_id].append(symbol)
            else:
                self.root_symbols_by_module[symbol.module].append(symbol)
        for feature in catalog.features:
            self.features_by_module[feature.module].append(feature)
        self.modules = sorted(
            set(self.root_symbols_by_module) | set(self.features_by_module),
            key=str.casefold,
        )
        self.glossary_by_term = {record.term: record for record in catalog.glossary}
        terms = [term for term in self.glossary_by_term if len(term) >= 2]
        terms.sort(key=lambda value: (-len(value), value.casefold()))
        self.term_pattern = re.compile("|".join(re.escape(term) for term in terms)) if terms else None
        self.tooltip_serial = 0
        self.symbols_by_name = self._build_symbol_lookup(catalog.symbols)

    @staticmethod
    def _build_symbol_lookup(symbols: Iterable[FSymbolRecord]) -> dict[str, list[FSymbolRecord]]:
        grouped: dict[str, list[FSymbolRecord]] = defaultdict(list)
        for symbol in symbols:
            grouped[symbol.name].append(symbol)
        return dict(grouped)

    def _signature_target(self, token: str, current: FSymbolRecord) -> FSymbolRecord | None:
        if token == current.name:
            return current
        candidates = self.symbols_by_name.get(token, [])
        if not candidates:
            return None

        if current.parent_id:
            siblings = [record for record in candidates if record.parent_id == current.parent_id]
            if len(siblings) == 1:
                return siblings[0]
            owner = self.symbol_by_id.get(current.parent_id)
            if owner and owner.name == token:
                return owner

        owned = [record for record in candidates if record.parent_id == current.id]
        if len(owned) == 1:
            return owned[0]

        same_module = [record for record in candidates if record.module == current.module]
        if len(same_module) == 1:
            return same_module[0]
        if len(candidates) == 1:
            return candidates[0]
        return None

    def decorate_terms(self, text: str, current_route: str) -> str:
        plain = sanitize_acs_prose(text)
        if not plain or not self.term_pattern:
            return escape(plain)
        output: list[str] = []
        cursor = 0
        for match in self.term_pattern.finditer(plain):
            output.append(escape(plain[cursor:match.start()]))
            term = match.group(0)
            record = self.glossary_by_term.get(term)
            if not record:
                output.append(escape(term))
                cursor = match.end()
                continue
            self.tooltip_serial += 1
            tooltip_id = f"term-tip-{self.tooltip_serial}"
            output.append(
                '<span class="term-wrap">'
                f'<button class="term-trigger" type="button" '
                f'aria-describedby="{tooltip_id}">{escape(term)}</button>'
                f'<span class="term-tooltip" id="{tooltip_id}" role="tooltip">'
                f'{escape(record.definition)}'
                "</span></span>"
            )
            cursor = match.end()
        output.append(escape(plain[cursor:]))
        return "".join(output)

    def linkify_signature(self, signature: str, current: FSymbolRecord) -> str:
        output: list[str] = []
        cursor = 0
        for match in re.finditer(r"[A-Za-z_]\w*", signature):
            output.append(escape(signature[cursor:match.start()]))
            token = match.group(0)
            target = self._signature_target(token, current)
            if target:
                href = relative_href(current.route, target.route)
                current_marker = ' aria-current="page"' if target.id == current.id else ""
                output.append(f'<a href="{escape(href)}"{current_marker}>{escape(token)}</a>')
            else:
                output.append(escape(token))
            cursor = match.end()
        output.append(escape(signature[cursor:]))
        return "".join(output)

    def _feature_member_groups(
        self,
        member: FFeatureMemberRecord,
    ) -> list[tuple[str, list[FSymbolRecord]]]:
        grouped: dict[str, list[FSymbolRecord]] = {}
        for symbol_id in member.target_ids:
            symbol = self.symbol_by_id.get(symbol_id)
            if symbol is None:
                continue
            grouped.setdefault(symbol.name, []).append(symbol)
        return list(grouped.items())

    def _linkify_feature_member_signature(
        self,
        member: FFeatureMemberRecord,
        route: str,
    ) -> str:
        links: list[tuple[int, int, FSymbolRecord]] = []
        for name, symbols in self._feature_member_groups(member):
            if len(symbols) != 1:
                continue
            for start, end in _symbol_name_spans(member.signature, name):
                links.append((start, end, symbols[0]))
        links.sort(key=lambda item: (item[0], -(item[1] - item[0]), item[2].id))

        output: list[str] = []
        cursor = 0
        for start, end, symbol in links:
            if start < cursor:
                continue
            output.append(escape(member.signature[cursor:start]))
            output.append(
                f'<a href="{escape(relative_href(route, symbol.route))}">'
                f'{escape(member.signature[start:end])}</a>'
            )
            cursor = end
        output.append(escape(member.signature[cursor:]))
        return "".join(output)

    def _feature_member_api_links(self, member: FFeatureMemberRecord, route: str) -> str:
        groups = self._feature_member_groups(member)
        if not groups:
            return (
                '<p class="notice">対応するAPIを特定できないため、宣言をリンクせず表示しています。</p>'
            )

        rendered: list[str] = []
        for name, symbols in groups:
            if len(symbols) == 1:
                symbol = symbols[0]
                rendered.append(
                    '<div class="fact"><dt>対応API</dt><dd>'
                    f'<a href="{escape(relative_href(route, symbol.route))}">'
                    f'<code>{escape(symbol.qualified_name)}</code></a>'
                    f' <span class="kind-label">{escape(kind_label(symbol.kind))}</span>'
                    '</dd></div>'
                )
                continue
            candidates = "".join(
                '<li>'
                f'<a href="{escape(relative_href(route, symbol.route))}">'
                f'<code>{escape(symbol.qualified_name)}</code></a>'
                f'<span class="summary"><code>{escape(symbol.signature)}</code></span>'
                '</li>'
                for symbol in symbols
            )
            rendered.append(
                '<div class="fact"><dt>overload候補</dt><dd>'
                f'<span><code>{escape(name)}</code> は複数の宣言に対応します。</span>'
                f'<ul>{candidates}</ul></dd></div>'
            )
        return '<dl class="facts">' + "".join(rendered) + "</dl>"

    def _render_feature_member(self, member: FFeatureMemberRecord, route: str) -> str:
        details: list[str] = []
        if member.description:
            details.append(
                '<div class="fact"><dt>説明</dt>'
                f'<dd>{self.decorate_terms(member.description, route)}</dd></div>'
            )
        if member.returns:
            details.append(
                '<div class="fact"><dt>戻り値</dt>'
                f'<dd>{self.decorate_terms(member.returns, route)}</dd></div>'
            )
        if member.usage:
            details.append(
                '<div class="fact"><dt>使う場面</dt>'
                f'<dd>{self.decorate_terms(member.usage, route)}</dd></div>'
            )
        if member.sample:
            details.append(
                '<div class="fact"><dt>使用例</dt><dd>'
                f'<pre class="signature" tabindex="0"><code>{escape(member.sample)}</code></pre>'
                '</dd></div>'
            )
        if member.note:
            details.append(
                '<div class="fact"><dt>補足</dt>'
                f'<dd>{self.decorate_terms(member.note, route)}</dd></div>'
            )
        detail_markup = '<dl class="facts">' + "".join(details) + "</dl>" if details else ""
        return (
            f'<article class="card" id="member-{escape(member.id)}">'
            f'<h3>API項目 {member.source_index + 1}</h3>'
            '<pre class="signature signature--declaration" tabindex="0" '
            f'aria-label="API項目 {member.source_index + 1} の宣言"><code>'
            f'{self._linkify_feature_member_signature(member, route)}</code></pre>'
            f'{detail_markup}{self._feature_member_api_links(member, route)}</article>'
        )

    def _module_route(self, module: str) -> str:
        return f"modules/{slug(module)}.html"

    def _top_navigation(self, route: str) -> str:
        links = [
            ("はじめる", ROOT_PAGES["guide"]),
            ("API", ROOT_PAGES["index"]),
            ("検索", ROOT_PAGES["search"]),
            ("用語集", ROOT_PAGES["glossary"]),
            ("問題を調べる", ROOT_PAGES["troubleshooting"]),
        ]
        return "".join(
            f'<a href="{escape(relative_href(route, target))}">{escape(label)}</a>'
            for label, target in links
        )

    def _sidebar(self, route: str, module: str | None, current_symbol: FSymbolRecord | None) -> str:
        links = [
            f'<a href="{escape(relative_href(route, ROOT_PAGES["index"]))}">リファレンス概要</a>',
            f'<a href="{escape(relative_href(route, ROOT_PAGES["guide"]))}">はじめる</a>',
            f'<a href="{escape(relative_href(route, ROOT_PAGES["search"]))}">機能とAPIを検索</a>',
        ]
        if module:
            links.append(
                f'<a href="{escape(relative_href(route, self._module_route(module)))}">'
                f'{escape(module)} モジュール</a>'
            )
        if current_symbol:
            if current_symbol.parent_id:
                owner = self.symbol_by_id[current_symbol.parent_id]
                links.append(
                    f'<a href="{escape(relative_href(route, owner.route))}">'
                    f'{escape(owner.name)} の概要</a>'
                )
                siblings = self.children_by_parent.get(owner.id, [])
                for sibling in siblings:
                    current = ' aria-current="page"' if sibling.id == current_symbol.id else ""
                    links.append(
                        f'<a class="tree-child" href="{escape(relative_href(route, sibling.route))}"{current}>'
                        f'{escape(sibling.name)}</a>'
                    )
            else:
                current = ' aria-current="page"'
                links.append(
                    f'<a href="{escape(relative_href(route, current_symbol.route))}"{current}>'
                    f'{escape(current_symbol.name)}</a>'
                )
                for child in self.children_by_parent.get(current_symbol.id, []):
                    links.append(
                        f'<a class="tree-child" href="{escape(relative_href(route, child.route))}">'
                        f'{escape(child.name)}</a>'
                    )
        links.append(f'<a href="{escape(relative_href(route, ROOT_PAGES["glossary"]))}">用語集</a>')
        return (
            '<aside class="sidebar" aria-label="ページナビゲーション">'
            '<h2>ACS リファレンス</h2>'
            '<nav aria-label="リファレンス内の関連ページ">' + "".join(links) + "</nav></aside>"
        )

    def _right_rail(self, sections: list[tuple[str, str]]) -> str:
        if not sections:
            return ""
        links = "".join(f'<a href="#{escape(identifier)}">{escape(label)}</a>' for identifier, label in sections)
        return (
            '<aside class="right-rail" aria-label="このページの目次">'
            '<h2>このページ</h2>'
            '<nav aria-label="このページ内の見出し">' + links + "</nav></aside>"
        )

    def _page_shell(
        self,
        *,
        route: str,
        title: str,
        description: str,
        body: str,
        module: str | None = None,
        current_symbol: FSymbolRecord | None = None,
        sections: list[tuple[str, str]] | None = None,
        load_search_index: bool = False,
    ) -> str:
        css_href = relative_href(route, "assets/css/reference.css")
        script_href = relative_href(route, "assets/js/reference.js")
        search_href = relative_href(route, ROOT_PAGES["search"])
        index_href = relative_href(route, ROOT_PAGES["index"])
        search_script_line = (
            "  "
            f'<script src="{escape(relative_href(route, "assets/js/search-index.js"))}" defer></script>\n'
            if load_search_index else ""
        )
        drawer_links = self._top_navigation(route)
        if module:
            drawer_links += (
                f'<a href="{escape(relative_href(route, self._module_route(module)))}">'
                f'{escape(module)} モジュール</a>'
            )
        right_rail = self._right_rail(sections or [])
        right_rail_line = f"    {right_rail}\n" if right_rail else ""
        return f'''<!doctype html>
<html lang="ja">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="description" content="{escape(description)}">
  <title>{escape(title)} — ACS リファレンス</title>
  <link rel="stylesheet" href="{escape(css_href)}">
{search_script_line}  <script src="{escape(script_href)}" defer></script>
</head>
<body>
  <a class="skip-link" href="#main">本文へ移動</a>
  <header class="site-header">
    <a class="brand" href="{escape(index_href)}"><span class="brand-mark">ACS</span><span class="brand-label">機能リファレンス</span></a>
    <form class="site-search" action="{escape(search_href)}" method="get" role="search" aria-label="ACSリファレンス全体を検索">
      <label class="visually-hidden" for="header-search">ACSの機能とAPIを検索</label>
      <input id="header-search" name="q" type="search" placeholder="型、関数、機能を検索…" autocomplete="off">
    </form>
    <div class="header-actions">
      <button class="icon-button" type="button" data-theme-toggle aria-label="配色を切り替える">◐</button>
      <button class="nav-toggle" type="button" data-nav-toggle aria-controls="mobile-drawer" aria-expanded="false" aria-label="ナビゲーションを開く">☰</button>
    </div>
  </header>
  <noscript><nav class="noscript-nav" aria-label="主要ナビゲーション">{drawer_links}</nav></noscript>
  <div class="drawer-backdrop" data-drawer-backdrop hidden aria-hidden="true"></div>
  <aside class="mobile-drawer" id="mobile-drawer" data-mobile-drawer hidden inert role="dialog" aria-modal="true" aria-labelledby="drawer-title">
    <button class="icon-button" type="button" data-nav-close aria-label="ナビゲーションを閉じる">×</button>
    <h2 class="drawer-title" id="drawer-title">ACS リファレンス</h2>
    <nav class="drawer-nav" aria-label="モバイル用主要ナビゲーション">{drawer_links}</nav>
  </aside>
  <div class="layout">
    {self._sidebar(route, module, current_symbol)}
    <main id="main" class="content">{body}<footer class="footer">ACS の機能、型、関数、メンバー、定数を参照できます。</footer></main>
{right_rail_line}  </div>
</body>
</html>
'''

    def _breadcrumbs(self, route: str, parts: list[tuple[str, str | None]]) -> str:
        rendered: list[str] = []
        for label, target in parts:
            if target:
                rendered.append(f'<a href="{escape(relative_href(route, target))}">{escape(label)}</a>')
            else:
                rendered.append(f'<span aria-current="page">{escape(label)}</span>')
        return '<nav class="breadcrumbs" aria-label="パンくず">' + '<span>/</span>'.join(rendered) + "</nav>"

    def render_index(self) -> str:
        route = ROOT_PAGES["index"]
        root_symbol_count = sum(1 for symbol in self.catalog.symbols if symbol.parent_id is None)
        member_count = len(self.catalog.symbols) - root_symbol_count
        full_image_relative = "../media/captures/edited/editor/blueprint-editor.png"
        mobile_image_relative = "../media/captures/edited/editor/mobile/blueprint-event-flow.png"
        image_root = self.docs_root / "media" / "captures" / "edited" / "editor"
        full_width, full_height = read_png_dimensions(
            image_root / "blueprint-editor.png",
            "トップページのACS Blueprint全体画像",
        )
        mobile_width, mobile_height = read_png_dimensions(
            image_root / "mobile" / "blueprint-event-flow.png",
            "トップページのACS Blueprintモバイル画像",
        )
        image_markup = (
            '<section class="section" id="screen"><h2>ACS Blueprint</h2>'
            '<figure class="reference-figure"><a class="reference-image-link" '
            f'href="{full_image_relative}" '
            'aria-label="ACS BlueprintのEvent Graphを原寸で表示">'
            '<picture>'
            f'<source media="(max-width: 700px)" srcset="{mobile_image_relative}" '
            f'width="{mobile_width}" height="{mobile_height}">'
            f'<img src="{full_image_relative}" '
            'alt="ACS BlueprintでEvent Graphを編集している画面" loading="lazy" '
            f'width="{full_width}" height="{full_height}">'
            '</picture></a>'
            '<figcaption>Event、関数、macro、変数をgraph上で接続します。狭い画面では主要な接続を拡大し、画像を選択すると全体を原寸で表示します。</figcaption></figure></section>'
        )
        module_cards = "".join(
            '<a class="card" href="{href}"><span class="card-title">{module}</span>'
            '<span class="card-description">{symbols}件のAPIと{features}件の機能説明</span>'
            '<span class="card-meta">モジュールを開く →</span></a>'.format(
                href=escape(relative_href(route, self._module_route(module))),
                module=escape(module),
                symbols=len(self.root_symbols_by_module.get(module, [])),
                features=len(self.features_by_module.get(module, [])),
            )
            for module in self.modules
        )
        body = f'''
{self._breadcrumbs(route, [("リファレンス", None)])}
<section class="hero">
  <div class="eyebrow">ACS / 機能リファレンス</div>
  <h1>ACSを、機能からコードまで辿る</h1>
  <p class="lead">機能、モジュール、型、関数、メンバー変数、定数を個別ページで確認できます。識別子を選ぶと、その項目のページへ移動します。</p>
  <div class="stats" aria-label="収録件数">
    <div class="stat"><strong>{len(self.modules)}</strong><span>モジュール</span></div>
    <div class="stat"><strong>{len(self.catalog.features)}</strong><span>機能ページ</span></div>
    <div class="stat"><strong>{root_symbol_count}</strong><span>型・関数・定数</span></div>
    <div class="stat"><strong>{member_count}</strong><span>メンバー・列挙値</span></div>
  </div>
</section>
{image_markup}
<section class="section" id="modules"><h2>モジュール</h2><div class="card-grid">{module_cards}</div></section>
'''
        return self._page_shell(
            route=route,
            title="概要",
            description="ACSの機能、型、関数、メンバー、定数を個別ページで参照できます。",
            body=body,
            sections=[("modules", "モジュール")],
        )

    def render_module(self, module: str) -> str:
        route = self._module_route(module)
        features = self.features_by_module.get(module, [])
        symbols = self.root_symbols_by_module.get(module, [])
        feature_cards = "".join(
            f'<a class="card" href="{escape(relative_href(route, feature.route))}">'
            f'<span class="card-title">{escape(feature.title)}</span>'
            f'<span class="card-description">{escape(feature.summary)}</span>'
            f'<span class="card-meta">{escape(feature.kind)}</span></a>'
            for feature in features
        ) or '<p>このモジュールには手書きの機能説明がありません。</p>'
        grouped: dict[str, list[FSymbolRecord]] = defaultdict(list)
        for symbol in symbols:
            grouped[kind_label(symbol.kind)].append(symbol)
        symbol_sections: list[str] = []
        for label in sorted(grouped, key=str.casefold):
            cards = "".join(
                f'<a class="member-card" href="{escape(relative_href(route, symbol.route))}">'
                f'<span><span class="identifier">{escape(symbol.qualified_name)}</span>'
                f'<span class="summary">{escape(symbol.description)}</span></span>'
                f'<span class="kind-label">{escape(label)} →</span></a>'
                for symbol in grouped[label]
            )
            symbol_sections.append(f'<h3>{escape(label)}</h3><div class="member-list">{cards}</div>')
        body = f'''
{self._breadcrumbs(route, [("リファレンス", ROOT_PAGES["index"]), (module, None)])}
<header class="symbol-header"><div><div class="eyebrow">ACS モジュール</div><h1 class="identifier">{escape(module)}</h1>
<p class="lead">{escape(module)} モジュールの機能説明とAPIをまとめています。</p></div>
<div class="badges"><span class="badge">機能 {len(features)}</span><span class="badge">API {len(symbols)}</span></div></header>
<section class="section" id="features"><h2>機能</h2><div class="card-grid">{feature_cards}</div></section>
<section class="section" id="api"><h2>API</h2>{''.join(symbol_sections)}</section>
'''
        return self._page_shell(
            route=route,
            title=f"{module} モジュール",
            description=f"ACSの{module}モジュールに含まれる機能とAPIです。",
            body=body,
            module=module,
            sections=[("features", "機能"), ("api", "API")],
        )

    def render_symbol(self, symbol: FSymbolRecord) -> str:
        route = symbol.route
        parent = self.symbol_by_id.get(symbol.parent_id) if symbol.parent_id else None
        breadcrumbs: list[tuple[str, str | None]] = [
            ("リファレンス", ROOT_PAGES["index"]),
            (symbol.module, self._module_route(symbol.module)),
        ]
        if parent:
            breadcrumbs.append((parent.name, parent.route))
        breadcrumbs.append((symbol.name, None))
        access_label = "公開API" if symbol.access == "public" else f"{symbol.access} 内部API"
        access_class = "public" if symbol.access == "public" else "internal"
        owner_fact = ""
        if parent:
            owner_fact = (
                '<div class="fact"><dt>所有型</dt><dd>'
                f'<a href="{escape(relative_href(route, parent.route))}"><code>{escape(parent.qualified_name)}</code></a>'
                "</dd></div>"
            )
        child_markup = ""
        children = self.children_by_parent.get(symbol.id, [])
        if children:
            cards = "".join(
                f'<a class="member-card" href="{escape(relative_href(route, child.route))}">'
                f'<span><span class="identifier">{escape(child.signature)}</span>'
                f'<span class="summary">{escape(child.description)}</span></span>'
                f'<span class="kind-label">{escape(kind_label(child.kind))} →</span></a>'
                for child in children
            )
            child_markup = f'<section class="section" id="members"><h2>項目</h2><div class="member-list">{cards}</div></section>'
        body = f'''
{self._breadcrumbs(route, breadcrumbs)}
<header class="symbol-header"><div><div class="eyebrow">{escape(kind_label(symbol.kind))} / {escape(symbol.module)}</div>
<h1 class="identifier">{escape(symbol.qualified_name)}</h1>
<p class="lead">{self.decorate_terms(symbol.description, route)}</p></div>
<div class="badges"><span class="badge {access_class}">{escape(access_label)}</span><span class="badge">{escape(kind_label(symbol.kind))}</span></div></header>
<section class="section" id="signature"><h2>宣言</h2><pre class="signature signature--declaration" aria-label="{escape(symbol.qualified_name)}の宣言"><code>{self.linkify_signature(symbol.signature, symbol)}</code></pre></section>
<section class="section" id="facts"><h2>基本情報</h2><dl class="facts">
<div class="fact"><dt>モジュール</dt><dd><a href="{escape(relative_href(route, self._module_route(symbol.module)))}">{escape(symbol.module)}</a></dd></div>
<div class="fact"><dt>分類</dt><dd>{escape(kind_label(symbol.kind))}</dd></div>
{owner_fact}<div class="fact"><dt>宣言位置</dt><dd class="path">{escape(symbol.source_path)}:{symbol.source_line}</dd></div>
</dl></section>
{child_markup}
'''
        sections = [("signature", "宣言"), ("facts", "基本情報")]
        if children:
            sections.append(("members", "項目"))
        return self._page_shell(
            route=route,
            title=symbol.qualified_name,
            description=symbol.description,
            body=body,
            module=symbol.module,
            current_symbol=symbol,
            sections=sections,
        )

    def render_feature(self, feature: FFeatureRecord) -> str:
        route = feature.route
        feature_symbol_ids = feature.symbol_ids or ([feature.symbol_id] if feature.symbol_id else [])
        feature_symbols = [
            self.symbol_by_id[symbol_id]
            for symbol_id in feature_symbol_ids
            if symbol_id in self.symbol_by_id
        ]
        api_link = ""
        if len(feature_symbols) == 1:
            symbol = feature_symbols[0]
            api_link = (
                '<div class="notice">対応するAPI: '
                f'<a href="{escape(relative_href(route, symbol.route))}"><code>{escape(symbol.qualified_name)}</code></a></div>'
            )
        elif feature_symbols:
            candidates = "".join(
                '<li>'
                f'<a href="{escape(relative_href(route, symbol.route))}">'
                f'<code>{escape(symbol.qualified_name)}</code></a>'
                f'<span class="summary"><code>{escape(symbol.signature)}</code></span>'
                '</li>'
                for symbol in feature_symbols
            )
            api_link = (
                '<div class="notice"><p>対応するAPI候補</p>'
                f'<ul>{candidates}</ul></div>'
            )
        usage = ""
        if feature.usage:
            usage = (
                '<section class="section" id="usage"><h2>使う場面</h2>'
                f'<p>{self.decorate_terms(feature.usage, route)}</p></section>'
            )
        note = ""
        if feature.note:
            note = (
                '<section class="section" id="note"><h2>補足</h2>'
                f'<p>{self.decorate_terms(feature.note, route)}</p></section>'
            )
        members = ""
        if feature.members:
            member_items = "".join(self._render_feature_member(member, route) for member in feature.members)
            members = (
                '<section class="section" id="api-items"><h2>API項目</h2>'
                f'<div class="member-list">{member_items}</div></section>'
            )
        sample = ""
        if feature.sample:
            sample = (
                '<section class="section" id="sample"><h2>使用例</h2>'
                f'<pre class="signature" tabindex="0" aria-label="{escape(feature.title)}の使用例"><code>{escape(feature.sample)}</code></pre></section>'
            )
        facts = [
            ("モジュール", feature.module),
            ("分類", feature.kind),
        ]
        if feature.header:
            facts.append(("ヘッダー", feature.header))
        fact_markup = "".join(
            f'<div class="fact"><dt>{escape(label)}</dt><dd>{escape(value)}</dd></div>'
            for label, value in facts
        )
        body = f'''
{self._breadcrumbs(route, [("リファレンス", ROOT_PAGES["index"]), (feature.module, self._module_route(feature.module)), (feature.title, None)])}
<header class="symbol-header"><div><div class="eyebrow">ACS 機能 / {escape(feature.module)}</div><h1 class="identifier">{escape(feature.title)}</h1>
<p class="lead">{self.decorate_terms(feature.summary, route)}</p></div><div class="badges"><span class="badge">{escape(feature.kind)}</span></div></header>
{api_link}
{usage}
{note}
<section class="section" id="facts"><h2>基本情報</h2><dl class="facts">{fact_markup}</dl></section>
{members}
{sample}
'''
        sections: list[tuple[str, str]] = []
        if feature.usage:
            sections.append(("usage", "使う場面"))
        if feature.note:
            sections.append(("note", "補足"))
        sections.append(("facts", "基本情報"))
        if feature.members:
            sections.append(("api-items", "API項目"))
        if feature.sample:
            sections.append(("sample", "使用例"))
        return self._page_shell(
            route=route,
            title=feature.title,
            description=feature.summary,
            body=body,
            module=feature.module,
            sections=sections,
        )

    def _render_doc_block(self, block: dict[str, object], route: str) -> str:
        if block.get("h2"):
            return f'<h2>{escape(strip_rich_text(block["h2"]))}</h2>'
        if block.get("h3"):
            return f'<h3>{escape(strip_rich_text(block["h3"]))}</h3>'
        if block.get("p"):
            return f'<p>{self.decorate_terms(str(block["p"]), route)}</p>'
        if block.get("note"):
            return f'<div class="notice">{self.decorate_terms(str(block["note"]), route)}</div>'
        if block.get("code"):
            return f'<pre class="signature" tabindex="0"><code>{escape(block["code"])}</code></pre>'
        if isinstance(block.get("ul"), list):
            items = "".join(f'<li>{self.decorate_terms(str(item), route)}</li>' for item in block["ul"])
            return f"<ul>{items}</ul>"
        return ""

    def render_guide_index(self) -> str:
        route = ROOT_PAGES["guide"]
        cards = "".join(
            f'<a class="card" href="{escape(relative_href(route, guide.route))}">'
            f'<span class="card-title">{escape(guide.title)}</span>'
            '<span class="card-description">ACSを使うための基本事項を説明します。</span>'
            '<span class="card-meta">ガイドを開く →</span></a>'
            for guide in self.catalog.guides
        )
        body = f'''
{self._breadcrumbs(route, [("リファレンス", ROOT_PAGES["index"]), ("はじめる", None)])}
<section class="hero"><div class="eyebrow">ACS / はじめる</div><h1>ACSの基本を確認する</h1>
<p class="lead">名前付け、エラー処理、モジュール、所有関係など、ACSを読むための共通事項を説明します。</p></section>
<section class="section" id="guides"><h2>ガイド</h2><div class="card-grid">{cards}</div></section>
'''
        return self._page_shell(
            route=route,
            title="はじめる",
            description="ACSの基本的な使い方と共通規約です。",
            body=body,
            sections=[("guides", "ガイド")],
        )

    def render_guide(self, guide: FGuideRecord) -> str:
        route = guide.route
        blocks = "".join(self._render_doc_block(block, route) for block in guide.blocks)
        body = f'''
{self._breadcrumbs(route, [("リファレンス", ROOT_PAGES["index"]), ("はじめる", ROOT_PAGES["guide"]), (guide.title, None)])}
<header class="symbol-header"><div><div class="eyebrow">ACS ガイド</div><h1>{escape(guide.title)}</h1></div></header>
<article class="doc-blocks" id="guide-content">{blocks}</article>
'''
        return self._page_shell(
            route=route,
            title=guide.title,
            description=f"ACSの{guide.title}に関するガイドです。",
            body=body,
            sections=[("guide-content", guide.title)],
        )

    def render_glossary(self) -> str:
        route = ROOT_PAGES["glossary"]
        entries = "".join(
            f'<div class="glossary-entry" id="term-{record.id}"><dt>{escape(record.term)}</dt><dd>{escape(record.definition)}</dd></div>'
            for record in self.catalog.glossary
        )
        body = f'''
{self._breadcrumbs(route, [("リファレンス", ROOT_PAGES["index"]), ("用語集", None)])}
<header class="symbol-header"><div><div class="eyebrow">ACS 用語集</div><h1>専門用語</h1>
<p class="lead">ACSの文書とAPI説明で使用する用語をまとめています。各リファレンスページでは、用語にカーソルを重ねるかフォーカスすると短い説明を確認できます。</p></div>
<div class="badges"><span class="badge">{len(self.catalog.glossary)} 用語</span></div></header>
<section class="section" id="terms"><h2>用語</h2><dl class="glossary-list">{entries}</dl></section>
'''
        return self._page_shell(
            route=route,
            title="用語集",
            description="ACSの専門用語と補足説明です。",
            body=body,
            sections=[("terms", "用語")],
        )

    def render_search(self) -> str:
        route = ROOT_PAGES["search"]
        body = f'''
{self._breadcrumbs(route, [("リファレンス", ROOT_PAGES["index"]), ("検索", None)])}
<header class="symbol-header"><div><div class="eyebrow">ACS 全体検索</div><h1>機能とAPIを検索</h1>
<p class="lead">機能名、モジュール名、型名、関数名、メンバー名、定数、用語、症状を横断検索します。</p></div></header>
<form class="page-search" data-reference-search role="search" aria-label="機能とAPIの検索結果を絞り込む">
  <label class="visually-hidden" for="reference-search-input">検索語</label>
  <input id="reference-search-input" data-reference-search-input type="search" name="q" placeholder="例: TUniquePtr、Get、所有権、真っ黒" autocomplete="off">
  <button type="submit">検索</button>
</form>
<noscript><p class="notice">検索結果の絞り込みにはJavaScriptが必要です。APIは<a href="{escape(relative_href(route, ROOT_PAGES["index"]))}">リファレンス概要</a>のモジュール一覧から参照できます。</p></noscript>
<p class="search-status" data-search-status aria-live="polite"></p>
<div class="result-list" data-search-results></div>
'''
        return self._page_shell(
            route=route,
            title="検索",
            description="ACSの機能とAPIを横断検索します。",
            body=body,
            load_search_index=True,
        )

    def render_troubleshooting_index(self) -> str:
        route = ROOT_PAGES["troubleshooting"]
        cards = "".join(
            f'<a class="plain-list" href="{escape(relative_href(route, item.route))}"></a>'
            for item in []
        )
        links = "".join(
            f'<a href="{escape(relative_href(route, item.route))}"><span>{escape(item.title)}</span><span class="kind-label">対処を開く →</span></a>'
            for item in self.catalog.troubleshooting
        )
        body = f'''
{self._breadcrumbs(route, [("リファレンス", ROOT_PAGES["index"]), ("問題を調べる", None)])}
<section class="hero"><div class="eyebrow">ACS / 問題を調べる</div><h1>症状から対処を探す</h1>
<p class="lead">ビルド、起動、描画、入力、アセットなどの症状ごとに確認項目をまとめています。</p></section>
<section class="section" id="cases"><h2>症状</h2><div class="plain-list">{links}</div>{cards}</section>
'''
        return self._page_shell(
            route=route,
            title="問題を調べる",
            description="ACSで発生する症状ごとの確認項目です。",
            body=body,
            sections=[("cases", "症状")],
        )

    def render_troubleshooting(self, record: FTroubleshootingRecord) -> str:
        route = record.route
        labels = {
            "symptom": "症状",
            "cause": "原因",
            "fix": "対処",
            "solution": "対処",
            "checks": "確認項目",
            "steps": "手順",
            "note": "補足",
            "body": "説明",
            "q": "症状",
            "a": "対処",
        }
        sections: list[str] = []
        for key, value in record.item.items():
            if key in {"title", "h2"} or value is None or value == "" or value == []:
                continue
            label = labels.get(key, strip_rich_text(key) or "説明")
            if isinstance(value, list):
                content = "<ul>" + "".join(f"<li>{self.decorate_terms(str(item), route)}</li>" for item in value) + "</ul>"
            else:
                content = f"<p>{self.decorate_terms(str(value), route)}</p>"
            sections.append(f'<section class="section"><h2>{escape(label)}</h2>{content}</section>')
        if not sections:
            sections.append('<section class="section"><h2>確認項目</h2><p>該当する機能ページとAPIページを確認してください。</p></section>')
        body = f'''
{self._breadcrumbs(route, [("リファレンス", ROOT_PAGES["index"]), ("問題を調べる", ROOT_PAGES["troubleshooting"]), (record.title, None)])}
<header class="symbol-header"><div><div class="eyebrow">ACS トラブルシューティング</div><h1>{escape(record.title)}</h1></div></header>
{''.join(sections)}
'''
        return self._page_shell(
            route=route,
            title=record.title,
            description=f"ACSの{record.title}に関する確認項目です。",
            body=body,
        )

    def search_index(self) -> list[dict[str, str]]:
        records: list[dict[str, str]] = []
        for module in self.modules:
            records.append({
                "title": module,
                "qualified": module,
                "text": f"ACS の {module} モジュールです。",
                "context": "モジュール",
                "url": self._module_route(module),
            })
        for feature in self.catalog.features:
            member_signatures = " ".join(member.signature for member in feature.members if member.signature)
            member_text = " ".join(
                value
                for member in feature.members
                for value in (
                    member.description,
                    member.returns,
                    member.usage,
                    member.note,
                    member.sample,
                )
                if value
            )
            records.append({
                "title": feature.title,
                "qualified": feature.title,
                "signature": " ".join(filter(None, (member_signatures, feature.sample))),
                "text": " ".join(filter(None, (
                    feature.summary,
                    feature.usage,
                    feature.note,
                    member_text,
                    feature.sample,
                ))),
                "context": f"{feature.module} / 機能",
                "url": feature.route,
            })
        for symbol in self.catalog.symbols:
            records.append({
                "title": symbol.name,
                "qualified": symbol.qualified_name,
                "text": symbol.description[:240],
                "signature": symbol.signature[:640],
                "context": f"{symbol.module} / {kind_label(symbol.kind)}",
                "url": symbol.route,
            })
        for guide in self.catalog.guides:
            records.append({
                "title": guide.title,
                "qualified": guide.title,
                "text": searchable_text(guide.blocks),
                "context": "ガイド",
                "url": guide.route,
            })
        for item in self.catalog.troubleshooting:
            records.append({
                "title": item.title,
                "qualified": item.title,
                "text": searchable_text(item.item),
                "context": "トラブルシューティング",
                "url": item.route,
            })
        for term in self.catalog.glossary:
            records.append({
                "title": term.term,
                "qualified": term.term,
                "text": term.definition,
                "context": "用語集",
                "url": f"glossary.html#term-{term.id}",
            })
        records.sort(key=lambda item: (item["title"].casefold(), item["context"].casefold(), item["url"]))
        return records

    def render_all(self) -> dict[str, bytes]:
        output: dict[str, bytes] = {}
        route_keys: set[str] = set()

        def add(route: str, content: str) -> None:
            key = route.casefold()
            if key in route_keys:
                raise ValueError(f"生成経路が衝突しています: {route}")
            route_keys.add(key)
            output[route] = content.encode("utf-8")

        add(ROOT_PAGES["index"], self.render_index())
        add(ROOT_PAGES["guide"], self.render_guide_index())
        add(ROOT_PAGES["search"], self.render_search())
        add(ROOT_PAGES["glossary"], self.render_glossary())
        add(ROOT_PAGES["troubleshooting"], self.render_troubleshooting_index())
        for module in self.modules:
            add(self._module_route(module), self.render_module(module))
        for symbol in self.catalog.symbols:
            add(symbol.route, self.render_symbol(symbol))
        for feature in self.catalog.features:
            add(feature.route, self.render_feature(feature))
        for guide in self.catalog.guides:
            add(guide.route, self.render_guide(guide))
        for record in self.catalog.troubleshooting:
            add(record.route, self.render_troubleshooting(record))

        search_records = self.search_index()
        contexts = sorted({record.get("context", "") for record in search_records}, key=str.casefold)
        context_ids = {context: index for index, context in enumerate(contexts)}
        compact_records = [
            [
                record.get("title", ""),
                record.get("qualified", ""),
                record.get("signature", ""),
                record.get("text", ""),
                context_ids[record.get("context", "")],
                record.get("url", ""),
            ]
            for record in search_records
        ]
        context_json = json.dumps(contexts, ensure_ascii=False, separators=(",", ":"))
        index_json = json.dumps(compact_records, ensure_ascii=False, separators=(",", ":"))
        add(
            "assets/js/search-index.js",
            f"window.ACS_SEARCH_CONTEXTS={context_json};\nwindow.ACS_SEARCH_INDEX={index_json};\n",
        )
        return output
