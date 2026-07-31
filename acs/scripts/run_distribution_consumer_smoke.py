#!/usr/bin/env python3
"""単一header配布物を一時consumerから実link・実行する。"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Sequence


SUPPORTED_CONFIGURATIONS = ("Debug", "Release")
EXECUTABLE_NAME = "acs_distribution_consumer.exe"
# 統合libraryが利用するWindows APIを配布headerから解決する必須library。
REQUIRED_HEADER_AUTO_LINK_LIBRARIES = ("advapi32.lib",)

CMAKE_PROJECT = r"""cmake_minimum_required(VERSION 3.24)
project(ACSDistributionConsumer LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "ACS distribution consumer smoke requires Windows and MSVC")
endif()
if(NOT DEFINED ACS_DISTRIBUTION_ROOT)
    message(FATAL_ERROR "ACS_DISTRIBUTION_ROOT is required")
endif()

# CMake/MSVC既定値のadvapi32を除き、配布headerの自動linkだけを検証する。
separate_arguments(ACS_CXX_STANDARD_LIBRARIES NATIVE_COMMAND
    "${CMAKE_CXX_STANDARD_LIBRARIES}")
list(FILTER ACS_CXX_STANDARD_LIBRARIES EXCLUDE REGEX
    "^[Aa][Dd][Vv][Aa][Pp][Ii]32(\\.lib)?$")
list(JOIN ACS_CXX_STANDARD_LIBRARIES " " CMAKE_CXX_STANDARD_LIBRARIES)
string(TOLOWER "${CMAKE_CXX_STANDARD_LIBRARIES}"
    ACS_CXX_STANDARD_LIBRARIES_LOWER)
if(ACS_CXX_STANDARD_LIBRARIES_LOWER MATCHES
        "(^|[ \t;])advapi32(\\.lib)?([ \t;]|$)")
    message(FATAL_ERROR "advapi32 must not enter through CMake standard libraries")
endif()

add_executable(acs_distribution_consumer
    "${ACS_DISTRIBUTION_ROOT}/examples/check.cpp")
target_compile_features(acs_distribution_consumer PRIVATE cxx_std_20)
target_compile_definitions(acs_distribution_consumer PRIVATE _HAS_EXCEPTIONS=1)
target_compile_options(acs_distribution_consumer PRIVATE
    /utf-8 /EHsc /GR- /permissive- /Zc:__cplusplus /Zc:preprocessor)
target_include_directories(acs_distribution_consumer PRIVATE
    "${ACS_DISTRIBUTION_ROOT}")
target_link_directories(acs_distribution_consumer PRIVATE
    "${ACS_DISTRIBUTION_ROOT}/lib/x64/$<CONFIG>")
