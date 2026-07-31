# ACS アーキテクチャ

本書は Arts Common System（ACS）のアーキテクチャを説明します。ACS は
Windows / DirectX 12 向けのモジュール式 C++20 ゲームフレームワークです。
ACS はランタイム基盤（型・スレッド・メモリ・コンテナ・数学）から始まり、
現在はプラットフォーム・ECS・イベント・アセット・描画・音声・ネットワーク・
UI までを含みます。現在は `src/*/Module.cmake` に **28 モジュール**を定義し、
標準構成の21モジュールと構成時に選ぶ7個の任意backendに分けています。

## 設計原則

1. **公開所有ABIへSTL型を持ち込まない。** 動的所有には `std::vector`,
   `std::string`, `std::function` ではなく `TArray<T>`, `FString` と関数pointerを
   使い、threadingも `TAtomic<T>`, `FThread`, `FMutex` を標準の境界にします。
   template trait、局所algorithm、時刻取得などの実装補助には `<type_traits>`,
   `<algorithm>`, `<atomic>`, `<chrono>` を必要な翻訳単位またはheaderで使用しますが、
   STL containerの所有権を公開APIへ露出しません。C標準header、compiler組み込み、
   Windows SDKとDirectXMathも明示的に利用します。
2. **例外なし・RTTI なし。** ACS sourceのエラーは `TResult<T, FErrorCode>` で伝搬します。
   raw-DX12構成は `/EHs-c- /GR- /D_HAS_EXCEPTIONS=0` です。Diligent static libraryは
   内部で例外とMSVC STLの例外型を使うため、Diligent構成のACS targetだけ
   `/EHsc /GR- /D_HAS_EXCEPTIONS=1` として最終binary内のSTL ABIを一致させます。
   これはcompiler ABIの境界条件であり、ACS sourceの `throw` / `try` / `catch`
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
6. **Unreal 風のモジュール構成。** モジュールは `src/<mod>/Module.cmake` から
   発見され、ユーザーが編集する `modules.cmake` で有効化されます。有効化された
   各モジュールは静的ライブラリターゲット `ACS::<Name>` と、コンパイル時 define
   `WITH_ACS_<NAME>=1` を公開します。モジュール個別の機能には `WITH_<FEATURE>=1`
   が付きます。

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
├── src/                        # 各 <mod>/ に Module.cmake がある
│   ├── foundation/             # Types, FSourceLoc, TResult, Assert, Panic, Log
│   ├── threading/              # TAtomic, FMutex, FRwLock, FThread, FThreadPool, FJobGraph
│   ├── memory/                 # FAllocator, FSystemAllocator/FLinearAllocator/FPoolAllocator/FArenaAllocator, TUniquePtr, TRc
│   ├── container/              # TArray, FString, THashMap, Hash, TSpan
│   ├── math/                   # FVec2/FVec3/FVec4, FMat4, FQuat, FCamera, Collision2D/3D, dispatch
│   ├── test/                   # 小さなテストフレームワーク + EXPECT_* マクロ
│   ├── platform/               # FWindow, FInput, FClock/FFrameTimer, FFileSystem, FStorage, FLocalization
│   ├── ecs/                    # FWorld, FEntityId, TQueryView, FSystemScheduler
│   ├── event/                  # FTimerManager, FMessageBroker (pub/sub)
│   ├── asset/                  # FAssetRegistry, 画像/メッシュ/音声ローダ, 非同期ロード
│   ├── render/                 # FRenderer, IRhiDevice, FStandardShader, FPbrShader, FSpriteBatch
│   ├── app/                    # FApplication, FAppConfig, ACS_DEFINE_MAIN
│   ├── audio/                  # XAudio2 再生
│   ├── network/                # TCP ソケット
│   ├── imgui/                  # Dear ImGui 統合
│   ├── mvvm/                   # MVVM データバインディング
│   ├── ui/                     # FWidget ベースの UI フレームワーク
│   ├── easy/                   # 初学者向けの高水準 API
│   ├── assetpack/              # .acpak の作成・読み取り
│   ├── gameframework/          # Scene、Node、ゲーム機能
│   ├── collision/              # 2D / 3D 衝突判定
│   └── <optional>/             # Steamworks、Lua、ONNX、OpenXR、crash、telemetry、local match
├── samples/                    # 68 個（00〜67）の番号付きサンプル (samples/README.md 参照)
└── tests/                      # モジュール単体テスト
```

`samples` は [学習用サンプルへの全面移行計画](LearningSamplesMigrationPlan.md) に従い、
現在の検証責務を新しい段階別サンプルへ移した後、既存群をすべて削除して置き換えます。
移行中は公開API、optional module、Debug/Release、実行、単一ヘッダー、配布のcoverageを失わないことを
削除条件とします。

### 依存グラフ

モジュールは `Foundation` を最下層とする階層 DAG を成します。各モジュールは
`src/<mod>/Module.cmake` で `PUBLIC_DEPS` / `PRIVATE_DEPS` を宣言し、
`acs_finalize_modules()` が構成時にすべての依存先も有効化されているか検証します。
おおまかには：

- `Foundation` は何にも依存しない。
- `Threading` / `Memory` / `Container` / `Math` / `Test` は `Foundation` の上に乗る。
- `Platform` / `Ecs` / `Event` が OS・エンティティ・メッセージング層を足す。
- `Asset` と `Render` がコンテンツ読み込みとグラフィックスを提供する。
- `App` がウィンドウ + レンダラ + ECS を `FApplication` ループにまとめる。
- `Audio` / `Network` / `Imgui` / `Mvvm` / `Ui` / `Easy` / `AssetPack` /
  `GameFramework` / `Collision` が標準構成の高水準機能を提供する。
- `Network` の WinSock I/O 境界と 0 バイト通信は
  [NetworkSocketSafety.md](NetworkSocketSafety.md) の契約に従う。
- `Steamworks` / `Scripting` / `MlOnnx` / `OpenXr` / `CrashWin` /
  `TelemetryFile` / `LocalMatch` は構成時に明示して有効化する任意backendである。

## 主要な設計判断

| サブシステム | 採用したもの | 理由 |
|---|---|---|
| FLogger | セルごとの Vyukov 有界 MPMC リング + writer スレッド | プロデューサのホットパスは CAS 1 回。writer がロックフリーに排出する。参考: 1024cores.net Vyukov MPMC, Quill async logger。 |
| FThreadPool | worker ごとの Chase-Lev SPMC deque + グローバル FMutex 投入 | 所有者の Push/Pop は通常ケースでアトミック操作なし。外部投入は FMutex 保護のフォールバックを通る。Steal は `Wait()` に参加してデッドロックを回避。参考: Chase & Lev SPAA 2005, enkiTS, Naughty Dog GDC 2015。 |
| TAtomic | Win32 `_Interlocked*` 組み込み | `std::atomic` 不使用。ARM64 では接尾辞付き（`_acq` / `_rel`）、x64 ではフルフェンスをベースラインに。 |
| THashMap | Robin Hood + 値の密配置 + 8-bit フィンガープリント | ankerl::unordered_dense レイアウト — 失敗ルックアップが最速、tombstone なし、連続イテレーション可。SIMD プロービングは v2 に延期。 |
| FAllocator 群 | 仮想 `FAllocator` 基底 + FSystemAllocator / FLinearAllocator / FPoolAllocator / FArenaAllocator | FPoolAllocator は単方向フリーリストと所有状態を同じ軽量ロックで保護する。FArenaAllocator は batch 予約と世代式 `Reset(false)` により、利用者統計を保ったまま cursor 更新と毎 frame の page 走査を削減する。 |
| Math | DirectXMath を `FVec3 / FVec4 / FMat4 / FQuat` でラップ | Microsoft 保守、SSE2〜AVX2 パス同梱、Windows 上 NEON 対応も視野。人間に優しい POD 型を公開し、バッチ演算は関数ポインタテーブルでディスパッチ。 |
| FString | 24 バイトの SSO union（22 バイトをインライン）+ 8 バイトの allocator 所有参照 | 全体は Win64 で 32 バイト。短い文字列は確保せず、長い文字列だけヒープへ移す。 |
| Test | 独自 `ACS_TEST(Suite, Name)` マクロ + `EXPECT_*` | GoogleTest 依存を避ける。FMutex 保護のレジストリ、テストごとの失敗カウンタ。 |

## 新しいモジュールの追加

1. `src/mymod/Module.cmake` を作る：
    ```cmake
    acs_module(
        NAME    MyMod
        TYPE    Runtime
        SOURCES Foo.cpp Bar.cpp
        HEADERS Foo.h Bar.h
        PUBLIC_DEPS Foundation Container
    )
    acs_module_feature(MODULE MyMod NAME FANCY
        DEFINE MYMOD_FANCY DESCRIPTION "Enable fancy mode" DEFAULT OFF)
    ```
2. ソース/ヘッダファイルを同じディレクトリに置く。
3. `modules.cmake` で有効化する：
    ```cmake
    acs_enable_module(MyMod FEATURES FANCY)
    ```
4. CMake を再構成する — 新しい `ACS::MyMod` ターゲットが現れ、モジュール
   define `WITH_ACS_MYMOD=1` と機能 define `WITH_MYMOD_FANCY=1` が PUBLIC
   利用者から見えるようになる。

## レンダラバックエンド

`Render` モジュールは 2 つの交換可能な RHI バックエンドをサポートし、構成時に
選択します（`CMakePresets.json` 参照）：

| バックエンド | CMake オプション | 備考 |
|---|---|---|
| Raw DX12 | `ACS_RENDER_DX12_RAW=ON`（既定） | 自前の DirectX 12 バックエンド。 |
| Diligent | `ACS_RENDER_DILIGENT=ON` | Diligent Engine 経由。初回構成でクローンする（約 10 分）。Vulkan / クロスプラットフォームへの道。 |

`CreateRhiDevice()` がリンクされたバックエンドへディスパッチします。ECS は
no-STL 不変条件を守るため `TArray<T>` 上の自前 sparse-set 設計を用います。
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
両方へ渡します。式は従来の HLSL `cloudAltitude` と同型で、接線近傍の二軸
neighbor-ray coverage はそのまま残します。full-resolution resolve は temporal
reprojection で既に求めた center ray の elevation も再利用し、未計算の経路だけ
unproject します。この最適化は dispatch 数、trace 解像度、march/light step 数を
変えないため、効果量は profiler の cloud GPU timestamp で実測してください。

密な上向き視点では、同じ world wind、shape frequency、layer-height reciprocal が
view sample と最大 8 個の light-cone probe で再評価されていました。これらは
`CloudCB` c21 へ frame ごとに一度だけ格納し、density/weather/curl/detail の
world-space 座標は変更せずに再利用します。正規化済み sun と連続
Duff/Frisvad basis も c22-c23 へ CPU で構築し、各 trace invocation の重複
normalize/basis 構築を除きます。旧式と hoist 後の高さ率、基底、cone direction は
代表値・layer 境界・天頂/地平/下半球方向で数値比較し、HLSL の実コンパイルも
ユニットテストに含めます。

`CloudMacroSample` は base-shape reject に使った `heightThreshold` も保持します。
従来は同じ profile/coverage 式を直後の shape/density 評価でも再計算しており、
密度のある view sample と各 light probe で二重でした。view ray は保守的な
occupancy coverage と authored density coverage の2値を同じ profile から一度ずつ
作り、light probe は1値だけを作って後段へ渡します。macro struct のfloat数、
noise fetch、light-cone sample位置、early-exit条件は変わりません。

エディタ側は volumetric cloud の `EnsureSize` が成功した frame だけ
`cloudsActive` を立て、その値で `FSky` の旧 48x3 cloud march を無効にするため、
modern path と fallback は二重実行されません。full-resolution resolve の
empty-sky short path は履歴の 5x5 footprint で silhouette/horizon を保護しており、
単一 low texel だけの早期判定へ縮めると過去のジャギーを再導入するため維持します。
この第2段の最適化でも 192 view steps、8 light probes、Ultra 4x4 phase、
trace/output dispatch と履歴規則は不変です。
