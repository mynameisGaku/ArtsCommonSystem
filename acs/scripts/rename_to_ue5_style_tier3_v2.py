# SPDX-License-Identifier: Apache-2.0
"""
Tier 3 v2: curated class rename (Tier 3 auto-discovery 失敗を踏まえた conservative 版)。

絶対に generic 名前を入れない。suffix/prefix で識別できる ACS 特有 class のみ対象。

EXCLUDE 方針:
  - 1 単語の generic 名前 (Container, Vertex, Storage, Frame, Line, State, World,
    Color, Camera, Asset, Game, Scene, Node, Application, Window, Renderer)
  - ImGui と衝突する Widget 名 (Button, Slider, Checkbox, Label, TextInput, Font)
  - enum 値や method 名と被りやすい単語

filename は v1 維持方針 (Tier 1/2 と同じ)。
"""
from __future__ import annotations
import argparse
import os
import re
import sys
from pathlib import Path

# ACS 固有・絶対安全な class 名のみ (suffix/prefix で識別可能)
TIER3_V2 = [
    # ----- Acpak (file pack 系) -----
    "AcpakReader", "AcpakWriter", "AcpakCrypto", "AcpakLz4",
    "AcpakKey", "AcpakHeader", "AcpakFileEntry",

    # ----- *Asset (loaders + types) -----
    "MeshAsset", "BinaryAsset", "TextAsset", "AudioAsset", "ImageAsset",
    "SkinnedMeshAsset",
    "BinaryAssetLoader", "TextAssetLoader",
    "AssetRegistry", "AssetFuture", "AssetBundle", "AssetBrowser",

    # ----- Render 専用 (specific name、3rd party と衝突なし) -----
    "Atmosphere", "Sky", "Ibl", "PostProcess", "PbrShader", "ShadowMap",
    "Ssr", "Ssao", "Ssgi", "MotionVector", "RefractionShader", "Blit",
    "HiZ", "StandardShader", "SkinnedShader", "SpriteBatch", "Particles",
    "RenderAssets", "AnimationPlayer",

    # ----- ECS (specific) -----
    "ComponentRegistry",

    # ----- Audio (Audio* prefix で識別) -----
    "AudioEngine", "AudioListener", "AudioSource3D",
    "XAudio2Backend",

    # ----- GameFramework types (specific) -----
    "BeatGrid", "BeatNote",
    "AnimationGraph", "AnimationCurve", "AnimationStateNode", "AnimationTransition",
    "AnimationClipInfo", "AnimationClipBinding",
    "NetSnapshot", "ScriptHost",
    "BehaviorTree", "BtAction", "BtNode", "BtSelector", "BtSequence",
    "Tilemap", "TriggerWorld2D",
    "CollisionWorld2D", "PhysicsBody2D",
    "ParticleEffectSystem",

    # ----- *Director (suffix で識別) -----
    "AudioDirector", "MusicDirector", "AmbientDirector",
    "LocalizationDirector", "CinematicsDirector", "PauseDirector",
    "PrivacyDirector", "StreamingDirector", "TelemetryDirector",
    "EconomyDirector", "ReplayDirector",

    # ----- *System (suffix で識別、game domain) -----
    "HealthSystem", "InventorySystem", "BuffSystem", "ScoreSystem",
    "PickupSystem", "WeaponSystem", "ProjectileSystem", "EffectSystem",
    "WaveSpawner", "DeckSystem", "CraftingSystem", "MatchGrid",
    "HungerSystem", "CheckpointSystem",
    "WeatherSystem", "DialogueSystem", "PrefabSystem", "DamageFeedback",

    # ----- *Manager (suffix) -----
    "AchievementManager", "TimerManager", "TweenManager", "TurnManager",

    # ----- Pool / Snapshot / Cache / Curve -----
    "NodePool", "ThreadPool", "MemorySystem", "MemorySnapshot",
    "TlsfAllocator", "LinearAllocator", "PoolAllocator",
    "ArenaAllocator", "SystemAllocator", "Allocator",

    # ----- Tween / Sequence / StateMachine -----
    "TweenHandle", "TimerHandle",
    "SequenceRunner", "SceneClock", "Sequence", "Tween",
    "StateMachine",

    # ----- Editor (suffix で識別) -----
    "EditorPanel", "EditorWorkspace", "EditorCamera", "EditorCameraState",
    "EditorTheme", "EditorCommand", "EditorGizmo",
    "PropertyDrawer", "UndoStack", "InspectorPanel", "HierarchyPanel",
    "EditorToolbar", "SelectionService",
    "ParticleEditorPanel", "ParticleEditorPreview",
    "ModelViewerPanel", "ModelInspectorPanel", "ModelMaterialPanel",
    "ModelAnimationPanel",
    "AnimCurveEditorPanel", "BehaviorTreeEditorPanel",
    "LevelEditorPanel", "SpriteAtlasEditorPanel",
    "FontEditorPanel", "CinematicsTimelineEditorPanel",

    # ----- *Bridge / *Stub (外部 SDK seam) -----
    "SteamworksBridge", "WorkshopBridge", "OpenXrBridge",
    "AssetLockingStub", "BuildFarmStub", "VoiceChatBackendStub",
    "UpscalerStub", "AssetPackReaderStub", "AssetPackWriterStub",

    # ----- *Profile / *Settings / *Flow -----
    "AccessibilityProfile", "AccessibilitySettings",
    "GameFlow", "TutorialFlow", "LlmSafetyPipeline",
    "BackendClient", "MlRuntime", "PartySystem",
    "DevConsole", "DebugDraw", "DebugOverlay",
    "ContentModerator", "SocialModeration",

    # ----- Save / Persistence -----
    "SaveSlot", "SaveArchive", "AppStateSlot",

    # ----- Misc systems -----
    "FadeTransition", "SceneEventBus", "SceneTimer", "SceneCommandQueue",
    "SceneServices", "SceneManager",
    "Random", "PhotoMode", "SpriteAnimator",
    "ModRegistry", "Settings", "InputRecorder",
    "Perception", "SeasonPass", "VoiceChat",
    "PauseDirector", "HotReload", "PerfBudget",
    "SpatialAudio", "CameraShakePresets", "DialogueLocalizer",
    "DialogueScript",
    "WaterVolume", "CrashReporter",
    "DynamicDifficulty", "SpritePack", "CharacterCustomizer",
    "CombatStateMachine", "CooldownTimer",
    "DungeonGenerator", "LapTimer",
    "Entitlement", "Progression", "Lockstep",
    "StudioWorkflow",
    "FxeditSerializer", "InspectorSeam",

    # ----- Input -----
    "InputMap",

    # ----- 2D suffix (specific) -----
    "Node2D", "Component2D", "Camera2D", "CameraStack",

    # ----- UI (Widget は除外、特定 name のみ) -----
    "UiLayer", "UiRect", "UiPadding", "UiColors", "UiInput", "UiRenderer",

    # ----- MVVM (specific) -----
    "ViewModel", "TwoWayBinder", "ArrayObserverHandle",

    # ----- Asset Pack -----
    "AssetPack", "AssetPackInfo",

    # ----- Backend errors / handles -----
    "BackendError", "ChunkId", "ChunkInfo",

    # ----- Bucket / Block (specific impl details) -----
    "BlockHeader", "BlockEntry",

    # ----- Misc support -----
    "TypeInfo", "TypeInfoBase",
    "TimelineKeyframe",

    # ----- ID / Handle types (specific) -----
    "TriggerId", "TriggerSlot",
    "WaveDef", "WaveEntry",
    "CardDef", "CardId",
    "BuffDef", "BuffInstance", "BuffOwnerId",
    "WeaponDef", "WeaponState",
    "TileId",
    "AchievementDef", "AchievementProgress",
    "CategoryFilter",
    "TurnSideId", "TurnSideState",
    "WaterVolumeId", "WaterVolumeInfo",
    "AssetLockInfo",
    "TutorialStep", "BgmSlot",

    # ----- App-level -----
    "Application", "AppConfig",  # 単語型だが ACS-app 文脈で安全
    "Sample",  # acs::Sample (テスト用)、method 名と被らない
    "Window", "WindowConfig",  # window は ACS::Window のみ、Win32 HWND は別物
    "Renderer",  # ACS::Renderer (Diligent / Dx12 wrapper、ImGui::Render 等の method とは別)

    # ----- Game (Scene 配下) -----
    "Game",
    # Scene 単体は除外 (method っぽい名前)

    # ----- Foundation utilities -----
    "Logger", "Panic",

    # ----- DLL / OS -----
    "UdpSocket",
]

