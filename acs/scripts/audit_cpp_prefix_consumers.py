#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""testsと配布consumerに残す旧公開型名をexact監査する。"""

from __future__ import annotations

import argparse
import collections
from contextlib import contextmanager, ExitStack
import ctypes
from ctypes import wintypes
from dataclasses import dataclass, field
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
from typing import Dict, Iterable, Iterator, List, Mapping, Optional, Sequence, Tuple
from unittest import mock


ALLOWLIST_RELATIVE_PATH = Path("scripts/data/cpp_prefix_consumer_legacy_allowlist.json")
REGISTRY_RELATIVE_PATH = Path("scripts/data/cpp_type_role_migrations.json")
TARGET_DIRECTORIES = (Path("src"), Path("tests"))
TARGET_REPOSITORY_FILES = (
    Path("dist/verification/consumer_contract.cpp"),
    Path("acs/scripts/run_distribution_consumer_smoke.py"),
    Path("acs/scripts/amalgamate.py"),
    Path("acs/scripts/audit_cpp_prefix_consumers.py"),
    Path("acs/scripts/build_single_header.ps1"),
    Path("acs/scripts/data/cpp_type_role_migrations.json"),
    Path("acs/scripts/data/cpp_prefix_consumer_legacy_allowlist.json"),
)
CONFIG_REPOSITORY_FILES = frozenset(
    {
        "acs/scripts/data/cpp_type_role_migrations.json",
        "acs/scripts/data/cpp_prefix_consumer_legacy_allowlist.json",
        "acs/scripts/audit_cpp_prefix_consumers.py",
    }
)
TEXT_SUFFIXES = frozenset(
    {
        ".bat",
        ".c",
        ".cc",
        ".cmake",
        ".cmd",
        ".cpp",
        ".cxx",
        ".h",
        ".hh",
        ".hpp",
        ".html",
        ".inl",
        ".ixx",
        ".js",
        ".json",
        ".md",
        ".ps1",
        ".py",
        ".txt",
    }
)
CPP_TEXT_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".ixx"})
ALLOWED_REASONS = frozenset(
    {
        "compatibility_fixture",
        "distribution_compatibility",
        "runtime_name_comparison",
        "runtime_test_suite_identity",
        "runtime_type_lookup",
    }
)
COMPATIBILITY_FILE_PATHS = frozenset(
    {
        "acs/tests/aobject_header_compile_tests.cpp",
        "acs/tests/app_forward_header_compile_tests.cpp",
        "acs/tests/gameframework_forward_header_compile_tests.cpp",
        "acs/tests/object_ptr_header_compile_tests.cpp",
        "acs/tests/subsystem_canonical_header_tests.cpp",
        "acs/tests/subsystem_spawn_header_compile_tests.cpp",
    }
)
EXPECTED_ALLOWLIST_SHA256 = "CC674048F685ABAE3B3DF7285D25D52C44DC37561F594D65C8F2FB59619AB778"
EXPECTED_IDENTITY_MACRO_SHA256 = "89047ADEDDCCDC1696C6A0AF88F6F60C4C5B70AD7DF472B6750A54372EAF416C"
EXPECTED_IDENTITY_MACRO_CATALOG_SHA256 = "239B0DCA6474063D7033DA1B82BFF1B8485D02F98C093349C7C64AEC31FCBA0C"
FILE_ATTRIBUTE_REPARSE_POINT = 0x00000400
FILE_ATTRIBUTE_DIRECTORY = 0x00000010
FILE_LIST_DIRECTORY = 0x00000001
FILE_READ_ATTRIBUTES = 0x00000080
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000
FILE_FLAG_BACKUP_SEMANTICS = 0x02000000
FILE_NAME_NORMALIZED = 0x00000000
VOLUME_NAME_GUID = 0x00000001
WINDOWS_NO_WINDOW = 0x08000000
WINDOWS_COMMAND_ENCODING = "oem"
EXPECTED_IDENTITY_MACRO_INDEXES = {
    name: frozenset({0})
    for name in (
        "ACS_ASSET_T",
        "ACS_COMMAND",
        "ACS_COMPONENT",
        "ACS_EVENT",
        "ACS_GAME_COMPONENT_KIND",
        "ACS_GAME_REFLECT",
        "ACS_GAME_SUBSYSTEM_KIND",
        "ACS_INTERFACE",
        "ACS_NODE",
        "ACS_OBJECT",
        "ACS_PREFAB",
        "ACS_REFLECT",
        "ACS_REFLECT_ENUM",
        "ACS_REGISTER",
        "ACS_REGISTER_ASSET",
        "ACS_REGISTER_COMMAND",
        "ACS_REGISTER_COMPONENT",
        "ACS_REGISTER_ENUM",
        "ACS_REGISTER_EVENT",
        "ACS_REGISTER_INTERFACE",
        "ACS_REGISTER_METHOD",
        "ACS_REGISTER_METHOD_F32",
        "ACS_REGISTER_METHOD_I32",
        "ACS_REGISTER_METHOD_RET_F32",
        "ACS_REGISTER_METHOD_RET_I32",
        "ACS_REGISTER_METHOD_RET_STR",
        "ACS_REGISTER_METHOD_STR",
        "ACS_REGISTER_NODE",
        "ACS_REGISTER_OBJECT",
        "ACS_REGISTER_PREFAB",
        "ACS_REGISTER_SCENE",
        "ACS_REGISTER_SERVICE",
        "ACS_REGISTER_STRUCT",
        "ACS_REGISTER_SUBSYSTEM",
        "ACS_REGISTER_SUBSYSTEM_EX",
        "ACS_REGISTER_SYSTEM",
        "ACS_SCENE",
        "ACS_SERVICE",
        "ACS_STRUCT",
        "ACS_SUBSYSTEM_KIND",
        "ACS_SYSTEM",
        "ACS_TEST",
    )
}


def _write_utf8_lf(path: Path, text: str) -> None:
    """Python 3.8でもLF固定のUTF-8 textを書き込む。"""

    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


class FByHandleFileInformation(ctypes.Structure):
    """GetFileInformationByHandleのdirectory identity部分。"""

    _fields_ = [
        ("dwFileAttributes", wintypes.DWORD),
        ("ftCreationTime", wintypes.FILETIME),
        ("ftLastAccessTime", wintypes.FILETIME),
        ("ftLastWriteTime", wintypes.FILETIME),
        ("dwVolumeSerialNumber", wintypes.DWORD),
        ("nFileSizeHigh", wintypes.DWORD),
        ("nFileSizeLow", wintypes.DWORD),
        ("nNumberOfLinks", wintypes.DWORD),
        ("nFileIndexHigh", wintypes.DWORD),
        ("nFileIndexLow", wintypes.DWORD),
    ]


@dataclass(frozen=True, order=True)
class FLegacySite:
    """行移動に影響されない旧名出現位置を表す。"""

    path: str
    file_sha256: str
    line_sha256: str
    line_occurrence: int
    token_occurrence: int
    legacy: str
    canonical: str
    lexical_context: str
    construct: str
    qualifier: str
    preprocessor_state: str


@dataclass(frozen=True)
class FObservedSite:
    """監査結果へ現在行番号を添えた旧名出現位置。"""

    site: FLegacySite
    line_number: int


@dataclass(frozen=True)
class FAllowedSite:
    """旧名を残す理由を持つ許可位置。"""

    site: FLegacySite
    line_hint: int
    reason: str


@dataclass(frozen=True, order=True)
class FPathIdentity:
    """path差替えを検知する物理file identity。"""

    device: int
    inode: int
    mode: int
    file_attributes: int


@dataclass(frozen=True, order=True)
class FDirectorySnapshot:
    """走査rootとancestorのdirectory identity。"""

    path: str
    identity: FPathIdentity


@dataclass(frozen=True)
class FDirectoryPin:
    """走査中保持するdirectory handleのidentityと物理path。"""

    identity: FPathIdentity
    physical_path: str
    descriptor: Optional[int]


@dataclass(frozen=True, order=True)
class FTargetFileSnapshot:
    """1 text fileのidentityとbytes正本。"""

    path: str
    identity: FPathIdentity
    byte_count: int
    bytes_sha256: str
    raw: bytes = field(compare=False, repr=False)


@dataclass(frozen=True)
class FRepositorySnapshot:
    """単一時点の対象path集合と内容hash。"""

    directories: Tuple[FDirectorySnapshot, ...]
    files: Tuple[FTargetFileSnapshot, ...]


@dataclass(frozen=True)
class FCppToken:
    """phase2後tokenと元source位置を保持する。"""

    kind: str
    value: str
    start: int
    end: int
    transformed: bool


@dataclass(frozen=True)
class FMacroDefinition:
    """active function-like ACS macroの正本定義。"""

    path: str
    line: int
    name: str
    parameters: Tuple[str, ...]
    body_tokens: Tuple[FCppToken, ...]
    preprocessor_state: str


def _reject_duplicate_keys(pairs: Sequence[Tuple[str, object]]) -> Dict[str, object]:
    """JSON object内の同名keyを上書きせず拒否する。"""

    result: Dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key: {}".format(key))
        result[key] = value
    return result


def _load_json_bytes(raw: bytes, source_name: str) -> object:
    """BOMなしUTF-8 JSONを重複key拒否で読む。"""

    if raw.startswith(b"\xef\xbb\xbf"):
        raise ValueError("UTF-8 BOM is not supported: {}".format(source_name))
    if b"\r" in raw:
        raise ValueError("CR is not supported: {}".format(source_name))
    return json.loads(raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys)


def _load_json(path: Path) -> object:
    """BOMなしUTF-8 JSONを重複key拒否で読む。"""

    return _load_json_bytes(path.read_bytes(), str(path))


def _canonical_json_sha256(value: object) -> str:
    """固定JSON表現のSHA-256を返す。"""

    payload = json.dumps(
        value,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=False,
        allow_nan=False,
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest().upper()


def _load_legacy_mapping_document(document: object) -> Tuple[Dict[str, str], str]:
    """正規registryから曖昧でない旧basename対応を読む。"""

    if not isinstance(document, dict) or set(document) != {"schema_version", "entries"}:
        raise ValueError("migration registry root fields are not exact")
    if document.get("schema_version") != 2:
        raise ValueError("unsupported migration registry schema_version")
    if not isinstance(document.get("entries"), list):
        raise ValueError("migration registry requires an entries array")
    mapping: Dict[str, str] = {}
    for entry in document["entries"]:
        if not isinstance(entry, dict):
            raise ValueError("migration registry entry must be an object")
        legacy = entry.get("legacy")
        canonical = entry.get("canonical")
        if legacy is None:
            continue
        if not isinstance(legacy, str) or not isinstance(canonical, str):
            raise ValueError("migration registry names must be strings")
        basename = legacy.rsplit("::", 1)[-1]
        if basename in mapping:
            raise ValueError("duplicate legacy basename: {}".format(basename))
        mapping[basename] = canonical
    rows = [[legacy, mapping[legacy]] for legacy in sorted(mapping)]
    return mapping, _canonical_json_sha256(rows)


def _load_legacy_mapping(registry_path: Path) -> Tuple[Dict[str, str], str]:
    """registry fileから重複しない旧basename対応を読む。"""

    return _load_legacy_mapping_document(_load_json(registry_path))


def _legacy_pattern(mapping: Mapping[str, str]) -> re.Pattern[str]:
    """identifier内部を避ける旧basename patternを構築する。"""

    alternatives = "|".join(
        sorted((re.escape(name) for name in mapping), key=len, reverse=True)
    )
    return re.compile(
        r"(?<![A-Za-z0-9_$\x80-\xff])(" + alternatives + r")(?![A-Za-z0-9_$\x80-\xff])"
    )


def _is_text_target(path: Path) -> bool:
    """consumer監査対象にするtext fileか判定する。"""

    return path.name == "CMakeLists.txt" or path.suffix.lower() in TEXT_SUFFIXES


def _path_identity(file_stat: os.stat_result) -> FPathIdentity:
    """no-follow statを比較用identityへ変換する。"""

    return FPathIdentity(
        device=int(file_stat.st_dev),
        inode=int(file_stat.st_ino),
        mode=int(stat.S_IFMT(file_stat.st_mode)),
        file_attributes=int(getattr(file_stat, "st_file_attributes", 0)),
    )


def _is_reparse(file_stat: os.stat_result) -> bool:
    """symlinkまたはWindows reparse pointを判定する。"""

    return stat.S_ISLNK(file_stat.st_mode) or bool(
        int(getattr(file_stat, "st_file_attributes", 0))
        & FILE_ATTRIBUTE_REPARSE_POINT
    )


def _native_path_text(path: Path) -> str:
    """Windows volume GUID rootへ必要な末尾separatorを補う。"""

    text = str(path)
    if (
        os.name == "nt"
        and text.startswith("\\\\?\\Volume{")
        and text.endswith("}")
    ):
        return text + "\\"
    return text


def _windows_physical_path_key(path: str) -> str:
    """GUID pathをseparatorとcase差に影響されない比較keyへ変換する。"""

    return path.rstrip("\\/").replace("/", "\\").casefold()


def _windows_process_handle_count() -> int:
    """self-test前後のprocess handle総数を返す。"""

    if os.name != "nt":
        return 0
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    get_current_process = kernel32.GetCurrentProcess
    get_current_process.argtypes = []
    get_current_process.restype = wintypes.HANDLE
    get_process_handle_count = kernel32.GetProcessHandleCount
    get_process_handle_count.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.DWORD),
    ]
    get_process_handle_count.restype = wintypes.BOOL
    handle_count = wintypes.DWORD()
    if not get_process_handle_count(
        get_current_process(), ctypes.byref(handle_count)
    ):
        raise ctypes.WinError(ctypes.get_last_error())
    return int(handle_count.value)


def _run_windows_command(
    arguments: Sequence[str], timeout_seconds: int = 10
) -> subprocess.CompletedProcess:
    """Windows commandの出力をUTF-8 modeに依存せず厳密に読む。"""

    if os.name != "nt":
        raise RuntimeError("Windows command is unavailable")
    completed = subprocess.run(
        list(arguments),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=False,
        timeout=timeout_seconds,
        creationflags=WINDOWS_NO_WINDOW,
    )
    stdout = completed.stdout.decode(WINDOWS_COMMAND_ENCODING, "strict")
    stderr = completed.stderr.decode(WINDOWS_COMMAND_ENCODING, "strict")
    return subprocess.CompletedProcess(
        completed.args, completed.returncode, stdout, stderr
    )


def _checked_stat(path: Path, expected_directory: bool) -> os.stat_result:
    """pathをfollowせず通常file/directoryだけ受理する。"""

    file_stat = os.stat(_native_path_text(path), follow_symlinks=False)
    if _is_reparse(file_stat):
        raise ValueError("reparse path is not allowed: {}".format(path))
    if expected_directory and not stat.S_ISDIR(file_stat.st_mode):
        raise ValueError("expected directory: {}".format(path))
    if not expected_directory and not stat.S_ISREG(file_stat.st_mode):
        raise ValueError("expected regular file: {}".format(path))
    return file_stat


def _validate_directory_pin(
    path: Path,
    before: os.stat_result,
    after: os.stat_result,
    handle_volume_serial: int,
    handle_file_index: int,
    handle_attributes: int,
    expected_identity: Optional[FPathIdentity],
) -> None:
    """directory pathとno-delete handleが同じ通常directoryか検証する。"""

    if (
        handle_attributes & FILE_ATTRIBUTE_REPARSE_POINT
        or not (handle_attributes & FILE_ATTRIBUTE_DIRECTORY)
    ):
        raise ValueError("directory handle is reparse or not a directory: {}".format(path))
    before_identity = _path_identity(before)
    after_identity = _path_identity(after)
    if (
        handle_volume_serial == 0
        or handle_file_index == 0
        or before_identity != after_identity
        or (int(after.st_dev), int(after.st_ino))
        != (handle_volume_serial, handle_file_index)
    ):
        raise ValueError("directory changed while acquiring pin: {}".format(path))
    if expected_identity is not None and after_identity != expected_identity:
        raise ValueError("directory entry identity changed before pin: {}".format(path))


