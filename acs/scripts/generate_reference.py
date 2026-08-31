#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ACSのC++宣言を機能・APIリファレンス用に解析する。

このモジュールは宣言抽出の互換入口も提供する。サイト生成は
``generate_reference_site.py`` が担当する。
"""

from __future__ import annotations

import html
import json
import re
from collections import Counter, OrderedDict, defaultdict
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Iterable


ACS_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ACS_ROOT.parent
SRC_ROOT = ACS_ROOT / "src"
SAMPLES_ROOT = ACS_ROOT / "samples"
DOCS_ROOT = ACS_ROOT / "docs"
ENGINE_ROOT = ACS_ROOT / "engine"


MODULE_INFO: "OrderedDict[str, dict[str, str]]" = OrderedDict([
    ("foundation", {
        "title": "Foundation",
        "summary": "基本型、エラー処理、Result、ログ、アサート、panic、source location。",
        "use": "全モジュールの土台。例外を使わず失敗を返す ACS の流儀はここから始まる。",
    }),
    ("threading", {
        "title": "Threading",
        "summary": "TAtomic、FMutex、FRwLock、CThread、CThreadPool、CJobGraph。",
        "use": "ロード、ジョブ分割、非同期処理、エンジン内部の同期に使う。",
    }),
    ("memory", {
        "title": "Memory",
        "summary": "FAllocator 群、TUniquePtr、TRc、仮想メモリ、FMemorySnapshot。",
        "use": "所有権と寿命を明示し、STL なしでメモリ管理するための層。",
    }),
    ("container", {
        "title": "Container",
        "summary": "TArray、FString、FStringView、TSpan、THashMap、JSON、Hash。",
        "use": "ACS 標準コンテナ。std::vector/std::string の代替として使う。",
    }),
    ("math", {
        "title": "Math",
        "summary": "FVec、FMat4、FQuat、FCamera、2D/3D collision、CPU dispatch。",
        "use": "座標、変換、カメラ、当たり判定、SIMD 実行時分岐を扱う。",
    }),
    ("test", {
        "title": "Test",
        "summary": "小さなテストフレームワークと EXPECT 系マクロ。",
        "use": "エンジン規約に沿った no-STL/no-exception の単体テストを書く。",
    }),
    ("platform", {
        "title": "Platform",
        "summary": "CWindow、CInput、CClock/CFrameTimer、CFileSystem、CStorage、FLocalization。",
        "use": "OS と直接接する層。ゲームコードは基本的にこの API 経由で触る。",
    }),
    ("ecs", {
        "title": "ECS",
        "summary": "FWorld、FEntityId、TSparseSet、TQueryView、FSystemScheduler。",
        "use": "大量の同種オブジェクトをデータ指向に処理する。",
    }),
    ("event", {
        "title": "Event",
        "summary": "FMessageBroker、TMessagePipe、FTimerManager。",
        "use": "pub/sub、時間差実行、フレームをまたぐ通知に使う。",
    }),
    ("asset", {
        "title": "Asset",
        "summary": "FAssetRegistry、FAssetFuture、画像・音声・テキスト・メッシュ読み込み。",
        "use": "ファイルからゲーム内リソースへ変換し、同期/非同期ロードする。",
    }),
    ("assetpack", {
        "title": "AssetPack",
        "summary": ".acpak の reader/writer、圧縮、CRC、GameFramework bridge。",
        "use": "出荷時にアセットを 1 ファイルへまとめる。暗号化ではなく実用的な難読化として扱う。",
    }),
    ("collision", {
        "title": "Collision",
        "summary": "スプライト alpha、3D メッシュ、凸包から collider を生成する CPU ジオメトリ処理。",
        "use": "素材から実用的な当たり判定形状を作る。",
    }),
    ("render", {
        "title": "Render",
        "summary": "FRenderer、RHI、CSpriteBatch、CFont、CStandardShader/CPbrShader、post process、IBL。",
        "use": "2D/3D 描画、低レベル GPU リソース、高レベル描画ヘルパを提供する。",
    }),
    ("app", {
        "title": "App",
        "summary": "FApplication、FAppConfig、ACS_DEFINE_MAIN。",
        "use": "ウィンドウ・入力・レンダラ・ECS をまとめた最小ゲームループを書く。",
    }),
    ("audio", {
        "title": "Audio",
        "summary": "FAudioEngine、FSoundHandle。",
        "use": "WAV/MP3 などを読み、効果音/BGM として再生する。",
    }),
    ("network", {
        "title": "Network",
        "summary": "TCP/UDP、listener、connection、IP address。",
        "use": "軽量な通信サンプルや独自プロトコルの土台にする。",
    }),
    ("imgui", {
        "title": "ImGui",
        "summary": "Dear ImGui 統合コンテキスト。",
        "use": "デバッグ UI、エディタ、ツールパネルを組み込む。",
    }),
    ("mvvm", {
        "title": "MVVM",
        "summary": "TObservable、FCommand、TOneWayBinder/TTwoWayBinder、FViewModel、ImGui bindings。",
        "use": "UI とゲーム状態をデータバインディングで接続する。",
    }),
    ("ui", {
        "title": "UI",
        "summary": "FWidget、標準 widget 群、FUiRenderer。",
        "use": "純正 FWidget ツリーで HUD やメニューを構築する。",
    }),
    ("easy", {
        "title": "Easy",
        "summary": "初学者向けの関数型 2D API。クラス継承なしでゲームを書く。",
        "use": "最初の 1 本、授業、短いプロトタイプに向く。",
    }),
    ("gameframework", {
        "title": "GameFramework",
        "summary": "FScene、FGame、統一ノード ANode、AComponent、2D/3D 物理、FInputMap、FTweenManager、セーブ、制作支援機能。",
        "use": "Application の上に実ゲーム制作向けの便利な部品を載せる。",
    }),
    ("localmatch", {
        "title": "LocalMatch",
        "summary": "ローカル deterministic matchmaker backend。",
        "use": "オンライン基盤なしでマッチングのフローを検証する。",
    }),
    ("mlonnx", {
        "title": "MlOnnx",
        "summary": "ONNX Runtime CPU backend。",
        "use": "GameFramework の IMlRuntime 実装として機械学習推論を接続する。",
    }),
    ("openxr", {
        "title": "OpenXR",
        "summary": "Khronos OpenXR loader bridge。",
        "use": "XR runtime の存在確認と XR backend 接続の実装に使う。",
    }),
    ("scripting", {
        "title": "Scripting",
        "summary": "Lua VM bridge。",
        "use": "GameFramework の FScriptHost に real scripting backend を差し込む。",
    }),
    ("steamworks", {
        "title": "Steamworks",
        "summary": "Steamworks SDK bridge implementation。",
        "use": "Steam 統合を opt-in モジュールとして隔離する。",
    }),
    ("crashwin", {
        "title": "CrashWin",
        "summary": "Windows DbgHelp minidump backend。",
        "use": "クラッシュレポートや minidump 出力の real backend。",
    }),
    ("telemetryfile", {
        "title": "TelemetryFile",
        "summary": "JSON Lines file telemetry backend。",
        "use": "バックエンドなしで telemetry の送信経路を検証する。",
    }),
])


SAMPLE_DESCRIPTIONS: dict[str, str] = {
    "00_HelloEasy": "acs::easy の最小 2D ゲーム。OpenWindow と while(NextFrame()) だけで描画・入力を学ぶ。",
    "01_HelloWindow": "FApplication の lifecycle、FWindow、CInput、終了処理の最小構成。",
    "02_HelloSprite": "CSpriteBatch によるスプライト、矩形、アルファブレンド。",
    "03_HelloText": "CFont と UTF-8 テキスト描画。日本語文字列の扱いも確認できる。",
    "04_HelloECS": "FWorld/FEntityId/TQueryView、FMessageBroker、FTimerManager を使う ECS 入門。",
    "05_HelloSave": "CStorage による INI 風の設定・セーブ永続化。",
    "06_HelloLocalization": "FLocalization と多言語切替。ja/en/fr の切替例。",
    "07_HelloAudio": "FAudioEngine で WAV/MP3 を再生する。",
    "08_HelloPhysics2D": "2D の円衝突、重力、マウス発射の基礎。",
    "09_HelloParticles": "2D パーティクル表現。火、火花、噴水、煙など。",
    "10_HelloModel": "CStandardShader と手続きプリミティブで最初の 3D 描画。",
    "11_HelloRaycast3D": "3D レイキャスト、ターゲット選択、HUD 表示。",
    "12_HelloLights": "複数点光源と動的ライティング。",
    "13_HelloSky": "手続きスカイと昼/夕/夜プリセット。",
    "14_HelloShadows": "シャドウマップ、PCF、太陽方向アニメーション。",
    "15_HelloAnimation": "スキンメッシュと GPU ボーンスキニング。",
    "16_HelloTriangle": "低レベル RHI、HLSL、頂点バッファ、パイプラインの最小例。",
    "17_HelloMesh": "3D メッシュ、定数バッファ、深度テスト、カメラ。",
    "18_HelloTextured": "テクスチャ、サンプラ、UV。",
    "19_HelloUI": "ACS 純正 FWidget UI と TObservable バインディング。",
    "20_HelloMVVM": "TObservable、TOneWayBinder/TTwoWayBinder、FCommand による MVVM。",
    "21_HelloImGui": "Dear ImGui 統合とデバッグ UI。",
    "22_HelloNet": "TCP echo。Application を使わない通信の素の例。",
    "23_HelloPbr": "PBR material と metallic/roughness の変化。",
    "24_HelloBloom": "HDR、Bloom、ACES tonemap。Diligent backend 向け。",
    "25_HelloIbl": "Image-Based Lighting の統合デモ。",
    "26_HelloLightmap": "Cornell box と CPU path tracing による lightmap bake。",
    "27_HelloShowcase": "PBR、IBL、屈折、post-process をまとめた cinematic demo。",
    "28_HelloGameFramework": "FGame、FScene、FSceneManager による画面遷移。",
    "29_HelloParticleEditor": "GameFramework tools の Particle Editor。",
    "30_HelloSceneInspector": "FHierarchyPanel、FInspectorPanel、FEditorToolbar を持つ Scene Inspector。",
    "31_HelloModelViewer": "モデルビューア、マテリアル/アニメーション/インスペクタ panels。",
    "32_HelloAnimCurveEditor": "FAnimationCurve の編集 UI。",
    "33_HelloBehaviorTreeEditor": "FBehaviorTree の編集と TreeActions の action 関数群。",
    "34_HelloLevelEditor": "FLevelEditorPanel とエディタ基盤。",
    "35_HelloSpriteAtlasEditor": "FSpriteAtlasEditorPanel と FSpritePack の atlas 作成フロー。",
    "36_HelloFontEditor": "CFont editor と glyph/preview workflow。",
    "37_HelloCinematicsEditor": "FCinematicsTimelineEditorPanel。",
    "38_HelloFullGame": "タイトル、ゲームプレイ、敵、弾、HUD、セーブを含む mini full game。",
    "39_HelloSteamworks": "ISteamworksBridge の stub/real backend デモ。",
    "40_HelloScripting": "IScriptVm と Lua backend の stub/real デモ。",
    "41_HelloOnnx": "IMlRuntime と ONNX Runtime backend の smoke test。",
    "42_HelloOpenXR": "IOpenXrBridge と OpenXR loader の smoke test。",
    "43_HelloCrashReporter": "Windows minidump backend のクラッシュレポート確認。",
    "44_HelloTelemetryFile": "JSON Lines telemetry sink の確認。",
    "45_HelloLocalMatchmaker": "deterministic local matchmaker の search/match/accept/cancel。",
    "46_HelloAssetPackBridge": ".acpak reader/writer と GameFramework bridge の smoke test。",
    "47_HelloLight2D": "2D 動的ライト、ソフト影、blob shadow。",
    "48_HelloSpriteCollider": "Sprite alpha から collider を生成する headless 検証。",
    "49_HelloMeshCollider": "3D mesh から triangle BVH collider を生成する検証。",
    "50_HelloSpritePhysics": "sprite collider と FCollisionWorld2D の統合。",
    "51_HelloConvexHull": "3D convex hull 生成と convex collider。",
    "52_HelloColliderViz2D": "2D collider の可視化。",
    "53_HelloColliderViz3D": "3D mesh/convex hull collider の wireframe 可視化。",
    "54_HelloCollideSlide": "APhysicsBody2D の collide-and-slide 検証。",
    "55_HelloScene2D": "FScene2D starter foundation。",
    "56_HelloSpriteAnim": "sprite-sheet animation と HUD text。",
    "57_HelloTriggers": "collision layers/masks と trigger enter/stay/exit。",
    "58_HelloTilemap": "Tilemap render と fade scene transition。",
    "59_HelloEffects2D": "shader-free interactive effects。",
    "60_HelloStencilMask": "任意形状 stencil mask で子ツリーを clipping。",
    "61_HelloWaterTopDown": "見下ろし水面、caustics、岸泡。",
    "62_HelloPersistVerify": "永続化 round-trip 検証。",
    "63_HelloVerticalSlice": "title/play/pause/game over/save を統合した縦スライス。",
}


GLOSSARY: "OrderedDict[str, str]" = OrderedDict([
    ("TResult", "例外の代わりに成功/失敗を返す ACS の標準結果型。IsErr() を確認してから Value() を読む。"),
    ("noexcept", "ACS の公開 callback/engine API で原則付ける例外禁止の契約。例外を投げず TResult や bool で失敗を返す。"),
    ("FApplication", "最小のゲームループ基底。OnStart/OnUpdate/OnRender/OnShutdown を override する。"),
    ("FGame", "GameFramework の FApplication 派生。FScene stack、固定 timestep、FRenderContext などをまとめる。"),
    ("FScene", "1 つの画面・状態。OnEnter/OnUpdate/OnRender/OnExit を持ち FSceneManager で遷移する。"),
    ("FSceneServices", "FScene が使う FTweenManager/CInput/FCamera などを必要分だけ attach する hub。"),
    ("FWorld", "ECS の中心。FEntityId と component sparse-set を管理する。"),
    ("FEntityId", "index + generation の値型 handle。Destroy 後の stale 参照を検出する。"),
    ("TQueryView", "FWorld 内の複数 component を持つ entity を反復する API。"),
    ("CSpriteBatch", "2D スプライト・矩形・文字をまとめて GPU へ投げる描画 helper。"),
    ("RHI", "Rendering Hardware Interface。DX12/Diligent の違いを IRhiDevice などの抽象に閉じ込める。"),
    ("IRhiDevice", "GPU device 抽象。buffer/texture/shader/pipeline を作成する入口。"),
    ("FAssetRegistry", "FAssetId と loader を管理し、画像・mesh・音声・text を同期/非同期ロードする。"),
    ("AssetPack", ".acpak archive。出荷時に loose files を 1 つへまとめる仕組み。"),
    ("ANode", "GameFramework の統一 object model。親子 FTransform3D を持ち、2D/3D helper を提供する。"),
    ("AComponent", "ANode に attach する再利用可能な振る舞い。sprite/collider/trigger など。"),
    ("FInputMap", "物理キーを gameplay action 名へ束ねる GameFramework の入力 mapping。"),
    ("FTweenManager", "値を時間で補間する仕組み。UI、カメラ、演出、scene transition に使う。"),
    ("Diligent", "Diligent Engine backend。HDR、cubemap、上級 post-process で使う sample がある。"),
    ("DX12 raw", "ACS 独自の DirectX 12 backend。既定の dx12-* preset。"),
    ("generate.ps1", "ACS の推奨生成スクリプト。Visual Studio solution、Binaries、Intermediate を現行レイアウトで作る。"),
    ("acs_assetpack", ".acpak archive を pack/unpack/list/verify/info する CLI。tools/acs_assetpack に実装がある。"),
    ("FScene2D", "GameFramework の 2D starter scene。ANode root、CSpriteBatch、FCamera2D、stencil/reflection などをまとめる。"),
])


QUICK_RECIPES: list[dict[str, object]] = [
    {
        "title": "一番短い 2D ゲーム",
        "module": "easy",
        "summary": "クラス継承なしで、関数呼び出しだけで動く入口。",
        "code": r'''#include "easy/Easy.h"
using namespace acs::easy;

int main() {
    OpenWindow(1280, 720, "my first ACS game");
    float x = 600.0f;

    while (NextFrame()) {
        if (IsKeyDown(EKey::Right)) x += 240.0f * DeltaTime();
        if (IsKeyDown(EKey::Left))  x -= 240.0f * DeltaTime();
        DrawRect(x, 320, 80, 80, FColor::Sky);
        DrawString(24, 24, "Arrow keys move the box", FColor::White);
    }
}''',
        "apis": ["OpenWindow", "NextFrame", "IsKeyDown", "DeltaTime", "DrawRect"],
    },
    {
        "title": "Application の基本形",
        "module": "app",
        "summary": "本格的なサンプルはこの形。FApplication を継承して 4 つの hook を実装する。",
        "code": r'''#include "app/Application.h"
#include "app/EntryPoint.h"
#include "platform/Input.h"

class FMyGame : public acs::FApplication {
public:
    void OnStart() noexcept override {
        SetClearColor(0.05f, 0.07f, 0.11f, 1.0f);
    }

    void OnUpdate(acs::f32 dt) noexcept override {
        (void)dt;
        if (acs::CInput::IsKeyPressed(acs::EKey::Escape)) Quit();
    }

    void OnRender() noexcept override {
        // BeginFrame / EndFrame は FApplication 側が呼ぶ。
    }

    void OnShutdown() noexcept override {}
};

ACS_DEFINE_MAIN(FMyGame)''',
        "apis": ["FApplication", "ACS_DEFINE_MAIN", "CInput"],
    },
    {
        "title": "ECS で位置と速度を更新する",
        "module": "ecs",
        "summary": "大量の entity を component の組み合わせで処理する基本パターン。",
        "code": r'''struct FPosition { acs::FVec2 value; };
struct FVelocity { acs::FVec2 value; };

acs::FWorld& world = GetWorld();
acs::FEntityId player = world.Create();
world.Add<FPosition>(player, { acs::FVec2{100.0f, 200.0f} });
world.Add<FVelocity>(player, { acs::FVec2{60.0f, 0.0f} });

world.Query<FPosition, FVelocity>().Each(
    [dt](acs::FEntityId, FPosition& p, FVelocity& v) {
        p.value.x += v.value.x * dt;
        p.value.y += v.value.y * dt;
    });''',
        "apis": ["FWorld", "FEntityId", "TQueryView"],
    },
    {
        "title": "FScene 遷移を書く",
        "module": "gameframework",
        "summary": "画面単位で状態を分け、FSceneManager に遷移を依頼する。",
        "code": r'''class FTitleScene final : public acs::game::FScene {
public:
    void OnEnter() noexcept override {
        // title assets を読む。
    }

    void OnUpdate(acs::f32) noexcept override {
        if (acs::CInput::IsKeyPressed(acs::EKey::Enter)) {
            Scenes().ChangeScene(acs::MakeUnique<FGameplayScene>());
        }
    }

    void OnRender(acs::game::FRenderContext& rc) noexcept override {
        rc.Sprites().DrawString(rc.GetFont(), "PRESS ENTER", 320, 240);
    }
};''',
        "apis": ["FScene", "FSceneManager", "FRenderContext"],
    },
    {
        "title": "CSpriteBatch で 2D を描く",
        "module": "render",
        "summary": "RHI の上で 2D sprite/rect/text をまとめて描く。",
        "code": r'''acs::CSpriteBatch sprites;
ACS_TRY(sprites.Init(*renderer.Device(), renderer.ColorFormat(), 4096));

sprites.Begin(*renderer.CommandList(), window.Width(), window.Height());
sprites.DrawRect(32, 32, 160, 48, acs::FVec4{0.1f, 0.2f, 0.4f, 0.9f});
sprites.Draw(texture, 240, 120, 64, 64);
sprites.DrawString(font, "score: 1000", 32, 96, acs::FVec4{1, 1, 1, 1});
sprites.End();''',
        "apis": ["CSpriteBatch", "IRhiCommandList", "CFont"],
    },
    {
        "title": "AssetPack bridge の考え方",
        "module": "assetpack",
        "summary": "開発時は loose file、出荷時は .acpak を mount する。ゲーム側ロードコードは同じに保つ。",
        "code": r'''acs::assetpack::CAcpakGameReader reader;
ACS_TRY(reader.Mount("game.acpak"));

auto size = reader.FileSize("textures/player.png");
if (size.IsErr()) {
    ACS_LOG_ERROR("asset missing: textures/player.png");
    return;
}

acs::TArray<acs::u8> bytes;
bytes.Resize(static_cast<acs::usize>(size.Value()));
ACS_TRY(reader.ReadFile("textures/player.png", bytes.Data(), size.Value()));''',
        "apis": ["AssetPack", "CAcpakGameReader", "IAssetPackReader"],
    },
]


@dataclass
class MemberInfo:
    access: str
    signature: str
    doc: str = ""
    line: int = 0
    owner_path: tuple[str, ...] = ()
    name: str = ""


@dataclass
class EnumValueInfo:
    name: str
    signature: str
    doc: str = ""
    line: int = 0


@dataclass
class ApiDecl:
    kind: str
    name: str
    module: str
    path: Path
    line: int
    namespace: str = ""
    access: str = "public"
    doc: str = ""
    members: list[MemberInfo] = field(default_factory=list)
    functions: list[MemberInfo] = field(default_factory=list)
    values: list[str] = field(default_factory=list)
    value_infos: list[EnumValueInfo] = field(default_factory=list)
    anchor: str = ""
    signature: str = ""
    owner_signature: str = ""
    owner_qualified_name: str = ""
    is_definition: bool = False


@dataclass
class HeaderInfo:
    module: str
    path: Path
    declarations: list[ApiDecl] = field(default_factory=list)


def esc(value: object) -> str:
    return html.escape(str(value), quote=True)


def rel(path: Path) -> str:
    return path.relative_to(ACS_ROOT).as_posix()


def rel_from_docs(path: Path) -> str:
    return "../" + path.relative_to(ACS_ROOT).as_posix()


def slug(text: str) -> str:
    out = re.sub(r"[^A-Za-z0-9_.:-]+", "-", text).strip("-")
    return out or "item"


def normalize_ws(text: str) -> str:
    return re.sub(r"\s+", " ", text.strip())


def clean_comment_line(line: str) -> str:
    line = line.strip()
    line = re.sub(r"^/{2,3}\s?", "", line)
    line = re.sub(r"^/\*+\s?", "", line)
    line = re.sub(r"^\*\s?", "", line)
    line = re.sub(r"\*/$", "", line)
    line = re.sub(r"^@(?:brief|details)\b\s*", "", line)
    line = re.sub(r"^@param\s+([A-Za-z_]\w*)\s+", r"\1: ", line)
    line = re.sub(r"^@tparam\s+([A-Za-z_]\w*)\s+", r"\1: ", line)
    line = re.sub(r"^@return\s+", "戻り値: ", line)
    return line.strip()


def clean_doc(lines_or_text: Iterable[str] | str) -> str:
    if isinstance(lines_or_text, str):
        lines = lines_or_text.splitlines()
    else:
        lines = list(lines_or_text)
    out: list[str] = []
    for raw in lines:
        line = clean_comment_line(raw)
        if not line:
            continue
        if "SPDX-License-Identifier" in line:
            continue
        if set(line) <= set("-=/* "):
            continue
        out.append(line)
    return normalize_ws(" ".join(out))


def leading_comment(text: str, pos: int, max_lines: int = 14) -> str:
    prefix = text[:pos]
    template_suffix = re.search(r"(?:template\s*<[^;{}]*>\s*)+$", prefix, flags=re.S)
    if template_suffix:
        prefix = prefix[:template_suffix.start()]
    block_matches = list(re.finditer(r"/\*.*?\*/", prefix, flags=re.S))
    block_match = block_matches[-1] if block_matches and not prefix[block_matches[-1].end():].strip() else None
    line_match = re.search(r"(?:^|\n)((?:\s*//[^\n]*(?:\n|$))+)[ \t\r\n]*$", prefix)
    if block_match and (not line_match or block_match.start() > line_match.start()):
        return clean_doc(block_match.group(0))
    if line_match:
        return clean_doc(line_match.group(0))

    before = prefix.splitlines()
    docs: list[str] = []
    seen_comment = False
    for line in reversed(before[-max_lines:]):
        stripped = line.strip()
        if stripped.startswith("//"):
            docs.append(stripped)
            seen_comment = True
            continue
        if stripped == "" and not seen_comment:
            continue
        break
    docs.reverse()
    return clean_doc(docs)


def find_matching_brace(text: str, open_idx: int) -> int:
    depth = 0
    i = open_idx
    state = "code"
    quote = ""
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if ch == "/" and nxt == "/":
                state = "line"
                i += 2
                continue
            if ch == "/" and nxt == "*":
                state = "block"
                i += 2
                continue
            if ch in ('"', "'"):
                state = "string"
                quote = ch
                i += 1
                continue
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return i
        elif state == "line":
            if ch == "\n":
                state = "code"
        elif state == "block":
            if ch == "*" and nxt == "/":
                state = "code"
                i += 2
                continue
        elif state == "string":
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                state = "code"
        i += 1
    return -1


def mask_comments(text: str) -> str:
    out = list(text)
    i = 0
    state = "code"
    quote = ""
    while i < len(out):
        ch = out[i]
        nxt = out[i + 1] if i + 1 < len(out) else ""
        if state == "code":
            if ch == "/" and nxt == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "line"
                continue
            if ch == "/" and nxt == "*":
                out[i] = out[i + 1] = " "
                i += 2
                state = "block"
                continue
            if ch in ('"', "'"):
                quote = ch
                i += 1
                state = "string"
                continue
        elif state == "line":
            if ch == "\n":
                state = "code"
            else:
                out[i] = " "
        elif state == "block":
            if ch == "*" and nxt == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "code"
                continue
            if ch != "\n":
                out[i] = " "
        elif state == "string":
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                state = "code"
        i += 1
    return "".join(out)


def mask_preprocessor(text: str) -> str:
    """preprocessor の logical line を空白化し、改行と文字位置を保つ。"""
    out = list(text)
    offset = 0
    continued = False
    for raw_line in text.splitlines(keepends=True):
        body = raw_line.rstrip("\r\n")
        is_directive = continued or body.lstrip().startswith("#")
        if is_directive:
            for index in range(offset, offset + len(raw_line)):
                if out[index] not in "\r\n":
                    out[index] = " "
        continued = is_directive and body.rstrip().endswith("\\")
        offset += len(raw_line)
    return "".join(out)


def mask_strings(text: str) -> str:
    """文字列・文字 literal を空白化し、改行と文字位置を保つ。"""
    out = list(text)
    cursor = 0
    while cursor < len(text):
        raw_match = re.match(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(', text[cursor:])
        if raw_match:
            terminator = ")" + raw_match.group(1) + '"'
            close = text.find(terminator, cursor + raw_match.end())
            end = len(text) if close < 0 else close + len(terminator)
            for index in range(cursor, end):
                if out[index] not in "\r\n":
                    out[index] = " "
            cursor = end
            continue
        prefix_length = 0
        if text.startswith("u8", cursor) and cursor + 2 < len(text) and text[cursor + 2] in ('"', "'"):
            prefix_length = 2
        elif text[cursor] in "uUL" and cursor + 1 < len(text) and text[cursor + 1] in ('"', "'"):
            prefix_length = 1
        quote_index = cursor + prefix_length
        if quote_index < len(text) and text[quote_index] in ('"', "'"):
            end = _skip_cpp_string(text, quote_index)
            for index in range(cursor, end):
                if out[index] not in "\r\n":
                    out[index] = " "
            cursor = end
            continue
        cursor += 1
    return "".join(out)


def mask_cpp_non_code(text: str, *, preprocessor: bool = True) -> str:
    """C++ declaration 走査の対象外を空白化し、source offset を維持する。"""
    masked = mask_strings(mask_comments(text))
    return mask_preprocessor(masked) if preprocessor else masked


def line_number(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def namespace_hint(text: str, pos: int) -> str:
    code = mask_cpp_non_code(text)
    namespace_openings: dict[int, str] = {}
    pattern = re.compile(r"\b(?:inline\s+)?namespace(?:\s+([A-Za-z_]\w*(?:::\w+)*))?\s*\{")
    for match in pattern.finditer(code, 0, min(pos, len(code))):
        brace = code.find("{", match.start(), match.end())
        if brace >= 0:
            namespace_openings[brace] = match.group(1) or ""

    scope_stack: list[str | None] = []
    for index, ch in enumerate(code[:pos]):
        if ch == "{":
            scope_stack.append(namespace_openings.get(index))
        elif ch == "}" and scope_stack:
            scope_stack.pop()

    parts: list[str] = []
    for namespace in scope_stack:
        if not namespace:
            continue
        components = [part for part in namespace.split("::") if part]
        if components and parts and components[0] == parts[-1]:
            components = components[1:]
        parts.extend(components)
    return "::".join(parts)


def is_namespace_scope(text: str, pos: int) -> bool:
    """positionまでの開いたscopeがnamespaceまたはextern blockだけならtrue。"""
    code = mask_cpp_non_code(text)
    allowed_openings: set[int] = set()
    namespace_pattern = re.compile(r"\b(?:inline\s+)?namespace(?:\s+[A-Za-z_]\w*(?:::\w+)*)?\s*\{")
    extern_pattern = re.compile(r"\bextern\s+\s*\{")
    for pattern in (namespace_pattern, extern_pattern):
        for match in pattern.finditer(code, 0, min(pos, len(code))):
            brace = code.find("{", match.start(), match.end())
            if brace >= 0:
                allowed_openings.add(brace)
    scope_stack: list[bool] = []
    for index, ch in enumerate(code[:pos]):
        if ch == "{":
            scope_stack.append(index in allowed_openings)
        elif ch == "}" and scope_stack:
            scope_stack.pop()
    return all(scope_stack)


def strip_initializer(signature: str) -> str:
    return normalize_ws(signature)


def _skip_cpp_comment(text: str, index: int) -> int:
    """index から始まる C++ comment の直後を返す。"""
    if text.startswith("//", index):
        newline = text.find("\n", index + 2)
        return len(text) if newline < 0 else newline + 1
    if text.startswith("/*", index):
        close = text.find("*/", index + 2)
        return len(text) if close < 0 else close + 2
    return index


def _skip_cpp_string(text: str, index: int) -> int:
    """文字列または文字literalの直後を返す。"""
    quote = text[index]
    cursor = index + 1
    while cursor < len(text):
        if text[cursor] == "\\":
            cursor += 2
            continue
        if text[cursor] == quote:
            return cursor + 1
        cursor += 1
    return len(text)


def _next_non_space(text: str, index: int) -> int:
    cursor = index
    while cursor < len(text) and text[cursor].isspace():
        cursor += 1
    return cursor


CLASS_METADATA_MACROS = frozenset({
    "ACS_ASSET_TYPE",
    "ACS_CLASS",
    "ACS_ENUM",
    "ACS_FUNCTION",
    "ACS_GAME_COMPONENT_KIND",
    "ACS_GAME_SUBSYSTEM_KIND",
    "ACS_PROPERTY",
    "ACS_RTTI",
    "ACS_RTTI_ROOT",
})


def _find_matching_parenthesis(text: str, open_index: int) -> int:
    """文字列とコメント内を除外し、対応する閉じ丸括弧の位置を返す。"""
    depth = 0
    cursor = open_index
    while cursor < len(text):
        if text.startswith(("//", "/*"), cursor):
            cursor = _skip_cpp_comment(text, cursor)
            continue
        character = text[cursor]
        if character in ('"', "'"):
            cursor = _skip_cpp_string(text, cursor)
            continue
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
            if depth == 0:
                return cursor
        cursor += 1
    return -1


def _standalone_class_metadata_macro_end(text: str, start: int) -> int:
    """単独行のクラス用metadata macroなら、その行の直後を返す。"""
    match = re.match(r"(ACS_[A-Z0-9_]+)\s*\(", text[start:])
    if match is None or match.group(1) not in CLASS_METADATA_MACROS:
        return -1

    open_index = start + match.end() - 1
    close_index = _find_matching_parenthesis(text, open_index)
    if close_index < 0:
        return -1

    cursor = close_index + 1
    semicolon_seen = False
    while cursor < len(text):
        if text.startswith("//", cursor):
            return _skip_cpp_comment(text, cursor)
        if text.startswith("/*", cursor):
            comment_end = _skip_cpp_comment(text, cursor)
            if "\n" in text[cursor:comment_end]:
                return comment_end
            cursor = comment_end
            continue
        character = text[cursor]
        if character in " \t\r":
            cursor += 1
            continue
        if character == ";" and not semicolon_seen:
            semicolon_seen = True
            cursor += 1
            continue
        if character == "\n":
            return cursor + 1
        return -1
    return cursor


def _function_signature_from_definition(prefix: str) -> str:
    """inline定義から関数本体とconstructor初期化子を除いた宣言を作る。"""
    signature = normalize_ws(mask_comments(prefix))
    open_index = signature.find("(")
    if open_index >= 0 and signature[:open_index].rstrip().endswith("operator"):
        first_close = signature.find(")", open_index + 1)
        second_open = signature.find("(", first_close + 1) if first_close >= 0 else -1
        if second_open >= 0:
            open_index = second_open
    close = -1
    if open_index >= 0:
        depth = 0
        for index in range(open_index, len(signature)):
            if signature[index] == "(":
                depth += 1
            elif signature[index] == ")":
                depth -= 1
                if depth == 0:
                    close = index
                    break
    if close >= 0:
        suffix = signature[close + 1:]
        initializer = _first_top_level_single_colon(suffix)
        if initializer >= 0:
            signature = signature[:close + 1] + suffix[:initializer]
    return strip_initializer(signature.rstrip() + ";")


def _is_template_angle_open(value: str, cursor: int) -> bool:
    """shift・比較演算子ではないtemplate開始の`<`だけを判定する。"""
    if cursor >= len(value) or value[cursor] != "<":
        return False
    if value.startswith(("<<", "<=", "<=>"), cursor):
        return False
    if cursor > 0 and value[cursor - 1] == "<":
        return False
    return True


def _split_top_level(value: str, delimiter: str) -> list[str]:
    """括弧・角括弧・波括弧・template引数の外側だけで分割する。"""
    parts: list[str] = []
    start = 0
    paren = bracket = brace = angle = 0
    cursor = 0
    while cursor < len(value):
        if value.startswith(("//", "/*"), cursor):
            cursor = _skip_cpp_comment(value, cursor)
            continue
        ch = value[cursor]
        if ch in ('"', "'"):
            cursor = _skip_cpp_string(value, cursor)
            continue
        if ch == "(":
            paren += 1
        elif ch == ")" and paren:
            paren -= 1
        elif ch == "[":
            bracket += 1
        elif ch == "]" and bracket:
            bracket -= 1
        elif ch == "{":
            brace += 1
        elif ch == "}" and brace:
            brace -= 1
        elif ch == "<" and paren == 0 and bracket == 0 and brace == 0 and _is_template_angle_open(value, cursor):
            angle += 1
        elif ch == ">" and angle:
            angle -= 1
        elif ch == delimiter and paren == bracket == brace == angle == 0:
            parts.append(value[start:cursor])
            start = cursor + 1
        cursor += 1
    parts.append(value[start:])
    return parts


def _split_top_level_ranges(value: str, delimiter: str) -> list[tuple[int, int]]:
    """top-level分割後の各範囲を元文字列上のoffsetで返す。"""
    ranges: list[tuple[int, int]] = []
    start = 0
    paren = bracket = brace = angle = 0
    cursor = 0
    while cursor < len(value):
        if value.startswith(("//", "/*"), cursor):
            cursor = _skip_cpp_comment(value, cursor)
            continue
        ch = value[cursor]
        if ch in ('"', "'"):
            cursor = _skip_cpp_string(value, cursor)
            continue
        if ch == "(":
            paren += 1
        elif ch == ")" and paren:
            paren -= 1
        elif ch == "[":
            bracket += 1
        elif ch == "]" and bracket:
            bracket -= 1
        elif ch == "{":
            brace += 1
        elif ch == "}" and brace:
            brace -= 1
        elif ch == "<" and paren == bracket == brace == 0 and _is_template_angle_open(value, cursor):
            angle += 1
        elif ch == ">" and angle:
            angle -= 1
        elif ch == delimiter and paren == bracket == brace == angle == 0:
            ranges.append((start, cursor))
            start = cursor + 1
        cursor += 1
    ranges.append((start, len(value)))
    return ranges


def _first_top_level(value: str, target: str) -> int:
    """入れ子の外側にある最初のtarget位置を返す。"""
    paren = bracket = brace = angle = 0
    cursor = 0
    while cursor < len(value):
        if value.startswith(("//", "/*"), cursor):
            cursor = _skip_cpp_comment(value, cursor)
            continue
        ch = value[cursor]
        if ch in ('"', "'"):
            cursor = _skip_cpp_string(value, cursor)
            continue
        if ch == target and paren == bracket == brace == angle == 0:
            return cursor
        if ch == "(":
            paren += 1
        elif ch == ")" and paren:
            paren -= 1
        elif ch == "[":
            bracket += 1
        elif ch == "]" and bracket:
            bracket -= 1
        elif ch == "{":
            brace += 1
        elif ch == "}" and brace:
            brace -= 1
        elif ch == "<" and paren == bracket == brace == 0 and _is_template_angle_open(value, cursor):
            angle += 1
        elif ch == ">" and angle:
            angle -= 1
        cursor += 1
    return -1


def _first_top_level_single_colon(value: str) -> int:
    """scope演算子を除き、入れ子の外側にある単独の`:`を返す。"""
    cursor = 0
    while cursor < len(value):
        relative = _first_top_level(value[cursor:], ":")
        if relative < 0:
            return -1
        index = cursor + relative
        previous = value[index - 1] if index > 0 else ""
        following = value[index + 1] if index + 1 < len(value) else ""
        if previous != ":" and following != ":":
            return index
        cursor = index + 1
    return -1


def _looks_like_function_declaration(signature: str) -> bool:
    """完全な宣言単位を関数 declarator として保守的に判定する。"""
    text = normalize_ws(mask_comments(signature)).rstrip(";")
    if not text or re.match(r"^(?:using|typedef|static_assert|enum|namespace)\b", text):
        return False
    elaborated_return = re.match(
        r"^(?:(?:virtual|static|inline|constexpr|consteval|friend)\s+)*"
        r"(?:class|struct|union)\s+"
        r"[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*(?:\s*<[^;{}]+>)?\s*"
        r"[*&]+\s*(?:[A-Za-z_]\w*::)*[A-Za-z_]\w*\s*\(",
        text,
    )
    if re.match(r"^(?:(?:virtual|static|inline|constexpr|consteval|friend)\s+)*(?:class|struct|union)\b", text) and not elaborated_return:
        return False
    if not ("(" in text and ")" in text):
        return False
    if re.search(
        r"\boperator\s*(?:\(\)|\[\]|new\[\]|delete\[\]|new|delete|"
        r"<=>|<<=|>>=|==|!=|<=|>=|&&|\|\||\+\+|--|->\*|->|"
        r"[+\-*/%<>=!&|^~,]+|[A-Za-z_]\w*(?:::\w+)*)\s*\(",
        text,
    ):
        return True
    first_open = _first_top_level(text, "(")
    pointer_declarator = re.search(r"\(\s*[*&]+\s*[A-Za-z_]\w*\s*\)", text)
    if pointer_declarator and pointer_declarator.start() <= first_open:
        return False
    first_equal = _first_top_level(text, "=")
    if first_open < 0:
        return False
    if first_equal >= 0 and first_equal < first_open:
        operator_prefix = text[max(0, first_equal - 12):first_equal]
        if "operator" not in operator_prefix:
            return False
    return not re.search(r"\b(?:if|for|while|switch|return|sizeof|alignof|static_assert)\s*\(", text)


def _split_simple_member_declaration(signature: str) -> list[str]:
    """`f32 x, y;`のような単純fieldを個別宣言へ展開する。"""
    text = normalize_ws(mask_comments(signature)).rstrip(";")
    if "(" in text or ")" in text:
        return [normalize_ws(mask_comments(signature))]
    parts = [part.strip() for part in _split_top_level(text, ",")]
    if len(parts) <= 1:
        return [normalize_ws(mask_comments(signature))]
    first_declarator = parts[0].split("=", 1)[0].strip()
    name_match = re.search(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*$", first_declarator)
    if not name_match:
        return [normalize_ws(mask_comments(signature))]
    type_prefix = parts[0][:name_match.start()].rstrip()
    if not type_prefix or type_prefix.endswith(("*", "&")):
        return [normalize_ws(mask_comments(signature))]
    result = [parts[0].rstrip() + ";"]
    for part in parts[1:]:
        result.append(f"{type_prefix} {part.rstrip()};")
    return result


@dataclass
class _TypeCandidate:
    kind: str
    name: str
    owner_qualification: tuple[str, ...]
    token_start: int
    signature_start: int
    open_index: int
    close_index: int


def _inside_template_parameter_list(code: str, position: int) -> bool:
    """position が直近の `template<...>` 内部ならtrueを返す。"""
    boundary = max(code.rfind(";", 0, position), code.rfind("{", 0, position), code.rfind("}", 0, position))
    template_start = code.rfind("template", boundary + 1, position)
    if template_start < 0 or not re.match(r"template\s*<", code[template_start:position]):
        return False
    open_angle = code.find("<", template_start, position)
    if open_angle < 0:
        return False
    depth = 0
    for ch in code[open_angle:position]:
        if ch == "<":
            depth += 1
        elif ch == ">" and depth:
            depth -= 1
    return depth > 0


def _template_declaration_start(code: str, token_start: int) -> int:
    """型宣言に直結するtemplate headがあればその開始位置を返す。"""
    boundary = max(code.rfind(";", 0, token_start), code.rfind("{", 0, token_start), code.rfind("}", 0, token_start))
    template_start = code.rfind("template", boundary + 1, token_start)
    if template_start < 0:
        return token_start
    match = re.match(r"template\s*<", code[template_start:token_start])
    if not match:
        return token_start
    open_angle = code.find("<", template_start, token_start)
    depth = 0
    close_angle = -1
    for index in range(open_angle, token_start):
        if code[index] == "<":
            depth += 1
        elif code[index] == ">" and depth:
            depth -= 1
            if depth == 0:
                close_angle = index
                break
    if close_angle < 0:
        return token_start
    between = code[close_angle + 1:token_start]
    return template_start if not between.strip() else token_start


def _declaration_open_or_semicolon(code: str, start: int) -> tuple[int, str]:
    """宣言headの次にあるtop-level `{` または `;` を返す。"""
    paren = bracket = angle = 0
    cursor = start
    while cursor < len(code):
        ch = code[cursor]
        if ch == "(":
            paren += 1
        elif ch == ")" and paren:
            paren -= 1
        elif ch == "[":
            bracket += 1
        elif ch == "]" and bracket:
            bracket -= 1
        elif ch == "<" and paren == bracket == 0 and _is_template_angle_open(code, cursor):
            angle += 1
        elif ch == ">" and angle:
            angle -= 1
        elif ch in "{;" and paren == bracket == angle == 0:
            return cursor, ch
        cursor += 1
    return -1, ""


def _delimiter_depth_at(code: str, position: int) -> tuple[int, int]:
    """指定位置を囲む丸括弧と角括弧の深さを返す。"""
    paren = bracket = 0
    for ch in code[:position]:
        if ch == "(":
            paren += 1
        elif ch == ")" and paren:
            paren -= 1
        elif ch == "[":
            bracket += 1
        elif ch == "]" and bracket:
            bracket -= 1
    return paren, bracket


def collect_type_candidates(code: str) -> list[_TypeCandidate]:
    """実体を持つclass/struct/union宣言をtemplate引数と区別して抽出する。"""
    pattern = re.compile(
        r"\b(class|struct|union)\s+"
        r"(?:(?:\[\[[^\]]+\]\]|alignas\s*\([^)]*\))\s+)*"
        r"([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)"
    )
    candidates: list[_TypeCandidate] = []
    for match in pattern.finditer(code):
        prefix = code[max(0, match.start() - 16):match.start()]
        if re.search(r"\benum\s*$", prefix):
            continue
        if _inside_template_parameter_list(code, match.start()):
            continue
        if any(_delimiter_depth_at(code, match.start())):
            continue
        delimiter, delimiter_kind = _declaration_open_or_semicolon(code, match.end())
        if delimiter < 0 or delimiter_kind != "{":
            continue
        signature_start = _template_declaration_start(code, match.start())
        if _looks_like_function_declaration(code[signature_start:delimiter]):
            continue
        close = find_matching_brace(code, delimiter)
        if close < 0:
            continue
        qualified_name = match.group(2).split("::")
        candidates.append(_TypeCandidate(
            kind=match.group(1),
            name=qualified_name[-1],
            owner_qualification=tuple(qualified_name[:-1]),
            token_start=match.start(),
            signature_start=signature_start,
            open_index=delimiter,
            close_index=close,
        ))

    # namespace直下または型の直下にある宣言だけを公開catalogへ渡す。
    # 型内関数の本体ではowner型の開始波括弧より深くなるため、局所型を除外できる。
    result: list[_TypeCandidate] = []
    accepted_openings: set[int] = set()
    for candidate in candidates:
        if is_namespace_scope(code, candidate.token_start):
            result.append(candidate)
            accepted_openings.add(candidate.open_index)
            continue
        parents = _containing_type_candidates(candidate, candidates)
        if not parents:
            continue
        owner = parents[-1]
        if owner.open_index not in accepted_openings:
            continue
        depth = 0
        for ch in code[owner.open_index + 1:candidate.token_start]:
            if ch == "{":
                depth += 1
            elif ch == "}" and depth:
                depth -= 1
        if depth == 0:
            result.append(candidate)
            accepted_openings.add(candidate.open_index)
    return result


def _containing_type_candidates(
    candidate: _TypeCandidate,
    candidates: list[_TypeCandidate],
) -> list[_TypeCandidate]:
    parents = [
        parent for parent in candidates
        if parent.open_index < candidate.token_start < candidate.close_index < parent.close_index
    ]
    parents.sort(key=lambda item: item.open_index)
    return parents


def _containing_type_names(candidate: _TypeCandidate, candidates: list[_TypeCandidate]) -> list[str]:
    return [parent.name for parent in _containing_type_candidates(candidate, candidates)]


def _containing_type_namespace_parts(parents: list[_TypeCandidate]) -> list[str]:
    """入れ子型のnamespaceへ、明示修飾された所有型も含めて追加する。"""
    if not parents:
        return []
    parts = list(parents[0].owner_qualification)
    parts.extend(parent.name for parent in parents)
    return parts


def _nested_declaration_access(
    code: str,
    position: int,
    parents: list[_TypeCandidate],
) -> str:
    """入れ子宣言の直近owner内におけるaccess指定を返す。"""
    if not parents:
        return "public"

    owner = parents[-1]
    access = "public" if owner.kind in {"struct", "union"} else "private"
    cursor = owner.open_index + 1
    while cursor < position:
        if code[cursor] == "{":
            close = find_matching_brace(code, cursor)
            if close < 0 or close >= position:
                break
            cursor = close + 1
            continue
        match = re.match(r"\b(public|protected|private)\s*:", code[cursor:position])
        if match:
            access = match.group(1)
            cursor += match.end()
            continue
        cursor += 1
    return access


def _join_namespace(base: str, additions: list[str]) -> str:
    parts = [part for part in base.split("::") if part]
    parts.extend(additions)
    return "::".join(parts)


def _type_candidate_signature(text: str, candidate: _TypeCandidate) -> str:
    return normalize_ws(mask_comments(text[candidate.signature_start:candidate.open_index])).rstrip() + ";"


def _candidate_owner_signature(text: str, parents: list[_TypeCandidate]) -> str:
    return _type_candidate_signature(text, parents[-1]) if parents else ""


def _record_class_declaration(
    declaration: str,
    access: str,
    doc: str,
    line: int,
    members: list[MemberInfo],
    functions: list[MemberInfo],
    owner_path: tuple[str, ...] = (),
    declared_name: str = "",
) -> None:
    signature = strip_initializer(normalize_ws(mask_comments(declaration)))
    if not signature or signature == ";":
        return
    if re.match(r"^(?:template\s*<[^>]+>\s*)?(?:friend|static_assert)\b", signature):
        return
    if signature.startswith("#"):
        return
    if re.match(r"^ACS_[A-Z0-9_]+\s*\([^;]*\)\s*;?$", signature):
        return
    if _looks_like_function_declaration(signature):
        functions.append(
            MemberInfo(
                access=access,
                signature=signature,
                doc=doc,
                line=line,
                owner_path=owner_path,
            )
        )
    else:
        member_signatures = _split_simple_member_declaration(signature)
        for member_signature in member_signatures:
            if len(member_signature) > 300 and "=" in member_signature:
                member_signature = member_signature.split("=", 1)[0].rstrip() + ";"
            members.append(
                MemberInfo(
                    access=access,
                    signature=member_signature,
                    doc=doc,
                    line=line,
                    owner_path=owner_path,
                    name=declared_name if len(member_signatures) == 1 else "",
                )
            )


def _member_declarator_name(signature: str) -> str | None:
    """field宣言からowner pathに使う宣言子名を返す。"""
    text = normalize_ws(mask_comments(signature)).rstrip(";")
    text = re.sub(r"\s*=.*$", "", text)
    text = re.sub(r"(?:\[[^\]]*\]\s*)+$", "", text)
    text = re.sub(r"\s*:\s*\d+\s*$", "", text)
    match = re.search(r"([A-Za-z_]\w*)\s*$", text)
    return match.group(1) if match else None


def parse_class_body(
    body: str,
    default_access: str,
    body_start_line: int = 1,
    owner_path: tuple[str, ...] = (),
) -> tuple[list[MemberInfo], list[MemberInfo]]:
    """class直下の宣言だけを読み、inline関数本体の文を除外する。"""
    access = default_access
    functions: list[MemberInfo] = []
    members: list[MemberInfo] = []
    cursor = 0
    declaration_start: int | None = None
    pending_metadata_start: int | None = None
    paren_depth = 0
    bracket_depth = 0

    def declaration_doc(start: int) -> str:
        doc = leading_comment(body, start, max_lines=40)
        if doc or pending_metadata_start is None:
            return doc
        return leading_comment(body, pending_metadata_start, max_lines=40)

    while cursor < len(body):
        if body.startswith(("//", "/*"), cursor):
            cursor = _skip_cpp_comment(body, cursor)
            continue
        ch = body[cursor]
        if ch in ('"', "'"):
            cursor = _skip_cpp_string(body, cursor)
            continue
        if declaration_start is None:
            if ch.isspace():
                cursor += 1
                continue
            access_match = re.match(r"(public|protected|private)\s*:", body[cursor:])
            if access_match:
                access = access_match.group(1)
                cursor += access_match.end()
                continue
            declaration_start = cursor
            metadata_end = _standalone_class_metadata_macro_end(body, declaration_start)
            if metadata_end >= 0:
                if pending_metadata_start is None:
                    pending_metadata_start = declaration_start
                declaration_start = None
                cursor = metadata_end
                paren_depth = 0
                bracket_depth = 0
                continue

        if ch == "(":
            paren_depth += 1
        elif ch == ")" and paren_depth > 0:
            paren_depth -= 1
        elif ch == "[":
            bracket_depth += 1
        elif ch == "]" and bracket_depth > 0:
            bracket_depth -= 1
        elif ch == "{" and paren_depth == 0 and bracket_depth == 0:
            close = find_matching_brace(body, cursor)
            if close < 0:
                break
            prefix = body[declaration_start:cursor]
            compact_prefix = normalize_ws(prefix)
            after = _next_non_space(body, close + 1)
            next_char = body[after] if after < len(body) else ""
            nested_type = re.match(r"^(?:template\s*<[^>]+>\s*)?(?:class|struct|enum|union)\b", compact_prefix)
            function_like = _looks_like_function_declaration(compact_prefix)
            if nested_type and not function_like:
                terminator = body.find(";", close + 1)
                if terminator < 0:
                    break
                trailing = body[close + 1:terminator].strip()
                aggregate_match = re.match(
                    r"^(?:template\s*<[^>]+>\s*)?(class|struct|enum|union)(?:\s+([A-Za-z_]\w*))?",
                    compact_prefix,
                )
                declaration_line = body_start_line + body.count("\n", 0, declaration_start)
                doc = declaration_doc(declaration_start)
                if trailing and aggregate_match:
                    declarator_end = len(trailing)
                    for marker in ("=", "{"):
                        marker_index = _first_top_level(trailing, marker)
                        if marker_index >= 0:
                            declarator_end = min(declarator_end, marker_index)
                    trailing_declarator = trailing[:declarator_end].strip()
                    aggregate_name = aggregate_match.group(2)
                    declared_type = aggregate_name or aggregate_match.group(1)
                    declared_member_name = _member_declarator_name(
                        f"{declared_type} {trailing_declarator};"
                    )
                    trailing_start = close + 1
                    while trailing_start < terminator and body[trailing_start].isspace():
                        trailing_start += 1
                    member_line = body_start_line + body.count("\n", 0, trailing_start)
                    member_begin = len(members)
                    display_declaration = (
                        normalize_ws(mask_comments(body[declaration_start:terminator + 1]))
                        if aggregate_name is None
                        else f"{declared_type} {trailing_declarator};"
                    )
                    _record_class_declaration(
                        display_declaration,
                        access,
                        doc,
                        member_line,
                        members,
                        functions,
                        owner_path,
                        declared_member_name or "",
                    )
                    # 匿名aggregateのfieldだけをinstance配下へ展開する。
                    # 名前付き型の本体は独立した型pageへ載せ、instanceへ複製しない。
                    if aggregate_name is None:
                        nested_body = body[cursor + 1:close]
                        nested_line = body_start_line + body.count("\n", 0, cursor + 1)
                        for owner_member in members[member_begin:]:
                            owner_name = owner_member.name or _member_declarator_name(owner_member.signature)
                            if owner_name is None:
                                continue
                            nested_members, nested_functions = parse_class_body(
                                nested_body,
                                "public" if aggregate_match.group(1) in {"struct", "union"} else "private",
                                nested_line,
                                owner_path + (owner_name,),
                            )
                            members.extend(nested_members)
                            functions.extend(nested_functions)
                elif aggregate_match and not aggregate_match.group(2) and aggregate_match.group(1) in {"struct", "union"}:
                    nested_members, nested_functions = parse_class_body(
                        body[cursor + 1:close],
                        "public",
                        body_start_line + body.count("\n", 0, cursor + 1),
                        owner_path,
                    )
                    members.extend(nested_members)
                    functions.extend(nested_functions)
                cursor = terminator + 1
                declaration_start = None
                pending_metadata_start = None
                paren_depth = 0
                bracket_depth = 0
                continue
            if function_like and next_char not in {",", "{"}:
                doc = declaration_doc(declaration_start)
                declaration_line = body_start_line + body.count("\n", 0, declaration_start)
                _record_class_declaration(
                    _function_signature_from_definition(prefix),
                    access,
                    doc,
                    declaration_line,
                    members,
                    functions,
                    owner_path,
                )
                cursor = close + 1
                after = _next_non_space(body, cursor)
                if after < len(body) and body[after] == ";":
                    cursor = after + 1
                declaration_start = None
                pending_metadata_start = None
                paren_depth = 0
                bracket_depth = 0
                continue
            cursor = close + 1
            continue
        elif ch == ";" and paren_depth == 0 and bracket_depth == 0:
            declaration = body[declaration_start:cursor + 1]
            doc = declaration_doc(declaration_start)
            declaration_line = body_start_line + body.count("\n", 0, declaration_start)
            normalized_declaration = normalize_ws(mask_comments(declaration))
            type_forward = re.match(
                r"^(?:template\s*<[^>]+>\s*)?"
                r"(?:class|struct|union|enum(?:\s+class)?)\s+"
                r"[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*(?:\s*<[^;{}]+>)?\s*;$",
                normalized_declaration,
            )
            if not type_forward:
                _record_class_declaration(
                    declaration,
                    access,
                    doc,
                    declaration_line,
                    members,
                    functions,
                    owner_path,
                )
            declaration_start = None
            pending_metadata_start = None
            paren_depth = 0
            bracket_depth = 0
            cursor += 1
            continue
        cursor += 1
    return members, functions


def parse_enum_values(body: str) -> list[str]:
    cleaned = mask_comments(body)
    values: list[str] = []
    for chunk in _split_top_level(cleaned, ","):
        item = chunk.strip()
        if not item:
            continue
        item = normalize_ws(item)
        item = item.split("=")[0].strip()
        item = re.sub(r"[^A-Za-z0-9_].*$", "", item)
        if item and item not in values:
            values.append(item)
    return values


def parse_enum_value_infos(body: str, body_start_line: int) -> list[EnumValueInfo]:
    """列挙値の宣言、説明、行をinitializerを含めて抽出する。"""
    result: list[EnumValueInfo] = []
    for start, end in _split_top_level_ranges(body, ","):
        raw = body[start:end]
        masked = mask_comments(raw)
        match = re.search(r"\b([A-Za-z_]\w*)\b", masked)
        if not match:
            continue
        name = match.group(1)
        absolute_name = start + match.start(1)
        signature = normalize_ws(mask_comments(raw)).strip()
        if not signature:
            continue
        result.append(EnumValueInfo(
            name=name,
            signature=signature,
            doc=leading_comment(body, absolute_name, max_lines=20),
            line=body_start_line + body.count("\n", 0, absolute_name),
        ))
    return result


def parse_macros(text: str, module: str, path: Path) -> list[ApiDecl]:
    non_api_macros = {
        "NOMINMAX",
        "WIN32_LEAN_AND_MEAN",
        "ENGINE_DLL",
        "D3D12_SUPPORTED",
        "__PLACEMENT_NEW_INLINE",
        "__PLACEMENT_VEC_NEW_INLINE",
    }
    decls: list[ApiDecl] = []
    seen_names: set[str] = set()
    lines = text.splitlines(keepends=True)
    index = 0
    offset = 0
    while index < len(lines):
        line = lines[index]
        logical = line
        line_number_value = index + 1
        logical_offset = offset
        while logical.rstrip("\r\n").rstrip().endswith("\\") and index + 1 < len(lines):
            index += 1
            offset += len(lines[index - 1])
            logical += lines[index]
        m = re.match(r"\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*(?:\([^\r\n)]*\))?)\b(.*)", logical, flags=re.S)
        if not m:
            offset += len(lines[index])
            index += 1
            continue
        name = m.group(1)
        base_name = name.split("(")[0]
        if (
            base_name in non_api_macros
            or base_name.startswith(("__", "ACS_DETAIL_"))
            or base_name in seen_names
        ):
            offset += len(lines[index])
            index += 1
            continue
        seen_names.add(base_name)
        decls.append(ApiDecl(
            kind="macro",
            name=base_name,
            module=module,
            path=path,
            line=line_number_value,
            doc=leading_comment(text, logical_offset),
            members=[MemberInfo(access="define", signature=normalize_ws(logical.strip()), doc="", line=line_number_value)],
            signature=normalize_ws(logical.strip()),
            is_definition=True,
        ))
        offset += len(lines[index])
        index += 1
    return decls


def _extract_declared_function_name(signature: str) -> str:
    return _declared_function_name_and_qualification(signature)[0]


def _declared_function_name_and_qualification(signature: str) -> tuple[str, bool]:
    """最初の関数declaratorから名前と型修飾の有無を返す。"""
    text = normalize_ws(mask_comments(signature))
    operator_match = re.search(
        r"\b(operator\s*(?:\(\)|\[\]|new\[\]|delete\[\]|new|delete|"
        r"<=>|<<=|>>=|==|!=|<=|>=|&&|\|\||\+\+|--|->\*|->|"
        r"[+\-*/%<>=!&|^~,]+|[A-Za-z_]\w*(?:::\w+)*))\s*\(",
        text,
    )
    if operator_match:
        prefix = text[:operator_match.start()].rstrip()
        return normalize_ws(operator_match.group(1)), prefix.endswith("::")
    first_open = _first_top_level(text, "(")
    if first_open < 0:
        return "", False
    prefix = text[:first_open].rstrip()
    match = re.search(r"(~?[A-Za-z_]\w*)\s*$", prefix)
    if not match:
        return "", False
    name = match.group(1)
    if name in {
        "if", "for", "while", "switch", "return", "sizeof", "alignof",
        "static_cast", "reinterpret_cast", "const_cast", "dynamic_cast",
        "decltype", "requires", "noexcept", "static_assert",
    }:
        return "", False
    qualifier_prefix = prefix[:match.start()].rstrip()
    return name, qualifier_prefix.endswith("::")


def parse_free_functions(text: str, module: str, path: Path, occupied: list[tuple[int, int]]) -> list[ApiDecl]:
    """namespace直下の関数宣言とinline定義をbrace-awareに抽出する。"""
    mask = list(mask_cpp_non_code(text))
    for start, end in occupied:
        for index in range(start, min(end + 1, len(mask))):
            if mask[index] not in "\r\n":
                mask[index] = " "
    masked = "".join(mask)
    decls: list[ApiDecl] = []

    def append_function(start: int, end: int, namespace: str, definition: bool) -> None:
        raw_signature = text[start:end]
        if definition:
            signature = _function_signature_from_definition(raw_signature)
        else:
            signature = strip_initializer(normalize_ws(mask_comments(raw_signature)))
        if not _looks_like_function_declaration(signature):
            return
        if re.match(r"^(?:template\s*<[^>]+>\s*)?(?:using|typedef|static_assert)\b", signature):
            return
        if re.match(r"^(?:template\s*<[^>]+>\s*)?[A-Z][A-Z0-9_]*\s*\(", signature):
            return
        name, qualified_definition = _declared_function_name_and_qualification(signature)
        if not name or name.startswith("~") or qualified_definition:
            return
        doc = leading_comment(text, start, max_lines=40)
        source_line = line_number(text, start)
        member = MemberInfo(access="free", signature=signature, doc=doc, line=source_line)
        decls.append(ApiDecl(
            kind="function",
            name=name,
            module=module,
            path=path,
            line=source_line,
            namespace=namespace,
            doc=doc,
            functions=[member],
            signature=signature,
            is_definition=definition,
        ))

    def nested_namespace(current: str, value: str) -> str:
        if not value:
            return current
        if not current:
            return value
        return f"{current}::{value}"

    def scan_region(start: int, end: int, namespace: str) -> None:
        cursor = _next_non_space(masked, start)
        declaration_start = cursor
        while cursor < end:
            delimiter, delimiter_kind = _declaration_open_or_semicolon(masked, declaration_start)
            if delimiter < 0 or delimiter >= end:
                return
            if delimiter_kind == ";":
                append_function(declaration_start, delimiter + 1, namespace, False)
                cursor = _next_non_space(masked, delimiter + 1)
                declaration_start = cursor
                continue
            close = find_matching_brace(masked, delimiter)
            if close < 0 or close > end:
                return
            prefix = normalize_ws(masked[declaration_start:delimiter])
            namespace_match = re.match(
                r"^(?:inline\s+)?namespace(?:\s+([A-Za-z_]\w*(?:::\w+)*))?$",
                prefix,
            )
            if namespace_match:
                scan_region(delimiter + 1, close, nested_namespace(namespace, namespace_match.group(1) or ""))
                cursor = _next_non_space(masked, close + 1)
                declaration_start = cursor
                continue
            if re.match(r'^extern(?:\s+"[^"]+")?$', normalize_ws(text[declaration_start:delimiter])):
                scan_region(delimiter + 1, close, namespace)
                cursor = _next_non_space(masked, close + 1)
                declaration_start = cursor
                continue
            if _looks_like_function_declaration(prefix):
                append_function(declaration_start, delimiter, namespace, True)
                cursor = _next_non_space(masked, close + 1)
                if cursor < end and masked[cursor] == ";":
                    cursor = _next_non_space(masked, cursor + 1)
                declaration_start = cursor
                continue
            # aggregate初期化やlambda本体は宣言の一部として保持し、末尾`;`まで進む。
            cursor = _next_non_space(masked, close + 1)
            declaration_start = declaration_start if cursor < end and masked[cursor] != ";" else cursor

    scan_region(0, len(masked), "")
    return decls


def parse_forward_declarations(
    text: str,
    code_text: str,
    module: str,
    path: Path,
    type_candidates: list[_TypeCandidate],
) -> list[ApiDecl]:
    """定義本体を持たない公開型宣言を抽出する。"""
    declarations: list[ApiDecl] = []
    type_pattern = re.compile(
        r"\b(class|struct|union)\s+"
        r"(?:(?:\[\[[^\]]+\]\]|alignas\s*\([^)]*\))\s+)*"
        r"([A-Za-z_]\w*)\s*;"
    )
    enum_pattern = re.compile(
        r"\benum\s+(?:(class|struct)\s+)?([A-Za-z_]\w*)"
        r"(?:\s*:\s*[^;{}]+)?\s*;"
    )

    def append(match: re.Match[str], kind: str, name: str) -> None:
        if _inside_template_parameter_list(code_text, match.start()) or any(_delimiter_depth_at(code_text, match.start())):
            return
        declaration_boundary = max(
            code_text.rfind(";", 0, match.start()),
            code_text.rfind("{", 0, match.start()),
            code_text.rfind("}", 0, match.start()),
        )
        if re.search(r"\bfriend\s*$", code_text[declaration_boundary + 1:match.start()]):
            return
        parents = [
            candidate for candidate in type_candidates
            if candidate.open_index < match.start() < match.end() < candidate.close_index
        ]
        parents.sort(key=lambda item: item.open_index)
        if not parents and not is_namespace_scope(text, match.start()):
            return
        signature_start = _template_declaration_start(code_text, match.start())
        declarations.append(ApiDecl(
            kind=kind,
            name=name,
            module=module,
            path=path,
            line=line_number(text, match.start()),
            namespace=_join_namespace(
                namespace_hint(text, match.start()),
                _containing_type_namespace_parts(parents),
            ),
            access=_nested_declaration_access(code_text, match.start(), parents),
            doc=leading_comment(text, signature_start),
            signature=normalize_ws(mask_comments(text[signature_start:match.end()])),
            owner_signature=_candidate_owner_signature(text, parents),
        ))

    for match in type_pattern.finditer(code_text):
        append(match, match.group(1), match.group(2))
    for match in enum_pattern.finditer(code_text):
        append(match, "enum class" if match.group(1) else "enum", match.group(2))
    return declarations


def parse_header(path: Path, source_root: Path | None = None) -> HeaderInfo:
    source_root = source_root or SRC_ROOT
    relpath = path.relative_to(source_root)
    module = relpath.parts[0].lower()
    text = path.read_text(encoding="utf-8", errors="replace")
    code_text = mask_cpp_non_code(text)
    info = HeaderInfo(module=module, path=path)
    occupied: list[tuple[int, int]] = []

    type_candidates = collect_type_candidates(code_text)
    for candidate in type_candidates:
        body = text[candidate.open_index + 1:candidate.close_index]
        default_access = "public" if candidate.kind in {"struct", "union"} else "private"
        members, functions = parse_class_body(body, default_access, line_number(text, candidate.open_index + 1))
        parents = _containing_type_candidates(candidate, type_candidates)
        base_namespace = namespace_hint(text, candidate.token_start)
        owner_parts = _containing_type_namespace_parts(parents)
        namespace = _join_namespace(
            base_namespace,
            owner_parts + list(candidate.owner_qualification),
        )
        explicit_owner = _join_namespace(base_namespace, list(candidate.owner_qualification))
        signature = _type_candidate_signature(text, candidate)
        info.declarations.append(ApiDecl(
            kind=candidate.kind,
            name=candidate.name,
            module=module,
            path=path,
            line=line_number(text, candidate.token_start),
            namespace=namespace,
            access=_nested_declaration_access(code_text, candidate.token_start, parents),
            doc=leading_comment(text, candidate.signature_start),
            members=members,
            functions=functions,
            signature=signature,
            owner_signature=_candidate_owner_signature(text, parents),
            owner_qualified_name=explicit_owner if candidate.owner_qualification else "",
            is_definition=True,
        ))
        occupied.append((candidate.signature_start, candidate.close_index))

    enum_pattern = re.compile(r"\benum\s+(?:(class|struct)\s+)?([A-Za-z_]\w*)\b")
    for m in enum_pattern.finditer(code_text):
        delimiter, delimiter_kind = _declaration_open_or_semicolon(code_text, m.end())
        if delimiter < 0 or delimiter_kind != "{":
            continue
        open_idx = delimiter
        close_idx = find_matching_brace(code_text, open_idx)
        if close_idx < 0:
            continue
        parents = [
            candidate for candidate in type_candidates
            if candidate.open_index < m.start() < close_idx < candidate.close_index
        ]
        parents.sort(key=lambda item: item.open_index)
        signature_start = _template_declaration_start(code_text, m.start())
        body = text[open_idx + 1:close_idx]
        signature = normalize_ws(mask_comments(text[signature_start:open_idx])).rstrip() + ";"
        value_infos = parse_enum_value_infos(body, line_number(text, open_idx + 1))
        info.declarations.append(ApiDecl(
            kind="enum class" if m.group(1) else "enum",
            name=m.group(2),
            module=module,
            path=path,
            line=line_number(text, m.start()),
            namespace=_join_namespace(
                namespace_hint(text, m.start()),
                _containing_type_namespace_parts(parents),
            ),
            access=_nested_declaration_access(code_text, m.start(), parents),
            doc=leading_comment(text, signature_start),
            values=[value.name for value in value_infos],
            value_infos=value_infos,
            signature=signature,
            owner_signature=_candidate_owner_signature(text, parents),
            is_definition=True,
        ))
        occupied.append((signature_start, close_idx))

    info.declarations.extend(parse_forward_declarations(text, code_text, module, path, type_candidates))
    info.declarations.extend(parse_free_functions(text, module, path, occupied))
    info.declarations.extend(parse_macros(text, module, path))
    return info


def parse_all_headers(source_root: Path | None = None) -> tuple[list[HeaderInfo], list[ApiDecl]]:
    source_root = source_root or SRC_ROOT
    headers = sorted(
        path
        for path in source_root.rglob("*")
        if path.is_file() and path.suffix.lower() in {".h", ".hh", ".hpp", ".inl"}
    )
    infos: list[HeaderInfo] = []
    decls: list[ApiDecl] = []
    seen_anchor: Counter[str] = Counter()
    for path in headers:
        info = parse_header(path, source_root)
        for decl in info.declarations:
            base = slug(f"api-{decl.module}-{decl.kind}-{decl.name}")
            seen_anchor[base] += 1
            decl.anchor = base if seen_anchor[base] == 1 else f"{base}-{seen_anchor[base]}"
            decls.append(decl)
        infos.append(info)
    return infos, decls


def parse_module_cmake() -> dict[str, dict[str, object]]:
    out: dict[str, dict[str, object]] = defaultdict(lambda: {"features": [], "deps": []})
    for module_dir in SRC_ROOT.iterdir():
        cmake = module_dir / "Module.cmake"
        if not cmake.exists():
            continue
        text = cmake.read_text(encoding="utf-8", errors="replace")
        mod = module_dir.name.lower()
        dep_tokens = re.findall(r"\b(?:PUBLIC_DEPS|PRIVATE_DEPS)\s+([A-Za-z0-9_\s]+?)(?:\n\s*[A-Z_]+\b|\))", text, flags=re.S)
        deps: list[str] = []
        for block in dep_tokens:
            for tok in re.findall(r"\b[A-Z][A-Za-z0-9_]*\b", block):
                if tok not in deps and tok not in {"PUBLIC", "PRIVATE", "DEPS"}:
                    deps.append(tok)
        features = re.findall(r"acs_module_feature\([^)]*?\bNAME\s+([A-Za-z0-9_]+)", text, flags=re.S)
        out[mod]["deps"] = deps
        out[mod]["features"] = features
    return out


def parse_cmake_presets() -> list[dict[str, object]]:
    path = ENGINE_ROOT / "CMakePresets.json"
    if not path.exists():
        return []
    data = json.loads(path.read_text(encoding="utf-8"))
    return data.get("configurePresets", [])


def parse_samples(api_names: set[str]) -> list[dict[str, object]]:
    samples: list[dict[str, object]] = []
    if not SAMPLES_ROOT.exists():
        return samples
    for d in sorted(SAMPLES_ROOT.iterdir()):
        if not d.is_dir() or not re.match(r"^\d{2}_", d.name):
            continue
        files = sorted([p for p in d.rglob("*") if p.suffix.lower() in {".cpp", ".h", ".hpp"}])
        joined = "\n".join(p.read_text(encoding="utf-8", errors="replace")[:120000] for p in files[:30])
        detected = [name for name in sorted(api_names) if re.search(rf"\b{re.escape(name)}\b", joined)]
        detected = [name for name in detected if len(name) > 2][:16]
        includes = sorted(set(re.findall(r'#include\s+"([^"]+)"', joined)))[:12]
        samples.append({
            "name": d.name,
            "path": d,
            "description": SAMPLE_DESCRIPTIONS.get(d.name, prettify_sample_name(d.name)),
            "requirement": sample_requirement(d.name),
            "files": files[:10],
            "apis": detected,
            "includes": includes,
        })
    return samples


def prettify_sample_name(name: str) -> str:
    body = re.sub(r"^\d{2}_", "", name)
    body = re.sub(r"(?<!^)([A-Z])", r" \1", body).strip()
    return f"{body} のサンプル。"


def sample_requirement(name: str) -> str:
    n = int(name[:2]) if re.match(r"^\d{2}", name) else -1
    dx12_only = {
        20, 21, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
        47, 52, 53, 55, 56, 58, 59, 60, 61, 63,
    }
    diligent_only = {24, 25, 26, 27}
    feature_flags = {
        41: "ACS_BUILD_ML_ONNX=ON",
        42: "ACS_BUILD_OPENXR=ON",
        43: "ACS_BUILD_CRASH_REPORTER=ON",
        44: "ACS_BUILD_TELEMETRY_FILE=ON",
        45: "ACS_BUILD_LOCAL_MATCHMAKER=ON",
    }
    if n in diligent_only:
        return "Diligent backend only"
    if n in feature_flags:
        return feature_flags[n]
    if n in dx12_only:
        return "DX12 raw only"
    if n == 40:
        return "console; Lua real backendは ACS_BUILD_SCRIPTING=ON"
    if n == 46:
        return "AssetPack module/tool smoke test"
    if n in {48, 49, 50, 51, 54, 57, 62}:
        return "console/headless"
    return "DX12 raw / Diligent"


def detect_module_order(modules: Iterable[str]) -> list[str]:
    known = [m for m in MODULE_INFO.keys() if m in modules]
    extra = sorted(m for m in modules if m not in MODULE_INFO)
    return known + extra


def term(name: str, anchor_map: dict[str, str]) -> str:
    tip = GLOSSARY.get(name, "")
    href = f"#{anchor_map.get(name, 'glossary-' + slug(name))}"
    return f'<a class="term" href="{href}" data-tip="{esc(tip)}">{esc(name)}</a>'


def render_code(code: str) -> str:
    return f'<pre><button class="copy" type="button">copy</button><code>{esc(code)}</code></pre>'


def render_member_table(title: str, rows: list[MemberInfo]) -> str:
    if not rows:
        return ""
    buf = [f"<h5>{esc(title)}</h5>", '<table class="member-table"><thead><tr><th>access</th><th>signature</th><th>説明</th></tr></thead><tbody>']
    for row in rows:
        doc = row.doc or ""
        buf.append(
            "<tr>"
            f"<td><span class='badge access-{esc(row.access)}'>{esc(row.access)}</span></td>"
            f"<td><code>{esc(row.signature)}</code></td>"
            f"<td>{esc(doc)}</td>"
            "</tr>"
        )
    buf.append("</tbody></table>")
    return "\n".join(buf)


def render_api_decl(decl: ApiDecl, anchor_map: dict[str, str]) -> str:
    source = rel_from_docs(decl.path)
    qname = f"{decl.namespace}::{decl.name}" if decl.namespace else decl.name
    index = " ".join([decl.name, decl.kind, decl.module, qname, rel(decl.path), decl.doc])
    title = esc(qname)
    doc = decl.doc or "ヘッダーから抽出した公開宣言。詳細な意味は source comment とサンプルの使用箇所を参照。"
    if decl.kind in {"class", "struct"}:
        public_funcs = [f for f in decl.functions if f.access == "public"]
        body = [
            f'<details class="api-item api-card" id="{esc(decl.anchor)}" data-index="{esc(index)}">',
            "<summary>",
            f"<span class='api-kind'>{esc(decl.kind)}</span> <strong>{title}</strong>",
            f"<span class='api-meta'>{esc(rel(decl.path))}:{decl.line}</span>",
            "</summary>",
            f"<p>{esc(doc)}</p>",
            "<p class='tiny'>"
            f"<a href='{esc(source)}'>source</a> "
            f"· public functions: {len(public_funcs)} "
            f"· all functions: {len(decl.functions)} "
            f"· members: {len(decl.members)}"
            "</p>",
            render_member_table("Functions", decl.functions),
            render_member_table("Member Variables / Fields", decl.members),
            "</details>",
        ]
        return "\n".join(body)
    if decl.kind in {"enum", "enum class"}:
        values = "".join(f"<code>{esc(v)}</code> " for v in decl.values)
        return (
            f'<article class="api-item api-card compact" id="{esc(decl.anchor)}" data-index="{esc(index)}">'
            f"<h4><span class='api-kind'>{esc(decl.kind)}</span> {title}</h4>"
            f"<p>{esc(doc)}</p>"
            f"<p class='tiny'><a href='{esc(source)}'>source</a> · {esc(rel(decl.path))}:{decl.line}</p>"
            f"<div class='enum-values'>{values}</div>"
            "</article>"
        )
    if decl.kind == "function":
        sig = decl.functions[0].signature if decl.functions else decl.name
        return (
            f'<article class="api-item api-card compact" id="{esc(decl.anchor)}" data-index="{esc(index)}">'
            f"<h4><span class='api-kind'>function</span> {title}</h4>"
            f"<p>{esc(doc)}</p>"
            f"<pre class='sig'><code>{esc(sig)}</code></pre>"
            f"<p class='tiny'><a href='{esc(source)}'>source</a> · {esc(rel(decl.path))}:{decl.line}</p>"
            "</article>"
        )
    if decl.kind == "macro":
        sig = decl.members[0].signature if decl.members else decl.name
        return (
            f'<article class="api-item api-card compact" id="{esc(decl.anchor)}" data-index="{esc(index)}">'
            f"<h4><span class='api-kind'>macro</span> {title}</h4>"
            f"<p>{esc(doc)}</p>"
            f"<pre class='sig'><code>{esc(sig)}</code></pre>"
            f"<p class='tiny'><a href='{esc(source)}'>source</a> · {esc(rel(decl.path))}:{decl.line}</p>"
            "</article>"
        )
    return ""


def render_setup(presets: list[dict[str, object]]) -> str:
    rows = []
    for p in presets:
        cache = p.get("cacheVariables", {})
        rows.append(
            "<tr>"
            f"<td><code>{esc(p.get('name', ''))}</code></td>"
            f"<td>{esc(p.get('displayName', ''))}</td>"
            f"<td>{esc(p.get('description', ''))}</td>"
            f"<td><code>{esc(cache.get('ACS_RENDER_DX12_RAW', ''))}</code></td>"
            f"<td><code>{esc(cache.get('ACS_RENDER_DILIGENT', ''))}</code></td>"
            "</tr>"
        )
    generate_code = (
        "cd acs\n"
        ".\\generate.ps1\n"
        ".\\generate.ps1 -Open\n"
        ".\\generate.ps1 -Sample 38_HelloFullGame -SolutionName MyShooter\n"
        ".\\generate.ps1 -AllSamples -Tests -Tools\n"
        ".\\generate.ps1 -Diligent"
    )
    cmake_code = (
        "cd acs\\engine\n"
        "cmake --preset dx12-debug\n"
        "cmake --build --preset dx12-debug"
    )
    return f"""
