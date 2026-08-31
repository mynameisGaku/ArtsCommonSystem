#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""ACSの型機能ページを現行C++宣言と照合する。"""

from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

from reference_site.catalog import build_catalog
from reference_site.model import FFeatureRecord, FReferenceCatalog, FSymbolRecord


SCRIPT_PATH = Path(__file__).resolve()
ACS_ROOT_DEFAULT = SCRIPT_PATH.parents[1]
TYPE_KIND_MARKERS = ("クラス", "構造体", "列挙", "共用体", "インターフェース", "テンプレート")
TYPE_SYMBOL_KINDS = {"class", "struct", "union", "enum", "enum class"}


def configure_utf8_console() -> None:
    """日本語診断をUTF-8で出力する。"""

    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="strict")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ACSの型機能ページとC++宣言を照合します。")
    parser.add_argument("--root", type=Path, default=ACS_ROOT_DEFAULT)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--self-test", action="store_true")
    return parser.parse_args()


def is_single_type_feature(feature: FFeatureRecord) -> bool:
    """単一のC++型を説明する機能ページかを返す。"""

    if not any(marker in feature.kind for marker in TYPE_KIND_MARKERS):
        return False
    name = feature.title.strip()
    if not name or any(separator in name for separator in (" / ", ",", "（", "(")):
        return False
    if "<" in name:
        name = name.split("<", 1)[0].strip()
    return bool(name) and name[0] in "ACFEIT" and name.replace("_", "").isalnum()


def audit_catalog(catalog: FReferenceCatalog, root: Path) -> list[str]:
    """型ページの未解決、誤解決、header欠落を返す。"""

    symbols_by_id = {symbol.id: symbol for symbol in catalog.symbols}
    errors: list[str] = []
    for feature in catalog.features:
        if not is_single_type_feature(feature):
            continue
        source = feature.source_file
        header = feature.header.replace("\\", "/").lstrip("/")
        header_path = root / (header if header.startswith("src/") else f"src/{header}")
        if not header_path.is_file():
            errors.append(f"{source}: headerがありません: {header}")
            continue
        if feature.symbol_id is None:
            errors.append(f"{source}: 型APIを一意に解決できません: {feature.title}")
            continue
        symbol = symbols_by_id.get(feature.symbol_id)
        if symbol is None:
            errors.append(f"{source}: 解決先symbolがありません: {feature.symbol_id}")
            continue
        if symbol.parent_id is not None or symbol.kind not in TYPE_SYMBOL_KINDS:
            errors.append(f"{source}: 解決先がroot型ではありません: {symbol.qualified_name}")
            continue
        expected_name = feature.title.split("<", 1)[0].strip()
        if symbol.name != expected_name:
            errors.append(f"{source}: 型名が一致しません: {feature.title} -> {symbol.qualified_name}")
        expected_header = header if header.startswith("src/") else f"src/{header}"
        if symbol.source_path.casefold() != expected_header.casefold():
            errors.append(
                f"{source}: headerと宣言位置が一致しません: "
                f"{expected_header} -> {symbol.source_path}"
            )
    return errors


def make_symbol() -> FSymbolRecord:
    return FSymbolRecord(
        id="symbol-current",
        module="test",
        kind="class",
        name="FCurrent",
        qualified_name="acs::FCurrent",
        signature="class FCurrent;",
        description="ACSの検査用型です。",
        access="public",
        source_path="src/test/FCurrent.h",
        source_line=1,
        route="symbols/test/fcurrent.html",
    )


def make_feature(symbol_id: str | None) -> FFeatureRecord:
    return FFeatureRecord(
        id="feature-current",
        module="test",
        title="FCurrent",
        kind="クラス",
        header="test/FCurrent.h",
        summary="FCurrentを説明します。",
        usage="検査で使用します。",
        sample="",
        source_file="features/test/fcurrent.json",
        route="features/test/fcurrent.html",
        symbol_id=symbol_id,
    )


def run_self_test() -> int:
    """型解決の成功と失敗を最小catalogで確認する。"""

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        header = root / "src" / "test" / "FCurrent.h"
        header.parent.mkdir(parents=True)
        header.write_text("class FCurrent;\n", encoding="utf-8")
        symbol = make_symbol()
        valid = FReferenceCatalog([symbol], [make_feature(symbol.id)], [], [], [])
        if audit_catalog(valid, root):
            print("自己テスト失敗: 正常な型解決を拒否しました。", file=sys.stderr)
            return 1
        invalid = FReferenceCatalog([symbol], [make_feature(None)], [], [], [])
        errors = audit_catalog(invalid, root)
        if len(errors) != 1 or "一意に解決できません" not in errors[0]:
            print("自己テスト失敗: 未解決型を検出できません。", file=sys.stderr)
            return 1
    print("型機能ページ監査の自己テストに成功しました。")
    return 0


def main() -> int:
    configure_utf8_console()
    arguments = parse_arguments()
    if arguments.self_test:
        return run_self_test()
    root = arguments.root.resolve()
    source = (arguments.source or root / "docs" / "reference" / "source").resolve()
    if not (root / "src").is_dir():
        print(f"ACS sourceがありません: {root / 'src'}", file=sys.stderr)
        return 2
    if not source.is_dir():
        print(f"reference sourceがありません: {source}", file=sys.stderr)
        return 2
    catalog = build_catalog(root, source)
    errors = audit_catalog(catalog, root)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        print(f"型機能ページ監査に失敗しました: {len(errors)}件", file=sys.stderr)
        return 1
    type_features = sum(is_single_type_feature(feature) for feature in catalog.features)
    print(f"型機能ページ監査に成功しました: {type_features}件")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
