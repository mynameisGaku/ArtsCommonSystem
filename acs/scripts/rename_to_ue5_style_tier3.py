# SPDX-License-Identifier: Apache-2.0
"""
ACS v1 -> v2 命名規則移行: Tier 3 (大量 auto-discovery)。

Tier 1/2 では curated list で進めたが、残り 400+ 型を手書きするのは
非現実的なので、ソースを scan してすべての class/struct 定義を抽出し、
ルールベースで F/T prefix を付与する。

ルール:
  ・`template<...> class X` / `struct X` -> T prefix
  ・通常の `class X` / `struct X` -> F prefix
  ・already-prefixed (I/E/T/F + 大文字) は skip
  ・lowercase 1 字 alias (u8/u32/i32 等) は skip
  ・known external (Diligent / ImGui / D3D12 / std) は skip
  ・既知 macro 名 / 全大文字 (3 字以上) は skip
  ・single-letter は skip
  ・以下の hand exclude list は skip

スコープ:
  src/**, samples/**, tests/**, tools/**, docs/**.md
  cmake-build-*/_deps/ は除外。
"""
from __future__ import annotations
import argparse
import os
import re
import sys
from pathlib import Path

# ハンド除外リスト (Diligent / Win32 / std / ImGui / DXMath / 既に Tier 1/2 で処理済 / 自前で混乱の元)
EXCLUDE_NAMES = {
    # std / OS / Win32
    "ifstream", "ofstream", "fstream", "stringstream",
    "atomic_flag", "true_type", "false_type", "pair", "tuple",
    # Diligent
    "RefCntAutoPtr", "Uint64", "Uint32", "Uint16", "Uint8",
    # ImGui (Im* prefix anyway)
    # Tier 1
    "Vec2","Vec3","Vec4","Mat3","Mat4","Quat","Aabb","Plane","Ray","Sphere","Frustum",
    "ErrorCode","String","StringView","Array","UniquePtr","Rc","WeakRc",
    "HashMap","HashSet","Span","Pool","Result",
    # Tier 2
    "NumLimits","Hasher","BoolConstant","FalseType","TrueType",
    "IsSame","IsLvalueRef","IsRvalueRef","IsIntegralImpl","IsIntegral",
    "IsFloatingImpl","IsFloating","IsPointer","IsTriviallyCopyable",
    "EnableIf","Conditional","RemoveRef","RemoveConst","RemoveCV","RemoveCVRef",
    "DefaultConverter","OkTag","ErrTag","SourceLoc","LogConfig",
    "Atomic","Callback","Thread","Mutex","ScopedLock","JobGraph",
    "AssetId","AssetHandle","NodeId","ShapeId","EmitterHandle","HealthId","ProjectileId",
    "Animation","AnimationKey","AnimationChannel","Bone","SkinnedVertex",
    "Viewport","ScissorRect","Color","Rgba8","DirLight",
    "BufferDesc","TextureDesc","ShaderDesc","PipelineDesc","BlendDesc",
    "DepthStencilDesc","RasterizerDesc","InputElementDesc","InputLayoutDesc",
    "Camera","Transform2D",
    # Macro / 全大文字
    "ACS_CONCAT","ACS_ASSERT","ACS_FORCEINLINE",
    # 既に F/T/I/E プレフィックス済 (auto detect が誤検出しないように)
    # マクロ・典型誤検出
    "ALIGNED", "PACKED",
    # 名前空間 (なぜか class/struct パターンで検出されたら除外)
    "acs", "detail",
}

# 「template <...> class/struct Foo」検出パターン (block 開始の宣言)
RE_TEMPLATE_DECL = re.compile(
    r"^\s*template\s*<[^>]*>\s*(?:class|struct)\s+([A-Z][A-Za-z0-9_]*)"
)
# 通常の class/struct 宣言
RE_NORMAL_DECL = re.compile(
    r"^\s*(?:class|struct)\s+([A-Z][A-Za-z0-9_]*)"
)

TARGET_EXTS = {".h", ".hpp", ".inl", ".cpp", ".cxx", ".cc", ".c", ".md", ".cmake", ".txt"}
EXCLUDE_DIRS = {
    "cmake-build-diligent-debug","cmake-build-diligent-release",
    "cmake-build-debug","cmake-build-release","build","_deps","out",
    ".git",".vs",".idea","node_modules",
}
INCLUDE_DIRS_WHITELIST = {"src","samples","tests","docs","tools","cmake","scripts"}

_INCLUDE_LINE = re.compile(r"^\s*#\s*include\b")