<section id="setup" class="panel">
  <h2>セットアップとビルド</h2>
  <p>初心者向けの推奨経路は <code>acs/generate.ps1</code> です。Visual Studio solution、<code>Binaries/</code>、<code>Intermediate/</code>、<code>Saved/generate.log</code> を現行レイアウトで作ります。</p>
  {render_code(generate_code)}
  <p>直接 CMake preset を使う場合は、preset ファイルが <code>acs/engine/CMakePresets.json</code> にあるため <code>acs/engine</code> で実行します。古い文書の <code>cd acs; cmake --preset ...</code> は現在の配置とずれています。</p>
  {render_code(cmake_code)}
  <table>
    <thead><tr><th>preset</th><th>表示名</th><th>用途</th><th>DX12 raw</th><th>Diligent</th></tr></thead>
    <tbody>{''.join(rows)}</tbody>
  </table>
  <div class="callout warn"><strong>注意:</strong> Diligent preset は初回 configure で外部依存を取得するため時間がかかります。Visual Studio 生成では実行ファイルが <code>acs/Binaries/Debug</code> や <code>acs/Binaries/Release</code> に入り、Ninja preset では <code>acs/Binaries</code> 直下に入ります。</div>
</section>
"""


def render_guide(anchor_map: dict[str, str]) -> str:
    return f"""
