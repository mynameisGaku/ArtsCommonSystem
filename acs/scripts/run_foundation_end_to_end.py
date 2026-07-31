#!/usr/bin/env python3
"""Debug/Release build・全CTest・基盤性能契約を一つのJSON証跡へまとめる。"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Callable, Sequence


REPORT_SCHEMA_VERSION = 4
ARTIFACT_TREE_DIGEST_ALGORITHM = "sha256-path-size-content-v1"
SOURCE_EVIDENCE_MODE = "start-endpoint-v1"
ARTIFACT_TREE_EVIDENCE_MODE = "stable-double-scan-no-reparse-v1"
CMAKE_EVIDENCE_KEYS = (
    "CMAKE_HOME_DIRECTORY",
    "ACS_RENDER_DX12_RAW",
    "ACS_RENDER_DILIGENT",
    "ACS_Render_DILIGENT",
    "ACS_BUILD_TESTS",
    "ACS_BUILD_TOOLS",
    "ACS_BUILD_SAMPLES",
    "ACS_ENABLE_DISTRIBUTION_CONSUMER_SMOKE",
    "ACS_DISTRIBUTION_CONSUMER_ROOT",
)


@dataclass(frozen=True)
class StepResult:
    """一つの検証段階について、再現に必要な最小情報を保持する。"""

    name: str
    status: str
    return_code: int
    duration_ms: int
    command: list[str]
    stdout_tail: str
    stderr_tail: str


@dataclass(frozen=True)
class SourceEvidence:
    """検証対象sourceと比較基点をcommit単位で固定する。"""

    root: str
    git_sha: str
    base_ref: str
    base_sha: str
    tracked_dirty: bool
    untracked_files: list[str]
    ahead_count: int
    behind_count: int


@dataclass(frozen=True)
class CMakeEvidence:
    """build treeの構成とcache bytesを固定する。"""

    cache_path: str
    cache_sha256: str
    values: dict[str, str]


@dataclass(frozen=True)
class ArtifactEvidence:
    """実際に検証したartifactのpath、size、digestを固定する。"""

    path: str
    size: int
    sha256: str


@dataclass(frozen=True)
class ArtifactTreeEvidence:
    """配布directory全体のfile集合、総size、digestを固定する。"""

    path: str
    algorithm: str
    evidence_mode: str
    stability_passes: int
    file_count: int
    total_size: int
    sha256: str


def output_tail(text: str, line_limit: int = 80) -> str:
    """巨大なビルド出力を抑えながら、失敗箇所の末尾は証跡へ残す。"""
    lines = text.splitlines()
    return "\n".join(lines[-line_limit:])


def run_step(
    name: str,
    command: Sequence[str],
    working_directory: Path,
    timeout_seconds: int,
) -> StepResult:
    """外部検証を実行し、例外も失敗結果へ正規化する。"""
    begin = time.perf_counter_ns()
    try:
        completed = subprocess.run(
            list(command),
            cwd=working_directory,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
        )
        return_code = completed.returncode
        stdout = completed.stdout
        stderr = completed.stderr
    except subprocess.TimeoutExpired as error:
        return_code = 124
        stdout = error.stdout or ""
        stderr = error.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode(errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode(errors="replace")
        stderr += f"\n{timeout_seconds} 秒でタイムアウトしました。"
    except OSError as error:
        return_code = 127
        stdout = ""
        stderr = str(error)
    duration_ms = (time.perf_counter_ns() - begin) // 1_000_000
    return StepResult(
        name=name,
        status="pass" if return_code == 0 else "fail",
        return_code=return_code,
        duration_ms=duration_ms,
        command=list(command),
        stdout_tail=output_tail(stdout),
        stderr_tail=output_tail(stderr),
    )


def sha256_file(path: Path) -> str:
    """大きいartifactも全体を保持せずSHA-256へ集約する。"""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def stat_signature(info: os.stat_result) -> tuple[int, ...]:
    """置換と内容更新を検出するため、時刻とfile identityを安定した組へまとめる。"""
    return (
        int(info.st_dev),
        int(info.st_ino),
        int(info.st_mode),
        int(info.st_size),
        int(info.st_mtime_ns),
        int(info.st_ctime_ns),
        int(getattr(info, "st_file_attributes", 0)),
    )


def reject_link_or_reparse(path: Path, info: os.stat_result) -> None:
    """symlinkとWindows reparse pointを配布境界として拒否する。"""
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    attributes = int(getattr(info, "st_file_attributes", 0))
    if stat.S_ISLNK(info.st_mode) or (reparse_flag and attributes & reparse_flag):
        raise RuntimeError(f"artifact path is link or reparse point: {path}")


def absolute_without_link_resolution(path: Path) -> Path:
    """linkを辿らず、呼出時のpath構成を保った絶対pathを返す。"""
    return Path(os.path.abspath(os.fspath(path)))


def validate_path_chain(path: Path) -> None:
    """対象までの全componentにlinkまたはreparse pointがないことを確認する。"""
    current = absolute_without_link_resolution(path)
    while True:
        info = current.lstat()
        reject_link_or_reparse(current, info)
        parent = current.parent
        if parent == current:
            break
        current = parent


def stable_file_digest(path: Path) -> tuple[int, str]:
    """fileをhashし、読取前後でidentity・size・更新時刻が不変なことを確認する。"""
    before_path = path.lstat()
    reject_link_or_reparse(path, before_path)
    if not stat.S_ISREG(before_path.st_mode):
        raise RuntimeError(f"artifact is not a regular file: {path}")

    digest = hashlib.sha256()
    with path.open("rb") as stream:
        before_stream = os.fstat(stream.fileno())
        if stat_signature(before_stream) != stat_signature(before_path):
            raise RuntimeError(f"artifact changed before hashing: {path}")
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
        after_stream = os.fstat(stream.fileno())

    after_path = path.lstat()
    signatures = {
        stat_signature(before_path),
        stat_signature(before_stream),
        stat_signature(after_stream),
        stat_signature(after_path),
    }
    if len(signatures) != 1:
        raise RuntimeError(f"artifact changed while hashing: {path}")
    return before_path.st_size, digest.hexdigest()


def run_git(source_root: Path, arguments: Sequence[str]) -> str:
    """source rootのgit値を取得し、曖昧な失敗を証跡へ入れない。"""
    completed = subprocess.run(
        ["git", "-C", str(source_root), *arguments],
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
    )
    if completed.returncode != 0:
        diagnostic = output_tail(completed.stderr or completed.stdout, 20)
        raise RuntimeError(
            f"git {' '.join(arguments)} failed: {diagnostic}"
        )
    return completed.stdout.strip()


def collect_source_evidence(source_root: Path, base_ref: str) -> SourceEvidence:
    """HEAD、比較基点、worktree差分を同じrepositoryから取得する。"""
    root = source_root.resolve()
    git_sha = run_git(root, ["rev-parse", "--verify", "HEAD^{commit}"])
    base_sha = run_git(
        root,
        ["rev-parse", "--verify", f"{base_ref}^{{commit}}"],
    )
    status_lines = run_git(
        root,
        ["status", "--porcelain=v1", "--untracked-files=all"],
    ).splitlines()
    untracked_files = sorted(
        line[3:] for line in status_lines if line.startswith("?? ")
    )
    tracked_dirty = any(not line.startswith("?? ") for line in status_lines)
    divergence = run_git(
        root,
        ["rev-list", "--left-right", "--count", f"{base_ref}...HEAD"],
    ).split()
    if len(divergence) != 2:
        raise RuntimeError(f"invalid git divergence output: {divergence}")
    behind_count, ahead_count = (int(value) for value in divergence)
    return SourceEvidence(
        root=str(root),
        git_sha=git_sha,
        base_ref=base_ref,
        base_sha=base_sha,
        tracked_dirty=tracked_dirty,
        untracked_files=untracked_files,
        ahead_count=ahead_count,
        behind_count=behind_count,
    )


def validate_source_requirements(
    evidence: SourceEvidence,
    require_clean_source: bool,
    require_base_ancestor: bool,
) -> None:
    """公開条件に応じてworktree汚染と比較基点からの後退を拒否する。"""
    if require_clean_source and (
        evidence.tracked_dirty or evidence.untracked_files
    ):
        raise RuntimeError("tracked or untracked source changes are present")
    if require_base_ancestor and evidence.behind_count != 0:
        raise RuntimeError(
            f"{evidence.base_ref} is not an ancestor of HEAD "
            f"(behind={evidence.behind_count})"
        )


def parse_cmake_cache(text: str) -> dict[str, str]:
    """CMakeCacheの通常entryだけをkey/valueへ変換する。"""
    values: dict[str, str] = {}
    for line in text.splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        declaration, value = line.split("=", 1)
        key = declaration.split(":", 1)[0]
        values[key] = value
    return values


def collect_cmake_evidence(build_directory: Path) -> CMakeEvidence:
    """release判断に必要なCMake構成とcache digestを取得する。"""
    cache_path = build_directory.resolve() / "CMakeCache.txt"
    if not cache_path.is_file():
        raise RuntimeError(f"CMakeCache.txt is missing: {cache_path}")
    parsed = parse_cmake_cache(cache_path.read_text(encoding="utf-8"))
    missing = [key for key in CMAKE_EVIDENCE_KEYS if key not in parsed]
    if missing:
        raise RuntimeError(
            "required CMake cache entries are missing: " + ", ".join(missing)
        )
    return CMakeEvidence(
        cache_path=str(cache_path),
        cache_sha256=sha256_file(cache_path),
        values={key: parsed[key] for key in CMAKE_EVIDENCE_KEYS},
    )


def parse_cache_expectations(values: Sequence[str]) -> dict[str, str]:
    """KEY=VALUE形式の期待構成を重複なく正規化する。"""
    expectations: dict[str, str] = {}
    for item in values:
        if "=" not in item:
            raise ValueError(f"invalid cache expectation: {item}")
        key, value = item.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not key or not value:
            raise ValueError(f"invalid cache expectation: {item}")
        if key in expectations and expectations[key] != value:
            raise ValueError(f"conflicting cache expectation: {key}")
        expectations[key] = value
    return expectations


def validate_cache_expectations(
    evidence: CMakeEvidence,
    expectations: dict[str, str],
) -> None:
    """要求されたbackend/build構成とcacheの完全一致を確認する。"""
    for key, expected in expectations.items():
        actual = evidence.values.get(key)
        if actual != expected:
            raise RuntimeError(
                f"CMake cache mismatch: {key} expected={expected} actual={actual}"
            )


def validate_build_source(
    evidence: CMakeEvidence,
    source_root: Path,
) -> None:
    """CMake build treeが検証対象checkoutのengineを参照することを確認する。"""
    expected = source_root.resolve() / "acs" / "engine"
    actual = Path(evidence.values["CMAKE_HOME_DIRECTORY"])
    try:
        matches = os.path.samefile(actual, expected)
    except OSError as error:
        raise RuntimeError(
            "CMake source directory cannot be compared: "
            f"expected={expected} actual={actual}: {error}"
        ) from error
    if not matches:
        raise RuntimeError(
            "CMake source directory mismatch: "
            f"expected={expected} actual={actual}"
        )


def collect_artifact_evidence_once(paths: Sequence[Path]) -> list[ArtifactEvidence]:
    """重複を除いたartifactを、一回の安定した読取でhash化する。"""
    artifacts: list[ArtifactEvidence] = []
    visited_paths: list[Path] = []
    for requested in paths:
        path = absolute_without_link_resolution(requested)
        validate_path_chain(path)
        if any(os.path.samefile(path, visited) for visited in visited_paths):
            continue
        visited_paths.append(path)
        size, file_digest = stable_file_digest(path)
        if size <= 0:
            raise RuntimeError(f"artifact is missing or empty: {path}")
        artifacts.append(
            ArtifactEvidence(
                path=str(path),
                size=size,
                sha256=file_digest,
            )
        )
    return artifacts


def collect_artifact_evidence(paths: Sequence[Path]) -> list[ArtifactEvidence]:
    """artifactを二回収集し、途中更新を含まない安定した証跡だけを返す。"""
    first = collect_artifact_evidence_once(paths)
    second = collect_artifact_evidence_once(paths)
    if first != second:
        raise RuntimeError("artifact changed between stability scans")
    return second


def collect_tree_files(root: Path) -> list[Path]:
    """reparse pointを辿らず、通常fileだけを決定的な順序で列挙する。"""
    files: list[Path] = []
    pending = [root]
    while pending:
        directory = pending.pop()
        before_directory = directory.lstat()
        reject_link_or_reparse(directory, before_directory)
        if not stat.S_ISDIR(before_directory.st_mode):
            raise RuntimeError(f"artifact tree entry is not a directory: {directory}")

        with os.scandir(directory) as stream:
            entries = sorted(
                stream,
                key=lambda entry: (entry.name.casefold(), entry.name),
            )
        for entry in entries:
            path = Path(entry.path)
            info = entry.stat(follow_symlinks=False)
            reject_link_or_reparse(path, info)
            if stat.S_ISDIR(info.st_mode):
                pending.append(path)
            elif stat.S_ISREG(info.st_mode):
                files.append(path)
            else:
                raise RuntimeError(f"artifact tree contains special file: {path}")

        after_directory = directory.lstat()
        if stat_signature(before_directory) != stat_signature(after_directory):
            raise RuntimeError(f"artifact tree directory changed while scanning: {directory}")

    return sorted(
        files,
        key=lambda path: (
            path.relative_to(root).as_posix().casefold(),
            path.relative_to(root).as_posix(),
        ),
    )


def collect_artifact_tree_evidence_once(paths: Sequence[Path]) -> list[ArtifactTreeEvidence]:
    """配布directoryを一回走査し、相対path・size・file digestへ集約する。"""
    trees: list[ArtifactTreeEvidence] = []
    visited_roots: list[Path] = []
    for requested in paths:
        root = absolute_without_link_resolution(requested)
        validate_path_chain(root)
        root_info = root.lstat()
        if not stat.S_ISDIR(root_info.st_mode):
            raise RuntimeError(f"artifact tree is missing: {root}")
        if any(os.path.samefile(root, visited) for visited in visited_roots):
            continue
        visited_roots.append(root)
        files = collect_tree_files(root)
        if not files:
            raise RuntimeError(f"artifact tree is empty: {root}")
        digest = hashlib.sha256()
        total_size = 0
        for path in files:
            relative = path.relative_to(root).as_posix()
            size, file_digest = stable_file_digest(path)
            total_size += size
            digest.update(relative.encode("utf-8"))
            digest.update(b"\0")
            digest.update(str(size).encode("ascii"))
            digest.update(b"\0")
            digest.update(bytes.fromhex(file_digest))
        trees.append(
            ArtifactTreeEvidence(
                path=str(root),
                algorithm=ARTIFACT_TREE_DIGEST_ALGORITHM,
                evidence_mode=ARTIFACT_TREE_EVIDENCE_MODE,
                stability_passes=2,
                file_count=len(files),
                total_size=total_size,
                sha256=digest.hexdigest(),
            )
        )
    return trees


def collect_artifact_tree_evidence(
    paths: Sequence[Path],
    between_passes_for_test: Callable[[], None] | None = None,
) -> list[ArtifactTreeEvidence]:
    """配布directoryを二回収集し、file集合とbytesが安定した証跡だけを返す。"""
    first = collect_artifact_tree_evidence_once(paths)
    if between_passes_for_test is not None:
        between_passes_for_test()
    second = collect_artifact_tree_evidence_once(paths)
    if first != second:
        raise RuntimeError("artifact tree changed between stability scans")
    return second


def validate_artifact_tree_parity(
    trees: Sequence[ArtifactTreeEvidence],
    require_identical: bool,
) -> None:
    """記録後の更新を拒否し、必要なら複数配布directoryの一致を確認する。"""
    current = collect_artifact_tree_evidence(
        [Path(tree.path) for tree in trees]
    )
    if list(trees) != current:
        raise RuntimeError("artifact tree changed after evidence collection")
    if not require_identical:
        return
    if len(trees) < 2:
        raise RuntimeError(
            "at least two artifact trees are required for parity validation"
        )
    expected = (
        trees[0].algorithm,
        trees[0].evidence_mode,
        trees[0].file_count,
        trees[0].total_size,
        trees[0].sha256,
    )
    for tree in trees[1:]:
        actual = (
            tree.algorithm,
            tree.evidence_mode,
            tree.file_count,
            tree.total_size,
            tree.sha256,
        )
        if actual != expected:
            raise RuntimeError(
                f"artifact tree mismatch: {trees[0].path} != {tree.path}"
            )


def build_report(
    configuration: str,
    steps: Sequence[StepResult],
    source: SourceEvidence,
    cmake: CMakeEvidence,
    artifacts: Sequence[ArtifactEvidence],
    artifact_trees: Sequence[ArtifactTreeEvidence],
) -> dict[str, object]:
    """段階結果から、機械判定できる安定した報告形式を生成する。"""
    passed = all(step.return_code == 0 for step in steps)
    return {
        "schema": REPORT_SCHEMA_VERSION,
        "status": "pass" if passed else "fail",
        "configuration": configuration,
        "evidence_contract": {
            "source": SOURCE_EVIDENCE_MODE,
            "artifact_tree": ARTIFACT_TREE_EVIDENCE_MODE,
        },
        "source": asdict(source),
        "cmake": asdict(cmake),
        "artifacts": [asdict(artifact) for artifact in artifacts],
        "artifact_trees": [asdict(tree) for tree in artifact_trees],
        "step_count": len(steps),
        "passed_steps": sum(step.return_code == 0 for step in steps),
        "steps": [asdict(step) for step in steps],
    }


def write_report(path: Path, report: dict[str, object]) -> None:
    """途中書き込みを公開しないよう、一時ファイルから置換する。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, path)