# 既知の generic 名前 (絶対に rename しない)
EXCLUDE_NAMES = {
    "Container", "Vertex", "Storage", "Frame", "Line",
    "State", "World", "Color", "Asset", "Scene", "Node",
    "Button", "Slider", "Checkbox", "Label", "TextInput", "Font",
    "Camera", "Image", "Mesh", "Texture", "Shader", "Buffer",
}

# Only rewrite C/C++ source. CMake/docs/file paths stay in v1 names because
# filenames and module names are intentionally not renamed.
TARGET_EXTS = {".h", ".hpp", ".inl", ".cpp", ".cxx", ".cc", ".c"}
EXCLUDE_DIRS = {
    "cmake-build-diligent-debug","cmake-build-diligent-release",
    "cmake-build-debug","cmake-build-release","build","_deps","out",
    ".git",".vs",".idea","node_modules",
}
INCLUDE_DIRS_WHITELIST = {"src","tests","tools"}

_INCLUDE_LINE = re.compile(r"^\s*#\s*include\b")

# 3rd-party namespace prefix 除外 (Tier 2 の Diligent::F* 漏れを再発防止)
_THIRD_PARTY_PREFIXES = ("Diligent::", "ImGui::", "Microsoft::", "DirectX::",
                         "winrt::", "dxc::", "D3D12::", "DXGI::", "std::")