<section id="guide" class="panel">
  <h2>最初に読む機能ガイド</h2>
  <div class="grid two">
    <article class="guide-card">
      <h3>最短ルート</h3>
      <ol>
        <li>{term('easy', anchor_map) if 'easy' in GLOSSARY else '<code>acs::easy</code>'} で座標・入力・描画に慣れる。</li>
        <li>{term('FApplication', anchor_map)} へ移り、<code>OnStart</code>/<code>OnUpdate</code>/<code>OnRender</code> の流れを覚える。</li>
        <li>大量オブジェクトは {term('FWorld', anchor_map)} と {term('TQueryView', anchor_map)}、画面遷移は {term('FScene', anchor_map)} を使う。</li>
        <li>描画が 2D なら {term('CSpriteBatch', anchor_map)}、3D なら <code>CStandardShader</code>/<code>CPbrShader</code> へ進む。</li>
      </ol>
    </article>
    <article class="guide-card">
      <h3>設計ルール</h3>
      <ul>
        <li>例外ではなく {term('TResult', anchor_map)} で失敗を返す。</li>
        <li>callback と engine API は基本 {term('noexcept', anchor_map)}。</li>
        <li>コンテナは <code>TArray</code>/<code>FString</code>/<code>THashMap</code> を使う。</li>
        <li>Windows/SDK 直触りは platform/bridge 層へ隔離する。</li>
      </ul>
    </article>
  </div>
