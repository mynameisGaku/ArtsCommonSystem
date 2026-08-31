# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import hashlib
import json
import os
import posixpath
import re
import shutil
import stat
import sys
import tempfile
from pathlib import Path, PurePosixPath
from typing import Callable

import generate_reference as _loaded_legacy_reference_parser  # noqa: F401
from reference_site.catalog import build_catalog
from reference_site.renderer import FReferenceRenderer


SCRIPT_PATH = Path(__file__).resolve()
ACS_ROOT_DEFAULT = SCRIPT_PATH.parents[1]
PACKAGE_ROOT = SCRIPT_PATH.parent / "reference_site"
REFERENCE_OUTPUT_MARKER = ".acs-reference-site.json"
REFERENCE_OUTPUT_KIND = "acs-reference-site"
REFERENCE_OUTPUT_SCHEMA = 1
REFERENCE_REQUIRED_FILES = (
    "index.html",
    "search.html",
    "glossary.html",
    "guide.html",
    "troubleshooting.html",
    "assets/css/reference.css",
    "assets/js/reference.js",
)
REFERENCE_REQUIRED_DIRECTORIES = ("features", "modules", "symbols")
STAGING_PREFIX = ".acs-ref-stage-"
BACKUP_PREFIX = ".acs-ref-old-"
RECOVERY_PREFIX = ".acs-ref-recovery-"
SOURCE_SNAPSHOT_PREFIX = ".acs-ref-source-"
BUILD_INPUT_SNAPSHOT_PREFIX = ".acs-ref-input-"
SWITCH_JOURNAL_KIND = "acs-reference-switch"
SWITCH_JOURNAL_SCHEMA = 1
SWITCH_JOURNAL_SUFFIX = ".acs-reference-switch.json"

FDirectoryIdentity = tuple[int, int]
FReferenceOutputIdentity = tuple[int, int, int, str, str, str]


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ACS の機能別・API別リファレンスを生成します。")
    parser.add_argument("--acs-root", type=Path, default=ACS_ROOT_DEFAULT)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check", action="store_true", help="生成済み内容との差分だけを検査します。")
    parser.add_argument(
        "--migrate-legacy-manifest-sha256",
        help="markerがない旧出力を移行するとき、そのmanifest.jsonのSHA-256を指定します。",
    )
    return parser.parse_args()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def build_output_marker() -> bytes:
    marker = {
        "kind": REFERENCE_OUTPUT_KIND,
        "schema": REFERENCE_OUTPUT_SCHEMA,
    }
    return (json.dumps(marker, ensure_ascii=False, sort_keys=True) + "\n").encode("utf-8")


def add_assets(files: dict[str, bytes], assets_root: Path | None = None) -> None:
    root = assets_root or PACKAGE_ROOT / "assets"
    assets = {
        "assets/css/reference.css": root / "reference.css",
        "assets/js/reference.js": root / "reference.js",
    }
    for route, source in assets.items():
        files[route] = source.read_bytes()


