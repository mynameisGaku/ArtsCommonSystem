# Arts Common System (ACS)

## ACS Editor

The current desktop Editor architecture, scene and asset workflows, profiler,
packaging constraints, and aggregate verification commands are documented in
[`acs/docs/EditorArchitecture.md`](acs/docs/EditorArchitecture.md).
Current production-foundation slices include explicit native ABI capability
negotiation, cancellable/cached material previews, deterministic multi-document
save coordination, and package `verify`/`inspect`/`diff` commands that operate
without extracting or executing the archive.

Windows 64-bit 向けのモジュール式 C++20 ゲームフレームワークです。ウィンドウ・入力・
2D/3D 描画・ECS・アセット・音声・ネットワーク・UI・GameFramework と、初学者向けの
`acs::easy` ファサードを同じツリーで提供します。既定の描画バックエンドは raw
DirectX 12 で、Diligent Engine バックエンドも選択できます。

## 最短の入口

最も小さい入口は [`00_HelloEasy`](acs/samples/00_HelloEasy/) です。現行の Easy API
では、継承や手書きの描画パイプラインなしで次の形から始められます。

```cpp
#include "easy/Easy.h"

using namespace acs::easy;

int main() {
    OpenWindow(1280, 720, "ACS Easy");
    float x = 600.0f;

    while (NextFrame()) {
        if (IsKeyPressed(EKey::Escape)) Quit();
        x += 180.0f * DeltaTime();
        DrawRect(x, 320.0f, 80.0f, 80.0f, FColor::Sky);
    }
    return 0;
}
```

実用的な2Dゲームの土台は、ソリューション生成時の既定サンプル
[`55_HelloScene2D`](acs/samples/55_HelloScene2D/) です。初学者向けの説明は
[`QUICKSTART.md`](acs/docs/QUICKSTART.md)、問題が起きた場合は
[`TROUBLESHOOTING.md`](acs/docs/TROUBLESHOOTING.md) を参照してください。

## 必要な環境

- Windows 10 / 11（64-bit）。トップレベルCMakeはWindows以外を明示的に拒否します。
- Visual Studio 2026 と「C++によるデスクトップ開発」ワークロード。現行
  `generate.ps1` の既定generatorは `Visual Studio 18 2026` です。
- CMake 3.24以上。選択したVisual Studio generatorを認識する新しいCMakeを推奨します。
- Windows SDK、PowerShell、Git、初回依存取得用のインターネット接続。

Ninjaと`CMakePresets.json`は現行の標準生成手順では使用しません。Visual Studio 2022を
使う場合は、generatorとソリューション拡張子を明示します。

```pwsh
cd acs
.\generate.ps1 -Generator "Visual Studio 17 2022" -Name ACSGame.sln
```

## 生成・ビルド・実行

リポジトリ直下から`acs/`へ移動し、PowerShellで実行します。

```pwsh
cd acs
.\generate.ps1 -Open
```

既定では次の構成になります。

- CMake source: `acs/engine/`
- build tree: `acs/Intermediate/vs/`
- solution: `acs/ACSGame.slnx`
- build target / startup project: `hello_scene2d`
- executable / DLL: `acs/Binaries/<構成>/`
- configure log: `acs/Saved/generate.log`
- renderer: raw DirectX 12
- samples: `55_HelloScene2D`だけ
- tests / CLI tools / optional backends: 無効

Visual Studioを開かず、CMake CLIでビルド・実行する場合は次のとおりです。

```pwsh
.\generate.ps1
cmake --build .\Intermediate\vs --config Debug --target hello_scene2d
.\Binaries\Debug\hello_scene2d.exe
```

PowerShellの実行ポリシーで`.ps1`が拒否される場合は、`generate.bat`をダブルクリックするか
次のコマンドを使えます。

```pwsh
powershell -NoProfile -ExecutionPolicy Bypass -File .\generate.ps1 -Open
```

### 主な生成オプション

