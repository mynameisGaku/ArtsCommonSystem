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
//   TResult<FFile, FErrorCode> r = OpenFile("foo");
//   if (!r) {
//       CLogger::Error("open failed: %s", r.Error().message);
//       return;
//   }
//   FFile& f = r.Value();
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/TypeTraits.h"
#include "foundation/Move.h"
#include "foundation/Error.h"
#include "foundation/Assert.h"

namespace acs {

namespace detail {
/** 成功側構築を一意に選ぶためのタグ型。 */
struct FOkTag    {};

/** エラー側構築を一意に選ぶためのタグ型。 */
struct FErrTag   {};

/** 値を持たない側の placeholder 用タグ型。 */
struct FEmptyTag {};
} // namespace detail

/** 成功側を明示構築する際に渡すタグ値。 */
inline constexpr detail::FOkTag    OkInit {};

/** エラー側を明示構築する際に渡すタグ値。 */
inline constexpr detail::FErrTag   ErrInit{};

/**
 * 成功値 T またはエラー値 E のいずれかを保持する例外なしの結果型。
 *
 * @details
 * std::expected / Rust の Result 相当。STL 不使用で、トリビアル破棄可能型では
 * ゼロコスト、ムーブ専用型でも動作する。内部は union で T と E を排他的に保持し、
 * どちらが有効かを m_HasValue で判別する。
 * @tparam T 成功時に保持する値の型。
 * @tparam E エラー時に保持する型 (既定 FErrorCode)。
 */
template<typename T, typename E = FErrorCode>
class [[nodiscard]] TResult {
public:
    /** 成功値の型。 */
    using ValueType = T;

    /** エラー値の型。 */
    using ErrorType = E;

    /**
     * 任意の型 U から T を構築する暗黙変換コンストラクタ (成功側)。
     *
     * @details
     * `TResult<int> r = 42;` のように値から直接構築できる。U が E や TResult 自身と
     * 同じ型のときは EnableIf で無効化し、`TResult(const E&)` 側へ確実に振り分ける。
     * @tparam U 値の型 (既定 T)。
     * @param v 成功値として格納する値 (完全転送される)。
     */
    template<typename U = T,
             typename = EnableIfT<!IsSameV<RemoveCVRefT<U>, E> &&
                                  !IsSameV<RemoveCVRefT<U>, TResult<T, E>>>>
    TResult(U&& v) noexcept
        : m_HasValue(true) {
        ::new (static_cast<void*>(&m_Storage.m_Value)) T(Forward<U>(v));
    }

    /**
     * OkInit タグ付きで成功側を明示構築する (rvalue 版)。
     *
     * @param v 成功値 (ムーブして格納する)。
     */
    TResult(detail::FOkTag, T&& v) noexcept
        : m_HasValue(true) {
        ::new (static_cast<void*>(&m_Storage.m_Value)) T(Move(v));
    }

    /**
     * OkInit タグ付きで成功側を明示構築する (lvalue 版)。
     *
     * @param v 成功値 (コピーして格納する)。
     */
    TResult(detail::FOkTag, const T& v) noexcept
        : m_HasValue(true) {
        ::new (static_cast<void*>(&m_Storage.m_Value)) T(v);
    }

    /**
     * ErrInit タグ付きでエラー側を明示構築する。
     *
     * @param e エラー値 (ムーブして格納する)。
     */
    TResult(detail::FErrTag, E&& e) noexcept
        : m_HasValue(false) {
        ::new (static_cast<void*>(&m_Storage.m_Error)) E(Move(e));
    }

    /**
     * E からの暗黙変換コンストラクタ (エラー側)。
     *
     * @details `return ACS_ERR(...);` のような書き方を可能にする。
     * @param e 格納するエラー値 (コピーして格納する)。
     */
    TResult(const E& e) noexcept
        : m_HasValue(false) {
        ::new (static_cast<void*>(&m_Storage.m_Error)) E(e);
    }

    /**
     * コピー構築する。
     *
     * @details other が保持している側 (value / error) に応じて配置 new 先を切り替える。
     * @param other コピー元。
     */
    TResult(const TResult& other) noexcept : m_HasValue(other.m_HasValue) {
        if (m_HasValue) ::new (static_cast<void*>(&m_Storage.m_Value)) T(other.m_Storage.m_Value);
        else            ::new (static_cast<void*>(&m_Storage.m_Error)) E(other.m_Storage.m_Error);
    }

