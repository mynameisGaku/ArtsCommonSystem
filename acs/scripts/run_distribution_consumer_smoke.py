#!/usr/bin/env python3
"""単一header配布物を一時consumerから実link・実行する。"""

from __future__ import annotations

import argparse
from contextlib import contextmanager, redirect_stderr, redirect_stdout
import ctypes
from ctypes import wintypes
from dataclasses import dataclass
import hashlib
import io
import os
import re
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import BinaryIO, Callable, Iterator, Optional, Sequence


SUPPORTED_CONFIGURATIONS = ("Debug", "Release")
EXECUTABLE_NAME = "acs_distribution_consumer.exe"
MANIFEST_NAME = "acs-distribution.sha256"
MANIFEST_SCHEMA = "ACS_DIST_SHA256_V2"
DISTRIBUTION_LIBRARY_NAMES = (
    "acs.lib",
    "Diligent-Archiver-static.lib",
    "Diligent-BasicPlatform.lib",
    "Diligent-Common.lib",
    "Diligent-GraphicsAccessories.lib",
    "Diligent-GraphicsEngine.lib",
    "Diligent-GraphicsEngineD3D12-static.lib",
    "Diligent-GraphicsEngineD3DBase.lib",
    "Diligent-GraphicsEngineNextGenBase.lib",
    "Diligent-GraphicsTools.lib",
    "Diligent-Primitives.lib",
    "Diligent-ShaderTools.lib",
    "Diligent-Win32Platform.lib",
    "xxhash.lib",
)
# 完全mirrorへ含める製品とthird-party licenseの固定相対path。
LICENSE_RELATIVE_PATHS = (
    "Licenses/ACS-License.txt",
    "Licenses/ThirdParty/DXC-License.txt",
    "Licenses/ThirdParty/DXC-ThirdPartyNotices.txt",
    "Licenses/ThirdParty/DearImGui-License.txt",
    "Licenses/ThirdParty/DiligentCore-License.txt",
    "Licenses/ThirdParty/GPUOpenShaderUtils-License.txt",
    "Licenses/ThirdParty/cgltf-License.txt",
    "Licenses/ThirdParty/dr_libs-License.txt",
    "Licenses/ThirdParty/mimalloc-License.txt",
    "Licenses/ThirdParty/stb-License.txt",
    "Licenses/ThirdParty/ufbx-License.txt",
    "Licenses/ThirdParty/xxHash-License.txt",
)
# producerから独立して固定するlibrary、license、manifest、tree、directory件数。
EXPECTED_LIBRARY_COUNT = 14
EXPECTED_LICENSE_COUNT = 12
EXPECTED_MANIFEST_PAYLOAD_COUNT = 44
EXPECTED_MIRROR_FILE_COUNT = 45
EXPECTED_MIRROR_DIRECTORY_COUNT = 7
MANIFEST_RELATIVE_PATHS = tuple(
    sorted(
        (
            "README.md",
            "acs.h",
            "verification/build_consumer_contract.cmd",
            "verification/consumer_contract.cpp",
            *LICENSE_RELATIVE_PATHS,
            *(
                f"lib/x64/{configuration}/{library}"
                for configuration in SUPPORTED_CONFIGURATIONS
                for library in DISTRIBUTION_LIBRARY_NAMES
            ),
        )
    )
)
MIRROR_RELATIVE_PATHS = tuple(
    sorted(
        (
            MANIFEST_NAME,
            *MANIFEST_RELATIVE_PATHS,
        )
    )
)
MIRROR_RELATIVE_DIRECTORIES = (
    "Licenses",
    "Licenses/ThirdParty",
    "lib",
    "lib/x64",
    "lib/x64/Debug",
    "lib/x64/Release",
    "verification",
)
MANIFEST_ENTRY = re.compile(r"^([0-9A-F]{64})  ([A-Za-z0-9._/-]+)$")
COPY_BUFFER_BYTES = 1024 * 1024
# named manifest 外の公式 consumer source も検証対象の内容へ固定する。
EXPECTED_CONSUMER_CONTRACT_SHA256 = "874DDE558258DDCAF5F0180EF7B0CA3FF10A8B0015280D68434E2B0E27BB1E21"
FILE_ATTRIBUTE_DIRECTORY = 0x10
FILE_ATTRIBUTE_REPARSE_POINT = 0x400
FILE_FLAG_BACKUP_SEMANTICS = 0x02000000
FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000
FILE_LIST_DIRECTORY = 0x0001
FILE_READ_ATTRIBUTES = 0x0080
GENERIC_READ = 0x80000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
FILE_INFO_BY_HANDLE_CLASS_FILE_ID_INFO = 18
JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
WAIT_OBJECT_0 = 0
SYNCHRONIZE = 0x00100000
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
CREATE_NO_WINDOW = 0x08000000
ERROR_INVALID_PARAMETER = 87
ERROR_SHARING_VIOLATION = 32
# 統合libraryが利用するWindows APIを配布headerから解決する必須library。
REQUIRED_HEADER_AUTO_LINK_LIBRARIES = ("advapi32.lib",)


class DistributionValidationError(RuntimeError):
    """配布rootまたはmanifestが公開契約を満たさないことを表す。"""


@dataclass(frozen=True)
class _PinnedMirror:
    """pin済みmirror rootと各objectのWin32 identityを保持する。"""

    root: Path
    identities: dict[Path, tuple[int, int]]

    def expected_identity(self, path: Path) -> tuple[int, int]:
        """実read対象に対応するpin取得時identityを返す。"""
        try:
            return self.identities[path]
        except KeyError as error:
            raise DistributionValidationError(
                f"distribution path was not pinned: {path}"
            ) from error


class _ByHandleFileInformation(ctypes.Structure):
    """GetFileInformationByHandleが返すfile identityを保持する。"""

    _fields_ = (
        ("file_attributes", wintypes.DWORD),
        ("creation_time", wintypes.FILETIME),
        ("last_access_time", wintypes.FILETIME),
        ("last_write_time", wintypes.FILETIME),
        ("volume_serial_number", wintypes.DWORD),
        ("file_size_high", wintypes.DWORD),
        ("file_size_low", wintypes.DWORD),
        ("number_of_links", wintypes.DWORD),
        ("file_index_high", wintypes.DWORD),
        ("file_index_low", wintypes.DWORD),
    )


class _FileIdInformation(ctypes.Structure):
    """GetFileInformationByHandleExが返す完全なvolume/file IDを保持する。"""

    _fields_ = (
        ("volume_serial_number", ctypes.c_uint64),
        ("file_id", ctypes.c_ubyte * 16),
    )


class _IoCounters(ctypes.Structure):
    """Job Objectが保持するprocess I/O集計のWin32 layout。"""

    _fields_ = (
        ("read_operation_count", ctypes.c_uint64),
        ("write_operation_count", ctypes.c_uint64),
        ("other_operation_count", ctypes.c_uint64),
        ("read_transfer_count", ctypes.c_uint64),
        ("write_transfer_count", ctypes.c_uint64),
        ("other_transfer_count", ctypes.c_uint64),
    )


class _JobObjectBasicLimitInformation(ctypes.Structure):
    """Job Objectの終了規則を渡すWin32 layout。"""

    _fields_ = (
        ("per_process_user_time_limit", ctypes.c_int64),
        ("per_job_user_time_limit", ctypes.c_int64),
        ("limit_flags", wintypes.DWORD),
        ("minimum_working_set_size", ctypes.c_size_t),
        ("maximum_working_set_size", ctypes.c_size_t),
        ("active_process_limit", wintypes.DWORD),
        ("affinity", ctypes.c_size_t),
        ("priority_class", wintypes.DWORD),
        ("scheduling_class", wintypes.DWORD),
    )


class _JobObjectExtendedLimitInformation(ctypes.Structure):
    """子孫processをJob Objectへ閉じ込めるWin32 layout。"""

    _fields_ = (
        ("basic_limit_information", _JobObjectBasicLimitInformation),
        ("io_info", _IoCounters),
        ("process_memory_limit", ctypes.c_size_t),
        ("job_memory_limit", ctypes.c_size_t),
        ("peak_process_memory_used", ctypes.c_size_t),
        ("peak_job_memory_used", ctypes.c_size_t),
    )


