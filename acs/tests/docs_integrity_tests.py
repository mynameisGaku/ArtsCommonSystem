# SPDX-License-Identifier: MIT

from __future__ import annotations

import hashlib
import json
import re
import unittest
import unicodedata
from pathlib import Path
from urllib.parse import unquote


ACS_ROOT = Path(__file__).resolve().parents[1]
DOCS_ROOT = ACS_ROOT / "docs"
JAPANESE_PATTERN = re.compile(r"[ぁ-んァ-ヶ一-龠々〆ヵヶ]")
MARKDOWN_LINK_PATTERN = re.compile(r"(!?)\[([^\]]*)\]\(([^)]+)\)")
FORBIDDEN_PROCESS_PATTERN = re.compile(
    r"CEDEC|Codex|OneDrive|ユーザーの指示|(?:作業|別)エージェント|引用しました|出典|移行中",
    flags=re.I,
)
LEGACY_DOC_PATTERN = re.compile(
    r"(?:REFERENCE\.html|RECIPES\.md|TUTORIALS\.md|API_REFERENCE\.md|"
    r"GETTING_STARTED\.md|QUICKSTART\.md|TROUBLESHOOTING\.md|ARCHITECTURE\.md|"
    r"StyleGuide\.md|NodeUnification\.md|SerializationSafety\.md|"
    r"LearningSamplesMigrationPlan\.md|reference/data)",
)
ALLOWED_TOP_LEVEL = {
    "README.md",
    "architecture",
    "decisions",
    "getting-started",
    "guides",
    "media",
    "operations",
    "reference",
    "safety",
    "standards",
    "tutorials",
    "validation",
}


def _link_target(raw_target: str) -> str:
    target = raw_target.strip()
    if target.startswith("<") and ">" in target:
        return target[1:target.index(">")]
    return target.split(maxsplit=1)[0]


class DocsIntegrityTests(unittest.TestCase):
    def test_top_level_information_architecture_is_explicit(self) -> None:
        actual = {path.name for path in DOCS_ROOT.iterdir()}
        self.assertEqual(actual, ALLOWED_TOP_LEVEL)

    def test_markdown_local_links_exist(self) -> None:
        failures: list[str] = []
        for document in sorted(DOCS_ROOT.rglob("*.md")):
            text = document.read_text(encoding="utf-8")
            for _image, _label, raw_target in MARKDOWN_LINK_PATTERN.findall(text):
                target = unquote(_link_target(raw_target)).replace("\\", "/")
                if not target or target.startswith(("#", "http://", "https://", "mailto:", "data:")):
                    continue
                path_text = target.split("#", 1)[0].split("?", 1)[0]
                if not path_text:
                    continue
                resolved = (document.parent / path_text).resolve()
                if not resolved.exists():
                    failures.append(f"{document.relative_to(ACS_ROOT).as_posix()}: {target}")
        self.assertFalse(failures, "存在しない文書リンク:\n" + "\n".join(failures[:100]))

    def test_markdown_images_have_japanese_alt_text(self) -> None:
        failures: list[str] = []
        for document in sorted(DOCS_ROOT.rglob("*.md")):
            text = document.read_text(encoding="utf-8")
            for image, label, target in MARKDOWN_LINK_PATTERN.findall(text):
                if not image:
                    continue
                if not JAPANESE_PATTERN.search(label):
                    failures.append(
                        f"{document.relative_to(ACS_ROOT).as_posix()}: {target}"
                    )
        self.assertFalse(failures, "日本語の代替説明がない画像:\n" + "\n".join(failures))

    def test_handwritten_docs_contain_only_acs_document_context(self) -> None:
        failures: list[str] = []
        for document in sorted(DOCS_ROOT.rglob("*.md")):
            text = document.read_text(encoding="utf-8")
            for match in FORBIDDEN_PROCESS_PATTERN.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                failures.append(
                    f"{document.relative_to(ACS_ROOT).as_posix()}:{line}: {match.group(0)}"
                )
            for match in LEGACY_DOC_PATTERN.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                failures.append(
                    f"{document.relative_to(ACS_ROOT).as_posix()}:{line}: {match.group(0)}"
                )
        self.assertFalse(failures, "ACS文書に残った工程文または旧参照:\n" + "\n".join(failures))

    def test_reference_source_manifest_matches_every_source_file(self) -> None:
        source_root = DOCS_ROOT / "reference" / "source"
        manifest = json.loads((source_root / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(set(manifest), {"schema", "counts", "files"})
        self.assertEqual(manifest["schema"], 2)
        actual = {
            path.relative_to(source_root).as_posix(): path
            for path in source_root.rglob("*.json")
            if path.name != "manifest.json"
        }
        self.assertEqual(set(manifest["files"]), set(actual))
        counts: dict[str, int] = {}
        for route, path in actual.items():
            content = path.read_bytes()
            entry = manifest["files"][route]
            self.assertEqual(entry["bytes"], len(content), route)
            self.assertEqual(entry["sha256"], hashlib.sha256(content).hexdigest(), route)
            category = route.split("/", 1)[0]
            counts[category] = counts.get(category, 0) + 1
        self.assertEqual(manifest["counts"], dict(sorted(counts.items())))
        self.assertEqual(manifest["counts"]["glossary"], 669)

        normalized_terms: dict[str, str] = {}
        for path in sorted((source_root / "glossary").glob("*.json")):
            data = json.loads(path.read_text(encoding="utf-8"))
            key = unicodedata.normalize("NFKC", data["term"]).casefold()
            self.assertNotIn(key, normalized_terms, f"用語が重複しています: {normalized_terms.get(key)} / {path}")
            normalized_terms[key] = path.name

    def test_reference_source_keeps_verified_api_contracts(self) -> None:
        source_root = DOCS_ROOT / "reference" / "source" / "features"

        def feature(relative: str) -> dict[str, object]:
            data = json.loads((source_root / relative).read_text(encoding="utf-8"))
            return data["feature"]

        asset_kind = feature("editor/eassetkind-fe8c2cd777.json")
        texture = next(member for member in asset_kind["members"] if member["sig"] == "Texture = 1")
        self.assertIn(".png", texture["desc"])
        self.assertIn(".hdr", texture["desc"])

        script_host = feature("gameframework/cscripthost-dbab9e8ff8.json")
        register = next(
            member for member in script_host["members"]
            if member["sig"] == "void RegisterStandardBindings()"
        )
        self.assertIn("現在は native function を登録しません", register["desc"])

        tutorial = feature("gameframework/ctutorialflow-d361aa5a63.json")
        tick = next(member for member in tutorial["members"] if member["sig"] == "void Tick(f32 dt)")
        self.assertIn("状態を変更しません", tick["desc"])

        reloc = feature("memory/frelochandle-d7cb5aa3bb.json")
        self.assertTrue(any(member["sig"] == "u32 index / u64 generation" for member in reloc["members"]))
        valid = next(member for member in reloc["members"] if member["sig"] == "bool IsValid() const")
        self.assertIn("generationが0以外", valid["desc"])

        diagnostics = feature("memory/farenaallocatordiagnostics-0dd853f43f.json")
        self.assertNotIn("when", diagnostics)
        self.assertNotIn("失敗件数", json.dumps(diagnostics, ensure_ascii=False))


if __name__ == "__main__":
    unittest.main()
