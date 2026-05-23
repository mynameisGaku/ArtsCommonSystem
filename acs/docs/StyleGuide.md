<!-- SPDX-License-Identifier: Apache-2.0 -->
# ACS Coding Style Guide (v1)

**目的 / Purpose**: ACS の唯一のコーディング規約。`.clang-format` / `.clang-tidy` / `acs_lint` が機械強制する内容と一対一で対応する。**This document is the single source of truth for ACS coding style** — what tools enforce, this doc explains.

**対象 / Scope**: `src/**`, `samples/**`, `tests/**` 配下の C++ コード。`cmake-build-*/_deps/` 配下のサードパーティ、`docs/`, `cmake/` は対象外。

**バージョン / Version**: v1 (2026-05-21)

> **基本方針 / Core philosophy** — ACS は既に内部で一貫したスタイルを持つ。本書はそれを **codify** するもので、新規ルールを多数導入するものではない。例外的な変更は §15 (Revision history) で追跡する。

---

## 目次 / Table of Contents

1. [基本不変条件 / Core Invariants](#1-基本不変条件--core-invariants)
2. [命名 / Naming](#2-命名--naming)
3. [フォーマット / Formatting](#3-フォーマット--formatting)
4. [ファイル / Files](#4-ファイル--files)
5. [エラー処理 / Error Handling](#5-エラー処理--error-handling)
6. [メモリ / Memory](#6-メモリ--memory)
7. [並行性 / Concurrency](#7-並行性--concurrency)
8. [テンプレート / Templates](#8-テンプレート--templates)
9. [モダン C++ 機能 / Modern C++ Features](#9-モダン-c-機能--modern-c-features)
10. [コメント / Comments](#10-コメント--comments)
11. [プラットフォーム移植性 / Portability](#11-プラットフォーム移植性--portability)
12. [ツールチェイン / Toolchain](#12-ツールチェイン--toolchain)
13. [規則カタログ / Rule Catalog (R001-R048)](#13-規則カタログ--rule-catalog-r001-r048)
14. [例外規定 / Exception Mechanism](#14-例外規定--exception-mechanism)
15. [改訂履歴 / Revision History](#15-改訂履歴--revision-history)

---

## 1. 基本不変条件 / Core Invariants

ACS は以下の **5 つの言語制約** の上に成り立つ。これらは規約というより設計の前提であり、変更不可。

| 不変条件 / Invariant | 説明 / Description |
|---|---|
| **No STL** | `<vector>`, `<string>`, `<unordered_map>`, `<memory>`, `<functional>` 等の STL コンテナ・ユーティリティを使わない。代わりに `acs::Array<T>`, `acs::String`, `acs::HashMap<K,V>`, `acs::UniquePtr<T>`, `acs::Rc<T>`, `acs::Callback` を使う。 |
| **No exceptions** | `throw` / `try` / `catch` を使わない。エラーは `Result<T, E>` で返す。すべての関数に `noexcept` を付ける。 |
| **No RTTI** | `dynamic_cast` / `typeid` を使わない。`-fno-rtti` (Clang/GCC) または `/GR-` (MSVC) でビルドする。型識別が必要なら手書きのタグ enum を使う。 |
| **`Result<T, E>`** | 失敗しうる関数は `Result<T, ErrorCode>` を返す。`[[nodiscard]]` でクラス自身が修飾されており、戻り値を捨てるとビルド警告。 |
| **canonical Callback** | コールバックは `using Cb = void (*)(/* payload */, void* user);` 形式 (関数ポインタ + `void*`)。`std::function` 不使用。 |

---

## 2. 命名 / Naming

### 2.1 型 / Types — `PascalCase`

```cpp
class  Renderer { /* ... */ };
struct ErrorCode { /* ... */ };
enum class ELogSeverity : u8 { Trace, Debug, Info, Warn, Error, Fatal };
template<typename T> class Array { /* ... */ };
```

**例外 / Exception**: プリミティブのエイリアス (`u8`, `u32`, `i64`, `f32`, `usize` 等) は小文字。これらは `foundation/Types.h` で定義されており、ビルトイン同等に扱う。

### 2.2 関数 / メソッド — `PascalCase`

```cpp
void BeginFrame() noexcept;
bool IsOk() const noexcept;
static UniquePtr<Renderer> Create(/* ... */);
```

**例外 / Exception**:
- `begin()` / `end()` / `cbegin()` / `cend()`: range-for 互換のため小文字必須。
- `operator==` / `operator[]` 等: 言語規定により小文字。
- HLSL シェーダ内 pass を C++ 側から呼ぶ場合の `Pass_<Stage>` 形式 (e.g. `Pass_Tonemap`, `Pass_TaaResolve`) は許可。

### 2.3 ローカル変数 / 引数 — `snake_case`

```cpp
void Foo(u32 frame_index, const Mat4& view_proj) noexcept {
    const f32 dt = ...;
    bool has_changed = false;
}
```

**例外 / Exception**: 数学コード内の 1 文字変数 (`x`, `y`, `z`, `t`, `r`, `g`, `b`, `a`, `i`, `j`, `n`, `dx`, `dy`) は許可。

### 2.4 クラス・メンバ変数 / Class Members

| 種別 / Kind | 規則 / Rule | 例 / Example |
|---|---|---|
| **private / protected member** | `_snake_case` (先頭アンダースコア) | `_data`, `_capacity`, `_has_value` |
| **public POD struct field** | `snake_case` (アンダースコアなし) | `ErrorCode::message`, `LogConfig::file_path` |
| **`static` class member** | `_snake_case` の場合は `_` のみ、global 風には `s_` 接頭辞 | (慣行は `_snake` で統一) |

**理由 / Rationale**: 既存コード (`src/foundation/Result.h`, `src/container/Array.h`) と Cocos2d-x の慣習に整合。`m_` プレフィックス (UE / Microsoft / Naughty Dog 流) は採用しない。

### 2.5 定数 / Constants — `kPascalCase`

```cpp
inline constexpr usize  kSsoCapacity      = 22;
static constexpr u32    kBloomMips        = 6;
static constexpr i32    kInvalidEntityId  = -1;
```

すべての `constexpr` グローバル / クラス静的 / 名前空間スコープ定数に `k` プレフィックス + PascalCase。

**例外 / Exception**: TLSF 等の文献に従う実装は `SCREAMING_SNAKE_CASE` でも可 (`FL_INDEX_MAX`, `SL_INDEX_LOG2`, `MIN_BLOCK_SIZE` 等)。これは reference 実装との対応を保つため。

### 2.6 マクロ / Macros — `ACS_SCREAMING_SNAKE`

```cpp
#define ACS_ASSERT(expr)        /* ... */
#define ACS_LOG_INFO(fmt, ...)  /* ... */
#define ACS_FORCEINLINE         /* ... */
```

ACS の公開マクロは必ず `ACS_` プレフィックス。内部ヘルパーは `_acs_` (小文字) 始まり。

### 2.7 `enum class` — `E` プレフィックス + `PascalCase` (Phase 19a〜)

```cpp
enum class ErrCategory  : u16 { None, Generic, Memory, OS, IO, Container, /* ... */ };  // 既に E 始まり、二重 E 不要
enum class ELogSeverity : u8  { Trace, Debug, Info, Warn, Error, Fatal, Off };
enum class EFlowState   : u8  { Splash, MainTitle, MainMenu, /* ... */ };
```

- 型名: **`E` プレフィックス + PascalCase** (Phase 19a で確定)。
- 値: PascalCase。
- 必ず **underlying type を明示** (`: u8` / `: u16` / `: u32`)。
- 必ず **`enum class`** (素の `enum` は禁止)。

**例外 / Exceptions**:
- 既に `E` で始まる単語 (e.g., `ErrCategory`, `EmitterHandle`, `EventType`) は二重 E を避けて従来名のまま。
- HLSL format 値 (`R8`, `R8G8B8A8`, `R32G32B32_F` 等) は HLSL 慣習に従う (型名は `EPixelFormat` で E prefix 適用)。

**Phase 19a の経緯**: ACS は元々 enum class に prefix なしだったが、UE5 経験者の親和性 + grep ヒット率向上 + interface の `I` prefix と整合させるため `E` prefix を必須化した。`scripts/rename_enums_to_e_prefix.py` で機械的に rename 済み (~80 enum / 254 file / 2551 replacement)。

**例外 / Exception**: HLSL の format に対応する `EPixelFormat` 値 (`R8`, `R8G8B8A8`, `R32G32B32_F` 等) は HLSL 慣習に従う (値名のみ、型名は `E` prefix 適用)。

### 2.8 名前空間 / Namespaces — 小文字

```cpp
namespace acs            { /* top-level */ }
namespace acs::easy      { /* easy facade */ }
namespace acs::game      { /* GameFramework */ }
namespace acs::detail    { /* impl-only */ }
namespace acs::*_detail  { /* per-module impl-only */ }
```

すべて小文字。`detail` または `<module>_detail` で実装専用のサブ名前空間を作る。

### 2.9 インタフェース / Interfaces — `I` プレフィックス

```cpp
class IRhiDevice    { virtual ~IRhiDevice() noexcept = default; /* ... */ };
class IRhiBuffer    { /* ... */ };
class IAssetLoader  { /* ... */ };
```

純粋仮想 (= 0) ベース抽象クラスにのみ `I` プレフィックスを付ける。これは ACS 既存の de facto 規約 (`src/render/IRhi*.h`, `src/asset/IAssetLoader.h`)。

### 2.10 ファイル名 / File Names — `PascalCase.{h,cpp}`

```
src/foundation/Result.h        Result.cpp
src/container/Array.h          Array.cpp
src/render/Diligent/DiligentDevice.h
```

主要型と同名。Win32 / cmake / make 系ファイル名は慣習に従う (`CMakeLists.txt`, `Module.cmake`)。

### 2.11 bool 命名 / Boolean Naming

- **メソッド**: `IsXxx()`, `HasXxx()`, `CanXxx()`, `ShouldXxx()` 形式。
- **メンバ・変数**: `is_xxx`, `has_xxx`, `should_xxx`, `can_xxx` 接頭辞。**UE 流 `b` プレフィックスは不採用** (snake_case と衝突するため)。

```cpp
// OK
bool IsAlive() const noexcept;
bool _is_active = false;
bool has_pending_destroy = true;

// NG (UE 流、ACS では使わない)
bool bIsActive;
bool bIsAlive;
```

---

## 3. フォーマット / Formatting

### 3.1 インデント — スペース 4

タブ禁止。4 スペース。`.editorconfig` が IDE を制御し、`.clang-format` が CI を制御する。

### 3.2 ブレース — K&R / Egyptian

開きブレースは同じ行に置く。`else` / `catch` は閉じブレース同行。

```cpp
if (condition) {
    DoX();
} else {
    DoY();
}

class Foo {
public:
    void Bar() {
        for (usize i = 0; i < n; ++i) {
            Step(i);
        }
    }
};
```

短い 1 行ボディはブレース省略可: `if (!ptr) return;` / `if (x < 0) x = 0;`。

### 3.3 行長 — 100 col target / 120 hard

通常コード ~100 列を目標、最大 120 列。テーブル整列や HLSL 文字列など長くなる場合は超えてもよい。

**日本語コメント考慮**: 全角 1 文字 ≒ 2 列換算で計算。`// 説明...` 内に日本語が多い場合は 60 半角 + 30 全角程度に収める。

### 3.4 `const` 配置 — west-const

```cpp
const int*  p;      // OK (west-const, pointer-to-const-int)
int* const  p;      // OK (const pointer)
const int& r;       // OK (reference to const)

int const*  p;      // NG (east-const, 採用しない)
```

ACS は **west-const** 統一。Win32 / DX12 / DirectXMath などの interop 先と整合。

### 3.5 ポインタ/参照配置 — west-pointer

```cpp
T*    p;      // OK
T&    r;      // OK
T *p;         // NG (east-pointer LLVM 流、採用しない)
```

**1 行 1 変数を強制**: `T* a, b;` の罠を避けるため、複数変数を 1 行で宣言しない。

```cpp
T* a;        // OK
T* b;        // OK
T* a, b;     // NG (a は T*、b は T、罠)
```

### 3.6 `auto` 使用方針 — 控えめ / Contextual

許可される使用:
- イテレータ (`auto it = ...begin()`)
- ラムダのキャプチャ (`auto fn = []() noexcept { ... };`)
- 右辺で型が明白 (`auto ptr = MakeUnique<Foo>();`)
- `Result<T>` 受け取り (`auto r = OpenFile(...);`)
- ポインタは `auto*` で明示 (`auto* p = Get();`)

禁止:
- プリミティブの省略 (`auto x = 5;` → `i32 x = 5;` と書く)
- 関数の戻り値型 (`auto Foo() { ... }` → 明示する)
- 公開 API の引数

### 3.7 Include 順序 / Include Order

各 `.cpp` ファイル:
1. **対応する自分のヘッダ** (`#include "easy/Easy.h"` ← Easy.cpp の場合)
2. **ACS プロジェクトヘッダ**: 依存層が低い順 (foundation → memory → container → threading → math → asset → audio → render → ui → platform)
3. **3rd-party**: `<imgui.h>`, `<DiligentCore/...>` 等
4. **C 標準ライブラリ**: `<cstdint>`, `<cstdio>` 等
5. **OS ヘッダ**: `<windows.h>`, `<DbgHelp.h>` (platform/ 以下のみ)

引用形式:
- ACS プロジェクトヘッダ: `"foundation/Types.h"` (引用、`src/` ルート相対)
- 3rd-party / system: `<header>` (山括弧)

`.h` ファイル: 自ヘッダ依存のみ include、`using` ディレクティブ禁止、forward declare 推奨。

### 3.8 Header Guard — `#pragma once`

すべてのヘッダで `#pragma once` を使う。`#ifndef ACS_FOO_H` 形式は使わない (LLVM/Google 流不採用)。

### 3.9 メンバー初期化 / Member Initialization

**in-class default initializer** を優先、コンストラクタ引数バインドのみ init list:

```cpp
class Array {
private:
    T*         _data     = nullptr;   // in-class default (preferred)
    usize      _size     = 0;
    usize      _capacity = 0;
    Allocator* _alloc    = nullptr;

public:
    Array(Allocator& a) noexcept : _alloc(&a) {}   // init list for arg binding
};
```

順序: **宣言順 = 初期化順** (init list の順番は宣言順に合わせる、`-Wreorder` 警告防止)。

### 3.10 初期化構文 / Initialization Syntax

- プリミティブ: `int x = 0;`, `f32 t = 0.0f;`, `bool ok = false;` (`=` 推奨)
- 集約: `Vec3 v{1.0f, 2.0f, 3.0f};` (`{}` 推奨、narrowing conversion 防止)
- コンストラクタ引数: `Array a(initial_cap, alloc);` (`()` 推奨、aggregate 衝突回避)

### 3.11 `using` vs `typedef` — `using` のみ

```cpp
using EntityId = u32;                    // OK
typedef u32 EntityId;                    // NG (古い C 流、不採用)
using Callback = void (*)(void*, u32);   // OK (function ptr alias)
template<typename T>
using Owned = UniquePtr<T>;              // OK (template alias)
```

### 3.12 Template 構文

```cpp
template<typename T>                          // OK
template<class T>                             // NG (典型的に typename を使う)
template<typename T, typename E = ErrorCode>  // OK
```

### 3.13 アクセス指定子順序 / Access Section Order

```cpp
class Foo {
public:                  // 1. public は最初
    Foo() noexcept = default;
    Foo(const Foo&) = delete;
    Foo& operator=(const Foo&) = delete;
    Foo(Foo&&) noexcept;
    Foo& operator=(Foo&&) noexcept;
    ~Foo() noexcept;

    // public API ...

protected:               // 2. protected (使うなら)
    /* ... */

private:                 // 3. private は最後
    // 内部状態 ...
};
```

1 つの class に 1 つの `public:` / `protected:` / `private:` (繰り返さない)。サブセクション分けは `// ---- セクション名 ----` で。

### 3.14 ファイル内ローカル / Anonymous Namespace

`.cpp` ファイル内のローカル関数・型・グローバル変数は **匿名 namespace** に入れる (`static` 修飾子は使わない):

```cpp
namespace acs {
namespace {  // anonymous namespace = TU-local linkage

constexpr u32 kMessageMax = 480;       // file-local constant
struct Cell { /* ... */ };              // file-local type
LoggerState g_state;                    // file-local global

void WriteAll(HANDLE h, /* ... */) noexcept {  // file-local function
    /* ... */
}

} // namespace
} // namespace acs
```

理由: `static` は関数のみ適用可能だが、匿名 namespace は型・テンプレート・グローバル変数すべてに適用できる。シンボル名のマングリングで `acs::(anonymous namespace)::WriteAll` のように表示され、スタックトレースの可読性も向上する。

### 3.15 出力パラメータ / Out-Parameters

**原則**: 失敗しうる関数は `Result<T>` を返す。多値返却は `struct` を返す。

```cpp
// OK — Result<T> で値を返す
Result<File> OpenFile(const char* path) noexcept;

// OK — 構造体で多値返却
struct RayHit { Vec3 point; f32 t; bool hit; };
RayHit Intersect(const Ray& r, const Sphere& s) noexcept;

// 例外的に許容 — 真にオプショナルな出力 (nullptr を渡せる)
void Compute(u32 in, u32* out_optional = nullptr) noexcept;

// NG — bool 戻り値 + out 参照は使わない
bool TryOpen(const char* path, File& out_file) noexcept;  // → Result<File> にする
```

ポインタ形式の out (`T* out`) が許容されるのは「呼び出し側が nullptr を渡すことで出力を抑制したい」場合のみ。それ以外は `Result<T>` または struct 戻り値を使う。

---

## 4. ファイル / Files

### 4.1 文字コード — UTF-8 (BOM なし)

すべてのソースファイルは UTF-8 (BOM なし)。日本語コメント可。`.editorconfig` で `charset = utf-8` 強制。

### 4.2 改行コード — LF

すべてのソース・ドキュメント・ビルド設定は LF。`.gitattributes` で `eol=lf` 強制。Windows シェル (`.bat`, `.cmd`, `.ps1`) のみ CRLF。

### 4.3 末尾改行 — 必須

ファイル末尾に LF を必ず置く。`.editorconfig` の `insert_final_newline = true`。

### 4.4 SPDX ヘッダ — 必須

すべての `.h` / `.cpp` / `.inl` ファイルの 1 行目に:

```cpp
// SPDX-License-Identifier: Apache-2.0
```

その下にバナー / `#pragma once` / `#include` 等が続く。

### 4.5 1 ファイル 1 主要型

`src/foundation/Result.h` には `Result<T,E>` クラスが主役。同じファイル内に補助 struct (`OkTag`, `ErrTag` 等) は可。

### 4.6 namespace 末尾コメント — 必須

```cpp
namespace acs {
/* ... */
} // namespace acs
```

`FixNamespaceComments: true` (clang-format) が自動付与。

---

## 5. エラー処理 / Error Handling

### 5.1 `Result<T, E>` が標準

```cpp
Result<File, ErrorCode> OpenFile(const char* path) noexcept;

// 呼び出し側
auto r = OpenFile("foo.txt");
if (r.IsErr()) {
    ACS_LOG_ERROR("open failed: %s", r.Error().message);
    return r.Error();
}
File& f = r.Value();
```

### 5.2 戻り値破棄禁止 — `[[nodiscard]]`

`Result<T, E>` クラス自体に `[[nodiscard]]` が付いている (foundation/Result.h)。意図的に破棄する場合は:

```cpp
(void)MaybeFailingCall();              // (void) で明示的破棄
auto _ = OptionalEffect();             // 名前付きで破棄

// テストの場合
auto r = ThreadPool::Init(4);
EXPECT_TRUE(r.IsOk());
```

### 5.3 `ACS_TRY` / `ACS_TRY_ASSIGN` マクロ

```cpp
Result<void> Process() noexcept {
    ACS_TRY(ValidateInput());                       // 失敗時に early-return
    ACS_TRY_ASSIGN(File f, OpenFile("data.bin"));   // 値を bind
    /* ... */
    return Ok;
}
```

### 5.4 アサーションマクロ三段階

| マクロ / Macro | 用途 / Use | リリースで除去? |
|---|---|---|
| `ACS_ASSERT(expr)` | デバッグ中に気づきたい事前条件 | はい (除去) |
| `ACS_ASSERTF(expr, fmt, ...)` | printf 風メッセージ付き | はい |
| `ACS_VERIFY(expr)` | リリースでも expr は評価する版 | 検査は除去、式は残る |
| `ACS_CHECK(expr)` | リリースでも倒したい不変条件 | **いいえ (常に検査)** |
| `ACS_CHECKF(expr, fmt, ...)` | CHECK + printf メッセージ | **いいえ** |
| `ACS_NOTREACHED()` | 到達したらバグ・常時 panic | **いいえ** |
| `ACS_UNREACHABLE()` | 最適化ヒント (本当に到達しない) | リリース時 UB hint |
| `ACS_NOT_IMPLEMENTED()` | 未実装パニック | **いいえ** |

> **注**: `ACS_UNREACHABLE()` はデバッグでは Panic を呼び、リリースでは `__assume(0)` (MSVC) / `__builtin_unreachable()` (Clang/GCC) に置換される最適化ヒント。「ここに来たら必ず落としたい」場合は `ACS_NOTREACHED()` を使うこと。`ACS_NOT_IMPLEMENTED()` はリリースでも Panic する (致命的なバグなので隠さない)。

**使い分け**:
- `ACS_ASSERT`: 「デバッグ中に弾きたい」範囲チェック・事前条件
- `ACS_CHECK`: 「リリースでも倒れたほうがマシ」な security/correctness 不変条件
- `ACS_NOTREACHED`: 「来るはずがないが、来たら必ず panic」防御
- `ACS_UNREACHABLE`: 「絶対来ない、最適化器に教える」(本当に来ない時のみ)

### 5.5 例外禁止

`throw` / `try` / `catch` / `<stdexcept>` / `<exception>` は使用禁止 (R001-R003)。リリースでもデバッグでも例外を投げない。

---

## 6. メモリ / Memory

### 6.1 STL コンテナ禁止

`<vector>`, `<string>`, `<unordered_map>`, `<map>`, `<set>`, `<list>`, `<deque>` 等の STL コンテナは使用禁止 (R006)。代わりに:

| STL | ACS 等価物 |
|---|---|
| `std::vector<T>` | `acs::Array<T>` |
| `std::string` | `acs::String` / `acs::StringView` |
| `std::unordered_map<K,V>` | `acs::HashMap<K,V>` |
| `std::span<T>` | `acs::Span<T>` |
| `std::unique_ptr<T>` | `acs::UniquePtr<T>` |
| `std::shared_ptr<T>` | `acs::Rc<T>` |
| `std::function<...>` | `acs::Callback` (関数ポインタ + void*) |
| `std::optional<T>` | `Result<T>` または明示的 `T*` |

### 6.2 アロケータは明示的に渡す

```cpp
// OK — Allocator& を引数として渡す (Bitsquid 流)
Array<u32> Build(Allocator& alloc) noexcept {
    Array<u32> a(alloc);
    a.Reserve(100);
    return a;
}
```

### 6.3 `new` / `delete` 直書き禁止

`MakeUnique<T>()` / `MakeRc<T>()` / `Allocator` 経由。raw `new` / `delete` は禁止 (R018)。`malloc` / `free` も同様に禁止 (R017、Allocator 経由必須)。

### 6.4 RAII 徹底

リソース所有者は `UniquePtr<T>` / `Rc<T>` / コンテナクラス。手動 `Free()` 呼び出しは原則禁止。

---

## 7. 並行性 / Concurrency

### 7.1 `std::thread` 等の STL 並行物禁止

`<thread>`, `<mutex>`, `<atomic>`, `<future>` の STL 並行ヘッダ禁止。代わりに `acs::Thread`, `acs::Mutex`, `acs::Atomic<T>`, `acs::ScopedLock`, `acs::JobGraph` を使う。

### 7.2 スレッドエントリは `noexcept` 必須

```cpp
void WorkerEntry(void* user) noexcept {  // noexcept 必須
    /* ... */
}
```

### 7.3 Atomic は `acs::Atomic<T>`

`std::atomic<T>` は不可。`acs::Atomic<T>` を使う。memory order は `acs::EMemoryOrder` 経由。

### 7.4 Mutex 取得は `ScopedLock`

```cpp
{
    ScopedLock lk(_mutex);
    /* protected section */
}  // automatic unlock
```

---

## 8. テンプレート / Templates

### 8.1 `typename` 統一

```cpp
template<typename T>           // OK
template<class T>              // NG
```

### 8.2 SFINAE より `if constexpr`

```cpp
// OK
template<typename T>
void Foo(T& result, T x) noexcept {
    if constexpr (IsTriviallyCopyableV<T>) {
        MemCopy(&result, &x, sizeof(T));
    } else {
        result = x.Clone();
    }
}
```

`EnableIfT<...>` SFINAE は既存コードと整合させる必要がある時のみ。新規コードは `concept` または `if constexpr` を優先。

### 8.3 C++20 `concept` 推奨

```cpp
// foundation/Concepts.h に ACS 専用の concept (SameAs, Integral, FloatingPoint 等) を置く。
// <concepts> ヘッダは使わない (STL ban の対象外だが ACS スタイル統一のため)。

template<typename T>
concept Hashable = requires(T t) { { Hash(t) } -> SameAs<u64>; };

template<Hashable T>
void Insert(T value);
```

> 現状 (v1) では `acs::Concepts` を未整備。`EnableIfT<>` SFINAE が併存するが、新規コードは `requires` / `concept` を使うこと。

---

## 9. モダン C++ 機能 / Modern C++ Features

### 9.1 `override` / `final` 必須

```cpp
class Dx12Device final : public IRhiDevice {     // class final
public:
    void WaitIdle() noexcept override;            // override 必須
};
```

- 派生 class で virtual override する場合は `override` 必須 (R030)。
- 派生不可な leaf class には `final` 推奨 (devirtualization の機会)。
- `virtual` キーワードは override 時に書かない (`override` だけ書く)。

### 9.2 `[[nodiscard]]`

- `Result<T, E>` は class 自体に `[[nodiscard]]` 付与済み。
- factory 関数 (`MakeUnique`, `CreateRhi*` 等) には個別に付与推奨。
- getter (`Size()`, `Data()` 等) は不要。

### 9.3 `constexpr` 積極採用

POD コンストラクタ、accessor、enum stringifier、数学ヘルパー、定数定義に `constexpr`。

### 9.4 trailing return type は通常使わない

```cpp
Result<int> Foo() noexcept;           // OK (leading)
auto Foo() noexcept -> Result<int>;   // NG (trailing は通常不要)
```

template 戻り値で leading が表現困難な場合のみ trailing。

### 9.5 C++20 modules — 当面延期

ACS は `#include` + `#pragma once` を継続。DiligentEngine / ImGui が modules 対応するまで保留。

---

## 10. コメント / Comments

### 10.1 言語 — 日本語ナラティブ + 英語識別子

- **識別子** (型名、関数名、ファイル名): 英語
- **コメント narrative**: 日本語
- **Doxygen tag**: 採用しない (`@brief` / `@param` / `@return` 等は使わない)
- **公開 API の `@brief` ribbon** (推奨): 1 行英語 `///` で IDE tooltip 用

### 10.2 コメント形式 — `//` 統一

```cpp
// 1 行コメント
//
// 複数行は `//` を続ける。
// /* ... */ ブロックコメントは使わない。

// ===========================================================================
// セクションバナー (78 char width)
// ===========================================================================

// ---- サブセクションヘッダ ----
```

Doxygen `///` / `/** */` は採用しない (R045-c)。

### 10.3 標準化された marker

| Marker | 意味 / Meaning | owner 必須? |
|---|---|---|
| `// TODO(owner): ...` | 後でやる作業 | はい |
| `// FIXME(owner): ...` | 既知バグ、リリース前に直す | はい |
| `// HACK(owner): ...` | 意図的回避策、issue リンク必須 | はい |
| `// PERF: ...` | パフォーマンス hot path 注意 | いいえ |
| `// NOTE: ...` | 読者向け補足 | いいえ |
| `// SAFETY: ...` | unsafe 操作 (placement new, reinterpret_cast) の安全性証明 | いいえ |

禁止: `XXX`, `BUG`, `KLUDGE`, `OPTIMIZE`, `WTF`。

### 10.4 ファイルヘッダ — 必須

```cpp
// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Foundation — Result<T, E> 型（例外なしエラー伝搬）
// -----------------------------------------------------------------------------
// 設計意図、使用例、特徴を 5-30 行で記述。
// =============================================================================
#pragma once
```

### 10.5 著者・バージョンタグ禁止

`@author`, `@version`, `@date` は使わない (git blame / git log で十分)。

---

## 11. プラットフォーム移植性 / Portability

### 11.1 `<Windows.h>` 等の OS ヘッダは `src/platform/` 限定

```cpp
// NG: foundation/Foo.h
#include <Windows.h>   // 公開ヘッダから OS ヘッダ漏洩は禁止

// OK: platform/Window.cpp
#include "foundation/Platform.h"   // platform/Platform.h が WIN32_LEAN_AND_MEAN, NOMINMAX 経由で <windows.h> を吸収
```

公開ヘッダ (`*.h`) から `<Windows.h>` / `<winsock2.h>` / `<bcrypt.h>` 等の OS ヘッダを直接 `#include` してはならない (R010)。`<d3d12.h>` / `<dxgi*.h>` (R011)、`<DiligentCore/*>` (R012) も同様。`foundation/Platform.h` 経由のみ許可 (R015)。

### 11.2 Win32 型は `.cpp` 限定

公開ヘッダで Win32 ハンドル型 (HWND, HANDLE, SOCKET 等) を露出する場合は **opaque 型でラップ**:
- `HWND` → `void*` (`// HWND` コメント付き)
- `SOCKET` → `uptr` (`// SOCKET` コメント付き)

### 11.3 13 platform seam

下記の 13 関心事のみ platform/ 以下に集約 (詳細は `docs/GameFramework.md` §15.5):

1. Window 2. Input 3. FileSystem 4. Storage 5. Time
6. VirtualMemory 7. Localization 8. Network 9. Audio (mix-thread)
10. Threading primitive 11. Crash reporting 12. Platform IO
13. Power / sleep

---

## 12. ツールチェイン / Toolchain

### 12.1 `.clang-format` (repo root)

K&R, 4-space, 100 col target, west-const, west-pointer, include preserve。 [`.clang-format`](../.clang-format) 参照。

### 12.2 `.clang-tidy` (repo root)

bugprone-* / cert-* / cppcoreguidelines-* / modernize-* / performance-* / readability-* + ACS の `readability-identifier-naming` カスタム設定 + `acs-*` カスタムチェック。[`.clang-tidy`](../.clang-tidy) 参照。

### 12.3 `.editorconfig` (repo root)

UTF-8 (BOM なし), LF, 4-space, `.bat`/`.ps1` のみ CRLF。[`.editorconfig`](../.editorconfig) 参照。

### 12.4 `.gitattributes` (repo root)

binary/text 分類、LF 強制、ACS 固有形式 (`.acpak`, `.acsr`, `.scene`) 識別。[`.gitattributes`](../.gitattributes) 参照。

### 12.5 `LICENSE` — Apache-2.0

Apache-2.0 (patent grant 込み)。各ソースファイルは `// SPDX-License-Identifier: Apache-2.0` 1 行のみ。長い copyright header は禁止。

### 12.6 `acs_lint` ターゲット (Phase 3c で導入)

```bash
cmake --build cmake-build-diligent-debug --target acs_lint
```

clang-tidy を `.clang-tidy` 設定で `src/`, `samples/`, `tests/` 全体に走らせる CMake target。`CMakeLists.txt` 末尾で定義。`CMAKE_EXPORT_COMPILE_COMMANDS=ON` でリポジトリルートに `compile_commands.json` が生成され、IDE 言語サーバ (clangd / VS / Rider / CLion) も同じデータで動作する。

**現在 (v1) 有効化されているチェック**:
- 標準 clang-tidy モジュール: `bugprone-*` / `cert-*` / `concurrency-*` / `cppcoreguidelines-*` / `hicpp-*` / `misc-*` / `modernize-*` / `performance-*` / `portability-*` / `readability-*`
- 命名強制: `readability-identifier-naming` を ACS スタイルに完全 calibration (R020-R029 該当)
- error 昇格: `bugprone-use-after-move` / `bugprone-dangling-handle` / `cert-dcl58-cpp` / `cppcoreguidelines-slicing` / `modernize-use-override` / `performance-move-const-arg`

**まだ未実装**: `acs-*` カスタムチェック (R001-R012 / R026 / R029 / R031-R032 / R040-R048)。`.clang-tidy` の Checks リストには `acs-*` がワイルドカード登録されているが、対応するプラグイン (clang-tidy out-of-tree shared lib) を後続フェーズで作る必要がある。`docs/GameFramework.md` §18.21 参照。それまでは本書 (StyleGuide.md) + 手動レビューが規範。

**`.github/workflows/lint.yml`**: GitHub Actions seed。リモートに push されれば自動で発火し `clang-format --dry-run` + `acs_lint` を実行する。CI 未稼働時は休眠 (リポジトリにファイルが存在するだけ)。

---

## 13. 規則カタログ / Rule Catalog (R001-R048)

> **注 / Note**: `Check` 列で `acs-Rxxx` と書かれた規則は ACS 専用のカスタム clang-tidy プラグイン (`acs_lint`、`docs/GameFramework.md` §18.21) の管轄。プラグインは **Phase 3 で実装予定**。それまでは本書 (StyleGuide.md) と手動レビューが規範となる。標準 clang-tidy チェックが利用可能な規則 (R020-R029 命名、R030 override 等) は `.clang-tidy` で既に gate されている。

### A. STL / 例外 / RTTI 禁止 (R001-R009)

| ID | 名称 / Name | 重大度 | チェック | 概要 |
|---|---|---|---|---|
| **R001** | no-throw | error | acs-R001 + bugprone-exception-baseclass | `throw` 禁止、`Result<T,E>` を返す |
| **R002** | no-try-catch | error | acs-R002 | `try` / `catch` 禁止 |
| **R003** | no-exception-header | error | acs-R003 | `<exception>`, `<stdexcept>` include 禁止 |
| **R004** | no-rtti-dynamic-cast | error | acs-R004 + cppcoreguidelines-pro-type-* | `dynamic_cast` 禁止 |
| **R005** | no-rtti-typeid | error | acs-R005 | `typeid` 禁止 |
| **R006** | no-stl-containers | error | acs-R006 | `<vector>`, `<map>`, `<unordered_map>`, `<set>`, `<list>`, `<deque>` 等 include 禁止 |
| **R007** | no-stl-string | error | acs-R007 | `<string>`, `std::string`, `std::string_view` 禁止 |
| **R008** | no-stl-memory | error | acs-R008 | `<memory>` の `std::unique_ptr`, `std::shared_ptr` 禁止 |
| **R009** | no-stl-functional | error | acs-R009 | `<functional>` の `std::function` 禁止 (`acs::Callback` を使う) |

### B. プラットフォーム移植性 (R010-R019)

| ID | 名称 | 重大度 | チェック | 概要 |
|---|---|---|---|---|
| **R010** | no-os-header-in-public | error | acs-R010 | 公開 `.h` で `<Windows.h>` 等 OS ヘッダ include 禁止 |
| **R011** | no-d3d12-in-public | error | acs-R011 | 公開 `.h` で `<d3d12.h>`, `<dxgi*.h>` 等 include 禁止 |
| **R012** | no-diligent-in-public | error | acs-R012 | 公開 `.h` で `<DiligentCore/*>` include 禁止 |
| **R013** | platform-windows-only-platform | error | acs-R013 | `<Windows.h>` は `src/platform/` 以下のみ |
| **R014** | platform-winsock-only-network | error | acs-R014 | `<winsock2.h>` は `src/network/` 以下のみ |
| **R015** | platform-foundation-platform-only | error | acs-R015 | `foundation/Platform.h` 経由で OS ヘッダ吸収 (直 include 禁止) |
| **R016** | win32-types-only-cpp | error | acs-R016 | HWND/HANDLE/DWORD/SOCKET の `.h` 露出禁止 (opaque で wrap) |
| **R017** | no-system-mem-direct | error | acs-R017 | `malloc`/`free` 直接呼び禁止 (`Allocator&` 経由) |
| **R018** | no-raw-new-delete | error | acs-R018 | raw `new`/`delete` 禁止 (`MakeUnique`/`MakeRc` 経由) |
| **R019** | no-cstdio-printf-fmt | warning | acs-R019 | `printf` 直接呼びは avoid (`ACS_LOG_*` 経由) |

### C. 命名 (R020-R029)

| ID | 名称 | 重大度 | チェック | 概要 |
|---|---|---|---|---|
| **R020** | type-pascal-case | error | readability-identifier-naming.ClassCase | 型は PascalCase |
| **R021** | function-pascal-case | error | readability-identifier-naming.FunctionCase | 関数・メソッドは PascalCase |
| **R022** | variable-snake-case | error | readability-identifier-naming.VariableCase | ローカル変数・引数は snake_case |
| **R023** | non-public-member-prefix-underscore | error | readability-identifier-naming.PrivateMemberPrefix + ProtectedMemberPrefix | private/protected メンバは `_snake_case` |
| **R024** | constant-k-pascal | error | readability-identifier-naming.ConstantPrefix='k' | constexpr 定数は `kPascalCase` |
| **R025** | macro-acs-upper-snake | error | readability-identifier-naming.MacroDefinitionCase | マクロは `ACS_UPPER_SNAKE_CASE` |
| **R026** | enum-class-required | error | acs-R026 | 素の `enum` 禁止、`enum class : <type>` 必須 |
| **R027** | enum-value-pascal-case | error | readability-identifier-naming.ScopedEnumConstantCase | enum 値は PascalCase |
| **R028** | namespace-lowercase | error | readability-identifier-naming.NamespaceCase | namespace は小文字 |
| **R029** | interface-i-prefix | warning | acs-R029 | 純粋仮想 interface は `I` プレフィックス |

### D. ライフサイクル / `noexcept` / `override` (R030-R039)

| ID | 名称 | 重大度 | チェック | 概要 |
|---|---|---|---|---|
| **R030** | override-required | error | modernize-use-override | virtual override は `override` キーワード必須 |
| **R031** | noexcept-public-fn | warning | acs-R031 | 公開 `.h` の関数は `noexcept` 必須 |
| **R032** | noexcept-thread-entry | error | acs-R032 | スレッドエントリ・コールバックは `noexcept` 必須 |
| **R033** | result-nodiscard | warning | acs-R033 | `Result<T,E>` を返す関数の戻り値破棄を検出 |
| **R034** | lifecycle-on-prefix | info | acs-R034 | ライフサイクルフック (OnSpawn/OnUpdate/OnExit) は `On` プレフィックス |
| **R035** | rule-of-five-or-zero | warning | cppcoreguidelines-special-member-functions | Rule of zero / five 違反 |
| **R036** | header-pragma-once | error | acs-R036 | ヘッダは `#pragma once` (include guard 禁止) |
| **R037** | header-self-contained | warning | misc-include-cleaner | ヘッダは自己完結 (必要な include を持つ) |
| **R038** | no-using-namespace-header | error | google-build-using-namespace | ヘッダ内 `using namespace` 禁止 |
| **R039** | const-correctness | warning | misc-const-correctness | const 可能なローカル変数は const に |

### E. ドメイン規則 (R040-R048)

| ID | 名称 | 重大度 | チェック | 概要 |
|---|---|---|---|---|
| **R040** | callback-canonical | warning | acs-R040 | コールバックは `void(*)(/*payload*/, void* user)` 形式 |
| **R041** | no-std-function | error | acs-R041 | `std::function` 禁止 (canonical Callback 使用) |
| **R042** | typed-handle | info | acs-R042 | EntityId / AssetId 等は型付き wrapper (raw u32 禁止) |
| **R043** | log-channel-known | warning | acs-R043 | `ACS_LOG_*` は登録済み channel のみ |
| **R044** | locale-via-director | info | acs-R044 | UI 文字列は `Tr("...")` 経由 (raw 英文字列禁止) |
| **R045** | comment-banner-style | info | acs-R045 | ファイルヘッダ・セクションは `// ====` バナー |
| **R046** | comment-marker-with-owner | warning | acs-R046 | TODO/FIXME/HACK は owner 注釈 (`TODO(name):`) 必須 |
| **R047** | spdx-header-required | error | acs-R047 | 全 `.h` / `.cpp` / `.inl` の 1 行目に SPDX |
| **R048** | event-via-registry | info | acs-R048 | event 配信は `gameframework/meta/Events.h` 経由 |

---

## 14. 例外規定 / Exception Mechanism

### 14.1 ファイル単位の rule 無効化

```cpp
// SPDX-License-Identifier: Apache-2.0
// acs-lint: disable R045
//
// 理由: TLSF 実装は文献に従う SCREAMING_SNAKE_CASE を保つ。
```

`// acs-lint: disable <RuleID>` をファイル先頭近くに置く。

### 14.2 行単位の rule 無効化

```cpp
auto _r = ThreadPool::Submit(t);  // acs-lint: NOLINT(R033)
```

行末コメント `// acs-lint: NOLINT(<RuleID>)` で当該行のみ無効化。

### 14.3 サブツリー単位の無効化

`.clang-tidy` を該当ディレクトリに配置して `Checks: '-acs-Rxxx'` 等で無効化。Phase 3 以降で必要に応じて。

---

## 15. 改訂履歴 / Revision History

| 日付 | バージョン | 変更点 |
|---|---|---|
| 2026-05-21 | v1.0 | 初版。Option A+ (現状 codify + 4 改善) で確定。Phase 0 で `.clang-format` 等 5 ファイル投入、Phase 1+ で `[[nodiscard]]` / 29 rename / SPDX / 11 discard fix を実施。Phase 2 で本書 + assertion triad (`ACS_CHECK` / `ACS_NOTREACHED`) を導入。R001-R048 は Phase 3 で `acs_lint` プラグインで機械化予定。 |

---

## 参考リンク / References

- [`docs/ARCHITECTURE.md`](ARCHITECTURE.md) — ACS 全体アーキテクチャ
- [`docs/GameFramework.md`](GameFramework.md) — GameFramework v13 仕様 (§15.3 メタ層、§18.21 acs_lint)
- [`.clang-format`](../.clang-format) — フォーマッタ設定
- [`.clang-tidy`](../.clang-tidy) — リント設定
- [`.editorconfig`](../.editorconfig) — IDE 共通設定
- [`.gitattributes`](../.gitattributes) — git 属性設定
- [`LICENSE`](../LICENSE) — Apache-2.0
