# SPDX-License-Identifier: MIT

from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ACS_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_ROOT = ACS_ROOT / "scripts"

sys.path.insert(0, str(SCRIPTS_ROOT))

import generate_reference_site as reference_generator  # noqa: E402


def _sha256(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def _html(version: str) -> bytes:
    return (
        '<!doctype html><html lang="ja"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width, initial-scale=1">'
        f"<title>ACS {version}</title></head><body>ACS 参照 {version}</body></html>\n"
    ).encode("utf-8")


def _manifest(files: dict[str, bytes]) -> bytes:
    value = {
        "schema": 2,
        "counts": {"files": len(files) + 1},
        "files": {
            route: {"bytes": len(content), "sha256": _sha256(content)}
            for route, content in sorted(files.items())
        },
    }
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


def _site_files(version: str, *, marker: bool = True) -> dict[str, bytes]:
    page = _html(version)
    files = {
        "index.html": page,
        "search.html": page,
        "glossary.html": page,
        "guide.html": page,
        "troubleshooting.html": page,
        "assets/css/reference.css": b"/* ACS */\n",
        "assets/js/reference.js": b"// ACS\n",
        "features/sample.html": page,
        "modules/sample.html": page,
        "symbols/sample.html": page,
    }
    if marker:
        files[reference_generator.REFERENCE_OUTPUT_MARKER] = reference_generator.build_output_marker()
    files["manifest.json"] = _manifest(files)
    return files


def _write_site(
    root: Path,
    version: str,
    *,
    source_files: dict[str, bytes] | None = None,
    marker: bool = True,
) -> None:
    reference_generator.write_files(root, _site_files(version, marker=marker))
    for route, content in (source_files or {}).items():
        target = root / "source" / Path(*route.split("/"))
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)


def _tree_snapshot(root: Path) -> tuple[tuple[str, ...], dict[str, bytes]]:
    directories = tuple(
        sorted(path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_dir())
    )
    files = {
        path.relative_to(root).as_posix(): path.read_bytes()
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }
    return directories, files


def _temporary_entries(parent: Path) -> set[str]:
    return {
        path.name
        for path in parent.iterdir()
        if path.name.startswith(
            (reference_generator.STAGING_PREFIX, reference_generator.BACKUP_PREFIX)
        )
    }


def _entries_with_prefix(parent: Path, prefix: str) -> list[Path]:
    return sorted(path for path in parent.iterdir() if path.name.startswith(prefix))