def build_manifest(catalog: object, files: dict[str, bytes]) -> bytes:
    manifest = {
        "schema": 2,
        "counts": {
            "modules": len({symbol.module for symbol in catalog.symbols} | {feature.module for feature in catalog.features}),
            "features": len(catalog.features),
            "symbols": len(catalog.symbols),
            "rootSymbols": sum(1 for symbol in catalog.symbols if symbol.parent_id is None),
            "memberSymbols": sum(1 for symbol in catalog.symbols if symbol.parent_id is not None),
            "glossary": len(catalog.glossary),
            "guides": len(catalog.guides),
            "troubleshooting": len(catalog.troubleshooting),
            "files": len(files) + 1,
        },
        "files": {
            route: {"sha256": sha256(content), "bytes": len(content)}
            for route, content in sorted(files.items())
        },
    }
    return (json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


def resolve_link(route: str, href: str) -> str | None:
    if not href or href.startswith(("http://", "https://", "mailto:", "javascript:", "data:")):
        return None
    target = href.split("#", 1)[0].split("?", 1)[0]
    if not target:
        return None
    if target.startswith("/"):
        raise ValueError(f"ルート絶対URLは使用できません: {route} -> {href}")
    return posixpath.normpath(posixpath.join(posixpath.dirname(route), target))


def validate_output(files: dict[str, bytes], output_root: Path, docs_root: Path) -> None:
    available = set(files)
    available_casefold = {route.casefold(): route for route in available}
    if len(available_casefold) != len(available):
        raise ValueError("大文字小文字を区別しない生成経路の衝突があります。")

    link_pattern = re.compile(r'\b(?:href|src|action)="([^"]+)"')
    id_pattern = re.compile(r'\bid="([^"]+)"')
    aria_controls_pattern = re.compile(r'\baria-controls="([^"]+)"')
    image_pattern = re.compile(r"<img\b[^>]*>")
    attribute_pattern = re.compile(r'([A-Za-z_:][-A-Za-z0-9_:.]*)="([^"]*)"')
    forbidden_patterns = {
        "ユーザーの指示": re.compile(r"ユーザーの指示"),
        "エージェント工程": re.compile(r"Codex|AI\s*セッション|次の\s*AI|作業エージェント|別エージェント", re.I),
        "handoff工程": re.compile(r"\bHANDOFF\b|\bhandoff\b", re.I),
    }
    errors: list[str] = []
    html_ids: dict[str, set[str]] = {}
    for route, content in files.items():
        if not route.endswith(".html"):
            continue
        ids = id_pattern.findall(content.decode("utf-8"))
        if len(ids) != len(set(ids)):
            errors.append(f"idが重複しています: {route}")
        html_ids[route] = set(ids)

    resolved_output = output_root.resolve()
    resolved_docs = docs_root.resolve()
    for route, content in files.items():
        if not route.endswith(".html"):
            continue
        text = content.decode("utf-8")
        if '<html lang="ja">' not in text:
            errors.append(f"lang=ja がありません: {route}")
        if 'name="viewport"' not in text:
            errors.append(f"viewport がありません: {route}")
        if not re.search(r"[\u3040-\u30ff\u3400-\u9fff]", text):
            errors.append(f"日本語本文がありません: {route}")
        if "fetch(" in text or 'type="module"' in text:
            errors.append(f"オフライン表示を妨げるscriptがあります: {route}")
        for label, pattern in forbidden_patterns.items():
            if pattern.search(text):
                errors.append(f"ACS外の工程文面 ({label}) があります: {route}")
        for image_match in image_pattern.finditer(text):
            attributes = dict(attribute_pattern.findall(image_match.group(0)))
            alt = attributes.get("alt", "").strip()
            if not alt or not re.search(r"[\u3040-\u30ff\u3400-\u9fff]", alt):
                errors.append(f"画像の日本語altがありません: {route}")
            if not attributes.get("width", "").isdigit() or int(attributes.get("width", "0")) <= 0:
                errors.append(f"画像のwidthが不正です: {route}")
            if not attributes.get("height", "").isdigit() or int(attributes.get("height", "0")) <= 0:
                errors.append(f"画像のheightが不正です: {route}")
            if attributes.get("loading") != "lazy":
                errors.append(f"画像の遅延読み込み指定がありません: {route}")
        for controlled_id in aria_controls_pattern.findall(text):
            if controlled_id not in html_ids[route]:
                errors.append(f"aria-controlsの対象がありません: {route} -> {controlled_id}")
        for href in link_pattern.findall(text):
            fragment = href.split("#", 1)[1].split("?", 1)[0] if "#" in href else ""
            try:
                target = resolve_link(route, href)
            except ValueError as error:
                errors.append(str(error))
                continue
            target_route = route if target is None and fragment else None
            if target:
                target_route = available_casefold.get(target.casefold())
                if target_route is None:
                    physical_target = (resolved_output / Path(*target.split("/"))).resolve()
                    try:
                        physical_target.relative_to(resolved_docs)
                    except ValueError:
                        errors.append(f"リンク先がdocs外です: {route} -> {href} ({physical_target})")
                        continue
                    if not physical_target.is_file():
                        errors.append(f"リンク先がありません: {route} -> {href} ({target})")
                        continue
            if fragment and target_route and target_route.endswith(".html"):
                if fragment not in html_ids.get(target_route, set()):
                    errors.append(f"fragmentがありません: {route} -> {href}")
    if errors:
        preview = "\n".join(errors[:80])
        suffix = f"\nほか {len(errors) - 80} 件" if len(errors) > 80 else ""
        raise ValueError(f"生成物の検査に失敗しました。\n{preview}{suffix}")


def path_exists(path: Path) -> bool:
    return os.path.lexists(path)


def is_reparse_point(path: Path) -> bool:
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return False
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    file_attributes = getattr(metadata, "st_file_attributes", 0)
    return path.is_symlink() or bool(reparse_flag and file_attributes & reparse_flag)


def require_plain_file(path: Path, label: str) -> None:
    if is_reparse_point(path) or not path.is_file():
        raise RuntimeError(f"{label}が通常fileではありません: {path}")


def require_plain_directory(path: Path, label: str) -> None:
    if is_reparse_point(path) or not path.is_dir():
        raise RuntimeError(f"{label}が通常directoryではありません: {path}")


def read_json_object(path: Path, label: str) -> dict[str, object]:
    require_plain_file(path, label)
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise RuntimeError(f"{label}を読み取れません: {path}") from error
    if not isinstance(value, dict):
        raise RuntimeError(f"{label}がJSON objectではありません: {path}")
    return value


def validate_reference_output_root(
    output_root: Path,
    *,
    legacy_manifest_sha256: str | None = None,
) -> None:
    """既存directoryがACS reference siteだけを管理する出力先か検査する。"""
    if not path_exists(output_root):
        raise RuntimeError(f"reference出力先がありません: {output_root}")
    require_plain_directory(output_root, "reference出力先")

    manifest_path = output_root / "manifest.json"
    manifest = read_json_object(manifest_path, "reference manifest")
    if manifest.get("schema") != 2:
        raise RuntimeError(f"reference manifestのschemaが不正です: {manifest_path}")
    manifest_files = manifest.get("files")
    if not isinstance(manifest_files, dict):
        raise RuntimeError(f"reference manifestのfilesが不正です: {manifest_path}")
    counts = manifest.get("counts")
    if not isinstance(counts, dict) or counts.get("files") != len(manifest_files) + 1:
        raise RuntimeError(f"reference manifestのfile件数が不正です: {manifest_path}")

    marker_path = output_root / REFERENCE_OUTPUT_MARKER
    has_marker = path_exists(marker_path)
    if has_marker:
        marker = read_json_object(marker_path, "reference marker")
        if marker.get("kind") != REFERENCE_OUTPUT_KIND or marker.get("schema") != REFERENCE_OUTPUT_SCHEMA:
            raise RuntimeError(f"reference markerが不正です: {marker_path}")
        if REFERENCE_OUTPUT_MARKER not in manifest_files:
            raise RuntimeError(f"reference markerがmanifestに登録されていません: {manifest_path}")
    elif legacy_manifest_sha256 is None:
        raise RuntimeError(f"reference markerがありません: {marker_path}")
    else:
        if not re.fullmatch(r"[0-9a-f]{64}", legacy_manifest_sha256):
            raise RuntimeError("旧reference manifestのSHA-256指定が不正です。")
        actual_manifest_sha256 = sha256(manifest_path.read_bytes())
        if actual_manifest_sha256 != legacy_manifest_sha256:
            raise RuntimeError(
                "旧reference manifestが明示されたSHA-256と一致しません: "
                f"{manifest_path}"
            )
        if REFERENCE_OUTPUT_MARKER in manifest_files:
            raise RuntimeError(f"manifestに登録されたreference markerがありません: {marker_path}")

    for route in REFERENCE_REQUIRED_FILES:
        required_path = output_root.joinpath(*route.split("/"))
        require_plain_file(required_path, f"reference必須file ({route})")
        record = manifest_files.get(route)
        if not isinstance(record, dict):
            raise RuntimeError(f"reference必須fileがmanifestにありません: {route}")
        content = required_path.read_bytes()
        if record.get("bytes") != len(content) or record.get("sha256") != sha256(content):
            raise RuntimeError(f"reference必須fileがmanifestと一致しません: {route}")

    if has_marker:
        marker_record = manifest_files.get(REFERENCE_OUTPUT_MARKER)
        marker_content = marker_path.read_bytes()
        if (
            not isinstance(marker_record, dict)
            or marker_record.get("bytes") != len(marker_content)
            or marker_record.get("sha256") != sha256(marker_content)
        ):
            raise RuntimeError(f"reference markerがmanifestと一致しません: {marker_path}")

    for name in REFERENCE_REQUIRED_DIRECTORIES:
        require_plain_directory(output_root / name, f"reference必須directory ({name})")

    validate_managed_output_tree(output_root, manifest_files)


def route_target(output_root: Path, route: str) -> Path:
    parts = route.split("/")
    posix_route = PurePosixPath(route)
    if (
        not route
        or "\\" in route
        or posix_route.is_absolute()
        or any(part in ("", ".", "..") or ":" in part for part in parts)
    ):
        raise RuntimeError(f"生成経路が不正です: {route}")
    return output_root.joinpath(*parts)


def scan_plain_tree(root: Path, *, skip_top_level: set[str] | None = None) -> tuple[set[str], set[str]]:
    """reparse pointを辿らず、通常fileとdirectoryの相対経路を列挙する。"""
    skipped = skip_top_level or set()
    files: set[str] = set()
    directories: set[str] = set()
    pending: list[tuple[Path, str]] = [(root, "")]
    while pending:
        current, prefix = pending.pop()
        with os.scandir(current) as entries:
            ordered = sorted(entries, key=lambda entry: entry.name.casefold())
        for entry in ordered:
            relative = f"{prefix}/{entry.name}" if prefix else entry.name
            path = Path(entry.path)
            if is_reparse_point(path):
                raise RuntimeError(f"管理対象treeにreparse pointがあります: {path}")
            if entry.is_dir(follow_symlinks=False):
                directories.add(relative)
                if not prefix and entry.name in skipped:
                    continue
                pending.append((path, relative))
            elif entry.is_file(follow_symlinks=False):
                files.add(relative)
            else:
                raise RuntimeError(f"管理対象treeに通常file以外の項目があります: {path}")
    return files, directories


def validate_managed_output_tree(output_root: Path, manifest_files: dict[str, object]) -> None:
    """manifest管理外の項目を拒否し、置換で無関係な内容を失わないようにする。"""
    expected_files = {"manifest.json"}
    expected_directories: set[str] = set()
    folded_routes: set[str] = set()
    for route, record in manifest_files.items():
        if not isinstance(route, str):
            raise RuntimeError("reference manifestに文字列以外の経路があります。")
        if route == "manifest.json" or route == "source" or route.startswith("source/"):
            raise RuntimeError(f"reference manifestに管理外の経路があります: {route}")
        folded = route.casefold()
        if folded in folded_routes:
            raise RuntimeError(f"reference manifestに大文字小文字だけが異なる経路があります: {route}")
        folded_routes.add(folded)
        target = route_target(output_root, route)
        require_plain_file(target, f"reference生成file ({route})")
        if not isinstance(record, dict):
            raise RuntimeError(f"reference manifestのfile情報が不正です: {route}")
        content = target.read_bytes()
        byte_count = record.get("bytes")
        content_hash = record.get("sha256")
        if (
            isinstance(byte_count, bool)
            or not isinstance(byte_count, int)
            or byte_count != len(content)
            or not isinstance(content_hash, str)
            or not re.fullmatch(r"[0-9a-f]{64}", content_hash)
            or content_hash != sha256(content)
        ):
            raise RuntimeError(f"reference生成fileがmanifestと一致しません: {route}")
        expected_files.add(route)
        parent = PurePosixPath(route).parent
        while parent != PurePosixPath("."):
            expected_directories.add(parent.as_posix())
            parent = parent.parent

    actual_files, actual_directories = scan_plain_tree(output_root, skip_top_level={"source"})
    if "source" in actual_directories:
        expected_directories.add("source")
    unexpected_files = sorted(actual_files - expected_files)
    missing_files = sorted(expected_files - actual_files)
    unexpected_directories = sorted(actual_directories - expected_directories)
    missing_directories = sorted(expected_directories - actual_directories)
    differences = [
        *(f"管理外file: {route}" for route in unexpected_files),
        *(f"不足file: {route}" for route in missing_files),
        *(f"管理外directory: {route}" for route in unexpected_directories),
        *(f"不足directory: {route}" for route in missing_directories),
    ]
    if differences:
        preview = "\n".join(differences[:80])
        suffix = f"\nほか {len(differences) - 80} 件" if len(differences) > 80 else ""
        raise RuntimeError(f"reference出力先にmanifest管理外の構造があります。\n{preview}{suffix}")


def reference_source_identity(source_root: Path) -> str:
    """source treeの経路と内容をまとめたidentityを返す。"""
    if not path_exists(source_root):
        return "none"
    require_plain_directory(source_root, "reference source")
    files, directories = scan_plain_tree(source_root)
    digest = hashlib.sha256()
    for route in sorted(directories):
        digest.update(b"D\0")
        digest.update(route.encode("utf-8"))
        digest.update(b"\n")
    for route in sorted(files):
        content = source_root.joinpath(*route.split("/")).read_bytes()
        digest.update(b"F\0")
        digest.update(route.encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(len(content)).encode("ascii"))
        digest.update(b"\0")
        digest.update(hashlib.sha256(content).digest())
        digest.update(b"\n")
    return digest.hexdigest()


def write_files(output_root: Path, files: dict[str, bytes]) -> None:
    """空のstaging directoryだけへ生成fileを書き込む。"""
    if path_exists(output_root):
        require_plain_directory(output_root, "staging出力先")
        if any(output_root.iterdir()):
            raise RuntimeError(f"staging出力先が空ではありません: {output_root}")
    else:
        output_root.mkdir(parents=True)
    for route, content in sorted(files.items()):
        if route == "source" or route.startswith("source/"):
            raise RuntimeError(f"source directoryは生成対象にできません: {route}")
        target = route_target(output_root, route)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)


