// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS GameFramework — ReflectApply
//   反射フィールド (offset 付き) を介して «実インスタンスへ値を読み書き» する。
// -----------------------------------------------------------------------------
// 長年保留だった「エディタで編集した値 (authored values) を、実体化したコンポーネント
// インスタンスへ適用する」ブリッジ。ACS_CLASS/ACS_PROPERTY のコード生成は ACS_RFIELD_D
// (offset + 既定値) で反射するため、ここで offset を使って値を実メンバへ書き込める。
//
//   void* obj = reg.CreateById(typeId);          // 実体化
//   ApplyDefaults(obj, *desc);                    // C++ 既定値で初期化
//   f32 v[4] = { 7.5f, 0, 0, 0 };
//   ApplyValueByName(obj, *desc, "speed", v);     // authored 値を上書き
//
// offset==0 && size==0 のフィールド (ACS_RPROP = スキーマのみ) は実メンバが無いので skip。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "gameframework/Reflect.h"

namespace acs::game {

/** フィールドが実メンバ (offset 付き) を持つか (ACS_RPROP のスキーマのみは false)。 */
inline bool FieldHasStorage(const FReflectField& f) noexcept { return f.size != 0u; }

namespace reflect_apply_detail {

using ApplyFn = void (*)(unsigned char*, const f32*) noexcept;
using ReadFn = void (*)(const unsigned char*, f32*) noexcept;

template<EFieldKind Kind>
void Apply(unsigned char* p, const f32* v) noexcept
{
    if constexpr (Kind == EFieldKind::Bool)
        *reinterpret_cast<bool*>(p) = v[0] != 0.0f;
    else if constexpr (Kind == EFieldKind::I32 ||
                       Kind == EFieldKind::ObjectRef)
        *reinterpret_cast<i32*>(p) = static_cast<i32>(v[0]);
    else if constexpr (Kind == EFieldKind::U32)
        *reinterpret_cast<u32*>(p) = static_cast<u32>(v[0]);
    else if constexpr (Kind == EFieldKind::F32)
        *reinterpret_cast<f32*>(p) = v[0];
    else if constexpr (Kind == EFieldKind::Vec2) {
        auto* d = reinterpret_cast<f32*>(p); d[0] = v[0]; d[1] = v[1];
    } else if constexpr (Kind == EFieldKind::Vec3) {
        auto* d = reinterpret_cast<f32*>(p);
        d[0] = v[0]; d[1] = v[1]; d[2] = v[2];
    } else if constexpr (Kind == EFieldKind::Vec4) {
        auto* d = reinterpret_cast<f32*>(p);
        d[0] = v[0]; d[1] = v[1]; d[2] = v[2]; d[3] = v[3];
    }
}

template<EFieldKind Kind>
void Read(const unsigned char* p, f32* out) noexcept
{
    if constexpr (Kind == EFieldKind::Bool)
        out[0] = *reinterpret_cast<const bool*>(p) ? 1.0f : 0.0f;
    else if constexpr (Kind == EFieldKind::I32 ||
                       Kind == EFieldKind::ObjectRef)
        out[0] = static_cast<f32>(*reinterpret_cast<const i32*>(p));
    else if constexpr (Kind == EFieldKind::U32)
        out[0] = static_cast<f32>(*reinterpret_cast<const u32*>(p));
    else if constexpr (Kind == EFieldKind::F32)
        out[0] = *reinterpret_cast<const f32*>(p);
    else if constexpr (Kind == EFieldKind::Vec2) {
        auto* d = reinterpret_cast<const f32*>(p); out[0] = d[0]; out[1] = d[1];
    } else if constexpr (Kind == EFieldKind::Vec3) {
        auto* d = reinterpret_cast<const f32*>(p);
        out[0] = d[0]; out[1] = d[1]; out[2] = d[2];
    } else if constexpr (Kind == EFieldKind::Vec4) {
        auto* d = reinterpret_cast<const f32*>(p);
        out[0] = d[0]; out[1] = d[1]; out[2] = d[2]; out[3] = d[3];
    }
}

inline constexpr ApplyFn kApplyDispatch[] = {
    &Apply<EFieldKind::Bool>, &Apply<EFieldKind::I32>,
    &Apply<EFieldKind::U32>, &Apply<EFieldKind::F32>,
    &Apply<EFieldKind::Vec2>, &Apply<EFieldKind::Vec3>,
    &Apply<EFieldKind::Vec4>, &Apply<EFieldKind::String>,
    &Apply<EFieldKind::Enum>, &Apply<EFieldKind::ObjectRef>
};
inline constexpr ReadFn kReadDispatch[] = {
    &Read<EFieldKind::Bool>, &Read<EFieldKind::I32>,
    &Read<EFieldKind::U32>, &Read<EFieldKind::F32>,
    &Read<EFieldKind::Vec2>, &Read<EFieldKind::Vec3>,
    &Read<EFieldKind::Vec4>, &Read<EFieldKind::String>,
    &Read<EFieldKind::Enum>, &Read<EFieldKind::ObjectRef>
};
inline constexpr bool kDispatchSupported[] = {
    true, true, true, true, true, true, true, false, false, true
};
inline constexpr usize kFieldKindCount =
    static_cast<usize>(EFieldKind::ObjectRef) + 1u;
static_assert(kFieldKindCount ==
              sizeof(kApplyDispatch) / sizeof(kApplyDispatch[0]));
static_assert(kFieldKindCount ==
              sizeof(kReadDispatch) / sizeof(kReadDispatch[0]));

} // reflect_apply_detail 名前空間

/** 組み込み種別が数値の読み書き記述子を持つか。プラグイン・未知種別は false。 */
constexpr bool ReflectFieldDispatchSupported(EFieldKind kind) noexcept
{
    const usize index = static_cast<usize>(kind);
    return index < reflect_apply_detail::kFieldKindCount &&
           reflect_apply_detail::kDispatchSupported[index];
}

/**
 * 1 フィールドの値 (f32 4 成分ソース) を obj の実メンバへ書き込む。
 *
 * @param obj 対象インスタンス先頭。
 * @param f 反射フィールド (offset/size/kind 付き)。
 * @param v 4 成分の値ソース (スカラは v[0] のみ使用)。
 */
inline void ApplyFieldValue(void* obj, const FReflectField& f, const f32 v[4]) noexcept {
    if (obj == nullptr || !FieldHasStorage(f)) return;
    auto* const p = static_cast<unsigned char*>(obj) + f.offset;
    const usize index = static_cast<usize>(f.kind);
    if (index < reflect_apply_detail::kFieldKindCount)
        reflect_apply_detail::kApplyDispatch[index](p, v);
}

/**
 * 1 フィールドの値を obj の実メンバから f32 4 成分へ読み出す。
 *
 * @param obj 対象インスタンス先頭。
 * @param f 反射フィールド。
 * @param out 4 成分の出力 (未使用成分は 0)。
 */
inline void ReadFieldValue(const void* obj, const FReflectField& f, f32 out[4]) noexcept {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (obj == nullptr || !FieldHasStorage(f)) return;
    const auto* const p = static_cast<const unsigned char*>(obj) + f.offset;
    const usize index = static_cast<usize>(f.kind);
    if (index < reflect_apply_detail::kFieldKindCount)
        reflect_apply_detail::kReadDispatch[index](p, out);
}

/**
 * 型の全フィールドを «スキーマ既定値 (defaults[])» で初期化する。
 *
 * @details C++ のメンバ初期化子とは別に、反射の defaults を実メンバへ書く。CreateById 直後に
 * 呼べば「反射上の既定値」と実体を一致させられる。offset 無しフィールドは skip。
 */
inline void ApplyDefaults(void* obj, const FTypeDesc& desc) noexcept {
    if (obj == nullptr || desc.fields == nullptr) return;
    for (u32 i = 0; i < desc.field_count; ++i) ApplyFieldValue(obj, desc.fields[i], desc.fields[i].defaults);
}

/**
 * 名前でフィールドを探し、値 (f32 4 成分) を実メンバへ書き込む。
 *
 * @return 該当フィールドへ書けたら true、無ければ false。
 */
inline bool ApplyValueByName(void* obj, const FTypeDesc& desc, const char* name, const f32 v[4]) noexcept {
    if (obj == nullptr || desc.fields == nullptr || name == nullptr) return false;
    for (u32 i = 0; i < desc.field_count; ++i) {
        const FReflectField& f = desc.fields[i];
        if (f.name == nullptr) continue;
        const char* a = f.name; const char* b = name;
        while (*a != '\0' && *a == *b) { ++a; ++b; }
        if (*a == '\0' && *b == '\0') {
            if (!FieldHasStorage(f)) return false;   // スキーマのみ → 書けない
            ApplyFieldValue(obj, f, v);
            return true;
        }
    }
    return false;
}

} // namespace acs::game