class ReferenceSiteAtomicGenerationTests(unittest.TestCase):
    def test_unrelated_output_and_similar_sibling_are_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "unrelated"
            output.mkdir()
            (output / "keep.txt").write_bytes(b"unrelated output")
            similar_sibling = parent / f"{reference_generator.BACKUP_PREFIX}unrelated"
            similar_sibling.mkdir()
            (similar_sibling / "keep.txt").write_bytes(b"similar sibling")
            output_before = _tree_snapshot(output)
            sibling_before = _tree_snapshot(similar_sibling)
            temporary_before = _temporary_entries(parent)

            with self.assertRaisesRegex(RuntimeError, "reference manifest"):
                reference_generator.write_files_atomically(
                    output,
                    _site_files("new"),
                    parent / "docs",
                )

            self.assertEqual(output_before, _tree_snapshot(output))
            self.assertEqual(sibling_before, _tree_snapshot(similar_sibling))
            self.assertEqual(temporary_before, _temporary_entries(parent))

    def test_manifest_unmanaged_directory_is_rejected_without_deletion(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"
            _write_site(output, "old")
            unmanaged = output / "private" / "keep.txt"
            unmanaged.parent.mkdir()
            unmanaged.write_bytes(b"not managed by the reference manifest")
            before = _tree_snapshot(output)

            with self.assertRaisesRegex(RuntimeError, "manifest管理外"):
                reference_generator.write_files_atomically(
                    output,
                    _site_files("new"),
                    parent / "docs",
                )

            self.assertEqual(before, _tree_snapshot(output))
            self.assertTrue(unmanaged.is_file())
            self.assertEqual(set(), _temporary_entries(parent))

    def test_invalid_marker_and_required_structure_are_rejected(self) -> None:
        mutations = (
            (
                "marker",
                lambda root: (root / reference_generator.REFERENCE_OUTPUT_MARKER).write_text(
                    '{"kind":"other","schema":1}\n', encoding="utf-8"
                ),
                "reference markerが不正",
            ),
            (
                "structure",
                lambda root: shutil.rmtree(root / "symbols"),
                "reference必須directory",
            ),
            (
                "managed-file",
                lambda root: (root / "features" / "sample.html").write_bytes(b"tampered"),
                "manifestと一致",
            ),
        )
        for label, mutate, expected_message in mutations:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                parent = Path(temporary)
                output = parent / "reference"
                _write_site(output, "old")
                mutate(output)
                before = _tree_snapshot(output)

                with self.assertRaisesRegex(RuntimeError, expected_message):
                    reference_generator.write_files_atomically(
                        output,
                        _site_files("new"),
                        parent / "docs",
                    )

                self.assertEqual(before, _tree_snapshot(output))
                self.assertEqual(set(), _temporary_entries(parent))

    def test_staging_without_marker_never_becomes_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"

            with self.assertRaisesRegex(RuntimeError, "reference markerがありません"):
                reference_generator.write_files_atomically(
                    output,
                    _site_files("new", marker=False),
                    parent / "docs",
                )

            self.assertFalse(output.exists())
            self.assertEqual(set(), _temporary_entries(parent))

    def test_output_created_at_switch_boundary_is_not_replaced(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary).resolve()
            output = parent / "reference"
            staging = parent / f"{reference_generator.STAGING_PREFIX}test"
            _write_site(staging, "new")
            _write_site(output, "concurrent")
            output_before = _tree_snapshot(output)
            staging_before = _tree_snapshot(staging)

            with self.assertRaisesRegex(RuntimeError, "切替直前にreference出力先が作成"):
                reference_generator.switch_reference_output(staging, output, None)

            self.assertEqual(output_before, _tree_snapshot(output))
            self.assertEqual(staging_before, _tree_snapshot(staging))

    def test_exception_before_switch_preserves_existing_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"
            _write_site(output, "old", source_files={"features/source.json": b"old source"})
            before = _tree_snapshot(output)

            with mock.patch.object(
                reference_generator,
                "validate_staged_reference_output",
                side_effect=RuntimeError("injected staging failure"),
            ):
                with self.assertRaisesRegex(RuntimeError, "injected staging failure"):
                    reference_generator.write_files_atomically(
                        output,
                        _site_files("new"),
                        parent / "docs",
                    )

            self.assertEqual(before, _tree_snapshot(output))
            self.assertEqual(set(), _temporary_entries(parent))

    def test_switch_exception_rolls_back_existing_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"
            _write_site(output, "old", source_files={"nested/source.bin": b"source"})
            before = _tree_snapshot(output)
            original_replace = reference_generator.os.replace

            def fail_staging_switch(source: os.PathLike[str], destination: os.PathLike[str]) -> None:
                source_path = Path(source)
                destination_path = Path(destination)
                if (
                    source_path.name.startswith(reference_generator.STAGING_PREFIX)
                    and destination_path == output
                ):
                    raise OSError("injected switch failure")
                original_replace(source, destination)

            with mock.patch.object(reference_generator.os, "replace", side_effect=fail_staging_switch):
                with self.assertRaisesRegex(OSError, "injected switch failure"):
                    reference_generator.write_files_atomically(
                        output,
                        _site_files("new"),
                        parent / "docs",
                    )

            self.assertEqual(before, _tree_snapshot(output))
            retained_staging = _entries_with_prefix(parent, reference_generator.STAGING_PREFIX)
            self.assertEqual(1, len(retained_staging))
            self.assertIn(b"ACS new", (retained_staging[0] / "index.html").read_bytes())
            self.assertEqual([], _entries_with_prefix(parent, reference_generator.BACKUP_PREFIX))

    def test_exception_after_staging_rename_rolls_back_existing_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"
            _write_site(output, "old", source_files={"nested/source.bin": b"source"})
            before = _tree_snapshot(output)
            original_replace = reference_generator.os.replace

            def interrupt_after_staging_switch(
                source: os.PathLike[str], destination: os.PathLike[str]
            ) -> None:
                source_path = Path(source)
                destination_path = Path(destination)
                original_replace(source, destination)
                if (
                    source_path.name.startswith(reference_generator.STAGING_PREFIX)
                    and destination_path == output
                ):
                    raise KeyboardInterrupt("injected interrupt after switch")

            with mock.patch.object(
                reference_generator.os,
                "replace",
                side_effect=interrupt_after_staging_switch,
            ):
                with self.assertRaisesRegex(RuntimeError, "競合treeを保持"):
                    reference_generator.write_files_atomically(
                        output,
                        _site_files("new"),
                        parent / "docs",
                    )

            self.assertEqual(before, _tree_snapshot(output))
            self.assertEqual(set(), _temporary_entries(parent))
            recovery_roots = _entries_with_prefix(parent, reference_generator.RECOVERY_PREFIX)
            self.assertEqual(1, len(recovery_roots))
            self.assertIn(b"ACS new", (recovery_roots[0] / "index.html").read_bytes())

    def test_exception_after_existing_output_rename_restores_existing_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"
            _write_site(output, "old", source_files={"nested/source.bin": b"source"})
            before = _tree_snapshot(output)
            original_replace = reference_generator.os.replace

            def interrupt_after_existing_output_move(
                source: os.PathLike[str], destination: os.PathLike[str]
            ) -> None:
                source_path = Path(source)
                destination_path = Path(destination)
                original_replace(source, destination)
                if source_path == output and destination_path.name.startswith(
                    reference_generator.BACKUP_PREFIX
                ):
                    raise KeyboardInterrupt("injected interrupt after existing output move")

            with mock.patch.object(
                reference_generator.os,
                "replace",
                side_effect=interrupt_after_existing_output_move,
            ):
                with self.assertRaisesRegex(
                    KeyboardInterrupt,
                    "injected interrupt after existing output move",
                ):
                    reference_generator.write_files_atomically(
                        output,
                        _site_files("new"),
                        parent / "docs",
                    )

            self.assertEqual(before, _tree_snapshot(output))
            retained_staging = _entries_with_prefix(parent, reference_generator.STAGING_PREFIX)
            self.assertEqual(1, len(retained_staging))
            self.assertIn(b"ACS new", (retained_staging[0] / "index.html").read_bytes())
            self.assertEqual([], _entries_with_prefix(parent, reference_generator.BACKUP_PREFIX))

    def test_successful_switch_preserves_source_tree_exactly(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"
            source_files = {
                "manifest.json": b'{"schema":2}\n',
                "features/a.json": "ACS の機能\n".encode("utf-8"),
                "binary/value.bin": bytes(range(32)),
            }
            _write_site(output, "old", source_files=source_files)
            source_before = _tree_snapshot(output / "source")
            similar_sibling = parent / f"{reference_generator.BACKUP_PREFIX}unrelated"
            similar_sibling.mkdir()
            (similar_sibling / "keep.txt").write_bytes(b"unrelated")
            sibling_before = _tree_snapshot(similar_sibling)
            temporary_before = _temporary_entries(parent)

            retained_backup = reference_generator.write_files_atomically(
                output,
                _site_files("new"),
                parent / "docs",
            )

            self.assertIn(b"ACS new", (output / "index.html").read_bytes())
            self.assertEqual(source_before, _tree_snapshot(output / "source"))
            self.assertEqual(sibling_before, _tree_snapshot(similar_sibling))
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            self.assertFalse(any(route.startswith("source/") for route in manifest["files"]))
            self.assertIsNotNone(retained_backup)
            assert retained_backup is not None
            self.assertEqual(source_before, _tree_snapshot(retained_backup / "source"))
            self.assertIn(b"ACS old", (retained_backup / "index.html").read_bytes())
            self.assertEqual(
                temporary_before | {retained_backup.name},
                _temporary_entries(parent),
            )

    def test_concurrent_source_change_aborts_and_keeps_new_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"
            source_path = output / "source" / "features" / "item.json"
            _write_site(output, "old", source_files={"features/item.json": b"source before"})
            original_copy = reference_generator.copy_reference_source

            def copy_then_change(
                source_root: Path,
                staging_root: Path,
                expected_source_identity: str,
            ) -> str:
                identity = original_copy(
                    source_root,
                    staging_root,
                    expected_source_identity,
                )
                source_path.write_bytes(b"source updated concurrently")
                return identity

            with mock.patch.object(
                reference_generator,
                "copy_reference_source",
                side_effect=copy_then_change,
            ):
                with self.assertRaisesRegex(RuntimeError, "reference sourceが変更"):
                    reference_generator.write_files_atomically(
                        output,
                        _site_files("new"),
                        parent / "docs",
                    )

            self.assertIn(b"ACS old", (output / "index.html").read_bytes())
            self.assertEqual(b"source updated concurrently", source_path.read_bytes())
            self.assertEqual(set(), _temporary_entries(parent))

    def test_source_snapshot_remains_immutable_when_live_source_changes_and_reverts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            source = parent / "reference-source"
            source.mkdir()
            source_file = source / "item.json"
            source_file.write_bytes(b"source before")

            snapshot, source_identity, snapshot_identity = (
                reference_generator.create_reference_source_snapshot(
                    source,
                    parent,
                )
            )
            try:
                source_file.write_bytes(b"source changed temporarily")
                self.assertEqual(b"source before", (snapshot / "item.json").read_bytes())
                self.assertEqual(
                    source_identity,
                    reference_generator.reference_source_identity(snapshot),
                )

                source_file.write_bytes(b"source before")
                self.assertEqual(
                    source_identity,
                    reference_generator.reference_source_identity(source),
                )
                self.assertEqual(b"source before", (snapshot / "item.json").read_bytes())
            finally:
                reference_generator.remove_owned_temporary_tree(
                    snapshot,
                    parent,
                    reference_generator.SOURCE_SNAPSHOT_PREFIX,
                    snapshot_identity,
                )

            self.assertEqual(
                [],
                _entries_with_prefix(parent, reference_generator.SOURCE_SNAPSHOT_PREFIX),
            )

    def test_source_update_after_publish_is_restored_without_data_loss(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"
            source_route = "nested/source.bin"
            _write_site(output, "old", source_files={source_route: b"source before"})
            original_replace = reference_generator.os.replace
            backup_root: Path | None = None

            def update_source_after_publish(
                source: os.PathLike[str],
                destination: os.PathLike[str],
            ) -> None:
                nonlocal backup_root
                source_path = Path(source)
                destination_path = Path(destination)
                original_replace(source, destination)
                if source_path == output and destination_path.name.startswith(
                    reference_generator.BACKUP_PREFIX
                ):
                    backup_root = destination_path
                elif (
                    source_path.name.startswith(reference_generator.STAGING_PREFIX)
                    and destination_path == output
                ):
                    assert backup_root is not None
                    (backup_root / "source" / source_route).write_bytes(
                        b"source updated after publish"
                    )
                    raise OSError("injected source update after publish")

            with mock.patch.object(
                reference_generator.os,
                "replace",
                side_effect=update_source_after_publish,
            ):
                with self.assertRaisesRegex(RuntimeError, "競合treeを保持"):
                    reference_generator.write_files_atomically(
                        output,
                        _site_files("new"),
                        parent / "docs",
                    )

            self.assertIn(b"ACS old", (output / "index.html").read_bytes())
            self.assertEqual(
                b"source updated after publish",
                (output / "source" / source_route).read_bytes(),
            )
            recovery_roots = _entries_with_prefix(parent, reference_generator.RECOVERY_PREFIX)
            self.assertEqual(1, len(recovery_roots))
            self.assertIn(b"ACS new", (recovery_roots[0] / "index.html").read_bytes())
            self.assertEqual(
                b"source before",
                (recovery_roots[0] / "source" / source_route).read_bytes(),
            )

    def test_concurrent_output_after_backup_move_is_preserved_in_recovery(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"
            _write_site(output, "old", source_files={"item.json": b"source"})
            original_replace = reference_generator.os.replace

            def create_concurrent_output(
                source: os.PathLike[str],
                destination: os.PathLike[str],
            ) -> None:
                source_path = Path(source)
                destination_path = Path(destination)
                original_replace(source, destination)
                if source_path == output and destination_path.name.startswith(
                    reference_generator.BACKUP_PREFIX
                ):
                    _write_site(
                        output,
                        "concurrent",
                        source_files={"item.json": b"concurrent source"},
                    )

            with mock.patch.object(
                reference_generator.os,
                "replace",
                side_effect=create_concurrent_output,
            ):
                with self.assertRaisesRegex(RuntimeError, "競合treeを保持"):
                    reference_generator.write_files_atomically(
                        output,
                        _site_files("new"),
                        parent / "docs",
                    )

            self.assertIn(b"ACS old", (output / "index.html").read_bytes())
            recovery_roots = _entries_with_prefix(parent, reference_generator.RECOVERY_PREFIX)
            self.assertEqual(1, len(recovery_roots))
            self.assertIn(b"ACS concurrent", (recovery_roots[0] / "index.html").read_bytes())
            retained_staging = _entries_with_prefix(parent, reference_generator.STAGING_PREFIX)
            self.assertEqual(1, len(retained_staging))
            self.assertIn(b"ACS new", (retained_staging[0] / "index.html").read_bytes())

    def test_staging_change_after_validation_is_rejected_and_retained(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"
            _write_site(output, "old", source_files={"item.json": b"source"})
            before = _tree_snapshot(output)
            original_switch = reference_generator.switch_reference_output

            def mutate_then_switch(*args: object, **kwargs: object) -> Path | None:
                staging_root = Path(args[0])
                (staging_root / "source" / "item.json").write_bytes(b"tampered staging")
                return original_switch(*args, **kwargs)

            with mock.patch.object(
                reference_generator,
                "switch_reference_output",
                side_effect=mutate_then_switch,
            ):
                with self.assertRaisesRegex(RuntimeError, "staging出力が最終検査後に変更"):
                    reference_generator.write_files_atomically(
                        output,
                        _site_files("new"),
                        parent / "docs",
                    )

            self.assertEqual(before, _tree_snapshot(output))
            retained_staging = _entries_with_prefix(parent, reference_generator.STAGING_PREFIX)
            self.assertEqual(1, len(retained_staging))
            self.assertEqual(
                b"tampered staging",
                (retained_staging[0] / "source" / "item.json").read_bytes(),
            )

    def test_generation_input_guard_aborts_before_output_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"
            _write_site(output, "old", source_files={"item.json": b"source"})
            before = _tree_snapshot(output)

            def reject_changed_input() -> None:
                raise RuntimeError("injected generation input change")

            with self.assertRaisesRegex(RuntimeError, "injected generation input change"):
                reference_generator.write_files_atomically(
                    output,
                    _site_files("new"),
                    parent / "docs",
                    pre_publish_check=reject_changed_input,
                )

            self.assertEqual(before, _tree_snapshot(output))
            self.assertFalse(reference_generator.switch_journal_path(output).exists())
            retained_staging = _entries_with_prefix(parent, reference_generator.STAGING_PREFIX)
            self.assertEqual(1, len(retained_staging))
            self.assertIn(b"ACS new", (retained_staging[0] / "index.html").read_bytes())

    def test_generation_input_change_after_publish_rolls_back_new_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"
            _write_site(output, "old", source_files={"item.json": b"source"})
            before = _tree_snapshot(output)
            checks = 0

            def reject_second_check() -> None:
                nonlocal checks
                checks += 1
                if checks == 2:
                    raise RuntimeError("injected post-publish input change")

            with self.assertRaisesRegex(RuntimeError, "競合treeを保持"):
                reference_generator.write_files_atomically(
                    output,
                    _site_files("new"),
                    parent / "docs",
                    pre_publish_check=reject_second_check,
                )

            self.assertEqual(2, checks)
            self.assertEqual(before, _tree_snapshot(output))
            self.assertFalse(reference_generator.switch_journal_path(output).exists())
            recovery_roots = _entries_with_prefix(parent, reference_generator.RECOVERY_PREFIX)
            self.assertEqual(1, len(recovery_roots))
            self.assertIn(b"ACS new", (recovery_roots[0] / "index.html").read_bytes())

    def test_staging_cannot_redefine_expected_content_after_validation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"
            _write_site(output, "old", source_files={"item.json": b"source"})
            before = _tree_snapshot(output)
            original_validate = reference_generator.validate_staged_reference_output

            def validate_then_substitute(
                staging_root: Path,
                expected: dict[str, bytes],
            ) -> None:
                original_validate(staging_root, expected)
                for route, content in _site_files("substituted").items():
                    target = staging_root / Path(*route.split("/"))
                    target.write_bytes(content)

            with mock.patch.object(
                reference_generator,
                "validate_staged_reference_output",
                side_effect=validate_then_substitute,
            ):
                with self.assertRaisesRegex(RuntimeError, "rendererの確定内容と一致"):
                    reference_generator.write_files_atomically(
                        output,
                        _site_files("new"),
                        parent / "docs",
                    )

            self.assertEqual(before, _tree_snapshot(output))
            self.assertEqual([], _entries_with_prefix(parent, reference_generator.STAGING_PREFIX))

    def test_cleanup_rejects_replaced_temporary_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            owned = reference_generator.create_same_volume_temporary_directory(
                parent,
                reference_generator.STAGING_PREFIX,
            )
            owned_identity = reference_generator.directory_identity(owned)
            (owned / "generated.txt").write_bytes(b"generated")
            moved_owned = parent / "moved-owned"
            os.replace(owned, moved_owned)
            owned.mkdir()
            (owned / "concurrent.txt").write_bytes(b"concurrent")

            with self.assertRaisesRegex(RuntimeError, "実体が作成時から変わって"):
                reference_generator.remove_owned_temporary_tree(
                    owned,
                    parent,
                    reference_generator.STAGING_PREFIX,
                    owned_identity,
                )

            self.assertEqual(b"generated", (moved_owned / "generated.txt").read_bytes())
            self.assertEqual(b"concurrent", (owned / "concurrent.txt").read_bytes())

    def test_backup_path_swap_during_rollback_never_publishes_impostor(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"
            _write_site(output, "old", source_files={"item.json": b"old source"})
            original_replace = reference_generator.os.replace
            original_identity = reference_generator.reference_output_identity
            moved_old = parent / "moved-old"
            backup_identity_reads = 0

            def fail_publish(source: os.PathLike[str], destination: os.PathLike[str]) -> None:
                source_path = Path(source)
                destination_path = Path(destination)
                if (
                    source_path.name.startswith(reference_generator.STAGING_PREFIX)
                    and destination_path == output
                ):
                    raise OSError("injected publish failure")
                original_replace(source, destination)

            def swap_backup_after_identity(
                root: Path,
                *,
                legacy_manifest_sha256: str | None = None,
            ) -> tuple[int, int, int, str, str, str]:
                nonlocal backup_identity_reads
                identity = original_identity(
                    root,
                    legacy_manifest_sha256=legacy_manifest_sha256,
                )
                if root.name.startswith(reference_generator.BACKUP_PREFIX):
                    backup_identity_reads += 1
                    if backup_identity_reads == 2:
                        original_replace(root, moved_old)
                        _write_site(
                            root,
                            "impostor",
                            source_files={"item.json": b"impostor source"},
                        )
                return identity

            with mock.patch.object(
                reference_generator.os,
                "replace",
                side_effect=fail_publish,
            ), mock.patch.object(
                reference_generator,
                "reference_output_identity",
                side_effect=swap_backup_after_identity,
            ):
                with self.assertRaisesRegex(RuntimeError, "reference出力の復元に失敗") as raised:
                    reference_generator.write_files_atomically(
                        output,
                        _site_files("new"),
                        parent / "docs",
                    )

            self.assertFalse(output.exists())
            self.assertIn(b"ACS old", (moved_old / "index.html").read_bytes())
            recovery_roots = _entries_with_prefix(parent, reference_generator.RECOVERY_PREFIX)
            self.assertEqual(1, len(recovery_roots))
            self.assertIn(b"ACS impostor", (recovery_roots[0] / "index.html").read_bytes())
            self.assertIn(str(recovery_roots[0]), str(raised.exception))

    def test_build_input_snapshot_keeps_all_generation_inputs_immutable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            acs_root = parent / "acs"
            docs_root = acs_root / "docs"
            source = parent / "reference-source"
            src_file = acs_root / "src" / "sample" / "Type.h"
            image_file = (
                docs_root
                / "media"
                / "captures"
                / "edited"
                / "editor"
                / "sample.png"
            )
            src_file.parent.mkdir(parents=True)
            image_file.parent.mkdir(parents=True)
            source.mkdir()
            src_file.write_bytes(b"struct FBefore {};")
            image_file.write_bytes(b"image before")
            (source / "item.json").write_bytes(b"source before")

            snapshot, identities, snapshot_identity = (
                reference_generator.create_reference_build_input_snapshot(
                    acs_root,
                    source,
                    docs_root,
                    parent,
                )
            )
            try:
                src_file.write_bytes(b"struct FChanged {};")
                image_file.write_bytes(b"image changed")
                self.assertEqual(
                    b"struct FBefore {};",
                    (snapshot / "src" / "sample" / "Type.h").read_bytes(),
                )
                self.assertEqual(
                    b"image before",
                    (
                        snapshot
                        / "docs"
                        / "media"
                        / "captures"
                        / "edited"
                        / "editor"
                        / "sample.png"
                    ).read_bytes(),
                )

                src_file.write_bytes(b"struct FBefore {};")
                image_file.write_bytes(b"image before")
                reference_generator.verify_reference_build_inputs(
                    acs_root,
                    source,
                    docs_root,
                    identities,
                )
                reference_generator.verify_reference_build_snapshot(snapshot, identities)
            finally:
                reference_generator.remove_owned_temporary_tree(
                    snapshot,
                    parent,
                    reference_generator.BUILD_INPUT_SNAPSHOT_PREFIX,
                    snapshot_identity,
                )

    def test_external_source_snapshot_is_copied_and_bound_to_generation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"
            source = parent / "reference-source"
            source.mkdir()
            (source / "item.json").write_bytes(b"external source")
            source_identity = reference_generator.reference_source_identity(source)
            _write_site(output, "old")

            retained_backup = reference_generator.write_files_atomically(
                output,
                _site_files("new"),
                parent / "docs",
                source_root=source,
                expected_source_identity=source_identity,
            )

            self.assertEqual(b"external source", (output / "source" / "item.json").read_bytes())
            self.assertIsNotNone(retained_backup)

    def test_markerless_output_requires_exact_legacy_manifest_hash(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "reference"
            _write_site(output, "legacy", marker=False)
            manifest_hash = _sha256((output / "manifest.json").read_bytes())
            before = _tree_snapshot(output)

            with self.assertRaisesRegex(RuntimeError, "reference markerがありません"):
                reference_generator.write_files_atomically(
                    output,
                    _site_files("new"),
                    parent / "docs",
                )
            with self.assertRaisesRegex(RuntimeError, "SHA-256と一致しません"):
                reference_generator.write_files_atomically(
                    output,
                    _site_files("new"),
                    parent / "docs",
                    legacy_manifest_sha256="0" * 64,
                )

            retained_backup = reference_generator.write_files_atomically(
                output,
                _site_files("new"),
                parent / "docs",
                legacy_manifest_sha256=manifest_hash,
            )

            self.assertIn(b"ACS new", (output / "index.html").read_bytes())
            self.assertIsNotNone(retained_backup)
            assert retained_backup is not None
            self.assertEqual(before, _tree_snapshot(retained_backup))

    def test_active_switch_journal_rejects_second_writer_without_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary).resolve()
            output = parent / "reference"
            staging = reference_generator.create_same_volume_temporary_directory(
                parent,
                reference_generator.STAGING_PREFIX,
            )
            _write_site(output, "old")
            _write_site(staging, "prepared")
            before = _tree_snapshot(output)
            backup = reference_generator.reserve_same_volume_temporary_path(
                parent,
                reference_generator.BACKUP_PREFIX,
            )
            journal, descriptor, journal_identity = reference_generator.create_switch_journal(
                output,
                staging,
                backup,
                reference_generator.reference_output_identity(output),
                reference_generator.reference_output_identity(staging),
            )
            try:
                with self.assertRaisesRegex(RuntimeError, "別processがreference出力を切り替え"):
                    reference_generator.write_files_atomically(
                        output,
                        _site_files("new"),
                        parent / "docs",
                    )
            finally:
                os.close(descriptor)
                reference_generator.remove_owned_journal(journal, journal_identity)

            self.assertEqual(before, _tree_snapshot(output))

    def test_interrupted_switch_restores_old_output_from_journal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary).resolve()
            output = parent / "reference"
            staging = reference_generator.create_same_volume_temporary_directory(
                parent,
                reference_generator.STAGING_PREFIX,
            )
            _write_site(output, "old")
            _write_site(staging, "prepared")
            old_identity = reference_generator.reference_output_identity(output)
            new_identity = reference_generator.reference_output_identity(staging)
            backup = reference_generator.reserve_same_volume_temporary_path(
                parent,
                reference_generator.BACKUP_PREFIX,
            )
            journal, descriptor, _ = reference_generator.create_switch_journal(
                output,
                staging,
                backup,
                old_identity,
                new_identity,
            )
            os.close(descriptor)
            os.replace(output, backup)

            recovery_roots = reference_generator.recover_interrupted_reference_switch(output)

            self.assertEqual([], recovery_roots)
            self.assertIn(b"ACS old", (output / "index.html").read_bytes())
            self.assertTrue(staging.is_dir())
            self.assertFalse(backup.exists())
            self.assertFalse(journal.exists())

    def test_interrupted_switch_accepts_fully_published_new_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary).resolve()
            output = parent / "reference"
            staging = reference_generator.create_same_volume_temporary_directory(
                parent,
                reference_generator.STAGING_PREFIX,
            )
            _write_site(output, "old")
            _write_site(staging, "new")
            old_identity = reference_generator.reference_output_identity(output)
            new_identity = reference_generator.reference_output_identity(staging)
            backup = reference_generator.reserve_same_volume_temporary_path(
                parent,
                reference_generator.BACKUP_PREFIX,
            )
            journal, descriptor, _ = reference_generator.create_switch_journal(
                output,
                staging,
                backup,
                old_identity,
                new_identity,
            )
            os.close(descriptor)
            os.replace(output, backup)
            os.replace(staging, output)

            recovery_roots = reference_generator.recover_interrupted_reference_switch(output)

            self.assertEqual([], recovery_roots)
            self.assertIn(b"ACS new", (output / "index.html").read_bytes())
            self.assertIn(b"ACS old", (backup / "index.html").read_bytes())
            self.assertFalse(journal.exists())

    def test_interrupted_switch_restores_source_updated_after_publish(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary).resolve()
            output = parent / "reference"
            staging = reference_generator.create_same_volume_temporary_directory(
                parent,
                reference_generator.STAGING_PREFIX,
            )
            source_route = "nested/source.bin"
            _write_site(output, "old", source_files={source_route: b"source before"})
            _write_site(staging, "new", source_files={source_route: b"source before"})
            old_identity = reference_generator.reference_output_identity(output)
            new_identity = reference_generator.reference_output_identity(staging)
            backup = reference_generator.reserve_same_volume_temporary_path(
                parent,
                reference_generator.BACKUP_PREFIX,
            )
            journal, descriptor, _ = reference_generator.create_switch_journal(
                output,
                staging,
                backup,
                old_identity,
                new_identity,
            )
            os.close(descriptor)
            os.replace(output, backup)
            os.replace(staging, output)
            (backup / "source" / source_route).write_bytes(b"source updated after publish")

            recovery_roots = reference_generator.recover_interrupted_reference_switch(output)

            self.assertEqual(1, len(recovery_roots))
            self.assertIn(b"ACS old", (output / "index.html").read_bytes())
            self.assertEqual(
                b"source updated after publish",
                (output / "source" / source_route).read_bytes(),
            )
            self.assertIn(b"ACS new", (recovery_roots[0] / "index.html").read_bytes())
            self.assertFalse(journal.exists())

    @unittest.skipUnless(os.name == "nt", "Windowsのdirectory切替を検査します。")
    def test_windows_switch_uses_renames_within_one_volume(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary).resolve()
            output = parent / "reference"
            _write_site(output, "old")
            original_replace = reference_generator.os.replace
            replacements: list[tuple[Path, Path, int, int]] = []

            def record_replace(source: os.PathLike[str], destination: os.PathLike[str]) -> None:
                source_path = Path(source)
                destination_path = Path(destination)
                replacements.append(
                    (
                        source_path,
                        destination_path,
                        source_path.stat().st_dev,
                        destination_path.parent.stat().st_dev,
                    )
                )
                original_replace(source, destination)

            with mock.patch.object(reference_generator.os, "replace", side_effect=record_replace):
                retained_backup = reference_generator.write_files_atomically(
                    output,
                    _site_files("new"),
                    parent / "docs",
                )

            self.assertEqual(2, len(replacements))
            for source, destination, source_device, destination_device in replacements:
                self.assertEqual(parent, source.parent)
                self.assertEqual(parent, destination.parent)
                self.assertEqual(source_device, destination_device)
            self.assertTrue(replacements[0][1].name.startswith(reference_generator.BACKUP_PREFIX))
            self.assertTrue(replacements[1][0].name.startswith(reference_generator.STAGING_PREFIX))
            self.assertEqual(output, replacements[1][1])
            self.assertIsNotNone(retained_backup)
            assert retained_backup is not None
            self.assertEqual({retained_backup.name}, _temporary_entries(parent))


if __name__ == "__main__":
    unittest.main()