</section>
"""


def render_tools() -> str:
    assetpack_code = (
        "acs_assetpack pack   <input_dir> <output.acpak> [--compress] [--encrypt --key-file <p>]\n"
        "acs_assetpack unpack <input.acpak> <output_dir> [--key-file <p> | --key <hex>]\n"
        "acs_assetpack list   <input.acpak>\n"
        "acs_assetpack verify <input.acpak> [--key-file <p> | --key <hex>]\n"
        "acs_assetpack info   <input.acpak>\n"
        "acs_assetpack help   [subcommand]"
    )
    pack_assets_code = (
        ".\\pack-assets.ps1\n"
        ".\\pack-assets.ps1 -In art -Out art.acpak\n"
        ".\\pack-assets.ps1 -Encrypt -KeyFile assets.key\n"
        ".\\pack-assets.ps1 -NoCompress\n"
        ".\\pack-assets.ps1 -NoVerify"
    )
    test_code = (
        "cmake --build C:\\dev\\acs_github\\acs\\Intermediate\\vs --config Debug --target acs_unit_tests\n"
        "ctest --test-dir C:\\dev\\acs_github\\acs\\Intermediate\\vs -C Debug"
    )
    cleanup_code = (
        ".\\clean-up.ps1\n"
        ".\\clean-up.ps1 -y\n"
        "acs\\tools\\capture_window.ps1 -Exe acs\\Binaries\\Debug\\hello_scene2d.exe -Out shot.png -Sleep 4"
    )
    return f"""
