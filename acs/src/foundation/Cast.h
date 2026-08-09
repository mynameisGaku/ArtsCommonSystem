// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Foundation — Cast<T> / CastChecked<T> (RTTI 不使用の安全ダウンキャスト)
// -----------------------------------------------------------------------------
// ACS は RTTI 無効 (dynamic_cast 不可) なので、`Cast<T>` / `CastChecked<T>` を
// 「軽量な侵入型 (intrusive) RTTI」で提供する。階層側が
// マクロで型 ID と IsA を opt-in 宣言し、Cast はそれを使って実行時に安全な
// ダウンキャストを行う。
//
//   提供物:
//     Cast<To>(From* p)        — p が To (またはその派生) なら To*、違えば nullptr
//     CastChecked<To>(From* p) — 同上だが、失敗時は ACS_CHECKF で panic (非 null 前提)
//     IsA<To>(const From* p)   — p が To 互換かを bool で返す
//     ACS_RTTI_ROOT(Type)      — 階層のルートクラスに置く (virtual な型 ID を導入)
//     ACS_RTTI(Type, Parent)   — 派生クラスに置く (IsA を親へ連鎖)
//
// 使い方:
//   class FShape { public: virtual ~FShape() = default; ACS_RTTI_ROOT(FShape) };
//   class FCircle : public FShape { public: ACS_RTTI(FCircle, FShape) };
//   FShape* s = ...;
//   if (FCircle* c = acs::Cast<FCircle>(s)) { ... }   // 安全 (違えば nullptr)
//   FCircle* c2 = acs::CastChecked<FCircle>(s);        // 確信があるとき (違えば panic)
//
// 仕組み:
//   ・型 ID は「型ごとに一意な static 変数のアドレス」(FClassId = const void*)。
//     RTTI / typeid 不使用。TypeTag<T>() (gameframework) と同じ手法。
//   ・各クラスは StaticClassId() (= 自分の ID) と、virtual IsA(id) を持つ。IsA は
//     「自分の ID か?」を見て、違えば親の IsA に委譲する。これで単一継承チェーンの
//     祖先判定 (= dynamic_cast 相当の多態ダウンキャスト) ができる。
//   ・Cast は p->IsA(To::StaticClassId()) が真のときだけ static_cast する。To が From
//     の派生でない (= static_cast 不可能) 組み合わせはコンパイルエラーになる
//     (単一継承チェーン内の up/down cast を対象とする)。
//
// 制約:
//   ・対象階層は ACS_RTTI_ROOT / ACS_RTTI で opt-in する必要がある (侵入型)。
//   ・多重継承の sibling cast は非対応 (static_cast が成立する単一チェーン用)。
//   ・スマートポインタは raw を取り出して使う: Cast<T>(ptr.Get())
//     (foundation は memory に依存しないため、ここでは raw ポインタのみ対応)。
// =============================================================================
#pragma once

#include "foundation/Compiler.h"   // ACS_FORCEINLINE
#include "foundation/Assert.h"     // ACS_CHECKF