def _windows_final_path(kernel32: object, handle: object) -> str:
    """directory handleからdrive aliasに依存しないGUID pathを取得する。"""

    get_final_path = kernel32.GetFinalPathNameByHandleW
    get_final_path.argtypes = [
        wintypes.HANDLE,
        wintypes.LPWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
    ]
    get_final_path.restype = wintypes.DWORD
    capacity = 32768
    buffer = ctypes.create_unicode_buffer(capacity)
    length = int(
        get_final_path(
            handle,
            buffer,
            capacity,
            FILE_NAME_NORMALIZED | VOLUME_NAME_GUID,
        )
    )
    if length == 0:
        raise ctypes.WinError(ctypes.get_last_error())
    if length >= capacity:
        raise ValueError("directory physical path exceeds fixed buffer")
    physical_path = buffer.value
    if not physical_path.startswith("\\\\?\\Volume{"):
        raise ValueError("directory physical path is not a volume GUID path")
    return physical_path


@contextmanager
def _held_directory(
    path: Path,
    expected_identity: Optional[FPathIdentity] = None,
    parent_descriptor: Optional[int] = None,
    entry_name: Optional[str] = None,
) -> Iterator[FDirectoryPin]:
    """directoryをno-follow/no-delete handleでpinしている間だけ制御を渡す。"""

    if os.name == "nt":
        if parent_descriptor is not None or entry_name is not None:
            raise ValueError("Windows directory pin does not accept a parent descriptor")
        before = _checked_stat(path, expected_directory=True)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        create_file = kernel32.CreateFileW
        create_file.argtypes = [
            wintypes.LPCWSTR,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.LPVOID,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.HANDLE,
        ]
        create_file.restype = wintypes.HANDLE
        get_information = kernel32.GetFileInformationByHandle
        get_information.argtypes = [
            wintypes.HANDLE,
            ctypes.POINTER(FByHandleFileInformation),
        ]
        get_information.restype = wintypes.BOOL
        close_handle = kernel32.CloseHandle
        close_handle.argtypes = [wintypes.HANDLE]
        close_handle.restype = wintypes.BOOL
        handle = create_file(
            _native_path_text(path),
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            None,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            None,
        )
        invalid_handle = ctypes.c_void_p(-1).value
        if handle == invalid_handle:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            information = FByHandleFileInformation()
            if not get_information(handle, ctypes.byref(information)):
                raise ctypes.WinError(ctypes.get_last_error())
            handle_file_index = (
                int(information.nFileIndexHigh) << 32
            ) | int(information.nFileIndexLow)
            after = _checked_stat(path, expected_directory=True)
            _validate_directory_pin(
                path,
                before,
                after,
                int(information.dwVolumeSerialNumber),
                handle_file_index,
                int(information.dwFileAttributes),
                expected_identity,
            )
            physical_path = _windows_final_path(kernel32, handle)
            yield FDirectoryPin(
                identity=_path_identity(after),
                physical_path=physical_path,
                descriptor=None,
            )
            final = _checked_stat(path, expected_directory=True)
            if _path_identity(final) != _path_identity(after):
                raise ValueError("directory changed while pinned: {}".format(path))
            if _windows_final_path(kernel32, handle) != physical_path:
                raise ValueError("directory physical path changed while pinned: {}".format(path))
        finally:
            if not close_handle(handle):
                raise ctypes.WinError(ctypes.get_last_error())
        return

    flags = os.O_RDONLY
    flags |= int(getattr(os, "O_DIRECTORY", 0))
    flags |= int(getattr(os, "O_NOFOLLOW", 0))
    if parent_descriptor is None:
        if entry_name is not None:
            raise ValueError("entry_name requires a parent descriptor")
        before = _checked_stat(path, expected_directory=True)
        file_descriptor = os.open(str(path), flags)
    else:
        if entry_name is None or "/" in entry_name or "\\" in entry_name:
            raise ValueError("relative directory entry name is not exact")
        before = os.stat(entry_name, dir_fd=parent_descriptor, follow_symlinks=False)
        if _is_reparse(before) or not stat.S_ISDIR(before.st_mode):
            raise ValueError("relative directory entry is not a regular directory")
        file_descriptor = os.open(entry_name, flags, dir_fd=parent_descriptor)
    try:
        opened = os.fstat(file_descriptor)
        if parent_descriptor is None:
            after = _checked_stat(path, expected_directory=True)
        else:
            after = os.stat(entry_name, dir_fd=parent_descriptor, follow_symlinks=False)
        if (
            _path_identity(before) != _path_identity(opened)
            or _path_identity(opened) != _path_identity(after)
            or (
                expected_identity is not None
                and _path_identity(after) != expected_identity
            )
        ):
            raise ValueError("directory changed while acquiring pin: {}".format(path))
        yield FDirectoryPin(
            identity=_path_identity(opened),
            physical_path=str(path.absolute()),
            descriptor=file_descriptor,
        )
        if parent_descriptor is None:
            final = _checked_stat(path, expected_directory=True)
        else:
            final = os.stat(entry_name, dir_fd=parent_descriptor, follow_symlinks=False)
        if _path_identity(final) != _path_identity(after):
            raise ValueError("directory changed while pinned: {}".format(path))
    finally:
        os.close(file_descriptor)


def _record_directory(
    path: Path,
    repository_root: Path,
    directories: Dict[str, FDirectorySnapshot],
    identity: FPathIdentity,
) -> None:
    """directoryのidentityを一度だけsnapshotへ記録する。"""

    relative_path = "." if path == repository_root else path.relative_to(repository_root).as_posix()
    item = FDirectorySnapshot(
        path=relative_path,
        identity=identity,
    )
    former = directories.get(relative_path)
    if former is not None and former != item:
        raise ValueError("directory identity changed during snapshot: {}".format(path))
    directories[relative_path] = item


def _pin_directory(
    path: Path,
    repository_root: Path,
    directories: Dict[str, FDirectorySnapshot],
    stack: ExitStack,
    held_paths: Dict[str, FDirectoryPin],
    expected_identity: Optional[FPathIdentity] = None,
    parent_pin: Optional[FDirectoryPin] = None,
    entry_name: Optional[str] = None,
) -> FDirectoryPin:
    """directory handleをsnapshot完了まで一度だけ保持する。"""

    key = str(path.absolute())
    if key not in held_paths:
        parent_descriptor = None
        if os.name != "nt" and parent_pin is not None:
            parent_descriptor = parent_pin.descriptor
            if parent_descriptor is None:
                raise ValueError("POSIX parent directory pin has no descriptor")
        relative_entry_name = entry_name if os.name != "nt" else None
        held_paths[key] = stack.enter_context(
            _held_directory(
                path,
                expected_identity,
                parent_descriptor,
                relative_entry_name,
            )
        )
    elif expected_identity is not None:
        if held_paths[key].identity != expected_identity:
            raise ValueError(
                "directory entry identity changed after pin: {}".format(path)
            )
    pin = held_paths[key]
    if os.name == "nt" and parent_pin is not None and entry_name is not None:
        expected_physical_path = str(Path(parent_pin.physical_path) / entry_name)
        if _windows_physical_path_key(pin.physical_path) != _windows_physical_path_key(
            expected_physical_path
        ):
            raise ValueError(
                "directory handle escaped its physical parent: {}".format(path)
            )
    _record_directory(path, repository_root, directories, pin.identity)
    return pin


def _record_ancestor_chain(
    path: Path,
    repository_root: Path,
    directories: Dict[str, FDirectorySnapshot],
    stack: ExitStack,
    held_paths: Dict[str, FDirectoryPin],
) -> None:
    """repository rootからpath親までをno-follow pinする。"""

    relative_path = path.relative_to(repository_root)
    current_path = repository_root
    current_pin = _pin_directory(
        current_path, repository_root, directories, stack, held_paths
    )
    for component in relative_path.parts[:-1]:
        current_path = current_path / component
        current_pin = _pin_directory(
            current_path,
            repository_root,
            directories,
            stack,
            held_paths,
            parent_pin=current_pin,
            entry_name=component,
        )


def _capture_state(
    file_stat: os.stat_result,
) -> Tuple[FPathIdentity, int, int]:
    """open前後を比較するidentity、size、mtimeを返す。"""

    if _is_reparse(file_stat):
        raise ValueError("reparse file state is not allowed")
    return (
        _path_identity(file_stat),
        int(file_stat.st_size),
        int(getattr(file_stat, "st_mtime_ns", 0)),
    )


def _validate_capture_states(
    path: Path,
    before: os.stat_result,
    opened: os.stat_result,
    after_open: os.stat_result,
    after_path: os.stat_result,
    byte_count: int,
) -> None:
    """pathとopen handleの4時点が同じ通常fileかを検証する。"""

    states = {
        _capture_state(before),
        _capture_state(opened),
        _capture_state(after_open),
        _capture_state(after_path),
    }
    if len(states) != 1 or byte_count != int(after_open.st_size):
        raise ValueError("file changed while reading snapshot: {}".format(path))