<section id="tools" class="panel">
  <h2>ツールとワークフロー</h2>
  <div class="grid two">
    <article class="guide-card">
      <h3>acs_assetpack</h3>
      <p><code>acs/tools/acs_assetpack/main.cpp</code> の実装に合わせた現行 CLI です。設計書の <code>extract</code> ではなく、実装上は <code>unpack</code> です。</p>
      {render_code(assetpack_code)}
      <p class="tiny">exit code: 0=成功、1=usage error、2=runtime error。鍵は 64 hex 文字の 256-bit。<code>--key</code> は履歴に残るため <code>--key-file</code> 推奨。</p>
    </article>
    <article class="guide-card">
      <h3>pack-assets.ps1</h3>
      <p>リポジトリ直下の <code>assets/</code> を既定で <code>game.acpak</code> にします。入力フォルダが無い/空なら失敗します。</p>
      {render_code(pack_assets_code)}
      <p class="tiny"><code>-Encrypt</code> 時は鍵ファイルを生成/使用します。鍵ファイルと <code>*.acpak</code> は秘密や出力物になり得るため、コミット対象にしない運用が必要です。</p>
    </article>
    <article class="guide-card">
      <h3>テスト</h3>
      <p><code>CMakePresets.json</code> には testPresets が無いので、生成済み build tree を指定して <code>ctest</code> を走らせます。</p>
      {render_code(test_code)}
    </article>
    <article class="guide-card">
      <h3>掃除とキャプチャ</h3>
      <p><code>clean-up.ps1</code> は再生成可能な build/output を確認して削除します。<code>capture_window.ps1</code> は GUI サンプルのスクリーンショット確認用です。</p>
      {render_code(cleanup_code)}
    </article>
  </div>
  <div class="callout warn"><strong>AssetPack の正確性:</strong> 現実装は <code>.acpak v1</code>、CRC32、LZ4 圧縮、AES-256-GCM、PBKDF2-HMAC-SHA256、Windows CNG/Bcrypt を使います。<code>docs/AssetPack.md</code> には将来設計も含まれるため、コマンド仕様はこのリファレンスと <code>main.cpp</code>/<code>AcpakFormat.h</code> を基準にしてください。</div>