def _fixed_contract_counts_match(counts: tuple[int, int, int, int, int]) -> bool:
    """producerと独立した5つのliteral件数契約に一致するかを返す。"""
    return counts == (
        EXPECTED_LIBRARY_COUNT,
        EXPECTED_LICENSE_COUNT,
        EXPECTED_MANIFEST_PAYLOAD_COUNT,
        EXPECTED_MIRROR_FILE_COUNT,
        EXPECTED_MIRROR_DIRECTORY_COUNT,
    )


def _assert_fixed_contract_counts() -> None:
    """libraryやlicense削除と派生集合の同時縮小を起動時に拒否する。"""
    counts = (
        len(DISTRIBUTION_LIBRARY_NAMES),
        len(LICENSE_RELATIVE_PATHS),
        len(MANIFEST_RELATIVE_PATHS),
        len(MIRROR_RELATIVE_PATHS),
        len(MIRROR_RELATIVE_DIRECTORIES),
    )
    if not _fixed_contract_counts_match(counts):
        raise DistributionValidationError(
            f"distribution fixed counts differ: {counts}"
        )


def _kernel32_function(name: str):
    """Windows API関数をlast-error対応で取得する。"""
    if os.name != "nt":
        raise DistributionValidationError("distribution snapshot pin requires Windows")
    return getattr(ctypes.WinDLL("kernel32", use_last_error=True), name)


def _open_windows_pin(path: Path, expected_directory: bool) -> tuple[int, tuple[int, int]]:
    """対象をhandle-boundで開き、rename/writeをsnapshot終了まで拒否する。"""
    create_file = _kernel32_function("CreateFileW")
    create_file.argtypes = (
        wintypes.LPCWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.LPVOID,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.HANDLE,
    )
    create_file.restype = wintypes.HANDLE
    desired_access = (
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES
        if expected_directory
        else GENERIC_READ
    )
    share_mode = FILE_SHARE_READ | (FILE_SHARE_WRITE if expected_directory else 0)
    flags = FILE_FLAG_OPEN_REPARSE_POINT | (
        FILE_FLAG_BACKUP_SEMANTICS if expected_directory else 0
    )
    handle = create_file(
        os.fspath(path),
        desired_access,
        share_mode,
        None,
        OPEN_EXISTING,
        flags,
        None,
    )
    invalid_handle = ctypes.c_void_p(-1).value
    if handle in {None, invalid_handle}:
        error = ctypes.get_last_error()
        raise DistributionValidationError(
            f"distribution path cannot be pinned: {path}: winerror={error}"
        )
    opened_handle = int(handle)
    try:
        get_information = _kernel32_function("GetFileInformationByHandle")
        get_information.argtypes = (
            wintypes.HANDLE,
            ctypes.POINTER(_ByHandleFileInformation),
        )
        get_information.restype = wintypes.BOOL
        information = _ByHandleFileInformation()
        if not get_information(opened_handle, ctypes.byref(information)):
            error = ctypes.get_last_error()
            raise DistributionValidationError(
                f"distribution path identity cannot be read: {path}: winerror={error}"
            )
        is_directory = bool(
            information.file_attributes & FILE_ATTRIBUTE_DIRECTORY
        )
        if (
            information.file_attributes & FILE_ATTRIBUTE_REPARSE_POINT
            or is_directory != expected_directory
        ):
            raise DistributionValidationError(
                f"distribution pin type differs: {path}"
            )
        legacy_identity = (
            int(information.volume_serial_number),
            (int(information.file_index_high) << 32)
            | int(information.file_index_low),
        )
        get_file_id = _kernel32_function("GetFileInformationByHandleEx")
        get_file_id.argtypes = (
            wintypes.HANDLE,
            ctypes.c_int,
            wintypes.LPVOID,
            wintypes.DWORD,
        )
        get_file_id.restype = wintypes.BOOL
        file_id_information = _FileIdInformation()
        if not get_file_id(
            opened_handle,
            FILE_INFO_BY_HANDLE_CLASS_FILE_ID_INFO,
            ctypes.byref(file_id_information),
            ctypes.sizeof(file_id_information),
        ):
            error = ctypes.get_last_error()
            raise DistributionValidationError(
                f"distribution path full identity cannot be read: {path}: "
                f"winerror={error}"
            )
        full_identity = _file_id_information_identity(file_id_information)
        identity = _select_windows_stat_identity(
            path,
            _assert_normal_path(path, expected_directory),
            legacy_identity,
            full_identity,
        )
    except BaseException:
        _close_windows_handle(opened_handle)
        raise
    return opened_handle, identity


def _close_windows_handle(handle: int) -> None:
    """取得済みWindows handleを閉じる。"""
    close_handle = _kernel32_function("CloseHandle")
    close_handle.argtypes = (wintypes.HANDLE,)
    close_handle.restype = wintypes.BOOL
    close_handle(handle)


def _current_process_handle_count() -> int:
    """self-test前後で現在processのWindows handle数を取得する。"""
    get_current_process = _kernel32_function("GetCurrentProcess")
    get_current_process.argtypes = ()
    get_current_process.restype = wintypes.HANDLE
    get_process_handle_count = _kernel32_function("GetProcessHandleCount")
    get_process_handle_count.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
    get_process_handle_count.restype = wintypes.BOOL
    count = wintypes.DWORD()
    if not get_process_handle_count(get_current_process(), ctypes.byref(count)):
        error = ctypes.get_last_error()
        raise DistributionValidationError(
            f"current process handle count cannot be read: winerror={error}"
        )
    return int(count.value)


def _has_reparse_attribute(path_stat: os.stat_result) -> bool:
    """Windowsのreparse属性を追従せず判定する。"""
    attributes = getattr(path_stat, "st_file_attributes", 0)
    reparse_attribute = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    return bool(attributes & reparse_attribute)


def _path_identity(path_stat: os.stat_result) -> tuple[int, int, int, int]:
    """copy前後で同じ通常fileを読んだことを確認する識別値を返す。"""
    return (
        path_stat.st_dev,
        path_stat.st_ino,
        path_stat.st_size,
        path_stat.st_mtime_ns,
    )


def _assert_normal_path(path: Path, expected_directory: bool) -> os.stat_result:
    """pathを追従せず、通常directoryまたは通常fileであることを確認する。"""
    try:
        path_stat = os.lstat(path)
    except OSError as error:
        raise DistributionValidationError(f"distribution path is unavailable: {path}: {error}") from error
    if _has_reparse_attribute(path_stat):
        raise DistributionValidationError(f"distribution path is a reparse point: {path}")
    if expected_directory and not stat.S_ISDIR(path_stat.st_mode):
        raise DistributionValidationError(f"distribution path is not a directory: {path}")
    if not expected_directory and not stat.S_ISREG(path_stat.st_mode):
        raise DistributionValidationError(f"distribution path is not a regular file: {path}")
    return path_stat


def _stat_object_identity(path_stat: os.stat_result) -> tuple[int, int]:
    """stat結果からvolumeとfile IDを取得し、欠落時はfail-closedにする。"""
    identity = (int(path_stat.st_dev), int(path_stat.st_ino))
    if not identity[0] or not identity[1]:
        raise DistributionValidationError(
            f"distribution stat identity is unavailable: {identity}"
        )
    return identity


def _file_id_information_identity(
    information: _FileIdInformation,
) -> tuple[int, int]:
    """Windowsの完全なvolume/file IDをPythonのstatと同じ整数組へ変換する。"""
    identity = (
        int(information.volume_serial_number),
        int.from_bytes(bytes(information.file_id), "little"),
    )
    if not identity[0] or not identity[1]:
        raise DistributionValidationError(
            f"distribution handle identity is unavailable: {identity}"
        )
    return identity


