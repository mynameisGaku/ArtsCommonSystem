#!/usr/bin/env python3
"""Release ビルド・全 CTest・基盤性能契約を一つの JSON 証跡へまとめる。"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Sequence


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


def build_report(
    configuration: str,
    steps: Sequence[StepResult],
) -> dict[str, object]:
    """段階結果から、機械判定できる安定した報告形式を生成する。"""
    passed = all(step.return_code == 0 for step in steps)
    return {
        "schema": 1,
        "status": "pass" if passed else "fail",
        "configuration": configuration,
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
    """集約器が成功・失敗を正しく判定し、出力を切り詰めることを確認する。"""
    passing = StepResult("ok", "pass", 0, 1, ["true"], "", "")
    failing = StepResult("ng", "fail", 7, 1, ["false"], "", "failure")
    pass_report = build_report("Release", [passing])
    fail_report = build_report("Release", [passing, failing])
    tail = output_tail("\n".join(str(index) for index in range(100)), 3)
    clean_command = make_build_command(Path("build"), "Release", 4, True)
    valid = (
        pass_report["status"] == "pass"
        and pass_report["passed_steps"] == 1
        and fail_report["status"] == "fail"
        and fail_report["passed_steps"] == 1
        and tail == "97\n98\n99"
        and "--clean-first" in clean_command
    )
    return 0 if valid else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--performance-executable", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--base-ref", default="origin/main")
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

    report = build_report(args.configuration, steps)
    write_report(args.output.resolve(), report)
    print(
        "foundation_end_to_end="
        f"{report['status']} ({report['passed_steps']}/{report['step_count']})"
    )
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