</section>
"""


def render_recipes(anchor_map: dict[str, str]) -> str:
    cards = []
    for recipe in QUICK_RECIPES:
        apis = " ".join(
            f"<a class='pill' href='#{esc(anchor_map.get(a, ''))}'>{esc(a)}</a>" if a in anchor_map else f"<span class='pill'>{esc(a)}</span>"
            for a in recipe["apis"]  # type: ignore[index]
        )
        cards.append(
            "<article class='recipe'>"
            f"<h3>{esc(recipe['title'])}</h3>"
            f"<p>{esc(recipe['summary'])}</p>"
            f"<p>{apis}</p>"
            f"{render_code(str(recipe['code']))}"
            "</article>"
        )
    return f"""
<section id="recipes" class="panel">
  <h2>コピーして使うサンプルコード</h2>
  <p>各コードは、対応する API カタログやサンプルへ飛べるようにしています。</p>
  {''.join(cards)}
</section>
"""


def render_modules(module_order: list[str], grouped: dict[str, list[ApiDecl]], headers_by_module: dict[str, list[HeaderInfo]], cmake_meta: dict[str, dict[str, object]], anchor_map: dict[str, str]) -> str:
    cards = []
    details = []
    for module in module_order:
        info = MODULE_INFO.get(module, {"title": module, "summary": "", "use": ""})
        decls = grouped.get(module, [])
        headers = headers_by_module.get(module, [])
        counts = Counter(d.kind for d in decls)
        key_symbols = [d for d in decls if d.kind in {"class", "struct"}][:8]
        symbol_links = " ".join(f"<a class='pill' href='#{esc(d.anchor)}'>{esc(d.name)}</a>" for d in key_symbols)
        deps = cmake_meta.get(module, {}).get("deps", [])
        features = cmake_meta.get(module, {}).get("features", [])
        cards.append(
            f"<article class='module-card' data-index='{esc(module + ' ' + info['title'] + ' ' + info['summary'])}'>"
            f"<h3><a href='#module-{esc(module)}'>{esc(info['title'])}</a></h3>"
            f"<p>{esc(info['summary'])}</p>"
            f"<p class='tiny'>headers {len(headers)} · API {len(decls)} · classes {counts.get('class',0)+counts.get('struct',0)} · enums {counts.get('enum',0)+counts.get('enum class',0)}</p>"
            f"<p>{symbol_links}</p>"
            "</article>"
        )
        header_links = " ".join(f"<a class='source-chip' href='{esc(rel_from_docs(h.path))}'>{esc(rel(h.path))}</a>" for h in headers)
        api_html = "\n".join(render_api_decl(d, anchor_map) for d in decls)
        details.append(
            f"<section class='module-section' id='module-{esc(module)}'>"
            f"<h3>{esc(info['title'])}</h3>"
            f"<p>{esc(info['use'])}</p>"
            f"<p class='tiny'>deps: {esc(', '.join(map(str, deps)) or 'none')} · features: {esc(', '.join(map(str, features)) or 'none')}</p>"
            f"<details><summary>Header files ({len(headers)})</summary><div class='source-list'>{header_links}</div></details>"
            f"<div class='api-list'>{api_html}</div>"
            "</section>"
        )
    return f"""