| オプション | 効果 |
|---|---|
| `-Open` | 生成したソリューションをVisual Studioで開く |
| `-Clean` | `Intermediate/vs`を削除してから再生成する |
| `-Name MyGame` | 表層のソリューション名を`MyGame.slnx`へ変更する |
| `-Sample 38_HelloFullGame` | 指定サンプルだけを生成し、targetも自動検出する |
| `-AllSamples` | 選択したバックエンドで利用可能な全サンプルを追加する |
| `-Tests` / `-Tools` | tests / `acs_assetpack` CLI targetを追加する |
| `-Diligent` | raw DX12に加えてDiligent rendererを有効にする |
| `-Scripting`, `-Steamworks`, `-Onnx`, `-OpenXr` | 対応する任意backendを有効にする |
| `-CrashReporter`, `-Telemetry`, `-Matchmaker` | Windows crash dump、file telemetry、local matchmakerを有効にする |
| `-AllBackends` | 上記の任意backendをまとめて有効にする。SDK設定が必要なbackendも含む |

Diligentや任意backendを初めて有効化するconfigureでは、追加のGit repositoryやarchiveを
取得するため時間がかかります。Steamworksなど、公開URLから自動取得できないSDKは個別設定が
必要です。

### テスト

```pwsh
.\generate.ps1 -Tests
cmake --build .\Intermediate\vs --config Debug
ctest --test-dir .\Intermediate\vs -C Debug --output-on-failure
```

## 型名規約

公開型は役割が名前から分かるよう、次のprefixを使います。

| 種別 | Prefix | 例 |
|---|---|---|
| 通常のclass / struct / union | `F` | `FRenderer`, `FErrorCode`, `FVec2` |
| `FObject`管理class | `A` | `ANode`, `AComponent`, `ASprite2DComponent` |
| template | `T` | `TArray<T>`, `TResult<T,E>`, `TUniquePtr<T>` |
| 純粋仮想interface | `I` | `IRhiDevice`, `IAssetLoader` |
| `enum class` | `E` | `EFormat`, `EKey`, `ELogSeverity` |

`using` / `typedef`の型alias、delegate、function-pointer callbackにはprefixを強制しません。
`u32`・`f32`・`usize`などのプリミティブaliasもprefixなしです。詳細は
[`StyleGuide.md`](acs/docs/StyleGuide.md) を参照してください。
ACS Editorの新規クラス生成とBlueprint C++生成も同じ規約を適用し、表示名を保ったまま
`FObject`管理型を`A`、通常型を`F`、列挙型を`E`で始まる安全なC++識別子へ正規化します。

## ノード統一

シーングラフのノードは2D/3D共通の`ANode`へ統一されています。旧`FNode2D` /
`FNode3D`は現行APIではなく、`ANode`が`FTransform3D`を1つ持ち、2D向けには
`SetPosition2D`・`Position2D`・`World2D`などのヘルパを提供します。ノードへ付ける
`FObject`管理componentは`AComponent`派生です。

```cpp
auto player = NewObject<ANode>(FStringView("Player"));
player->SetPosition2D(FVec2{0.0f, 0.0f});
player->AddComponent<ASprite2DComponent>(
    FVec2{0.9f, 0.9f}, FVec4{0.15f, 0.85f, 1.0f, 1.0f});
Root().AddChild(Move(player));
```

親は`TObjectPtr<ANode>`で子を所有し、長期参照には`TWeakObjectPtr<ANode>`を使います。
移行理由と所有権・transform・描画順の詳細は
[`NodeUnification.md`](acs/docs/NodeUnification.md) にあります。

## モジュール

有効化設定は [`acs/engine/modules.cmake`](acs/engine/modules.cmake)、トップレベルCMakeは
[`acs/engine/CMakeLists.txt`](acs/engine/CMakeLists.txt) です。

