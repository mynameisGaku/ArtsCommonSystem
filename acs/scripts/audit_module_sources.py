#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""各 ``src/<module>/Module.cmake`` と compile source の対応を監査する。

acsbuild の assembled 形式は CMake 変数と条件付き ``list(APPEND ...)`` を使うため、
従来の ``--check`` では厳密比較されない。この監査は形式に依存せず、module 配下の
全 ``.cpp`` が manifest に現れることと、literal な登録先が実在することを検証する。
header は internal / public の分類が別契約なので意図的に対象外とする。
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys
import tempfile
from typing import Iterable, Mapping, Sequence


EXCLUDED_DIRECTORY_NAMES = frozenset(
    {
        ".git",
        ".vs",
        "Binaries",
        "Intermediate",
        "Saved",
        "ThirdParty",
        "third_party",
        "x64",
    }
)

# editor_abi は通常 module ではない。EditorAbi.cpp は engine 本体、
# GameReflectShim.cpp は editor が生成する game project CMake から組み込む。
ALTERNATE_MANIFESTS: Mapping[str, tuple[str, ...]] = {
    "editor_abi": (
        "engine/CMakeLists.txt",
        "editor/AcsEditor/ProjectManager.cs",
    ),
}


def configure_utf8_console() -> None:
    """Windows の既定 code page に依存せず、日本語診断を UTF-8 で出力する。"""

    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="strict")


@dataclass(frozen=True)
class FModuleSourceViolation:
    """manifest と filesystem の不一致。"""

    manifest_path: Path
    kind: str
    source_path: str

    def format(self, root: Path) -> str:
        try:
            display_manifest = self.manifest_path.relative_to(root).as_posix()
        except ValueError:
            display_manifest = self.manifest_path.as_posix()

        if self.kind == "missing":
            detail = f"compile source が未登録です: {self.source_path}"
        elif self.kind == "stale":
            detail = f"登録された compile source が存在しません: {self.source_path}"
        elif self.kind == "missing_manifest":
            detail = f"Module.cmake も代替 manifest もありません: {self.source_path}"
        else:
            detail = f"代替 manifest に compile source が未登録です: {self.source_path}"
        return (
            f"{display_manifest}: error: [module-source-manifest] {detail}"
        )


def strip_cmake_comments(text: str) -> str:
    """生成済み Module.cmake の行末 comment を文字列の外側だけ除く。"""

    output: list[str] = []
    for line in text.splitlines():
        quote = ""
        escaped = False
        kept: list[str] = []
        for character in line:
            if escaped:
                kept.append(character)
                escaped = False
                continue
            if character == "\\" and quote:
                kept.append(character)
                escaped = True
                continue
            if quote:
                kept.append(character)
                if character == quote:
                    quote = ""
                continue
            if character in ('"', "'"):
                quote = character
                kept.append(character)
                continue
            if character == "#":
                break
            kept.append(character)
        output.append("".join(kept))
    return "\n".join(output)


def extract_literal_cpp_entries(text: str) -> set[str]:
    """CMake token から module 相対の literal ``.cpp`` path だけを取り出す。"""

    entries: set[str] = set()
    uncommented = strip_cmake_comments(text)
    for raw_token in re.findall(r"[^\s()]+", uncommented):
        for item in raw_token.split(";"):
            token = item.strip().strip("\"'")
            if not token.lower().endswith(".cpp"):
                continue
            # FetchContent 等の外部 source は filesystem inventory の対象外。
            if "$" in token:
                continue
            normalized = token.replace("\\", "/")
            if re.match(r"^[A-Za-z]:/", normalized) or normalized.startswith("/"):
                continue
            entries.add(normalized)
    return entries


def iter_cpp_files(directory: Path) -> Iterable[Path]:
    """除外 directory を避けて compile source を決定的順序で列挙する。"""

    paths = (
        path
        for path in directory.rglob("*")
        if path.is_file()
        and path.suffix.lower() == ".cpp"
        and not any(part in EXCLUDED_DIRECTORY_NAMES for part in path.parts)
    )
    return sorted(paths, key=lambda path: path.as_posix())


def audit_module(
    root: Path,
    module_directory: Path,
    manifest_path: Path,
) -> list[FModuleSourceViolation]:
    """通常 module 1 件の未登録と stale 登録を検出する。"""

    text = manifest_path.read_text(encoding="utf-8")
    entries = extract_literal_cpp_entries(text)
    actual = {
        path.relative_to(module_directory).as_posix()
        for path in iter_cpp_files(module_directory)
    }

    violations = [
        FModuleSourceViolation(manifest_path, "missing", path)
        for path in sorted(actual - entries)
    ]
    for entry in sorted(entries):
        candidate = (module_directory / entry).resolve()
        # module 外の明示 source は existence だけを検証する。
        if not candidate.is_file():
            violations.append(
                FModuleSourceViolation(manifest_path, "stale", entry)
            )
    return violations


