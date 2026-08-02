#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ACS の単一 HTML API リファレンスを生成する。

生成物は意図的に自己完結かつ外部依存なしとする:

  python acs/scripts/generate_reference.py

acs/src のヘッダー、サンプル、CMake メタデータ、既存文書を走査し、
acs/docs/REFERENCE.html を書き出す。パーサーは C++ コンパイラーではないため
保守的に解析するが、日常的なエンジン利用に必要な構造を網羅できる範囲で抽出する。
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
OUT_PATH = DOCS_ROOT / "REFERENCE.html"


MODULE_INFO: "OrderedDict[str, dict[str, str]]" = OrderedDict([
    ("foundation", {
        "title": "Foundation",
        "summary": "基本型、エラー処理、Result、ログ、アサート、panic、source location。",
        "use": "全モジュールの土台。例外を使わず失敗を返す ACS の流儀はここから始まる。",
    }),
    ("threading", {
        "title": "Threading",
        "summary": "TAtomic、FMutex、FRwLock、FThread、FThreadPool、FJobGraph。",
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
        "summary": "FVec、FMat4、FQuat、CCamera、2D/3D collision、CPU dispatch。",
        "use": "座標、変換、カメラ、当たり判定、SIMD 実行時分岐を扱う。",
    }),
    ("test", {
        "title": "Test",
        "summary": "小さなテストフレームワークと EXPECT 系マクロ。",
        "use": "エンジン規約に沿った no-STL/no-exception の単体テストを書く。",
    }),
    ("platform", {
        "title": "Platform",
        "summary": "FWindow、FInput、Time API、FFileSystem、FStorage、FLocalization。",
        "use": "OS と直接接する層。ゲームコードは基本的にこの API 経由で触る。",
    }),
    ("ecs", {
        "title": "ECS",
        "summary": "FWorld、FEntityId、TSparseSet、TQueryView、FSystemScheduler。",
        "use": "大量の同種オブジェクトをデータ指向に処理する。",
    }),
    ("event", {
        "title": "Event",
        "summary": "CMessageBroker、TMessagePipe、CTimerManager。",
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
        "summary": "FRenderer、RHI、FSpriteBatch、FFont、FStandardShader/FPbrShader、post process、IBL。",
        "use": "2D/3D 描画、低レベル GPU リソース、高レベル描画ヘルパを提供する。",
    }),
    ("app", {
        "title": "App",
        "summary": "FApplication、FAppConfig、ACS_DEFINE_MAIN。",
        "use": "ウィンドウ・入力・レンダラ・ECS をまとめた最小ゲームループを書く。",
    }),
    ("audio", {
        "title": "Audio",
        "summary": "CAudioEngine、FSoundHandle。",
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
        "summary": "AScene、CGame、統一ノード ANode、AComponent、2D/3D 物理、FInputMap、CTweenManager、セーブ、制作支援機能。",
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
    "01_HelloWindow": "FApplication の lifecycle、FWindow、FInput、終了処理の最小構成。",
    "02_HelloSprite": "FSpriteBatch によるスプライト、矩形、アルファブレンド。",
    "03_HelloText": "FFont と UTF-8 テキスト描画。日本語文字列の扱いも確認できる。",
    "04_HelloECS": "FWorld/FEntityId/TQueryView、CMessageBroker、CTimerManager を使う ECS 入門。",
    "05_HelloSave": "FStorage による INI 風の設定・セーブ永続化。",
    "06_HelloLocalization": "FLocalization と多言語切替。ja/en/fr の切替例。",
    "07_HelloAudio": "CAudioEngine で WAV/MP3 を再生する。",
    "08_HelloPhysics2D": "2D の円衝突、重力、マウス発射の基礎。",
    "09_HelloParticles": "2D パーティクル表現。火、火花、噴水、煙など。",
    "10_HelloModel": "FStandardShader と手続きプリミティブで最初の 3D 描画。",
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
    "28_HelloGameFramework": "CGame、AScene、CSceneManager による画面遷移。",
    "29_HelloParticleEditor": "GameFramework tools の Particle Editor。",
    "30_HelloSceneInspector": "FHierarchyPanel、FInspectorPanel、FEditorToolbar を持つ Scene Inspector。",
    "31_HelloModelViewer": "モデルビューア、マテリアル/アニメーション/インスペクタ panels。",
    "32_HelloAnimCurveEditor": "FAnimationCurve の編集 UI。",
    "33_HelloBehaviorTreeEditor": "FBehaviorTree の編集と TreeActions の action 関数群。",
    "34_HelloLevelEditor": "FLevelEditorPanel とエディタ基盤。",
    "35_HelloSpriteAtlasEditor": "FSpriteAtlasEditorPanel と FSpritePack の atlas 作成フロー。",
    "36_HelloFontEditor": "FFont editor と glyph/preview workflow。",
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
    "55_HelloScene2D": "AScene2D starter foundation。",
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
    ("CApplication", "最小のゲームループ基底。OnStart/OnUpdate/OnRender/OnShutdown を override する。"),
    ("CGame", "GameFramework の CApplication 派生。AScene stack、固定 timestep、FRenderContext などをまとめる。"),
    ("AScene", "1 つの画面・状態。OnEnter/OnUpdate/OnRender/OnExit を持ち CSceneManager で遷移する。"),
    ("CSceneServices", "AScene が使う CSceneClock/CTweenManager/FInputMap/CCamera2D などを必要分だけ attach する hub。"),
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
    ("AScene2D", "AScene の 2D 向け既定サービス構成だけを選ぶ空派生。root と描画配線は AScene、CSpriteBatch は CGame が共有する。"),
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
        "summary": "本格的なサンプルはこの形。CApplication を継承して 4 つの hook を実装する。",
        "code": r'''#include "app/Application.h"
#include "app/EntryPoint.h"
#include "platform/Input.h"

class CMyGame : public acs::CApplication {
public:
    void OnStart() noexcept override {
        SetClearColor(0.05f, 0.07f, 0.11f, 1.0f);
    }

    void OnUpdate(acs::f32 dt) noexcept override {
        (void)dt;
        if (acs::CInput::IsKeyPressed(acs::EKey::Escape)) Quit();
    }

    void OnRender() noexcept override {
        // BeginFrame / EndFrame は CApplication 側が呼ぶ。
    }

    void OnShutdown() noexcept override {}
};

ACS_DEFINE_MAIN(CMyGame)''',
        "apis": ["CApplication", "ACS_DEFINE_MAIN", "CInput"],
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
        "title": "AScene 遷移を書く",
        "module": "gameframework",
        "summary": "画面単位で状態を分け、CSceneManager に遷移を依頼する。",
        "code": r'''class ATitleScene final : public acs::game::AScene {
public:
    void OnEnter() noexcept override {
        // title assets を読む。
    }

    void OnUpdate(acs::f32) noexcept override {
        if (acs::CInput::IsKeyPressed(acs::EKey::Enter)) {
            Scenes().ChangeScene(acs::MakeUnique<AGameplayScene>());
        }
    }

    void OnRender(acs::game::FRenderContext& rc) noexcept override {
        rc.Sprites().DrawString(rc.GetFont(), "PRESS ENTER", 320, 240);
    }
};''',
        "apis": ["AScene", "CSceneManager", "FRenderContext"],
    },
    {
        "title": "FSpriteBatch で 2D を描く",
        "module": "render",
        "summary": "RHI の上で 2D sprite/rect/text をまとめて描く。",
        "code": r'''acs::FSpriteBatch sprites;
ACS_TRY(sprites.Init(*renderer.Device(), renderer.ColorFormat(), 4096));

sprites.Begin(*renderer.CommandList(), window.Width(), window.Height());
sprites.DrawRect(32, 32, 160, 48, acs::FVec4{0.1f, 0.2f, 0.4f, 0.9f});
sprites.Draw(texture, 240, 120, 64, 64);
sprites.DrawString(font, "score: 1000", 32, 96, acs::FVec4{1, 1, 1, 1});
sprites.End();''',
        "apis": ["FSpriteBatch", "IRhiCommandList", "FFont"],
    },
    {
        "title": "AssetPack bridge の考え方",
        "module": "assetpack",
        "summary": "開発時は loose file、出荷時は .acpak を mount する。ゲーム側ロードコードは同じに保つ。",
        "code": r'''acs::assetpack::FAcpakGameReader reader;
ACS_TRY(reader.Mount("game.acpak"));

auto size = reader.FileSize("textures/player.png");
if (size.IsErr()) {
    ACS_LOG_ERROR("asset missing: textures/player.png");
    return;
}

acs::TArray<acs::u8> bytes;
bytes.Resize(static_cast<acs::usize>(size.Value()));
ACS_TRY(reader.ReadFile("textures/player.png", bytes.Data(), size.Value()));''',
        "apis": ["AssetPack", "FAcpakGameReader", "IAssetPackReader"],
    },
]


@dataclass
class MemberInfo:
    access: str
    signature: str
    doc: str = ""


@dataclass
class ApiDecl:
    kind: str
    name: str
    module: str
    path: Path
    line: int
    namespace: str = ""
    doc: str = ""
    members: list[MemberInfo] = field(default_factory=list)
    functions: list[MemberInfo] = field(default_factory=list)
    values: list[str] = field(default_factory=list)
    anchor: str = ""


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
    line = re.sub(r"\*/$", "", line)
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
    before = text[:pos].splitlines()
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


def line_number(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def namespace_hint(text: str, pos: int) -> str:
    prefix = text[:pos]
    names = re.findall(r"\bnamespace\s+([A-Za-z_]\w*(?:::\w+)*)\s*\{", prefix)
    if "acs::easy" in names:
        return "acs::easy"
    if names and names[-1] == "game" and "acs" in names:
        return "acs::game"
    if names:
        return names[-1]
    return ""


def strip_initializer(signature: str) -> str:
    sig = normalize_ws(signature)
    sig = re.sub(r"\s*=\s*default\s*;", ";", sig)
    sig = re.sub(r"\s*=\s*delete\s*;", ";", sig)
    return sig


def parse_class_body(body: str, default_access: str) -> tuple[list[MemberInfo], list[MemberInfo]]:
    access = default_access
    comments: list[str] = []
    stmt: list[str] = []
    functions: list[MemberInfo] = []
    members: list[MemberInfo] = []
    nested_depth = 0

    for raw in body.splitlines():
        stripped = raw.strip()
        if not stripped:
            continue
        if stripped.startswith("//"):
            if nested_depth == 0:
                comments.append(stripped)
            continue
        if stripped in ("public:", "protected:", "private:"):
            access = stripped[:-1]
            comments = []
            stmt = []
            continue

        if nested_depth > 0:
            nested_depth += stripped.count("{") - stripped.count("}")
            continue

        if re.match(r"^(class|struct|enum)\b", stripped) and "{" in stripped and not stripped.endswith(";"):
            nested_depth += stripped.count("{") - stripped.count("}")
            comments = []
            stmt = []
            continue

        if stripped.startswith(("static_assert", "friend ", "#", "ACS_", "using ")):
            comments = []
            stmt = []
            continue

        stmt.append(stripped)
        joined = normalize_ws(" ".join(stmt))
        if joined.endswith(";"):
            doc = clean_doc(comments)
            sig = strip_initializer(joined)
            comments = []
            stmt = []
            if "(" in sig and ")" in sig and not re.match(r"^(typedef|using)\b", sig):
                functions.append(MemberInfo(access=access, signature=sig, doc=doc))
            elif not re.match(r"^(typedef|using)\b", sig):
                members.append(MemberInfo(access=access, signature=sig, doc=doc))
    return members, functions


def parse_enum_values(body: str) -> list[str]:
    cleaned = re.sub(r"//.*", "", body)
    cleaned = re.sub(r"/\*.*?\*/", "", cleaned, flags=re.S)
    values: list[str] = []
    for chunk in cleaned.split(","):
        item = chunk.strip()
        if not item:
            continue
        item = normalize_ws(item)
        item = item.split("=")[0].strip()
        item = re.sub(r"[^A-Za-z0-9_].*$", "", item)
        if item and item not in values:
            values.append(item)
    return values


def parse_macros(text: str, module: str, path: Path) -> list[ApiDecl]:
    decls: list[ApiDecl] = []
    for idx, line in enumerate(text.splitlines(), start=1):
        m = re.match(r"\s*#\s*define\s+([A-Z_][A-Z0-9_]*(?:\([^)]*\))?)\b(.*)", line)
        if not m:
            continue
        name = m.group(1)
        base_name = name.split("(")[0]
        if not (base_name.startswith("ACS_") or base_name.startswith("WITH_ACS_")):
            continue
        decls.append(ApiDecl(
            kind="macro",
            name=base_name,
            module=module,
            path=path,
            line=idx,
            doc=leading_comment(text, text.find(line)),
            members=[MemberInfo(access="define", signature=normalize_ws(line.strip()), doc="")],
        ))
    return decls


def parse_free_functions(text: str, module: str, path: Path, occupied: list[tuple[int, int]]) -> list[ApiDecl]:
    mask = list(text)
    for start, end in occupied:
        for i in range(start, min(end + 1, len(mask))):
            if mask[i] != "\n":
                mask[i] = " "
    masked = "".join(mask)

    decls: list[ApiDecl] = []
    comments: list[str] = []
    stmt: list[str] = []
    stmt_start = 0
    depth = 0
    offset = 0
    skip_names = {
        "if", "for", "while", "switch", "return", "sizeof", "static_cast",
        "reinterpret_cast", "const_cast", "void", "constexpr", "class", "struct",
        "enum", "namespace", "template", "decltype", "operator",
    }
    for raw in masked.splitlines(keepends=True):
        line = raw.rstrip("\r\n")
        stripped = line.strip()
        if stripped.startswith("//"):
            comments.append(stripped)
            offset += len(raw)
            continue
        if not stripped:
            offset += len(raw)
            continue
        depth += stripped.count("{") - stripped.count("}")
        if stripped.startswith(("#", "template", "using ", "typedef ", "return ")):
            comments = []
            stmt = []
            offset += len(raw)
            continue
        if not stmt:
            stmt_start = offset
        stmt.append(stripped)
        joined = normalize_ws(" ".join(stmt))
        if joined.endswith(";"):
            if "(" in joined and ")" in joined and not re.search(r"\b(if|while|for|switch)\s*\(", joined):
                m = re.search(r"([~A-Za-z_]\w*)\s*\([^;]*\)\s*(?:const\s*)?(?:noexcept)?\s*;", joined)
                if m:
                    name = m.group(1)
                    if name not in skip_names and not name.startswith("~"):
                        decls.append(ApiDecl(
                            kind="function",
                            name=name,
                            module=module,
                            path=path,
                            line=line_number(masked, stmt_start),
                            namespace=namespace_hint(masked, stmt_start),
                            doc=clean_doc(comments),
                            functions=[MemberInfo(access="free", signature=strip_initializer(joined), doc=clean_doc(comments))],
                        ))
            comments = []
            stmt = []
        offset += len(raw)
    return decls


def parse_header(path: Path) -> HeaderInfo:
    relpath = path.relative_to(SRC_ROOT)
    module = relpath.parts[0].lower()
    text = path.read_text(encoding="utf-8", errors="replace")
    code_text = mask_comments(text)
    info = HeaderInfo(module=module, path=path)
    occupied: list[tuple[int, int]] = []

    class_pattern = re.compile(r"(?<!enum\s)\b(class|struct)\s+([A-Za-z_]\w*)\b[^;{}]*\{", re.M)
    for m in class_pattern.finditer(code_text):
        tail = code_text[m.end(2):]
        if tail.lstrip().startswith("("):
            continue
        open_idx = code_text.find("{", m.start())
        close_idx = find_matching_brace(code_text, open_idx)
        if close_idx < 0:
            continue
        # 無名宣言に見える誤検出は除外する。
        name = m.group(2)
        if name in {"if", "for", "while", "switch"}:
            continue
        body = text[open_idx + 1:close_idx]
        default_access = "public" if m.group(1) == "struct" else "private"
        members, functions = parse_class_body(body, default_access)
        kind = "struct" if m.group(1) == "struct" else "class"
        info.declarations.append(ApiDecl(
            kind=kind,
            name=name,
            module=module,
            path=path,
            line=line_number(text, m.start()),
            namespace=namespace_hint(text, m.start()),
            doc=leading_comment(text, m.start()),
            members=members,
            functions=functions,
        ))
        occupied.append((m.start(), close_idx))

    enum_pattern = re.compile(r"\benum\s+(class\s+)?([A-Za-z_]\w*)\b[^;{}]*\{", re.M)
    for m in enum_pattern.finditer(code_text):
        open_idx = code_text.find("{", m.start())
        close_idx = find_matching_brace(code_text, open_idx)
        if close_idx < 0:
            continue
        body = text[open_idx + 1:close_idx]
        info.declarations.append(ApiDecl(
            kind="enum class" if m.group(1) else "enum",
            name=m.group(2),
            module=module,
            path=path,
            line=line_number(text, m.start()),
            namespace=namespace_hint(text, m.start()),
            doc=leading_comment(text, m.start()),
            values=parse_enum_values(body),
        ))
        occupied.append((m.start(), close_idx))

    info.declarations.extend(parse_free_functions(text, module, path, occupied))
    info.declarations.extend(parse_macros(text, module, path))
    return info


def parse_all_headers() -> tuple[list[HeaderInfo], list[ApiDecl]]:
    headers = sorted(SRC_ROOT.rglob("*.h"))
    infos: list[HeaderInfo] = []
    decls: list[ApiDecl] = []
    seen_anchor: Counter[str] = Counter()
    for path in headers:
        info = parse_header(path)
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
        <li>{term('CApplication', anchor_map)} へ移り、<code>OnStart</code>/<code>OnUpdate</code>/<code>OnRender</code> の流れを覚える。</li>
        <li>大量オブジェクトは {term('FWorld', anchor_map)} と {term('TQueryView', anchor_map)}、画面遷移は {term('AScene', anchor_map)} を使う。</li>
        <li>描画が 2D なら {term('CSpriteBatch', anchor_map)}、3D なら <code>FStandardShader</code>/<code>FPbrShader</code> へ進む。</li>
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


def main() -> None:
    DOCS_ROOT.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_text(build_reference(), encoding="utf-8", newline="\n")
    print(f"wrote {OUT_PATH}")


if __name__ == "__main__":
    main()