    /**
     * ムーブ構築する。
     *
     * @details other が保持している側に応じて、value / error をムーブで配置 new する。
     * @param other ムーブ元。
     */
    TResult(TResult&& other) noexcept : m_HasValue(other.m_HasValue) {
        if (m_HasValue) ::new (static_cast<void*>(&m_Storage.m_Value)) T(Move(other.m_Storage.m_Value));
        else            ::new (static_cast<void*>(&m_Storage.m_Error)) E(Move(other.m_Storage.m_Error));
    }

    /**
     * コピー代入する。
     *
     * @details 既存の保持値を破棄してから other 側を配置 new で再構築する (自己代入は無視)。
     * @param other コピー元。
     * @return *this。
     */
    TResult& operator=(const TResult& other) noexcept {
        if (this == &other) return *this;
        Destroy();
        m_HasValue = other.m_HasValue;
        if (m_HasValue) ::new (static_cast<void*>(&m_Storage.m_Value)) T(other.m_Storage.m_Value);
        else            ::new (static_cast<void*>(&m_Storage.m_Error)) E(other.m_Storage.m_Error);
        return *this;
    }

    /**
     * ムーブ代入する。
     *
     * @details 既存の保持値を破棄してから other 側をムーブで再構築する (自己代入は無視)。
     * @param other ムーブ元。
     * @return *this。
     */
    TResult& operator=(TResult&& other) noexcept {
        if (this == &other) return *this;
        Destroy();
        m_HasValue = other.m_HasValue;
        if (m_HasValue) ::new (static_cast<void*>(&m_Storage.m_Value)) T(Move(other.m_Storage.m_Value));
        else            ::new (static_cast<void*>(&m_Storage.m_Error)) E(Move(other.m_Storage.m_Error));
        return *this;
    }

    /** 保持している側 (value / error) のデストラクタを正しく呼んで破棄する。 */
    ~TResult() noexcept { Destroy(); }

    /**
     * 成功側を保持しているかを返す。
     *
     * @return 成功値を保持していれば true。
     */
    bool IsOk()  const noexcept { return m_HasValue; }

    /**
     * エラー側を保持しているかを返す。
     *
     * @return エラー値を保持していれば true。
     */
    bool IsErr() const noexcept { return !m_HasValue; }

    /**
     * bool 変換: 成功側を保持しているかを返す (`if (r)` で成功判定)。
     *
     * @return 成功値を保持していれば true。
     */
    explicit operator bool() const noexcept { return m_HasValue; }

    /**
     * 成功値への可変参照を返す。
     *
     * @details IsErr() の状態で呼ぶと ACS_ASSERT で停止する。事前に IsOk() を確認すること。
     * @return 成功値への参照。
     */
    T&       Value()       noexcept { ACS_ASSERT(m_HasValue); return m_Storage.m_Value; }

    /**
     * 成功値への const 参照を返す。
     *
     * @details IsErr() の状態で呼ぶと ACS_ASSERT で停止する。事前に IsOk() を確認すること。
     * @return 成功値への const 参照。
     */
    const T& Value() const noexcept { ACS_ASSERT(m_HasValue); return m_Storage.m_Value; }

    /**
     * エラー値への可変参照を返す。
     *
     * @details IsOk() の状態で呼ぶと ACS_ASSERT で停止する。事前に IsErr() を確認すること。
     * @return エラー値への参照。
     */
    E&       Error()       noexcept { ACS_ASSERT(!m_HasValue); return m_Storage.m_Error; }

    /**
     * エラー値への const 参照を返す。
     *
     * @details IsOk() の状態で呼ぶと ACS_ASSERT で停止する。事前に IsErr() を確認すること。
     * @return エラー値への const 参照。
     */
    const E& Error() const noexcept { ACS_ASSERT(!m_HasValue); return m_Storage.m_Error; }

