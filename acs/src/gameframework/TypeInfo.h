// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar J — TTypeInfo
//
// RTTI 不使用の最小反射 (reflection)。シリアライザ / インスペクタ / デバッガ
// などが「フィールド名 / 型名 / オフセット / サイズ」を知るための最小限の
// メタ情報を、コンパイル時に生成する。
//
// 使い方:
//   struct FPlayerState {
//       acs::f32 x;
//       acs::f32 y;
//       acs::i32 hp;
//   };
//
//   // (ヘッダ末尾、グローバル空間で)
//   ACS_GAME_REFLECT(FPlayerState,
//       ACS_GAME_FIELD(FPlayerState, x,  "f32"),
//       ACS_GAME_FIELD(FPlayerState, y,  "f32"),
//       ACS_GAME_FIELD(FPlayerState, hp, "i32"))
//
//   // 取得:
//   const auto& ti = acs::game::Reflect<FPlayerState>();
//   for (acs::u32 i = 0; i < ti.field_count; ++i) {
//       const auto& f = ti.fields[i];
//       // f.name / f.type_name / f.offset / f.size を使う
//   }
//
// 設計選択:
//   ・RTTI / <typeinfo> 不使用。型 ID は AppState / AComponent と同じ
//     「template static int のアドレス」パターン (`TypeTag<T>()`)。
//   ・ヘッダオンリ。`TTypeInfo<T>` の特殊化に static const FFieldInfo[] を持ち、
//     `Reflect<T>()` から FTypeInfoBase の static 参照を返す。
//   ・default `TTypeInfo<T>` は「未反射」を表す空特殊化 (field_count == 0)。
//   ・依存は foundation/Types.h + <cstddef> (offsetof) のみ。STL 不使用。
#pragma once

#include "foundation/Types.h"

#include <cstddef>   // offsetof

namespace acs::game {

/**
 * 反射された 1 フィールドの記述。
 */
struct FFieldInfo {
    /** フィールド名 (例: "x")。 */
    const c8* name;

    /** 型名文字列 (ユーザー指定、例: "f32")。 */
    const c8* type_name;

    /** 型先頭からのバイトオフセット (offsetof)。 */
    usize     offset;

    /** フィールドのバイトサイズ (sizeof)。 */
    usize     size;
};

/**
 * 型 T の反射メタ情報の base 形 (Reflect<T>() の戻り値)。
 */
struct FTypeInfoBase {
    /** 型名文字列 (例: "FPlayerState")。 */
    const c8*        type_name;

    /** sizeof(T)。 */
    usize            size;

    /** alignof(T)。 */
    usize            alignment;

    /** FFieldInfo 配列 (field_count 要素)。 */
    const FFieldInfo* fields;

    /** フィールド数。 */
    u32              field_count;

    /** 一意 type ID (= TypeTag<T>())。 */
    const void*      type_tag;
};

/**
 * 型 T の一意 ID (static int のアドレス) を返す。
 *
 * @details
 * AppState / AComponent と同じパターン。RTTI 不使用で、各 T のインスタンス化ごとに
 * 別 instantiation = 別アドレスになる。
 * @tparam T ID を取得する型。
 * @return 型 T を一意に識別する不透明ポインタ。
 */
template<typename T>
inline const void* TypeTag() noexcept {
    static const int s_tag = 0;
    return static_cast<const void*>(&s_tag);
}

/**
 * 型 T の static FFieldInfo 配列ホルダ。
 *
 * @details
 * ACS_GAME_REFLECT macro が特殊化を生成する。default 特殊化は空
 * (fields == nullptr / count == 0)。
 * @tparam T 反射対象の型。
 */
template<typename T>
struct TReflectedFields {
    /**
     * FFieldInfo 配列を返す。
     *
     * @return FFieldInfo 配列 (default 特殊化は nullptr)。
     */
    static constexpr const FFieldInfo* Fields() noexcept { return nullptr; }

    /**
     * フィールド数を返す。
     *
     * @return フィールド数 (default 特殊化は 0)。
     */
    static constexpr u32              Count()  noexcept { return 0; }
};

/**
 * 型 T の反射情報 (ユーザー特殊化される)。
 *
 * @details
 * default 特殊化は未反射を表す (type_name == nullptr / field_count == 0)。
 * ACS_GAME_REFLECT macro でユーザーが特殊化を生成する。
 * @tparam T 反射対象の型。
 */
template<typename T>
struct TTypeInfo {
    /**
     * 型 T の反射情報への static 参照を返す。
     *
     * @return 型 T の FTypeInfoBase への const 参照。
     */
    static const FTypeInfoBase& Get() noexcept {
        static const FTypeInfoBase s_info {
            /* type_name   */ nullptr,
            /* size        */ sizeof(T),
            /* alignment   */ alignof(T),
            /* fields      */ nullptr,
            /* field_count */ 0u,
            /* type_tag    */ TypeTag<T>(),
        };
        return s_info;
    }
};

/**
 * TTypeInfo<T> の反射情報を取得する短縮形。
 *
 * @tparam T 反射情報を取得する型。
 * @return 型 T の FTypeInfoBase への const 参照。
 */
template<typename T>
inline const FTypeInfoBase& Reflect() noexcept {
    return TTypeInfo<T>::Get();
}

} // namespace acs::game

/**
 * FFieldInfo の構造体初期化子を生成するマクロ (ACS_GAME_REFLECT の引数に使う)。
 *
 * @param T フィールドを持つ型。
 * @param field フィールド名。
 * @param type_str 型名文字列。
 */
#define ACS_GAME_FIELD(T, field, type_str)                                       \
    ::acs::game::FieldInfo{                                                      \
        #field,                                                                  \
        (type_str),                                                              \
        static_cast<::acs::usize>(offsetof(T, field)),                           \
        static_cast<::acs::usize>(sizeof(((T*)0)->field))                        \
    }

/**
 * 型 T を反射するマクロ。
 *
 * @details
 * TReflectedFields<T> と TTypeInfo<T> の特殊化を生成する。template 特殊化のため
 * グローバル名前空間で呼ぶこと。
 * @param T 反射する型。
 * @param ... ACS_GAME_FIELD で生成した FFieldInfo 初期化子の可変長リスト。
 */
#define ACS_GAME_REFLECT(T, ...)                                                 \
    namespace acs::game {                                                        \
    template<>                                                                   \
    struct TReflectedFields<T> {                                                 \
        static const ::acs::game::FieldInfo* Fields() noexcept {                 \
            static const ::acs::game::FieldInfo s_fields[] = { __VA_ARGS__ };    \
            return s_fields;                                                     \
        }                                                                        \
        static ::acs::u32 Count() noexcept {                                     \
            static const ::acs::game::FieldInfo s_fields[] = { __VA_ARGS__ };    \
            return static_cast<::acs::u32>(                                      \
                sizeof(s_fields) / sizeof(s_fields[0]));                         \
        }                                                                        \
    };                                                                           \
    template<>                                                                   \
    struct TTypeInfo<T> {                                                         \
        static const ::acs::game::FTypeInfoBase& Get() noexcept {                 \
            static const ::acs::game::FTypeInfoBase s_info {                      \
                /* type_name   */ #T,                                            \
                /* size        */ sizeof(T),                                     \
                /* alignment   */ alignof(T),                                    \
                /* fields      */ TReflectedFields<T>::Fields(),                 \
                /* field_count */ TReflectedFields<T>::Count(),                  \
                /* type_tag    */ ::acs::game::TypeTag<T>(),                     \
            };                                                                   \
            return s_info;                                                       \
        }                                                                        \
    };                                                                           \
    } // namespace acs::game
