# ACS アーキテクチャ

本書は Arts Common System（ACS）のアーキテクチャを説明します。ACS は
Windows / DirectX 12 向けのモジュール式 C++20 ゲームフレームワークです。
ACS はランタイム基盤（型・スレッド・メモリ・コンテナ・数学）から始まり、
現在はプラットフォーム・ECS・イベント・アセット・描画・音声・ネットワーク・
UI までを含みます。現在は `src/*/*.Build.cs` に **30 モジュール**を定義し、
標準構成の23モジュールと構成時に選ぶ7個の任意backendに分けています。

## 設計原則

1. **公開所有 ABI は ACS 型で統一する。** 動的所有には `TArray<T>` と `FString`、
   callback 境界には ACS delegate または関数 pointer、threading には `TAtomic<T>`、
   `FThread`、`FMutex` を使います。template trait、局所 algorithm、時刻取得などの
   実装補助は必要な翻訳単位または header に閉じ、所有権を公開 API へ露出しません。
   C runtime header、compiler 組み込み、Windows SDK、DirectXMath は用途を限定して
   明示的に利用します。
2. **例外なし・RTTI なし。** ACS sourceのエラーは `TResult<T, FErrorCode>` で伝搬します。
   raw-DX12構成は `/EHs-c- /GR- /D_HAS_EXCEPTIONS=0` です。任意 render backend を含む
   構成だけは、その static library と compiler exception ABI を一致させる target 設定を
   使います。これは binary 境界の条件であり、ACS source の `throw` / `try` / `catch`
   禁止と `TResult` 契約は変わりません。
3. **既定でスレッドセーフ。** すべての公開 API はスレッド安全性の契約を
   ドキュメント化し守ります。コンテナ自体は内部ロックを持ちませんが、
   エンジンの `FRwLock` / `FMutex` / `TAtomic` プリミティブの背後で安全に使えます。
4. **必要な所に SIMD。** 数学型は SSE に適するよう 16 バイト境界にパディング
   し、DirectXMath を経由します（SSE2 をベースラインに、SSE4.1/AVX/AVX2 の
   高速パスを実行時 CPU 検出で選択）。
5. **診断優先のエラー報告。** すべてのアサート・パニック・エラーは
   `FSourceLoc`（コンパイラ組み込みを使った独自の `source_location` shim）で
   `__FILE__` / `__LINE__` / `__FUNCTION__` を記録します。パニックは DbgHelp
   経由でシンボル化済みスタックトレースを出力します。
6. **manifest 駆動のモジュール構成。** 正規 manifest は
   `src/<mod>/<Name>.Build.cs` に置き、`acsbuild gen` が `Module.cmake` を生成します。
   利用者は `modules.cmake` で module を有効化します。有効な module は静的 library
   target `ACS::<Name>` と compile define `WITH_ACS_<NAME>=1` を公開し、module 個別の
   feature には `WITH_<FEATURE>=1` が付きます。

## モジュール構成

```
acs/
├── CMakeLists.txt
├── CMakePresets.json           # ビルドプリセット (dx12 / diligent)
├── modules.cmake               # ユーザー編集: どのモジュール/機能を作るか
├── cmake/
│   ├── ACSCompilerOptions.cmake
│   ├── ACSModuleSystem.cmake
│   └── ACSThirdParty.cmake
├── src/                        # 各 <mod>/ に正規 Build.cs と生成 Module.cmake がある
│   ├── foundation/             # Types, FSourceLoc, TResult, Assert, Panic, Log
│   ├── threading/              # TAtomic, FMutex, FRwLock, FThread, CThreadPool, CJobGraph
│   ├── memory/                 # IAllocator, CSystemAllocator/CLinearAllocator/CPoolAllocator/CArenaAllocator, TUniquePtr, TRc
│   ├── container/              # TArray, FString, THashMap, Hash, TSpan
│   ├── subsystem/              # owner scopeに従う共有serviceの登録・所有・更新
│   ├── math/                   # FVec2/FVec3/FVec4, FMat4, FQuat, FCamera, Collision2D/3D, dispatch
│   ├── timing/                 # FFixedStepClock, FFixedStepClockSnapshot, 固定更新の一括変換
│   ├── test/                   # 小さなテストフレームワーク + EXPECT_* マクロ
│   ├── platform/               # FWindow, CInput, FClock/FFrameTimer, CFileSystem, FStorage, FLocalization
│   ├── ecs/                    # CWorld, FEntityId, TQueryView, CSystemScheduler
│   ├── event/                  # CTimerManager, CMessageBroker (pub/sub)
│   ├── asset/                  # CAssetRegistry, 画像/メッシュ/音声ローダ, 非同期ロード
│   ├── render/                 # CRenderer, IRhiDevice, FStandardShader, FPbrShader, FSpriteBatch
│   ├── app/                    # CApplication, FAppConfig, ACS_DEFINE_MAIN
│   ├── audio/                  # XAudio2 再生
│   ├── network/                # TCP ソケット
│   ├── imgui/                  # Dear ImGui 統合
│   ├── mvvm/                   # MVVM データバインディング
│   ├── ui/                     # AWidget ベースの UI フレームワーク
│   ├── easy/                   # 初学者向けの高水準 API
│   ├── assetpack/              # .acpak の作成・読み取り
│   ├── gameframework/          # Scene、Node、ゲーム機能
│   ├── collision/              # 2D / 3D 衝突判定
│   └── <optional>/             # Steamworks、Lua、ONNX、OpenXR、crash、telemetry、local match
└── tests/                      # モジュール単体テスト
```

