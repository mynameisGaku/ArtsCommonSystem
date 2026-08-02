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
.\acs.ps1 configure --generator "Visual Studio 17 2022" --name ACSGame.sln
```

## 生成・ビルド・実行

日常のWindows操作は
[`ProjectOperations.md`](acs/docs/ProjectOperations.md)を正本とします。PowerShellでは
`.\acs.ps1`、コマンドプロンプトとIDE外部ツールでは`acs.cmd`を使います。両launcherは
同じ操作実体へ委譲し、buildやcleanの規則を複製しません。

```pwsh
.\acs.ps1 open
```

```bat
acs.cmd open
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

Visual Studioを開かずにbuild・実行する場合も同じ入口を使います。

```pwsh
.\acs.ps1 configure
.\acs.ps1 build --config Debug --target hello_scene2d
.\acs\Binaries\Debug\hello_scene2d.exe
```

コマンドプロンプトでは次のように実行します。

```bat
acs.cmd configure
acs.cmd build --config Debug --target hello_scene2d
acs\Binaries\Debug\hello_scene2d.exe
```

### 主な生成オプション

| オプション | 効果 |
|---|---|
| `--open` | 生成したソリューションをVisual Studioで開く |
| `--clean` | `Intermediate/vs`を削除してから再生成する |
| `--name MyGame` | 表層のソリューション名を`MyGame.slnx`へ変更する |
| `--sample 38_HelloFullGame` | 指定サンプルだけを生成し、targetも自動検出する |
| `--all-samples` | 選択したバックエンドで利用可能な全サンプルを追加する |
| `--tests` / `--tools` | tests / `acs_assetpack` CLI targetを追加する |
| `--diligent` | raw DX12に加えてDiligent rendererを有効にする |
| `--scripting`, `--steamworks`, `--onnx`, `--openxr` | 対応する任意backendを有効にする |
| `--crash-reporter`, `--telemetry`, `--matchmaker` | Windows crash dump、file telemetry、local matchmakerを有効にする |
| `--all-backends` | 上記の任意backendをまとめて有効にする。SDK設定が必要なbackendも含む |

Diligentや任意backendを初めて有効化するconfigureでは、追加のGit repositoryやarchiveを
取得するため時間がかかります。Steamworksなど、公開URLから自動取得できないSDKは個別設定が
必要です。

### テスト

```pwsh
.\acs.ps1 configure --tests
.\acs.ps1 build --config Debug
.\acs.ps1 test --config Debug
```

### 低位の診断手順

`acs/generate.ps1`、`cmake --build`、`ctest`は統一入口が委譲する既存実体です。
launcher自体の調査やCMake固有の診断では直接実行できますが、通常の生成・build・test・
配布・cleanでは上記launcherを使います。追加argumentはPowerShellでは引用した`'--'`、
コマンドプロンプトでは`--`より後ろへ指定できます。

```pwsh
powershell -NoProfile -ExecutionPolicy Bypass -File .\acs\generate.ps1 -Tests
cmake --build .\acs\Intermediate\vs --config Debug
ctest --test-dir .\acs\Intermediate\vs -C Debug --output-on-failure
```

## 型名規約

公開型は役割が名前から分かるよう、次のprefixを使います。

| 種別 | Prefix | 例 |
|---|---|---|
| owner / registryに所有され、多態的に扱われるobject | `A` | `AObject`, `ANode`, `AComponent` |
| 機能・処理を持つ具象class | `C` | `CAudioEngine`, `CMessageBroker`, `CTimerManager` |
| データ中心のstruct / union、値、handle | `F` | `FErrorCode`, `FVec2`, `FSoundHandle` |
| template | `T` | `TArray<T>`, `TResult<T,E>`, `TUniquePtr<T>` |
| 純粋仮想interface | `I` | `IRhiDevice`, `IAssetLoader` |
| `enum class` | `E` | `EFormat`, `EKey`, `ELogSeverity` |

