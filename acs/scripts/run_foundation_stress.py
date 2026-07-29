#!/usr/bin/env python3
"""基盤所有権・queue・cancelテストを別processで反復する。"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path
from typing import Sequence


def output_tail(value: str, line_limit: int = 40) -> str:
    """失敗診断用に出力末尾だけを返す。"""
    return "\n".join(value.splitlines()[-line_limit:])


def run_repeated(
    command: Sequence[str],
    iterations: int,
    timeout_seconds: int,
) -> tuple[bool, int, str]:
    """独立processを指定回数実行し、最初の失敗位置と診断を返す。"""
    for iteration in range(1, iterations + 1):
        try:
            completed = subprocess.run(
                list(command),
                check=False,
                capture_output=True,
                text=True,
                timeout=timeout_seconds,
            )
        except subprocess.TimeoutExpired as error:
            stdout = error.stdout or ""
            stderr = error.stderr or ""
            if isinstance(stdout, bytes):
                stdout = stdout.decode(errors="replace")
            if isinstance(stderr, bytes):
                stderr = stderr.decode(errors="replace")
            diagnostic = output_tail(stdout + "\n" + stderr)
            return False, iteration, diagnostic + "\nprocess timeout"
        except OSError as error:
            return False, iteration, str(error)
        if completed.returncode != 0:
            diagnostic = output_tail(completed.stdout + "\n" + completed.stderr)
            return False, iteration, diagnostic
    return True, iterations, ""


def self_test() -> int:
    """成功・失敗・末尾切り詰めを実processで確認する。"""
    passing = [sys.executable, "-c", "raise SystemExit(0)"]
    failing = [sys.executable, "-c", "raise SystemExit(7)"]
    passed, completed, _ = run_repeated(passing, 2, 10)
    failed, failed_at, _ = run_repeated(failing, 2, 10)
    tail = output_tail("0\n1\n2\n3", 2)
    valid = passed and completed == 2 and not failed and failed_at == 1 and tail == "2\n3"
    return 0 if valid else 1


def main() -> int:
    """引数を検証し、反復stressの成否を終了codeへ反映する。"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--iterations", type=int, default=32)
    parser.add_argument("--timeout-seconds", type=int, default=60)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.executable is None:
        parser.error("--executable is required unless --self-test is used")
    if not 1 <= args.iterations <= 1000:
        parser.error("--iterations must be between 1 and 1000")
    if not 1 <= args.timeout_seconds <= 600:
        parser.error("--timeout-seconds must be between 1 and 600")

    executable = args.executable.resolve()
    passed, completed, diagnostic = run_repeated(
        [str(executable)],
        args.iterations,
        args.timeout_seconds,
    )
    if not passed:
        print(
            f"foundation_stress=fail iteration={completed}/"
            f"{args.iterations}\n{diagnostic}",
            file=sys.stderr,
        )
        return 1
    print(f"foundation_stress=pass iterations={completed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
