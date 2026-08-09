# SPDX-License-Identifier: Apache-2.0
"""
ACS v1 -> v2 命名規則移行: Tier 2.

対象 (Tier 1 で残った foundation traits / handles / render desc / math class
全体):
  - foundation type traits (template): IsSame, RemoveRef, ... -> T prefix
  - foundation POD: OkTag, ErrTag, SourceLoc, LogConfig -> F prefix
  - container template: Hasher, NumLimits -> T prefix
  - threading: Atomic, Thread, Mutex, ScopedLock, JobGraph -> T/F prefix
  - render desc POD: Viewport, ScissorRect, Color, Rgba8, BufferDesc, ... -> F prefix
  - handle types: AssetId, NodeId, ShapeId, EmitterHandle, ... -> F prefix
  - math class: Camera, Transform2D, ... -> F prefix
  - misc: DefaultConverter (mvvm), Callback -> F/T prefix

スコープと include skip は Tier 1 と同じ。
"""
from __future__ import annotations
import argparse
import os
import re
import sys
from pathlib import Path

# (old_name, new_name) - Tier 2
TIER2 = [
    # foundation type traits (template -> T)
    ("NumLimits",              "TNumLimits"),
    ("Hasher",                 "THasher"),
    ("BoolConstant",           "TBoolConstant"),
    ("FalseType",              "TFalseType"),    # typedef BoolConstant<false>
    ("TrueType",               "TTrueType"),     # typedef BoolConstant<true>
    ("IsSame",                 "TIsSame"),
    ("IsLvalueRef",            "TIsLvalueRef"),
    ("IsRvalueRef",            "TIsRvalueRef"),
    ("IsIntegralImpl",         "TIsIntegralImpl"),
    ("IsIntegral",             "TIsIntegral"),
    ("IsFloatingImpl",         "TIsFloatingImpl"),
    ("IsFloating",             "TIsFloating"),
    ("IsPointer",              "TIsPointer"),
    ("IsTriviallyCopyable",    "TIsTriviallyCopyable"),
    ("EnableIf",               "TEnableIf"),
    ("Conditional",            "TConditional"),
    ("RemoveRef",              "TRemoveRef"),
    ("RemoveConst",            "TRemoveConst"),
    ("RemoveCV",               "TRemoveCV"),
    ("RemoveCVRef",            "TRemoveCVRef"),
    ("DefaultConverter",       "TDefaultConverter"),
    # foundation POD (-> F)
    ("OkTag",                  "FOkTag"),
    ("ErrTag",                 "FErrTag"),
    ("SourceLoc",              "FSourceLoc"),
    ("LogConfig",              "FLogConfig"),
    # atomic / callback
    ("Atomic",                 "TAtomic"),       # template
    ("Callback",               "FCallback"),     # using = void(*)(...,void*)
    # threading
    ("Thread",                 "FThread"),
    ("Mutex",                  "FMutex"),
    ("ScopedLock",             "FScopedLock"),
    ("JobGraph",               "FJobGraph"),
    # asset handle / id
    ("AssetId",                "FAssetId"),
    ("AssetHandle",            "FAssetHandle"),
    ("NodeId",                 "FNodeId"),
    ("ShapeId",                "FShapeId"),
    ("EmitterHandle",          "FEmitterHandle"),
    ("HealthId",               "FHealthId"),
    ("ProjectileId",           "FProjectileId"),
    # asset POD (mesh / skinned)
    ("AnimationKey",           "FAnimationKey"),
    ("AnimationChannel",       "FAnimationChannel"),
    ("Animation",              "FAnimation"),
    ("Bone",                   "FBone"),
    ("SkinnedVertex",          "FSkinnedVertex"),
    # render desc
    ("Viewport",               "FViewport"),
    ("ScissorRect",            "FScissorRect"),
    ("Color",                  "FColor"),
    ("Rgba8",                  "FRgba8"),
    ("DirLight",               "FDirLight"),
    ("BufferDesc",             "FBufferDesc"),
    ("TextureDesc",            "FTextureDesc"),
    ("ShaderDesc",             "FShaderDesc"),
    ("PipelineDesc",           "FPipelineDesc"),
    ("BlendDesc",              "FBlendDesc"),
    ("DepthStencilDesc",       "FDepthStencilDesc"),
    ("RasterizerDesc",         "FRasterizerDesc"),
    ("InputElementDesc",       "FInputElementDesc"),
    ("InputLayoutDesc",        "FInputLayoutDesc"),
    # math class
    ("Camera",                 "FCamera"),
    ("Transform2D",            "FTransform2D"),
]

TARGET_EXTS = {".h", ".hpp", ".inl", ".cpp", ".cxx", ".cc", ".c", ".md", ".cmake", ".txt"}
EXCLUDE_DIRS = {
    "cmake-build-diligent-debug",
    "cmake-build-diligent-release",
    "cmake-build-debug",
    "cmake-build-release",
    "build",
    "_deps",
    "out",
    ".git",
    ".vs",
    ".idea",
    "node_modules",
}
INCLUDE_DIRS_WHITELIST = {"src", "tests", "docs", "tools", "cmake", "scripts"}

_INCLUDE_LINE = re.compile(r"^\s*#\s*include\b")


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
        for dirpath, dirnames, filenames in os.walk(top_path):
            dirnames[:] = [d for d in dirnames if d not in EXCLUDE_DIRS]
            for fn in filenames:
                p = Path(dirpath) / fn
                if p.suffix.lower() in TARGET_EXTS:
                    if not is_excluded(p, repo_root):
                        files.append(p)
    for top_file in ["CMakeLists.txt", "README.md", "ONBOARDING.md"]:
        p = repo_root / top_file
        if p.exists():
            files.append(p)
    return files


def build_patterns(mapping: list[tuple[str, str]]) -> list[tuple[re.Pattern, str]]:
    patterns: list[tuple[re.Pattern, str]] = []
    for old, new in mapping:
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
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    print(f"[tier2] repo root = {repo_root}")
    print(f"[tier2] mapping = {len(TIER2)} types")

    files = collect_files(repo_root)
    print(f"[tier2] scanning {len(files)} files")

    patterns = build_patterns(TIER2)

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
    print(f"\n[tier2] top 30 files:")
    for p, n in per_file[:30]:
        rel = p.relative_to(repo_root)
        print(f"  {n:5d}  {rel}")

    print(f"\n[tier2] {total_n} replacements / {total_files} files")
    if args.dry_run:
        print("[tier2] (dry run)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