| モジュール | 役割 | 代表的な現行API |
|---|---|---|
| `Foundation` | 基本型・結果・エラー・ログ | `TResult<T,E>`, `FErrorCode`, `FLogger`, `FStackTrace` |
| `Threading` | thread・同期・job | `TAtomic<T>`, `FMutex`, `FThreadPool`, `FJobGraph` |
| `Memory` | allocator・所有権・追跡 | `FAllocator`, `TUniquePtr<T>`, `TSharedPtr<T>`, `TObjectPtr<T>` |
| `Container` | 配列・文字列・hash・view | `TArray<T>`, `FString`, `THashMap<K,V>`, `TSpan<T>` |
| `Math` | vector・matrix・camera・衝突基本形状 | `FVec2`, `FMat4`, `FQuat`, `FCamera`, `FAabb3` |
| `Platform` | window・input・file・time | `FWindow`, `FInput`, `FFileSystem`, `FFrameTimer` |
| `Ecs` | entity / componentとquery | `FWorld`, `FEntityId`, `TQueryView`, `FEntityCommandBuffer` |
| `Event` | pub/sub・message pipe・timer | `FMessageBroker`, `TMessagePipe<T>`, `FTimerManager` |
| `Asset` | asset registryと各loader | `FAssetRegistry`, `IAssetLoader`, `FImageAsset`, `FMeshAsset` |
| `Render` | RHI・2D/3D描画 | `FRenderer`, `IRhiDevice`, `FSpriteBatch`, `FFont`, `FPbrShader` |
| `App` | application lifecycleとentry point | `FApplication`, `FAppConfig`, `ACS_DEFINE_MAIN` |
| `Audio` | XAudio2再生 | `FAudioEngine`, `FSoundHandle` |
| `Network` | network初期化・TCP・UDP | `FNetwork`, `FTcpConnection`, `FTcpListener`, `FUdpSocket` |
| `Imgui` | Dear ImGui統合（raw DX12時） | `FImGuiCtx` |
| `Mvvm` | observableとbinding | `TObservable<T>`, `TTwoWayBinder<T>`, `FCommand` |
| `Ui` | retained-mode UI | `FWidget`, `FUiRenderer`, `FTextInput` |
| `Easy` | 初学者向け手続きAPI | `FColor`, `FSprite`, `FSound`, `FJobBatch` |
| `AssetPack` | `.acpak`読書き・圧縮・暗号化 | `FAcpakReader`, `FAcpakWriter`, `FAcpakCrypto` |
| `Collision` | sprite / meshからcollider生成 | `FSpriteCollider`, `FMeshCollider` |
| `GameFramework` | game・scene・統一node / component | `FGame`, `FScene2D`, `FScene3D`, `ANode`, `AComponent` |
| `Test` | 単体テストframework | `ACS_TEST`, `EXPECT_*` |

任意module / backendは生成スイッチで追加されます。

| スイッチ | 追加される実装 |
|---|---|
| `-Scripting` | Lua 5.4 backend（`FLuaVm`） |
| `-Steamworks` | Steamworks SDK backend（`FSteamworksBridgeImpl`） |
| `-Onnx` | ONNX Runtime CPU backend（`FOnnxMlRuntime`） |
| `-OpenXr` | Khronos OpenXR loader（`FKhronosOpenXrBridge`） |
| `-CrashReporter` | Windows DbgHelp minidump backend（`FWindowsCrashReporter`） |
| `-Telemetry` | JSON Lines file backend（`FFileTelemetryBackendClient`） |
| `-Matchmaker` | deterministic local matchmaker（`FLocalMatchmaker`） |

## サンプル

`acs/samples/`には、`00_HelloEasy`から`66_HelloVertexSSS`まで、CMake targetを持つ
**67個**の番号付きサンプルディレクトリがあります。backend依存のサンプルは、対応する
生成スイッチを有効にした時だけCMake targetへ追加されます。