# Enum namespaces / scoped values that share names with ACS classes.
_SCOPED_VALUE_PREFIXES = ("ESvc::",)


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
    return files


def build_patterns(names: list[str]) -> list[tuple[re.Pattern, str]]:
    """
    `(?<![\w.>])` で member access (.foo, ->foo) を除外。
    `:` は除外しないので `acs::Foo` も match (Tier 1/2 と同じ方針)。
    ただし下流で 3rd-party namespace を post-filter する。
    """
    patterns: list[tuple[re.Pattern, str]] = []
    for old in sorted(set(names), key=len, reverse=True):
        if old in EXCLUDE_NAMES:
            continue
        new = "F" + old
        pat = re.compile(rf"(?<![\w.>])\b{re.escape(old)}\b(?![\w])")
        patterns.append((pat, new, old))  # type: ignore
    return patterns  # type: ignore


def transform_text(text: str, patterns: list) -> tuple[str, int]:
    total = 0
    out_lines: list[str] = []
    for line in text.splitlines(keepends=True):
        if _INCLUDE_LINE.match(line):
            out_lines.append(line)
            continue
        new_line = line
        for pat, new, old in patterns:
            # 3rd-party namespace prefix の場合は skip
            def safe_sub(m: re.Match) -> str:
                pre_start = max(0, m.start() - 20)
                pre = new_line[pre_start:m.start()]
                for tp in _THIRD_PARTY_PREFIXES:
                    if pre.endswith(tp):
                        return m.group(0)  # don't replace
                for sp in _SCOPED_VALUE_PREFIXES:
                    if pre.endswith(sp):
                        return m.group(0)
                return new
            new_line2, n = pat.subn(safe_sub, new_line)
            if n:
                total += n
                new_line = new_line2
        out_lines.append(new_line)
    return "".join(out_lines), total


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    print(f"[tier3-v2] repo root = {repo_root}")
    print(f"[tier3-v2] curated list = {len(TIER3_V2)} names")
    print(f"[tier3-v2] excludes = {len(EXCLUDE_NAMES)} generic names")

    files = collect_files(repo_root)
    print(f"[tier3-v2] scanning {len(files)} files")

    patterns = build_patterns(TIER3_V2)

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
    print(f"\n[tier3-v2] top 30 files:")
    for p, n in per_file[:30]:
        rel = p.relative_to(repo_root)
        print(f"  {n:5d}  {rel}")
    print(f"\n[tier3-v2] {total_n} replacements / {total_files} files")
    if args.dry_run:
        print("[tier3-v2] (dry run)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
