# SPDX-License-Identifier: MIT

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ACS_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ACS_ROOT / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

from reference_site.catalog import sanitize_acs_prose, strip_rich_text  # noqa: E402
from sanitize_reference_sources import FSourceSanitizer, validate_document  # noqa: E402


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


class ReferenceSourceSanitizerTests(unittest.TestCase):
    def test_rich_text_preserves_cpp_angle_brackets(self) -> None:
        self.assertEqual(strip_rich_text("TArray&lt;T&gt;"), "TArray<T>")
        self.assertEqual(strip_rich_text("TArray<t>"), "TArray<t>")
        self.assertEqual(strip_rich_text("operator&lt;=&gt;"), "operator<=>")
        self.assertEqual(strip_rich_text("<t>アロケータ</t>"), "アロケータ")

    def test_prose_removes_only_phase_token_from_current_contract(self) -> None:
        source = "Wav は RIFF ヘッダ込み（Phase 3）。形式を指定する。"
        self.assertEqual(
            sanitize_acs_prose(source),
            "Wav は RIFF ヘッダ込み。 形式を指定する。",
        )

    def test_code_translates_known_comments_and_preserves_labels(self) -> None:
        sanitizer = FSourceSanitizer()
        source = (
            'const char* endpoint = "https://acs.local/v1"; // server endpoint\n'
            "value = Convert(value); /* Convert value to world space. */\n"
            "value = Root(value); // root\n"
            "value = Draw(value, /*color=*/color);\n"
            "value = Tick(value); // 60fps\n"
            "value = State(value); // → Engaged\n"
            "value += 1; // 値を進める。\n"
        )
        self.assertEqual(
            sanitizer.code(source),
            'const char* endpoint = "https://acs.local/v1";\n'
            "value = Convert(value);\n"
            "value = Root(value); // ルートボーン\n"
            "value = Draw(value, /*color=*/color);\n"
            "value = Tick(value); // 60fps\n"
            "value = State(value); // → Engaged\n"
            "value += 1; // 値を進める。",
        )

    def test_feature_omits_unspecified_usage_fields(self) -> None:
        sanitizer = FSourceSanitizer()
        output = sanitizer.feature({
            "schema": 1,
            "module": {
                "id": "memory",
                "title": "メモリ",
                "description": "ACSのメモリ機能です。",
            },
            "feature": {
                "name": "FDiagnostics",
                "kind": "構造体",
                "header": "memory/Diagnostics.h",
                "summary": "診断値を保持します。",
                "when": "—",
                "members": [
                    {"sig": "usize count", "desc": "件数を保持します。", "when": ""},
                ],
            },
        })
        self.assertNotIn("when", output["feature"])
        self.assertNotIn("when", output["feature"]["members"][0])
        self.assertFalse(validate_document("features/memory/fdiagnostics.json", output))

    def test_reloc_handle_signature_keeps_generation_width(self) -> None:
        sanitizer = FSourceSanitizer()
        output = sanitizer.feature({
            "schema": 1,
            "module": {
                "id": "memory",
                "title": "メモリ",
                "description": "ACSのメモリ機能です。",
            },
            "feature": {
                "name": "FRelocHandle",
                "kind": "構造体",
                "header": "memory/RelocHandle.h",
                "summary": "再配置可能な領域を指します。",
                "members": [{"sig": "u32 index / u32 generation", "desc": "識別値を保持します。"}],
            },
        })
        self.assertEqual(output["feature"]["members"][0]["sig"], "u32 index / u64 generation")

    def test_prose_rewrites_external_comparisons_to_acs_description(self) -> None:
        sanitizer = FSourceSanitizer()
        self.assertEqual(
            sanitizer.prose(
                "DXLib 等で慣れた 0〜255 表記で色を作りたい時。",
                "fallback",
            ),
            "0〜255の整数で色を作るときに使用します。",
        )
        self.assertEqual(
            sanitizer.prose(
                "UE5 級の見た目を狙う 3D 描画。",
                "fallback",
            ),
            "ACSの3D描画で高品質な見た目を構成します。",
        )

    def test_output_validator_rejects_generic_fallback_and_external_process(self) -> None:
        fallback = {
            "schema": 2,
            "term": "FMutex",
            "definition": "ACS で使用する「FMutex」を表す用語です。",
        }
        self.assertTrue(
            any("fallback" in error for error in validate_document("glossary/fmutex.json", fallback))
        )
        external = {
            "schema": 2,
            "term": "確認",
            "definition": "ユーザーの指示で追加したACSの項目です。",
        }
        self.assertTrue(
            any("工程文" in error for error in validate_document("glossary/process.json", external))
        )

    def test_return_description_keeps_meaning(self) -> None:
        sanitizer = FSourceSanitizer()
        self.assertEqual(sanitizer.ret("voice handle"), "voice handle を返します。")
        self.assertEqual(sanitizer.ret("world transform"), "world transform を返します。")

    def test_end_to_end_validates_manifest_and_merges_explicit_terms(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            output = root / "output"
            files = {
                "features/test/ftest.json": {
                    "schema": 1,
                    "module": {
                        "id": "test",
                        "title": "test — ACS 検証",
                        "description": "ACS の検証機能です。",
                    },
                    "feature": {
                        "name": "FTest",
                        "kind": "構造体",
                        "header": "test/FTest.h",
                        "summary": "FTest の状態を保持します。",
                        "when": "検証状態を渡すときに使います。",
                    },
                },
                "guides/01.json": {
                    "schema": 1,
                    "title": "ACS の開始方法",
                    "blocks": [{"p": "ACS を初期化します。"}],
                },
                "troubleshooting/001.json": {
                    "schema": 1,
                    "title": "初期化を確認する",
                    "item": {
                        "q": "ACS を初期化できません。",
                        "tags": ["初期化"],
                        "a": "初期化結果を確認してください。",
                    },
                },
                "glossary/gameframework-a.json": {
                    "schema": 1,
                    "term": "GameFramework",
                    "definition": "ACS のゲーム構築用モジュールです。",
                },
                "glossary/gameframework-b.json": {
                    "schema": 1,
                    "term": "gameframework",
                    "definition": "ACS のシーン機能を提供します。",
                },
            }
            for relative, data in files.items():
                _write_json(source / relative, data)
            _write_json(
                source / "manifest.json",
                {
                    "schema": 1,
                    "featureCount": 1,
                    "guideSectionCount": 1,
                    "troubleshootingCount": 1,
                    "glossaryCount": 2,
                    "files": list(files),
                },
            )
            completed = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPTS / "sanitize_reference_sources.py"),
                    "--source",
                    str(source),
                    "--output",
                    str(output),
                ],
                check=False,
                capture_output=True,
                text=True,
                encoding="utf-8",
                env={**os.environ, "PYTHONUTF8": "1"},
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(set(manifest), {"schema", "counts", "files"})
            self.assertEqual(manifest["counts"]["glossary"], 1)
            self.assertNotIn("mergedGlossarySources", manifest)
            glossary_files = list((output / "glossary").glob("*.json"))
            self.assertEqual(len(glossary_files), 1)
            glossary = json.loads(glossary_files[0].read_text(encoding="utf-8"))
            self.assertEqual(glossary["term"], "GameFramework")

    def test_end_to_end_rejects_unregistered_normalized_term_collision(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            output = root / "output"
            files = {
                "glossary/custom-a.json": {
                    "schema": 1,
                    "term": "CustomTerm",
                    "definition": "ACSの検証用語です。",
                },
                "glossary/custom-b.json": {
                    "schema": 1,
                    "term": "customterm",
                    "definition": "ACSの別の検証用語です。",
                },
            }
            for relative, data in files.items():
                _write_json(source / relative, data)
            _write_json(
                source / "manifest.json",
                {
                    "schema": 1,
                    "featureCount": 0,
                    "guideSectionCount": 0,
                    "troubleshootingCount": 0,
                    "glossaryCount": 2,
                    "files": list(files),
                },
            )
            completed = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPTS / "sanitize_reference_sources.py"),
                    "--source",
                    str(source),
                    "--output",
                    str(output),
                ],
                check=False,
                capture_output=True,
                text=True,
                encoding="utf-8",
                env={**os.environ, "PYTHONUTF8": "1"},
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("glossaryの正規化衝突", completed.stderr)


if __name__ == "__main__":
    unittest.main()