    /**
     * 成功値を返し、エラー側のときは fallback を返す。
     *
     * @param fallback エラー側のときに返す既定値。
     * @return 成功側なら保持値、エラー側なら fallback。
     */
    T ValueOr(T fallback) const noexcept {
        return m_HasValue ? m_Storage.m_Value : fallback;
    }

private:
    /**
     * 保持側に応じて適切なデストラクタを呼ぶ。
     *
     * @details トリビアル破棄可能型に対しては何もしない (ゼロコスト)。
     */
    void Destroy() noexcept {
        if (m_HasValue) {
            if constexpr (!IsTriviallyDestructibleV<T>) m_Storage.m_Value.~T();
        } else {
            if constexpr (!IsTriviallyDestructibleV<E>) m_Storage.m_Error.~E();
        }
    }

    /**
     * 成功値 T とエラー値 E を排他的に保持する union ストレージ。
     *
     * @details どちらが有効かは外側の m_HasValue で判別する。
     */
    union FStorage {
        /** 何も構築しない (どちらのメンバを生かすかは TResult が配置 new で決める)。 */
        FStorage() noexcept {}

        /** 破棄はメンバごとに TResult::Destroy が行うため空。 */
        ~FStorage() noexcept {}

        /** 成功側の値。 */
        T m_Value;

        /** エラー側の値。 */
        E m_Error;
    } m_Storage;

    /** 成功側 (true) かエラー側 (false) かを示すフラグ。 */
    bool m_HasValue;
};

/**
 * 成功 / エラーのみを表す値なし版の結果型 (TResult<void, E> 特殊化)。
 *
 * @details
 * 成功したかどうかだけを返す関数 (例: `TResult<void> Save()`) に使う。
 * 内部は m_HasValue で成功 / エラーを判別し、エラー側のみ E を保持する。
 * @tparam E エラー時に保持する型。
 */
template<typename E>
class [[nodiscard]] TResult<void, E> {
public:
    /** 成功値の型 (void)。 */
    using ValueType = void;

    /** エラー値の型。 */
    using ErrorType = E;

    /** デフォルト構築する (成功状態)。 */
    TResult() noexcept : m_HasValue(true) {}

    /** OkInit タグ付きで成功側を明示構築する。 */
    TResult(detail::FOkTag) noexcept : m_HasValue(true) {}

    /**
     * ErrInit タグ付きでエラー側を明示構築する。
     *
     * @param e エラー値 (ムーブして格納する)。
     */
    TResult(detail::FErrTag, E&& e) noexcept : m_HasValue(false) {
        ::new (static_cast<void*>(&m_Storage.m_Error)) E(Move(e));
    }

    /**
     * E からの暗黙変換コンストラクタ (エラー側)。
     *
     * @param e 格納するエラー値 (コピーして格納する)。
     */
    TResult(const E& e) noexcept : m_HasValue(false) {
        ::new (static_cast<void*>(&m_Storage.m_Error)) E(e);
    }

    /**
     * コピー構築する (エラー側のときだけ E をコピーする)。
     *
     * @param other コピー元。
     */
    TResult(const TResult& other) noexcept : m_HasValue(other.m_HasValue) {
        if (!m_HasValue) ::new (static_cast<void*>(&m_Storage.m_Error)) E(other.m_Storage.m_Error);
    }

    /**
     * ムーブ構築する (エラー側のときだけ E をムーブする)。
     *
     * @param other ムーブ元。
     */
    TResult(TResult&& other) noexcept : m_HasValue(other.m_HasValue) {
        if (!m_HasValue) ::new (static_cast<void*>(&m_Storage.m_Error)) E(Move(other.m_Storage.m_Error));
    }

    /**
     * コピー代入する (自己代入は無視、既存値破棄後に再構築)。
     *
     * @param other コピー元。
     * @return *this。
     */
    TResult& operator=(const TResult& other) noexcept {
        if (this == &other) return *this;
        Destroy();
        m_HasValue = other.m_HasValue;
        if (!m_HasValue) ::new (static_cast<void*>(&m_Storage.m_Error)) E(other.m_Storage.m_Error);
        return *this;
    }

    /**
     * ムーブ代入する (自己代入は無視、既存値破棄後に再構築)。
     *
     * @param other ムーブ元。
     * @return *this。
     */
    TResult& operator=(TResult&& other) noexcept {
        if (this == &other) return *this;
        Destroy();
        m_HasValue = other.m_HasValue;
        if (!m_HasValue) ::new (static_cast<void*>(&m_Storage.m_Error)) E(Move(other.m_Storage.m_Error));
        return *this;
    }