def _select_windows_stat_identity(
    path: Path,
    path_stat: os.stat_result,
    legacy_identity: tuple[int, int],
    full_identity: tuple[int, int],
) -> tuple[int, int]:
    """実行中のPythonが公開するWindows識別子を固定handleの候補と照合する。"""
    actual_identity = _stat_object_identity(path_stat)
    if actual_identity not in {legacy_identity, full_identity}:
        raise DistributionValidationError(
            f"distribution path identity does not match its handle: {path}: "
            f"legacy={legacy_identity} full={full_identity} "
            f"actual={actual_identity}"
        )
    return actual_identity


def _assert_pinned_identity(
    path: Path,
    expected_identity: tuple[int, int],
    expected_directory: bool,
) -> None:
    """path側のvolumeとfile IDをhandle取得時の同一objectへ固定する。"""
    path_stat = _assert_normal_path(path, expected_directory)
    actual_identity = _stat_object_identity(path_stat)
    if not expected_identity[0] or not expected_identity[1] or actual_identity != expected_identity:
        raise DistributionValidationError(
            f"distribution path identity changed: {path}: "
            f"expected={expected_identity} actual={actual_identity}"
        )


def _assert_normal_ancestors(path: Path) -> None:
    """既存ancestorをrootまで追従せず検査する。"""
    current = path
    while True:
        _assert_normal_path(current, True)
        if current.parent == current:
            break
        current = current.parent


def _distribution_path(root: Path, relative_path: str, expected_directory: bool = False) -> Path:
    """固定相対pathをroot内へ解決し、全componentをno-followで検査する。"""
    parts = relative_path.split("/")
    if (
        not relative_path
        or relative_path.startswith("/")
        or "\\" in relative_path
        or any(part in {"", ".", ".."} for part in parts)
    ):
        raise DistributionValidationError(f"unsafe distribution relative path: {relative_path}")
    current = root
    for index, part in enumerate(parts):
        current = current / part
        _assert_normal_path(current, expected_directory or index + 1 != len(parts))
    return current


def _enumerate_mirror(root: Path) -> tuple[tuple[str, ...], tuple[str, ...]]:
    """mirrorをno-followで列挙し、fileとdirectoryの相対pathを返す。"""
    files: list[str] = []
    directories: list[str] = []

    def visit(directory: Path, relative_parent: str) -> None:
        try:
            entries = sorted(os.scandir(directory), key=lambda item: item.name)
        except OSError as error:
            raise DistributionValidationError(
                f"distribution directory cannot be listed: {directory}: {error}"
            ) from error
        for entry in entries:
            relative = f"{relative_parent}/{entry.name}" if relative_parent else entry.name
            try:
                entry_stat = entry.stat(follow_symlinks=False)
            except OSError as error:
                raise DistributionValidationError(
                    f"distribution entry cannot be inspected: {entry.path}: {error}"
                ) from error
            if _has_reparse_attribute(entry_stat):
                raise DistributionValidationError(f"distribution entry is a reparse point: {entry.path}")
            if stat.S_ISDIR(entry_stat.st_mode):
                directories.append(relative)
                visit(Path(entry.path), relative)
            elif stat.S_ISREG(entry_stat.st_mode):
                files.append(relative)
            else:
                raise DistributionValidationError(f"distribution entry has an unsupported type: {entry.path}")

    visit(root, "")
    return tuple(sorted(files)), tuple(sorted(directories))


def _assert_complete_mirror_shape(root: Path) -> None:
    """固定45 file、7 directory、非emptyのmirror形状を確認する。"""
    _assert_fixed_contract_counts()
    files, directories = _enumerate_mirror(root)
    if files != MIRROR_RELATIVE_PATHS:
        raise DistributionValidationError(
            f"distribution file set differs: expected={len(MIRROR_RELATIVE_PATHS)} actual={len(files)}"
        )
    if directories != MIRROR_RELATIVE_DIRECTORIES:
        raise DistributionValidationError(
            "distribution directory set differs: "
            f"expected={MIRROR_RELATIVE_DIRECTORIES} actual={directories}"
        )
    for relative_path in MIRROR_RELATIVE_PATHS:
        path_stat = _assert_normal_path(
            root / Path(*relative_path.split("/")),
            False,
        )
        if path_stat.st_size <= 0:
            raise DistributionValidationError(
                f"distribution file is empty: {relative_path}"
            )


def validate_complete_mirror(root: Path) -> Path:
    """配布rootの物理境界と固定45 file集合を確認する。"""
    normalized = Path(os.path.abspath(os.fspath(root)))
    _assert_normal_ancestors(normalized)
    _assert_complete_mirror_shape(normalized)
    return normalized


@contextmanager
def pin_complete_mirror(root: Path) -> Iterator[_PinnedMirror]:
    """全mirror objectをread-only pinし、世代差替えと書換えを拒否する。"""
    normalized = Path(os.path.abspath(os.fspath(root)))
    _assert_normal_ancestors(normalized)
    if os.name != "nt":
        _assert_complete_mirror_shape(normalized)
        identities = {
            normalized / Path(*relative_path.split("/")): _stat_object_identity(
                os.lstat(normalized / Path(*relative_path.split("/")))
            )
            for relative_path in MIRROR_RELATIVE_PATHS
        }
        yield _PinnedMirror(normalized, identities)
        return

    handles: list[int] = []
    pinned_identities: dict[Path, tuple[int, int]] = {}
    directory_paths: set[Path] = set()
    try:
        ancestors: list[Path] = []
        current = normalized
        while True:
            ancestors.append(current)
            if current.parent == current:
                break
            current = current.parent
        for directory in ancestors:
            handle, identity = _open_windows_pin(directory, True)
            handles.append(handle)
            pinned_identities[directory] = identity
            directory_paths.add(directory)

        _assert_complete_mirror_shape(normalized)
        for relative_directory in MIRROR_RELATIVE_DIRECTORIES:
            directory = normalized / Path(*relative_directory.split("/"))
            handle, identity = _open_windows_pin(directory, True)
            handles.append(handle)
            pinned_identities[directory] = identity
            directory_paths.add(directory)
        for relative_path in MIRROR_RELATIVE_PATHS:
            path = normalized / Path(*relative_path.split("/"))
            handle, identity = _open_windows_pin(path, False)
            handles.append(handle)
            pinned_identities[path] = identity

        _assert_complete_mirror_shape(normalized)
        for path, identity in pinned_identities.items():
            _assert_pinned_identity(path, identity, path in directory_paths)
        yield _PinnedMirror(normalized, pinned_identities)
        _assert_complete_mirror_shape(normalized)
        for path, identity in pinned_identities.items():
            _assert_pinned_identity(path, identity, path in directory_paths)
    finally:
        for handle in reversed(handles):
            _close_windows_handle(handle)


def parse_distribution_manifest(manifest_bytes: bytes) -> dict[str, str]:
    """named manifestのcanonical byte列と固定44 entryを厳密に検証する。"""
    _assert_fixed_contract_counts()
    if manifest_bytes.startswith(b"\xef\xbb\xbf") or b"\r" in manifest_bytes or not manifest_bytes.endswith(b"\n"):
        raise DistributionValidationError("distribution manifest is not canonical LF UTF-8 without BOM")
    try:
        text = manifest_bytes.decode("ascii")
    except UnicodeDecodeError as error:
        raise DistributionValidationError("distribution manifest is not ASCII") from error
    lines = text[:-1].split("\n")
    if len(lines) != 1 + len(MANIFEST_RELATIVE_PATHS) or lines[0] != MANIFEST_SCHEMA:
        raise DistributionValidationError("distribution manifest schema or entry count differs")
    entries: dict[str, str] = {}
    casefolded_paths: set[str] = set()
    for line in lines[1:]:
        match = MANIFEST_ENTRY.fullmatch(line)
        if match is None:
            raise DistributionValidationError(f"distribution manifest entry is not canonical: {line!r}")
        file_hash, relative_path = match.groups()
        if relative_path.casefold() in casefolded_paths:
            raise DistributionValidationError(f"distribution manifest path is duplicated: {relative_path}")
        casefolded_paths.add(relative_path.casefold())
        entries[relative_path] = file_hash
    if tuple(entries) != MANIFEST_RELATIVE_PATHS:
        raise DistributionValidationError("distribution manifest paths differ from the fixed contract")
    canonical = MANIFEST_SCHEMA + "\n" + "".join(
        f"{entries[path]}  {path}\n" for path in MANIFEST_RELATIVE_PATHS
    )
    if canonical.encode("ascii") != manifest_bytes:
        raise DistributionValidationError("distribution manifest bytes are not canonical")
    return entries