set_property(TARGET acs_distribution_consumer PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
"""


def normalize_configuration(value: str) -> str:
    """Debug/Releaseだけを受け入れ、pathへ任意値を流さない。"""
    for configuration in SUPPORTED_CONFIGURATIONS:
        if value.casefold() == configuration.casefold():
            return configuration
    raise ValueError(f"unsupported configuration: {value}")


def required_distribution_files(root: Path, configuration: str) -> tuple[Path, ...]:
    """consumerの構文・link・実行に必要な最小配布fileを返す。"""
    return (
        root / "acs.h",
        root / "examples" / "check.cpp",
        root / "lib" / "x64" / configuration / "acs.lib",
    )


def invalid_auto_link_libraries(header_path: Path) -> tuple[str, ...]:
    """配布headerで自動link指示が欠落または重複する必須libraryを返す。"""
    header_text = header_path.read_text(encoding="utf-8")
    return tuple(
        library_name
        for library_name in REQUIRED_HEADER_AUTO_LINK_LIBRARIES
        if header_text.count(f'#pragma comment(lib, "{library_name}")') != 1
    )


def make_configure_command(
    cmake: str,
    source_directory: Path,
    build_directory: Path,
    distribution_root: Path,
    configuration: str,
    generator: str,
    generator_platform: str,
    generator_toolset: str,
) -> list[str]:
    """親buildと同じgenerator条件の一時consumer configure命令を作る。"""
    command = [
        cmake,
        "-S",
        str(source_directory),
        "-B",
        str(build_directory),
        "-G",
        generator,
    ]
    if generator_platform:
        command.extend(["-A", generator_platform])
    if generator_toolset:
        command.extend(["-T", generator_toolset])
    command.extend(
        [
            f"-DCMAKE_BUILD_TYPE={configuration}",
            f"-DACS_DISTRIBUTION_ROOT={distribution_root}",
        ]
    )
    return command


def run_command(command: Sequence[str], timeout_seconds: int) -> subprocess.CompletedProcess[str]:
    """外部commandを実行し、失敗診断を呼び出し側へ保持する。"""
    return subprocess.run(
        list(command),
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout_seconds,
    )


def print_failure(name: str, result: subprocess.CompletedProcess[str]) -> None:
    """失敗した外部commandの標準出力と標準errorを明示する。"""
    print(
        f"distribution_consumer_smoke=fail step={name} "
        f"exit={result.returncode}",
        file=sys.stderr,
    )
    if result.stdout:
        print(result.stdout, file=sys.stderr)
    if result.stderr:
        print(result.stderr, file=sys.stderr)


def find_consumer_executable(build_directory: Path) -> Path:
    """一時build内のconsumer実行fileを一意に解決する。"""
    candidates = sorted(build_directory.rglob(EXECUTABLE_NAME))
    if len(candidates) != 1:
        raise RuntimeError(
            "consumer executable count is not one: "
            f"{len(candidates)} under {build_directory}"
        )
    return candidates[0]


def run_smoke(args: argparse.Namespace) -> int:
    """配布物をrepo外でconfigure、link、実行し結果を返す。"""
    if os.name != "nt":
        print(
            "distribution_consumer_smoke=fail reason=Windows/MSVC required",
            file=sys.stderr,
        )
        return 2

    try:
        configuration = normalize_configuration(args.configuration)
    except ValueError as error:
        print(f"distribution_consumer_smoke=fail reason={error}", file=sys.stderr)
        return 2

    distribution_root = args.distribution_root.resolve()
    missing = [
        str(path)
        for path in required_distribution_files(distribution_root, configuration)
        if not path.is_file() or path.stat().st_size <= 0
    ]
    if missing:
        print(
            "distribution_consumer_smoke=fail missing="
            + ", ".join(missing),
            file=sys.stderr,
        )
        return 2

    try:
        invalid_auto_links = invalid_auto_link_libraries(distribution_root / "acs.h")
    except (OSError, UnicodeError) as error:
        print(
            f"distribution_consumer_smoke=fail reason=auto-link-read: {error}",
            file=sys.stderr,
        )
        return 2
    if invalid_auto_links:
        print(
            "distribution_consumer_smoke=fail invalid_auto_link="
            + ", ".join(invalid_auto_links),
            file=sys.stderr,
        )
        return 2

    try:
        with tempfile.TemporaryDirectory(
            prefix=f"acs-distribution-consumer-{configuration.lower()}-"
        ) as temporary:
            root = Path(temporary)
            source_directory = root / "source"
            build_directory = root / "build"
            source_directory.mkdir()
            (source_directory / "CMakeLists.txt").write_text(
                CMAKE_PROJECT,
                encoding="utf-8",
                newline="\n",
            )

            configure = run_command(
                make_configure_command(
                    args.cmake,
                    source_directory,
                    build_directory,
                    distribution_root,
                    configuration,
                    args.generator,
                    args.generator_platform,
                    args.generator_toolset,
                ),
                args.timeout_seconds,
            )
            if configure.returncode != 0:
                print_failure("configure", configure)
                return 1

            build = run_command(
                [
                    args.cmake,
                    "--build",
                    str(build_directory),
                    "--config",
                    configuration,
                    "--parallel",
                    "1",
                ],
                args.timeout_seconds,
            )
            if build.returncode != 0:
                print_failure("build", build)
                return 1

            executable = find_consumer_executable(build_directory)
            execute = run_command([str(executable)], args.timeout_seconds)
            if execute.returncode != 0:
                print_failure("execute", execute)
                return 1
            if "acs.h OK" not in execute.stdout:
                print(
                    "distribution_consumer_smoke=fail "
                    "reason=success marker missing",
                    file=sys.stderr,
                )
                return 1
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(
            f"distribution_consumer_smoke=fail reason={error}",
            file=sys.stderr,
        )
        return 1

    print(
        "distribution_consumer_smoke=pass "
        f"configuration={configuration}"
    )
    return 0


def self_test() -> int:
    """引数制限、path契約、generator命令、実行file解決を固定する。"""
    with tempfile.TemporaryDirectory(prefix="acs-distribution-smoke-selftest-") as temporary:
        root = Path(temporary)
        expected = required_distribution_files(root, "Debug")
        build_directory = root / "build"
        executable = build_directory / "Debug" / EXECUTABLE_NAME
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"probe")
        valid_header = root / "valid-acs.h"
        valid_header.write_text(
            '#pragma comment(lib, "advapi32.lib")\n',
            encoding="utf-8",
            newline="\n",
        )
        missing_header = root / "missing-acs.h"
        missing_header.write_text(
            '#pragma comment(lib, "user32.lib")\n',
            encoding="utf-8",
            newline="\n",
        )
        duplicate_header = root / "duplicate-acs.h"
        duplicate_header.write_text(
            '#pragma comment(lib, "advapi32.lib")\n'
            '#pragma comment(lib, "advapi32.lib")\n',
            encoding="utf-8",
            newline="\n",
        )
        command = make_configure_command(
            "cmake",
            root / "source",
            build_directory,
            root,
            "Release",
            "Visual Studio Test",
            "x64",
            "vTest",
        )
        try:
            normalize_configuration("profile")
            rejects_invalid = False
        except ValueError:
            rejects_invalid = True
        valid = (
            normalize_configuration("debug") == "Debug"
            and normalize_configuration("RELEASE") == "Release"
            and expected[0] == root / "acs.h"
            and expected[2] == root / "lib" / "x64" / "Debug" / "acs.lib"
            and command[-2:] == [
                "-DCMAKE_BUILD_TYPE=Release",
                f"-DACS_DISTRIBUTION_ROOT={root}",
            ]
            and command[command.index("-A") + 1] == "x64"
            and command[command.index("-T") + 1] == "vTest"
            and find_consumer_executable(build_directory) == executable
            and invalid_auto_link_libraries(valid_header) == ()
            and invalid_auto_link_libraries(missing_header) == ("advapi32.lib",)
            and invalid_auto_link_libraries(duplicate_header) == ("advapi32.lib",)
            and "_HAS_EXCEPTIONS=1" in CMAKE_PROJECT
            and "/EHsc" in CMAKE_PROJECT
            and "/EHs-c-" not in CMAKE_PROJECT
            and "list(FILTER ACS_CXX_STANDARD_LIBRARIES EXCLUDE REGEX" in CMAKE_PROJECT
            and "target_link_libraries" not in CMAKE_PROJECT
            and rejects_invalid
        )
    return 0 if valid else 1


def main() -> int:
    """CLI引数を検証してself-testまたは実smokeを実行する。"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--distribution-root", type=Path)
    parser.add_argument("--configuration")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--generator")
    parser.add_argument("--generator-platform", default="")
    parser.add_argument("--generator-toolset", default="")
    parser.add_argument("--timeout-seconds", type=int, default=300)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.distribution_root is None:
        parser.error("--distribution-root is required unless --self-test is used")
    if args.configuration is None:
        parser.error("--configuration is required unless --self-test is used")
    if not args.generator:
        parser.error("--generator is required unless --self-test is used")
    if args.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")
    return run_smoke(args)


if __name__ == "__main__":
    raise SystemExit(main())
