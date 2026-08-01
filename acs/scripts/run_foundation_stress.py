#!/usr/bin/env python3
"""基盤所有権・queue・cancelテストを別processで反復する。"""

from __future__ import annotations

import argparse
import io
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from typing import Callable, ContextManager, List, Optional, Sequence, Tuple


DirectoryFactory = Callable[..., ContextManager[str]]
DirectoryObserver = Optional[Callable[[Path], None]]
DirectoryCompletionObserver = Optional[Callable[[Path], None]]


def output_tail(value: str, line_limit: int = 40) -> str:
    """失敗診断用に出力末尾だけを返す。"""
    return "\n".join(value.splitlines()[-line_limit:])


def format_os_error(prefix: str, error: OSError) -> str:
    """OS失敗を地域設定に依存しない短い診断へ整形する。"""
    if isinstance(error, FileNotFoundError):
        return f"{prefix}: file not found"
    error_number = error.errno if error.errno is not None else "unknown"
    return f"{prefix}: errno={error_number}"


def decode_timeout_output(value: object) -> str:
    """timeout例外の文字列またはbyte出力を診断用文字列へ揃える。"""
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode(errors="replace")
    return str(value)


def join_output(stdout: str, stderr: str) -> str:
    """空出力へ余分な改行を足さず標準出力と標準エラーを連結する。"""
    return "\n".join(value.rstrip("\r\n") for value in (stdout, stderr) if value)


def observed_directories_are_clean(directories: Sequence[Path], expected_count: int, parent: Path) -> bool:
    """観測した分離cwdが一意で指定親配下にあり、すべて削除済みかを返す。"""
    return len(directories) == expected_count and len(set(directories)) == expected_count and all(path.parent == parent and not path.exists() for path in directories)


def run_process(command: Sequence[str], timeout_seconds: int, working_directory: Optional[Path]) -> Tuple[bool, str]:
    """信頼済みcommandの直接子だけを一度実行する。"""
    run_arguments = {
        "args": list(command),
        "check": False,
        "capture_output": True,
        "text": True,
        "timeout": timeout_seconds,
    }
    if working_directory is not None:
        run_arguments["cwd"] = str(working_directory)
    try:
        completed = subprocess.run(**run_arguments)
    except subprocess.TimeoutExpired as error:
        output = join_output(decode_timeout_output(error.stdout), decode_timeout_output(error.stderr))
        diagnostic = output_tail(output)
        return False, (diagnostic + "\n" if diagnostic else "") + "process timeout"
    except OSError as error:
        return False, format_os_error("process launch failed", error)
    if completed.returncode != 0:
        output = output_tail(join_output(completed.stdout, completed.stderr))
        return False, (output + "\n" if output else "") + f"process exit code={completed.returncode}"
    return True, ""


def run_repeated(
    command: Sequence[str],
    iterations: int,
    timeout_seconds: int,
    isolate_cwd: bool = False,
    directory_factory: DirectoryFactory = tempfile.TemporaryDirectory,
    directory_observer: DirectoryObserver = None,
    directory_completion_observer: DirectoryCompletionObserver = None,
) -> Tuple[bool, int, str]:
    """独立processを反復し、最初の失敗位置と安定した診断を返す。"""
    for iteration in range(1, iterations + 1):
        if isolate_cwd:
            try:
                temporary_directory = directory_factory(prefix="acs_foundation_stress_")
            except OSError as error:
                return False, iteration, format_os_error("temporary directory creation failed", error)
            try:
                with temporary_directory as directory:
                    working_directory = Path(directory)
                    if directory_observer is not None:
                        directory_observer(working_directory)
                    passed, diagnostic = run_process(command, timeout_seconds, working_directory)
                    if directory_completion_observer is not None:
                        directory_completion_observer(working_directory)
            except OSError as error:
                return False, iteration, format_os_error("temporary directory cleanup failed", error)
        else:
            passed, diagnostic = run_process(command, timeout_seconds, None)
        if not passed:
            return False, iteration, diagnostic
    return True, iterations, ""