def discover_types(repo_root: Path) -> tuple[set[str], set[str]]:
    """src/ 内の class/struct 定義を抽出。
    Returns (templates, normal_classes)。"""
    templates: set[str] = set()
    normals: set[str] = set()
    src_dir = repo_root / "src"
    for dp, dns, fns in os.walk(src_dir):
        dns[:] = [d for d in dns if d not in EXCLUDE_DIRS]
        for fn in fns:
            if not fn.endswith((".h", ".hpp", ".inl")):
                continue
            p = Path(dp) / fn
            try:
                text = p.read_text(encoding="utf-8")
            except Exception:
                continue
            for line in text.splitlines():
                m = RE_TEMPLATE_DECL.match(line)
                if m:
                    templates.add(m.group(1))
                    continue
                m = RE_NORMAL_DECL.match(line)
                if m:
                    name = m.group(1)
                    if name not in templates:
                        normals.add(name)
    return templates, normals


def is_already_prefixed(name: str) -> bool:
    """既に I/E/F/T prefix + 大文字続きなら skip。"""
    if len(name) < 2:
        return True
    if name[0] in "IETF" and name[1].isupper():
        return True
    return False


def build_mapping(templates: set[str], normals: set[str]) -> dict[str, str]:
    mapping: dict[str, str] = {}
    for name in templates:
        if name in EXCLUDE_NAMES:
            continue
        if is_already_prefixed(name):
            continue
        mapping[name] = "T" + name
    for name in normals:
        if name in EXCLUDE_NAMES:
            continue
        if is_already_prefixed(name):
            continue
        mapping[name] = "F" + name
    return mapping


def is_excluded(path: Path, repo_root: Path) -> bool:
    rel = path.relative_to(repo_root)
    for p in rel.parts:
        if p in EXCLUDE_DIRS:
            return True
    return False


def collect_files(repo_root: Path) -> list[Path]:
    files: list[Path] = []
    for top in INCLUDE_DIRS_WHITELIST:
        top_path = repo_root / top
        if not top_path.exists():
            continue
        for dp, dns, fns in os.walk(top_path):
            dns[:] = [d for d in dns if d not in EXCLUDE_DIRS]
            for fn in fns:
                p = Path(dp) / fn
                if p.suffix.lower() in TARGET_EXTS and not is_excluded(p, repo_root):
                    files.append(p)
    for top_file in ["CMakeLists.txt","README.md","ONBOARDING.md"]:
        p = repo_root / top_file
        if p.exists():
            files.append(p)
    return files


def build_patterns(mapping: dict[str, str]) -> list[tuple[re.Pattern, str]]:
    patterns: list[tuple[re.Pattern, str]] = []
    # 長い名前優先 (sub-string 衝突防止)
    for old in sorted(mapping.keys(), key=len, reverse=True):
        new = mapping[old]
        pat = re.compile(rf"(?<![\w.>])\b{re.escape(old)}\b(?![\w])")
        patterns.append((pat, new))
    return patterns


def transform_text(text: str, patterns: list[tuple[re.Pattern, str]]) -> tuple[str, int]:
    total = 0
    out_lines: list[str] = []
    for line in text.splitlines(keepends=True):
        if _INCLUDE_LINE.match(line):
            out_lines.append(line)
            continue
        new_line = line
        for pat, new in patterns:
            new_line, n = pat.subn(new, new_line)
            total += n
        out_lines.append(new_line)
    return "".join(out_lines), total


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--print-mapping", action="store_true",
                    help="検出した mapping を表示して終了")
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    print(f"[tier3] repo root = {repo_root}")

    templates, normals = discover_types(repo_root)
    mapping = build_mapping(templates, normals)
    print(f"[tier3] discovered: {len(templates)} templates, {len(normals)} classes")
    print(f"[tier3] mapping size: {len(mapping)} types")

    if args.print_mapping:
        for old, new in sorted(mapping.items()):
            print(f"  {old:35s} -> {new}")
        return 0

    files = collect_files(repo_root)
    print(f"[tier3] scanning {len(files)} files")

    patterns = build_patterns(mapping)

    total_files = 0
    total_n = 0
    per_file: list[tuple[Path, int]] = []
    for f in files:
        try:
            text = f.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        new_text, n = transform_text(text, patterns)
        if n > 0:
            total_files += 1
            total_n += n
            per_file.append((f, n))
            if not args.dry_run:
                f.write_text(new_text, encoding="utf-8")

    per_file.sort(key=lambda x: -x[1])
    print(f"\n[tier3] top 30 files:")
    for p, n in per_file[:30]:
        rel = p.relative_to(repo_root)
        print(f"  {n:5d}  {rel}")
    print(f"\n[tier3] {total_n} replacements / {total_files} files")
    if args.dry_run:
        print("[tier3] (dry run)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