namespace公開の意味付きscalar/value型aliasは`F`で始めます。delegate、callback、
function-pointer aliasはprefix自由で、template aliasは今回のscalar監査対象外です。
`u32`・`f32`・`usize`などのプリミティブaliasもprefixなしです。詳細は
[`StyleGuide.md`](acs/docs/StyleGuide.md) を参照してください。
型名の移行はC++の型identifierだけを対象とし、既存のheaderとfile名はinclude経路の
互換性のため維持します。公開型の正規名、定義header、互換aliasはregistryでexact固定され、
登録済みの型名移行債務は0件です。新しい公開型やrole変更は
[`TypeRoleAudit.md`](acs/docs/TypeRoleAudit.md) の監査で再流入を防ぎます。
ACS Editorの新規クラス生成とBlueprint C++生成も同じ規約を適用し、表示名を保ったまま
ACS objectを`A`、機能classを`C`、データ型を`F`、列挙型を`E`で始まる安全なC++識別子へ正規化します。

正規名の例は`AObject`、`CApplication`、`CMessageBroker`、`AAsset`、`CRenderer`です。
旧名は再コンパイルするsource向けの一時`using`だけを残します。`A` / `C` classの改名で
旧object fileとのABI互換symbolは提供しないため、取り込むconsumerは
Debug/Releaseとも全量をclean rebuildしてください。

## ノード統一

シーングラフのノードは2D/3D共通の`ANode`へ統一されています。旧`FNode2D` /
`FNode3D`は現行APIではなく、`ANode`が`FTransform3D`を1つ持ち、2D向けには
`SetPosition2D`・`Position2D`・`World2D`などのヘルパを提供します。ノードへ付ける
componentは`AObject`の実派生である`AComponent`から派生します。

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

## Subsystemの取得

Subsystemは、ownerと寿命が明確で、複数の利用者が共有する更新・終了処理付きの機能に
限って使います。`CApplication`がEngine寿命、`CGame`がGameInstance寿命、`AScene`が
World寿命のSubsystemを所有します。`GetSubsystem<T>()`は各ownerのscopeから親へ探し、
WorldからはWorld → GameInstance → Engineの順で解決します。`ANode`と`AComponent`は、
所属するsceneから配線された同じAPIを使えます。

```cpp
// Engine寿命のassetとtimerを取得する。
AAssetSubsystem* assets = application.GetSubsystem<AAssetSubsystem>();
ATimerSubsystem* timers = game.GetSubsystem<ATimerSubsystem>();

// World寿命のeventとclockをscene配下から取得する。
AEventBus* events = scene.GetSubsystem<AEventBus>();
AWorldClockSubsystem* node_clock = node.GetSubsystem<AWorldClockSubsystem>();
AEventBus* component_events = component.GetSubsystem<AEventBus>();
```

未登録、またはownerへ未配線の場合は`nullptr`を返します。Subsystemの登録・scope・frame phaseは
専用のcatalogとcollectionで管理し、単なる局所値や決定論的な計算機能はSubsystemにしません。

## 今後のVoxel破壊