def check_files(output_root: Path, expected: dict[str, bytes]) -> list[str]:
    actual_routes: set[str] = set()
    if output_root.exists():
        for path in output_root.rglob("*"):
            if not path.is_file():
                continue
            relative = path.relative_to(output_root).as_posix()
            if relative == "source/manifest.json" or relative.startswith("source/"):
                continue
            actual_routes.add(relative)
    expected_routes = set(expected)
    differences: list[str] = []
    for missing in sorted(expected_routes - actual_routes):
        differences.append(f"不足: {missing}")
    for extra in sorted(actual_routes - expected_routes):
        differences.append(f"余分: {extra}")
    for route in sorted(expected_routes & actual_routes):
        actual = (output_root / Path(*route.split("/"))).read_bytes()
        if actual != expected[route]:
            differences.append(f"内容差分: {route}")
    return differences


def reference_output_identity(
    output_root: Path,
    *,
    legacy_manifest_sha256: str | None = None,
) -> tuple[int, int, int, str, str, str]:
    """切替直前の競合を検出するため、管理情報のsnapshotを返す。"""
    validate_reference_output_root(
        output_root,
        legacy_manifest_sha256=legacy_manifest_sha256,
    )
    metadata = output_root.stat()
    marker_path = output_root / REFERENCE_OUTPUT_MARKER
    marker_hash = sha256(marker_path.read_bytes()) if path_exists(marker_path) else "legacy"
    source_hash = reference_source_identity(output_root / "source")
    return (
        metadata.st_dev,
        metadata.st_ino,
        metadata.st_mtime_ns,
        sha256((output_root / "manifest.json").read_bytes()),
        marker_hash,
        source_hash,
    )


def expected_reference_output_content_identity(
    files: dict[str, bytes],
    source_identity: str,
) -> tuple[str, str, str]:
    """rendererが確定した生成内容から、stagingに依存しない期待identityを返す。"""
    manifest = files.get("manifest.json")
    marker = files.get(REFERENCE_OUTPUT_MARKER)
    if manifest is None or marker is None:
        raise RuntimeError("生成予定fileにreference manifestまたはmarkerがありません。")
    return sha256(manifest), sha256(marker), source_identity


def same_reference_output_object(
    identity: FReferenceOutputIdentity,
    expected: FReferenceOutputIdentity,
) -> bool:
    """source更新を許容しつつ、同じdirectory実体と生成siteかを判定する。"""
    return identity[:2] == expected[:2] and identity[3:5] == expected[3:5]


def switch_journal_path(output_root: Path) -> Path:
    return output_root.parent / f".{output_root.name}{SWITCH_JOURNAL_SUFFIX}"


def lock_journal_descriptor(descriptor: int) -> None:
    """process終了で解放される非blocking排他lockをjournalへ設定する。"""
    os.lseek(descriptor, 0, os.SEEK_SET)
    if os.name == "nt":
        import msvcrt

        msvcrt.locking(descriptor, msvcrt.LK_NBLCK, 1)
        return
    import fcntl

    fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)


def read_descriptor_bytes(descriptor: int) -> bytes:
    os.lseek(descriptor, 0, os.SEEK_SET)
    chunks: list[bytes] = []
    while True:
        chunk = os.read(descriptor, 65536)
        if not chunk:
            return b"".join(chunks)
        chunks.append(chunk)


def file_identity_from_descriptor(descriptor: int) -> FDirectoryIdentity:
    metadata = os.fstat(descriptor)
    if not stat.S_ISREG(metadata.st_mode):
        raise RuntimeError("reference切替journalが通常fileではありません。")
    return metadata.st_dev, metadata.st_ino


