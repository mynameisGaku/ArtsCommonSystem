#!/usr/bin/env python3
"""Debug/Release build・全CTest・基盤性能契約を一つのJSON証跡へまとめる。"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Sequence


REPORT_SCHEMA_VERSION = 2
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
    """HEAD、比較基点、tracked差分を同じrepositoryから取得する。"""
    root = source_root.resolve()
    git_sha = run_git(root, ["rev-parse", "--verify", "HEAD^{commit}"])
    base_sha = run_git(
        root,
        ["rev-parse", "--verify", f"{base_ref}^{{commit}}"],
    )
    tracked_status = run_git(
        root,
        ["status", "--porcelain", "--untracked-files=no"],
    )
    return SourceEvidence(
        root=str(root),
        git_sha=git_sha,
        base_ref=base_ref,
        base_sha=base_sha,
        tracked_dirty=bool(tracked_status),
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


def collect_artifact_evidence(paths: Sequence[Path]) -> list[ArtifactEvidence]:
    """重複を除いた検証artifactを非空fileとしてhash化する。"""
    artifacts: list[ArtifactEvidence] = []
    visited: set[str] = set()
    for requested in paths:
        path = requested.resolve()
        key = os.path.normcase(str(path))
        if key in visited:
            continue
        visited.add(key)
        if not path.is_file() or path.stat().st_size <= 0:
            raise RuntimeError(f"artifact is missing or empty: {path}")
        artifacts.append(
            ArtifactEvidence(
                path=str(path),
                size=path.stat().st_size,
                sha256=sha256_file(path),
            )
        )
    return artifacts


def build_report(
    configuration: str,
    steps: Sequence[StepResult],
    source: SourceEvidence,
    cmake: CMakeEvidence,
    artifacts: Sequence[ArtifactEvidence],
) -> dict[str, object]:
    """段階結果から、機械判定できる安定した報告形式を生成する。"""
    passed = all(step.return_code == 0 for step in steps)
    return {
        "schema": REPORT_SCHEMA_VERSION,
        "status": "pass" if passed else "fail",
        "configuration": configuration,
        "source": asdict(source),
        "cmake": asdict(cmake),
        "artifacts": [asdict(artifact) for artifact in artifacts],
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
        git_commands = (
            ["git", "init"],
            ["git", "add", "tracked.txt"],
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

        source = collect_source_evidence(root, "HEAD")
        tracked_path.write_text("dirty\n", encoding="utf-8")
        dirty_source = collect_source_evidence(root, "HEAD")
        tracked_path.write_text("clean\n", encoding="utf-8")
        cmake = collect_cmake_evidence(build_directory)
        artifacts = collect_artifact_evidence(
            [artifact_path, artifact_path]
        )
        passing = StepResult("ok", "pass", 0, 1, ["true"], "", "")
        failing = StepResult("ng", "fail", 7, 1, ["false"], "", "failure")
        pass_report = build_report(
            "Release", [passing], source, cmake, artifacts
        )
        fail_report = build_report(
            "Release", [passing, failing], source, cmake, artifacts
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
            and pass_report["source"]["git_sha"] == source.base_sha
            and not pass_report["source"]["tracked_dirty"]
            and dirty_source.tracked_dirty
            and pass_report["cmake"]["values"]["ACS_RENDER_DILIGENT"] == "ON"
            and len(pass_report["artifacts"]) == 1
            and pass_report["artifacts"][0]["sha256"]
            == hashlib.sha256(b"foundation-evidence").hexdigest()
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
    parser.add_argument("--output", type=Path)
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--base-ref", default="origin/main")
    parser.add_argument("--expect-cache", action="append", default=[])
    parser.add_argument("--require-clean-source", action="store_true")
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
        if args.require_clean_source and source_evidence.tracked_dirty:
            raise RuntimeError("tracked source changes are present")
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
        if (
            final_source_evidence.git_sha != source_evidence.git_sha
            or final_source_evidence.base_sha != source_evidence.base_sha
        ):
            raise RuntimeError("source or base commit changed during verification")
        if args.require_clean_source and final_source_evidence.tracked_dirty:
            raise RuntimeError("tracked source changes appeared during verification")
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
    except (OSError, RuntimeError) as error:
        artifacts = []
        steps.append(
            StepResult(
                name="artifact_evidence",
                status="fail",
                return_code=1,
                duration_ms=0,
                command=["sha256", *[str(path) for path in artifact_paths]],
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
    )
    write_report(args.output.resolve(), report)
    print(
        "foundation_end_to_end="
        f"{report['status']} ({report['passed_steps']}/{report['step_count']})"
    )
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
