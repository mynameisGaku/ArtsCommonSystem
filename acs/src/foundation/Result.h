// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Foundation — TResult<T, E> 型（例外なしエラー伝搬）
// -----------------------------------------------------------------------------
// std::expected / Rust TResult と同じ役割を持つ「成功値 or エラー値」型。
// 例外を使わずにエラーを伝搬するのが ACS 全体の方針。
//
// 特徴:
//   - STL 不使用
//   - トリビアル破棄可能型 (T / E) ではゼロコスト
//   - ムーブ専用型 (TUniquePtr 等) でも動作
//   - E のデフォルトは FErrorCode（任意の型でも可）
//   - TResult<void, E> 特殊化を提供（成功 / エラーのみを返す関数用）
//
// 典型的な使用例:
//   TResult<File, FErrorCode> r = OpenFile("foo");
//   if (!r) {
//       Logger::Error("open failed: %s", r.Error().message);
//       return;
//   }
//   File& f = r.Value();
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/TypeTraits.h"
#include "foundation/Move.h"
#include "foundation/Error.h"
#include "foundation/Assert.h"

namespace acs {

namespace detail {
// 構築時のオーバーロード解決を一意にするためのタグ型
struct FOkTag    {};
struct FErrTag   {};
struct EmptyTag {};
} // namespace detail

inline constexpr detail::FOkTag    OkInit {};   // 成功側構築用タグ
inline constexpr detail::FErrTag   ErrInit{};   // エラー側構築用タグ

// =============================================================================
// TResult<T, E> — T 値を持つ成功または E エラーのいずれか
// =============================================================================
template<typename T, typename E = FErrorCode>
class [[nodiscard]] TResult {
public:
    using ValueType = T;
    using ErrorType = E;

    // ---- コンストラクタ ---------------------------------------------------

    // 任意の型 U から T を構築できる場合の暗黙変換コンストラクタ。
    // 例: TResult<int> r = 42;
    // U が E（FErrorCode 等）と同じ型のときはこのオーバーロードを無効化し、
    // 下の `TResult(const E&)` 側に確実にディスパッチさせる。
    template<typename U = T,
             typename = EnableIfT<!IsSameV<RemoveCVRefT<U>, E> &&
                                  !IsSameV<RemoveCVRefT<U>, TResult<T, E>>>>
    TResult(U&& v) noexcept
        : _has_value(true) {
        ::new (static_cast<void*>(&_storage._value)) T(Forward<U>(v));
    }

    // OkInit タグ + 値を渡して明示的に成功側を構築する（rvalue / lvalue 両対応）。
    TResult(detail::FOkTag, T&& v) noexcept
        : _has_value(true) {
        ::new (static_cast<void*>(&_storage._value)) T(Move(v));
    }
    TResult(detail::FOkTag, const T& v) noexcept
        : _has_value(true) {
        ::new (static_cast<void*>(&_storage._value)) T(v);
    }

    // ErrInit タグ + エラー値で明示的にエラー側を構築する。
    TResult(detail::FErrTag, E&& e) noexcept
        : _has_value(false) {
        ::new (static_cast<void*>(&_storage._error)) E(Move(e));
    }

    // E の暗黙変換コンストラクタ。`return ACS_ERR(...);` のような呼び方を可能にする。
    TResult(const E& e) noexcept
        : _has_value(false) {
        ::new (static_cast<void*>(&_storage._error)) E(e);
    }

    // ---- コピー / ムーブ -------------------------------------------------
    // どちらの側 (value / error) を保持しているかで配置 new の対象を切り替える。
    TResult(const TResult& other) noexcept : _has_value(other._has_value) {
        if (_has_value) ::new (static_cast<void*>(&_storage._value)) T(other._storage._value);
        else            ::new (static_cast<void*>(&_storage._error)) E(other._storage._error);
    }

    TResult(TResult&& other) noexcept : _has_value(other._has_value) {
        if (_has_value) ::new (static_cast<void*>(&_storage._value)) T(Move(other._storage._value));
        else            ::new (static_cast<void*>(&_storage._error)) E(Move(other._storage._error));
    }

    TResult& operator=(const TResult& other) noexcept {
        if (this == &other) return *this;
        Destroy();
        _has_value = other._has_value;
        if (_has_value) ::new (static_cast<void*>(&_storage._value)) T(other._storage._value);
        else            ::new (static_cast<void*>(&_storage._error)) E(other._storage._error);
        return *this;
    }

    TResult& operator=(TResult&& other) noexcept {
        if (this == &other) return *this;
        Destroy();
        _has_value = other._has_value;
        if (_has_value) ::new (static_cast<void*>(&_storage._value)) T(Move(other._storage._value));
        else            ::new (static_cast<void*>(&_storage._error)) E(Move(other._storage._error));
        return *this;
    }

    // 保持している側のデストラクタを正しく呼ぶ。
    ~TResult() noexcept { Destroy(); }

    // ---- 状態確認 --------------------------------------------------------
    bool IsOk()  const noexcept { return _has_value; }      // 成功か
    bool IsErr() const noexcept { return !_has_value; }     // エラーか
    explicit operator bool() const noexcept { return _has_value; } // if (r) で成功判定

    // ---- 値アクセス（Ok 側）---------------------------------------------
    // 注: IsErr() の状態で Value() を呼ぶと ACS_ASSERT で即座に停止する
    //     （誤用を確実に検出するため）。呼ぶ前に IsOk() で成功を確認すること。
    T&       Value()       noexcept { ACS_ASSERT(_has_value); return _storage._value; }
    const T& Value() const noexcept { ACS_ASSERT(_has_value); return _storage._value; }