公開 API の回帰契約は `tests` が所有します。学習用実行例は現在同梱せず、
再導入候補を `LearningSamplesMigrationPlan.md` に集約します。

### 依存グラフ

モジュールは `Foundation` を最下層とする階層 DAG を成します。各 module の
`Build.cs` が `PublicDeps` / `PrivateDeps` を宣言し、生成された `Module.cmake` を
`acs_finalize_modules()` が読みます。構成時にはすべての依存先が有効か検証します。
おおまかには：

- `Foundation` は何にも依存しない。
- `Threading` / `Memory` / `Container` / `Math` / `Timing` / `Test` は `Foundation` の上に乗る。
- `Container` の所有権、allocator、失敗、参照無効化は
  [ContainerContracts.md](ContainerContracts.md) を正規契約とする。
- `Subsystem` は `Foundation` / `Memory` / `Container` の上で、明確な owner、寿命、
  更新または終了処理を持つ共有 service だけを扱う。局所状態と値計算は通常の型へ置く。
- `Platform` / `Ecs` / `Event` が OS・エンティティ・メッセージング層を足す。
- `Asset` と `Render` がコンテンツ読み込みとグラフィックスを提供する。
- `App` がウィンドウ + レンダラ + ECS を `FApplication` ループにまとめる。
- `Audio` / `Network` / `Imgui` / `Mvvm` / `Ui` / `Easy` / `AssetPack` /
  `GameFramework` / `Collision` が標準構成の高水準機能を提供する。
- `Network` の WinSock I/O 境界と 0 バイト通信は
  [NetworkSocketSafety.md](NetworkSocketSafety.md) の契約に従う。
- `Timing` の固定更新時計は利用側が値所有し、GameFrameworkのシーン時間やWorld共有時計を置換しない。
  一括処理と保存値の契約は[FixedStepClock.md](FixedStepClock.md)に従う。
- `Steamworks` / `Scripting` / `MlOnnx` / `OpenXr` / `CrashWin` /
  `TelemetryFile` / `LocalMatch` は構成時に明示して有効化する任意backendである。

## 主要な設計判断

| 機能領域 | 現在の構成 | 契約 |
|---|---|---|
| FLogger | slot ごとの sequence 値を持つ有界 MPMC ring、writer thread、固定購読表 | producer は 1 回の CAS で slot を予約し、writer が排出後に複数通知先を登録順で呼ぶ。詳細は [複数ログ通知先の契約](LogSinkSubscriptions.md) を参照。 |
| CThreadPool | worker ごとの単一 producer / 複数 consumer queue と、`FMutex` で保護する外部投入列 | owner worker の Push / Pop は通常経路で lock を取らず、外部投入を明示した共有列へ分離する。`Wait()` 中の worker も未完了 task を処理する。 |
| TAtomic | Win32 `_Interlocked*` 組み込み | ARM64 では acquire / release 接尾辞、x64 では full fence を基準に、ACS memory order を実装する。 |
| THashMap | 探索距離と 8-bit 指紋を持つ bucket 列、密な value 配列 | 不在検索は探索距離で打ち切り、削除時は bucket 列を詰める。value は連続領域を range-for で走査できる。詳細は [Container 契約](ContainerContracts.md) を参照。 |
| IAllocator 群 | `IAllocator` 境界と `CSystemAllocator` / `CLinearAllocator` / `CPoolAllocator` / `CArenaAllocator` | `CPoolAllocator` は free list と所有状態を同じ lock で保護する。`CArenaAllocator` は batch 予約と世代付き reset で、利用者統計を保ちながら page を再利用する。 |
| Math | DirectXMath を `FVec3 / FVec4 / FMat4 / FQuat` で包む | ACS value 型を公開し、実行時 CPU 検出で選んだ関数 table へ batch 演算を dispatch する。 |
| FString | 24 バイトの SSO union（22 バイトをインライン）+ 8 バイトの allocator 所有参照 | 全体は Win64 で 32 バイト。短い文字列は確保せず、長い文字列だけヒープへ移す。 |
| Test | `ACS_TEST(Suite, Name)` マクロ、`EXPECT_*`、`FMutex` 保護の registry | test ごとの失敗数を集計し、同一 binary 内の module test を一つの runner から実行する。 |

## 新しいモジュールの追加

