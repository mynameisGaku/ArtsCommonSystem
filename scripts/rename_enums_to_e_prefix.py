#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# ACS Phase 19a: enum class XxxYyy -> EXxxYyy 一括 rename
#
# 使い方:
#   python scripts/rename_enums_to_e_prefix.py --dry-run    # 影響範囲確認
#   python scripts/rename_enums_to_e_prefix.py              # 実適用
#
# 設計:
# - word boundary `\b<Name>\b` で partial match を完全回避
#   - `ShakePreset` を rename しても `Preset` の rename には引っかからない
#   - `MyKeyClass` の中の `Key` は引っかからない (between letters 境界なし)
# - .h / .cpp / .md / .hlsl / CMakeLists.txt 対象
# - 既に E prefix の enum は除外
# - DXLib / Diligent / Imgui 等の third_party は除外

import os
import re
import sys
from pathlib import Path

ROOT = Path(r"C:\Users\g0190\OneDrive\Desktop\acs_github\acs")

# Rename 対象 enum 一覧 (`enum class <Name>` で grep 抽出した 80 個)
ENUM_NAMES = [
    "AllocKind", "ArrayChange", "AssetState", "AttenuationCurve",
    "BeatLane", "BlendMode", "BtStatus", "BuffKind", "BuffStackPolicy",
    "BufferUsage", "ChunkState", "ColorblindMode", "CombatState",
    "ConsentCategory", "ContentRating", "CosmeticSlot", "CraftStatus",
    "CullMode", "CurveInterpolation", "DamageType", "DialogueScriptState",
    "DifficultyLevel", "FadeKind", "FadePhase", "FieldKind", "FlowState",
    "Format", "GamepadAxis", "GamepadButton", "ItemCategory", "Judgement",
    "Key", "Locale", "LogSeverity", "MatchStatus", "MemoryOrder",
    "ModerationVerdict", "MotionReduction", "MouseButton", "MusicState",
    "NetMode", "PartyState", "PauseReason", "PickupKind", "PixelFormat",
    "PlayMode", "Preset", "PrimitiveTopology", "ProjectileKind",
    "RecorderMode", "ReplayMode", "ReportCategory", "ResourceState",
    "RhiBackendKind", "SafetyRule", "SafetyVerdict", "SampleFormat",
    "SamplerAddress", "SamplerFilter", "ScriptOpKind", "SeasonStatus",
    "Segment", "SettingKind", "ShaderStage", "ShakePreset", "SpecialKind",
    "StackDir", "SurvivalStat", "Svc", "TextSize", "TileKind",
    "TimelineTrackKind", "TurnPhase", "UpscalerKind", "VoiceChannel",
    "VoiceProvider", "WaveState", "WeaponKind", "WeatherKind", "WidgetKind",
    "XrPlatform",
]

# ファイル拡張子フィルタ
TARGET_EXTS = {".h", ".cpp", ".hpp", ".cc", ".hlsl", ".md", ".cmake", ".txt"}

# 除外パス
EXCLUDE_DIRS = {
    "third_party", "build", "cmake-build-debug", "cmake-build-release",
    "cmake-build-diligent-debug", "cmake-build-diligent-release",
    ".git", "__pycache__", ".idea", ".vs",
}

def should_process(path: Path) -> bool:
    # 除外ディレクトリ
    for part in path.parts:
        if part in EXCLUDE_DIRS:
            return False
    return path.suffix.lower() in TARGET_EXTS or path.name == "CMakeLists.txt"

def build_regex():
    # 単一 regex で全 enum を一括置換 (alternation)
    # 長い名前から先にマッチさせる (ShakePreset を Preset より先に)
    sorted_names = sorted(ENUM_NAMES, key=len, reverse=True)
    pattern = r"\b(" + "|".join(re.escape(n) for n in sorted_names) + r")\b"
    return re.compile(pattern)

def main():
    dry_run = "--dry-run" in sys.argv
    regex = build_regex()
    total_files = 0
    changed_files = 0
    total_replacements = 0
    per_enum_count = {n: 0 for n in ENUM_NAMES}

    for root, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
        for fname in files:
            path = Path(root) / fname
            if not should_process(path):
                continue
            total_files += 1
            try:
                content = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            new_content, n = regex.subn(lambda m: "E" + m.group(1), content)
            if n > 0:
                # 各 enum がいくつヒットしたかカウント
                for m in regex.finditer(content):
                    per_enum_count[m.group(1)] += 1
                changed_files += 1
                total_replacements += n
                if not dry_run:
                    path.write_text(new_content, encoding="utf-8")
                print(f"  {path.relative_to(ROOT)}: {n} replacements")

    print()
    print(f"=== Summary ===")
    print(f"Files scanned:    {total_files}")
    print(f"Files changed:    {changed_files}")
    print(f"Total replaces:   {total_replacements}")
    if dry_run:
        print(f"(DRY RUN - no files written)")
    print()
    print(f"=== Per-enum counts (top 30) ===")
    sorted_counts = sorted(per_enum_count.items(), key=lambda x: x[1], reverse=True)
    for name, count in sorted_counts[:30]:
        if count > 0:
            print(f"  {name:30s} -> E{name:30s} {count} hit")

if __name__ == "__main__":
    main()
