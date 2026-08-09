#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate tracked ACS artifacts and their provenance ledger."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import subprocess
import sys
import tempfile
from typing import Any, Sequence
import zlib


LEDGER_PATH = "artifact/ledger.json"
AUDIT_COMMAND = "python acs/scripts/audit_artifact_ledger.py --root ."
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
COMMIT_PATTERN = re.compile(r"[0-9a-f]{40}")
OBJECT_PATTERN = re.compile(r"[0-9a-f]{40}")
SHA256_PATTERN = re.compile(r"[0-9A-Fa-f]{64}")
WINDOWS_REPARSE_POINT = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
REFERENCE_ENCODINGS = ("utf-8", "utf-16-le", "utf-16-be", "utf-32-le", "utf-32-be")
GENERATED_FROM_KEYS = frozenset(
    {
        "status",
        "historical_generator_path",
        "inferred_parent_commit",
        "historical_context_commit",
        "historical_backend",
        "exact_source_commit",
        "exact_capture_command",
        "evidence",
    }
)
BUILD_REPRODUCIBILITY_KEYS = frozenset(
    {"status", "exact_byte_reproduction", "reason", "current_semantic_candidate"}
)
SEMANTIC_CANDIDATE_UNVERIFIED_KEYS = frozenset({"status", "commands"})
SEMANTIC_CANDIDATE_UNAVAILABLE_KEYS = frozenset({"status", "reason", "commands"})
REQUIRED_VERIFICATION_COMMANDS = (
    "cmake --build acs/Intermediate/vs --config Debug --parallel 1",
    "ctest --test-dir acs/Intermediate/vs -C Debug --output-on-failure",
    "cmake --build acs/Intermediate/vs --config Release --parallel 1",
    "ctest --test-dir acs/Intermediate/vs -C Release --output-on-failure",
)
REQUIRED_VERIFICATION_GATES = frozenset(
    {
        "byte-identity",
        "rename-and-residue",
        "reference-scan",
        "debug-release-build-test",
    }
)


class FAuditError(RuntimeError):
    """A repository or command failure that makes the audit indeterminate."""


def configure_utf8_console() -> None:
    """Keep diagnostics deterministic on Windows code pages."""

    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="strict")


