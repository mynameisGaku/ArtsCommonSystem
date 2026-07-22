# ACS アーキテクチャ

本書は Arts Common System（ACS）のアーキテクチャを説明します。ACS は
Windows / DirectX 12 向けのモジュール式 C++20 ゲームフレームワークです。
ACS はランタイム基盤（型・スレッド・メモリ・コンテナ・数学）から始まり、
現在はプラットフォーム・ECS・イベント・アセット・描画・音声・ネットワーク・
UI までを含みます。**下記の 17 モジュールはすべて実装済みです。**

## 設計原則

1. **STL を使わない。** `std::vector`, `std::string`, `std::atomic`,
   `std::thread`, `std::mutex`, `std::function`, `<algorithm>`,
   `<type_traits>` などは使用しません。代替は `acs::` 名前空間にあります
   （`TArray<T>`, `FString`, `TAtomic<T>`, `FThread`, `FMutex`, ...）。C 標準
   ヘッダ（`<cstdint>`, `<cstddef>`, `<cstring>`, `<cstdio>`, `<cmath>`）、
   コンパイラ組み込み（`<intrin.h>`, `<immintrin.h>`）、Windows SDK
   （`<windows.h>`, `<DbgHelp.h>`, `<DirectXMath.h>`）は明示的に許可します。
2. **例外なし・RTTI なし。** エラーは `TResult<T, FErrorCode>` で伝搬します。
   コンパイラは `/EHs-c- /GR- /D_HAS_EXCEPTIONS=0` で構成されます。
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
│   └── ui/                     # FWidget ベースの UI フレームワーク
├── samples/                    # 26 個のサンプルプログラム (samples/README.md 参照)
└── tests/                      # モジュール単体テスト
```

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
- `Audio` / `Network` / `Imgui` / `Mvvm` / `Ui` はより高レベルの opt-in。

## 主要な設計判断

| サブシステム | 採用したもの | 理由 |
|---|---|---|
| FLogger | セルごとの Vyukov 有界 MPMC リング + writer スレッド | プロデューサのホットパスは CAS 1 回。writer がロックフリーに排出する。参考: 1024cores.net Vyukov MPMC, Quill async logger。 |
| FThreadPool | worker ごとの Chase-Lev SPMC deque + グローバル FMutex 投入 | 所有者の Push/Pop は通常ケースでアトミック操作なし。外部投入は FMutex 保護のフォールバックを通る。Steal は `Wait()` に参加してデッドロックを回避。参考: Chase & Lev SPAA 2005, enkiTS, Naughty Dog GDC 2015。 |
| TAtomic | Win32 `_Interlocked*` 組み込み | `std::atomic` 不使用。ARM64 では接尾辞付き（`_acq` / `_rel`）、x64 ではフルフェンスをベースラインに。 |
| THashMap | Robin Hood + 値の密配置 + 8-bit フィンガープリント | ankerl::unordered_dense レイアウト — 失敗ルックアップが最速、tombstone なし、連続イテレーション可。SIMD プロービングは v2 に延期。 |
| FAllocator 群 | 仮想 `FAllocator` 基底 + FSystemAllocator / FLinearAllocator / FPoolAllocator / FArenaAllocator | FPoolAllocator は単方向フリーリストと所有状態を同じ軽量ロックで保護し、外部ポインタや二重解放も拒否する。 |
| Math | DirectXMath を `FVec3 / FVec4 / FMat4 / FQuat` でラップ | Microsoft 保守、SSE2〜AVX2 パス同梱、Windows 上 NEON 対応も視野。人間に優しい POD 型を公開し、バッチ演算は関数ポインタテーブルでディスパッチ。 |
| FString | 24 バイトの SSO（22 バイトをインライン）+ ヒープフォールバック | absl/folly 風のレイアウト。x64 のキャッシュライン 1/3 程度のサイズに合わせている。 |
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
