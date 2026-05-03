# Arts Common System (ACS)

> ゲーム開発の基盤となる、モジュール式 C++20 ランタイム。
> Originally developed by students at [Arts College Yokohama](https://www.kccollege.ac.jp/).

---

## 概要

ACS はゲーム制作の現場で頻出する処理を共通化・再利用することを目的とした
**コアランタイム**です。Phase 1 では以下を**ゼロから**再構築しました（既存
DXLib ベース実装を完全に廃棄）：

- **STL を使用しない** — `Array<T>`, `String`, `HashMap<K,V>`, `Atomic<T>`,
  `Thread`, `Mutex`, `RwLock`, `UniquePtr<T>`, `Rc<T>` などを自前実装
- **全機能スレッドセーフ** — ロックフリー / Win32 SRWLOCK / `_Interlocked*`
- **SIMD 対応** — DirectXMath を x64 バックエンドにラップ、CPU 機能の実行時検出
- **詳細なエラー報告** — `Result<T,E>`, `ACS_ASSERT`, スタックトレース付き Panic
- **UE 風モジュールシステム** — `modules.cmake` でモジュールと機能を選択

## Phase 1 モジュール

| モジュール | 主要内容 |
|---|---|
| `Foundation` | Types, SourceLoc, Result, Assert, Panic, StackTrace, async Logger |
| `Threading`  | Atomic, Mutex, RwLock, ConditionVar, Thread, work-stealing ThreadPool |
| `Memory`     | Allocator IF, System/Linear/Pool(lock-free)/Arena allocators, UniquePtr, Rc |
| `Container`  | Span, Array, String (SSO), StringView, HashMap (Robin-Hood), Hash |
| `Math`       | Vec2/3/4, Mat4, Quat (DirectXMath wrapper), CPU detect, runtime dispatch |
| `Test`       | `ACS_TEST` マクロ + `EXPECT_*` 群 + ランナー |

詳細は [`acs/docs/ARCHITECTURE.md`](acs/docs/ARCHITECTURE.md) を参照。

## ビルド

要件: Windows + Visual Studio 2022 (MSVC) または Clang-cl, CMake 3.24+

```pwsh
cd acs
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## モジュールの選択（UE 風）

`acs/modules.cmake` を編集して、ビルドするモジュールと機能を選択：

```cmake
acs_enable_module(Foundation REQUIRED FEATURES STACKTRACE LOG_FILE_SINK)
acs_enable_module(Math       REQUIRED FEATURES DIRECTXMATH AVX2)
# acs_enable_module(Render            FEATURES DX12_RAW)   # Phase 2
```

各モジュールには `WITH_ACS_<NAME>=1`、各機能には `WITH_<FEATURE>=1` の
プリプロセッサ define が自動的に付与され、コード側で `#if` ガード可能。

## Phase 2 予定

| モジュール | 候補バックエンド |
|---|---|
| Render | DirectX 12 (raw + DirectX-Headers + D3D12MA), The-Forge, または bgfx (一つ採用) |
| Asset  | DDS / glTF / wav 等 |
| ECS    | entt または自前 archetype |
| Editor | Dear ImGui (DX12 バックエンド) |

## ライセンス

教育目的での使用に限定されています。商用利用・外部配布は事前許可が必要です。