<section id="module-map" class="panel">
  <h2>機能マップ</h2>
  <div class="grid modules">{''.join(cards)}</div>
</section>
<section id="api" class="panel">
  <h2>API カタログ</h2>
  <p>ヘッダーから自動抽出した class/struct/enum/free function/macro です。検索欄で絞り込めます。各カードを開くとメンバ変数・関数・source へのリンクが出ます。</p>
  {''.join(details)}
</section>
"""


def render_samples(samples: list[dict[str, object]], anchor_map: dict[str, str]) -> str:
    cards = []
    for s in samples:
        files = s["files"]  # type: ignore[index]
        file_links = " ".join(f"<a class='source-chip' href='{esc(rel_from_docs(p))}'>{esc(p.name)}</a>" for p in files[:5])  # type: ignore[union-attr]
        api_links = []
        for name in s["apis"][:10]:  # type: ignore[index]
            if name in anchor_map:
                api_links.append(f"<a class='pill' href='#{esc(anchor_map[name])}'>{esc(name)}</a>")
            else:
                api_links.append(f"<span class='pill'>{esc(name)}</span>")
        cards.append(
            f"<article class='sample-card' data-index='{esc(str(s['name']) + ' ' + str(s['description']) + ' ' + ' '.join(s['apis']))}'>"
            f"<h3><a href='{esc(rel_from_docs(s['path']))}'>{esc(s['name'])}</a></h3>"
            f"<p>{esc(s['description'])}</p>"
            f"<p><span class='pill requirement'>{esc(s['requirement'])}</span></p>"
            f"<p>{''.join(api_links)}</p>"
            f"<p class='tiny'>files: {file_links}</p>"
            "</article>"
        )
    return f"""