def run_and_report(
    command: Sequence[str],
    iterations: int,
    timeout_seconds: int,
    isolate_cwd: bool,
    directory_factory: DirectoryFactory = tempfile.TemporaryDirectory,
    directory_observer: DirectoryObserver = None,
    directory_completion_observer: DirectoryCompletionObserver = None,
) -> int:
    """反復結果をCLIの固定形式で出力し終了codeへ変換する。"""
    passed, completed, diagnostic = run_repeated(command, iterations, timeout_seconds, isolate_cwd, directory_factory, directory_observer, directory_completion_observer)
    if not passed:
        print(f"foundation_stress=fail iteration={completed}/{iterations}\n{diagnostic}", file=sys.stderr)
        return 1
    print(f"foundation_stress=pass iterations={completed}")
    return 0


def run_self_test_cases(caller_directory: Path) -> bool:
    """制御した呼出元cwd内で既定動作と分離動作を検証する。"""
    def controlled_directory_factory(**arguments: object) -> ContextManager[str]:
        """self-testの分離cwdを制御済みcaller配下へ作る。"""
        return tempfile.TemporaryDirectory(dir=str(caller_directory), **arguments)

    observed_directories: List[Path] = []
    marker_name = f"acs_foundation_stress_self_test_{os.getpid()}.marker"
    marker_command = [sys.executable, "-c", f"from pathlib import Path; Path('{marker_name}').write_text('ok', encoding='utf-8')"]
    success_output = io.StringIO()
    success_error = io.StringIO()
    with redirect_stdout(success_output), redirect_stderr(success_error):
        success_code = run_and_report(marker_command, 2, 10, True, controlled_directory_factory, observed_directories.append)
    if success_code != 0 or success_output.getvalue() != "foundation_stress=pass iterations=2\n" or success_error.getvalue() != "":
        return False
    if not observed_directories_are_clean(observed_directories, 2, caller_directory):
        return False
    if (caller_directory / marker_name).exists():
        return False

    failure_marker = "failure.marker"
    failing_command = [sys.executable, "-c", f"from pathlib import Path; import sys; Path('{failure_marker}').write_text('failure', encoding='utf-8'); print('failure-out'); print('failure-err', file=sys.stderr); raise SystemExit(7)"]
    failure_output = io.StringIO()
    failure_error = io.StringIO()
    failure_directories: List[Path] = []
    failure_markers: List[bool] = []
    with redirect_stdout(failure_output), redirect_stderr(failure_error):
        failure_code = run_and_report(failing_command, 2, 10, True, controlled_directory_factory, failure_directories.append, lambda path: failure_markers.append((path / failure_marker).is_file()))
    expected_failure = "foundation_stress=fail iteration=1/2\nfailure-out\nfailure-err\nprocess exit code=7\n"
    if failure_code != 1 or failure_output.getvalue() != "" or failure_error.getvalue() != expected_failure:
        return False
    if failure_markers != [True] or not observed_directories_are_clean(failure_directories, 1, caller_directory):
        return False

    timeout_marker = "timeout.marker"
    timeout_command = [sys.executable, "-c", f"from pathlib import Path; import sys,time; Path('{timeout_marker}').write_text('timeout', encoding='utf-8'); print('timeout-out', flush=True); print('timeout-err', file=sys.stderr, flush=True); time.sleep(10)"]
    timeout_output = io.StringIO()
    timeout_error = io.StringIO()
    timeout_directories: List[Path] = []
    timeout_markers: List[bool] = []
    with redirect_stdout(timeout_output), redirect_stderr(timeout_error):
        timeout_code = run_and_report(timeout_command, 1, 1, True, controlled_directory_factory, timeout_directories.append, lambda path: timeout_markers.append((path / timeout_marker).is_file()))
    expected_timeout = "foundation_stress=fail iteration=1/1\ntimeout-out\ntimeout-err\nprocess timeout\n"
    if timeout_code != 1 or timeout_output.getvalue() != "" or timeout_error.getvalue() != expected_timeout:
        return False
    if timeout_markers != [True] or not observed_directories_are_clean(timeout_directories, 1, caller_directory):
        return False

    missing_output = io.StringIO()
    missing_error = io.StringIO()
    missing_directories: List[Path] = []
    missing_command = [str(Path(tempfile.gettempdir()) / f"acs_foundation_stress_missing_{os.getpid()}.exe")]
    with redirect_stdout(missing_output), redirect_stderr(missing_error):
        missing_code = run_and_report(missing_command, 1, 10, True, controlled_directory_factory, missing_directories.append)
    expected_missing = "foundation_stress=fail iteration=1/1\nprocess launch failed: file not found\n"
    if missing_code != 1 or missing_output.getvalue() != "" or missing_error.getvalue() != expected_missing:
        return False
    if not observed_directories_are_clean(missing_directories, 1, caller_directory):
        return False

    class CleanupFailureDirectory:
        """標準cleanup後の失敗を安定して再現するself-test専用fixture。"""

        def __init__(self, prefix: str) -> None:
            self._directory = tempfile.TemporaryDirectory(prefix=prefix, dir=str(caller_directory))

        def __enter__(self) -> str:
            return self._directory.__enter__()

        def __exit__(self, exception_type: object, exception: object, traceback: object) -> None:
            self._directory.__exit__(exception_type, exception, traceback)
            raise OSError(5, "fixture cleanup failure")

    cleanup_output = io.StringIO()
    cleanup_error = io.StringIO()
    cleanup_directories: List[Path] = []
    with redirect_stdout(cleanup_output), redirect_stderr(cleanup_error):
        cleanup_code = run_and_report(marker_command, 1, 10, True, CleanupFailureDirectory, cleanup_directories.append)
    expected_cleanup = "foundation_stress=fail iteration=1/1\ntemporary directory cleanup failed: errno=5\n"
    if cleanup_code != 1 or cleanup_output.getvalue() != "" or cleanup_error.getvalue() != expected_cleanup:
        return False
    if not observed_directories_are_clean(cleanup_directories, 1, caller_directory):
        return False

    parallel_directories: List[List[Path]] = [[], []]
    parallel_command = [sys.executable, "-c", "import time; time.sleep(0.1)"]
    with ThreadPoolExecutor(max_workers=2) as executor:
        futures = [executor.submit(run_repeated, parallel_command, 2, 10, True, controlled_directory_factory, parallel_directories[index].append) for index in range(2)]
        parallel_results = [future.result() for future in futures]
    flattened_directories = parallel_directories[0] + parallel_directories[1]
    if not all(result[0] and result[1] == 2 for result in parallel_results) or len(flattened_directories) != 4:
        return False
    if not observed_directories_are_clean(flattened_directories, 4, caller_directory):
        return False

    legacy_passed, _, _ = run_repeated(marker_command, 1, 10)
    legacy_marker = caller_directory / marker_name
    if not legacy_passed or not legacy_marker.is_file() or Path.cwd() != caller_directory:
        return False
    if [path.name for path in caller_directory.iterdir()] != [marker_name]:
        return False
    if output_tail("0\n1\n2\n3", 2) != "2\n3":
        return False
    return True


def self_test() -> int:
    """専用caller cwdを片付けながら既定動作と分離動作を検証する。"""
    previous_directory = Path.cwd()
    caller_directory: Optional[Path] = None
    cases_passed = False
    try:
        with tempfile.TemporaryDirectory(prefix="acs_foundation_stress_self_test_") as directory:
            caller_directory = Path(directory)
            os.chdir(str(caller_directory))
            try:
                cases_passed = run_self_test_cases(caller_directory)
            finally:
                os.chdir(str(previous_directory))
    except OSError:
        return 1
    if caller_directory is None or caller_directory.exists() or Path.cwd() != previous_directory:
        return 1
    return 0 if cases_passed else 1


def main() -> int:
    """引数を検証し、反復stressの成否を終了codeへ反映する。"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--iterations", type=int, default=32)
    parser.add_argument("--timeout-seconds", type=int, default=60)
    parser.add_argument("--isolate-cwd", action="store_true")
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
    return run_and_report([str(executable)], args.iterations, args.timeout_seconds, args.isolate_cwd)


if __name__ == "__main__":
    raise SystemExit(main())
