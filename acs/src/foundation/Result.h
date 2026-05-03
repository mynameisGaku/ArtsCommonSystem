// =============================================================================
// ACS Foundation — Result<T, E> 型（例外なしエラー伝搬）
// -----------------------------------------------------------------------------
// std::expected / Rust Result と同じ役割を持つ「成功値 or エラー値」型。
// 例外を使わずにエラーを伝搬するのが ACS 全体の方針。
//
// 特徴:
//   - STL 不使用
//   - トリビアル破棄可能型 (T / E) ではゼロコスト
//   - ムーブ専用型 (UniquePtr 等) でも動作
//   - E のデフォルトは ErrorCode（任意の型でも可）
//   - Result<void, E> 特殊化を提供（成功 / エラーのみを返す関数用）
//
// 典型的な使用例:
//   Result<File, ErrorCode> r = OpenFile("foo");
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

namespace acs {

namespace detail {
// 構築時のオーバーロード解決を一意にするためのタグ型
struct OkTag    {};
struct ErrTag   {};
struct EmptyTag {};
} // namespace detail

inline constexpr detail::OkTag    OkInit {};   // 成功側構築用タグ
inline constexpr detail::ErrTag   ErrInit{};   // エラー側構築用タグ

// =============================================================================
// Result<T, E> — T 値を持つ成功または E エラーのいずれか
// =============================================================================
template<typename T, typename E = ErrorCode>
class Result {
public:
    using ValueType = T;
    using ErrorType = E;

    // ---- コンストラクタ ---------------------------------------------------

    // 任意の型 U から T を構築できる場合の暗黙変換コンストラクタ。
    // 例: Result<int> r = 42;
    // U が E（ErrorCode 等）と同じ型のときはこのオーバーロードを無効化し、
    // 下の `Result(const E&)` 側に確実にディスパッチさせる。
    template<typename U = T,
             typename = EnableIfT<!IsSameV<RemoveCVRefT<U>, E> &&
                                  !IsSameV<RemoveCVRefT<U>, Result<T, E>>>>
    Result(U&& v) noexcept
        : _has_value(true) {
        ::new (static_cast<void*>(&_storage._value)) T(Forward<U>(v));
    }

    // OkInit タグ + 値を渡して明示的に成功側を構築する（rvalue / lvalue 両対応）。
    Result(detail::OkTag, T&& v) noexcept
        : _has_value(true) {
        ::new (static_cast<void*>(&_storage._value)) T(Move(v));
    }
    Result(detail::OkTag, const T& v) noexcept
        : _has_value(true) {
        ::new (static_cast<void*>(&_storage._value)) T(v);
    }

    // ErrInit タグ + エラー値で明示的にエラー側を構築する。
    Result(detail::ErrTag, E&& e) noexcept
        : _has_value(false) {
        ::new (static_cast<void*>(&_storage._error)) E(Move(e));
    }

    // E の暗黙変換コンストラクタ。`return ACS_ERR(...);` のような呼び方を可能にする。
    Result(const E& e) noexcept
        : _has_value(false) {
        ::new (static_cast<void*>(&_storage._error)) E(e);
    }

    // ---- コピー / ムーブ -------------------------------------------------
    // どちらの側 (value / error) を保持しているかで配置 new の対象を切り替える。
    Result(const Result& other) noexcept : _has_value(other._has_value) {
        if (_has_value) ::new (static_cast<void*>(&_storage._value)) T(other._storage._value);
        else            ::new (static_cast<void*>(&_storage._error)) E(other._storage._error);
    }

    Result(Result&& other) noexcept : _has_value(other._has_value) {
        if (_has_value) ::new (static_cast<void*>(&_storage._value)) T(Move(other._storage._value));
        else            ::new (static_cast<void*>(&_storage._error)) E(Move(other._storage._error));
    }

    Result& operator=(const Result& other) noexcept {
        if (this == &other) return *this;
        Destroy();
        _has_value = other._has_value;
        if (_has_value) ::new (static_cast<void*>(&_storage._value)) T(other._storage._value);
        else            ::new (static_cast<void*>(&_storage._error)) E(other._storage._error);
        return *this;
    }