def create_switch_journal(
    output_root: Path,
    staging_root: Path,
    backup_root: Path | None,
    expected_output_identity: FReferenceOutputIdentity | None,
    expected_staging_identity: FReferenceOutputIdentity,
) -> tuple[Path, int, FDirectoryIdentity]:
    """writer排他lockを兼ねる切替journalを永続化する。"""
    journal_path = switch_journal_path(output_root)
    flags = os.O_CREAT | os.O_EXCL | os.O_RDWR | getattr(os, "O_BINARY", 0)
    descriptor: int | None = None
    created_identity: FDirectoryIdentity | None = None
    try:
        descriptor = os.open(journal_path, flags, 0o600)
        os.write(descriptor, b" ")
        os.fsync(descriptor)
        created_identity = file_identity_from_descriptor(descriptor)
        lock_journal_descriptor(descriptor)
        payload = {
            "kind": SWITCH_JOURNAL_KIND,
            "schema": SWITCH_JOURNAL_SCHEMA,
            "output": output_root.name,
            "staging": staging_root.name,
            "backup": backup_root.name if backup_root is not None else None,
            "oldIdentity": list(expected_output_identity) if expected_output_identity else None,
            "newIdentity": list(expected_staging_identity),
        }
        content = (json.dumps(payload, ensure_ascii=False, sort_keys=True) + "\n").encode("utf-8")
        os.lseek(descriptor, 0, os.SEEK_SET)
        os.ftruncate(descriptor, 0)
        os.write(descriptor, content)
        os.fsync(descriptor)
        return journal_path, descriptor, file_identity_from_descriptor(descriptor)
    except FileExistsError as error:
        raise RuntimeError(
            f"別のreference生成または中断済み切替があります: {journal_path}"
        ) from error
    except BaseException:
        if descriptor is not None:
            os.close(descriptor)
        if created_identity is not None and path_exists(journal_path):
            try:
                remove_owned_journal(journal_path, created_identity)
            except (OSError, RuntimeError):
                pass
        raise


def remove_owned_journal(path: Path, expected_identity: FDirectoryIdentity) -> None:
    if not path_exists(path):
        return
    require_plain_file(path, "reference切替journal")
    metadata = path.stat()
    if (metadata.st_dev, metadata.st_ino) != expected_identity:
        raise RuntimeError(f"reference切替journalの実体が差し替えられました: {path}")
    path.unlink()


def parse_journal_identity(value: object, label: str) -> FReferenceOutputIdentity:
    if not isinstance(value, list) or len(value) != 6:
        raise RuntimeError(f"reference切替journalの{label}が不正です。")
    if any(isinstance(item, bool) or not isinstance(item, int) for item in value[:3]):
        raise RuntimeError(f"reference切替journalの{label}が不正です。")
    if any(not isinstance(item, str) for item in value[3:]):
        raise RuntimeError(f"reference切替journalの{label}が不正です。")
    return value[0], value[1], value[2], value[3], value[4], value[5]


def acquire_interrupted_switch_journal(
    journal_path: Path,
) -> tuple[int, FDirectoryIdentity, dict[str, object]]:
    """active writerがいないjournalだけを復旧用に取得する。"""
    if is_reparse_point(journal_path):
        raise RuntimeError(f"reference切替journalがreparse pointです: {journal_path}")
    flags = os.O_RDWR | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(journal_path, flags)
    try:
        identity = file_identity_from_descriptor(descriptor)
        try:
            lock_journal_descriptor(descriptor)
        except OSError as error:
            raise RuntimeError(
                f"別processがreference出力を切り替えています: {journal_path}"
            ) from error
        try:
            payload = json.loads(read_descriptor_bytes(descriptor).decode("utf-8"))
        except (UnicodeError, json.JSONDecodeError) as error:
            raise RuntimeError(f"reference切替journalを読み取れません: {journal_path}") from error
        if not isinstance(payload, dict):
            raise RuntimeError(f"reference切替journalがJSON objectではありません: {journal_path}")
        return descriptor, identity, payload
    except BaseException:
        os.close(descriptor)
        raise


def create_same_volume_temporary_directory(parent: Path, prefix: str) -> Path:
    temporary = Path(tempfile.mkdtemp(prefix=prefix, dir=parent))
    if temporary.stat().st_dev != parent.stat().st_dev:
        temporary.rmdir()
        raise RuntimeError(f"一時directoryを出力先と同じvolumeに作成できません: {temporary}")
    return temporary


def directory_identity(path: Path) -> FDirectoryIdentity:
    """経路差し替えを検出するため、directory実体のidentityを返す。"""
    require_plain_directory(path, "identity対象directory")
    metadata = path.stat()
    return metadata.st_dev, metadata.st_ino


def reserve_same_volume_temporary_path(parent: Path, prefix: str) -> Path:
    """同一volumeに、rename先としてまだ存在しない一時経路を予約する。"""
    temporary = create_same_volume_temporary_directory(parent, prefix)
    temporary.rmdir()
    return temporary


def remove_owned_temporary_tree(
    path: Path,
    parent: Path,
    prefix: str,
    expected_identity: FDirectoryIdentity,
) -> None:
    if not path_exists(path):
        return
    if path.parent != parent or not path.name.startswith(prefix):
        raise RuntimeError(f"管理外の一時directoryは削除できません: {path}")
    require_plain_directory(path, "管理対象の一時directory")
    if directory_identity(path) != expected_identity:
        raise RuntimeError(f"一時directoryの実体が作成時から変わっています: {path}")
    shutil.rmtree(path)


def copy_plain_tree_into(source_root: Path, destination_root: Path) -> None:
    """reparse pointを追跡せず、検査用の空directoryへtreeを複製する。"""
    require_plain_directory(source_root, "snapshot生成元")
    require_plain_directory(destination_root, "snapshot出力先")
    if any(destination_root.iterdir()):
        raise RuntimeError(f"snapshot出力先が空ではありません: {destination_root}")
    shutil.copytree(source_root, destination_root, dirs_exist_ok=True, symlinks=True)


def create_reference_source_snapshot(
    source_root: Path,
    parent: Path,
) -> tuple[Path, str, FDirectoryIdentity]:
    """live sourceを検査済みの不変入力treeへ複製する。"""
    source_identity = reference_source_identity(source_root)
    if source_identity == "none":
        raise RuntimeError(f"reference sourceがありません: {source_root}")
    snapshot_root = create_same_volume_temporary_directory(parent, SOURCE_SNAPSHOT_PREFIX)
    snapshot_directory_identity = directory_identity(snapshot_root)
    try:
        copy_plain_tree_into(source_root, snapshot_root)
        if reference_source_identity(snapshot_root) != source_identity:
            raise RuntimeError("reference source snapshotの内容が生成元と一致しません。")
        if reference_source_identity(source_root) != source_identity:
            raise RuntimeError("reference sourceがsnapshot作成中に変更されました。")
    except BaseException:
        if path_exists(snapshot_root):
            remove_owned_temporary_tree(
                snapshot_root,
                parent,
                SOURCE_SNAPSHOT_PREFIX,
                snapshot_directory_identity,
            )
        raise
    return snapshot_root, source_identity, snapshot_directory_identity


