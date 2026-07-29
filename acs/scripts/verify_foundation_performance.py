#!/usr/bin/env python3
"""ACS 基盤パフォーマンス報告の決定的な契約を検証する。"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


def validate(report: dict[str, Any]) -> list[str]:
    """タイミング値ではなく、削減作業量と遅延上限を検証する。"""
    errors: list[str] = []
    if report.get("schema") != 1:
        errors.append("schema must be 1")
    if report.get("status") != "pass":
        errors.append("producer status must be pass")

    xinput = report.get("xinput", {})
    baseline_polls = int(xinput.get("baseline_calls", -1))
    optimized_polls = int(xinput.get("optimized_calls", -1))
    if baseline_polls <= 0 or optimized_polls * 4 != baseline_polls:
        errors.append("disconnected XInput calls must be reduced by exactly 75 percent")
    if int(xinput.get("reconnect_bound_frames", -1)) > 4:
        errors.append("controller reconnect latency must remain within four frames")

    logger = report.get("logger", {})
    records = int(logger.get("burst_records", -1))
    baseline_wakes = int(logger.get("baseline_wake_signals", -1))
    optimized_wakes = int(logger.get("optimized_wake_signals", -1))
    if records <= 0 or baseline_wakes != records:
        errors.append("logger baseline must wake once per record")
    if optimized_wakes != 1:
        errors.append("a synthetic non-drained logger burst must wake exactly once")
    return errors


def self_test() -> int:
    """検証器自身が成功例と失敗例を区別できることを確認する。"""
    passing = {
        "schema": 1,
        "status": "pass",
        "xinput": {
            "baseline_calls": 400,
            "optimized_calls": 100,
            "reconnect_bound_frames": 4,
        },
        "logger": {
            "burst_records": 64,
            "baseline_wake_signals": 64,
            "optimized_wake_signals": 1,
        },
    }
    failing = json.loads(json.dumps(passing))
    failing["xinput"]["optimized_calls"] = 101
    failing["logger"]["optimized_wake_signals"] = 64
    return 0 if not validate(passing) and len(validate(failing)) == 2 else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.executable is None:
        parser.error("--executable is required unless --self-test is used")

    completed = subprocess.run(
        [str(args.executable)],
        check=False,
        capture_output=True,
        text=True,
        timeout=20,
    )
    if completed.returncode != 0:
        sys.stderr.write(completed.stdout)
        sys.stderr.write(completed.stderr)
        return completed.returncode or 1
    try:
        report = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        print(f"invalid performance JSON: {error}", file=sys.stderr)
        return 1

    errors = validate(report)
    if errors:
        for error in errors:
            print(f"foundation performance contract: {error}", file=sys.stderr)
        return 1
    print("foundation_performance_contract=ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