def run_git(
    root: Path,
    arguments: Sequence[str],
    *,
    accepted_codes: frozenset[int] = frozenset({0}),
    input_bytes: bytes | None = None,
    environment_overrides: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[bytes]:
    """Run Git without a shell and reject every unexpected exit status."""

    environment = os.environ.copy()
    if environment_overrides is not None:
        environment.update(environment_overrides)
    result = subprocess.run(
        ["git", *arguments],
        cwd=root,
        check=False,
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
    )
    if result.returncode not in accepted_codes:
        stderr = result.stderr.decode("utf-8", errors="replace").strip()
        command = "git " + " ".join(arguments)
        raise FAuditError(
            f"{command} failed with exit {result.returncode}: {stderr}"
        )
    return result


def create_candidate_tree(root: Path) -> str:
    """Build a temporary-index tree from tracked changes and new artifact files."""

    changed_paths = run_git(
        root,
        ["diff", "--name-only", "--no-renames", "-z", "HEAD", "--"],
    ).stdout
    untracked_artifact_paths = run_git(
        root,
        ["ls-files", "--others", "--exclude-standard", "-z", "--", "artifact"],
    ).stdout
    candidate_paths = {
        path
        for output in (changed_paths, untracked_artifact_paths)
        for path in output.split(b"\0")
        if path
    }
    encoded_candidate_paths = (
        b"\0".join(sorted(candidate_paths)) + b"\0" if candidate_paths else b""
    )
    with tempfile.TemporaryDirectory(prefix="acs-artifact-candidate-index-") as temp:
        index_path = Path(temp) / "index"
        environment = {"GIT_INDEX_FILE": str(index_path)}
        run_git(root, ["read-tree", "HEAD"], environment_overrides=environment)
        if encoded_candidate_paths:
            run_git(
                root,
                [
                    "add",
                    "-A",
                    "--force",
                    "--pathspec-from-file=-",
                    "--pathspec-file-nul",
                ],
                input_bytes=encoded_candidate_paths,
                environment_overrides=environment,
            )
        tree = run_git(
            root,
            ["write-tree"],
            environment_overrides=environment,
        ).stdout.decode("ascii").strip()
    if OBJECT_PATTERN.fullmatch(tree) is None:
        raise FAuditError("Git returned an invalid candidate tree id")
    return tree


def require_mapping(value: Any, location: str) -> dict[str, Any]:
    """Return one JSON object or fail with its ledger location."""

    if not isinstance(value, dict):
        raise FAuditError(f"{location} must be an object")
    return value


def require_string(mapping: dict[str, Any], key: str, location: str) -> str:
    """Return one non-empty string field."""

    value = mapping.get(key)
    if not isinstance(value, str) or not value:
        raise FAuditError(f"{location}.{key} must be a non-empty string")
    return value


def require_integer(mapping: dict[str, Any], key: str, location: str) -> int:
    """Return one non-negative integer field while rejecting booleans."""

    value = mapping.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise FAuditError(f"{location}.{key} must be a non-negative integer")
    return value


def require_boolean(mapping: dict[str, Any], key: str, location: str) -> bool:
    """Return one Boolean field without accepting integer substitutes."""

    value = mapping.get(key)
    if not isinstance(value, bool):
        raise FAuditError(f"{location}.{key} must be a Boolean")
    return value


def require_nullable_string(
    mapping: dict[str, Any], key: str, location: str
) -> str | None:
    """Return null or one non-empty string field."""

    value = mapping.get(key)
    if value is not None and (not isinstance(value, str) or not value):
        raise FAuditError(f"{location}.{key} must be null or a non-empty string")
    return value


def require_string_list(mapping: dict[str, Any], key: str, location: str) -> list[str]:
    """Return one non-empty list containing only non-empty strings."""

    value = mapping.get(key)
    if (
        not isinstance(value, list)
        or not value
        or any(not isinstance(item, str) or not item for item in value)
    ):
        raise FAuditError(
            f"{location}.{key} must be a non-empty list of non-empty strings"
        )
    return value


def require_exact_keys(
    mapping: dict[str, Any], expected: frozenset[str], location: str
) -> None:
    """Reject missing or unknown fields in a schema-versioned object."""

    actual = frozenset(mapping)
    if actual != expected:
        missing = sorted(expected - actual)
        unknown = sorted(actual - expected)
        raise FAuditError(
            f"{location} keys differ; missing={missing}, unknown={unknown}"
        )


def validate_relative_path(value: str, location: str) -> PurePosixPath:
    """Accept only normalized repository-relative POSIX paths."""

    if "\\" in value or value.startswith(("/", "//")):
        raise FAuditError(f"{location} must be a relative POSIX path: {value}")
    if re.match(r"^[A-Za-z]:", value):
        raise FAuditError(f"{location} must not contain a drive: {value}")
    path = PurePosixPath(value)
    if (
        not path.parts
        or any(part in ("", ".", "..") for part in path.parts)
        or path.as_posix() != value
    ):
        raise FAuditError(f"{location} is not normalized: {value}")
    return path


def filesystem_path(root: Path, path: PurePosixPath) -> Path:
    """Convert a validated ledger path without accepting host separators."""

    return root.joinpath(*path.parts)


def path_exists_without_following(path: Path) -> bool:
    """Return true for files, directories, and dangling links/reparse points."""

    try:
        os.lstat(path)
        return True
    except FileNotFoundError:
        return False


def reject_reparse_chain(root: Path, path: Path, location: str) -> None:
    """Reject links and Windows reparse points from the root through a file."""

    try:
        relative = path.relative_to(root)
    except ValueError as error:
        raise FAuditError(f"{location} escaped repository root") from error

    current = root
    for part in relative.parts:
        current = current / part
        try:
            metadata = os.lstat(current)
        except FileNotFoundError:
            raise FAuditError(f"{location} does not exist: {current}")
        attributes = getattr(metadata, "st_file_attributes", 0)
        if stat.S_ISLNK(metadata.st_mode) or attributes & WINDOWS_REPARSE_POINT:
            raise FAuditError(f"{location} crosses a reparse point: {current}")


def parse_png_dimensions(data: bytes, location: str) -> tuple[int, int]:
    """Read PNG dimensions from the mandatory first IHDR chunk."""

    if len(data) < 33 or data[:8] != PNG_SIGNATURE:
        raise FAuditError(f"{location} is not a PNG")
    chunk_length = struct.unpack(">I", data[8:12])[0]
    if data[12:16] != b"IHDR" or chunk_length != 13:
        raise FAuditError(f"{location} has an invalid PNG IHDR")
    width, height = struct.unpack(">II", data[16:24])
    if width == 0 or height == 0:
        raise FAuditError(f"{location} has zero PNG dimensions")
    return width, height


def tree_entry(root: Path, revision: str, path: str) -> tuple[str, str, str] | None:
    """Return mode, type, and object id for one exact path in a Git tree."""

    result = run_git(root, ["ls-tree", "-z", revision, "--", path])
    if not result.stdout:
        return None
    records = [record for record in result.stdout.split(b"\0") if record]
    if len(records) != 1:
        raise FAuditError(f"{revision}:{path} resolved to {len(records)} entries")
    try:
        metadata, recorded_path = records[0].split(b"\t", 1)
        mode, object_type, object_id = metadata.decode("ascii").split(" ")
        decoded_path = recorded_path.decode("utf-8")
    except (UnicodeDecodeError, ValueError) as error:
        raise FAuditError(f"invalid ls-tree output for {revision}:{path}") from error
    if decoded_path != path:
        raise FAuditError(
            f"ls-tree path mismatch for {revision}:{path}: {decoded_path}"
        )
    return mode, object_type, object_id


def tree_entries_recursive(
    root: Path, revision: str, pathspec: str | None = None
) -> tuple[tuple[str, str, str, str], ...]:
    """Return raw recursive tree records as path, mode, type, and object id."""

    arguments = ["ls-tree", "-r", "-z", "--full-tree", revision]
    if pathspec is not None:
        arguments.extend(["--", pathspec])
    result = run_git(root, arguments)
    entries: list[tuple[str, str, str, str]] = []
    for record in (item for item in result.stdout.split(b"\0") if item):
        try:
            metadata, recorded_path = record.split(b"\t", 1)
            mode, object_type, object_id = metadata.decode("ascii").split(" ")
            path = recorded_path.decode("utf-8")
        except (UnicodeDecodeError, ValueError) as error:
            raise FAuditError(f"invalid recursive ls-tree output for {revision}") from error
        entries.append((path, mode, object_type, object_id))
    return tuple(entries)


def batch_blob_bytes(root: Path, object_ids: frozenset[str]) -> dict[str, bytes]:
    """Read a set of blobs with one Git process and strict framing checks."""

    if not object_ids:
        return {}
    ordered_ids = sorted(object_ids)
    query = b"".join(object_id.encode("ascii") + b"\n" for object_id in ordered_ids)
    result = subprocess.run(
        ["git", "cat-file", "--batch"],
        cwd=root,
        check=False,
        input=query,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        stderr = result.stderr.decode("utf-8", errors="replace").strip()
        raise FAuditError(f"git cat-file --batch failed: {stderr}")

    blobs: dict[str, bytes] = {}
    offset = 0
    for requested_id in ordered_ids:
        newline = result.stdout.find(b"\n", offset)
        if newline < 0:
            raise FAuditError("git cat-file --batch returned a truncated header")
        try:
            object_id, object_type, size_text = result.stdout[offset:newline].decode(
                "ascii"
            ).split(" ")
            size = int(size_text)
        except (UnicodeDecodeError, ValueError) as error:
            raise FAuditError("git cat-file --batch returned an invalid header") from error
        if object_id != requested_id or object_type != "blob" or size < 0:
            raise FAuditError(
                f"git cat-file --batch returned {object_id} {object_type} "
                f"for requested blob {requested_id}"
            )
        data_start = newline + 1
        data_end = data_start + size
        if data_end >= len(result.stdout) or result.stdout[data_end : data_end + 1] != b"\n":
            raise FAuditError("git cat-file --batch returned a truncated blob")
        blobs[object_id] = result.stdout[data_start:data_end]
        offset = data_end + 1
    if offset != len(result.stdout):
        raise FAuditError("git cat-file --batch returned unexpected trailing data")
    return blobs


def commit_parents(root: Path, commit: str) -> tuple[str, ...]:
    """Return the exact parent list for one validated commit."""

    output = run_git(root, ["rev-list", "--parents", "-n", "1", commit]).stdout
    try:
        fields = output.decode("ascii").strip().split()
    except UnicodeDecodeError as error:
        raise FAuditError(f"Git returned invalid parents for {commit}") from error
    if not fields or fields[0] != commit:
        raise FAuditError(f"Git returned mismatched parents for {commit}")
    return tuple(fields[1:])


def validate_introduction_commit(
    root: Path,
    introduced_commit: str,
    source_path: str,
    recorded_blob: str,
    location: str,
) -> None:
    """Require the recorded commit to add this exact source path and blob."""

    introduced_entry = tree_entry(root, introduced_commit, source_path)
    if introduced_entry is None:
        raise FAuditError(f"{location}: introduced commit does not track source")
    mode, object_type, object_id = introduced_entry
    if mode != "100644" or object_type != "blob" or object_id != recorded_blob:
        raise FAuditError(f"{location}: introduced commit has the wrong source blob")
    for parent in commit_parents(root, introduced_commit):
        if tree_entry(root, parent, source_path) is not None:
            raise FAuditError(
                f"{location}: source path already exists in introduced commit parent {parent}"
            )


def blob_bytes(root: Path, revision: str, path: str) -> bytes:
    """Read one tracked blob, failing closed on every Git error."""

    return run_git(root, ["cat-file", "blob", f"{revision}:{path}"]).stdout


def assert_commit(root: Path, commit: str, location: str) -> None:
    """Require one full lowercase SHA-1 commit id."""

    if COMMIT_PATTERN.fullmatch(commit) is None:
        raise FAuditError(f"{location} must be a full lowercase commit id")
    run_git(root, ["cat-file", "-e", f"{commit}^{{commit}}"])


def assert_ancestor(root: Path, ancestor: str, descendant: str, location: str) -> None:
    """Require the recorded history anchor to be reachable from the candidate."""

    result = run_git(
        root,
        ["merge-base", "--is-ancestor", ancestor, descendant],
        accepted_codes=frozenset({0, 1}),
    )
    if result.returncode == 1:
        raise FAuditError(f"{location} is not an ancestor of {descendant}")


def validate_generated_from(
    root: Path,
    entry: dict[str, Any],
    introduced_commit: str,
    location: str,
) -> None:
    """Validate the explicit incomplete historical provenance contract."""

    generated_location = f"{location}.generated-from"
    generated = require_mapping(entry.get("generated-from"), generated_location)
    require_exact_keys(generated, GENERATED_FROM_KEYS, generated_location)
    if generated.get("status") != "incomplete-historical-provenance":
        raise FAuditError(
            f"{generated_location}.status must be incomplete-historical-provenance"
        )
    generator_path = require_string(
        generated, "historical_generator_path", generated_location
    )
    validate_relative_path(
        generator_path, f"{generated_location}.historical_generator_path"
    )

    inferred_parent = require_string(
        generated, "inferred_parent_commit", generated_location
    )
    historical_context = require_string(
        generated, "historical_context_commit", generated_location
    )
    assert_commit(root, inferred_parent, f"{generated_location}.inferred_parent_commit")
    assert_commit(
        root,
        historical_context,
        f"{generated_location}.historical_context_commit",
    )
    if inferred_parent not in commit_parents(root, introduced_commit):
        raise FAuditError(
            f"{generated_location}.inferred_parent_commit is not an exact parent"
        )
    assert_ancestor(
        root,
        historical_context,
        introduced_commit,
        f"{generated_location}.historical_context_commit",
    )

    require_nullable_string(generated, "historical_backend", generated_location)
    exact_source = require_nullable_string(
        generated, "exact_source_commit", generated_location
    )
    if exact_source is not None:
        assert_commit(root, exact_source, f"{generated_location}.exact_source_commit")
        assert_ancestor(
            root,
            exact_source,
            introduced_commit,
            f"{generated_location}.exact_source_commit",
        )
    require_nullable_string(generated, "exact_capture_command", generated_location)
    require_string(generated, "evidence", generated_location)


def validate_build_reproducibility(
    entry: dict[str, Any], location: str
) -> None:
    """Validate the schema-v2 statement that exact reproduction is unavailable."""

    build_location = f"{location}.build_reproducibility"
    build = require_mapping(entry.get("build_reproducibility"), build_location)
    require_exact_keys(build, BUILD_REPRODUCIBILITY_KEYS, build_location)
    if build.get("status") != "historical-provenance-incomplete":
        raise FAuditError(
            f"{build_location}.status must be historical-provenance-incomplete"
        )
    if require_boolean(build, "exact_byte_reproduction", build_location):
        raise FAuditError(
            f"{build_location}.exact_byte_reproduction must be false in schema version 2"
        )
    require_string(build, "reason", build_location)

    candidate_location = f"{build_location}.current_semantic_candidate"
    candidate = require_mapping(
        build.get("current_semantic_candidate"), candidate_location
    )
    status = require_string(candidate, "status", candidate_location)
    if status == "unverified":
        require_exact_keys(
            candidate, SEMANTIC_CANDIDATE_UNVERIFIED_KEYS, candidate_location
        )
        require_string_list(candidate, "commands", candidate_location)
    elif status == "not-available":
        require_exact_keys(
            candidate, SEMANTIC_CANDIDATE_UNAVAILABLE_KEYS, candidate_location
        )
        require_string(candidate, "reason", candidate_location)
        commands = candidate.get("commands")
        if not isinstance(commands, list) or commands:
            raise FAuditError(f"{candidate_location}.commands must be an empty list")
    else:
        raise FAuditError(
            f"{candidate_location}.status must be unverified or not-available"
        )


def validate_verification_gates(entry: dict[str, Any], location: str) -> None:
    """Require each schema-v2 verification gate exactly once."""

    gates = entry.get("verification_gates")
    if not isinstance(gates, list) or not gates:
        raise FAuditError(f"{location}.verification_gates must be a non-empty list")

    gate_names: set[str] = set()
    for index, raw_gate in enumerate(gates):
        gate_location = f"{location}.verification_gates[{index}]"
        gate = require_mapping(raw_gate, gate_location)
        gate_name = require_string(gate, "gate", gate_location)
        if gate_name in gate_names:
            raise FAuditError(f"{gate_location}.gate is duplicated: {gate_name}")
        gate_names.add(gate_name)
        if gate_name == "debug-release-build-test":
            require_exact_keys(
                gate,
                frozenset({"gate", "status", "commands"}),
                gate_location,
            )
            if gate.get("status") != "required-before-integration":
                raise FAuditError(
                    f"{gate_location}.status must be required-before-integration"
                )
            commands = require_string_list(gate, "commands", gate_location)
            if tuple(commands) != REQUIRED_VERIFICATION_COMMANDS:
                raise FAuditError(
                    f"{gate_location}.commands must equal the Debug/Release gate list"
                )
        else:
            require_exact_keys(
                gate, frozenset({"gate", "expected"}), gate_location
            )
            require_string(gate, "expected", gate_location)
    if frozenset(gate_names) != REQUIRED_VERIFICATION_GATES:
        missing = sorted(REQUIRED_VERIFICATION_GATES - gate_names)
        unknown = sorted(gate_names - REQUIRED_VERIFICATION_GATES)
        raise FAuditError(
            f"{location}.verification_gates differ; "
            f"missing={missing}, unknown={unknown}"
        )


def scan_references(
    root: Path,
    needles: frozenset[str],
    *,
    revision: str,
    excluded_paths: frozenset[str] = frozenset(),
) -> tuple[str, ...]:
    """Scan every tracked blob case-insensitively, including UTF-16 data."""

    if not needles:
        return ()
    entries = tuple(
        entry
        for entry in tree_entries_recursive(root, revision)
        if (
            entry[0] != LEDGER_PATH
            and entry[0] not in excluded_paths
            and entry[2] == "blob"
        )
    )
    blobs = batch_blob_bytes(root, frozenset(entry[3] for entry in entries))
    encoded_needles = {
        encoding: re.compile(
            b"|".join(
                re.escape(needle.casefold().encode(encoding))
                for needle in sorted(needles, key=lambda item: (-len(item), item))
            ),
            re.IGNORECASE,
        )
        for encoding in REFERENCE_ENCODINGS
    }
    matches: set[str] = set()
    for path, _mode, _object_type, object_id in entries:
        data = blobs[object_id]
        for encoding, pattern in encoded_needles.items():
            if pattern.search(data) is not None:
                matches.add(f"{path} [{encoding}]")
    return tuple(sorted(matches))


def validate_reference_contract(
    entry: dict[str, Any],
    source_path: str,
    destination_path: str,
    location: str,
) -> frozenset[str]:
    """Validate the zero-reference schema and return its search needles."""

    references = require_mapping(entry.get("references"), f"{location}.references")
    expected_fields = (
        "textual_reference_count_at_baseline",
        "textual_reference_count_at_current",
        "build_manifest_reference_count_at_baseline",
        "build_manifest_reference_count_at_current",
    )
    for field in expected_fields:
        if require_integer(references, field, f"{location}.references") != 0:
            raise FAuditError(
                f"{location}.references.{field} must be zero in schema version 2"
            )
    if references.get("audit_command") != AUDIT_COMMAND:
        raise FAuditError(
            f"{location}.references.audit_command must be {AUDIT_COMMAND!r}"
        )

    basename = PurePosixPath(destination_path).name
    return frozenset({source_path, destination_path, basename})


def validate_entry(
    root: Path,
    entry: dict[str, Any],
    index: int,
    candidate_revision: str,
) -> tuple[str, str, str, frozenset[str]]:
    """Validate one ledger entry and return its unique paths."""

    location = f"artifacts[{index}]"
    source_path = require_string(entry, "source_path_at_baseline", location)
    destination_path = require_string(entry, "destination_path", location)
    source = validate_relative_path(
        source_path, f"{location}.source_path_at_baseline"
    )
    destination = validate_relative_path(
        destination_path, f"{location}.destination_path"
    )
    if not destination.parts or destination.parts[0] != "artifact":
        raise FAuditError(f"{location}.destination_path must be under artifact/")
    if destination_path == LEDGER_PATH or source_path == destination_path:
        raise FAuditError(f"{location} has an invalid source/destination pair")
    if entry.get("source-of-truth") != destination_path:
        raise FAuditError(f"{location}.source-of-truth must equal destination_path")
    if entry.get("classification") != "historical-visual-evidence":
        raise FAuditError(f"{location}.classification is unsupported")
    if entry.get("canonical_role") != "artifact-source":
        raise FAuditError(f"{location}.canonical_role is unsupported")
    require_string(entry, "responsibility_owner", location)
    require_string(entry, "move_reason", location)

    tracked = require_mapping(entry.get("tracked"), f"{location}.tracked")
    baseline_commit = require_string(
        tracked, "baseline_commit", f"{location}.tracked"
    )
    introduced_commit = require_string(
        tracked, "introduced_by_commit", f"{location}.tracked"
    )
    recorded_blob = require_string(tracked, "git_blob", f"{location}.tracked")
    recorded_sha256 = require_string(tracked, "sha256", f"{location}.tracked")
    byte_count = require_integer(tracked, "byte_count", f"{location}.tracked")
    width = require_integer(tracked, "width", f"{location}.tracked")
    height = require_integer(tracked, "height", f"{location}.tracked")
    if width == 0 or height == 0:
        raise FAuditError(f"{location}.tracked PNG dimensions must be positive")
    if OBJECT_PATTERN.fullmatch(recorded_blob) is None:
        raise FAuditError(f"{location}.tracked.git_blob is not a full object id")
    if SHA256_PATTERN.fullmatch(recorded_sha256) is None:
        raise FAuditError(f"{location}.tracked.sha256 is invalid")
    if tracked.get("media_type") != "image/png":
        raise FAuditError(f"{location}.tracked.media_type must be image/png")

    assert_commit(root, baseline_commit, f"{location}.tracked.baseline_commit")
    assert_commit(root, introduced_commit, f"{location}.tracked.introduced_by_commit")
    assert_ancestor(
        root,
        introduced_commit,
        baseline_commit,
        f"{location}.tracked.introduced_by_commit",
    )
    assert_ancestor(
        root,
        baseline_commit,
        "HEAD",
        f"{location}.tracked.baseline_commit",
    )
    validate_introduction_commit(
        root,
        introduced_commit,
        source_path,
        recorded_blob,
        f"{location}.tracked.introduced_by_commit",
    )
    validate_generated_from(root, entry, introduced_commit, location)
    validate_build_reproducibility(entry, location)
    validate_verification_gates(entry, location)

    baseline_entry = tree_entry(root, baseline_commit, source_path)
    if baseline_entry is None:
        raise FAuditError(f"{location}: baseline source is not tracked")
    baseline_mode, baseline_type, baseline_blob = baseline_entry
    if baseline_mode != "100644" or baseline_type != "blob":
        raise FAuditError(f"{location}: baseline source is not a regular blob")
    if baseline_blob != recorded_blob:
        raise FAuditError(
            f"{location}: baseline blob {baseline_blob} != {recorded_blob}"
        )

    if tree_entry(root, candidate_revision, source_path) is not None:
        raise FAuditError(f"{location}: source path remains in the candidate tree")
    candidate_entry = tree_entry(root, candidate_revision, destination_path)
    if candidate_entry is None:
        raise FAuditError(f"{location}: destination is not tracked in the candidate tree")
    candidate_mode, candidate_type, candidate_blob = candidate_entry
    if candidate_mode != "100644" or candidate_type != "blob":
        raise FAuditError(f"{location}: destination is not a regular blob")
    if candidate_blob != recorded_blob:
        raise FAuditError(f"{location}: candidate destination blob drifted")

    source_file = filesystem_path(root, source)
    destination_file = filesystem_path(root, destination)
    if path_exists_without_following(source_file):
        raise FAuditError(f"{location}: source path remains in the working tree")
    if not path_exists_without_following(destination_file):
        raise FAuditError(f"{location}: destination is absent from the working tree")
    reject_reparse_chain(root, destination_file, f"{location}.destination_path")
    if not destination_file.is_file():
        raise FAuditError(f"{location}: destination is not a regular file")

    baseline_bytes = blob_bytes(root, baseline_commit, source_path)
    candidate_bytes = blob_bytes(root, candidate_revision, destination_path)
    current_bytes = destination_file.read_bytes()
    if baseline_bytes != candidate_bytes or candidate_bytes != current_bytes:
        raise FAuditError(f"{location}: baseline, candidate, and working bytes differ")
    if len(current_bytes) != byte_count:
        raise FAuditError(f"{location}: byte_count does not match destination")
    actual_sha256 = hashlib.sha256(current_bytes).hexdigest().upper()
    if actual_sha256 != recorded_sha256.upper():
        raise FAuditError(f"{location}: SHA-256 does not match destination")
    actual_width, actual_height = parse_png_dimensions(current_bytes, location)
    if (actual_width, actual_height) != (width, height):
        raise FAuditError(f"{location}: PNG dimensions do not match ledger")

    reference_needles = validate_reference_contract(
        entry,
        source_path,
        destination_path,
        location,
    )
    return source_path, destination_path, baseline_commit, reference_needles


def audit_repository(root: Path) -> list[str]:
    """Validate the artifact ledger against Git history and candidate bytes."""

    root = root.resolve(strict=True)
    top_level = run_git(root, ["rev-parse", "--show-toplevel"]).stdout
    try:
        git_root = Path(top_level.decode("utf-8").strip()).resolve(strict=True)
    except (UnicodeDecodeError, OSError) as error:
        raise FAuditError("Git returned an invalid repository root") from error
    if git_root != root:
        raise FAuditError(f"--root must be Git top-level: {git_root}")

    candidate_revision = create_candidate_tree(root)
    ledger_file = root / LEDGER_PATH
    reject_reparse_chain(root, ledger_file, LEDGER_PATH)
    ledger_entry = tree_entry(root, candidate_revision, LEDGER_PATH)
    if ledger_entry is None or ledger_entry[:2] != ("100644", "blob"):
        raise FAuditError(f"{LEDGER_PATH} must be a tracked regular candidate blob")
    try:
        raw_ledger = ledger_file.read_bytes()
        if blob_bytes(root, candidate_revision, LEDGER_PATH) != raw_ledger:
            raise FAuditError(f"{LEDGER_PATH} candidate and working bytes differ")
        if raw_ledger.startswith(b"\xef\xbb\xbf"):
            raise FAuditError(f"{LEDGER_PATH} must not contain a UTF-8 BOM")
        ledger = json.loads(raw_ledger.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise FAuditError(f"{LEDGER_PATH} is not strict UTF-8 JSON: {error}") from error
    ledger_object = require_mapping(ledger, LEDGER_PATH)
    require_exact_keys(
        ledger_object,
        frozenset({"schema_version", "artifacts"}),
        LEDGER_PATH,
    )
    if ledger_object.get("schema_version") != 2:
        raise FAuditError(f"{LEDGER_PATH}.schema_version must equal 2")
    artifacts = ledger_object.get("artifacts")
    if not isinstance(artifacts, list):
        raise FAuditError(f"{LEDGER_PATH}.artifacts must be a list")

    violations: list[str] = []
    sources: set[str] = set()
    destinations: set[str] = set()
    baseline_needles: dict[str, set[str]] = {}
    baseline_sources: dict[str, set[str]] = {}
    current_needles: set[str] = set()
    for index, raw_entry in enumerate(artifacts):
        entry = require_mapping(raw_entry, f"artifacts[{index}]")
        source_path, destination_path, baseline_commit, reference_needles = validate_entry(
            root, entry, index, candidate_revision
        )
        if source_path in sources:
            raise FAuditError(f"duplicate source path: {source_path}")
        if destination_path in destinations:
            raise FAuditError(f"duplicate destination path: {destination_path}")
        sources.add(source_path)
        destinations.add(destination_path)
        baseline_needles.setdefault(baseline_commit, set()).update(reference_needles)
        baseline_sources.setdefault(baseline_commit, set()).add(source_path)
        current_needles.update(reference_needles)

    for baseline_commit, needles in sorted(baseline_needles.items()):
        matches = scan_references(
            root,
            frozenset(needles),
            revision=baseline_commit,
            excluded_paths=frozenset(baseline_sources[baseline_commit]),
        )
        if matches:
            violations.append(
                f"baseline {baseline_commit} references are not zero: "
                + " | ".join(matches)
            )
    current_matches = scan_references(
        root,
        frozenset(current_needles),
        revision=candidate_revision,
        excluded_paths=frozenset(destinations),
    )
    if current_matches:
        violations.append(
            "current references are not zero: " + " | ".join(current_matches)
        )

    tracked_artifacts = {
        path
        for path, _mode, _object_type, _object_id in tree_entries_recursive(
            root, candidate_revision, "artifact"
        )
        if path != LEDGER_PATH
    }
    if tracked_artifacts != destinations:
        unlisted = sorted(tracked_artifacts - destinations)
        missing = sorted(destinations - tracked_artifacts)
        raise FAuditError(
            "tracked artifact paths differ from ledger destinations; "
            f"unlisted={unlisted}, missing={missing}"
        )
    return violations


def png_bytes(width: int = 2, height: int = 1) -> bytes:
    """Create a deterministic RGBA PNG for synthetic repository tests."""

    def chunk(kind: bytes, payload: bytes) -> bytes:
        body = kind + payload
        return (
            struct.pack(">I", len(payload))
            + body
            + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    row = b"\x00" + bytes((32, 64, 96, 255)) * width
    pixels = row * height
    return (
        PNG_SIGNATURE
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(pixels))
        + chunk(b"IEND", b"")
    )


def write_json(path: Path, value: dict[str, Any]) -> None:
    """Write deterministic strict UTF-8 JSON for synthetic fixtures."""

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def commit_all(root: Path, message: str) -> str:
    """Commit a synthetic fixture with local deterministic identity."""

    run_git(root, ["add", "--all"])
    run_git(
        root,
        [
            "-c",
            "user.name=ACS Artifact Audit",
            "-c",
            "user.email=artifact-audit@acs.invalid",
            "commit",
            "-m",
            message,
            "--quiet",
        ],
    )
    return run_git(root, ["rev-parse", "HEAD"]).stdout.decode("ascii").strip()


def create_fixture(
    root: Path,
    *,
    baseline_reference: bool = False,
    commit_candidate: bool = True,
) -> dict[str, Any]:
    """Create one valid artifact move as a commit or pending candidate."""

    run_git(root, ["init", "--quiet"])
    with (root / "historical-context.txt").open(
        "w", encoding="utf-8", newline="\n"
    ) as output:
        output.write("historical context\n")
    historical_context = commit_all(root, "historical context")
    source_path = "acs/screenshots/render-proof.png"
    destination_path = "artifact/visual/render-proof.png"
    source_file = root.joinpath(*PurePosixPath(source_path).parts)
    source_file.parent.mkdir(parents=True, exist_ok=True)
    data = png_bytes()
    source_file.write_bytes(data)
    if baseline_reference:
        with (root / "baseline-reference.txt").open(
            "w", encoding="utf-8", newline="\n"
        ) as output:
            output.write("render-proof.png\n")
    baseline_commit = commit_all(root, "baseline")
    source_blob = tree_entry(root, baseline_commit, source_path)
    if source_blob is None:
        raise FAuditError("self-test baseline blob is missing")

    destination_file = root.joinpath(*PurePosixPath(destination_path).parts)
    destination_file.parent.mkdir(parents=True, exist_ok=True)
    source_file.replace(destination_file)
    ledger: dict[str, Any] = {
        "schema_version": 2,
        "artifacts": [
            {
                "source_path_at_baseline": source_path,
                "destination_path": destination_path,
                "classification": "historical-visual-evidence",
                "responsibility_owner": "acs/src/render",
                "canonical_role": "artifact-source",
                "source-of-truth": destination_path,
                "tracked": {
                    "baseline_commit": baseline_commit,
                    "introduced_by_commit": baseline_commit,
                    "git_blob": source_blob[2],
                    "byte_count": len(data),
                    "sha256": hashlib.sha256(data).hexdigest().upper(),
                    "media_type": "image/png",
                    "width": 2,
                    "height": 1,
                },
                "generated-from": {
                    "status": "incomplete-historical-provenance",
                    "historical_generator_path": "acs/tools/RenderProofCapture",
                    "inferred_parent_commit": historical_context,
                    "historical_context_commit": historical_context,
                    "historical_backend": None,
                    "exact_source_commit": None,
                    "exact_capture_command": None,
                    "evidence": "Synthetic historical provenance is intentionally incomplete.",
                },
                "references": {
                    "textual_reference_count_at_baseline": 0,
                    "textual_reference_count_at_current": 0,
                    "build_manifest_reference_count_at_baseline": 0,
                    "build_manifest_reference_count_at_current": 0,
                    "audit_command": AUDIT_COMMAND,
                },
                "build_reproducibility": {
                    "status": "historical-provenance-incomplete",
                    "exact_byte_reproduction": False,
                    "reason": "Synthetic capture details were not recorded.",
                    "current_semantic_candidate": {
                        "status": "not-available",
                        "reason": "No current ACS executable owns this historical visual.",
                        "commands": [],
                    },
                },
                "move_reason": "self-test",
                "verification_gates": [
                    {
                        "gate": "byte-identity",
                        "expected": "Synthetic bytes remain identical.",
                    },
                    {
                        "gate": "rename-and-residue",
                        "expected": "Synthetic source is absent.",
                    },
                    {
                        "gate": "reference-scan",
                        "expected": "Synthetic references remain absent.",
                    },
                    {
                        "gate": "debug-release-build-test",
                        "status": "required-before-integration",
                        "commands": list(REQUIRED_VERIFICATION_COMMANDS),
                    },
                ],
            }
        ],
    }
    write_json(root / LEDGER_PATH, ledger)
    if commit_candidate:
        commit_all(root, "move artifact")
    return ledger


def expect_failure(root: Path, expected_text: str) -> None:
    """Require one synthetic mutation to fail for the intended reason."""

    try:
        violations = audit_repository(root)
    except FAuditError as error:
        message = str(error)
    else:
        message = " | ".join(violations)
    if expected_text not in message:
        raise FAuditError(
            f"self-test expected {expected_text!r}, got {message!r}"
        )


def create_empty_fixture(root: Path) -> None:
    """Create an uncommitted candidate that removes the final tracked artifact."""

    ledger = create_fixture(root)
    destination = root.joinpath(
        *PurePosixPath(ledger["artifacts"][0]["destination_path"]).parts
    )
    destination.unlink()
    write_json(root / LEDGER_PATH, {"schema_version": 2, "artifacts": []})


def run_self_test() -> None:
    """Exercise success and fail-closed mutations in isolated Git fixtures."""

    with tempfile.TemporaryDirectory(prefix="acs-artifact-ledger-audit-") as temp:
        parent = Path(temp)

        valid_root = parent / "valid"
        valid_root.mkdir()
        create_fixture(valid_root)
        violations = audit_repository(valid_root)
        if violations:
            raise FAuditError("valid fixture failed: " + " | ".join(violations))
        try:
            scan_references(
                valid_root,
                frozenset({"render-proof.png"}),
                revision="not-a-revision",
            )
        except FAuditError as error:
            if "failed with exit" not in str(error):
                raise
        else:
            raise FAuditError("invalid reference-scan revision was accepted")

        empty_root = parent / "empty"
        empty_root.mkdir()
        create_empty_fixture(empty_root)
        violations = audit_repository(empty_root)
        if violations:
            raise FAuditError("empty fixture failed: " + " | ".join(violations))

        new_candidate_root = parent / "new-artifact-candidate"
        new_candidate_root.mkdir()
        create_fixture(new_candidate_root, commit_candidate=False)
        index_path = Path(
            run_git(new_candidate_root, ["rev-parse", "--git-path", "index"])
            .stdout.decode("utf-8")
            .strip()
        )
        if not index_path.is_absolute():
            index_path = new_candidate_root / index_path
        index_before = index_path.read_bytes()
        status_before = run_git(
            new_candidate_root,
            ["status", "--porcelain=v1", "-z", "--untracked-files=all"],
        ).stdout
        violations = audit_repository(new_candidate_root)
        status_after = run_git(
            new_candidate_root,
            ["status", "--porcelain=v1", "-z", "--untracked-files=all"],
        ).stdout
        if violations:
            raise FAuditError(
                "new artifact candidate failed: " + " | ".join(violations)
            )
        if index_path.read_bytes() != index_before or status_after != status_before:
            raise FAuditError("candidate audit changed the live index or working files")

        stray_root = parent / "untracked-artifact"
        stray_root.mkdir()
        create_empty_fixture(stray_root)
        stray = stray_root / "artifact/visual/stray.png"
        stray.parent.mkdir(parents=True, exist_ok=True)
        stray.write_bytes(png_bytes())
        expect_failure(stray_root, "unlisted=")

        unverified_root = parent / "unverified"
        unverified_root.mkdir()
        ledger = create_fixture(unverified_root)
        ledger["artifacts"][0]["build_reproducibility"][
            "current_semantic_candidate"
        ] = {
            "status": "unverified",
            "commands": ["cmake --build synthetic"],
        }
        write_json(unverified_root / LEDGER_PATH, ledger)
        violations = audit_repository(unverified_root)
        if violations:
            raise FAuditError(
                "unverified fixture failed: " + " | ".join(violations)
            )

        schema_root = parent / "schema"
        schema_root.mkdir()
        ledger = create_fixture(schema_root)
        ledger["schema_version"] = 1
        write_json(schema_root / LEDGER_PATH, ledger)
        expect_failure(schema_root, "schema_version")

        traversal_root = parent / "traversal"
        traversal_root.mkdir()
        ledger = create_fixture(traversal_root)
        ledger["artifacts"][0]["destination_path"] = "artifact/../escape.png"
        write_json(traversal_root / LEDGER_PATH, ledger)
        expect_failure(traversal_root, "not normalized")

        duplicate_root = parent / "duplicate"
        duplicate_root.mkdir()
        ledger = create_fixture(duplicate_root)
        ledger["artifacts"].append(copy.deepcopy(ledger["artifacts"][0]))
        write_json(duplicate_root / LEDGER_PATH, ledger)
        expect_failure(duplicate_root, "duplicate source path")

        source_root = parent / "source-remains"
        source_root.mkdir()
        ledger = create_fixture(source_root)
        source = source_root / ledger["artifacts"][0]["source_path_at_baseline"]
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(png_bytes())
        expect_failure(source_root, "source path remains")

        sha_root = parent / "sha"
        sha_root.mkdir()
        ledger = create_fixture(sha_root)
        ledger["artifacts"][0]["tracked"]["sha256"] = "0" * 64
        write_json(sha_root / LEDGER_PATH, ledger)
        expect_failure(sha_root, "SHA-256")

        size_root = parent / "size"
        size_root.mkdir()
        ledger = create_fixture(size_root)
        ledger["artifacts"][0]["tracked"]["byte_count"] += 1
        write_json(size_root / LEDGER_PATH, ledger)
        expect_failure(size_root, "byte_count")

        dimensions_root = parent / "dimensions"
        dimensions_root.mkdir()
        ledger = create_fixture(dimensions_root)
        ledger["artifacts"][0]["tracked"]["width"] += 1
        write_json(dimensions_root / LEDGER_PATH, ledger)
        expect_failure(dimensions_root, "PNG dimensions")

        current_ref_root = parent / "current-reference"
        current_ref_root.mkdir()
        create_fixture(current_ref_root)
        with (current_ref_root / "current-reference.txt").open(
            "w", encoding="utf-8", newline="\n"
        ) as output:
            output.write("render-proof.png\n")
        commit_all(current_ref_root, "add current reference")
        expect_failure(current_ref_root, "current references are not zero")

        baseline_ref_root = parent / "baseline-reference"
        baseline_ref_root.mkdir()
        create_fixture(baseline_ref_root, baseline_reference=True)
        expect_failure(baseline_ref_root, "references are not zero")

        unlisted_root = parent / "unlisted-artifact"
        unlisted_root.mkdir()
        create_fixture(unlisted_root)
        unlisted = unlisted_root / "artifact/visual/unlisted.png"
        unlisted.write_bytes(png_bytes())
        commit_all(unlisted_root, "add unlisted artifact")
        expect_failure(unlisted_root, "unlisted=")

        introduction_root = parent / "wrong-introduction"
        introduction_root.mkdir()
        ledger = create_fixture(introduction_root)
        ledger["artifacts"][0]["tracked"]["introduced_by_commit"] = ledger[
            "artifacts"
        ][0]["generated-from"]["inferred_parent_commit"]
        write_json(introduction_root / LEDGER_PATH, ledger)
        expect_failure(introduction_root, "does not track source")

        uppercase_ref_root = parent / "uppercase-reference"
        uppercase_ref_root.mkdir()
        create_fixture(uppercase_ref_root)
        with (uppercase_ref_root / "uppercase-reference.txt").open(
            "w", encoding="utf-8", newline="\n"
        ) as output:
            output.write("RENDER-PROOF.PNG\n")
        commit_all(uppercase_ref_root, "add uppercase reference")
        expect_failure(uppercase_ref_root, "current references are not zero")

        utf16_ref_root = parent / "utf16-reference"
        utf16_ref_root.mkdir()
        create_fixture(utf16_ref_root)
        (utf16_ref_root / "utf16-reference.bin").write_bytes(
            "RENDER-PROOF.PNG".encode("utf-16-le")
        )
        commit_all(utf16_ref_root, "add UTF-16 reference")
        expect_failure(utf16_ref_root, "current references are not zero")

        generated_schema_root = parent / "generated-schema"
        generated_schema_root.mkdir()
        ledger = create_fixture(generated_schema_root)
        ledger["artifacts"][0]["generated-from"] = {}
        write_json(generated_schema_root / LEDGER_PATH, ledger)
        expect_failure(generated_schema_root, "generated-from keys differ")

        reproducibility_schema_root = parent / "reproducibility-schema"
        reproducibility_schema_root.mkdir()
        ledger = create_fixture(reproducibility_schema_root)
        ledger["artifacts"][0]["build_reproducibility"] = {}
        write_json(reproducibility_schema_root / LEDGER_PATH, ledger)
        expect_failure(reproducibility_schema_root, "build_reproducibility keys differ")

        gate_schema_root = parent / "gate-schema"
        gate_schema_root.mkdir()
        ledger = create_fixture(gate_schema_root)
        ledger["artifacts"][0]["verification_gates"] = [{}]
        write_json(gate_schema_root / LEDGER_PATH, ledger)
        expect_failure(gate_schema_root, ".gate must be a non-empty string")


def parse_arguments(arguments: Sequence[str]) -> argparse.Namespace:
    """Parse the public audit command line."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, help="repository top-level")
    parser.add_argument("--self-test", action="store_true")
    parsed = parser.parse_args(arguments)
    if parsed.self_test == (parsed.root is not None):
        parser.error("choose exactly one of --root or --self-test")
    return parsed


def main(arguments: Sequence[str] | None = None) -> int:
    """Run the selected audit and return a stable process status."""

    configure_utf8_console()
    parsed = parse_arguments(sys.argv[1:] if arguments is None else arguments)
    try:
        if parsed.self_test:
            run_self_test()
            print("artifact ledger audit self-test PASS cases=22")
            return 0
        violations = audit_repository(parsed.root)
    except (FAuditError, OSError) as error:
        print(f"artifact ledger audit ERROR: {error}", file=sys.stderr)
        return 2
    if violations:
        for violation in violations:
            print(f"artifact ledger audit FAIL: {violation}", file=sys.stderr)
        return 1
    print("artifact ledger audit PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