def reference_build_input_roots(
    acs_root: Path,
    source_root: Path,
    docs_root: Path,
) -> dict[str, Path]:
    """生成結果へ読み込むlive treeを、snapshot内の相対経路へ対応付ける。"""
    return {
        "src": acs_root / "src",
        "source": source_root,
        "docs/media/captures/edited/editor": (
            docs_root / "media" / "captures" / "edited" / "editor"
        ),
        "reference-site-assets": PACKAGE_ROOT / "assets",
    }


def create_reference_build_input_snapshot(
    acs_root: Path,
    source_root: Path,
    docs_root: Path,
    parent: Path,
) -> tuple[Path, dict[str, str], FDirectoryIdentity]:
    """C++宣言、正本、画像、UI資源を同じ不変snapshotへ固定する。"""
    live_roots = reference_build_input_roots(acs_root, source_root, docs_root)
    identities = {
        relative: reference_source_identity(live_root)
        for relative, live_root in live_roots.items()
    }
    missing = [relative for relative, identity in identities.items() if identity == "none"]
    if missing:
        raise RuntimeError(f"reference生成入力がありません: {', '.join(missing)}")

    snapshot_root = create_same_volume_temporary_directory(
        parent,
        BUILD_INPUT_SNAPSHOT_PREFIX,
    )
    snapshot_directory_identity = directory_identity(snapshot_root)
    try:
        for relative, live_root in live_roots.items():
            destination = snapshot_root.joinpath(*relative.split("/"))
            destination.mkdir(parents=True)
            copy_plain_tree_into(live_root, destination)
            if reference_source_identity(destination) != identities[relative]:
                raise RuntimeError(f"reference生成入力を同一内容で複製できません: {relative}")
        for relative, live_root in live_roots.items():
            if reference_source_identity(live_root) != identities[relative]:
                raise RuntimeError(f"snapshot作成中にreference生成入力が変更されました: {relative}")
    except BaseException:
        if path_exists(snapshot_root):
            remove_owned_temporary_tree(
                snapshot_root,
                parent,
                BUILD_INPUT_SNAPSHOT_PREFIX,
                snapshot_directory_identity,
            )
        raise
    return snapshot_root, identities, snapshot_directory_identity


def verify_reference_build_inputs(
    acs_root: Path,
    source_root: Path,
    docs_root: Path,
    expected_identities: dict[str, str],
) -> None:
    """snapshot作成後にlive生成入力が変わっていないことを検査する。"""
    live_roots = reference_build_input_roots(acs_root, source_root, docs_root)
    for relative, expected_identity in expected_identities.items():
        live_root = live_roots.get(relative)
        if live_root is None or reference_source_identity(live_root) != expected_identity:
            raise RuntimeError(f"reference生成中に入力が変更されました: {relative}")


def verify_reference_build_snapshot(
    snapshot_root: Path,
    expected_identities: dict[str, str],
) -> None:
    """生成処理が不変snapshotを変更していないことを検査する。"""
    for relative, expected_identity in expected_identities.items():
        snapshot_input = snapshot_root.joinpath(*relative.split("/"))
        if reference_source_identity(snapshot_input) != expected_identity:
            raise RuntimeError(f"reference生成中にsnapshotが変更されました: {relative}")


def copy_reference_source(
    source: Path,
    staging_root: Path,
    expected_source_identity: str,
) -> str:
    """生成に用いたsource snapshotだけをstagingへ複製する。"""
    source_identity = reference_source_identity(source)
    if source_identity != expected_source_identity:
        raise RuntimeError("reference sourceが生成開始後に変更されました。")
    if source_identity == "none":
        return source_identity
    destination = staging_root / "source"
    shutil.copytree(source, destination, symlinks=True)
    if reference_source_identity(destination) != expected_source_identity:
        raise RuntimeError("reference sourceをstagingへ同一内容で複製できませんでした。")
    if reference_source_identity(source) != expected_source_identity:
        raise RuntimeError("reference sourceが複製中に変更されました。")
    return expected_source_identity


def move_tree_to_recovery(source: Path, parent: Path) -> Path:
    """切替境界で現れたtreeを上書きせず、復元可能な経路へ退避する。"""
    recovery_root = reserve_same_volume_temporary_path(parent, RECOVERY_PREFIX)
    try:
        os.replace(source, recovery_root)
    except BaseException:
        if path_exists(source) or not path_exists(recovery_root):
            raise
    return recovery_root


def restore_reference_backup(
    backup_root: Path,
    output_root: Path,
    *,
    expected_backup_identity: tuple[int, int, int, str, str, str],
    recovery_roots: list[Path],
    legacy_manifest_sha256: str | None,
) -> None:
    """競合treeをすべて保持したまま、既知の旧出力を正規位置へ戻す。"""
    parent = output_root.parent

    for _ in range(3):
        backup_identity = reference_output_identity(
            backup_root,
            legacy_manifest_sha256=legacy_manifest_sha256,
        )
        if not same_reference_output_object(backup_identity, expected_backup_identity):
            raise RuntimeError(f"旧reference出力のbackup経路が差し替えられました: {backup_root}")
        if path_exists(output_root):
            recovery_roots.append(move_tree_to_recovery(output_root, parent))
        backup_identity = reference_output_identity(
            backup_root,
            legacy_manifest_sha256=legacy_manifest_sha256,
        )
        if not same_reference_output_object(backup_identity, expected_backup_identity):
            raise RuntimeError(f"旧reference出力のbackup経路が復元直前に変わりました: {backup_root}")
        try:
            os.replace(backup_root, output_root)
        except BaseException:
            if not path_exists(backup_root) and path_exists(output_root):
                try:
                    restored_identity = reference_output_identity(
                        output_root,
                        legacy_manifest_sha256=legacy_manifest_sha256,
                    )
                except (OSError, RuntimeError):
                    pass
                else:
                    if same_reference_output_object(restored_identity, expected_backup_identity):
                        return
                if path_exists(output_root):
                    recovery_roots.append(move_tree_to_recovery(output_root, parent))
            if path_exists(backup_root):
                continue
            raise
        try:
            restored_identity = reference_output_identity(
                output_root,
                legacy_manifest_sha256=legacy_manifest_sha256,
            )
        except (OSError, RuntimeError):
            if path_exists(output_root):
                recovery_roots.append(move_tree_to_recovery(output_root, parent))
            raise
        if not same_reference_output_object(restored_identity, expected_backup_identity):
            recovery_roots.append(move_tree_to_recovery(output_root, parent))
            raise RuntimeError("復元したreference出力が旧出力と一致しません。")
        return
    raise RuntimeError(
        "競合するreference出力が繰り返し作成されたため、旧出力を復元できません。"
    )