def _assert_read_identity(
    source: Path,
    path_stat: os.stat_result,
    expected_identity: tuple[int, int],
) -> None:
    """実read handleがpin取得時と同じvolume/file IDを指すことを確認する。"""
    actual_identity = _stat_object_identity(path_stat)
    if actual_identity != expected_identity:
        raise DistributionValidationError(
            f"distribution read handle identity differs: {source}: "
            f"expected={expected_identity} actual={actual_identity}"
        )


def _read_regular_file(
    source: Path,
    expected_identity: tuple[int, int],
) -> bytes:
    """pin済みobjectと同じhandleから通常file全体を読む。"""
    before = _assert_normal_path(source, False)
    _assert_read_identity(source, before, expected_identity)
    try:
        with source.open("rb") as input_file:
            opened = os.fstat(input_file.fileno())
            _assert_read_identity(source, opened, expected_identity)
            payload = input_file.read()
    except OSError as error:
        raise DistributionValidationError(
            f"distribution file cannot be read: {source}: {error}"
        ) from error
    after = _assert_normal_path(source, False)
    _assert_read_identity(source, after, expected_identity)
    if _path_identity(before) != _path_identity(after):
        raise DistributionValidationError(
            f"distribution source changed while reading: {source}"
        )
    return payload


def _read_manifest(mirror: _PinnedMirror) -> tuple[bytes, dict[str, str]]:
    """manifestをpin済みidentityから読み、byte列と検証済みentryを返す。"""
    manifest_path = _distribution_path(mirror.root, MANIFEST_NAME)
    manifest_bytes = _read_regular_file(
        manifest_path,
        mirror.expected_identity(manifest_path),
    )
    return manifest_bytes, parse_distribution_manifest(manifest_bytes)


def _copy_and_hash(
    source: Path,
    destination: Path,
    expected_identity: tuple[int, int],
    source_opener: Optional[Callable[[Path], BinaryIO]] = None,
) -> tuple[str, int]:
    """同じ通常fileをstream copyし、copyしたbyte列のSHAとsizeを返す。"""
    before = _assert_normal_path(source, False)
    _assert_read_identity(source, before, expected_identity)
    destination.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    size = 0
    opener = source_opener or (lambda path: path.open("rb"))
    try:
        with opener(source) as input_file:
            opened = os.fstat(input_file.fileno())
            _assert_read_identity(source, opened, expected_identity)
            with destination.open("xb") as output_file:
                while True:
                    chunk = input_file.read(COPY_BUFFER_BYTES)
                    if not chunk:
                        break
                    output_file.write(chunk)
                    digest.update(chunk)
                    size += len(chunk)
    except OSError as error:
        raise DistributionValidationError(f"distribution snapshot copy failed: {source}: {error}") from error
    after = _assert_normal_path(source, False)
    _assert_read_identity(source, after, expected_identity)
    if _path_identity(before) != _path_identity(after):
        raise DistributionValidationError(f"distribution source changed during snapshot: {source}")
    return digest.hexdigest().upper(), size


def _hash_regular_file(
    source: Path,
    expected_identity: tuple[int, int],
) -> tuple[str, int]:
    """通常fileをidentity bracket内でhashし、SHAとsizeを返す。"""
    before = _assert_normal_path(source, False)
    _assert_read_identity(source, before, expected_identity)
    digest = hashlib.sha256()
    size = 0
    try:
        with source.open("rb") as input_file:
            opened = os.fstat(input_file.fileno())
            _assert_read_identity(source, opened, expected_identity)
            while True:
                chunk = input_file.read(COPY_BUFFER_BYTES)
                if not chunk:
                    break
                digest.update(chunk)
                size += len(chunk)
    except OSError as error:
        raise DistributionValidationError(f"distribution hash failed: {source}: {error}") from error
    after = _assert_normal_path(source, False)
    _assert_read_identity(source, after, expected_identity)
    if _path_identity(before) != _path_identity(after):
        raise DistributionValidationError(f"distribution source changed while hashing: {source}")
    return digest.hexdigest().upper(), size


def create_verified_snapshot(
    live_root: Path,
    snapshot_root: Path,
    configuration: str,
    expected_consumer_contract_sha256: str,
    copy_hook: Optional[Callable[[str, int], None]] = None,
    commit_hook: Optional[Callable[[], None]] = None,
) -> Path:
    """manifestと一致する選択構成を専用rootへ複製し、live参照を切る。"""
    _assert_normal_path(snapshot_root.parent, True)
    snapshot_root.mkdir()
    with pin_complete_mirror(live_root) as mirror:
        manifest_before, entries = _read_manifest(mirror)
        selected_paths = frozenset(
            (
                "README.md",
                "acs.h",
                "verification/build_consumer_contract.cmd",
                *LICENSE_RELATIVE_PATHS,
                *(
                    f"lib/x64/{configuration}/{library}"
                    for library in DISTRIBUTION_LIBRARY_NAMES
                ),
            )
        )
        for index, relative_path in enumerate(MANIFEST_RELATIVE_PATHS):
            source = _distribution_path(mirror.root, relative_path)
            source_identity = mirror.expected_identity(source)
            if relative_path in selected_paths:
                file_hash, size = _copy_and_hash(
                    source,
                    snapshot_root / Path(*relative_path.split("/")),
                    source_identity,
                )
            else:
                file_hash, size = _hash_regular_file(source, source_identity)
            if size <= 0 or file_hash != entries[relative_path]:
                raise DistributionValidationError(
                    f"distribution payload hash differs: {relative_path}"
                )
            if copy_hook is not None:
                try:
                    copy_hook(relative_path, index)
                except OSError as error:
                    raise DistributionValidationError(
                        "distribution live payload mutation was blocked: "
                        f"{relative_path}"
                    ) from error

        contract_source = _distribution_path(mirror.root, "verification/consumer_contract.cpp")
        contract_identity = mirror.expected_identity(contract_source)
        contract_before_hash, contract_before_size = _hash_regular_file(
            contract_source,
            contract_identity,
        )
        contract_hash, contract_size = _copy_and_hash(
            contract_source,
            snapshot_root / "verification" / "consumer_contract.cpp",
            contract_identity,
        )
        contract_after_hash, contract_after_size = _hash_regular_file(
            contract_source,
            contract_identity,
        )
        if (
            contract_size <= 0
            or contract_before_size != contract_size
            or contract_after_size != contract_size
            or contract_before_hash != contract_hash
            or contract_after_hash != contract_hash
            or contract_hash != expected_consumer_contract_sha256
        ):
            raise DistributionValidationError(
                "distribution consumer contract differs from the canonical source"
            )
    if commit_hook is not None:
        commit_hook()
    # copy 後に pin を取り直し、検証中の差し替えを manifest byte で拒否する。
    with pin_complete_mirror(live_root) as mirror:
        manifest_after, _ = _read_manifest(mirror)
    if manifest_before != manifest_after:
        raise DistributionValidationError(
            "distribution manifest changed during snapshot"
        )
    return snapshot_root

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
    "${ACS_DISTRIBUTION_ROOT}/verification/consumer_contract.cpp")
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
        root / "verification" / "consumer_contract.cpp",
        *(
            root / "lib" / "x64" / configuration / library
            for library in DISTRIBUTION_LIBRARY_NAMES
        ),
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