破壊可能なVoxel worldは、現在のprefix・module・Subsystem契約を維持した独立waveで追加します。
CPU側の密度・material・chunk snapshotなどのデータは`F`、編集・mesh生成などの機能classは`C`とし、
複数ownerが共有する寿命と終了処理が必要になるまではSubsystem化しません。core処理と描画連携を
`Voxel` / `VoxelRender`へ分離し、非同期mesh生成はchunk instance IDとgenerationで古い結果を破棄、
動的GPU meshは静的asset registryから分離する方針です。破壊eventのmaterialとpenetrationの意味は、
実装前の契約reviewで確定します。着想元は
[4Gamerの講演記事](https://www.4gamer.net/games/897/G089771/20260729064/)と
[DevelopersIOのセッションレポート](https://dev.classmethod.jp/articles/cedec-2026-voxel/)です。

## モジュール

有効化設定は [`acs/engine/modules.cmake`](acs/engine/modules.cmake)、トップレベルCMakeは
[`acs/engine/CMakeLists.txt`](acs/engine/CMakeLists.txt) です。

| モジュール | 役割 | 代表的な現行API |
|---|---|---|
| `Foundation` | 基本型・結果・エラー・ログ | `TResult<T,E>`, `FErrorCode`, `CLogger`, `FStackTrace` |
| `Threading` | thread・同期・job | `TAtomic<T>`, `FMutex`, `CThreadPool`, `CJobGraph` |
| `Memory` | allocator・所有権・追跡 | `IAllocator`, `TUniquePtr<T>`, `TSharedPtr<T>`, `TObjectPtr<T>` |
| `Container` | 配列・文字列・hash・view | `TArray<T>`, `FString`, `THashMap<K,V>`, `TSpan<T>` |
| `Math` | vector・matrix・camera・衝突基本形状 | `FVec2`, `FMat4`, `FQuat`, `CCamera`, `FAabb3` |
| `Timing` | 決定論的な固定更新計算とsnapshot | `CFixedStepClock`, `FFixedStepClockSnapshot` |
| `Platform` | window・input・file・time | `FWindow`, `CInput`, `CFileSystem`, `FFrameTimer` |
| `Ecs` | entity / componentとquery | `CWorld`, `FEntityId`, `TQueryView`, `FEntityCommandBuffer` |
| `Event` | pub/sub・message pipe・timer | `CMessageBroker`, `TMessagePipe<T>`, `CTimerManager` |
| `Asset` | asset registryと各loader | `CAssetRegistry`, `IAssetLoader`, `AImageAsset`, `AMeshAsset` |
| `Render` | RHI・2D/3D描画 | `CRenderer`, `IRhiDevice`, `CSpriteBatch`, `FFont`, `CPbrShader` |
| `App` | application lifecycleとentry point | `CApplication`, `FAppConfig`, `AAssetSubsystem`, `ATimerSubsystem` |
| `Audio` | XAudio2再生 | `CAudioEngine`, `FSoundHandle` |
| `Network` | network初期化・TCP・UDP | `CNetwork`, `FTcpConnection`, `FTcpListener`, `FUdpSocket` |
| `Imgui` | Dear ImGui統合（raw DX12時） | `FImGuiCtx` |
| `Mvvm` | observableとbinding | `TObservable<T>`, `TTwoWayBinder<T>`, `FCommand` |
| `Ui` | retained-mode UI | `AWidget`, `CUiRenderer`, `ATextInput` |
| `Easy` | 初学者向け手続きAPI | `FColor`, `FSprite`, `FSound`, `FJobBatch` |
| `AssetPack` | `.acpak`読書き・圧縮・暗号化 | `CAcpakReader`, `CAcpakWriter`, `CAcpakCrypto` |
| `Collision` | sprite / meshからcollider生成 | `CSpriteCollider`, `CMeshCollider` |
| `GameFramework` | game・scene・統一node / component | `CGame`, `AScene2D`, `CScene3D`, `ANode`, `AComponent` |
| `Test` | 単体テストframework | `ACS_TEST`, `EXPECT_*` |

任意module / backendは生成スイッチで追加されます。

| スイッチ | 追加される実装 |
|---|---|
| `-Scripting` | Lua 5.4 backend（`CLuaVm`） |
| `-Steamworks` | Steamworks SDK backend（`CSteamworksBridgeImpl`） |
| `-Onnx` | ONNX Runtime CPU backend（`COnnxMlRuntime`） |
| `-OpenXr` | Khronos OpenXR loader（`CKhronosOpenXrBridge`） |
| `-CrashReporter` | Windows DbgHelp minidump backend（`CWindowsCrashReporter`） |
| `-Telemetry` | JSON Lines file backend（`CFileTelemetryBackendClient`） |
| `-Matchmaker` | deterministic local matchmaker（`CLocalMatchmaker`） |

## サンプル

`acs/samples/`には、`00_HelloEasy`から`66_HelloVertexSSS`まで、CMake targetを持つ
**68個（00〜67）**の番号付きサンプルディレクトリがあります。backend依存のサンプルは、対応する
生成スイッチを有効にした時だけCMake targetへ追加されます。

| サンプル | target | 内容 | 追加条件 |
|---|---|---|---|
| `00_HelloEasy` | `hello_easy` | Easy APIによる最小2Dループ | 常時 |
| `01_HelloWindow` | `hello_window` | `CApplication`とwindow / rendererの最小構成 | 常時 |
| `20_HelloMVVM` | `hello_mvvm` | MVVMとImGui binding | raw DX12 |
| `24_HelloBloom` | `hello_bloom` | HDR bloom / post process | `--diligent` |
| `38_HelloFullGame` | `hello_full_game` | 複数sceneを持つ完結ミニゲーム | raw DX12 |
| `41_HelloOnnx` | `hello_onnx` | ONNX Runtime smoke test | `--onnx` |
| `42_HelloOpenXR` | `hello_openxr` | OpenXR loader smoke test | `--openxr` |
| `46_HelloAssetPackBridge` | `hello_asset_pack_bridge` | `.acpak` write / mount / read | 常時 |
| `55_HelloScene2D` | `hello_scene2d` | `AScene2D`と統一`ANode`の実用starter | raw DX12・既定 |
| `63_HelloVerticalSlice` | `hello_vertical_slice` | titleからsaveまでの2D vertical slice | raw DX12 |
| `64_HelloJobs` | `hello_jobs` | Easy job / parallel API | 常時 |
| `66_HelloVertexSSS` | `hello_vertex_sss` | `CVertexScatter`による頂点空間SSS | 常時 |

全ソースをソリューションへ加える場合は`.\acs.ps1 configure --all-samples`、1件だけなら
`.\acs.ps1 configure --sample 64_HelloJobs`のように指定します。

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
- [`ProjectOperations.md`](acs/docs/ProjectOperations.md) — Windowsでのconfigure・build・test・配布・cleanの統一入口
- [`FixedStepClock.md`](acs/docs/FixedStepClock.md) — 値所有の固定更新時計と一括処理の安全契約
- [`LearningSamplesMigrationPlan.md`](acs/docs/LearningSamplesMigrationPlan.md) — 既存68サンプルを段階的な学習用サンプルへ全面移行する必須計画
- [`RECIPES.md`](acs/docs/RECIPES.md) — 3D描画・音・UIなどの逆引き
- [`samples/README.md`](acs/samples/README.md) — 入門サンプルの学習ガイド
- [`ARCHITECTURE.md`](acs/docs/ARCHITECTURE.md) — module構成と設計
- [`StyleGuide.md`](acs/docs/StyleGuide.md) — A / C / F / I / T / E命名とcoding rule
- [`TypeRoleAudit.md`](acs/docs/TypeRoleAudit.md) — 公開型registry、互換alias、移行債務をexact照合する型役割監査
- [`NodeUnification.md`](acs/docs/NodeUnification.md) — `ANode`統一と移行指針
- [`SceneUnification.md`](acs/docs/SceneUnification.md) — `AScene`統一の設計と移行手順（未実装）
- [`SerializationSafety.md`](acs/docs/SerializationSafety.md) — 外部入力・永続化・checked API
- [`TROUBLESHOOTING.md`](acs/docs/TROUBLESHOOTING.md) — よくある問題と対処

## ライセンス

ACS本体は [Apache License 2.0](acs/LICENSE) です。ライセンス条件に従って、商用利用・
改変・再配布が可能です。FetchContentや任意SDKで組み込む第三者componentには、それぞれの
ライセンスと配布条件が適用されます。
