// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "gameframework/Reflect.h"

namespace acs::game {

/**
 * フィールドが実メンバを持つかを返す。
 *
 * @param f 判定する反射フィールド。
 * @return メンバ領域があれば true。定義情報だけを持つACS_RPROPは false。
 */
inline bool FieldHasStorage(const FReflectField& f) noexcept { return f.size != 0u; }

namespace reflect_apply_detail {

/**
 * 型が決まった反射フィールドへ値を書き込む。
 *
 * @tparam Kind 書き込む反射フィールド種別。
 * @param p 書き込み先メンバの先頭。
 * @param v 書き込む4成分値。
 */
template<EFieldKind Kind>
inline void Apply(unsigned char* p, const f32* v) noexcept
{
    if constexpr (Kind == EFieldKind::Bool)
        *reinterpret_cast<bool*>(p) = v[0] != 0.0f;
    else if constexpr (Kind == EFieldKind::I32 || Kind == EFieldKind::ObjectRef)
        *reinterpret_cast<i32*>(p) = static_cast<i32>(v[0]);
    else if constexpr (Kind == EFieldKind::U32)
        *reinterpret_cast<u32*>(p) = static_cast<u32>(v[0]);
    else if constexpr (Kind == EFieldKind::F32)
        *reinterpret_cast<f32*>(p) = v[0];
    else if constexpr (Kind == EFieldKind::Vec2) {
        /** 2成分の書き込み先。 */
        auto* destination = reinterpret_cast<f32*>(p);
        destination[0] = v[0]; destination[1] = v[1];
    } else if constexpr (Kind == EFieldKind::Vec3) {
        /** 3成分の書き込み先。 */
        auto* destination = reinterpret_cast<f32*>(p);
        destination[0] = v[0]; destination[1] = v[1]; destination[2] = v[2];
    } else if constexpr (Kind == EFieldKind::Vec4) {
        /** 4成分の書き込み先。 */
        auto* destination = reinterpret_cast<f32*>(p);
        destination[0] = v[0]; destination[1] = v[1]; destination[2] = v[2]; destination[3] = v[3];
    }
}

/**
 * 型が決まった反射フィールドから値を読み出す。
 *
 * @tparam Kind 読み出す反射フィールド種別。
 * @param p 読み出し元メンバの先頭。
 * @param out 読み出した4成分値の出力先。
 */
template<EFieldKind Kind>
inline void Read(const unsigned char* p, f32* out) noexcept
{
    if constexpr (Kind == EFieldKind::Bool)
        out[0] = *reinterpret_cast<const bool*>(p) ? 1.0f : 0.0f;
    else if constexpr (Kind == EFieldKind::I32 || Kind == EFieldKind::ObjectRef)
        out[0] = static_cast<f32>(*reinterpret_cast<const i32*>(p));
    else if constexpr (Kind == EFieldKind::U32)
        out[0] = static_cast<f32>(*reinterpret_cast<const u32*>(p));
    else if constexpr (Kind == EFieldKind::F32)
        out[0] = *reinterpret_cast<const f32*>(p);
    else if constexpr (Kind == EFieldKind::Vec2) {
        /** 2成分の読み出し元。 */
        auto* source = reinterpret_cast<const f32*>(p);
        out[0] = source[0]; out[1] = source[1];
    } else if constexpr (Kind == EFieldKind::Vec3) {
        /** 3成分の読み出し元。 */
        auto* source = reinterpret_cast<const f32*>(p);
        out[0] = source[0]; out[1] = source[1]; out[2] = source[2];
    } else if constexpr (Kind == EFieldKind::Vec4) {
        /** 4成分の読み出し元。 */
        auto* source = reinterpret_cast<const f32*>(p);
        out[0] = source[0]; out[1] = source[1]; out[2] = source[2]; out[3] = source[3];
    }
}

/** 各組み込み種別が数値の読み書きに対応するかを示す表。 */
inline constexpr bool kDispatchSupported[] = {true, true, true, true, true, true, true, false, false, true};
/** 組み込み反射フィールド種別の総数。 */
inline constexpr usize kFieldKindCount = static_cast<usize>(EFieldKind::ObjectRef) + 1u;
static_assert(kFieldKindCount == sizeof(kDispatchSupported) / sizeof(kDispatchSupported[0]));

/**
 * 実行時の種別を、インライン化可能な型別処理へ振り分ける。
 *
 * @details 関数ポインター表を使わず、コンパイラーが各特殊化を呼び出し元へ
 * 展開できる形を保つ。未対応種別は従来どおり何もしない。
 *
 * @param kind 書き込む反射フィールド種別。
 * @param p 書き込み先メンバの先頭。
 * @param v 書き込む4成分値。
 */
inline void ApplyByKind(EFieldKind kind, unsigned char* p, const f32* v) noexcept
{
    switch (kind) {
    case EFieldKind::Bool:      Apply<EFieldKind::Bool>(p, v); break;
    case EFieldKind::I32:       Apply<EFieldKind::I32>(p, v); break;
    case EFieldKind::U32:       Apply<EFieldKind::U32>(p, v); break;
    case EFieldKind::F32:       Apply<EFieldKind::F32>(p, v); break;
    case EFieldKind::Vec2:      Apply<EFieldKind::Vec2>(p, v); break;
    case EFieldKind::Vec3:      Apply<EFieldKind::Vec3>(p, v); break;
    case EFieldKind::Vec4:      Apply<EFieldKind::Vec4>(p, v); break;
    case EFieldKind::ObjectRef: Apply<EFieldKind::ObjectRef>(p, v); break;
    default: break;
    }
}

/**
 * 種別ごとの読み出し処理を間接呼び出しなしで選択する。
 *
 * @param kind 読み出す反射フィールド種別。
 * @param p 読み出し元メンバの先頭。
 * @param out 読み出した4成分値の出力先。
 */
inline void ReadByKind(EFieldKind kind, const unsigned char* p, f32* out) noexcept
{
    switch (kind) {
    case EFieldKind::Bool:      Read<EFieldKind::Bool>(p, out); break;
    case EFieldKind::I32:       Read<EFieldKind::I32>(p, out); break;
    case EFieldKind::U32:       Read<EFieldKind::U32>(p, out); break;
    case EFieldKind::F32:       Read<EFieldKind::F32>(p, out); break;
    case EFieldKind::Vec2:      Read<EFieldKind::Vec2>(p, out); break;
    case EFieldKind::Vec3:      Read<EFieldKind::Vec3>(p, out); break;
    case EFieldKind::Vec4:      Read<EFieldKind::Vec4>(p, out); break;
    case EFieldKind::ObjectRef: Read<EFieldKind::ObjectRef>(p, out); break;
    default: break;
    }
}

} // reflect_apply_detail 名前空間

/**
 * 組み込み種別が数値の読み書きに対応するかを返す。
 *
 * @param kind 判定する反射フィールド種別。
 * @return 対応する組み込み種別なら true。プラグイン・未知種別は false。
 */
constexpr bool ReflectFieldDispatchSupported(EFieldKind kind) noexcept
{
    /** 対応表へ使う列挙値の添字。 */
    const usize index = static_cast<usize>(kind);
    return index < reflect_apply_detail::kFieldKindCount && reflect_apply_detail::kDispatchSupported[index];
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
    /** 対象フィールドの書き込み先メンバ。 */
    auto* const p = static_cast<unsigned char*>(obj) + f.offset;
    reflect_apply_detail::ApplyByKind(f.kind, p, v);
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
    /** 対象フィールドの読み出し元メンバ。 */
    const auto* const p = static_cast<const unsigned char*>(obj) + f.offset;
    reflect_apply_detail::ReadByKind(f.kind, p, out);
}

/**
 * 型の全フィールドを «スキーマ既定値 (defaults[])» で初期化する。
 *
 * @details C++ のメンバ初期化子とは別に、反射の defaults を実メンバへ書く。CreateById 直後に
 * 呼べば「反射上の既定値」と実体を一致させられる。offset 無しフィールドは skip。
 *
 * @param obj 初期化する対象インスタンス。
 * @param desc 適用する型記述子。
 */
inline void ApplyDefaults(void* obj, const FTypeDesc& desc) noexcept {
    if (obj == nullptr || desc.fields == nullptr) return;
    /** 既定値を適用するフィールド位置。 */
    for (u32 field_index = 0; field_index < desc.field_count; ++field_index) ApplyFieldValue(obj, desc.fields[field_index], desc.fields[field_index].defaults);
}

/**
 * 名前でフィールドを探し、値 (f32 4 成分) を実メンバへ書き込む。
 *
 * @param obj 書き込み先の対象インスタンス。
 * @param desc 検索する型記述子。
 * @param name 検索するフィールド名。
 * @param v 書き込む4成分値。
 * @return 該当フィールドへ書けたら true、無ければ false。
 */
inline bool ApplyValueByName(void* obj, const FTypeDesc& desc, const char* name, const f32 v[4]) noexcept {
    if (obj == nullptr || desc.fields == nullptr || name == nullptr) return false;
    /** 名前を照合するフィールド位置。 */
    for (u32 field_index = 0; field_index < desc.field_count; ++field_index) {
        /** 現在照合している反射フィールド。 */
        const FReflectField& f = desc.fields[field_index];
        if (f.name == nullptr) continue;
        /** 登録済みフィールド名の比較位置。 */
        const char* registered_character = f.name;
        /** 検索名の比較位置。 */
        const char* requested_character = name;
        while (*registered_character != '\0' && *registered_character == *requested_character) { ++registered_character; ++requested_character; }
        if (*registered_character == '\0' && *requested_character == '\0') {
            if (!FieldHasStorage(f)) return false;   // スキーマのみ → 書けない
            ApplyFieldValue(obj, f, v);
            return true;
        }
    }
    return false;
}

} // acs::game 名前空間