<section id="samples" class="panel">
  <h2>サンプル逆引き</h2>
  <p>実際に存在する <code>acs/samples/00_*</code> 〜 <code>63_*</code> を全て列挙しています。古い README よりこちらが実態に近いです。</p>
  <div class="grid samples">{''.join(cards)}</div>
</section>
"""


def render_glossary(anchor_map: dict[str, str]) -> str:
    rows = []
    for name, desc in GLOSSARY.items():
        api = anchor_map.get(name)
        link = f"<a href='#{esc(api)}'>API</a>" if api else ""
        rows.append(
            f"<tr id='glossary-{esc(slug(name))}'><td><code>{esc(name)}</code></td><td>{esc(desc)}</td><td>{link}</td></tr>"
        )
    return f"""
<section id="glossary" class="panel">
  <h2>用語集</h2>
  <p>本文中の下線付き用語はホバーで説明、クリックで API またはこの表へ移動します。</p>
  <table><thead><tr><th>用語</th><th>意味</th><th>詳細</th></tr></thead><tbody>{''.join(rows)}</tbody></table>
</section>
"""


def render_css() -> str:
    return r"""
:root {
  --bg: #0b1020;
  --panel: #121a2f;
  --panel2: #18223b;
  --text: #e8edf7;
  --muted: #a8b4cf;
  --line: #2d3a5d;
  --accent: #7cc7ff;
  --accent2: #8ef2b3;
  --warn: #ffd37c;
  --code: #09111f;
}
* { box-sizing: border-box; }
html { scroll-behavior: smooth; }
body {
  margin: 0;
  font-family: "Segoe UI", "Yu Gothic UI", "Meiryo", system-ui, sans-serif;
  color: var(--text);
  background: radial-gradient(circle at top left, #1d315d 0, #0b1020 42rem);
}
a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }
code, pre { font-family: "Cascadia Mono", "Consolas", monospace; }
pre {
  position: relative;
  overflow: auto;
  padding: 1rem;
  background: var(--code);
  border: 1px solid var(--line);
  border-radius: 12px;
}
.copy {
  position: absolute;
  top: .5rem;
  right: .5rem;
  border: 1px solid var(--line);
  background: var(--panel2);
  color: var(--text);
  border-radius: 999px;
  padding: .25rem .65rem;
  cursor: pointer;
}
.layout { display: grid; grid-template-columns: 18rem 1fr; min-height: 100vh; }
aside {
  position: sticky;
  top: 0;
  height: 100vh;
  overflow: auto;
  padding: 1.25rem;
  border-right: 1px solid var(--line);
  background: rgba(10, 16, 32, .88);
  backdrop-filter: blur(10px);
}
aside h1 { font-size: 1.2rem; margin: 0 0 1rem; }
aside nav a { display: block; padding: .35rem 0; color: var(--muted); }
aside nav a:hover { color: var(--text); }
main { padding: 1.5rem 2rem 4rem; min-width: 0; }
.hero {
  padding: 2rem;
  border: 1px solid var(--line);
  border-radius: 24px;
  background: linear-gradient(135deg, rgba(124,199,255,.18), rgba(142,242,179,.08));
  margin-bottom: 1.5rem;
}
.hero h1 { font-size: clamp(2rem, 5vw, 4rem); margin: 0; letter-spacing: -.04em; }
.hero p { color: var(--muted); max-width: 78rem; }
.toolbar {
  display: grid;
  grid-template-columns: 1fr auto auto;
  gap: .75rem;
  align-items: center;
  margin: 1rem 0;
}
input[type="search"] {
  width: 100%;
  padding: .85rem 1rem;
  color: var(--text);
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: 999px;
}
button.small {
  border: 1px solid var(--line);
  background: var(--panel2);
  color: var(--text);
  border-radius: 999px;
  padding: .8rem 1rem;
  cursor: pointer;
}
.panel {
  margin: 1.5rem 0;
  padding: 1.25rem;
  border: 1px solid var(--line);
  border-radius: 20px;
  background: rgba(18, 26, 47, .84);
}
.panel h2 { margin-top: 0; font-size: 1.75rem; }
.grid { display: grid; gap: 1rem; }
.grid.two { grid-template-columns: repeat(auto-fit, minmax(18rem, 1fr)); }
.grid.modules { grid-template-columns: repeat(auto-fill, minmax(17rem, 1fr)); }
.grid.samples { grid-template-columns: repeat(auto-fill, minmax(20rem, 1fr)); }
.module-card, .sample-card, .guide-card, .recipe, .api-card {
  background: rgba(24, 34, 59, .88);
  border: 1px solid var(--line);
  border-radius: 16px;
  padding: 1rem;
}
.module-card h3, .sample-card h3, .guide-card h3, .recipe h3 { margin-top: 0; }
.tiny { color: var(--muted); font-size: .88rem; }
.pill, .source-chip {
  display: inline-block;
  margin: .18rem .15rem .18rem 0;
  padding: .18rem .5rem;
  border: 1px solid var(--line);
  border-radius: 999px;
  color: var(--text);
  background: rgba(124,199,255,.09);
  font-size: .82rem;
}
.source-chip { color: var(--muted); background: rgba(255,255,255,.03); }
.requirement { color: var(--warn); background: rgba(255,211,124,.08); }
.module-section { border-top: 1px solid var(--line); padding-top: 1.25rem; margin-top: 1.25rem; }
.api-list { display: grid; gap: .75rem; }
.api-card summary { cursor: pointer; }
.api-card.compact h4 { margin: 0 0 .5rem; }
.api-kind {
  display: inline-block;
  min-width: 4.8rem;
  color: var(--accent2);
  font-size: .78rem;
  text-transform: uppercase;
  letter-spacing: .06em;
}
.api-meta { float: right; color: var(--muted); font-size: .85rem; }
.member-table, table {
  width: 100%;
  border-collapse: collapse;
  margin: .75rem 0;
}
th, td {
  vertical-align: top;
  border-bottom: 1px solid var(--line);
  padding: .55rem .5rem;
}
th { text-align: left; color: var(--accent2); }
.member-table code { white-space: pre-wrap; }
.badge {
  display: inline-block;
  padding: .1rem .45rem;
  border-radius: 999px;
  background: rgba(255,255,255,.08);
  color: var(--muted);
}
.access-public { color: var(--accent2); }
.access-private { color: #ff9aa7; }
.access-protected { color: var(--warn); }
.enum-values code { display: inline-block; margin: .18rem; padding: .2rem .45rem; background: var(--code); border-radius: 6px; }
.term {
  position: relative;
  border-bottom: 1px dotted var(--accent);
  cursor: help;
}
.term:hover::after {
  content: attr(data-tip);
  position: absolute;
  left: 0;
  top: 1.6em;
  z-index: 20;
  width: min(28rem, 80vw);
  padding: .7rem .85rem;
  color: var(--text);
  background: #050913;
  border: 1px solid var(--accent);
  border-radius: 10px;
  box-shadow: 0 12px 30px rgba(0,0,0,.45);
}
.callout {
  padding: .85rem 1rem;
  border: 1px solid var(--line);
  border-left: 4px solid var(--accent);
  border-radius: 12px;
  background: rgba(124,199,255,.08);
}
.callout.warn { border-left-color: var(--warn); }
.hidden { display: none !important; }
.highlight { outline: 2px solid var(--accent); box-shadow: 0 0 0 5px rgba(124,199,255,.12); }
@media (max-width: 960px) {
  .layout { grid-template-columns: 1fr; }
  aside { position: static; height: auto; }
  .toolbar { grid-template-columns: 1fr; }
  .api-meta { float: none; display: block; margin-top: .3rem; }
}
"""


def render_js() -> str:
    return r"""
const q = document.getElementById('search');
const status = document.getElementById('search-status');
function applySearch() {
  const needle = (q.value || '').trim().toLowerCase();
  const items = document.querySelectorAll('.api-item,.module-card,.sample-card');
  let visible = 0;
  items.forEach(el => {
    const hay = (el.dataset.index || el.textContent || '').toLowerCase();
    const show = !needle || hay.includes(needle);
    el.classList.toggle('hidden', !show);
    if (show) visible++;
  });
  status.textContent = needle ? `${visible} items` : '';
}
q.addEventListener('input', applySearch);
document.getElementById('expand-all').addEventListener('click', () => {
  document.querySelectorAll('#api details').forEach(d => d.open = true);
});
document.getElementById('collapse-all').addEventListener('click', () => {
  document.querySelectorAll('#api details').forEach(d => d.open = false);
});
document.querySelectorAll('button.copy').forEach(btn => {
  btn.addEventListener('click', async () => {
    const code = btn.parentElement.querySelector('code').innerText;
    await navigator.clipboard.writeText(code);
    btn.textContent = 'copied';
    setTimeout(() => btn.textContent = 'copy', 900);
  });
});
function openHashTarget() {
  const id = decodeURIComponent(location.hash.replace(/^#/, ''));
  if (!id) return;
  const el = document.getElementById(id);
  if (!el) return;
  let cur = el;
  while (cur) {
    if (cur.tagName === 'DETAILS') cur.open = true;
    cur = cur.parentElement;
  }
  document.querySelectorAll('.highlight').forEach(e => e.classList.remove('highlight'));
  el.classList.add('highlight');
  setTimeout(() => el.classList.remove('highlight'), 1800);
}
window.addEventListener('hashchange', openHashTarget);
openHashTarget();
"""


def build_reference() -> str:
    header_infos, decls = parse_all_headers()
    cmake_meta = parse_module_cmake()
    presets = parse_cmake_presets()
    grouped: dict[str, list[ApiDecl]] = defaultdict(list)
    headers_by_module: dict[str, list[HeaderInfo]] = defaultdict(list)
    for h in header_infos:
        headers_by_module[h.module].append(h)
    for d in decls:
        grouped[d.module].append(d)
    modules = set(headers_by_module) | set(grouped)
    module_order = detect_module_order(modules)
    anchor_map: dict[str, str] = {}
    for d in decls:
        anchor_map.setdefault(d.name, d.anchor)

    api_names = {d.name for d in decls}
    samples = parse_samples(api_names)

    counts = Counter(d.kind for d in decls)
    generated_at = datetime.now().strftime("%Y-%m-%d %H:%M")
    sidebar_modules = "".join(f"<a href='#module-{esc(m)}'>{esc(MODULE_INFO.get(m, {'title': m})['title'])}</a>" for m in module_order)

    return f"""<!doctype html>
<html lang="ja">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ACS 全機能リファレンス</title>
  <style>{render_css()}</style>
</head>
<body>
<div class="layout">
  <aside>
    <h1>ACS Reference</h1>
    <input id="search" type="search" placeholder="API / sample / module を検索">
    <p id="search-status" class="tiny"></p>
    <nav>
      <a href="#top">概要</a>
      <a href="#setup">セットアップ</a>
      <a href="#tools">ツール</a>
      <a href="#guide">読み方</a>
      <a href="#recipes">サンプルコード</a>
      <a href="#module-map">機能マップ</a>
      <a href="#samples">サンプル逆引き</a>
      <a href="#api">API カタログ</a>
      <a href="#glossary">用語集</a>
      <hr>
      {sidebar_modules}
    </nav>
  </aside>
  <main id="top">
    <section class="hero">
      <h1>ACS 全機能リファレンス</h1>
      <p>公開ヘッダー、CMake モジュール、サンプルを横断した静的リファレンスです。用語はホバーで説明を表示し、クリックで API カードまたは用語集へ移動します。</p>
      <p class="tiny">Generated: {esc(generated_at)} · Headers: {len(header_infos)} · API declarations: {len(decls)} · Classes/Structs: {counts.get('class',0)+counts.get('struct',0)} · Enums: {counts.get('enum',0)+counts.get('enum class',0)} · Free functions: {counts.get('function',0)} · Samples: {len(samples)}</p>
      <div class="toolbar">
        <span class="tiny">検索は左サイドバーでもこのページ全体でも即時反映されます。</span>
        <button id="expand-all" class="small" type="button">API を全部開く</button>
        <button id="collapse-all" class="small" type="button">API を全部閉じる</button>
      </div>
    </section>
    {render_setup(presets)}
    {render_tools()}
    {render_guide(anchor_map)}
    {render_recipes(anchor_map)}
    {render_modules(module_order, grouped, headers_by_module, cmake_meta, anchor_map)}
    {render_samples(samples, anchor_map)}
    {render_glossary(anchor_map)}
    <section class="panel">
      <h2>生成について</h2>
      <p>このファイルは <code>acs/scripts/generate_reference.py</code> から生成されています。API が増えたら同じコマンドを実行してください。</p>
      {render_code('python acs/scripts/generate_reference.py')}
    </section>
  </main>
</div>
<script>{render_js()}</script>
</body>
</html>
"""


def main() -> int:
    """分割リファレンス生成器へ処理を委譲する。"""
    from generate_reference_site import main as generate_reference_site

    return generate_reference_site()


if __name__ == "__main__":
    raise SystemExit(main())