    // ---- エラーアクセス（Err 側）----------------------------------------
    // 注: IsOk() の状態で Error() を呼ぶと ACS_ASSERT で即座に停止する。
    E&       Error()       noexcept { ACS_ASSERT(!_has_value); return _storage._error; }
    const E& Error() const noexcept { ACS_ASSERT(!_has_value); return _storage._error; }

    // 値を取得、エラー時は fallback を返す（簡易デフォルト適用）
    T ValueOr(T fallback) const noexcept {
        return _has_value ? _storage._value : fallback;
    }

private:
    // 保持側に応じて適切なデストラクタを呼び出す。
    // トリビアル破棄可能型は何もしない（ゼロコスト）。
    void Destroy() noexcept {
        if (_has_value) {
            if constexpr (!IsTriviallyDestructibleV<T>) _storage._value.~T();
        } else {
            if constexpr (!IsTriviallyDestructibleV<E>) _storage._error.~E();
        }
    }

    // ストレージは union で T と E を排他的に保持する（メモリ節約）。
    // どちらが有効かは _has_value で判別する。
    union Storage {
        Storage() noexcept {}
        ~Storage() noexcept {}
        T _value;
        E _error;
    } _storage;
    bool _has_value;
};

// =============================================================================
// TResult<void, E> — 値を持たないバージョン（成功 / エラーのみ）
// =============================================================================
// 例: TResult<void> Save() — 保存が成功したかだけを返す。
template<typename E>
class [[nodiscard]] TResult<void, E> {
public:
    using ValueType = void;
    using ErrorType = E;

    TResult() noexcept : _has_value(true) {}                                      // デフォルトは成功
    TResult(detail::FOkTag) noexcept : _has_value(true) {}                          // 明示的成功
    TResult(detail::FErrTag, E&& e) noexcept : _has_value(false) {                  // 明示的エラー
        ::new (static_cast<void*>(&_storage._error)) E(Move(e));
    }
    TResult(const E& e) noexcept : _has_value(false) {                             // E からの暗黙変換
        ::new (static_cast<void*>(&_storage._error)) E(e);
    }

    TResult(const TResult& other) noexcept : _has_value(other._has_value) {
        if (!_has_value) ::new (static_cast<void*>(&_storage._error)) E(other._storage._error);
    }
    TResult(TResult&& other) noexcept : _has_value(other._has_value) {
        if (!_has_value) ::new (static_cast<void*>(&_storage._error)) E(Move(other._storage._error));
    }
    TResult& operator=(const TResult& other) noexcept {
        if (this == &other) return *this;
        Destroy();
        _has_value = other._has_value;
        if (!_has_value) ::new (static_cast<void*>(&_storage._error)) E(other._storage._error);
        return *this;
    }
    TResult& operator=(TResult&& other) noexcept {
        if (this == &other) return *this;
        Destroy();
        _has_value = other._has_value;
        if (!_has_value) ::new (static_cast<void*>(&_storage._error)) E(Move(other._storage._error));
        return *this;
    }
    ~TResult() noexcept { Destroy(); }

    bool IsOk()  const noexcept { return _has_value; }
    bool IsErr() const noexcept { return !_has_value; }
    explicit operator bool() const noexcept { return _has_value; }

    // 注: IsOk() の状態で Error() を呼ぶと ACS_ASSERT で即座に停止する。
    E&       Error()       noexcept { ACS_ASSERT(!_has_value); return _storage._error; }
    const E& Error() const noexcept { ACS_ASSERT(!_has_value); return _storage._error; }

private:
    void Destroy() noexcept {
        if (!_has_value) if constexpr (!IsTriviallyDestructibleV<E>) _storage._error.~E();
    }
    union Storage {
        Storage() noexcept {}
        ~Storage() noexcept {}
        u8 _pad;
        E  _error;
    } _storage;
    bool _has_value;
};

// =============================================================================
// ヘルパー関数
// =============================================================================

// Ok(value) — TResult<T> 成功側を簡潔に構築する。
template<typename T> TResult<T> Ok(T v) noexcept { return TResult<T>(OkInit, Move(v)); }
// Ok() — TResult<void> 成功側を構築する。
inline TResult<void> Ok() noexcept { return TResult<void>(OkInit); }

// Err(error) — 任意の TResult<T> をエラー側で構築する。
template<typename T = void>
TResult<T> Err(FErrorCode e) noexcept { return TResult<T>(e); }

// =============================================================================
// 早期リターンマクロ（Rust の ? 演算子 相当）
// -----------------------------------------------------------------------------
// ACS_TRY(expr) — TResult<void> 用。エラーなら関数から FErrorCode を返す。
// ACS_TRY_ASSIGN(name, expr) — TResult<T> 用。成功なら値を name に束縛。
// =============================================================================
#define ACS_TRY(expr)                                                         \
    do {                                                                      \
        auto _acs_try_r = (expr);                                             \
        if (_acs_try_r.IsErr()) return ::acs::Err(_acs_try_r.Error());        \
    } while (0)

#define ACS_TRY_ASSIGN(name, expr)                                            \
    auto ACS_CONCAT(_acs_tmp_, __LINE__) = (expr);                            \
    if (ACS_CONCAT(_acs_tmp_, __LINE__).IsErr())                              \
        return ::acs::Err(ACS_CONCAT(_acs_tmp_, __LINE__).Error());           \
    auto&& name = ACS_CONCAT(_acs_tmp_, __LINE__).Value()

} // namespace acs