| サンプル | target | 内容 | 追加条件 |
|---|---|---|---|
| `00_HelloEasy` | `hello_easy` | Easy APIによる最小2Dループ | 常時 |
| `01_HelloWindow` | `hello_window` | `FApplication`とwindow / rendererの最小構成 | 常時 |
| `20_HelloMVVM` | `hello_mvvm` | MVVMとImGui binding | raw DX12 |
| `24_HelloBloom` | `hello_bloom` | HDR bloom / post process | `-Diligent` |
| `38_HelloFullGame` | `hello_full_game` | 複数sceneを持つ完結ミニゲーム | raw DX12 |
| `41_HelloOnnx` | `hello_onnx` | ONNX Runtime smoke test | `-Onnx` |
| `42_HelloOpenXR` | `hello_openxr` | OpenXR loader smoke test | `-OpenXr` |
| `46_HelloAssetPackBridge` | `hello_asset_pack_bridge` | `.acpak` write / mount / read | 常時 |
| `55_HelloScene2D` | `hello_scene2d` | `FScene2D`と統一`ANode`の実用starter | raw DX12・既定 |
| `63_HelloVerticalSlice` | `hello_vertical_slice` | titleからsaveまでの2D vertical slice | raw DX12 |
| `64_HelloJobs` | `hello_jobs` | Easy job / parallel API | 常時 |
| `66_HelloVertexSSS` | `hello_vertex_sss` | `FVertexScatter`による頂点空間SSS | 常時 |

全ソースをソリューションへ加える場合は`.\generate.ps1 -AllSamples`、1件だけなら
`.\generate.ps1 -Sample 64_HelloJobs`のように指定します。

## 設計上の前提

- ACS sourceはC++例外を使用せず、失敗を`TResult<T,E>`などで明示的に返します。
  raw-DX12構成ではcompiler例外も無効です。Diligent static libraryを含む構成だけは
  MSVC STL ABIを一致させるためcompiler例外を有効にしますが、ACSの例外禁止規約は変わりません。
  RTTIはどちらの構成でも無効です。
- 公開・コアAPIは`TArray`や`FString`などのallocator対応型を使います。一部のbackend実装は
  C/C++ runtimeの低レベルutilityを利用しますが、STL containerを公開APIへ要求しません。
- `ACS_ASSERT`、`FSourceLoc`、`FStackTrace`、各種checked APIで失敗境界を明示します。
- 生成物は`Binaries`・`Intermediate`・`Saved`へ分離し、ソースツリー表層を保ちます。
- module / feature依存はCMakeで検証し、利用可能なtargetだけを生成します。

詳しい設計は [`ARCHITECTURE.md`](acs/docs/ARCHITECTURE.md) を参照してください。

## ドキュメント

- [`FoundationOptimizationWaveM.md`](acs/docs/FoundationOptimizationWaveM.md) — handle layout、timer、immutable input decode、完了通知batch、event snapshot、canonical package pathのWave M検証
- [`FoundationOptimizationWaveK.md`](acs/docs/FoundationOptimizationWaveK.md) — mapped package I/O、scratch再利用、依存batch、path所有のWave K検証
- [`FoundationOptimizationWaveC.md`](acs/docs/FoundationOptimizationWaveC.md) — アセット/ECS/reflection/RHI の Wave C 最適化と Release 証跡
- [`QUICKSTART.md`](acs/docs/QUICKSTART.md) — 初学者向けの導入
- [`RECIPES.md`](acs/docs/RECIPES.md) — 3D描画・音・UIなどの逆引き
- [`samples/README.md`](acs/samples/README.md) — 入門サンプルの学習ガイド
- [`ARCHITECTURE.md`](acs/docs/ARCHITECTURE.md) — module構成と設計
- [`StyleGuide.md`](acs/docs/StyleGuide.md) — F / A / T / I / E命名とcoding rule
- [`NodeUnification.md`](acs/docs/NodeUnification.md) — `ANode`統一と移行指針
- [`SerializationSafety.md`](acs/docs/SerializationSafety.md) — 外部入力・永続化・checked API
- [`TROUBLESHOOTING.md`](acs/docs/TROUBLESHOOTING.md) — よくある問題と対処

## ライセンス

ACS本体は [Apache License 2.0](acs/LICENSE) です。ライセンス条件に従って、商用利用・
改変・再配布が可能です。FetchContentや任意SDKで組み込む第三者componentには、それぞれの
ライセンスと配布条件が適用されます。