    /** エラー側のときに E のデストラクタを呼んで破棄する。 */
    ~TResult() noexcept { Destroy(); }

    /**
     * 成功側かを返す。
     *
     * @return 成功なら true。
     */
    bool IsOk()  const noexcept { return m_HasValue; }

    /**
     * エラー側かを返す。
     *
     * @return エラーなら true。
     */
    bool IsErr() const noexcept { return !m_HasValue; }

    /**
     * bool 変換: 成功側かを返す (`if (r)` で成功判定)。
     *
     * @return 成功なら true。
     */
    explicit operator bool() const noexcept { return m_HasValue; }

    /**
     * エラー値への可変参照を返す。
     *
     * @details IsOk() の状態で呼ぶと ACS_ASSERT で停止する。事前に IsErr() を確認すること。
     * @return エラー値への参照。
     */
    E&       Error()       noexcept { ACS_ASSERT(!m_HasValue); return m_Storage.m_Error; }

    /**
     * エラー値への const 参照を返す。
     *
     * @details IsOk() の状態で呼ぶと ACS_ASSERT で停止する。事前に IsErr() を確認すること。
     * @return エラー値への const 参照。
     */
    const E& Error() const noexcept { ACS_ASSERT(!m_HasValue); return m_Storage.m_Error; }

private:
    /**
     * エラー側のときに E のデストラクタを呼ぶ。
     *
     * @details トリビアル破棄可能な E に対しては何もしない (ゼロコスト)。
     */
    void Destroy() noexcept {
        if (!m_HasValue) if constexpr (!IsTriviallyDestructibleV<E>) m_Storage.m_Error.~E();
    }

    /**
     * エラー値 E を保持する union ストレージ (成功側ではパディングのみ使う)。
     *
     * @details 値を持たない版なので成功側にメンバ値はなく、m_Pad はサイズ確保用。
     */
    union FStorage {
        /** 何も構築しない (エラー側のみ TResult が配置 new で m_Error を生かす)。 */
        FStorage() noexcept {}

        /** 破棄は TResult::Destroy が行うため空。 */
        ~FStorage() noexcept {}

        /** 成功側でのサイズ確保用ダミー。 */
        u8 m_Pad;

        /** エラー側の値。 */
        E  m_Error;
    } m_Storage;

    /** 成功側 (true) かエラー側 (false) かを示すフラグ。 */
    bool m_HasValue;
};

/**
 * TResult<T> の成功側を簡潔に構築する。
 *
 * @tparam T 成功値の型 (引数から推論)。
 * @param v 成功値 (ムーブして格納する)。
 * @return v を保持する成功側 TResult<T>。
 */
template<typename T> TResult<T> Ok(T v) noexcept { return TResult<T>(OkInit, Move(v)); }

/**
 * TResult<void> の成功側を構築する。
 *
 * @return 成功側の TResult<void>。
 */
inline TResult<void> Ok() noexcept { return TResult<void>(OkInit); }

/**
 * 任意の TResult<T> をエラー側で構築する。
 *
 * @tparam T 結果型の成功値の型 (既定 void)。
 * @param e 格納する FErrorCode。
 * @return e を保持するエラー側 TResult<T>。
 */
template<typename T = void>
TResult<T> Err(FErrorCode e) noexcept { return TResult<T>(e); }

// 早期リターンマクロ（Rust の ? 演算子 相当）
// ACS_TRY(expr) — TResult<void> 用。エラーなら関数から FErrorCode を返す。
// ACS_TRY_ASSIGN(name, expr) — TResult<T> 用。成功なら値を name に束縛。
#define ACS_TRY(expr)                                                         \
    do {                                                                      \
        auto m_AcsTryR = (expr);                                             \
        if (m_AcsTryR.IsErr()) return ::acs::Err(m_AcsTryR.Error());        \
    } while (0)

#define ACS_TRY_ASSIGN(name, expr)                                            \
    auto ACS_CONCAT(m_AcsTmp, __LINE__) = (expr);                            \
    if (ACS_CONCAT(m_AcsTmp, __LINE__).IsErr())                              \
        return ::acs::Err(ACS_CONCAT(m_AcsTmp, __LINE__).Error());           \
    auto&& name = ACS_CONCAT(m_AcsTmp, __LINE__).Value()

} // namespace acs