def _capture_file(
    path: Path,
    repository_root: Path,
    expected_identity: Optional[FPathIdentity] = None,
    parent_pin: Optional[FDirectoryPin] = None,
    entry_name: Optional[str] = None,
) -> FTargetFileSnapshot:
    """no-follow確認した同じopen handleからfile bytesを取得する。"""

    use_relative_open = os.name != "nt" and parent_pin is not None
    if use_relative_open:
        if parent_pin.descriptor is None:
            raise ValueError("POSIX parent directory pin has no descriptor")
        if entry_name is None or "/" in entry_name or "\\" in entry_name:
            raise ValueError("relative file entry name is not exact")
        before = os.stat(
            entry_name,
            dir_fd=parent_pin.descriptor,
            follow_symlinks=False,
        )
        if _is_reparse(before) or not stat.S_ISREG(before.st_mode):
            raise ValueError("relative entry is not a regular file")
    else:
        before = _checked_stat(path, expected_directory=False)
    if expected_identity is not None and _path_identity(before) != expected_identity:
        raise ValueError("file entry identity changed before open: {}".format(path))
    open_flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOINHERIT", 0)
    open_flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    if use_relative_open:
        file_descriptor = os.open(
            entry_name,
            open_flags,
            dir_fd=parent_pin.descriptor,
        )
    else:
        file_descriptor = os.open(str(path), open_flags)
    try:
        opened = os.fstat(file_descriptor)
        if _capture_state(before) != _capture_state(opened):
            raise ValueError("file identity changed before handle read: {}".format(path))
        chunks: List[bytes] = []
        while True:
            chunk = os.read(file_descriptor, 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
        raw = b"".join(chunks)
        after_open = os.fstat(file_descriptor)
        if use_relative_open:
            after_path = os.stat(
                entry_name,
                dir_fd=parent_pin.descriptor,
                follow_symlinks=False,
            )
        else:
            after_path = _checked_stat(path, expected_directory=False)
        _validate_capture_states(
            path, before, opened, after_open, after_path, len(raw)
        )
    finally:
        os.close(file_descriptor)
    return FTargetFileSnapshot(
        path=path.relative_to(repository_root).as_posix(),
        identity=_path_identity(after_path),
        byte_count=len(raw),
        bytes_sha256=hashlib.sha256(raw).hexdigest().upper(),
        raw=raw,
    )


def _eager_entry_identity(
    entry: os.DirEntry[str], parent_pin: FDirectoryPin
) -> Tuple[os.stat_result, FPathIdentity]:
    """scandir iterator中にentryのno-follow identityを固定する。"""

    entry_stat = entry.stat(follow_symlinks=False)
    if _is_reparse(entry_stat):
        raise ValueError("reparse entry is not allowed: {}".format(entry.name))
    entry_inode = int(entry.inode())
    if entry_inode == 0:
        raise ValueError("directory entry has no stable inode: {}".format(entry.name))
    entry_device = int(entry_stat.st_dev)
    if entry_device == 0:
        entry_device = parent_pin.identity.device
    identity = FPathIdentity(
        device=entry_device,
        inode=entry_inode,
        mode=int(stat.S_IFMT(entry_stat.st_mode)),
        file_attributes=int(getattr(entry_stat, "st_file_attributes", 0)),
    )
    return entry_stat, identity


def _directory_scandir(
    directory: Path, directory_pin: FDirectoryPin
) -> os.ScandirIterator[str]:
    """Windowsはphysical path、POSIXは保持中fdから列挙する。"""

    if os.name == "nt":
        return os.scandir(directory_pin.physical_path)
    if directory_pin.descriptor is None:
        raise ValueError("POSIX directory pin has no descriptor")
    return os.scandir(directory_pin.descriptor)


def _capture_directory_tree(
    directory: Path,
    repository_root: Path,
    directories: Dict[str, FDirectorySnapshot],
    files: Dict[str, FTargetFileSnapshot],
    stack: ExitStack,
    held_paths: Dict[str, FDirectoryPin],
    expected_identity: Optional[FPathIdentity] = None,
    parent_pin: Optional[FDirectoryPin] = None,
    entry_name: Optional[str] = None,
) -> None:
    """directoryをfollowせず1回列挙してtext fileをsnapshot化する。"""

    directory_pin = _pin_directory(
        directory,
        repository_root,
        directories,
        stack,
        held_paths,
        expected_identity,
        parent_pin,
        entry_name,
    )
    with _directory_scandir(directory, directory_pin) as iterator:
        for entry in iterator:
            path = directory / entry.name
            entry_stat, eager_identity = _eager_entry_identity(entry, directory_pin)
            if os.name == "nt":
                expected_directory = stat.S_ISDIR(entry_stat.st_mode)
                if expected_directory or stat.S_ISREG(entry_stat.st_mode):
                    path_identity = _path_identity(
                        _checked_stat(path, expected_directory=expected_directory)
                    )
                    if path_identity != eager_identity:
                        raise ValueError(
                            "directory entry changed before handle acquisition: {}".format(
                                path
                            )
                        )
            if stat.S_ISDIR(entry_stat.st_mode):
                _capture_directory_tree(
                    path,
                    repository_root,
                    directories,
                    files,
                    stack,
                    held_paths,
                    eager_identity,
                    directory_pin,
                    entry.name,
                )
                continue
            if not stat.S_ISREG(entry_stat.st_mode) or not _is_text_target(path):
                continue
            item = _capture_file(
                path,
                repository_root,
                eager_identity,
                directory_pin,
                entry.name,
            )
            if item.path in files:
                raise ValueError("duplicate target path: {}".format(item.path))
            files[item.path] = item


def _pin_absolute_ancestor_chain(
    path: Path,
    stack: ExitStack,
    held_paths: Dict[str, FDirectoryPin],
    expected_identity: Optional[FPathIdentity] = None,
) -> FDirectoryPin:
    """volume rootから対象directoryまでを順にpinする。"""

    if os.name == "nt" and len(path.parts) >= 2 and path.parts[0] == "\\\\?\\":
        volume_root = Path(path.parts[0] + path.parts[1] + "\\")
        chain = [volume_root]
        current_path = volume_root
        for component in path.parts[2:]:
            current_path = current_path / component
            chain.append(current_path)
    else:
        chain = list(reversed(path.parents)) + [path]
    parent_pin: Optional[FDirectoryPin] = None
    current_pin: Optional[FDirectoryPin] = None
    for ancestor in chain:
        key = str(ancestor.absolute())
        if key in held_paths:
            current_pin = held_paths[key]
        else:
            ancestor_expected = expected_identity if ancestor == path else None
            parent_descriptor = None
            entry_name = None
            if os.name != "nt" and parent_pin is not None:
                parent_descriptor = parent_pin.descriptor
                entry_name = ancestor.name
            current_pin = stack.enter_context(
                _held_directory(
                    ancestor,
                    ancestor_expected,
                    parent_descriptor,
                    entry_name,
                )
            )
            held_paths[key] = current_pin
        if os.name == "nt" and _windows_physical_path_key(
            current_pin.physical_path
        ) != _windows_physical_path_key(_native_path_text(ancestor)):
            raise ValueError("absolute directory handle escaped its physical path")
        parent_pin = current_pin
    if current_pin is None:
        raise ValueError("absolute directory chain is empty")
    if expected_identity is not None and current_pin.identity != expected_identity:
        raise ValueError("absolute directory identity changed")
    return current_pin


def _capture_repository_snapshot(acs_root: Path) -> FRepositorySnapshot:
    """対象path集合、identity、bytes hashを単一snapshotとして取得する。"""

    directories: Dict[str, FDirectorySnapshot] = {}
    files: Dict[str, FTargetFileSnapshot] = {}
    held_paths: Dict[str, FDirectoryPin] = {}
    with ExitStack() as stack:
        requested_acs_root = acs_root.absolute()
        requested_repository_root = requested_acs_root.parent
        if os.name == "nt":
            repository_alias_pin = stack.enter_context(
                _held_directory(requested_repository_root)
            )
            held_paths[str(requested_repository_root)] = repository_alias_pin
            acs_alias_pin = stack.enter_context(_held_directory(requested_acs_root))
            held_paths[str(requested_acs_root)] = acs_alias_pin
            repository_root = Path(repository_alias_pin.physical_path)
            physical_acs_root = Path(acs_alias_pin.physical_path)
            if physical_acs_root.parent != repository_root:
                raise ValueError("ACS physical root is outside repository root")
            repository_pin = _pin_absolute_ancestor_chain(
                repository_root,
                stack,
                held_paths,
                repository_alias_pin.identity,
            )
            acs_root = physical_acs_root
        else:
            repository_root = requested_repository_root
            repository_pin = _pin_absolute_ancestor_chain(
                repository_root, stack, held_paths
            )
            acs_root = repository_root / requested_acs_root.name
        _record_directory(
            repository_root,
            repository_root,
            directories,
            repository_pin.identity,
        )
        acs_pin = _pin_directory(
            acs_root,
            repository_root,
            directories,
            stack,
            held_paths,
            acs_alias_pin.identity if os.name == "nt" else None,
            repository_pin,
            acs_root.name,
        )
        for relative_directory in TARGET_DIRECTORIES:
            directory = acs_root / relative_directory
            _record_ancestor_chain(
                directory / "sentinel",
                repository_root,
                directories,
                stack,
                held_paths,
            )
            _capture_directory_tree(
                directory,
                repository_root,
                directories,
                files,
                stack,
                held_paths,
            )
        for relative_path in TARGET_REPOSITORY_FILES:
            path = repository_root / relative_path
            _record_ancestor_chain(
                path, repository_root, directories, stack, held_paths
            )
            parent_path = path.parent
            parent_pin = held_paths.get(str(parent_path.absolute()))
            if parent_pin is None:
                raise ValueError("target parent directory is not pinned")
            item = _capture_file(
                path,
                repository_root,
                parent_pin=parent_pin,
                entry_name=path.name,
            )
            if item.path in files:
                raise ValueError("duplicate target path: {}".format(item.path))
            files[item.path] = item
    return FRepositorySnapshot(
        directories=tuple(sorted(directories.values())),
        files=tuple(sorted(files.values())),
    )


def _snapshot_diagnostics(
    initial: FRepositorySnapshot, final: FRepositorySnapshot
) -> List[str]:
    """new/delete/replace/content raceを決定的に報告する。"""

    diagnostics: List[str] = []
    initial_directories = {item.path: item for item in initial.directories}
    final_directories = {item.path: item for item in final.directories}
    initial_files = {item.path: item for item in initial.files}
    final_files = {item.path: item for item in final.files}
    for path in sorted(set(initial_directories) | set(final_directories)):
        if path not in initial_directories:
            diagnostics.append("snapshot-new-directory:{}".format(path))
        elif path not in final_directories:
            diagnostics.append("snapshot-deleted-directory:{}".format(path))
        elif initial_directories[path] != final_directories[path]:
            diagnostics.append("snapshot-replaced-directory:{}".format(path))
    for path in sorted(set(initial_files) | set(final_files)):
        if path not in initial_files:
            diagnostics.append("snapshot-new-file:{}".format(path))
        elif path not in final_files:
            diagnostics.append("snapshot-deleted-file:{}".format(path))
        elif initial_files[path] != final_files[path]:
            diagnostics.append("snapshot-replaced-or-changed-file:{}".format(path))
    return diagnostics


def _snapshot_file(snapshot: FRepositorySnapshot, path: str) -> FTargetFileSnapshot:
    """snapshot内のexact pathを1件だけ返す。"""

    matches = [item for item in snapshot.files if item.path == path]
    if len(matches) != 1:
        raise ValueError("snapshot file must exist exactly once: {}".format(path))
    return matches[0]


def _cpp_line_contexts(lines: Sequence[str]) -> Tuple[List[List[str]], List[str]]:
    """C++ comment/stringをmaskし、各行のpreprocessor状態を返す。"""

    context_lines: List[List[str]] = []
    masked_lines: List[str] = []
    state = "code"
    raw_terminator = ""
    for line in lines:
        contexts = ["code"] * len(line)
        masked = list(line)
        position = 0
        while position < len(line):
            if state == "block_comment":
                end = line.find("*/", position)
                stop = len(line) if end < 0 else end + 2
                for index in range(position, stop):
                    contexts[index] = "block_comment"
                    masked[index] = " "
                position = stop
                if end >= 0:
                    state = "code"
                continue
            if state == "raw_string":
                end = line.find(raw_terminator, position)
                stop = len(line) if end < 0 else end + len(raw_terminator)
                for index in range(position, stop):
                    contexts[index] = "raw_string"
                    masked[index] = " "
                position = stop
                if end >= 0:
                    state = "code"
                    raw_terminator = ""
                continue
            if state in {"string", "character"}:
                delimiter = '"' if state == "string" else "'"
                escaped = False
                while position < len(line):
                    contexts[position] = state
                    masked[position] = " "
                    character = line[position]
                    position += 1
                    if escaped:
                        escaped = False
                    elif character == "\\":
                        escaped = True
                    elif character == delimiter:
                        state = "code"
                        break
                continue

            if line.startswith("//", position):
                for index in range(position, len(line)):
                    contexts[index] = "line_comment"
                    masked[index] = " "
                position = len(line)
                continue
            if line.startswith("/*", position):
                state = "block_comment"
                continue
            if line.startswith('R"', position):
                opening = line.find("(", position + 2, min(len(line), position + 19))
                if opening >= 0:
                    delimiter = line[position + 2 : opening]
                    raw_terminator = ")" + delimiter + '"'
                    state = "raw_string"
                    continue
            if line[position] == '"':
                contexts[position] = "string"
                masked[position] = " "
                position += 1
                state = "string"
                continue
            if line[position] == "'":
                contexts[position] = "character"
                masked[position] = " "
                position += 1
                state = "character"
                continue
            position += 1
        context_lines.append(contexts)
        masked_lines.append("".join(masked))

    preprocessor_states: List[str] = []
    current_state = "active"
    stack: List[Tuple[str, str, bool]] = []
    for masked in masked_lines:
        preprocessor_states.append(current_state)
        directive = masked.lstrip()
        literal_if = re.match(r"^#\s*if\s*\(?\s*([01])\s*\)?\s*(?://.*)?$", directive)
        if literal_if is not None:
            branch_value = literal_if.group(1) == "1"
            branch_kind = "known_true" if branch_value else "known_false"
            stack.append((current_state, branch_kind, branch_value))
            if not branch_value or current_state == "inactive":
                current_state = "inactive"
            else:
                current_state = current_state
        elif re.match(r"^#\s*(?:if|ifdef|ifndef)\b", directive):
            stack.append((current_state, "unknown", False))
            current_state = (
                "inactive" if current_state == "inactive" else "conditional"
            )
        elif re.match(r"^#\s*else\b", directive) and stack:
            parent_state, branch_kind, branch_taken = stack[-1]
            if parent_state == "inactive" or branch_taken:
                current_state = "inactive"
            elif branch_kind == "unknown":
                current_state = "conditional"
            else:
                current_state = parent_state
            stack[-1] = (parent_state, branch_kind, True)
        elif re.match(r"^#\s*elif\b", directive) and stack:
            parent_state, branch_kind, branch_taken = stack[-1]
            literal_elif = re.match(
                r"^#\s*elif\s*\(?\s*([01])\s*\)?\s*$", directive
            )
            if parent_state == "inactive" or branch_taken:
                current_state = "inactive"
            elif branch_kind == "unknown" or literal_elif is None:
                current_state = "conditional"
                branch_kind = "unknown"
            elif literal_elif.group(1) == "1":
                current_state = parent_state
                branch_taken = True
            else:
                current_state = "inactive"
            stack[-1] = (parent_state, branch_kind, branch_taken)
        elif re.match(r"^#\s*endif\b", directive) and stack:
            parent_state, _, _ = stack.pop()
            current_state = parent_state
    return context_lines, preprocessor_states


def _qualifier_before(line: str, position: int) -> str:
    """旧basename直前の明示namespace qualifierを返す。"""

    prefix = line[:position]
    match = re.search(r"((?:[A-Za-z_][A-Za-z0-9_]*::)+)$", prefix)
    return "" if match is None else match.group(1)[:-2]


def _statement_around(
    source_text: str, position: int, keyword: str
) -> Tuple[int, str]:
    """positionを含むkeyword statementの開始位置と本文を返す。"""

    start = source_text.rfind(keyword, 0, position + 1)
    if start < 0 or source_text.rfind(";", start, position) >= 0:
        return -1, ""
    end = source_text.find(";", position)
    return (-1, "") if end < 0 else (start, source_text[start : end + 1])


def _compatibility_assertion_kind(
    source_text: str,
    position: int,
    lexical_context: str,
    legacy: str,
    canonical: str,
    relative_path: str,
) -> str:
    """旧名がexact is_same assertionの型引数またはmessageかを返す。"""

    statement_start, statement = _statement_around(
        source_text, position, "static_assert"
    )
    if not statement:
        return ""
    match = re.search(
        r"(?:std\s*::\s*)?(?:is_same_v|IsSameV)\s*<\s*([^,>]+)\s*,\s*([^>]+)\s*>",
        statement,
        flags=re.DOTALL,
    )
    if match is None:
        return ""
    operand_spans = (
        (
            statement_start + match.start(1),
            statement_start + match.end(1),
        ),
        (
            statement_start + match.start(2),
            statement_start + match.end(2),
        ),
    )
    if not any(start <= position < end for start, end in operand_spans):
        return ""
    left_type = re.sub(r"\s+", "", match.group(1)).lstrip(":")
    right_type = re.sub(r"\s+", "", match.group(2)).lstrip(":")
    left_name = left_type.rsplit("::", 1)[-1]
    right_name = right_type.rsplit("::", 1)[-1]
    canonical_name = canonical.rsplit("::", 1)[-1]
    canonical_pair = {left_name, right_name} == {legacy, canonical_name}
    canonical_namespace = canonical.rsplit("::", 1)[0]
    legacy_qualified = canonical_namespace + "::" + legacy
    canonical_variants = {
        canonical,
        "acs::" + canonical_name,
        "acs::game::" + canonical_name,
    }
    legacy_variants = {
        legacy_qualified,
        "acs::" + legacy,
        "acs::game::" + legacy,
    }
    qualified_pair_valid = (
        left_type in canonical_variants and right_type in legacy_variants
    ) or (
        right_type in canonical_variants and left_type in legacy_variants
    )
    unqualified_pair = "::" not in left_type and "::" not in right_type
    canonical_pair = canonical_pair and (qualified_pair_valid or unqualified_pair)
    top_level_legacy = "acs::" + legacy
    reexport_pair = (
        relative_path == "acs/tests/gameframework_forward_header_compile_tests.cpp"
        and {left_type, right_type} == {top_level_legacy, legacy_qualified}
    )
    if not canonical_pair and not reexport_pair:
        return ""
    if lexical_context != "code":
        return ""
    return "compatibility_reexport_assertion" if reexport_pair else "compatibility_type_assertion"


def _runtime_literal_kind(source_text: str, position: int) -> str:
    """旧名literalが許可したcallのactual argumentかを返す。"""

    quote_start = source_text.rfind('"', 0, position + 1)
    quote_end = source_text.find('"', position)
    if quote_start < 0 or quote_end < position:
        return ""
    if re.match(r"\s*\)", source_text[quote_end + 1 :]) is None:
        return ""
    before_literal = source_text[:quote_start]
    if re.search(
        r"(?:\bCreateComponentByName|\b[A-Za-z_]\w*\s*(?:\.|->)\s*(?:FindByName|Create))\s*\(\s*$",
        before_literal,
        flags=re.DOTALL,
    ):
        return "runtime_type_lookup"
    strcmp_start = before_literal.rfind("strcmp")
    if strcmp_start < 0:
        return ""
    strcmp_prefix = before_literal[strcmp_start:]
    match = re.fullmatch(r"strcmp\s*\((.*),\s*", strcmp_prefix, flags=re.DOTALL)
    if match is None:
        return ""
    first_argument = match.group(1)
    if re.search(
        r"(?:(?:\.|->)\s*Name\s*\(\s*\)|->\s*name)\s*$",
        first_argument,
    ):
        return "runtime_name_comparison"
    return ""


def _is_first_macro_argument(
    source_text: str, position: int, macro_name: str
) -> bool:
    """positionがmacro callの先頭argument直後かを判定する。"""

    return re.search(
        r"\b{}\s*\(\s*$".format(re.escape(macro_name)),
        source_text[:position],
        flags=re.DOTALL,
    ) is not None


def _construct_for_occurrence(
    relative_path: str,
    line: str,
    lexical_context: str,
    legacy: str,
    canonical: str,
    source_text: str,
    absolute_position: int,
) -> str:
    """旧名出現を自己申告でなくsource構文から分類する。"""

    lowered = line.lower()
    if lexical_context == "string":
        runtime_kind = _runtime_literal_kind(source_text, absolute_position)
        if runtime_kind:
            return runtime_kind
        assertion_kind = _compatibility_assertion_kind(
            source_text,
            absolute_position,
            lexical_context,
            legacy,
            canonical,
            relative_path,
        )
        if assertion_kind:
            return assertion_kind
        return "unsupported_string"
    if lexical_context in {"line_comment", "block_comment"}:
        if relative_path in COMPATIBILITY_FILE_PATHS and ("旧" in line or "legacy" in lowered):
            return "compatibility_comment"
        return "unsupported_comment"
    if lexical_context != "code":
        return "unsupported_{}".format(lexical_context)
    assertion_kind = _compatibility_assertion_kind(
        source_text,
        absolute_position,
        lexical_context,
        legacy,
        canonical,
        relative_path,
    )
    if assertion_kind:
        return assertion_kind
    if _is_first_macro_argument(source_text, absolute_position, "ACS_TEST"):
        return "runtime_test_suite_identity"
    if (
        relative_path == "acs/tests/application_subsystem_tests.cpp"
        and re.search(r"\b{}\s*\*\s*const\s+Legacy\w+\s*=".format(re.escape(legacy)), line)
    ):
        return "compatibility_alias_use"
    if relative_path == "acs/tests/aobject_header_compile_tests.cpp" and (
        "AsLegacyObject" in line or "AsCanonicalObject" in line
    ):
        return "compatibility_conversion"
    if (
        relative_path == "acs/tests/object_ptr_header_compile_tests.cpp"
        and "public acs::FObject" in line
    ):
        return "compatibility_inheritance"
    if relative_path == "acs/tests/subsystem_canonical_header_tests.cpp" and (
        "AcceptLegacy" in line
        or "CLegacyGameHeaderFirstConsumer" in line
        or "TUniquePtr<acs::FScene> InitialScene" in line
    ):
        return "compatibility_forward_surface"
    if (
        relative_path == "acs/tests/subsystem_spawn_header_compile_tests.cpp"
        and "AcceptLegacy" in line
    ):
        return "compatibility_forward_surface"
    return "unsupported_code"


def _phase2_source(source_text: str) -> Tuple[str, List[int]]:
    """translation phase2のline spliceを除去し元offset対応を返す。"""

    output: List[str] = []
    offsets: List[int] = []
    position = 0
    while position < len(source_text):
        if source_text.startswith("\\\r\n", position):
            position += 3
            continue
        if source_text.startswith("\\\n", position):
            position += 2
            continue
        output.append(source_text[position])
        offsets.append(position)
        position += 1
    return "".join(output), offsets


def _decode_cpp_string_content(content: str) -> Optional[str]:
    """通常C++ string literalのescapeを監査用Unicode列へ戻す。"""

    output: List[str] = []
    position = 0
    simple_escapes = {
        "'": "'",
        '"': '"',
        "?": "?",
        "\\": "\\",
        "a": "\a",
        "b": "\b",
        "f": "\f",
        "n": "\n",
        "r": "\r",
        "t": "\t",
        "v": "\v",
    }
    while position < len(content):
        if content[position] != "\\":
            output.append(content[position])
            position += 1
            continue
        position += 1
        if position >= len(content):
            return None
        escape = content[position]
        if escape in simple_escapes:
            output.append(simple_escapes[escape])
            position += 1
            continue
        if escape == "x":
            end = position + 1
            while end < len(content) and content[end] in "0123456789abcdefABCDEF":
                end += 1
            if end == position + 1:
                return None
            value = int(content[position + 1 : end], 16)
            if value > 0x10FFFF:
                return None
            output.append(chr(value))
            position = end
            continue
        if escape in "01234567":
            end = position + 1
            while end < min(len(content), position + 3) and content[end] in "01234567":
                end += 1
            output.append(chr(int(content[position:end], 8)))
            position = end
            continue
        if escape in {"u", "U"}:
            digit_count = 4 if escape == "u" else 8
            end = position + 1 + digit_count
            digits = content[position + 1 : end]
            if len(digits) != digit_count or re.fullmatch(r"[0-9A-Fa-f]+", digits) is None:
                return None
            value = int(digits, 16)
            if value > 0x10FFFF:
                return None
            output.append(chr(value))
            position = end
            continue
        return None
    return "".join(output)


def _parse_cpp_string_literal(source_text: str, position: int) -> Optional[Tuple[int, str]]:
    """positionから始まる通常またはraw C++ string literalを読む。"""

    raw_match = re.match(r"(?:(?:u8|u|U|L)?R)\"", source_text[position:])
    if raw_match is not None:
        quote_position = position + len(raw_match.group(0)) - 1
        opening = source_text.find(
            "(", quote_position + 1, min(len(source_text), quote_position + 18)
        )
        if opening < 0:
            return None
        delimiter = source_text[quote_position + 1 : opening]
        if any(character.isspace() or character in "()\\" for character in delimiter):
            return None
        terminator = ")" + delimiter + '"'
        closing = source_text.find(terminator, opening + 1)
        if closing < 0:
            return None
        return closing + len(terminator), source_text[opening + 1 : closing]
    ordinary_match = re.match(r"(?:u8|u|U|L)?\"", source_text[position:])
    if ordinary_match is None:
        return None
    quote_position = position + len(ordinary_match.group(0)) - 1
    end = quote_position + 1
    escaped = False
    while end < len(source_text):
        character = source_text[end]
        end += 1
        if escaped:
            escaped = False
        elif character == "\\":
            escaped = True
        elif character == '"':
            return end, _decode_cpp_string_content(
                source_text[quote_position + 1 : end - 1]
            ) or ""
    return None


def _cpp_tokens(source_text: str) -> List[FCppToken]:
    """commentを除外したbounded C++ token streamを作る。"""

    phase2, offsets = _phase2_source(source_text)
    tokens: List[FCppToken] = []
    position = 0
    while position < len(phase2):
        if phase2[position].isspace():
            position += 1
            continue
        if phase2.startswith("//", position):
            line_end = phase2.find("\n", position + 2)
            position = len(phase2) if line_end < 0 else line_end + 1
            continue
        if phase2.startswith("/*", position):
            comment_end = phase2.find("*/", position + 2)
            position = len(phase2) if comment_end < 0 else comment_end + 2
            continue
        token_start = position
        parsed_string = _parse_cpp_string_literal(phase2, position)
        if parsed_string is not None:
            position, literal_value = parsed_string
            literal_source = phase2[token_start:position]
            original_start = offsets[token_start]
            original_end = offsets[position - 1] + 1
            tokens.append(
                FCppToken(
                    "string",
                    literal_value,
                    original_start,
                    original_end,
                    source_text[original_start:original_end] != literal_source,
                )
            )
            continue
        identifier = re.match(r"[A-Za-z_][A-Za-z0-9_]*", phase2[position:])
        if identifier is not None:
            value = identifier.group(0)
            position += len(value)
            original_start = offsets[token_start]
            original_end = offsets[position - 1] + 1
            tokens.append(
                FCppToken(
                    "identifier",
                    value,
                    original_start,
                    original_end,
                    source_text[original_start:original_end] != value,
                )
            )
            continue
        operator = next(
            (
                value
                for value in ("##", "::", "->")
                if phase2.startswith(value, position)
            ),
            phase2[position],
        )
        position += len(operator)
        original_start = offsets[token_start]
        original_end = offsets[position - 1] + 1
        tokens.append(
            FCppToken("punct", operator, original_start, original_end, False)
        )
    return tokens


def _macro_call_arguments(
    tokens: Sequence[FCppToken], position: int
) -> Tuple[List[List[FCppToken]], int]:
    """macro名positionからbalanced argument列とcall終端を返す。"""

    if position + 1 >= len(tokens) or tokens[position + 1].value != "(":
        raise ValueError("function-like macro call requires an opening parenthesis")
    depth = 1
    cursor = position + 2
    arguments: List[List[FCppToken]] = [[]]
    while cursor < len(tokens) and depth > 0:
        token = tokens[cursor]
        if token.value == "(":
            depth += 1
        elif token.value == ")":
            depth -= 1
        if depth == 1 and token.value == ",":
            arguments.append([])
        elif depth > 0:
            arguments[-1].append(token)
        cursor += 1
    if depth != 0:
        raise ValueError("unbalanced ACS macro invocation")
    return arguments, cursor


def _macro_definitions_from_text(
    relative_path: str, source_text: str
) -> List[FMacroDefinition]:
    """1 sourceのactive function-like ACS macro定義を読む。"""

    phase2, _ = _phase2_source(source_text)
    lines = phase2.splitlines()
    contexts, states = _cpp_line_contexts(lines)
    definitions: List[FMacroDefinition] = []
    for line_number, line in enumerate(lines, 1):
        masked = "".join(
            character if contexts[line_number - 1][index] == "code" else " "
            for index, character in enumerate(line)
        )
        define_prefix = re.match(r"^\s*#\s*define\s+(ACS_[A-Za-z0-9_]+)", masked)
        if define_prefix is None or states[line_number - 1] == "inactive":
            continue
        name = define_prefix.group(1)
        after_name = define_prefix.end()
        if after_name >= len(masked) or masked[after_name] != "(":
            continue
        definition = re.match(
            r"^\s*#\s*define\s+(ACS_[A-Za-z0-9_]+)\(([^)]*)\)\s*(.*)$",
            line,
        )
        if definition is None:
            raise ValueError(
                "malformed function-like ACS macro definition: {}:{}".format(
                    relative_path, line_number
                )
            )
        raw_parameters = definition.group(2).strip()
        parameters: List[str] = []
        if raw_parameters:
            for raw_parameter in raw_parameters.split(","):
                parameter = raw_parameter.strip()
                if parameter == "...":
                    parameter = "__VA_ARGS__"
                if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", parameter) is None:
                    raise ValueError(
                        "unsupported ACS macro parameter: {}:{}:{}".format(
                            relative_path, line_number, parameter
                        )
                    )
                if parameter in parameters:
                    raise ValueError(
                        "duplicate ACS macro parameter: {}:{}:{}".format(
                            relative_path, line_number, parameter
                        )
                    )
                parameters.append(parameter)
        definitions.append(
            FMacroDefinition(
                path=relative_path,
                line=line_number,
                name=name,
                parameters=tuple(parameters),
                body_tokens=tuple(_cpp_tokens(definition.group(3))),
                preprocessor_state=states[line_number - 1],
            )
        )
    return definitions


def _macro_definitions(snapshot: FRepositorySnapshot) -> Tuple[FMacroDefinition, ...]:
    """src snapshotからactive ACS macro定義を一意に収集する。"""

    definitions: List[FMacroDefinition] = []
    for item in snapshot.files:
        if not item.path.startswith("acs/src/") or (
            Path(item.path).suffix.lower() not in CPP_TEXT_SUFFIXES
        ):
            continue
        raw = item.raw[3:] if item.raw.startswith(b"\xef\xbb\xbf") else item.raw
        definitions.extend(
            _macro_definitions_from_text(item.path, raw.decode("utf-8"))
        )
    names: Dict[str, List[FMacroDefinition]] = collections.defaultdict(list)
    for definition in definitions:
        if definition.preprocessor_state == "active" and any(
            item.preprocessor_state == "active" for item in names[definition.name]
        ):
            raise ValueError(
                "duplicate active ACS macro definition: {}".format(definition.name)
            )
        names[definition.name].append(definition)
    return tuple(
        sorted(definitions, key=lambda item: (item.name, item.path, item.line))
    )


def _exact_parameter_index(
    argument: Sequence[FCppToken], parameters: Sequence[str]
) -> Optional[int]:
    """callee argumentがcaller parameterそのものならindexを返す。"""

    tokens = list(argument)
    while len(tokens) >= 2 and tokens[0].value == "(" and tokens[-1].value == ")":
        depth = 0
        closes_at_end = False
        for position, token in enumerate(tokens):
            if token.value == "(":
                depth += 1
            elif token.value == ")":
                depth -= 1
                if depth == 0:
                    closes_at_end = position == len(tokens) - 1
                    break
        if not closes_at_end:
            break
        tokens = tokens[1:-1]
    if len(tokens) != 1 or tokens[0].kind != "identifier":
        return None
    try:
        return tuple(parameters).index(tokens[0].value)
    except ValueError:
        return None


def _identity_macro_catalog(
    definitions: Sequence[FMacroDefinition],
) -> Dict[str, frozenset[int]]:
    """stringize/paste/wrapper推移からidentity argumentを固定点計算する。"""

    parameter_lists: Dict[str, Tuple[str, ...]] = {}
    for definition in definitions:
        previous = parameter_lists.setdefault(
            definition.name, definition.parameters
        )
        if previous != definition.parameters:
            raise ValueError(
                "conditional ACS macro parameter mismatch: {}".format(
                    definition.name
                )
            )
    by_name = set(parameter_lists)
    identity: Dict[str, set[int]] = {
        name: set() for name in parameter_lists
    }
    calls: Dict[str, List[Tuple[str, List[List[FCppToken]]]]] = {
        name: [] for name in parameter_lists
    }
    graph: Dict[str, set[str]] = {name: set() for name in parameter_lists}
    for definition in definitions:
        parameters = definition.parameters
        for token_index, token in enumerate(definition.body_tokens):
            if token.kind != "identifier" or token.value not in parameters:
                continue
            parameter_is_type_identity = token.value in {"T", "Type"} or (
                definition.name == "ACS_TEST" and token.value == "suite"
            )
            if (
                parameter_is_type_identity
                and (
                    (
                        token_index > 0
                        and definition.body_tokens[token_index - 1].value
                        in {"#", "##"}
                    )
                    or (
                        token_index + 1 < len(definition.body_tokens)
                        and definition.body_tokens[token_index + 1].value == "##"
                    )
                )
            ):
                identity[definition.name].add(parameters.index(token.value))
        token_index = 0
        while token_index + 1 < len(definition.body_tokens):
            token = definition.body_tokens[token_index]
            if (
                token.kind != "identifier"
                or not token.value.startswith("ACS_")
                or definition.body_tokens[token_index + 1].value != "("
            ):
                token_index += 1
                continue
            if token.value not in by_name:
                raise ValueError(
                    "unknown ACS macro invocation in {}: {}".format(
                        definition.name, token.value
                    )
                )
            arguments, call_end = _macro_call_arguments(
                definition.body_tokens, token_index
            )
            calls[definition.name].append((token.value, arguments))
            graph[definition.name].add(token.value)
            token_index = call_end

    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(name: str) -> None:
        """catalog wrapper cycleをfail-closedで検出する。"""

        if name in visiting:
            raise ValueError("cyclic ACS macro wrapper: {}".format(name))
        if name in visited:
            return
        visiting.add(name)
        for callee in sorted(graph[name]):
            visit(callee)
        visiting.remove(name)
        visited.add(name)

    for name in sorted(graph):
        visit(name)

    changed = True
    while changed:
        changed = False
        for definition in definitions:
            for callee, arguments in calls[definition.name]:
                for argument_index in identity[callee]:
                    if argument_index >= len(arguments):
                        raise ValueError(
                            "identity ACS macro argument is missing: {} -> {}".format(
                                definition.name, callee
                            )
                        )
                    parameter_index = _exact_parameter_index(
                        arguments[argument_index],
                        parameter_lists[definition.name],
                    )
                    if (
                        parameter_index is not None
                        and parameter_index not in identity[definition.name]
                    ):
                        identity[definition.name].add(parameter_index)
                        changed = True
    return {
        name: frozenset(sorted(indexes))
        for name, indexes in sorted(identity.items())
    }


def _identity_macro_contract_diagnostics(
    catalog: Mapping[str, frozenset[int]],
) -> List[str]:
    """導出catalogのidentity型引数集合をexplicit契約と照合する。"""

    actual = {name: indexes for name, indexes in catalog.items() if indexes}
    diagnostics: List[str] = []
    for name in sorted(set(actual) | set(EXPECTED_IDENTITY_MACRO_INDEXES)):
        if name not in actual:
            diagnostics.append("identity-macro-missing:{}".format(name))
        elif name not in EXPECTED_IDENTITY_MACRO_INDEXES:
            diagnostics.append("identity-macro-unexpected:{}".format(name))
        elif actual[name] != EXPECTED_IDENTITY_MACRO_INDEXES[name]:
            diagnostics.append(
                "identity-macro-indexes:{}:expected={}:actual={}".format(
                    name,
                    sorted(EXPECTED_IDENTITY_MACRO_INDEXES[name]),
                    sorted(actual[name]),
                )
            )
    return diagnostics


def _identity_macro_catalog_sha256(
    definitions: Sequence[FMacroDefinition],
    catalog: Mapping[str, frozenset[int]],
) -> str:
    """macro定義tokenと導出catalogのsemantic SHAを返す。"""

    definition_rows = [
        [
            definition.path,
            definition.line,
            definition.name,
            list(definition.parameters),
            [[token.kind, token.value] for token in definition.body_tokens],
            definition.preprocessor_state,
        ]
        for definition in definitions
    ]
    catalog_rows = [
        [name, sorted(catalog[name])] for name in sorted(catalog)
    ]
    return _canonical_json_sha256([definition_rows, catalog_rows])


def _identity_macro_rows_for_text(
    relative_path: str,
    source_text: str,
    mapping: Mapping[str, str],
    catalog: Mapping[str, frozenset[int]],
) -> List[List[object]]:
    """実行時identity macroのmapped exact型引数を行へする。"""

    tokens = _cpp_tokens(source_text)
    mapped_names = set(mapping)
    mapped_names.update(name.rsplit("::", 1)[-1] for name in mapping.values())
    rows: List[List[object]] = []
    position = 0
    while position + 1 < len(tokens):
        macro_token = tokens[position]
        if (
            macro_token.kind != "identifier"
            or not macro_token.value.startswith("ACS_")
            or tokens[position + 1].value != "("
        ):
            position += 1
            continue
        line_number = source_text.count("\n", 0, macro_token.start) + 1
        source_line = source_text.splitlines()[line_number - 1]
        if source_line.lstrip().startswith("#"):
            position += 1
            continue
        if macro_token.value not in catalog:
            raise ValueError(
                "unknown ACS macro invocation in consumer: {}:{}:{}".format(
                    relative_path, line_number, macro_token.value
                )
            )
        arguments, call_end = _macro_call_arguments(tokens, position)
        for argument_index in sorted(catalog[macro_token.value]):
            if argument_index >= len(arguments):
                raise ValueError(
                    "identity macro argument is missing: {}:{}:{}".format(
                        relative_path, line_number, macro_token.value
                    )
                )
            argument = list(arguments[argument_index])
            while argument and argument[0].value == "::":
                argument = argument[1:]
            if not argument or any(
                token.kind != "identifier"
                if token_index % 2 == 0
                else token.value != "::"
                for token_index, token in enumerate(argument)
            ):
                continue
            argument_name = "".join(token.value for token in argument)
            if argument_name.rsplit("::", 1)[-1] in mapped_names:
                rows.append(
                    [relative_path, line_number, macro_token.value, argument_name]
                )
        position = call_end
    return rows


def _identity_macro_rows(
    snapshot: FRepositorySnapshot,
    mapping: Mapping[str, str],
    catalog: Mapping[str, frozenset[int]],
) -> List[List[object]]:
    """consumer snapshotのmapped identity macro行を決定的順序で返す。"""

    rows: List[List[object]] = []
    for item in snapshot.files:
        if item.path.startswith("acs/src/") or (
            Path(item.path).suffix.lower() not in CPP_TEXT_SUFFIXES
        ):
            continue
        raw = item.raw[3:] if item.raw.startswith(b"\xef\xbb\xbf") else item.raw
        rows.extend(
            _identity_macro_rows_for_text(
                item.path, raw.decode("utf-8"), mapping, catalog
            )
        )
    return sorted(rows)


def _scan_text(
    relative_path: str,
    text: str,
    mapping: Mapping[str, str],
    pattern: re.Pattern[str],
    file_sha256: Optional[str] = None,
) -> List[FObservedSite]:
    """comment/stringを含む全textから旧basename位置を収集する。"""

    observed: List[FObservedSite] = []
    source_sha256 = (
        hashlib.sha256(text.encode("utf-8")).hexdigest().upper()
        if file_sha256 is None
        else file_sha256
    )
    line_hash_counts: Dict[str, int] = collections.defaultdict(int)
    line_occurrences: List[int] = []
    lines = text.splitlines()
    line_offsets: List[int] = []
    next_offset = 0
    for raw_line in text.splitlines(keepends=True):
        line_offsets.append(next_offset)
        next_offset += len(raw_line)
    if Path(relative_path).suffix.lower() in CPP_TEXT_SUFFIXES:
        context_lines, preprocessor_states = _cpp_line_contexts(lines)
    else:
        context_lines = [["text"] * len(line) for line in lines]
        preprocessor_states = ["active"] * len(lines)
    for line_number, line in enumerate(lines, 1):
        line_sha256 = hashlib.sha256(line.encode("utf-8")).hexdigest().upper()
        line_occurrence = line_hash_counts[line_sha256]
        line_hash_counts[line_sha256] += 1
        line_occurrences.append(line_occurrence)
        token_counts: Dict[str, int] = collections.defaultdict(int)
        for match in pattern.finditer(line):
            legacy = match.group(1)
            token_occurrence = token_counts[legacy]
            token_counts[legacy] += 1
            lexical_context = context_lines[line_number - 1][match.start()]
            absolute_position = line_offsets[line_number - 1] + match.start()
            observed.append(
                FObservedSite(
                    site=FLegacySite(
                        path=relative_path,
                        file_sha256=source_sha256,
                        line_sha256=line_sha256,
                        line_occurrence=line_occurrence,
                        token_occurrence=token_occurrence,
                        legacy=legacy,
                        canonical=mapping[legacy],
                        lexical_context=lexical_context,
                        construct=_construct_for_occurrence(
                            relative_path,
                            line,
                            lexical_context,
                            legacy,
                            mapping[legacy],
                            text,
                            absolute_position,
                        ),
                        qualifier=_qualifier_before(line, match.start()),
                        preprocessor_state=preprocessor_states[line_number - 1],
                    ),
                    line_number=line_number,
                )
            )

    if Path(relative_path).suffix.lower() not in CPP_TEXT_SUFFIXES:
        return observed

    synthetic_counts: Dict[Tuple[int, str], int] = collections.defaultdict(int)

    def append_synthetic(position: int, legacy: str, construct: str) -> None:
        """translation phaseで復元される旧名を監査結果へ追加する。"""

        line_number = text.count("\n", 0, position) + 1
        if line_number > len(lines):
            return
        line = lines[line_number - 1]
        line_column = position - line_offsets[line_number - 1]
        if line_column >= len(context_lines[line_number - 1]):
            return
        context = context_lines[line_number - 1][line_column]
        line_sha256 = hashlib.sha256(line.encode("utf-8")).hexdigest().upper()
        token_key = (line_number, legacy)
        token_occurrence = synthetic_counts[token_key]
        synthetic_counts[token_key] += 1
        observed.append(
            FObservedSite(
                site=FLegacySite(
                    path=relative_path,
                    file_sha256=source_sha256,
                    line_sha256=line_sha256,
                    line_occurrence=line_occurrences[line_number - 1],
                    token_occurrence=token_occurrence,
                    legacy=legacy,
                    canonical=mapping[legacy],
                    lexical_context=context,
                    construct=construct,
                    qualifier="",
                    preprocessor_state=preprocessor_states[line_number - 1],
                ),
                line_number=line_number,
            )
        )

    def append_forbidden_token_paste(position: int) -> None:
        """対応名を直接復元しない##も許可不能なsynthetic siteにする。"""

        line_number = text.count("\n", 0, position) + 1
        if line_number > len(lines):
            return
        line = lines[line_number - 1]
        line_sha256 = hashlib.sha256(line.encode("utf-8")).hexdigest().upper()
        observed.append(
            FObservedSite(
                site=FLegacySite(
                    path=relative_path,
                    file_sha256=source_sha256,
                    line_sha256=line_sha256,
                    line_occurrence=line_occurrences[line_number - 1],
                    token_occurrence=0,
                    legacy="##",
                    canonical="forbidden-token-paste",
                    lexical_context="code",
                    construct="unsupported_token_paste_operator",
                    qualifier="",
                    preprocessor_state=preprocessor_states[line_number - 1],
                ),
                line_number=line_number,
            )
        )

    translated_tokens = _cpp_tokens(text)
    mapped_token_paste_found = False
    token_index = 0
    while token_index < len(translated_tokens):
        token = translated_tokens[token_index]
        if token.kind == "identifier" and token.transformed and token.value in mapping:
            append_synthetic(
                token.start, token.value, "unsupported_line_splice"
            )
        if token.kind == "identifier":
            pasted_name = token.value
            paste_end = token_index
            while (
                paste_end + 2 < len(translated_tokens)
                and translated_tokens[paste_end + 1].value == "##"
                and translated_tokens[paste_end + 2].kind == "identifier"
            ):
                pasted_name += translated_tokens[paste_end + 2].value
                paste_end += 2
            if paste_end > token_index and pasted_name in mapping:
                append_synthetic(
                    token.start, pasted_name, "unsupported_token_paste"
                )
                mapped_token_paste_found = True
                token_index = paste_end + 1
                continue
        if token.kind == "string":
            literal_value = token.value
            literal_end = token_index
            while (
                literal_end + 1 < len(translated_tokens)
                and translated_tokens[literal_end + 1].kind == "string"
            ):
                literal_end += 1
                literal_value += translated_tokens[literal_end].value
            original_literal = text[
                token.start : translated_tokens[literal_end].end
            ]
            if literal_value in mapping and literal_value not in original_literal:
                append_synthetic(
                    token.start,
                    literal_value,
                    "unsupported_encoded_or_concatenated_literal",
                )
            composed_value = literal_value
            composed_end = literal_end
            while (
                composed_end + 2 < len(translated_tokens)
                and translated_tokens[composed_end + 1].value == "+"
                and translated_tokens[composed_end + 2].kind == "string"
            ):
                composed_value += translated_tokens[composed_end + 2].value
                composed_end += 2
            composed_source = text[token.start : translated_tokens[composed_end].end]
            if (
                composed_end > literal_end
                and composed_value in mapping
                and composed_value not in composed_source
            ):
                append_synthetic(
                    token.start,
                    composed_value,
                    "unsupported_runtime_string_composition",
                )
            token_index = composed_end + 1
            continue
        token_index += 1
    if not mapped_token_paste_found:
        token_paste = next(
            (token for token in translated_tokens if token.value == "##"), None
        )
        if token_paste is not None:
            append_forbidden_token_paste(token_paste.start)
    return observed


def _scan_snapshot(
    snapshot: FRepositorySnapshot, mapping: Mapping[str, str]
) -> List[FObservedSite]:
    """固定済みsnapshot bytesからconsumer旧名位置を収集する。"""

    pattern = _legacy_pattern(mapping)
    observed: List[FObservedSite] = []
    for item in snapshot.files:
        if item.path in CONFIG_REPOSITORY_FILES or item.path.startswith("acs/src/"):
            continue
        raw = item.raw
        if raw.startswith(b"\xef\xbb\xbf"):
            raw = raw[3:]
        text = raw.decode("utf-8")
        observed.extend(
            _scan_text(item.path, text, mapping, pattern, item.bytes_sha256)
        )
    return sorted(observed, key=lambda item: item.site)


def _parse_nonnegative_integer(entry: Mapping[str, object], key: str) -> int:
    """allowlistの0以上整数fieldを検証する。"""

    value = entry.get(key)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ValueError("{} must be a non-negative integer".format(key))
    return value


def _parse_positive_integer(entry: Mapping[str, object], key: str) -> int:
    """allowlistの1以上整数fieldを検証する。"""

    value = _parse_nonnegative_integer(entry, key)
    if value == 0:
        raise ValueError("{} must be a positive integer".format(key))
    return value


def _parse_string(entry: Mapping[str, object], key: str) -> str:
    """allowlistの空でないstring fieldを検証する。"""

    value = entry.get(key)
    if not isinstance(value, str) or not value:
        raise ValueError("{} must be a non-empty string".format(key))
    return value


def _parse_any_string(entry: Mapping[str, object], key: str) -> str:
    """allowlistの空文字を許すstring fieldを検証する。"""

    value = entry.get(key)
    if not isinstance(value, str):
        raise ValueError("{} must be a string".format(key))
    return value


def _expected_allowlist_reason(path: str, construct: str) -> str:
    """構文と配布consumer pathから許可理由を一意に決める。"""

    if construct in {
        "runtime_name_comparison",
        "runtime_test_suite_identity",
        "runtime_type_lookup",
    }:
        return construct
    if path == "dist/verification/consumer_contract.cpp" and construct.startswith("compatibility_"):
        return "distribution_compatibility"
    if construct.startswith("compatibility_"):
        return "compatibility_fixture"
    return ""


def _load_allowlist_document(
    document: object,
    mapping: Mapping[str, str],
    registry_sha256: str,
) -> List[FAllowedSite]:
    """structural site allowlistをexact検証して読む。"""

    if not isinstance(document, dict):
        raise ValueError("allowlist root must be an object")
    if set(document) != {"schema_version", "registry_legacy_sha256", "entries"}:
        raise ValueError("allowlist root fields are not exact")
    if document.get("schema_version") != 1:
        raise ValueError("unsupported allowlist schema_version")
    if document.get("registry_legacy_sha256") != registry_sha256:
        raise ValueError("allowlist registry_legacy_sha256 is stale")
    raw_entries = document.get("entries")
    if not isinstance(raw_entries, list):
        raise ValueError("allowlist entries must be an array")
    allowed: List[FAllowedSite] = []
    seen = set()
    for raw_entry in raw_entries:
        if not isinstance(raw_entry, dict):
            raise ValueError("allowlist entry must be an object")
        expected_keys = {
            "path",
            "file_sha256",
            "line_hint",
            "line_sha256",
            "line_occurrence",
            "token_occurrence",
            "legacy",
            "canonical",
            "reason",
            "lexical_context",
            "construct",
            "qualifier",
            "preprocessor_state",
        }
        if set(raw_entry) != expected_keys:
            raise ValueError("allowlist entry fields are not exact")
        legacy = _parse_string(raw_entry, "legacy")
        canonical = _parse_string(raw_entry, "canonical")
        if mapping.get(legacy) != canonical:
            raise ValueError("allowlist alias mapping is stale: {}".format(legacy))
        line_sha256 = _parse_string(raw_entry, "line_sha256")
        if not re.fullmatch(r"[0-9A-F]{64}", line_sha256):
            raise ValueError("line_sha256 must be uppercase SHA-256")
        file_sha256 = _parse_string(raw_entry, "file_sha256")
        if not re.fullmatch(r"[0-9A-F]{64}", file_sha256):
            raise ValueError("file_sha256 must be uppercase SHA-256")
        reason = _parse_string(raw_entry, "reason")
        if reason not in ALLOWED_REASONS:
            raise ValueError("unsupported allowlist reason: {}".format(reason))
        path = _parse_string(raw_entry, "path")
        if "\\" in path or path.startswith("/") or ".." in Path(path).parts:
            raise ValueError("allowlist path must be repository-relative POSIX")
        lexical_context = _parse_string(raw_entry, "lexical_context")
        construct = _parse_string(raw_entry, "construct")
        qualifier = _parse_any_string(raw_entry, "qualifier")
        preprocessor_state = _parse_string(raw_entry, "preprocessor_state")
        if preprocessor_state not in {"active", "conditional"}:
            raise ValueError("allowlist occurrences must not be preprocessor-inactive")
        expected_reason = _expected_allowlist_reason(path, construct)
        if expected_reason != reason:
            raise ValueError("allowlist reason does not match source construct")
        if (
            preprocessor_state == "conditional"
            and construct != "runtime_test_suite_identity"
        ):
            raise ValueError(
                "conditional allowlist occurrences are limited to ACS_TEST suite identities"
            )
        site = FLegacySite(
            path=path,
            file_sha256=file_sha256,
            line_sha256=line_sha256,
            line_occurrence=_parse_nonnegative_integer(raw_entry, "line_occurrence"),
            token_occurrence=_parse_nonnegative_integer(raw_entry, "token_occurrence"),
            legacy=legacy,
            canonical=canonical,
            lexical_context=lexical_context,
            construct=construct,
            qualifier=qualifier,
            preprocessor_state=preprocessor_state,
        )
        if site in seen:
            raise ValueError("duplicate allowlist structural site: {}".format(site))
        seen.add(site)
        allowed.append(
            FAllowedSite(
                site=site,
                line_hint=_parse_positive_integer(raw_entry, "line_hint"),
                reason=reason,
            )
        )
    return sorted(allowed, key=lambda item: item.site)


def _load_allowlist(
    allowlist_path: Path,
    mapping: Mapping[str, str],
    registry_sha256: str,
) -> List[FAllowedSite]:
    """allowlist fileをexact schemaで読む。"""

    return _load_allowlist_document(
        _load_json(allowlist_path), mapping, registry_sha256
    )


def _allowlist_sha256(registry_sha256: str, allowed: Iterable[FAllowedSite]) -> str:
    """行番号に依存しないallowlist正本SHAを返す。"""

    rows = [
        [
            item.site.path,
            item.site.file_sha256,
            item.site.line_sha256,
            item.site.line_occurrence,
            item.site.token_occurrence,
            item.site.legacy,
            item.site.canonical,
            item.site.lexical_context,
            item.site.construct,
            item.site.qualifier,
            item.site.preprocessor_state,
            item.line_hint,
            item.reason,
        ]
        for item in sorted(allowed, key=lambda item: item.site)
    ]
    return _canonical_json_sha256([registry_sha256, rows])


def _reconcile(
    observed: Iterable[FObservedSite], allowed: Iterable[FAllowedSite]
) -> List[str]:
    """unknown、missing、duplicateを決定的な診断へ変換する。"""

    diagnostics: List[str] = []
    observed_by_site: Dict[FLegacySite, List[FObservedSite]] = collections.defaultdict(list)
    for item in observed:
        observed_by_site[item.site].append(item)
    allowed_by_site: Dict[FLegacySite, List[FAllowedSite]] = collections.defaultdict(list)
    for item in allowed:
        allowed_by_site[item.site].append(item)
    for site, items in observed_by_site.items():
        if len(items) > 1:
            diagnostics.append("duplicate-observed:{}:{}".format(site.path, site.legacy))
    for site, items in allowed_by_site.items():
        if len(items) > 1:
            diagnostics.append("duplicate-allowlist:{}:{}".format(site.path, site.legacy))
    for site, items in observed_by_site.items():
        if site not in allowed_by_site:
            diagnostics.append(
                "unknown:{}:{}:{}->{}".format(
                    site.path, items[0].line_number, site.legacy, site.canonical
                )
            )
        elif items[0].line_number != allowed_by_site[site][0].line_hint:
            diagnostics.append(
                "line-drift:{}:{}:{}:{}".format(
                    site.path,
                    allowed_by_site[site][0].line_hint,
                    items[0].line_number,
                    site.legacy,
                )
            )
    for site, items in allowed_by_site.items():
        if site not in observed_by_site:
            diagnostics.append(
                "missing:{}:{}:{}->{}".format(
                    site.path, items[0].line_hint, site.legacy, site.canonical
                )
            )
    return sorted(diagnostics)


def _self_test() -> int:
    """bytes正本、構文分類、snapshot raceを小型fixtureで固定する。"""

    mapping = {"FOldType": "acs::CNewType"}
    pattern = _legacy_pattern(mapping)
    baseline_source = (
        "static_assert(acs::IsSameV<acs::FOldType, acs::CNewType>);\n"
    )
    baseline_observed = _scan_text(
        "acs/tests/probe.cpp", baseline_source, mapping, pattern
    )
    baseline_allowed = [
        FAllowedSite(item.site, item.line_number, "compatibility_fixture")
        for item in baseline_observed
    ]
    registry_sha256 = _canonical_json_sha256([["FOldType", "acs::CNewType"]])

    def serialize_allowed(item: FAllowedSite) -> Dict[str, object]:
        """self-test用allowlist entryをproduction schemaで作る。"""

        return {
            "path": item.site.path,
            "file_sha256": item.site.file_sha256,
            "line_hint": item.line_hint,
            "line_sha256": item.site.line_sha256,
            "line_occurrence": item.site.line_occurrence,
            "token_occurrence": item.site.token_occurrence,
            "legacy": item.site.legacy,
            "canonical": item.site.canonical,
            "reason": item.reason,
            "lexical_context": item.site.lexical_context,
            "construct": item.site.construct,
            "qualifier": item.site.qualifier,
            "preprocessor_state": item.site.preprocessor_state,
        }

    def allowlist_document(entries: Sequence[Mapping[str, object]]) -> Dict[str, object]:
        """self-test用allowlist documentをexact root schemaで作る。"""

        return {
            "schema_version": 1,
            "registry_legacy_sha256": registry_sha256,
            "entries": list(entries),
        }

    expected_sha256 = _allowlist_sha256(registry_sha256, baseline_allowed)
    if len(baseline_observed) != 1 or _reconcile(
        baseline_observed, baseline_allowed
    ):
        print("self-test baseline reconcile failed", file=sys.stderr)
        return 1

    extra = _scan_text("acs/tests/extra.cpp", "FOldType Extra;\n", mapping, pattern)
    if not any(item.startswith("unknown:") for item in _reconcile(extra, [])):
        print("self-test unknown mutation was not rejected", file=sys.stderr)
        return 1
    if not any(item.startswith("missing:") for item in _reconcile([], baseline_allowed)):
        print("self-test missing mutation was not rejected", file=sys.stderr)
        return 1

    stale_source = "static_assert(acs::IsSameV<FOldType, acs::CNewType>, \"changed\");\n"
    stale_observed = _scan_text(
        "acs/tests/probe.cpp", stale_source, mapping, pattern
    )
    stale_diagnostics = _reconcile(stale_observed, baseline_allowed)
    if not (
        any(item.startswith("unknown:") for item in stale_diagnostics)
        and any(item.startswith("missing:") for item in stale_diagnostics)
    ):
        print("self-test file hash mutation was not rejected", file=sys.stderr)
        return 1

    shifted_observed = _scan_text(
        "acs/tests/probe.cpp", "\n" + baseline_source, mapping, pattern
    )
    shifted_allowed = [
        FAllowedSite(shifted_observed[0].site, 1, "compatibility_fixture")
    ]
    if not any(
        item.startswith("line-drift:")
        for item in _reconcile(shifted_observed, shifted_allowed)
    ):
        print("self-test line drift was not rejected", file=sys.stderr)
        return 1

    duplicate_diagnostics = _reconcile(
        baseline_observed, baseline_allowed + baseline_allowed
    )
    if not any(item.startswith("duplicate-allowlist:") for item in duplicate_diagnostics):
        print("self-test duplicate mutation was not rejected", file=sys.stderr)
        return 1
    coordinated_allowed = [
        FAllowedSite(item.site, item.line_number, "compatibility_fixture")
        for item in stale_observed
    ]
    if _reconcile(stale_observed, coordinated_allowed):
        print("self-test coordinated fixture setup failed", file=sys.stderr)
        return 1
    if _allowlist_sha256(registry_sha256, coordinated_allowed) == expected_sha256:
        print("self-test coordinated baseline mutation was not rejected", file=sys.stderr)
        return 1

    inactive = _scan_text(
        "acs/tests/probe.cpp", "#if 0\n" + baseline_source + "#endif\n", mapping, pattern
    )
    if len(inactive) != 1 or inactive[0].site.preprocessor_state != "inactive":
        print("self-test inactive context was not classified", file=sys.stderr)
        return 1
    literal_branches = _scan_text(
        "acs/tests/preprocessor.cpp",
        "#if(0)\nFOldType Inactive;\n#else\nFOldType Active;\n#endif\n",
        mapping,
        pattern,
    )
    if [item.site.preprocessor_state for item in literal_branches] != [
        "inactive",
        "active",
    ]:
        print("self-test literal preprocessor branches failed", file=sys.stderr)
        return 1
    conditional_suite = _scan_text(
        "acs/tests/preprocessor.cpp",
        "#if ACS_HAS_BACKEND\nACS_TEST(FOldType) {}\n"
        "#else\nACS_TEST(FOldType) {}\n#endif\n",
        mapping,
        pattern,
    )
    if len(conditional_suite) != 2 or any(
        item.site.preprocessor_state != "conditional"
        or item.site.construct != "runtime_test_suite_identity"
        for item in conditional_suite
    ):
        print("self-test conditional preprocessor branches failed", file=sys.stderr)
        return 1
    conditional_allowed = FAllowedSite(
        conditional_suite[0].site,
        conditional_suite[0].line_number,
        "runtime_test_suite_identity",
    )
    conditional_entry = serialize_allowed(conditional_allowed)
    if len(
        _load_allowlist_document(
            allowlist_document([conditional_entry]), mapping, registry_sha256
        )
    ) != 1:
        print("self-test conditional ACS_TEST allowlist failed", file=sys.stderr)
        return 1
    for state in ("inactive", "unknown"):
        state_entry = dict(conditional_entry)
        state_entry["preprocessor_state"] = state
        try:
            _load_allowlist_document(
                allowlist_document([state_entry]), mapping, registry_sha256
            )
        except ValueError:
            continue
        print("self-test invalid preprocessor state was not rejected", file=sys.stderr)
        return 1
    old_boolean_entry = dict(conditional_entry)
    old_boolean_entry.pop("preprocessor_state")
    old_boolean_entry["preprocessor_active"] = True
    try:
        _load_allowlist_document(
            allowlist_document([old_boolean_entry]), mapping, registry_sha256
        )
    except ValueError:
        pass
    else:
        print("self-test old boolean preprocessor field was not rejected", file=sys.stderr)
        return 1
    conditional_compatibility = serialize_allowed(baseline_allowed[0])
    conditional_compatibility["preprocessor_state"] = "conditional"
    try:
        _load_allowlist_document(
            allowlist_document([conditional_compatibility]), mapping, registry_sha256
        )
    except ValueError:
        pass
    else:
        print("self-test conditional compatibility was not rejected", file=sys.stderr)
        return 1
    comment_wrapped = _scan_text(
        "acs/tests/probe.cpp", "// " + baseline_source, mapping, pattern
    )
    if comment_wrapped[0].site.construct != "unsupported_comment":
        print("self-test comment wrapper was not rejected", file=sys.stderr)
        return 1
    rogue_namespace = _scan_text(
        "acs/tests/probe.cpp",
        "namespace rogue { using acs::FOldType; }\n",
        mapping,
        pattern,
    )
    if rogue_namespace[0].site.construct != "unsupported_code":
        print("self-test rogue namespace was not rejected", file=sys.stderr)
        return 1

    runtime_observed = _scan_text(
        "acs/tests/runtime.cpp",
        'auto* Type = registry.FindByName("FOldType");\n',
        mapping,
        pattern,
    )
    if len(runtime_observed) != 1 or runtime_observed[0].site.construct != "runtime_type_lookup":
        print("self-test runtime identity was not classified", file=sys.stderr)
        return 1
    codeified = _scan_text(
        "acs/tests/runtime.cpp", "FOldType Lookup;\n", mapping, pattern
    )
    if codeified[0].site.construct != "unsupported_code":
        print("self-test runtime codeification was not rejected", file=sys.stderr)
        return 1

    translated_cases = (
        ("F ## Ol ## dType Value;\n", "FOldType", "unsupported_token_paste"),
        (
            "#define JOIN3(A, B, C) A ## B ## C\n"
            "auto Value = JOIN3(F, Old, Type);\n",
            "##",
            "unsupported_token_paste_operator",
        ),
        ("FOl\\\ndT\\\nype Value;\n", "FOldType", "unsupported_line_splice"),
        (
            'auto* Value = Registry.FindByName("FOld"/*join*/"Type");\n',
            "FOldType",
            "unsupported_encoded_or_concatenated_literal",
        ),
        (
            'auto* Value = Registry.FindByName("\\x046OldType");\n',
            "FOldType",
            "unsupported_encoded_or_concatenated_literal",
        ),
        (
            'auto* Value = Registry.FindByName(u8"FOld" "Type");\n',
            "FOldType",
            "unsupported_encoded_or_concatenated_literal",
        ),
        (
            'auto* Value = Registry.FindByName(R"(FOld)" "Type");\n',
            "FOldType",
            "unsupported_encoded_or_concatenated_literal",
        ),
        (
            'auto* Value = Registry.FindByName("FOld" + "Type");\n',
            "FOldType",
            "unsupported_runtime_string_composition",
        ),
    )
    for source, expected_legacy, expected_construct in translated_cases:
        translated = _scan_text(
            "acs/tests/translation.cpp", source, mapping, pattern
        )
        if len(translated) != 1 or (
            translated[0].site.legacy,
            translated[0].site.construct,
        ) != (expected_legacy, expected_construct):
            print("self-test translation phase mutation failed", file=sys.stderr)
            return 1

    multiline_runtime = _scan_text(
        "acs/tests/runtime.cpp",
        'auto* Value = Registry.FindByName(\n    "FOldType"\n);\n',
        mapping,
        pattern,
    )
    if len(multiline_runtime) != 1 or (
        multiline_runtime[0].site.construct != "runtime_type_lookup"
    ):
        print("self-test multiline runtime argument failed", file=sys.stderr)
        return 1
    mixed_runtime = _scan_text(
        "acs/tests/runtime.cpp",
        'auto* Valid = Registry.FindByName("FOldType");\n'
        'auto* Rogue = Other("FOldType");\n',
        mapping,
        pattern,
    )
    if [item.site.construct for item in mixed_runtime] != [
        "runtime_type_lookup",
        "unsupported_string",
    ]:
        print("self-test runtime call binding failed", file=sys.stderr)
        return 1
    for invalid_literal in (
        'Registry.FindByName("FOldType"sv);\n',
        'Registry.FindByName(u8"FOldType");\n',
        'Registry.FindByName(L"FOldType");\n',
        'Registry.FindByName(R"(FOldType)");\n',
    ):
        invalid_runtime = _scan_text(
            "acs/tests/runtime.cpp", invalid_literal, mapping, pattern
        )
        if len(invalid_runtime) != 1 or invalid_runtime[0].site.construct in {
            "runtime_name_comparison",
            "runtime_type_lookup",
        }:
            print("self-test prefixed or suffixed literal was accepted", file=sys.stderr)
            return 1

    valid_compatibility_sources = (
        "static_assert(acs::IsSameV<acs::FOldType, acs::CNewType>);\n",
        "static_assert(acs::IsSameV<acs::CNewType, acs::FOldType>);\n",
        "static_assert(acs::IsSameV<FOldType, CNewType>);\n",
    )
    for source in valid_compatibility_sources:
        compatibility = _scan_text(
            "acs/tests/compatibility.cpp", source, mapping, pattern
        )
        if len(compatibility) != 1 or (
            compatibility[0].site.construct != "compatibility_type_assertion"
        ):
            print("self-test symmetric compatibility binding failed", file=sys.stderr)
            return 1
    invalid_compatibility_sources = (
        "static_assert(acs::IsSameV<acs::FOldType, acs::FOldType>);\n",
        "static_assert(acs::IsSameV<rogue::FOldType, acs::CNewType>);\n",
        "static_assert(acs::IsSameV<acs::FOldType, rogue::CNewType>);\n",
    )
    for source in invalid_compatibility_sources:
        compatibility = _scan_text(
            "acs/tests/compatibility.cpp", source, mapping, pattern
        )
        if not compatibility or any(
            item.site.construct == "compatibility_type_assertion"
            for item in compatibility
        ):
            print("self-test invalid compatibility binding was accepted", file=sys.stderr)
            return 1
    extra_compatibility = _scan_text(
        "acs/tests/compatibility.cpp",
        "static_assert(acs::IsSameV<acs::FOldType, acs::CNewType>"
        " && sizeof(rogue::FOldType), \"FOldType\");\n",
        mapping,
        pattern,
    )
    if [item.site.construct for item in extra_compatibility] != [
        "compatibility_type_assertion",
        "unsupported_code",
        "unsupported_string",
    ]:
        print("self-test extra compatibility token was accepted", file=sys.stderr)
        return 1

    identity_definitions = _macro_definitions_from_text(
        "acs/src/identity_fixture.h",
        "#define ACS_TEST(suite, name) #suite #name\n"
        "#define ACS_REGISTER_SYSTEM(Type, ...) #Type\n"
        "#define ACS_RTTI(Type, Parent) ClassTagOf<Type>()\n",
    )
    identity_catalog = _identity_macro_catalog(identity_definitions)
    fixed_point_definitions = _macro_definitions_from_text(
        "acs/src/fixed_point_fixture.h",
        "#define ACS_IDENTITY(Type) #Type\n"
        "#define ACS_WRAPPER(Type) ACS_IDENTITY(Type)\n"
        "#define ACS_WRAPPER2(Type) ACS_WRAPPER((Type))\n"
        "#define ACS_NONIDENTITY(Type) sizeof(Type)\n",
    )
    fixed_point_catalog = _identity_macro_catalog(fixed_point_definitions)
    if fixed_point_catalog != {
        "ACS_IDENTITY": frozenset({0}),
        "ACS_NONIDENTITY": frozenset(),
        "ACS_WRAPPER": frozenset({0}),
        "ACS_WRAPPER2": frozenset({0}),
    }:
        print("self-test identity fixed-point propagation failed", file=sys.stderr)
        return 1
    fixed_point_sha256 = _identity_macro_catalog_sha256(
        fixed_point_definitions, fixed_point_catalog
    )
    mutated_fixed_point_definitions = _macro_definitions_from_text(
        "acs/src/fixed_point_fixture.h",
        "#define ACS_IDENTITY(Type) sizeof(Type)\n"
        "#define ACS_WRAPPER(Type) ACS_IDENTITY(Type)\n"
        "#define ACS_WRAPPER2(Type) ACS_WRAPPER((Type))\n"
        "#define ACS_NONIDENTITY(Type) sizeof(Type)\n",
    )
    mutated_fixed_point_catalog = _identity_macro_catalog(
        mutated_fixed_point_definitions
    )
    if _identity_macro_catalog_sha256(
        mutated_fixed_point_definitions, mutated_fixed_point_catalog
    ) == fixed_point_sha256:
        print("self-test identity catalog mutation was not rejected", file=sys.stderr)
        return 1
    for invalid_definitions in (
        "#define ACS_WRAPPER(Type) ACS_UNKNOWN(Type)\n",
        "#define ACS_LEFT(Type) ACS_RIGHT(Type)\n"
        "#define ACS_RIGHT(Type) ACS_LEFT(Type)\n",
        "#define ACS_BROKEN(Type\n",
    ):
        try:
            parsed_invalid = _macro_definitions_from_text(
                "acs/src/invalid_macro_fixture.h", invalid_definitions
            )
            _identity_macro_catalog(parsed_invalid)
        except ValueError:
            continue
        print("self-test invalid macro catalog was not rejected", file=sys.stderr)
        return 1
    if _identity_macro_contract_diagnostics(EXPECTED_IDENTITY_MACRO_INDEXES):
        print("self-test identity macro contract baseline failed", file=sys.stderr)
        return 1
    mutated_contract = dict(EXPECTED_IDENTITY_MACRO_INDEXES)
    mutated_contract.pop("ACS_TEST")
    if not any(
        item == "identity-macro-missing:ACS_TEST"
        for item in _identity_macro_contract_diagnostics(mutated_contract)
    ):
        print("self-test identity macro contract mutation failed", file=sys.stderr)
        return 1
    identity_rows = _identity_macro_rows_for_text(
        "acs/tests/identity.cpp",
        "ACS_TEST(FOldType, Smoke) {}\n"
        "ACS_REGISTER_SYSTEM(FOldType)\n"
        "ACS_RTTI(FOldType, FOldType)\n",
        mapping,
        identity_catalog,
    )
    if identity_rows != [
        ["acs/tests/identity.cpp", 1, "ACS_TEST", "FOldType"],
        ["acs/tests/identity.cpp", 2, "ACS_REGISTER_SYSTEM", "FOldType"],
    ]:
        print("self-test identity macro argument table failed", file=sys.stderr)
        return 1
    invalid_identity_source = (
        "ACS_TEST(CNewType, FOldType) {}\n"
        "ACS_REGISTER_SYSTEM(CNewType, sizeof(FOldType))\n"
        "ACS_REGISTER_SYSTEM(Wrapper<FOldType>)\n"
    )
    invalid_identity_rows = _identity_macro_rows_for_text(
        "acs/tests/identity.cpp",
        invalid_identity_source,
        mapping,
        identity_catalog,
    )
    if invalid_identity_rows != [
        ["acs/tests/identity.cpp", 1, "ACS_TEST", "CNewType"],
        ["acs/tests/identity.cpp", 2, "ACS_REGISTER_SYSTEM", "CNewType"],
    ]:
        print("self-test canonical or nested identity argument failed", file=sys.stderr)
        return 1
    lookalike_identity = (
        '// ACS_TEST(FOldType, Comment)\n'
        'const char* Text = "ACS_TEST(FOldType, String)";\n'
        'const char* Raw = R"(ACS_TEST(FOldType, Raw))";\n'
    )
    if _identity_macro_rows_for_text(
        "acs/tests/identity.cpp",
        lookalike_identity,
        mapping,
        identity_catalog,
    ):
        print("self-test identity macro lookalike was accepted", file=sys.stderr)
        return 1
    if not all(
        item.site.construct == "unsupported_code"
        for item in _scan_text(
            "acs/tests/identity.cpp", invalid_identity_source, mapping, pattern
        )
    ):
        print("self-test wrong identity argument escaped source audit", file=sys.stderr)
        return 1

    identity_baseline_rows: List[List[object]] = [
        ["acs/tests/asset_registry_tests.cpp", 222, "ACS_TEST", "FAssetRegistry"],
        ["acs/tests/asset_registry_tests.cpp", 265, "ACS_TEST", "FAssetRegistry"],
        ["acs/tests/asset_registry_tests.cpp", 301, "ACS_TEST", "FAssetRegistry"],
        ["acs/tests/asset_registry_tests.cpp", 315, "ACS_TEST", "FAssetRegistry"],
        ["acs/tests/asset_registry_tests.cpp", 336, "ACS_TEST", "CAssetRegistry"],
        ["acs/tests/asset_registry_tests.cpp", 356, "ACS_TEST", "CAssetRegistry"],
        ["acs/tests/asset_registry_tests.cpp", 420, "ACS_TEST", "CAssetRegistry"],
        ["acs/tests/asset_registry_tests.cpp", 471, "ACS_TEST", "CAssetRegistry"],
        ["acs/tests/asset_registry_tests.cpp", 519, "ACS_TEST", "CAssetRegistry"],
        ["acs/tests/diligent_memory_adapter_tests.cpp", 41, "ACS_TEST", "FDiligentDevice"],
        ["acs/tests/diligent_memory_adapter_tests.cpp", 55, "ACS_TEST", "FDiligentMemoryAdapter"],
        ["acs/tests/diligent_memory_adapter_tests.cpp", 88, "ACS_TEST", "FDiligentDevice"],
        ["acs/tests/diligent_memory_adapter_tests.cpp", 130, "ACS_TEST", "FDiligentDevice"],
        ["acs/tests/diligent_memory_adapter_tests.cpp", 249, "ACS_TEST", "FDiligentDevice"],
        ["acs/tests/jobgraph_tests.cpp", 30, "ACS_TEST", "FJobGraph"],
        ["acs/tests/jobgraph_tests.cpp", 66, "ACS_TEST", "FJobGraph"],
        ["acs/tests/jobgraph_tests.cpp", 87, "ACS_TEST", "FJobGraph"],
    ]
    if _canonical_json_sha256(identity_baseline_rows) != EXPECTED_IDENTITY_MACRO_SHA256:
        print("self-test identity macro hard baseline failed", file=sys.stderr)
        return 1
    for mutated_rows in (
        identity_baseline_rows[:-1],
        identity_baseline_rows
        + [["acs/tests/new.cpp", 1, "ACS_TEST", "FOldType"]],
        identity_baseline_rows[:-1]
        + [["acs/tests/jobgraph_tests.cpp", 87, "ACS_TEST", "CJobGraph"]],
    ):
        if _canonical_json_sha256(mutated_rows) == EXPECTED_IDENTITY_MACRO_SHA256:
            print("self-test identity macro mutation was not rejected", file=sys.stderr)
            return 1

    try:
        json.loads('{"entries":[],"entries":[]}', object_pairs_hook=_reject_duplicate_keys)
    except ValueError:
        pass
    else:
        print("self-test duplicate JSON key was not rejected", file=sys.stderr)
        return 1
    duplicate_registry = {
        "schema_version": 2,
        "entries": [
            {"legacy": "acs::FOldType", "canonical": "acs::CNewType"},
            {"legacy": "acs::FOldType", "canonical": "acs::CNewType"},
        ],
    }
    try:
        _load_legacy_mapping_document(duplicate_registry)
    except ValueError:
        pass
    else:
        print("self-test duplicate legacy basename was not rejected", file=sys.stderr)
        return 1
    for wrong_schema in (None, 1, 3):
        registry_document = {"schema_version": wrong_schema, "entries": []}
        try:
            _load_legacy_mapping_document(registry_document)
        except ValueError:
            continue
        print("self-test registry schema mutation was not rejected", file=sys.stderr)
        return 1
    try:
        _load_allowlist_document(
            {
                "schema_version": 1,
                "registry_legacy_sha256": registry_sha256,
                "entries": [],
                "unexpected": True,
            },
            mapping,
            registry_sha256,
        )
    except ValueError:
        pass
    else:
        print("self-test allowlist root mutation was not rejected", file=sys.stderr)
        return 1

    boundary = _scan_text(
        "acs/tests/boundary.cpp",
        "XFOldType FOldType2 $FOldType FOldType; \"FOldType\" // FOldType\n",
        mapping,
        pattern,
    )
    if len(boundary) != 3:
        print("self-test text boundary scan failed", file=sys.stderr)
        return 1

    directory_identity = FPathIdentity(1, 1, stat.S_IFDIR, 0)
    file_identity = FPathIdentity(1, 2, stat.S_IFREG, 0)
    replacement_identity = FPathIdentity(1, 3, stat.S_IFREG, 0)
    directories = (FDirectorySnapshot(".", directory_identity),)
    initial_file = FTargetFileSnapshot(
        "acs/tests/probe.cpp", file_identity, 1, "A" * 64, b"a"
    )
    initial_snapshot = FRepositorySnapshot(directories, (initial_file,))
    replacement_snapshot = FRepositorySnapshot(
        directories,
        (
            FTargetFileSnapshot(
                "acs/tests/probe.cpp", replacement_identity, 1, "B" * 64, b"b"
            ),
        ),
    )
    added_snapshot = FRepositorySnapshot(
        directories,
        (
            initial_file,
            FTargetFileSnapshot(
                "acs/tests/new.cpp", replacement_identity, 1, "C" * 64, b"c"
            ),
        ),
    )
    if _snapshot_diagnostics(
        initial_snapshot, FRepositorySnapshot(directories, ())
    ) != ["snapshot-deleted-file:acs/tests/probe.cpp"]:
        print("self-test snapshot delete was not rejected", file=sys.stderr)
        return 1
    if not any(
        item.startswith("snapshot-replaced-or-changed-file:")
        for item in _snapshot_diagnostics(initial_snapshot, replacement_snapshot)
    ):
        print("self-test snapshot replacement was not rejected", file=sys.stderr)
        return 1
    if not any(
        item.startswith("snapshot-new-file:")
        for item in _snapshot_diagnostics(initial_snapshot, added_snapshot)
    ):
        print("self-test snapshot addition was not rejected", file=sys.stderr)
        return 1
    fake_reparse = type(
        "FReparseStat",
        (),
        {"st_mode": stat.S_IFREG, "st_file_attributes": FILE_ATTRIBUTE_REPARSE_POINT},
    )()
    if not _is_reparse(fake_reparse):
        print("self-test reparse identity was not rejected", file=sys.stderr)
        return 1

    def make_directory_stat(
        device: int, inode: int, attributes: int = FILE_ATTRIBUTE_DIRECTORY
    ) -> object:
        """directory pin fixture用のstat相当値を作る。"""

        return type(
            "FDirectoryStat",
            (),
            {
                "st_dev": device,
                "st_ino": inode,
                "st_mode": stat.S_IFDIR,
                "st_file_attributes": attributes,
            },
        )()

    valid_directory_stat = make_directory_stat(7, 11)
    _validate_directory_pin(
        Path("directory"),
        valid_directory_stat,
        valid_directory_stat,
        7,
        11,
        FILE_ATTRIBUTE_DIRECTORY,
        _path_identity(valid_directory_stat),
    )
    invalid_directory_pins = (
        (7, 12, FILE_ATTRIBUTE_DIRECTORY),
        (8, 11, FILE_ATTRIBUTE_DIRECTORY),
        (0, 11, FILE_ATTRIBUTE_DIRECTORY),
        (7, 0, FILE_ATTRIBUTE_DIRECTORY),
        (7, 11, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT),
    )
    for volume_serial, file_index, attributes in invalid_directory_pins:
        try:
            _validate_directory_pin(
                Path("directory"),
                valid_directory_stat,
                valid_directory_stat,
                volume_serial,
                file_index,
                attributes,
                _path_identity(valid_directory_stat),
            )
        except ValueError:
            continue
        print("self-test invalid directory pin was accepted", file=sys.stderr)
        return 1

    def make_stat(inode: int, attributes: int = 0) -> object:
        """capture race fixture用のstat相当値を作る。"""

        return type(
            "FCaptureStat",
            (),
            {
                "st_dev": 1,
                "st_ino": inode,
                "st_mode": stat.S_IFREG,
                "st_file_attributes": attributes,
                "st_size": 3,
                "st_mtime_ns": 7,
            },
        )()

    try:
        _validate_capture_states(
            Path("probe.cpp"),
            make_stat(1),
            make_stat(2),
            make_stat(2),
            make_stat(2),
            3,
        )
    except ValueError:
        pass
    else:
        print("self-test A-to-B handle race was not rejected", file=sys.stderr)
        return 1
    try:
        _validate_capture_states(
            Path("probe.cpp"),
            make_stat(1, FILE_ATTRIBUTE_REPARSE_POINT),
            make_stat(1),
            make_stat(1),
            make_stat(1),
            3,
        )
    except ValueError:
        pass
    else:
        print("self-test reparse-to-A race was not rejected", file=sys.stderr)
        return 1
    for invalid_byte_count in (2, 4):
        try:
            _validate_capture_states(
                Path("probe.cpp"),
                make_stat(1),
                make_stat(1),
                make_stat(1),
                make_stat(1),
                invalid_byte_count,
            )
        except ValueError:
            continue
        print("self-test fd byte count mismatch was not rejected", file=sys.stderr)
        return 1

    if os.name == "nt":
        invalid_oem_bytes = None
        for invalid_value in range(0x80, 0x100):
            candidate = bytes((invalid_value,))
            try:
                candidate.decode(WINDOWS_COMMAND_ENCODING, "strict")
            except UnicodeDecodeError:
                invalid_oem_bytes = candidate
                break
        if invalid_oem_bytes is not None:
            for invalid_stream in ("stdout", "stderr"):
                child_source = (
                    "import sys;sys.{}.buffer.write(bytes.fromhex('{}'))".format(
                        invalid_stream, invalid_oem_bytes.hex()
                    )
                )
                try:
                    _run_windows_command([sys.executable, "-c", child_source])
                except UnicodeDecodeError:
                    continue
                print(
                    "self-test invalid OEM {} was accepted".format(
                        invalid_stream
                    ),
                    file=sys.stderr,
                )
                return 1
        subprocess_warmup = _run_windows_command(
            ["cmd.exe", "/d", "/c", "exit", "0"]
        )
        if subprocess_warmup.returncode != 0:
            print("self-test subprocess warmup failed", file=sys.stderr)
            return 1
    process_handles_before = _windows_process_handle_count()
    with tempfile.TemporaryDirectory(
        prefix="acs-prefix-consumer-audit-"
    ) as temp_name, tempfile.TemporaryDirectory(
        prefix="acs-prefix-consumer-external-"
    ) as external_name:
        repository_root = Path(temp_name)
        external_root = Path(external_name)
        acs_root = repository_root / "acs"
        (acs_root / "src").mkdir(parents=True)
        (acs_root / "tests").mkdir(parents=True)
        for relative_path in TARGET_REPOSITORY_FILES:
            target = repository_root / relative_path
            target.parent.mkdir(parents=True, exist_ok=True)
            _write_utf8_lf(target, "")

        if os.name == "nt":
            pinned_path = repository_root / "pinned-directory"
            renamed_path = repository_root / "renamed-directory"
            pinned_path.mkdir()
            with _held_directory(pinned_path):
                try:
                    os.rename(pinned_path, renamed_path)
                except OSError as error:
                    if int(getattr(error, "winerror", 0)) not in {5, 32}:
                        raise
                else:
                    print("self-test pinned directory rename was allowed", file=sys.stderr)
                    return 1
            os.rename(pinned_path, renamed_path)
            os.rename(renamed_path, pinned_path)
            try:
                with _held_directory(pinned_path):
                    raise RuntimeError("directory pin cleanup fixture")
            except RuntimeError:
                pass
            os.rename(pinned_path, renamed_path)
            os.rename(renamed_path, pinned_path)
            pinned_path.rmdir()

        fake_pin = FDirectoryPin(
            identity=FPathIdentity(3, 5, stat.S_IFDIR, 0),
            physical_path=str(repository_root),
            descriptor=47,
        )
        with mock.patch.object(os, "name", "posix"), mock.patch.object(
            os, "scandir"
        ) as mocked_scandir:
            _directory_scandir(repository_root, fake_pin)
            if mocked_scandir.call_args != mock.call(47):
                print("self-test POSIX scandir did not use the held fd", file=sys.stderr)
                return 1

        case_directories: Dict[str, FDirectorySnapshot] = {}
        case_held_paths: Dict[str, FDirectoryPin] = {}

        @contextmanager
        def fake_held_directory(*_args: object, **_kwargs: object) -> Iterator[FDirectoryPin]:
            """case-sensitive path key fixture用のpinを返す。"""

            yield fake_pin

        with ExitStack() as case_stack, mock.patch(
            __name__ + "._held_directory", side_effect=fake_held_directory
        ) as mocked_held_directory:
            _pin_directory(
                repository_root / "Foo",
                repository_root,
                case_directories,
                case_stack,
                case_held_paths,
            )
            _pin_directory(
                repository_root / "foo",
                repository_root,
                case_directories,
                case_stack,
                case_held_paths,
            )
            if mocked_held_directory.call_count != 2 or len(case_held_paths) != 2:
                print("self-test case-distinct directories were deduplicated", file=sys.stderr)
                return 1

        short_path = acs_root / "tests/capture-short.bin"
        long_path = acs_root / "tests/capture-long.bin"
        short_path.write_bytes(b"abc")
        long_payload = (b"0123456789ABCDEF" * 131073) + b"tail"
        long_path.write_bytes(long_payload)
        with mock.patch.object(
            Path,
            "read_bytes",
            side_effect=AssertionError("Path.read_bytes must not be used"),
        ):
            short_capture = _capture_file(short_path, repository_root)
            long_capture = _capture_file(long_path, repository_root)
        if short_capture.raw != b"abc" or long_capture.raw != long_payload:
            print("self-test fd short/long read failed", file=sys.stderr)
            return 1
        short_path.unlink()
        long_path.unlink()

        if os.name == "nt":
            # 実ディレクトリの置換で物理識別子の再確認と読み取り前拒否を検証する。
            external_probe_payload = b"ACS_PREFIX_EXTERNAL_PHYSICAL_SENTINEL_7D0E8C82\n"
            external_probe_path = external_root / "replacement/external-probe.cpp"
            external_probe_path.parent.mkdir(parents=True)
            external_probe_path.write_bytes(external_probe_payload)
            external_sentinel = external_root / "sentinel.txt"
            _write_utf8_lf(external_sentinel, "external sentinel")
            external_sentinel_sha256 = hashlib.sha256(external_sentinel.read_bytes()).hexdigest()
            swap_path = acs_root / "tests/swap-entry"
            saved_path = acs_root / "tests/swap-entry-original"
            swap_path.mkdir()
            mutation_done = False
            replacement_scans = 0
            original_checked_stat = _checked_stat
            original_directory_scandir = _directory_scandir

            def swap_before_check(checked_path: Path, expected_directory: bool) -> os.stat_result:
                """検査直前に通常ディレクトリを別通常ディレクトリへ置き換える。"""

                nonlocal mutation_done
                if checked_path.name == "swap-entry" and not mutation_done:
                    os.rename(swap_path, saved_path)
                    swap_path.mkdir()
                    (swap_path / "external-probe.cpp").write_bytes(external_probe_payload)
                    mutation_done = True
                return original_checked_stat(checked_path, expected_directory)

            def observe_directory_scandir(scanned_path: Path, directory_pin: FDirectoryPin) -> os.ScandirIterator[str]:
                """置換後ディレクトリを読み取らないことを数える。"""

                nonlocal replacement_scans
                if scanned_path.name == "swap-entry":
                    replacement_scans += 1
                return original_directory_scandir(scanned_path, directory_pin)

            try:
                with mock.patch(__name__ + "._checked_stat", side_effect=swap_before_check), mock.patch(
                    __name__ + "._directory_scandir", side_effect=observe_directory_scandir
                ):
                    _capture_repository_snapshot(acs_root)
            except ValueError:
                pass
            else:
                print("self-test directory entry replacement was accepted", file=sys.stderr)
                return 1
            finally:
                if mutation_done:
                    (swap_path / "external-probe.cpp").unlink()
                    swap_path.rmdir()
                    os.rename(saved_path, swap_path)
            if not mutation_done or replacement_scans != 0:
                print("self-test replacement was read before rejection", file=sys.stderr)
                return 1
            if hashlib.sha256(external_sentinel.read_bytes()).hexdigest() != external_sentinel_sha256:
                print("self-test external sentinel changed", file=sys.stderr)
                return 1
            implementation_text = Path(__file__).read_text(encoding="utf-8").casefold()
            forbidden_commands = ("sub" + "st.exe", "new" + "-psdrive", "net" + " use", "mk" + "link")
            if any(command in implementation_text for command in forbidden_commands):
                print("self-test found a forbidden path command", file=sys.stderr)
                return 1
            swap_path.rmdir()

        _write_utf8_lf(acs_root / "tests/probe.cpp", baseline_source)
        registry_document = {
            "schema_version": 2,
            "entries": [
                {"legacy": "acs::FOldType", "canonical": "acs::CNewType"}
            ],
        }
        registry_path = acs_root / REGISTRY_RELATIVE_PATH
        _write_utf8_lf(
            registry_path,
            json.dumps(registry_document, separators=(",", ":")) + "\n",
        )
        fixture_mapping, fixture_registry_sha256 = _load_legacy_mapping(registry_path)
        allowlist_path = acs_root / ALLOWLIST_RELATIVE_PATH
        _write_utf8_lf(
            allowlist_path,
            json.dumps(
                {
                    "schema_version": 1,
                    "registry_legacy_sha256": fixture_registry_sha256,
                    "entries": [],
                },
                separators=(",", ":"),
            )
            + "\n",
        )
        draft_snapshot = _capture_repository_snapshot(acs_root)
        fixture_observed = _scan_snapshot(draft_snapshot, fixture_mapping)
        fixture_allowed = [
            FAllowedSite(item.site, item.line_number, "compatibility_fixture")
            for item in fixture_observed
        ]
        fixture_entries = [
            {
                "path": item.site.path,
                "file_sha256": item.site.file_sha256,
                "line_hint": item.line_hint,
                "line_sha256": item.site.line_sha256,
                "line_occurrence": item.site.line_occurrence,
                "token_occurrence": item.site.token_occurrence,
                "legacy": item.site.legacy,
                "canonical": item.site.canonical,
                "reason": item.reason,
                "lexical_context": item.site.lexical_context,
                "construct": item.site.construct,
                "qualifier": item.site.qualifier,
                "preprocessor_state": item.site.preprocessor_state,
            }
            for item in fixture_allowed
        ]
        _write_utf8_lf(
            allowlist_path,
            json.dumps(
                {
                    "schema_version": 1,
                    "registry_legacy_sha256": fixture_registry_sha256,
                    "entries": fixture_entries,
                },
                separators=(",", ":"),
            )
            + "\n",
        )
        fixture_initial = _capture_repository_snapshot(acs_root)
        registry_item = _snapshot_file(
            fixture_initial, "acs/{}".format(REGISTRY_RELATIVE_PATH.as_posix())
        )
        allowlist_item = _snapshot_file(
            fixture_initial, "acs/{}".format(ALLOWLIST_RELATIVE_PATH.as_posix())
        )
        loaded_mapping, loaded_registry_sha256 = _load_legacy_mapping_document(
            _load_json_bytes(registry_item.raw, registry_item.path)
        )
        loaded_allowed = _load_allowlist_document(
            _load_json_bytes(allowlist_item.raw, allowlist_item.path),
            loaded_mapping,
            loaded_registry_sha256,
        )
        if _reconcile(_scan_snapshot(fixture_initial, loaded_mapping), loaded_allowed):
            print("self-test full snapshot reconcile failed", file=sys.stderr)
            return 1
        fixture_final = _capture_repository_snapshot(acs_root)
        if _snapshot_diagnostics(fixture_initial, fixture_final):
            print("self-test stable snapshot bracket failed", file=sys.stderr)
            return 1
        auditor_path = repository_root / "acs/scripts/audit_cpp_prefix_consumers.py"
        _write_utf8_lf(auditor_path, "# self-test mutation\n")
        auditor_mutated = _capture_repository_snapshot(acs_root)
        if not any(
            item
            == "snapshot-replaced-or-changed-file:acs/scripts/audit_cpp_prefix_consumers.py"
            for item in _snapshot_diagnostics(fixture_initial, auditor_mutated)
        ):
            print("self-test auditor race was not rejected", file=sys.stderr)
            return 1
        _write_utf8_lf(auditor_path, "")
        _write_utf8_lf(
            registry_path,
            json.dumps(registry_document, separators=(",", ":")) + "\n\n",
        )
        registry_mutated = _capture_repository_snapshot(acs_root)
        if not any(
            item
            == "snapshot-replaced-or-changed-file:acs/scripts/data/cpp_type_role_migrations.json"
            for item in _snapshot_diagnostics(fixture_initial, registry_mutated)
        ):
            print("self-test registry race was not rejected", file=sys.stderr)
            return 1
        _write_utf8_lf(
            registry_path,
            json.dumps(registry_document, separators=(",", ":")) + "\n",
        )
        _write_utf8_lf(
            allowlist_path,
            allowlist_path.read_text(encoding="utf-8") + "\n",
        )
        allowlist_mutated = _capture_repository_snapshot(acs_root)
        if not any(
            item
            == "snapshot-replaced-or-changed-file:acs/scripts/data/cpp_prefix_consumer_legacy_allowlist.json"
            for item in _snapshot_diagnostics(fixture_initial, allowlist_mutated)
        ):
            print("self-test allowlist race was not rejected", file=sys.stderr)
            return 1
        _write_utf8_lf(acs_root / "tests/probe.cpp", stale_source)
        fixture_mutated = _capture_repository_snapshot(acs_root)
        if not _snapshot_diagnostics(fixture_initial, fixture_mutated):
            print("self-test snapshot race was not rejected", file=sys.stderr)
            return 1

    process_handles_after = _windows_process_handle_count()
    if process_handles_after != process_handles_before:
        print(
            "self-test directory handle leak was detected: {} -> {}".format(
                process_handles_before, process_handles_after
            ),
            file=sys.stderr,
        )
        return 1

    print("consumer prefix self-test PASS cases=54 observed=1")
    return 0


def _parse_arguments(argv: Optional[Sequence[str]]) -> argparse.Namespace:
    """command lineを解析する。"""

    parser = argparse.ArgumentParser(
        description="testsと配布consumerの旧公開型名をexact監査します"
    )
    parser.add_argument("--root", type=Path, help="ACS directory（例: acs）")
    parser.add_argument("--self-test", action="store_true", help="内蔵mutation fixtureを実行")
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    """監査または内蔵self-testを実行する。"""

    arguments = _parse_arguments(argv)
    if arguments.self_test:
        return _self_test()
    if arguments.root is None:
        print("--root is required unless --self-test is used", file=sys.stderr)
        return 2
    acs_root = arguments.root.absolute()
    try:
        initial_snapshot = _capture_repository_snapshot(acs_root)
        registry_item = _snapshot_file(
            initial_snapshot,
            "acs/{}".format(REGISTRY_RELATIVE_PATH.as_posix()),
        )
        allowlist_item = _snapshot_file(
            initial_snapshot,
            "acs/{}".format(ALLOWLIST_RELATIVE_PATH.as_posix()),
        )
        mapping, registry_sha256 = _load_legacy_mapping_document(
            _load_json_bytes(registry_item.raw, registry_item.path)
        )
        allowed = _load_allowlist_document(
            _load_json_bytes(allowlist_item.raw, allowlist_item.path),
            mapping,
            registry_sha256,
        )
        actual_allowlist_sha256 = _allowlist_sha256(registry_sha256, allowed)
        if actual_allowlist_sha256 != EXPECTED_ALLOWLIST_SHA256:
            print(
                "consumer prefix allowlist baseline mismatch: expected={} actual={}".format(
                    EXPECTED_ALLOWLIST_SHA256, actual_allowlist_sha256
                ),
                file=sys.stderr,
            )
            return 1
        macro_definitions = _macro_definitions(initial_snapshot)
        identity_macro_catalog = _identity_macro_catalog(macro_definitions)
        identity_macro_catalog_sha256 = _identity_macro_catalog_sha256(
            macro_definitions, identity_macro_catalog
        )
        identity_macro_sha256 = _canonical_json_sha256(
            _identity_macro_rows(
                initial_snapshot, mapping, identity_macro_catalog
            )
        )
        observed = _scan_snapshot(initial_snapshot, mapping)
        final_snapshot = _capture_repository_snapshot(acs_root)
    except (OSError, UnicodeDecodeError, ValueError, json.JSONDecodeError) as error:
        print("consumer prefix audit input error: {}".format(error), file=sys.stderr)
        return 2

    diagnostics = _snapshot_diagnostics(initial_snapshot, final_snapshot)
    diagnostics.extend(
        _identity_macro_contract_diagnostics(identity_macro_catalog)
    )
    if identity_macro_catalog_sha256 != EXPECTED_IDENTITY_MACRO_CATALOG_SHA256:
        diagnostics.append(
            "identity-macro-catalog-baseline:expected={}:actual={}".format(
                EXPECTED_IDENTITY_MACRO_CATALOG_SHA256,
                identity_macro_catalog_sha256,
            )
        )
    if identity_macro_sha256 != EXPECTED_IDENTITY_MACRO_SHA256:
        diagnostics.append(
            "identity-macro-baseline:expected={}:actual={}".format(
                EXPECTED_IDENTITY_MACRO_SHA256, identity_macro_sha256
            )
        )
    diagnostics.extend(_reconcile(observed, allowed))
    diagnostics = sorted(diagnostics)
    for diagnostic in diagnostics:
        print(diagnostic, file=sys.stderr)
    if diagnostics:
        print(
            "consumer prefix audit FAIL files={} observed={} allowlist={} violations={}".format(
                len(initial_snapshot.files),
                len(observed),
                len(allowed),
                len(diagnostics),
            ),
            file=sys.stderr,
        )
        return 1
    reason_counts = collections.Counter(item.reason for item in allowed)
    print(
        (
            "consumer prefix audit PASS files={} observed={} compatibility={} "
            "distribution_compatibility={} runtime_name_comparison={} "
            "runtime_type_lookup={} runtime_test_suite_identity={} violations=0"
        ).format(
            len(initial_snapshot.files),
            len(observed),
            reason_counts["compatibility_fixture"],
            reason_counts["distribution_compatibility"],
            reason_counts["runtime_name_comparison"],
            reason_counts["runtime_type_lookup"],
            reason_counts["runtime_test_suite_identity"],
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
