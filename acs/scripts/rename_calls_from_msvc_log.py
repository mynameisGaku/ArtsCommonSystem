# SPDX-License-Identifier: Apache-2.0
"""MSVCのC2039 logを読み、対応表にある型とmemberの参照だけを改名する。

複数型に存在するmember名を一括置換すると無関係なAPIまで壊れる。
file/line/columnとerror内の型名を突き合わせ、対象が一意な箇所だけを置換する。

最初の位置引数はbuild log、``--dry``は書込み抑止、``--map``は型別member対応表JSONを受け取る。
対応表は型名をkeyとし、旧member名から新member名へのobjectをvalueに持つ。

macro展開で報告行がずれた場合は周辺行を検索し、対象名が一意なときだけ置換する。
一意に解決できない箇所はskippedとして変更しない。
"""
import collections
import json
import re
import sys
from pathlib import Path

# --map未指定時に使う型別member対応表。
DEFAULT_ARRAY_MAP = {
    "Size": "Num", "Capacity": "Max", "Data": "GetData", "Back": "Last",
    "Clear": "Reset", "ReleaseStorage": "Empty", "Resize": "SetNum",
    "Find": "FindByKey",
}
DEFAULT_MAP_BY_TYPE = {
    "TArray": DEFAULT_ARRAY_MAP,
    "TInlineArray": DEFAULT_ARRAY_MAP,
    "TSpan": {"Size": "Num", "Data": "GetData", "Back": "Last"},
    "THashMap": {"Size": "Num", "Insert": "Add", "TryInsert": "TryAdd",
                 "Clear": "Reset", "ReleaseStorage": "Empty"},
    "TObservableArray": {"Size": "Num", "Data": "GetData", "Clear": "Reset"},
}

# drive-absolute file、line、columnを含むMSVC C2039行を抽出する。
LINE_RE = re.compile(
    r"^(?P<file>[A-Za-z]:[\\/][^()]+)\((?P<line>\d+),(?P<col>\d+)\):"
    r"\s*(?:fatal\s+)?error\s+C2039:\s*(?P<rest>.*)$"
)
MEMBER_RE = re.compile(r"'(?P<member>[A-Za-z_]\w*)'")
TYPE_RE = re.compile(r"'(?:acs::)?(?P<type>T[A-Za-z0-9_]*)")


def main(argv: list[str]) -> int:
    """build log を読んで該当呼び出しを改名し、件数を表示する。"""

    dry = "--dry" in argv
    positional = [a for a in argv if not a.startswith("--")]
    if not positional:
        print("usage: rename_calls_from_msvc_log.py <build.log> [--dry] [--map map.json]",
              file=sys.stderr)
        return 2
    log_path = Path(positional[0])

    map_by_type = DEFAULT_MAP_BY_TYPE
    if "--map" in argv:
        map_by_type = json.loads(Path(argv[argv.index("--map") + 1]).read_text(encoding="utf-8"))

    edits: dict[str, dict[tuple[int, int], tuple[str, str]]] = collections.defaultdict(dict)
    unmatched: collections.Counter = collections.Counter()
    per_type: collections.Counter = collections.Counter()

    for raw in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        matched = LINE_RE.match(raw.strip())
        if not matched:
            continue
        rest = matched.group("rest")
        members = MEMBER_RE.findall(rest)
        if not members:
            continue
        member = members[0]
        chosen = None
        for type_name in TYPE_RE.findall(rest):
            if type_name in map_by_type and member in map_by_type[type_name]:
                chosen = map_by_type[type_name][member]
                per_type[type_name] += 1
                break
        if chosen is None:
            unmatched[(member,)] += 1
            continue
        edits[matched.group("file")][
            (int(matched.group("line")), int(matched.group("col")))
        ] = (member, chosen)

    applied = 0
    skipped = 0
    for path_text, sites in edits.items():
        path = Path(path_text)
        if not path.exists():
            continue
        text = path.read_bytes().decode("utf-8")
        newline = "\r\n" if "\r\n" in text else "\n"
        lines = text.split(newline)
        for (line_no, col), (old, new) in sorted(sites.items(), reverse=True):
            if line_no - 1 >= len(lines):
                skipped += 1
                continue
            line = lines[line_no - 1]
            start = col - 1
            if line[start:start + len(old)] == old:
                lines[line_no - 1] = line[:start] + new + line[start + len(old):]
                applied += 1
                continue
            # マクロ引数などで報告行がずれる。窓内に 1 回だけなら安全に置換できる。
            patched = False
            for window in (0, 4, 10):
                found = []
                for index in range(line_no - 1, min(len(lines), line_no + window)):
                    for hit in re.finditer(r"\b" + old + r"\b(?=\s*\()", lines[index]):
                        found.append((index, hit.start()))
                if len(found) == 1:
                    index, start = found[0]
                    lines[index] = lines[index][:start] + new + lines[index][start + len(old):]
                    applied += 1
                    patched = True
                    break
            if not patched:
                skipped += 1
        if not dry:
            path.write_bytes(newline.join(lines).encode("utf-8"))

    print("files={} applied={} skipped={}".format(len(edits), applied, skipped))
    print("by type:", dict(per_type))
    if unmatched:
        print("unmatched members (対応表に無い):", dict(unmatched.most_common(10)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