def recover_interrupted_reference_switch(
    output_root: Path,
    *,
    legacy_manifest_sha256: str | None = None,
) -> list[Path]:
    """強制終了で残ったjournalから、完全な旧siteまたは新siteへ復旧する。"""
    output_root = Path(os.path.abspath(output_root))
    parent = output_root.parent.resolve()
    output_root = parent / output_root.name
    journal_path = switch_journal_path(output_root)
    if not path_exists(journal_path):
        return []

    descriptor, journal_identity, payload = acquire_interrupted_switch_journal(journal_path)
    recovery_roots: list[Path] = []
    completed = False
    try:
        if (
            payload.get("kind") != SWITCH_JOURNAL_KIND
            or payload.get("schema") != SWITCH_JOURNAL_SCHEMA
            or payload.get("output") != output_root.name
        ):
            raise RuntimeError(f"reference切替journalの識別情報が不正です: {journal_path}")

        staging_name = payload.get("staging")
        backup_name = payload.get("backup")
        if (
            not isinstance(staging_name, str)
            or Path(staging_name).name != staging_name
            or not staging_name.startswith(STAGING_PREFIX)
        ):
            raise RuntimeError(f"reference切替journalのstaging経路が不正です: {journal_path}")
        staging_root = parent / staging_name
        expected_new = parse_journal_identity(payload.get("newIdentity"), "newIdentity")

        old_value = payload.get("oldIdentity")
        expected_old = (
            parse_journal_identity(old_value, "oldIdentity")
            if old_value is not None
            else None
        )
        if expected_old is None:
            if backup_name is not None:
                raise RuntimeError(f"reference切替journalのbackup情報が不正です: {journal_path}")
            backup_root = None
        else:
            if (
                not isinstance(backup_name, str)
                or Path(backup_name).name != backup_name
                or not backup_name.startswith(BACKUP_PREFIX)
            ):
                raise RuntimeError(f"reference切替journalのbackup経路が不正です: {journal_path}")
            backup_root = parent / backup_name

        output_identity: FReferenceOutputIdentity | None = None
        if path_exists(output_root):
            try:
                output_identity = reference_output_identity(
                    output_root,
                    legacy_manifest_sha256=legacy_manifest_sha256,
                )
            except (OSError, RuntimeError):
                output_identity = None

        output_is_new = (
            output_identity is not None
            and same_reference_output_object(output_identity, expected_new)
            and output_identity[5] == expected_new[5]
        )
        output_is_old = (
            expected_old is not None
            and output_identity is not None
            and same_reference_output_object(output_identity, expected_old)
        )
        if output_is_new:
            if expected_old is not None:
                if backup_root is None or not path_exists(backup_root):
                    raise RuntimeError(
                        f"公開済みreference出力の旧backupがありません: {journal_path}"
                    )
                backup_identity = reference_output_identity(
                    backup_root,
                    legacy_manifest_sha256=legacy_manifest_sha256,
                )
                if not same_reference_output_object(backup_identity, expected_old):
                    raise RuntimeError(
                        f"公開済みreference出力の旧backupが一致しません: {backup_root}"
                    )
                if backup_identity[5] != expected_old[5]:
                    restore_reference_backup(
                        backup_root,
                        output_root,
                        expected_backup_identity=expected_old,
                        recovery_roots=recovery_roots,
                        legacy_manifest_sha256=legacy_manifest_sha256,
                    )
            completed = True
        elif output_is_old:
            completed = True
        elif expected_old is not None and backup_root is not None:
            restore_reference_backup(
                backup_root,
                output_root,
                expected_backup_identity=expected_old,
                recovery_roots=recovery_roots,
                legacy_manifest_sha256=legacy_manifest_sha256,
            )
            completed = True
        elif expected_old is None:
            if path_exists(output_root):
                recovery_roots.append(move_tree_to_recovery(output_root, parent))
            if path_exists(staging_root):
                staging_identity = reference_output_identity(staging_root)
                if (
                    not same_reference_output_object(staging_identity, expected_new)
                    or staging_identity[5] != expected_new[5]
                ):
                    raise RuntimeError(
                        f"中断されたreference stagingがjournalと一致しません: {staging_root}"
                    )
            completed = True
        else:
            raise RuntimeError(
                f"中断されたreference出力を自動復旧できません: {journal_path}"
            )
    finally:
        os.close(descriptor)

    if completed:
        remove_owned_journal(journal_path, journal_identity)
    return recovery_roots


def validate_staged_reference_output(staging_root: Path, expected: dict[str, bytes]) -> None:
    """staging上の構造と全生成fileの内容を切替前に検査する。"""
    validate_reference_output_root(staging_root)
    differences = check_files(staging_root, expected)
    if differences:
        preview = "\n".join(differences[:80])
        suffix = f"\nほか {len(differences) - 80} 件" if len(differences) > 80 else ""
        raise RuntimeError(f"staging生成物の検査に失敗しました。\n{preview}{suffix}")