    Result& operator=(Result&& other) noexcept {
        if (this == &other) return *this;
        Destroy();
        _has_value = other._has_value;
        if (_has_value) ::new (static_cast<void*>(&_storage._value)) T(Move(other._storage._value));
        else            ::new (static_cast<void*>(&_storage._error)) E(Move(other._storage._error));
        return *this;
    }

    // 保持している側のデストラクタを正しく呼ぶ。
    ~Result() noexcept { Destroy(); }

    // ---- 状態確認 --------------------------------------------------------
    bool IsOk()  const noexcept { return _has_value; }      // 成功か
    bool IsErr() const noexcept { return !_has_value; }     // エラーか
    explicit operator bool() const noexcept { return _has_value; } // if (r) で成功判定

    // ---- 値アクセス（Ok 側）---------------------------------------------
    // 注: IsErr() の状態で呼ぶと未定義動作。事前に IsOk() を確認すること。
    T&       Value()       noexcept { return _storage._value; }
    const T& Value() const noexcept { return _storage._value; }

    // ---- エラーアクセス（Err 側）----------------------------------------
    E&       Error()       noexcept { return _storage._error; }
    const E& Error() const noexcept { return _storage._error; }

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
// Result<void, E> — 値を持たないバージョン（成功 / エラーのみ）
// =============================================================================
// 例: Result<void> Save() — 保存が成功したかだけを返す。
template<typename E>
class Result<void, E> {
public:
    using ValueType = void;
    using ErrorType = E;

    Result() noexcept : _has_value(true) {}                                      // デフォルトは成功
    Result(detail::OkTag) noexcept : _has_value(true) {}                          // 明示的成功
    Result(detail::ErrTag, E&& e) noexcept : _has_value(false) {                  // 明示的エラー
        ::new (static_cast<void*>(&_storage._error)) E(Move(e));
    }
    Result(const E& e) noexcept : _has_value(false) {                             // E からの暗黙変換
        ::new (static_cast<void*>(&_storage._error)) E(e);
    }

    Result(const Result& other) noexcept : _has_value(other._has_value) {
        if (!_has_value) ::new (static_cast<void*>(&_storage._error)) E(other._storage._error);
    }
    Result(Result&& other) noexcept : _has_value(other._has_value) {
        if (!_has_value) ::new (static_cast<void*>(&_storage._error)) E(Move(other._storage._error));
    }
    Result& operator=(const Result& other) noexcept {
        if (this == &other) return *this;
        Destroy();
        _has_value = other._has_value;
        if (!_has_value) ::new (static_cast<void*>(&_storage._error)) E(other._storage._error);
        return *this;
    }
    Result& operator=(Result&& other) noexcept {
        if (this == &other) return *this;
        Destroy();
        _has_value = other._has_value;
        if (!_has_value) ::new (static_cast<void*>(&_storage._error)) E(Move(other._storage._error));
        return *this;
    }
    ~Result() noexcept { Destroy(); }

    bool IsOk()  const noexcept { return _has_value; }
    bool IsErr() const noexcept { return !_has_value; }
    explicit operator bool() const noexcept { return _has_value; }

    E&       Error()       noexcept { return _storage._error; }
    const E& Error() const noexcept { return _storage._error; }

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

// Ok(value) — Result<T> 成功側を簡潔に構築する。
template<typename T> Result<T> Ok(T v) noexcept { return Result<T>(OkInit, Move(v)); }
// Ok() — Result<void> 成功側を構築する。
inline Result<void> Ok() noexcept { return Result<void>(OkInit); }

// Err(error) — 任意の Result<T> をエラー側で構築する。
template<typename T = void>
Result<T> Err(ErrorCode e) noexcept { return Result<T>(e); }

// =============================================================================
// 早期リターンマクロ（Rust の ? 演算子 相当）
// -----------------------------------------------------------------------------
// ACS_TRY(expr) — Result<void> 用。エラーなら関数から ErrorCode を返す。
// ACS_TRY_ASSIGN(name, expr) — Result<T> 用。成功なら値を name に束縛。
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