def _create_kill_on_close_job() -> int:
    """終了時に配下processを全停止するWindows Job Objectを作る。"""
    create_job = _kernel32_function("CreateJobObjectW")
    create_job.argtypes = (wintypes.LPVOID, wintypes.LPCWSTR)
    create_job.restype = wintypes.HANDLE
    job = create_job(None, None)
    if not job:
        raise OSError(ctypes.get_last_error(), "consumer Job Objectを作成できません")
    information = _JobObjectExtendedLimitInformation()
    information.basic_limit_information.limit_flags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    set_information = _kernel32_function("SetInformationJobObject")
    set_information.argtypes = (
        wintypes.HANDLE,
        ctypes.c_int,
        wintypes.LPVOID,
        wintypes.DWORD,
    )
    set_information.restype = wintypes.BOOL
    if not set_information(
        job,
        JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
        ctypes.byref(information),
        ctypes.sizeof(information),
    ):
        error = ctypes.get_last_error()
        _close_windows_handle(int(job))
        raise OSError(error, "consumer Job Objectを初期化できません")
    return int(job)


def _assign_process_to_job(job: int, process: subprocess.Popen) -> None:
    """起動待機中のwrapperをJob Objectへ登録する。"""
    assign = _kernel32_function("AssignProcessToJobObject")
    assign.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
    assign.restype = wintypes.BOOL
    process_handle = wintypes.HANDLE(int(process._handle))
    if not assign(wintypes.HANDLE(job), process_handle):
        raise OSError(ctypes.get_last_error(), "consumer processをJob Objectへ登録できません")


def _terminate_job(job: int) -> None:
    """timeoutしたcommandと全子孫processを同時に停止する。"""
    terminate = _kernel32_function("TerminateJobObject")
    terminate.argtypes = (wintypes.HANDLE, wintypes.UINT)
    terminate.restype = wintypes.BOOL
    if not terminate(wintypes.HANDLE(job), 1):
        raise OSError(ctypes.get_last_error(), "consumer Job Objectを停止できません")


def _job_child_main(command: Sequence[str]) -> int:
    """親がJob登録を完了するまで待ち、trusted commandを同じJob内で実行する。"""
    if not command or sys.stdin.buffer.read(1) != b"S":
        return 254
    return subprocess.call(list(command))


def run_command(command: Sequence[str], timeout_seconds: float) -> subprocess.CompletedProcess[str]:
    """外部commandをJob内で実行し、timeout時は子孫も停止する。"""
    if os.name != "nt":
        raise OSError("distribution consumer command requires Windows")
    normalized_command = [str(argument) for argument in command]
    wrapper = [
        sys.executable,
        "-B",
        str(Path(__file__).resolve()),
        "--internal-job-child",
        *normalized_command,
    ]
    job = _create_kill_on_close_job()
    process: Optional[subprocess.Popen] = None
    try:
        process = subprocess.Popen(
            wrapper,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=CREATE_NO_WINDOW,
        )
        try:
            _assign_process_to_job(job, process)
        except OSError:
            process.kill()
            process.communicate()
            raise
        try:
            stdout_bytes, stderr_bytes = process.communicate(
                input=b"S",
                timeout=timeout_seconds,
            )
        except subprocess.TimeoutExpired as error:
            _terminate_job(job)
            stdout_bytes, stderr_bytes = process.communicate(timeout=30)
            raise subprocess.TimeoutExpired(
                normalized_command,
                timeout_seconds,
                output=stdout_bytes.decode("utf-8", errors="replace"),
                stderr=stderr_bytes.decode("utf-8", errors="replace"),
            ) from error
        return subprocess.CompletedProcess(
            normalized_command,
            process.returncode,
            stdout_bytes.decode("utf-8", errors="replace"),
            stderr_bytes.decode("utf-8", errors="replace"),
        )
    finally:
        _close_windows_handle(job)


def _remaining_timeout(deadline: float, command: Sequence[str]) -> float:
    """consumer全体deadlineまでの残り秒数を各commandへ渡す。"""
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise subprocess.TimeoutExpired(list(command), 0)
    return remaining


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


def _write_utf8_lf(path: Path, text: str) -> None:
    """Python 3.8でもUTF-8 BOMなし・LFでtext fileを書く。"""
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(text)


def _run_smoke(
    args: argparse.Namespace,
    snapshot_creator: Callable[[Path, Path, str, str], Path],
    command_runner: Callable[[Sequence[str], float], subprocess.CompletedProcess[str]],
    temporary_root_observer: Optional[Callable[[Path], None]],
) -> int:
    """依存を明示して、成功・失敗・timeoutのcleanupを同じ経路で実行する。"""
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

    try:
        deadline = time.monotonic() + args.timeout_seconds
        with tempfile.TemporaryDirectory(
            prefix=f"acs-distribution-consumer-{configuration.lower()}-"
        ) as temporary:
            root = Path(temporary)
            if temporary_root_observer is not None:
                temporary_root_observer(root)
            distribution_root = snapshot_creator(
                args.distribution_root,
                root / "distribution",
                configuration,
                EXPECTED_CONSUMER_CONTRACT_SHA256,
            )
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
            invalid_auto_links = invalid_auto_link_libraries(distribution_root / "acs.h")
            if invalid_auto_links:
                print(
                    "distribution_consumer_smoke=fail invalid_auto_link="
                    + ", ".join(invalid_auto_links),
                    file=sys.stderr,
                )
                return 2
            source_directory = root / "source"
            build_directory = root / "build"
            source_directory.mkdir()
            _write_utf8_lf(source_directory / "CMakeLists.txt", CMAKE_PROJECT)

            configure_command = make_configure_command(
                args.cmake,
                source_directory,
                build_directory,
                distribution_root,
                configuration,
                args.generator,
                args.generator_platform,
                args.generator_toolset,
            )
            configure = command_runner(
                configure_command,
                _remaining_timeout(deadline, configure_command),
            )
            if configure.returncode != 0:
                print_failure("configure", configure)
                return 1

            build_command = [
                args.cmake,
                "--build",
                str(build_directory),
                "--config",
                configuration,
                "--parallel",
                "1",
            ]
            build = command_runner(
                build_command,
                _remaining_timeout(deadline, build_command),
            )
            if build.returncode != 0:
                print_failure("build", build)
                return 1

            executable = find_consumer_executable(build_directory)
            execute_command = [str(executable)]
            execute = command_runner(
                execute_command,
                _remaining_timeout(deadline, execute_command),
            )
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
    except (OSError, RuntimeError, UnicodeError, subprocess.TimeoutExpired) as error:
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


def run_smoke(
    args: argparse.Namespace,
    temporary_root_observer: Optional[Callable[[Path], None]] = None,
) -> int:
    """配布物をrepo外でconfigure、link、実行し結果を返す。"""
    return _run_smoke(
        args,
        create_verified_snapshot,
        run_command,
        temporary_root_observer,
    )


def _test_manifest_bytes(root: Path) -> bytes:
    """self-test fixtureの固定44 payloadからcanonical manifestを作る。"""
    lines = [MANIFEST_SCHEMA]
    for relative_path in MANIFEST_RELATIVE_PATHS:
        payload = (root / Path(*relative_path.split("/"))).read_bytes()
        lines.append(f"{hashlib.sha256(payload).hexdigest().upper()}  {relative_path}")
    return ("\n".join(lines) + "\n").encode("ascii")


def _write_test_distribution(root: Path, generation: str) -> None:
    """self-test専用の完全な45 file mirrorを作る。"""
    for relative_path in MIRROR_RELATIVE_PATHS:
        if relative_path == MANIFEST_NAME:
            continue
        destination = root / Path(*relative_path.split("/"))
        destination.parent.mkdir(parents=True, exist_ok=True)
        if relative_path == "acs.h":
            payload = (
                '#pragma comment(lib, "advapi32.lib")\n'
                f"// generation={generation}\n"
            ).encode("utf-8")
        else:
            payload = f"{generation}:{relative_path}\n".encode("utf-8")
        destination.write_bytes(payload)
    (root / MANIFEST_NAME).write_bytes(_test_manifest_bytes(root))


def _expect_distribution_failure(operation: Callable[[], object]) -> bool:
    """配布検証が想定どおりfail-closedになることを返す。"""
    try:
        operation()
    except (DistributionValidationError, OSError):
        return True
    return False


def _capture_distribution_failure(operation: Callable[[], object]) -> str:
    """self-testでfail-closed理由をexact比較する。"""
    try:
        operation()
    except (DistributionValidationError, OSError) as error:
        return str(error)
    return ""


