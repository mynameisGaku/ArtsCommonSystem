// SPDX-License-Identifier: Apache-2.0
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
