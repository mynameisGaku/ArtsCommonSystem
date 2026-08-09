# SPDX-License-Identifier: Apache-2.0
"""cpp_prefix_consumer_legacy_allowlist.json を実測から再生成する。

``audit_cpp_prefix_consumers.py`` は allowlist を「観測結果そのもの」と突き合わせる
exact 監査で、``--write`` を持たない。allowlist の entry は対象 file の **全体 SHA-256**
を含むため、tests / dist/verification を 1 byte でも変更すると、その file の
entry が全て無効化されて監査が落ちる。

このスクリプトは監査本体を import し、同じ ``_capture_repository_snapshot`` →
``_scan_snapshot`` の観測をそのまま allowlist として書き出す。reason は
``(path, legacy, construct)`` をキーに旧 allowlist から引き継ぎ、対応が無い新規 site は
監査本体と同じ構文・path 規則から理由を決める。

使い方:

    python -B scripts/regen_prefix_consumer_allowlist.py            # acs/ を推定
    python -B scripts/regen_prefix_consumer_allowlist.py C:\\acsw\\p2\\acs
    python -B scripts/regen_prefix_consumer_allowlist.py --dry      # 書かずに差分だけ見る

書き出した後、表示される ``EXPECTED_ALLOWLIST_SHA256`` を
``scripts/audit_cpp_prefix_consumers.py`` の同名定数へ **同じ commit で** 反映すること。
JSON は BOM 無し LF で書く (loader が CRLF を拒否するため)。
"""
import collections
import json
import sys
from pathlib import Path


def main(argv: list[str]) -> int:
    """allowlist を再生成し、新しい baseline SHA-256 を表示する。"""

    dry = "--dry" in argv
    positional = [a for a in argv if not a.startswith("--")]
    acs_root = Path(positional[0]).absolute() if positional else Path(__file__).resolve().parents[1]

    sys.path.insert(0, str(acs_root / "scripts"))
    import audit_cpp_prefix_consumers as audit   # noqa: E402  (path 設定後に import する)

    allowlist_path = acs_root / audit.ALLOWLIST_RELATIVE_PATH
    previous = json.loads(allowlist_path.read_text(encoding="utf-8"))
    previous_reasons: dict[tuple[str, str, str], str] = {}
    for entry in previous["entries"]:
        previous_reasons.setdefault(
            (entry["path"], entry["legacy"], entry["construct"]), entry["reason"]
        )

    snapshot = audit._capture_repository_snapshot(acs_root)
    registry_item = audit._snapshot_file(
        snapshot, "acs/{}".format(audit.REGISTRY_RELATIVE_PATH.as_posix())
    )
    mapping, registry_sha256 = audit._load_legacy_mapping_document(
        audit._load_json_bytes(registry_item.raw, registry_item.path)
    )
    observed = audit._scan_snapshot(snapshot, mapping)

    entries = []
    defaulted = collections.Counter()
    for item in observed:
        site = item.site
        key = (site.path, site.legacy, site.construct)
        expected_reason = audit._expected_allowlist_reason(site.path, site.construct)
        if not expected_reason:
            raise RuntimeError(
                "unsupported allowlist site: {}:{}".format(site.path, site.construct)
            )
        reason = previous_reasons.get(key)
        if reason != expected_reason:
            reason = expected_reason
            defaulted[key] += 1
        entries.append(
            {
                "path": site.path,
                "file_sha256": site.file_sha256,
                "line_hint": item.line_number,
                "line_sha256": site.line_sha256,
                "line_occurrence": site.line_occurrence,
                "token_occurrence": site.token_occurrence,
                "legacy": site.legacy,
                "canonical": site.canonical,
                "reason": reason,
                "lexical_context": site.lexical_context,
                "construct": site.construct,
                "qualifier": site.qualifier,
                "preprocessor_state": site.preprocessor_state,
            }
        )

    document = {
        "schema_version": 1,
        "registry_legacy_sha256": registry_sha256,
        "entries": entries,
    }
    if not dry:
        # BOM 無し LF で書く (write_text だと Windows で CRLF になり loader が拒否する)。
        allowlist_path.write_bytes(
            (json.dumps(document, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
        )

    allowed = audit._load_allowlist_document(document, mapping, registry_sha256)
    diagnostics = audit._reconcile(observed, allowed)
    print("entries: {} (was {})".format(len(entries), len(previous["entries"])))
    print("reason defaulted: {} {}".format(sum(defaulted.values()), dict(defaulted)))
    print("EXPECTED_ALLOWLIST_SHA256 =", audit._allowlist_sha256(registry_sha256, allowed))
    print("reconcile diagnostics:", len(diagnostics), diagnostics[:5])
    return 1 if diagnostics else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