def _process_has_exited(process_id: int, timeout_milliseconds: int) -> bool:
    """timeout fixtureの子孫processがJob終了後に残っていないことを調べる。"""
    open_process = _kernel32_function("OpenProcess")
    open_process.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    open_process.restype = wintypes.HANDLE
    process = open_process(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        False,
        process_id,
    )
    if not process:
        return ctypes.get_last_error() == ERROR_INVALID_PARAMETER
    try:
        wait = _kernel32_function("WaitForSingleObject")
        wait.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        wait.restype = wintypes.DWORD
        return wait(process, timeout_milliseconds) == WAIT_OBJECT_0
    finally:
        _close_windows_handle(int(process))


def _self_test_reparse_rejection() -> bool:
    """作成操作なしの属性fixtureでreparse入力の拒否を固定する。"""

    class _ReparseStat:
        """reparse属性だけを持つ検査用stat結果。"""

        st_file_attributes = FILE_ATTRIBUTE_REPARSE_POINT

    original_lstat = os.lstat
    observed_paths: list[object] = []

    def reparse_lstat(path: object) -> _ReparseStat:
        """検査対象を記録し、reparse属性付き結果を返す。"""
        observed_paths.append(path)
        return _ReparseStat()

    os.lstat = reparse_lstat
    try:
        reason = _capture_distribution_failure(
            lambda: _assert_normal_path(Path("synthetic-reparse"), True)
        )
    finally:
        os.lstat = original_lstat
    return (
        len(observed_paths) == 1
        and reason == "distribution path is a reparse point: synthetic-reparse"
    )


def _self_test_directory_pin(root: Path) -> bool:
    """directory pin中だけ実renameを共有違反で拒否することを固定する。"""
    if os.name != "nt":
        return True
    probe = root / "directory-pin-probe"
    moved = root / "directory-pin-probe-moved"
    probe.mkdir()
    handles_before = _current_process_handle_count()
    handle, identity = _open_windows_pin(probe, True)
    move_error = 0
    moved_while_pinned = False
    try:
        _assert_pinned_identity(probe, identity, True)
        try:
            os.rename(probe, moved)
            moved_while_pinned = True
        except OSError as error:
            move_error = int(getattr(error, "winerror", 0) or 0)
    finally:
        _close_windows_handle(handle)
    handles_after = _current_process_handle_count()

    if moved.exists() and not probe.exists():
        os.rename(moved, probe)
    moved_after_close = False
    restored_after_close = False
    try:
        os.rename(probe, moved)
        moved_after_close = moved.is_dir() and not probe.exists()
    finally:
        if moved.exists() and not probe.exists():
            os.rename(moved, probe)
            restored_after_close = probe.is_dir() and not moved.exists()
    final_identity = _stat_object_identity(os.lstat(probe))
    return (
        not moved_while_pinned
        and move_error == ERROR_SHARING_VIOLATION
        and moved_after_close
        and restored_after_close
        and final_identity == identity
        and handles_before == handles_after
    )


def _self_test_pin_identity_failure_closes_handle(root: Path) -> bool:
    """完全識別子の拒否経路でも取得済みhandleを残さないことを固定する。"""
    probe = root / "pin-identity-failure-probe"
    probe.mkdir()
    handles_before = _current_process_handle_count()
    module = sys.modules[__name__]
    original_converter = _file_id_information_identity

    def reject_identity(information: _FileIdInformation) -> tuple[int, int]:
        del information
        raise DistributionValidationError("synthetic full identity rejection")

    try:
        setattr(module, "_file_id_information_identity", reject_identity)
        reason = _capture_distribution_failure(
            lambda: _open_windows_pin(probe, True)
        )
    finally:
        setattr(module, "_file_id_information_identity", original_converter)
    handles_after = _current_process_handle_count()
    return (
        reason == "synthetic full identity rejection"
        and handles_before == handles_after
    )


def _self_test_file_id_information() -> bool:
    """64ビットvolume IDと128ビットfile IDを切り詰めないことを固定する。"""
    expected_volume = 0xFEDCBA9876543210
    expected_file = 0x0123456789ABCDEFFEDCBA9876543210
    information = _FileIdInformation()
    information.volume_serial_number = expected_volume
    information.file_id[:] = expected_file.to_bytes(16, "little")
    return _file_id_information_identity(information) == (
        expected_volume,
        expected_file,
    )


def _self_test_windows_stat_identity_selection() -> bool:
    """Python 3.8系の旧形式と3.12以降の完全形式だけを受理する。"""

    class _SyntheticStat:
        """識別子選択だけを検査する最小stat結果。"""

        def __init__(self, identity: tuple[int, int]) -> None:
            self.st_dev, self.st_ino = identity

    legacy_identity = (0x76543210, 0x0123456789ABCDEF)
    full_identity = (
        0xFEDCBA9876543210,
        0x0123456789ABCDEFFEDCBA9876543210,
    )
    probe = Path("synthetic-identity")
    return (
        _select_windows_stat_identity(
            probe,
            _SyntheticStat(legacy_identity),
            legacy_identity,
            full_identity,
        )
        == legacy_identity
        and _select_windows_stat_identity(
            probe,
            _SyntheticStat(full_identity),
            legacy_identity,
            full_identity,
        )
        == full_identity
        and _expect_distribution_failure(
            lambda: _select_windows_stat_identity(
                probe,
                _SyntheticStat((full_identity[0], legacy_identity[1])),
                legacy_identity,
                full_identity,
            )
        )
    )