def switch_reference_output(
    staging_root: Path,
    output_root: Path,
    expected_output_identity: tuple[int, int, int, str, str, str] | None,
    *,
    source_root: Path | None = None,
    expected_source_identity: str | None = None,
    expected_staging_identity: tuple[int, int, int, str, str, str] | None = None,
    pre_publish_check: Callable[[], None] | None = None,
    legacy_manifest_sha256: str | None = None,
) -> Path | None:
    """検証済みtreeだけを公開し、競合treeを削除せず旧出力を復元する。"""
    staging_root = Path(os.path.abspath(staging_root))
    output_root = Path(os.path.abspath(output_root))
    parent = output_root.parent.resolve()
    output_root = parent / output_root.name
    staging_root = staging_root.parent.resolve() / staging_root.name
    require_plain_directory(parent, "reference出力先の親directory")
    require_plain_directory(staging_root, "staging出力先")
    if staging_root.parent != parent or not staging_root.name.startswith(STAGING_PREFIX):
        raise RuntimeError(f"管理外のstaging directoryは切り替えられません: {staging_root}")
    if staging_root.stat().st_dev != parent.stat().st_dev:
        raise RuntimeError(f"staging directoryがreference出力先と同じvolumeにありません: {staging_root}")
    staging_identity = reference_output_identity(staging_root)
    if expected_staging_identity is not None and staging_identity != expected_staging_identity:
        raise RuntimeError("staging出力が最終検査後に変更されました。")

    normalized_source_root = (
        Path(os.path.abspath(source_root)) if source_root is not None else None
    )
    source_is_managed = normalized_source_root == output_root / "source"
    if normalized_source_root is not None and expected_source_identity is not None:
        if reference_source_identity(normalized_source_root) != expected_source_identity:
            raise RuntimeError("切替直前にreference sourceが変更されたため、切替を中止しました。")

    output_exists = path_exists(output_root)
    if expected_output_identity is None:
        if output_exists:
            raise RuntimeError("切替直前にreference出力先が作成されたため、切替を中止しました。")
    else:
        if (
            not output_exists
            or reference_output_identity(
                output_root,
                legacy_manifest_sha256=legacy_manifest_sha256,
            )
            != expected_output_identity
        ):
            raise RuntimeError("切替直前にreference出力先が変更されたため、切替を中止しました。")

    backup_root = (
        reserve_same_volume_temporary_path(parent, BACKUP_PREFIX)
        if output_exists
        else None
    )
    journal_path, journal_descriptor, journal_identity = create_switch_journal(
        output_root,
        staging_root,
        backup_root,
        expected_output_identity,
        staging_identity,
    )
    recovery_roots: list[Path] = []
    published = False
    mutation_started = False
    journal_complete = False
    try:
        # 全量検査は旧siteが正規位置にある間に完了し、二つのrenameを連続させる。
        if reference_output_identity(staging_root) != staging_identity:
            raise RuntimeError("staging出力が公開準備中に変更されました。")
        if pre_publish_check is not None:
            pre_publish_check()
        if (
            normalized_source_root is not None
            and expected_source_identity is not None
            and reference_source_identity(normalized_source_root) != expected_source_identity
        ):
            raise RuntimeError("reference sourceが公開準備中に変更されました。")
        current_output_identity = (
            reference_output_identity(
                output_root,
                legacy_manifest_sha256=legacy_manifest_sha256,
            )
            if path_exists(output_root)
            else None
        )
        if current_output_identity != expected_output_identity:
            raise RuntimeError("reference出力先が公開準備中に変更されました。")

        if output_exists:
            mutation_started = True
            try:
                os.replace(output_root, backup_root)
            except BaseException:
                if path_exists(output_root) or not path_exists(backup_root):
                    raise
                # rename自体が完了してから通知された例外も、復元処理へ渡す。
                raise
            if directory_identity(backup_root) != expected_output_identity[:2]:
                raise RuntimeError("旧reference出力が切替境界で変更されました。")
            if path_exists(output_root):
                raise RuntimeError("旧出力の退避後にreference出力先が作成されました。")

        verification_source = (
            backup_root / "source"
            if source_is_managed and backup_root is not None
            else normalized_source_root
        )

        try:
            mutation_started = True
            os.replace(staging_root, output_root)
        except BaseException:
            if path_exists(staging_root) or not path_exists(output_root):
                raise
            published = True
            if reference_output_identity(output_root) != staging_identity:
                raise
            raise
        published = True
        if reference_output_identity(output_root) != staging_identity:
            raise RuntimeError("公開したreference出力が最終検査済みtreeと一致しません。")
        if expected_source_identity is not None:
            published_source = output_root / "source"
            if reference_source_identity(published_source) != expected_source_identity:
                raise RuntimeError("公開したreference sourceが生成時の内容と一致しません。")
            if (
                verification_source is not None
                and reference_source_identity(verification_source) != expected_source_identity
            ):
                raise RuntimeError("reference sourceが切替中に変更されました。")
        if pre_publish_check is not None:
            pre_publish_check()
        journal_complete = True
    except BaseException as switch_error:
        if not mutation_started:
            journal_complete = True
        if backup_root is not None:
            rollback_required = True
            if not path_exists(backup_root) and path_exists(output_root):
                try:
                    output_identity = reference_output_identity(
                        output_root,
                        legacy_manifest_sha256=legacy_manifest_sha256,
                    )
                except (OSError, RuntimeError):
                    pass
                else:
                    rollback_required = not same_reference_output_object(
                        output_identity,
                        expected_output_identity,
                    )
            if rollback_required:
                try:
                    restore_reference_backup(
                        backup_root,
                        output_root,
                        expected_backup_identity=expected_output_identity,
                        recovery_roots=recovery_roots,
                        legacy_manifest_sha256=legacy_manifest_sha256,
                    )
                    journal_complete = True
                except BaseException as rollback_error:
                    keep_output = False
                    if path_exists(output_root):
                        try:
                            output_identity = reference_output_identity(
                                output_root,
                                legacy_manifest_sha256=legacy_manifest_sha256,
                            )
                        except (OSError, RuntimeError):
                            pass
                        else:
                            keep_output = same_reference_output_object(
                                output_identity,
                                expected_output_identity,
                            )
                        if not keep_output:
                            try:
                                recovery_roots.append(move_tree_to_recovery(output_root, parent))
                            except BaseException as recovery_error:
                                recovery_text = ", ".join(
                                    str(path) for path in recovery_roots
                                ) or "なし"
                                raise RuntimeError(
                                    "reference出力と競合treeの両方を復旧できませんでした。"
                                    f"旧出力: {backup_root} / recovery: {recovery_text}"
                                ) from recovery_error
                    recovery_text = ", ".join(str(path) for path in recovery_roots) or "なし"
                    raise RuntimeError(
                        "reference出力の復元に失敗しました。"
                        f"旧出力: {backup_root} / recovery: {recovery_text}"
                    ) from rollback_error
            else:
                journal_complete = True
        elif expected_output_identity is None:
            if path_exists(output_root):
                try:
                    recovery_roots.append(move_tree_to_recovery(output_root, parent))
                    journal_complete = True
                except BaseException as recovery_error:
                    raise RuntimeError(
                        f"新規reference出力をrecoveryへ退避できませんでした: {output_root}"
                    ) from recovery_error
            elif path_exists(staging_root):
                journal_complete = True
        if recovery_roots:
            recovery_text = ", ".join(str(path) for path in recovery_roots)
            raise RuntimeError(
                f"reference出力の切替を中止し、競合treeを保持しました: {recovery_text}"
            ) from switch_error
        if published:
            raise RuntimeError("公開済みreference出力の切替を中止しました。") from switch_error
        raise
    finally:
        os.close(journal_descriptor)
        if journal_complete:
            try:
                remove_owned_journal(journal_path, journal_identity)
            except (OSError, RuntimeError) as cleanup_error:
                print(
                    f"reference切替journalを削除できませんでした: {journal_path}: {cleanup_error}",
                    file=sys.stderr,
                )

    # 旧sourceを開いたままの処理が切替後に書き戻しても失わないよう、backupは自動削除しない。
    return backup_root