1. `src/mymod/MyMod.Build.cs` を作る：
    ```csharp
    using Acs.Build;
    namespace Acs.Modules;

    public sealed class MyMod : AcsModule
    {
        public MyMod()
        {
            Type = ModuleType.Runtime;
            PublicDeps.AddRange(new[] { "Foundation", "Container" });
            Feature("FANCY", "MYMOD_FANCY", def: false,
                desc: "Enable the MyMod fancy feature");
        }
    }
    ```
2. ソース/ヘッダファイルを同じディレクトリに置く。
3. `dotnet run --project tools/acsbuild -- gen` で `Module.cmake` を生成する。
4. `modules.cmake` で有効化する：
    ```cmake
    acs_enable_module(MyMod FEATURES FANCY)
    ```
5. CMake を再構成する — 新しい `ACS::MyMod` ターゲットが現れ、モジュール
   define `WITH_ACS_MYMOD=1` と機能 define `WITH_MYMOD_FANCY=1` が PUBLIC
   利用者から見えるようになる。

## レンダラバックエンド

`Render` モジュールは 2 つの交換可能な RHI バックエンドをサポートし、構成時に
選択します（`CMakePresets.json` 参照）：

| バックエンド | CMake オプション | 備考 |
|---|---|---|
| Raw DX12 | `ACS_RENDER_DX12_RAW=ON`（既定） | 自前の DirectX 12 バックエンド。 |
| Diligent | `ACS_RENDER_DILIGENT=ON` | ACS の Diligent adapter と取得済み static dependency を使う。 |

`CreateRhiDevice()` がリンクされたバックエンドへディスパッチします。ECS は
ACS の公開所有型契約を守り、`TArray<T>` 上の sparse-set 設計を用います。
アセット（画像・glTF/FBX メッシュ・音声）は `FAssetRegistry` 経由で読み込まれ、
非同期ロードも選べます。ImGui 統合は現状 raw DX12 バックエンドのみを対象と
しています。

### ボリュメトリック雲の workload 診断

`FVolumetricClouds::LastFrameWorkload()` は、直近の cloud compute/composite が
実際に投入した仕事量を allocation なしで保持します。定常フレームの trace と
full-resolution resolve、初回だけの shape/weather/detail/curl bake、任意の
shadow-cache rebuild を別々の dispatch 数として記録します。logical invocation
は有効 texel/voxel 数、launched thread は 8x8 または 4x4x4 workgroup の端数を
含む実 dispatch 範囲です。

`maximum_view_samples` と `maximum_light_samples` は shader loop の保守的な上限で、
empty-space skipping、透過率 early exit、画面内容による分岐後の実行数では
ありません。したがって最適化判断では、これらのカウンタで「初回 bake」と
「定常処理」を分離し、RHI の cloud GPU timestamp を実時間の正本として併記します。
この診断は描画解像度、march 数、lighting 数、temporal reconstruction を変更せず、
計測のために画質を下げません。算術は `u64` 飽和で、異常な診断入力が小さい値へ
wrap することも防ぎます。

地面接線の `local_up` と角度 cutoff は camera/world-origin/cloud-layer ごとの
frame 不変値として CPU で一度だけ計算し、`CloudCB` の c20 から trace/resolve の
両方へ渡します。HLSL `cloudAltitude` と同じ座標契約を使い、接線近傍の二軸
neighbor-ray coverage を維持します。full-resolution resolve は temporal
reprojection で既に求めた center ray の elevation も再利用し、未計算の経路だけ
unproject します。この最適化は dispatch 数、trace 解像度、march/light step 数を
変えないため、効果量は profiler の cloud GPU timestamp で実測してください。

密な上向き視点では、同じ world wind、shape frequency、layer-height reciprocal が
view sample と最大 8 個の light-cone probe で再評価されていました。これらは
`CloudCB` c21 へ frame ごとに一度だけ格納し、density/weather/curl/detail の
world-space 座標は変更せずに再利用します。正規化済み sun と連続な正規直交基底も
c22-c23 へ CPU で構築し、各 trace invocation の重複 normalize / basis 構築を除きます。
CPU と shader の高さ率、基底、cone direction は代表値、layer 境界、天頂、地平、
下半球方向で数値一致を検証し、HLSL の実コンパイルも unit test に含めます。

`CloudMacroSample` は base-shape reject に使う `heightThreshold` も保持します。
view ray は同じ profile から occupancy coverage と authored density coverage を
一度ずつ作り、light probe は必要な coverage 1 値を後段へ渡します。macro struct の
float 数、noise fetch、light-cone sample 位置、early-exit 条件は固定します。

エディタ側は volumetric cloud の `EnsureSize` が成功した frame だけ
`cloudsActive` を立て、その値で `FSky` の 48x3 cloud march を無効にします。
compute path と fallback は同じ frame で二重実行されません。full-resolution resolve の
empty-sky short path は履歴の 5x5 footprint で silhouette と horizon を保護します。
192 view steps、8 light probes、Ultra 4x4 phase、trace / output dispatch、履歴規則を
固定したまま workload を計測します。