def self_test() -> int:
    """manifest、snapshot、引数、generator命令のfail-closed契約を固定する。"""
    with tempfile.TemporaryDirectory(prefix="acs-distribution-smoke-selftest-") as temporary:
        root = Path(temporary)
        distribution = root / "distribution"
        distribution.mkdir()
        _write_test_distribution(distribution, "A")
        directory_pin_contract = _self_test_directory_pin(root)
        pin_failure_cleanup_contract = (
            _self_test_pin_identity_failure_closes_handle(root)
        )
        fixture_contract_hash = hashlib.sha256(
            (distribution / "verification" / "consumer_contract.cpp").read_bytes()
        ).hexdigest().upper()
        expected = required_distribution_files(distribution, "Debug")
        build_directory = root / "build"
        executable = build_directory / "Debug" / EXECUTABLE_NAME
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"probe")
        valid_header = root / "valid-acs.h"
        _write_utf8_lf(
            valid_header,
            '#pragma comment(lib, "advapi32.lib")\n',
        )
        missing_header = root / "missing-acs.h"
        _write_utf8_lf(
            missing_header,
            '#pragma comment(lib, "user32.lib")\n',
        )
        duplicate_header = root / "duplicate-acs.h"
        _write_utf8_lf(
            duplicate_header,
            '#pragma comment(lib, "advapi32.lib")\n'
            '#pragma comment(lib, "advapi32.lib")\n',
        )
        command = make_configure_command(
            "cmake",
            root / "source",
            build_directory,
            distribution,
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
        fixed_counts = (
            len(DISTRIBUTION_LIBRARY_NAMES),
            len(LICENSE_RELATIVE_PATHS),
            len(MANIFEST_RELATIVE_PATHS),
            len(MIRROR_RELATIVE_PATHS),
            len(MIRROR_RELATIVE_DIRECTORIES),
        )
        count_mutations = tuple(
            tuple(value - 1 if index == mutation else value for index, value in enumerate(fixed_counts))
            for mutation in range(len(fixed_counts))
        )
        distribution_stat = os.lstat(distribution)
        distribution_identity = (
            int(distribution_stat.st_dev),
            int(distribution_stat.st_ino),
        )
        valid = (
            normalize_configuration("debug") == "Debug"
            and normalize_configuration("RELEASE") == "Release"
            and _fixed_contract_counts_match(fixed_counts)
            and fixed_counts == (14, 12, 44, 45, 7)
            and all(not _fixed_contract_counts_match(counts) for counts in count_mutations)
            and LICENSE_RELATIVE_PATHS == tuple(sorted(LICENSE_RELATIVE_PATHS))
            and tuple(
                path for path in MIRROR_RELATIVE_PATHS if path != MANIFEST_NAME
            )
            == MANIFEST_RELATIVE_PATHS
            and all(path in MANIFEST_RELATIVE_PATHS for path in LICENSE_RELATIVE_PATHS)
            and not _expect_distribution_failure(
                lambda: _assert_pinned_identity(
                    distribution,
                    distribution_identity,
                    True,
                )
            )
            and _expect_distribution_failure(
                lambda: _assert_pinned_identity(
                    distribution,
                    (distribution_identity[0] ^ 1, distribution_identity[1]),
                    True,
                )
            )
            and hashlib.sha256(
                (
                    Path(__file__).resolve().parents[2]
                    / "dist"
                    / "verification"
                    / "consumer_contract.cpp"
                ).read_bytes()
            ).hexdigest().upper()
            == EXPECTED_CONSUMER_CONTRACT_SHA256
            and len(expected) == 2 + len(DISTRIBUTION_LIBRARY_NAMES)
            and expected[0] == distribution / "acs.h"
            and expected[2] == distribution / "lib" / "x64" / "Debug" / "acs.lib"
            and command[-2:] == [
                "-DCMAKE_BUILD_TYPE=Release",
                f"-DACS_DISTRIBUTION_ROOT={distribution}",
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
            and directory_pin_contract
            and pin_failure_cleanup_contract
            and _self_test_file_id_information()
            and _self_test_windows_stat_identity_selection()
        )
        manifest_bytes = (distribution / MANIFEST_NAME).read_bytes()
        parsed = parse_distribution_manifest(manifest_bytes)
        snapshot = create_verified_snapshot(
            distribution,
            root / "snapshot",
            "Debug",
            fixture_contract_hash,
        )
        valid = (
            valid
            and len(parsed) == len(MANIFEST_RELATIVE_PATHS)
            and validate_complete_mirror(distribution) == distribution
            and all(path.is_file() for path in required_distribution_files(snapshot, "Debug"))
            and (snapshot / "README.md").is_file()
            and (snapshot / "verification" / "build_consumer_contract.cmd").is_file()
            and all(
                (snapshot / Path(*relative_path.split("/"))).is_file()
                for relative_path in LICENSE_RELATIVE_PATHS
            )
            and not (snapshot / "lib" / "x64" / "Release").exists()
        )

        alias_source = distribution / "lib" / "x64" / "Debug" / "acs.lib"
        alias_target = root / "alternate-readable-object.lib"
        alias_target.write_bytes(alias_source.read_bytes())
        alias_destination = root / "alias-remap-copy.lib"
        with pin_complete_mirror(distribution) as mirror:
            alias_reason = _capture_distribution_failure(
                lambda: _copy_and_hash(
                    alias_source,
                    alias_destination,
                    mirror.expected_identity(alias_source),
                    lambda path: alias_target.open("rb"),
                )
            )
        valid = (
            valid
            and alias_reason.startswith(
                "distribution read handle identity differs: "
            )
            and not alias_destination.exists()
            and alias_target.read_bytes() == alias_source.read_bytes()
        )

        manifest_lines = manifest_bytes.decode("ascii").splitlines()
        swapped_lines = list(manifest_lines)
        swapped_lines[1], swapped_lines[2] = swapped_lines[2], swapped_lines[1]
        duplicate_lines = list(manifest_lines)
        duplicate_lines[-1] = duplicate_lines[-2]
        lowercase_lines = list(manifest_lines)
        lowercase_lines[1] = lowercase_lines[1].lower()
        invalid_manifests = (
            b"\xef\xbb\xbf" + manifest_bytes,
            manifest_bytes.replace(b"\n", b"\r\n"),
            manifest_bytes.replace(
                MANIFEST_SCHEMA.encode("ascii"),
                b"ACS_DIST_SHA256_V1",
                1,
            ),
            ("\n".join(swapped_lines) + "\n").encode("ascii"),
            ("\n".join(duplicate_lines) + "\n").encode("ascii"),
            ("\n".join(lowercase_lines) + "\n").encode("ascii"),
            ("\n".join(manifest_lines[:-1]) + "\n").encode("ascii"),
            manifest_bytes[:-1],
        )
        valid = valid and all(
            _expect_distribution_failure(lambda candidate=candidate: parse_distribution_manifest(candidate))
            for candidate in invalid_manifests
        )

        # canonical形式のままhashだけ異なるmanifestをpayload照合で拒否する。
        mismatched_hash_lines = list(manifest_lines)
        mismatched_hash_lines[1] = "0" * 64 + mismatched_hash_lines[1][64:]
        (distribution / MANIFEST_NAME).write_bytes(
            ("\n".join(mismatched_hash_lines) + "\n").encode("ascii")
        )
        valid = valid and _expect_distribution_failure(
            lambda: create_verified_snapshot(
                distribution,
                root / "hash-mismatch-snapshot",
                "Debug",
                fixture_contract_hash,
            )
        )
        (distribution / MANIFEST_NAME).write_bytes(manifest_bytes)

        extra = distribution / "unexpected.bin"
        extra.write_bytes(b"unexpected")
        valid = valid and _expect_distribution_failure(
            lambda: validate_complete_mirror(distribution)
        )
        extra.unlink()

        # licenseの欠落と改変を完全treeとmanifest hashの両方で拒否する。
        missing_license = distribution / Path(*LICENSE_RELATIVE_PATHS[0].split("/"))
        missing_license_payload = missing_license.read_bytes()
        missing_license.unlink()
        valid = valid and _expect_distribution_failure(
            lambda: validate_complete_mirror(distribution)
        )
        missing_license.write_bytes(missing_license_payload)

        tampered_license = distribution / Path(*LICENSE_RELATIVE_PATHS[-1].split("/"))
        original_license_payload = tampered_license.read_bytes()
        tampered_license.write_bytes(b"tampered-license\n")
        valid = valid and _expect_distribution_failure(
            lambda: create_verified_snapshot(
                distribution,
                root / "tampered-license-snapshot",
                "Release",
                fixture_contract_hash,
            )
        )
        tampered_license.write_bytes(original_license_payload)

        contract_path = distribution / "verification" / "consumer_contract.cpp"
        original_contract = contract_path.read_bytes()
        contract_path.write_bytes(b"stable but different generation\n")
        valid = valid and _expect_distribution_failure(
            lambda: create_verified_snapshot(
                distribution,
                root / "contract-drift-snapshot",
                "Debug",
                fixture_contract_hash,
            )
        )
        contract_path.write_bytes(original_contract)

        tampered = distribution / "lib" / "x64" / "Debug" / "acs.lib"
        original_payload = tampered.read_bytes()
        tampered.write_bytes(b"tampered")
        valid = valid and _expect_distribution_failure(
            lambda: create_verified_snapshot(
                distribution,
                root / "tampered-snapshot",
                "Debug",
                fixture_contract_hash,
            )
        )
        tampered.write_bytes(original_payload)

        race_sentinel = root / "race-sentinel.txt"
        race_sentinel.write_text("outside", encoding="utf-8")
        mutation_attempts = 0
        protected_payload = distribution / "lib" / "x64" / "Debug" / "acs.lib"
        protected_bytes = protected_payload.read_bytes()

        def mutate_payload_after_nth_copy(relative_path: str, index: int) -> None:
            nonlocal mutation_attempts
            del relative_path
            if index == 5:
                mutation_attempts += 1
                protected_payload.write_bytes(b"generation-race")

        payload_race_reason = _capture_distribution_failure(
            lambda: create_verified_snapshot(
                distribution,
                root / "raced-payload-snapshot",
                "Debug",
                fixture_contract_hash,
                mutate_payload_after_nth_copy,
            )
        )
        valid = (
            valid
            and mutation_attempts == 1
            and payload_race_reason.startswith(
                "distribution live payload mutation was blocked: "
            )
            and protected_payload.read_bytes() == protected_bytes
            and race_sentinel.read_text(encoding="utf-8") == "outside"
        )

        commit_attempts = 0

        def publish_generation_b() -> None:
            nonlocal commit_attempts
            commit_attempts += 1
            _write_test_distribution(distribution, "B")

        manifest_race_reason = _capture_distribution_failure(
            lambda: create_verified_snapshot(
                distribution,
                root / "raced-manifest-snapshot",
                "Debug",
                fixture_contract_hash,
                commit_hook=publish_generation_b,
            )
        )
        valid = (
            valid
            and commit_attempts == 1
            and manifest_race_reason == "distribution manifest changed during snapshot"
            and (distribution / "acs.h").read_bytes().endswith(b"generation=B\n")
            and race_sentinel.read_text(encoding="utf-8") == "outside"
        )
        _write_test_distribution(distribution, "A")
        manifest_bytes = (distribution / MANIFEST_NAME).read_bytes()

        swap_attempted = False
        swap_succeeded = False
        moved_distribution = root / "moved-distribution"

        def attempt_root_swap(relative_path: str, index: int) -> None:
            nonlocal swap_attempted, swap_succeeded
            del relative_path
            if index != 0:
                return
            swap_attempted = True
            try:
                distribution.rename(moved_distribution)
                swap_succeeded = True
                moved_distribution.rename(distribution)
            except OSError:
                pass

        root_pin_snapshot = create_verified_snapshot(
            distribution,
            root / "root-pin-snapshot",
            "Debug",
            fixture_contract_hash,
            attempt_root_swap,
        )
        valid = (
            valid
            and root_pin_snapshot.is_dir()
            and swap_attempted
            and not swap_succeeded
            and distribution.is_dir()
            and not moved_distribution.exists()
            and _self_test_reparse_rejection()
        )

        command_success = run_command(
            [sys.executable, "-c", "print('job-success')"],
            10,
        )
        command_failure = run_command(
            [sys.executable, "-c", "raise SystemExit(7)"],
            10,
        )
        timeout_process_id = root / "timeout-child.pid"
        descendant_code = (
            "import os,pathlib,time;"
            f"pathlib.Path({str(timeout_process_id)!r}).write_text(str(os.getpid()));"
            "time.sleep(60)"
        )
        parent_code = (
            "import pathlib,subprocess,sys,time;"
            f"p=pathlib.Path({str(timeout_process_id)!r});"
            f"subprocess.Popen([sys.executable,'-c',{descendant_code!r}]);"
            "end=time.time()+5;"
            "\nwhile not p.exists() and time.time()<end: time.sleep(0.01)\n"
            "print('descendant-ready',flush=True);time.sleep(60)"
        )
        command_timeout = False
        try:
            run_command([sys.executable, "-c", parent_code], 2)
        except subprocess.TimeoutExpired:
            command_timeout = True
        child_process_id = (
            int(timeout_process_id.read_text(encoding="utf-8"))
            if timeout_process_id.is_file()
            else 0
        )
        valid = (
            valid
            and command_success.returncode == 0
            and command_success.stdout == "job-success\r\n"
            and command_failure.returncode == 7
            and command_timeout
            and child_process_id > 0
            and _process_has_exited(child_process_id, 5000)
        )

        smoke_args = argparse.Namespace(
            distribution_root=distribution,
            configuration="Debug",
            cmake="cmake",
            generator="Visual Studio Test",
            generator_platform="x64",
            generator_toolset="vTest",
            timeout_seconds=30,
        )
        production_snapshot = create_verified_snapshot
        production_runner = run_command

        def fixture_snapshot(
            live_root: Path,
            snapshot_root: Path,
            configuration: str,
            expected_hash: str,
        ) -> Path:
            del expected_hash
            return production_snapshot(
                live_root,
                snapshot_root,
                configuration,
                fixture_contract_hash,
            )

        def successful_runner(
            arguments: Sequence[str],
            timeout_seconds: float,
        ) -> subprocess.CompletedProcess[str]:
            del timeout_seconds
            if "--build" in arguments:
                output = Path(arguments[arguments.index("--build") + 1])
                executable_path = output / "Debug" / EXECUTABLE_NAME
                executable_path.parent.mkdir(parents=True, exist_ok=True)
                executable_path.write_bytes(b"fixture")
            stdout = "acs.h OK\n" if len(arguments) == 1 else ""
            return subprocess.CompletedProcess(list(arguments), 0, stdout, "")

        def failing_runner(
            arguments: Sequence[str],
            timeout_seconds: float,
        ) -> subprocess.CompletedProcess[str]:
            del timeout_seconds
            return subprocess.CompletedProcess(list(arguments), 9, "", "fixture-error")

        def timeout_runner(
            arguments: Sequence[str],
            timeout_seconds: float,
        ) -> subprocess.CompletedProcess[str]:
            raise subprocess.TimeoutExpired(list(arguments), timeout_seconds)

        smoke_results: list[tuple[int, str, str, Path]] = []
        for runner in (successful_runner, failing_runner, timeout_runner):
            captured_stdout = io.StringIO()
            captured_stderr = io.StringIO()
            observed: list[Path] = []
            with redirect_stdout(captured_stdout), redirect_stderr(captured_stderr):
                result = _run_smoke(
                    smoke_args,
                    fixture_snapshot,
                    runner,
                    observed.append,
                )
            smoke_results.append(
                (
                    result,
                    captured_stdout.getvalue(),
                    captured_stderr.getvalue(),
                    observed[0],
                )
            )
        valid = (
            valid
            and [item[0] for item in smoke_results] == [0, 1, 1]
            and "distribution_consumer_smoke=pass configuration=Debug" in smoke_results[0][1]
            and "step=configure exit=9" in smoke_results[1][2]
            and "timed out" in smoke_results[2][2]
            and all(not item[3].exists() for item in smoke_results)
        )

        integrated_process_id = root / "integrated-timeout-child.pid"
        integrated_descendant_code = (
            "import os,pathlib,time;"
            f"pathlib.Path({str(integrated_process_id)!r}).write_text(str(os.getpid()));"
            "time.sleep(60)"
        )
        integrated_parent_code = (
            "import pathlib,subprocess,sys,time;"
            f"p=pathlib.Path({str(integrated_process_id)!r});"
            f"subprocess.Popen([sys.executable,'-c',{integrated_descendant_code!r}]);"
            "end=time.time()+5;"
            "\nwhile not p.exists() and time.time()<end: time.sleep(0.01)\n"
            "print('integrated-descendant-ready',flush=True);time.sleep(60)"
        )

        def integrated_timeout_runner(
            arguments: Sequence[str],
            timeout_seconds: float,
        ) -> subprocess.CompletedProcess[str]:
            del arguments
            return production_runner(
                [sys.executable, "-c", integrated_parent_code],
                timeout_seconds,
            )

        integrated_args = argparse.Namespace(**vars(smoke_args))
        integrated_args.timeout_seconds = 2
        integrated_roots: list[Path] = []
        integrated_stdout = io.StringIO()
        integrated_stderr = io.StringIO()
        module = sys.modules[__name__]
        setattr(module, "create_verified_snapshot", fixture_snapshot)
        setattr(module, "run_command", integrated_timeout_runner)
        try:
            with redirect_stdout(integrated_stdout), redirect_stderr(integrated_stderr):
                integrated_result = run_smoke(
                    integrated_args,
                    integrated_roots.append,
                )
        finally:
            setattr(module, "create_verified_snapshot", production_snapshot)
            setattr(module, "run_command", production_runner)
        integrated_child_process_id = (
            int(integrated_process_id.read_text(encoding="utf-8"))
            if integrated_process_id.is_file()
            else 0
        )
        valid = (
            valid
            and integrated_result == 1
            and "timed out" in integrated_stderr.getvalue()
            and len(integrated_roots) == 1
            and not integrated_roots[0].exists()
            and integrated_child_process_id > 0
            and _process_has_exited(integrated_child_process_id, 5000)
        )
    return 0 if valid and not root.exists() else 1


def main() -> int:
    """CLI引数を検証してself-testまたは実smokeを実行する。"""
    if len(sys.argv) >= 2 and sys.argv[1] == "--internal-job-child":
        return _job_child_main(sys.argv[2:])
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