def audit_alternate_manifest(
    root: Path,
    module_directory: Path,
    manifest_names: Sequence[str],
) -> list[FModuleSourceViolation]:
    """Module.cmake を持たない明示 allowlist directory を検証する。"""

    manifest_paths = tuple(root / name for name in manifest_names)
    missing_manifests = [path for path in manifest_paths if not path.is_file()]
    marker = manifest_paths[0] if manifest_paths else module_directory
    if missing_manifests:
        return [
            FModuleSourceViolation(
                marker,
                "missing_manifest",
                path.relative_to(root).as_posix(),
            )
            for path in missing_manifests
        ]

    combined = "\n".join(
        path.read_text(encoding="utf-8") for path in manifest_paths
    )
    violations: list[FModuleSourceViolation] = []
    for source_path in iter_cpp_files(module_directory):
        relative_to_root = source_path.relative_to(root).as_posix()
        if (
            relative_to_root not in combined
            and source_path.name not in combined
        ):
            violations.append(
                FModuleSourceViolation(
                    marker,
                    "alternate_missing",
                    relative_to_root,
                )
            )
    return violations


def audit_root(
    root: Path,
    alternate_manifests: Mapping[str, tuple[str, ...]] = ALTERNATE_MANIFESTS,
) -> tuple[list[FModuleSourceViolation], int, int]:
    """全 source directory を監査し、違反・module 数・source 数を返す。"""

    source_root = root / "src"
    if not source_root.is_dir():
        raise ValueError(f"src directory が存在しません: {source_root}")

    violations: list[FModuleSourceViolation] = []
    module_count = 0
    source_count = 0
    for module_directory in sorted(
        (path for path in source_root.iterdir() if path.is_dir()),
        key=lambda path: path.name,
    ):
        sources = tuple(iter_cpp_files(module_directory))
        if not sources:
            continue
        source_count += len(sources)
        manifest_path = module_directory / "Module.cmake"
        if manifest_path.is_file():
            module_count += 1
            violations.extend(
                audit_module(root, module_directory, manifest_path)
            )
            continue

        alternate = alternate_manifests.get(module_directory.name)
        if alternate is not None:
            module_count += 1
            violations.extend(
                audit_alternate_manifest(
                    root, module_directory, alternate
                )
            )
            continue

        violations.append(
            FModuleSourceViolation(
                manifest_path,
                "missing_manifest",
                module_directory.relative_to(root).as_posix(),
            )
        )
    return violations, module_count, source_count


def run_self_test() -> bool:
    """assembled list、comment、stale、代替 manifest の回帰ケースを検証する。"""

    with tempfile.TemporaryDirectory(prefix="acs-module-audit-") as temp:
        root = Path(temp)
        module = root / "src" / "demo"
        (module / "backend").mkdir(parents=True)
        (module / "Main.cpp").write_text("// main\n", encoding="utf-8")
        (module / "backend" / "Win.cpp").write_text(
            "// backend\n", encoding="utf-8"
        )
        manifest = module / "Module.cmake"
        manifest.write_text(
            """
set(_demo_sources
    Main.cpp
)
if(WIN32)
    list(APPEND _demo_sources backend/Win.cpp)
endif()
# CommentOnly.cpp
list(APPEND _third_party ${external_SOURCE_DIR}/ThirdParty.cpp)
acs_module(NAME Demo SOURCES ${_demo_sources})
""",
            encoding="utf-8",
        )

        violations, modules, sources = audit_root(
            root, alternate_manifests={}
        )
        if violations or modules != 1 or sources != 2:
            return False

        manifest.write_text(
            """
set(_demo_sources
    Main.cpp
    Gone.cpp
)
# backend/Win.cpp は comment なので登録ではない。
acs_module(NAME Demo SOURCES ${_demo_sources})
""",
            encoding="utf-8",
        )
        violations, _, _ = audit_root(root, alternate_manifests={})
        kinds = sorted(
            (violation.kind, violation.source_path)
            for violation in violations
        )
        if kinds != [
            ("missing", "backend/Win.cpp"),
            ("stale", "Gone.cpp"),
        ]:
            return False

        alternate = root / "src" / "bridge"
        alternate.mkdir(parents=True)
        (alternate / "Shim.cpp").write_text("// shim\n", encoding="utf-8")
        (root / "engine").mkdir()
        (root / "engine" / "CMakeLists.txt").write_text(
            "target_sources(bridge PRIVATE ../src/bridge/Shim.cpp)\n",
            encoding="utf-8",
        )
        violations, modules, sources = audit_root(
            root,
            alternate_manifests={
                "bridge": ("engine/CMakeLists.txt",),
            },
        )
        return (
            kinds
            == [
                ("missing", "backend/Win.cpp"),
                ("stale", "Gone.cpp"),
            ]
            and len(violations) == 2
            and modules == 2
            and sources == 3
        )


def main(arguments: Sequence[str] | None = None) -> int:
    """command line entry point。"""

    configure_utf8_console()
    parser = argparse.ArgumentParser(
        description="Module.cmake の compile source 登録を監査する"
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="src/ と engine/ を含む ACS tree root",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="監査器自身の回帰 test を実行する",
    )
    options = parser.parse_args(arguments)

    if options.self_test:
        if not run_self_test():
            print("module source manifest 監査 self-test: FAILED", file=sys.stderr)
            return 1
        print("module source manifest 監査 self-test: PASS")
        return 0

    root = options.root.resolve()
    try:
        violations, module_count, source_count = audit_root(root)
    except (OSError, UnicodeError, ValueError) as error:
        print(
            f"module source manifest 監査を開始できません: {error}",
            file=sys.stderr,
        )
        return 2

    for violation in violations:
        print(violation.format(root), file=sys.stderr)
    if violations:
        print(
            f"module source manifest 監査: FAILED "
            f"({len(violations)} violations)",
            file=sys.stderr,
        )
        return 1

    print(
        f"module source manifest 監査: PASS "
        f"({module_count} source directories / {source_count} .cpp)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
