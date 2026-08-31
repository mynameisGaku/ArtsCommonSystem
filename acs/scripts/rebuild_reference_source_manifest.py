# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import hashlib
import json
import os
import tempfile
from collections import Counter
from pathlib import Path


SCRIPT_PATH = Path(__file__).resolve()
ACS_ROOT_DEFAULT = SCRIPT_PATH.parents[1]
SUPPORTED_CATEGORIES = {"features", "guides", "troubleshooting", "glossary"}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ACSリファレンス正本のmanifestを再構築します。")
    parser.add_argument(
        "--source",
        type=Path,
        default=ACS_ROOT_DEFAULT / "docs" / "reference" / "source",
    )
    parser.add_argument("--check", action="store_true", help="既存manifestとの差分だけを検査します。")
    return parser.parse_args()


def stable_json(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=False) + "\n").encode("utf-8")


def build_manifest(source_root: Path) -> bytes:
    counts: Counter[str] = Counter()
    files: dict[str, dict[str, object]] = {}
    for path in sorted(source_root.rglob("*.json")):
        if path.name == "manifest.json":
            continue
        relative = path.relative_to(source_root)
        category = relative.parts[0] if relative.parts else ""
        if category not in SUPPORTED_CATEGORIES:
            raise ValueError(f"未対応のcategoryです: {relative.as_posix()}")
        content = path.read_bytes()
        data = json.loads(content.decode("utf-8"))
        if not isinstance(data, dict) or data.get("schema") != 2:
            raise ValueError(f"schema 2のJSONではありません: {relative.as_posix()}")
        route = relative.as_posix()
        files[route] = {
            "sha256": hashlib.sha256(content).hexdigest(),
            "bytes": len(content),
        }
        counts[category] += 1
    manifest = {
        "schema": 2,
        "counts": dict(sorted(counts.items())),
        "files": files,
    }
    return stable_json(manifest)


def write_atomic(path: Path, content: bytes) -> None:
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=".manifest-",
        suffix=".json.tmp",
        dir=path.parent,
    )
    temporary_path = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_path, path)
    except BaseException:
        if temporary_path.exists():
            temporary_path.unlink()
        raise


def main() -> int:
    arguments = parse_arguments()
    source_root = arguments.source.resolve()
    if not source_root.is_dir():
        print(f"ACSリファレンス正本がありません: {source_root}")
        return 2
    manifest_path = source_root / "manifest.json"
    expected = build_manifest(source_root)
    if arguments.check:
        actual = manifest_path.read_bytes() if manifest_path.is_file() else b""
        if actual != expected:
            print("ACSリファレンス正本のmanifestに差分があります。")
            return 1
        print("ACSリファレンス正本のmanifestは最新です。")
        return 0
    write_atomic(manifest_path, expected)
    data = json.loads(expected.decode("utf-8"))
    print(
        "ACSリファレンス正本のmanifestを更新しました: "
        f"{len(data['files'])}ファイル / 用語 {data['counts'].get('glossary', 0)}件"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