namespace acs {

/** RTTI 不使用の型 ID (型ごとに一意な static アドレス、null 不可)。 */
using FClassId = const void*;

namespace cast_detail {

/**
 * 型 T に一意な FClassId を返す (RTTI 不使用、static 変数のアドレス)。
 *
 * @details 各 T のインスタンス化ごとに別 static = 別アドレスになるため一意。
 * @tparam T ID を取りたい型。
 * @return 型 T を一意に識別する不透明ポインタ。
 */
template<class T>
ACS_FORCEINLINE FClassId ClassTagOf() noexcept {
    static const char s_tag = 0;
    return static_cast<FClassId>(&s_tag);
}

} // namespace cast_detail

// =============================================================================
// 侵入型 RTTI 宣言マクロ
// =============================================================================

/**
 * 階層のルートクラスに置く。virtual な型 ID 機構 (StaticClassId / GetClassId / IsA) を導入する。
 *
 * @param Type このクラス自身の型名。
 */
#define ACS_RTTI_ROOT(Type)                                                     \
    /** この型の static な型 ID。 */                                            \
    static ::acs::FClassId StaticClassId() noexcept {                           \
        return ::acs::cast_detail::ClassTagOf<Type>();                          \
    }                                                                           \
    /** この instance の最派生型 ID。 */                                        \
    virtual ::acs::FClassId GetClassId() const noexcept {                       \
        return StaticClassId();                                                 \
    }                                                                           \
    /** id がこの型 (またはその祖先) と一致するか。 */                         \
    virtual bool IsA(::acs::FClassId id) const noexcept {                       \
        return id == StaticClassId();                                           \
    }

/**
 * 派生クラスに置く。IsA を親クラスへ連鎖させ、祖先判定を可能にする。
 *
 * @param Type このクラス自身の型名。
 * @param Parent 直接の親クラス名 (ACS_RTTI_ROOT / ACS_RTTI 済みであること)。
 */
#define ACS_RTTI(Type, Parent)                                                  \
    static ::acs::FClassId StaticClassId() noexcept {                           \
        return ::acs::cast_detail::ClassTagOf<Type>();                          \
    }                                                                           \
    ::acs::FClassId GetClassId() const noexcept override {                      \
        return StaticClassId();                                                 \
    }                                                                           \
    bool IsA(::acs::FClassId id) const noexcept override {                      \
        return id == StaticClassId() || Parent::IsA(id);                        \
    }

// =============================================================================
// Cast / CastChecked / IsA
// =============================================================================

/**
 * p が To (またはその派生) かを返す。
 *
 * @tparam To 問い合わせる型 (ACS_RTTI_* 宣言済み)。
 * @param p 検査する non-owning ポインタ (nullptr 可)。
 * @return p が非 null かつ To 互換なら true。
 */
template<class To, class From>
ACS_FORCEINLINE bool IsA(const From* p) noexcept {
    return p != nullptr && p->IsA(To::StaticClassId());
}

/**
 * p を To* へ安全にダウンキャストする (失敗は nullptr)。
 *
 * @details p が To (またはその派生) のときだけ static_cast する。null や型不一致は nullptr。
 * @tparam To キャスト先の型 (ACS_RTTI_* 宣言済み)。
 * @param p キャストする non-owning ポインタ (nullptr 可)。
 * @return To*、または不一致/null なら nullptr。
 */
template<class To, class From>
ACS_FORCEINLINE To* Cast(From* p) noexcept {
    return (p != nullptr && p->IsA(To::StaticClassId())) ? static_cast<To*>(p) : nullptr;
}

/** const 版 Cast。 */
template<class To, class From>
ACS_FORCEINLINE const To* Cast(const From* p) noexcept {
    return (p != nullptr && p->IsA(To::StaticClassId())) ? static_cast<const To*>(p) : nullptr;
}

/**
 * p を To* へキャストする。失敗時 (null / 型不一致) は ACS_CHECKF で panic する。
 *
 * @details 「ここは必ず To のはず」という前提を実行時に検査する場面で使う。
 * @tparam To キャスト先の型 (ACS_RTTI_* 宣言済み)。
 * @param p キャストする non-owning ポインタ。
 * @return To* (必ず非 null。さもなくば panic)。
 */
template<class To, class From>
ACS_FORCEINLINE To* CastChecked(From* p) noexcept {
    To* r = Cast<To>(p);
    ACS_CHECKF(r != nullptr, "CastChecked failed: source is null or not the requested type");
    return r;
}

/** const 版 CastChecked。 */
template<class To, class From>
ACS_FORCEINLINE const To* CastChecked(const From* p) noexcept {
    const To* r = Cast<To>(p);
    ACS_CHECKF(r != nullptr, "CastChecked failed: source is null or not the requested type");
    return r;
}

} // namespace acs