def write_files_atomically(
    output_root: Path,
    files: dict[str, bytes],
    docs_root: Path,
    *,
    source_root: Path | None = None,
    authoritative_source_root: Path | None = None,
    expected_source_identity: str | None = None,
    pre_publish_check: Callable[[], None] | None = None,
    legacy_manifest_sha256: str | None = None,
) -> Path | None:
    """検査済みsourceと完全なstaging生成物だけを出力先へ反映する。"""
    output_root = Path(os.path.abspath(output_root))
    parent = output_root.parent.resolve()
    output_root = parent / output_root.name
    parent.mkdir(parents=True, exist_ok=True)
    require_plain_directory(parent, "reference出力先の親directory")
    recovered_roots = recover_interrupted_reference_switch(
        output_root,
        legacy_manifest_sha256=legacy_manifest_sha256,
    )
    for recovered_root in recovered_roots:
        print(f"中断された競合treeをrecoveryへ保持しました: {recovered_root}")

    initial_identity: tuple[int, int, int, str, str, str] | None = None
    if path_exists(output_root):
        initial_identity = reference_output_identity(
            output_root,
            legacy_manifest_sha256=legacy_manifest_sha256,
        )

    copied_source_root = (
        Path(os.path.abspath(source_root))
        if source_root is not None
        else output_root / "source"
    )
    live_source_root = (
        Path(os.path.abspath(authoritative_source_root))
        if authoritative_source_root is not None
        else copied_source_root
    )
    actual_source_identity = (
        expected_source_identity
        if expected_source_identity is not None
        else reference_source_identity(live_source_root)
    )
    if reference_source_identity(copied_source_root) != actual_source_identity:
        raise RuntimeError("生成用source snapshotが検査済みsourceと一致しません。")
    if reference_source_identity(live_source_root) != actual_source_identity:
        raise RuntimeError("reference sourceが生成開始後に変更されました。")
    if live_source_root == output_root / "source" and initial_identity is not None:
        if actual_source_identity != initial_identity[-1]:
            raise RuntimeError("生成元sourceと既存reference出力のsourceが一致しません。")

    staging_root = create_same_volume_temporary_directory(parent, STAGING_PREFIX)
    staging_directory_identity = directory_identity(staging_root)
    retained_backup: Path | None = None
    switch_started = False
    try:
        write_files(staging_root, files)
        copy_reference_source(copied_source_root, staging_root, actual_source_identity)
        validate_output(files, staging_root, docs_root)
        validate_staged_reference_output(staging_root, files)
        expected_content_identity = expected_reference_output_content_identity(
            files,
            actual_source_identity,
        )
        staging_identity = reference_output_identity(staging_root)
        if (
            staging_identity[:2] != staging_directory_identity
            or staging_identity[3:] != expected_content_identity
        ):
            raise RuntimeError("staging生成物がrendererの確定内容と一致しません。")

        if reference_source_identity(copied_source_root) != actual_source_identity:
            changed_label = (
                "reference source"
                if copied_source_root == live_source_root
                else "source snapshot"
            )
            raise RuntimeError(
                f"staging生成中に{changed_label}が変更されたため、切替を中止しました。"
            )
        if reference_source_identity(live_source_root) != actual_source_identity:
            raise RuntimeError("staging生成中にreference sourceが変更されたため、切替を中止しました。")
        current_identity = (
            reference_output_identity(
                output_root,
                legacy_manifest_sha256=legacy_manifest_sha256,
            )
            if path_exists(output_root)
            else None
        )
        if current_identity != initial_identity:
            raise RuntimeError("staging生成中にreference出力先が変更されたため、切替を中止しました。")
        switch_started = True
        retained_backup = switch_reference_output(
            staging_root,
            output_root,
            initial_identity,
            source_root=live_source_root,
            expected_source_identity=actual_source_identity,
            expected_staging_identity=staging_identity,
            pre_publish_check=pre_publish_check,
            legacy_manifest_sha256=legacy_manifest_sha256,
        )
    finally:
        if not switch_started and path_exists(staging_root):
            try:
                remove_owned_temporary_tree(
                    staging_root,
                    parent,
                    STAGING_PREFIX,
                    staging_directory_identity,
                )
            except (OSError, RuntimeError) as cleanup_error:
                print(f"staging directoryを削除できませんでした: {staging_root}: {cleanup_error}", file=sys.stderr)
        elif switch_started and path_exists(staging_root):
            print(
                f"切替を開始したstaging directoryを復旧用に保持しました: {staging_root}",
                file=sys.stderr,
            )
    return retained_backup


def main() -> int:
    arguments = parse_arguments()
    acs_root = arguments.acs_root.resolve()
    docs_root = acs_root / "docs"
    source_root = (arguments.source or docs_root / "reference" / "source").resolve()
    output_argument = arguments.output or docs_root / "reference"
    absolute_output = Path(os.path.abspath(output_argument))
    output_root = absolute_output.parent.resolve() / absolute_output.name
    if not (acs_root / "src").is_dir():
        print(f"ACS source がありません: {acs_root / 'src'}", file=sys.stderr)
        return 2
    legacy_manifest_sha256 = arguments.migrate_legacy_manifest_sha256
    try:
        recovered_roots = recover_interrupted_reference_switch(
            output_root,
            legacy_manifest_sha256=legacy_manifest_sha256,
        )
    except (OSError, RuntimeError) as error:
        print(f"中断されたreference切替を復旧できませんでした: {error}", file=sys.stderr)
        return 2
    for recovered_root in recovered_roots:
        print(f"中断された競合treeをrecoveryへ保持しました: {recovered_root}")
    if not source_root.is_dir():
        print(f"reference source がありません: {source_root}", file=sys.stderr)
        return 2
    if path_exists(output_root):
        try:
            validate_reference_output_root(
                output_root,
                legacy_manifest_sha256=legacy_manifest_sha256,
            )
        except RuntimeError as error:
            print(f"reference出力先を使用できません: {error}", file=sys.stderr)
            return 2

    output_root.parent.mkdir(parents=True, exist_ok=True)
    try:
        require_plain_directory(output_root.parent, "reference出力先の親directory")
        build_snapshot, build_input_identities, build_snapshot_identity = (
            create_reference_build_input_snapshot(
                acs_root,
                source_root,
                docs_root,
                output_root.parent,
            )
        )
        source_snapshot = build_snapshot / "source"
        source_identity = build_input_identities["source"]
        snapshot_docs_root = build_snapshot / "docs"
        snapshot_assets_root = build_snapshot / "reference-site-assets"
    except (OSError, RuntimeError) as error:
        print(f"reference生成入力snapshotを作成できませんでした: {error}", file=sys.stderr)
        return 2

    result = 2
    try:
        catalog = build_catalog(build_snapshot, source_snapshot)
        renderer = FReferenceRenderer(catalog, snapshot_docs_root)
        files = renderer.render_all()
        add_assets(files, snapshot_assets_root)
        files[REFERENCE_OUTPUT_MARKER] = build_output_marker()
        files["manifest.json"] = build_manifest(catalog, files)
        verify_reference_build_snapshot(build_snapshot, build_input_identities)
        verify_reference_build_inputs(
            acs_root,
            source_root,
            docs_root,
            build_input_identities,
        )

        if arguments.check:
            validate_output(files, output_root, docs_root)
            differences = check_files(output_root, files)
            if reference_source_identity(output_root / "source") != source_identity:
                differences.append("内容差分: source/")
            verify_reference_build_inputs(
                acs_root,
                source_root,
                docs_root,
                build_input_identities,
            )
            if differences:
                print("参照サイトに差分があります。", file=sys.stderr)
                for difference in differences[:100]:
                    print(f"- {difference}", file=sys.stderr)
                if len(differences) > 100:
                    print(f"- ほか {len(differences) - 100} 件", file=sys.stderr)
                result = 1
            else:
                print(f"参照サイトは最新です: {len(files)} ファイル")
                result = 0
        else:
            retained_backup = write_files_atomically(
                output_root,
                files,
                docs_root,
                source_root=source_snapshot,
                authoritative_source_root=source_root,
                expected_source_identity=source_identity,
                pre_publish_check=lambda: verify_reference_build_inputs(
                    acs_root,
                    source_root,
                    docs_root,
                    build_input_identities,
                ),
                legacy_manifest_sha256=legacy_manifest_sha256,
            )
            print(
                "参照サイトを生成しました: "
                f"{len(files)} ファイル / {len(catalog.features)} 機能 / "
                f"{len(catalog.symbols)} API項目 / {len(catalog.glossary)} 用語"
            )
            if retained_backup is not None:
                print(f"旧参照サイトを復元用に保持しました: {retained_backup}")
            result = 0
    except (OSError, RuntimeError, ValueError) as error:
        print(f"参照サイトを生成または検査できませんでした: {error}", file=sys.stderr)
        result = 2
    finally:
        if path_exists(build_snapshot):
            try:
                remove_owned_temporary_tree(
                    build_snapshot,
                    output_root.parent,
                    BUILD_INPUT_SNAPSHOT_PREFIX,
                    build_snapshot_identity,
                )
            except (OSError, RuntimeError) as cleanup_error:
                print(
                    f"生成入力snapshotを削除できませんでした: {build_snapshot}: {cleanup_error}",
                    file=sys.stderr,
                )
                result = 2
    return result


if __name__ == "__main__":
    raise SystemExit(main())