def make_build_command(build_directory: Path, configuration: str, jobs: int, clean_first: bool) -> list[str]:
    """構成と完全再ビルド方針からCMakeビルド命令を作る。"""
    command = ["cmake", "--build", str(build_directory), "--config", configuration]
    if clean_first:
        command.append("--clean-first")
    command.extend(["--parallel", str(jobs)])
    return command


def self_test() -> int:
    """schema、source、cache、artifact digestと合否判定を固定する。"""
    with tempfile.TemporaryDirectory(
        prefix="acs-foundation-evidence-selftest-"
    ) as temporary:
        root = Path(temporary)
        tracked_path = root / "tracked.txt"
        tracked_path.write_text("clean\n", encoding="utf-8")
        ignore_path = root / ".gitignore"
        ignore_path.write_text(
            "acs/\nartifact.bin\nbuild/\ndistribution/\n"
            "distribution-mirror/\n",
            encoding="utf-8",
        )
        git_commands = (
            ["git", "init"],
            ["git", "add", ".gitignore", "tracked.txt"],
            [
                "git",
                "-c",
                "user.name=ACS SelfTest",
                "-c",
                "user.email=acs-selftest@example.invalid",
                "commit",
                "-m",
                "self test",
            ],
        )
        for command in git_commands:
            completed = subprocess.run(
                command,
                cwd=root,
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
            )
            if completed.returncode != 0:
                return 1

        build_directory = root / "build"
        build_directory.mkdir()
        cmake_source_directory = root / "acs" / "engine"
        cmake_source_directory.mkdir(parents=True)
        cache_path = build_directory / "CMakeCache.txt"
        cache_entries = [
            f"{key}:BOOL=ON"
            for key in CMAKE_EVIDENCE_KEYS
            if key != "CMAKE_HOME_DIRECTORY"
        ]
        cache_entries.append(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={cmake_source_directory}"
        )
        cache_path.write_text(
            "\n".join(cache_entries) + "\n",
            encoding="utf-8",
        )
        artifact_path = root / "artifact.bin"
        artifact_path.write_bytes(b"foundation-evidence")
        artifact_tree = root / "distribution"
        artifact_tree.mkdir()
        (artifact_tree / "acs.h").write_bytes(b"header")
        library_directory = artifact_tree / "lib"
        library_directory.mkdir()
        (library_directory / "acs.lib").write_bytes(b"library")
        mirror_tree = root / "distribution-mirror"
        mirror_tree.mkdir()
        (mirror_tree / "acs.h").write_bytes(b"header")
        mirror_library_directory = mirror_tree / "lib"
        mirror_library_directory.mkdir()
        (mirror_library_directory / "acs.lib").write_bytes(b"library")

        source = collect_source_evidence(root, "HEAD")
        tracked_path.write_text("dirty\n", encoding="utf-8")
        dirty_source = collect_source_evidence(root, "HEAD")
        tracked_path.write_text("clean\n", encoding="utf-8")
        untracked_path = root / "untracked.cpp"
        untracked_path.write_text("int value;\n", encoding="utf-8")
        untracked_source = collect_source_evidence(root, "HEAD")
        untracked_path.unlink()
        cmake = collect_cmake_evidence(build_directory)
        artifacts = collect_artifact_evidence(
            [artifact_path, artifact_path]
        )
        artifact_trees = collect_artifact_tree_evidence(
            [artifact_tree, artifact_tree]
        )
        repeated_artifact_trees = collect_artifact_tree_evidence(
            [artifact_tree]
        )
        if os.name == "nt":
            # 同じdirectoryを指す通常pathと拡張長pathを重複として扱う。
            physical_alias = Path("\\\\?\\" + str(artifact_tree))
            physical_alias_trees = collect_artifact_tree_evidence(
                [artifact_tree, physical_alias]
            )
            rejects_physical_alias = len(physical_alias_trees) == 1
        else:
            rejects_physical_alias = True
        try:
            collect_artifact_tree_evidence(
                [artifact_tree],
                lambda: (library_directory / "acs.lib").write_bytes(b"changed"),
            )
            rejects_between_pass_mutation = False
        except RuntimeError:
            rejects_between_pass_mutation = True
        (library_directory / "acs.lib").write_bytes(b"library")

        recorded_before_mutation = collect_artifact_tree_evidence(
            [artifact_tree]
        )
        (library_directory / "acs.lib").write_bytes(b"changed")
        try:
            validate_artifact_tree_parity(recorded_before_mutation, False)
            rejects_after_collection_mutation = False
        except RuntimeError:
            rejects_after_collection_mutation = True
        (library_directory / "acs.lib").write_bytes(b"library")

        outside_tree = root / "outside-tree"
        outside_tree.mkdir()
        (outside_tree / "outside.bin").write_bytes(b"outside")
        reparse_entry = artifact_tree / "outside-link"
        if os.name == "nt":
            create_reparse = subprocess.run(
                [
                    "cmd.exe",
                    "/d",
                    "/c",
                    "mklink",
                    "/J",
                    str(reparse_entry),
                    str(outside_tree),
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
            )
            reparse_created = create_reparse.returncode == 0
        else:
            reparse_entry.symlink_to(outside_tree, target_is_directory=True)
            reparse_created = True
        try:
            if reparse_created:
                try:
                    collect_artifact_tree_evidence([artifact_tree])
                    rejects_reparse_tree = False
                except RuntimeError:
                    rejects_reparse_tree = True
            else:
                rejects_reparse_tree = False
        finally:
            if reparse_created:
                if os.name == "nt":
                    os.rmdir(reparse_entry)
                else:
                    reparse_entry.unlink()

        try:
            validate_artifact_tree_parity(artifact_trees, True)
            rejects_single_tree_parity = False
        except RuntimeError:
            rejects_single_tree_parity = True
        matching_artifact_trees = collect_artifact_tree_evidence(
            [artifact_tree, mirror_tree]
        )
        validate_artifact_tree_parity(matching_artifact_trees, True)
        (mirror_library_directory / "acs.lib").write_bytes(b"mismatch")
        mismatched_artifact_trees = collect_artifact_tree_evidence(
            [artifact_tree, mirror_tree]
        )
        try:
            validate_artifact_tree_parity(mismatched_artifact_trees, True)
            rejects_tree_mismatch = False
        except RuntimeError:
            rejects_tree_mismatch = True
        (mirror_library_directory / "acs.lib").write_bytes(b"library")
        (library_directory / "acs.lib").write_bytes(b"tampered")
        changed_artifact_trees = collect_artifact_tree_evidence(
            [artifact_tree]
        )
        (library_directory / "acs.lib").write_bytes(b"library")
        validate_source_requirements(source, True, True)
        try:
            validate_source_requirements(dirty_source, True, True)
            rejects_tracked = False
        except RuntimeError:
            rejects_tracked = True
        try:
            validate_source_requirements(untracked_source, True, True)
            rejects_untracked = False
        except RuntimeError:
            rejects_untracked = True
        behind_source = SourceEvidence(
            source.root,
            source.git_sha,
            source.base_ref,
            source.base_sha,
            False,
            [],
            0,
            1,
        )
        try:
            validate_source_requirements(behind_source, False, True)
            rejects_behind = False
        except RuntimeError:
            rejects_behind = True
        passing = StepResult("ok", "pass", 0, 1, ["true"], "", "")
        failing = StepResult("ng", "fail", 7, 1, ["false"], "", "failure")
        pass_report = build_report(
            "Release",
            [passing],
            source,
            cmake,
            artifacts,
            artifact_trees,
        )
        fail_report = build_report(
            "Release",
            [passing, failing],
            source,
            cmake,
            artifacts,
            artifact_trees,
        )
        expectations = parse_cache_expectations(
            ["ACS_RENDER_DX12_RAW=ON", "ACS_BUILD_TESTS=ON"]
        )
        validate_cache_expectations(cmake, expectations)
        validate_build_source(cmake, root)
        wrong_source_values = dict(cmake.values)
        wrong_source = root / "other" / "engine"
        wrong_source.mkdir(parents=True)
        wrong_source_values["CMAKE_HOME_DIRECTORY"] = str(wrong_source)
        wrong_source_evidence = CMakeEvidence(
            cmake.cache_path,
            cmake.cache_sha256,
            wrong_source_values,
        )
        try:
            validate_cache_expectations(
                cmake,
                {"ACS_RENDER_DX12_RAW": "OFF"},
            )
            rejects_mismatch = False
        except RuntimeError:
            rejects_mismatch = True
        try:
            validate_build_source(wrong_source_evidence, root)
            rejects_wrong_source = False
        except RuntimeError:
            rejects_wrong_source = True

        tail = output_tail("\n".join(str(index) for index in range(100)), 3)
        clean_command = make_build_command(
            Path("build"), "Release", 4, True
        )
        valid = (
            pass_report["schema"] == REPORT_SCHEMA_VERSION
            and pass_report["status"] == "pass"
            and pass_report["passed_steps"] == 1
            and pass_report["evidence_contract"]["source"]
            == SOURCE_EVIDENCE_MODE
            and pass_report["evidence_contract"]["artifact_tree"]
            == ARTIFACT_TREE_EVIDENCE_MODE
            and pass_report["source"]["git_sha"] == source.base_sha
            and not pass_report["source"]["tracked_dirty"]
            and not pass_report["source"]["untracked_files"]
            and pass_report["source"]["ahead_count"] == 0
            and pass_report["source"]["behind_count"] == 0
            and dirty_source.tracked_dirty
            and rejects_tracked
            and rejects_untracked
            and rejects_behind
            and pass_report["cmake"]["values"]["ACS_RENDER_DILIGENT"] == "ON"
            and len(pass_report["artifacts"]) == 1
            and pass_report["artifacts"][0]["sha256"]
            == hashlib.sha256(b"foundation-evidence").hexdigest()
            and len(pass_report["artifact_trees"]) == 1
            and pass_report["artifact_trees"][0]["algorithm"]
            == ARTIFACT_TREE_DIGEST_ALGORITHM
            and pass_report["artifact_trees"][0]["evidence_mode"]
            == ARTIFACT_TREE_EVIDENCE_MODE
            and pass_report["artifact_trees"][0]["stability_passes"] == 2
            and pass_report["artifact_trees"][0]["file_count"] == 2
            and pass_report["artifact_trees"][0]["total_size"] == 13
            and artifact_trees == repeated_artifact_trees
            and rejects_physical_alias
            and artifact_trees[0].sha256
            != changed_artifact_trees[0].sha256
            and rejects_between_pass_mutation
            and rejects_after_collection_mutation
            and rejects_reparse_tree
            and rejects_single_tree_parity
            and rejects_tree_mismatch
            and fail_report["status"] == "fail"
            and fail_report["passed_steps"] == 1
            and rejects_mismatch
            and rejects_wrong_source
            and tail == "97\n98\n99"
            and "--clean-first" in clean_command
        )
    return 0 if valid else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--performance-executable", type=Path)
    parser.add_argument("--artifact", type=Path, action="append", default=[])
    parser.add_argument(
        "--artifact-tree",
        type=Path,
        action="append",
        default=[],
    )
    parser.add_argument(
        "--require-identical-artifact-trees",
        action="store_true",
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--base-ref", default="origin/main")
    parser.add_argument("--expect-cache", action="append", default=[])
    parser.add_argument("--require-clean-source", action="store_true")
    parser.add_argument("--require-base-ancestor", action="store_true")
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    parser.add_argument("--timeout-seconds", type=int, default=1800)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--clean-first", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.build_dir is None:
        parser.error("--build-dir is required unless --self-test is used")
    if args.performance_executable is None:
        parser.error(
            "--performance-executable is required unless --self-test is used"
        )
    if args.output is None:
        parser.error("--output is required unless --self-test is used")
    if args.jobs <= 0 or args.timeout_seconds <= 0:
        parser.error("--jobs and --timeout-seconds must be positive")

    build_directory = args.build_dir.resolve()
    performance_executable = args.performance_executable.resolve()
    source_root = (
        args.source_root.resolve()
        if args.source_root is not None
        else Path(__file__).resolve().parents[2]
    )
    try:
        source_evidence = collect_source_evidence(
            source_root,
            args.base_ref,
        )
        cmake_evidence = collect_cmake_evidence(build_directory)
        validate_build_source(cmake_evidence, source_root)
        cache_expectations = parse_cache_expectations(args.expect_cache)
        validate_cache_expectations(cmake_evidence, cache_expectations)
        validate_source_requirements(
            source_evidence,
            args.require_clean_source,
            args.require_base_ancestor,
        )
    except (OSError, RuntimeError, ValueError) as error:
        print(f"foundation_end_to_end=evidence_error: {error}", file=sys.stderr)
        return 2

    steps: list[StepResult] = []
    if args.source_root is not None:
        steps.append(
            run_step(
                "changed_cpp_rules",
                [
                    sys.executable,
                    str(Path(__file__).with_name("audit_changed_cpp_rules.py")),
                    "--root",
                    str(args.source_root.resolve()),
                    "--base-ref",
                    args.base_ref,
                ],
                args.source_root.resolve(),
                args.timeout_seconds,
            )
        )
    if not args.skip_build and all(step.return_code == 0 for step in steps):
        steps.append(
            run_step(
                "release_build",
                make_build_command(build_directory, args.configuration, args.jobs, args.clean_first),
                build_directory,
                args.timeout_seconds,
            )
        )
    if all(step.return_code == 0 for step in steps):
        steps.append(
            run_step(
                "ctest",
                [
                    "ctest",
                    "--test-dir",
                    str(build_directory),
                    "-C",
                    args.configuration,
                    "--output-on-failure",
                    "--no-tests=error",
                ],
                build_directory,
                args.timeout_seconds,
            )
        )
    if all(step.return_code == 0 for step in steps):
        steps.append(
            run_step(
                "foundation_performance_contract",
                [
                    sys.executable,
                    str(Path(__file__).with_name(
                        "verify_foundation_performance.py"
                    )),
                    "--executable",
                    str(performance_executable),
                ],
                build_directory,
                args.timeout_seconds,
            )
        )

    try:
        final_source_evidence = collect_source_evidence(
            source_root,
            args.base_ref,
        )
        final_cmake_evidence = collect_cmake_evidence(build_directory)
        validate_build_source(final_cmake_evidence, source_root)
        validate_cache_expectations(
            final_cmake_evidence,
            cache_expectations,
        )
        validate_source_requirements(
            final_source_evidence,
            args.require_clean_source,
            args.require_base_ancestor,
        )
        if (
            final_source_evidence.git_sha != source_evidence.git_sha
            or final_source_evidence.base_sha != source_evidence.base_sha
        ):
            raise RuntimeError("source or base commit changed during verification")
        source_evidence = final_source_evidence
        cmake_evidence = final_cmake_evidence
    except (OSError, RuntimeError) as error:
        steps.append(
            StepResult(
                name="context_evidence",
                status="fail",
                return_code=1,
                duration_ms=0,
                command=["git", "rev-parse", "HEAD", "and", "CMakeCache.txt"],
                stdout_tail="",
                stderr_tail=str(error),
            )
        )

    artifact_paths = [performance_executable, *args.artifact]
    try:
        artifacts = collect_artifact_evidence(artifact_paths)
        artifact_trees = collect_artifact_tree_evidence(args.artifact_tree)
        validate_artifact_tree_parity(
            artifact_trees,
            args.require_identical_artifact_trees,
        )
    except (OSError, RuntimeError) as error:
        artifacts = []
        artifact_trees = []
        steps.append(
            StepResult(
                name="artifact_evidence",
                status="fail",
                return_code=1,
                duration_ms=0,
                command=[
                    "sha256",
                    *[str(path) for path in artifact_paths],
                    *[str(path) for path in args.artifact_tree],
                ],
                stdout_tail="",
                stderr_tail=str(error),
            )
        )

    report = build_report(
        args.configuration,
        steps,
        source_evidence,
        cmake_evidence,
        artifacts,
        artifact_trees,
    )
    write_report(args.output.resolve(), report)
    print(
        "foundation_end_to_end="
        f"{report['status']} ({report['passed_steps']}/{report['step_count']})"
    )
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
