// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar J — TypeInfo (Phase 2)
//
// RTTI 不使用の最小反射 (reflection)。シリアライザ / インスペクタ / デバッガ
// などが「フィールド名 / 型名 / オフセット / サイズ」を知るための最小限の
// メタ情報を、コンパイル時に生成する。
//
// 使い方:
//   struct PlayerState {
//       acs::f32 x;
//       acs::f32 y;
//       acs::i32 hp;
//   };
//
//   // (ヘッダ末尾、グローバル空間で)
//   ACS_GAME_REFLECT(PlayerState,
//       ACS_GAME_FIELD(PlayerState, x,  "f32"),
//       ACS_GAME_FIELD(PlayerState, y,  "f32"),
//       ACS_GAME_FIELD(PlayerState, hp, "i32"))
//
//   // 取得:
//   const auto& ti = acs::game::Reflect<PlayerState>();
//   for (acs::u32 i = 0; i < ti.field_count; ++i) {
//       const auto& f = ti.fields[i];
//       // f.name / f.type_name / f.offset / f.size を使う
//   }
//
// 設計選択:
//   ・RTTI / <typeinfo> 不使用。型 ID は AppState / Component2D と同じ
//     「template static int のアドレス」パターン (`TypeTag<T>()`)。
//   ・ヘッダオンリ。`TypeInfo<T>` の特殊化に static const FieldInfo[] を持ち、
//     `Reflect<T>()` から TypeInfoBase の static 参照を返す。
//   ・default `TypeInfo<T>` は「未反射」を表す空特殊化 (field_count == 0)。
//   ・依存は foundation/Types.h + <cstddef> (offsetof) のみ。STL 不使用。
#pragma once

#include "foundation/Types.h"

#include <cstddef>   // offsetof

namespace acs::game {

// -----------------------------------------------------------------------------
// FieldInfo — 反射された 1 フィールドの記述
// -----------------------------------------------------------------------------
struct FieldInfo {
    const c8* name;       // フィールド名 (例: "x")
    const c8* type_name;  // 型名文字列 (ユーザー指定、例: "f32")
    usize     offset;     // 型先頭からのバイトオフセット (offsetof)
    usize     size;       // フィールドのバイトサイズ (sizeof)
};

// -----------------------------------------------------------------------------
// TypeInfoBase — 型 T の反射メタ情報の base 形 (Reflect<T>() の戻り)
// -----------------------------------------------------------------------------
struct TypeInfoBase {
    const c8*        type_name;     // 型名文字列 (例: "PlayerState")
    usize            size;          // sizeof(T)
    usize            alignment;     // alignof(T)
    const FieldInfo* fields;        // FieldInfo 配列 (field_count 要素)
    u32              field_count;   // フィールド数
    const void*      type_tag;      // 一意 type ID (= TypeTag<T>())
};

// -----------------------------------------------------------------------------
// TypeTag<T>() — 型 T の一意 ID (static int のアドレス)
//
// AppState / Component2D と同じパターン。RTTI 不使用、各 T インスタンス化で
// 別 instantiation = 別アドレス。
// -----------------------------------------------------------------------------
template<typename T>
inline const void* TypeTag() noexcept {
    static const int s_tag = 0;
    return static_cast<const void*>(&s_tag);
}

// -----------------------------------------------------------------------------
// ReflectedFields<T> — 型 T の static FieldInfo 配列ホルダ
//
// ACS_GAME_REFLECT macro が特殊化を生成する。
// default 特殊化は空 (fields == nullptr / count == 0)。
// -----------------------------------------------------------------------------
template<typename T>
struct ReflectedFields {
    static constexpr const FieldInfo* Fields() noexcept { return nullptr; }
    static constexpr u32              Count()  noexcept { return 0; }
};

// -----------------------------------------------------------------------------
// TypeInfo<T> — 型 T の反射情報 (ユーザー特殊化される)
//
// default 特殊化 = 未反射 (type_name == nullptr / field_count == 0)。
// ACS_GAME_REFLECT macro でユーザーが特殊化を生成する。
// -----------------------------------------------------------------------------
template<typename T>
struct TypeInfo {
    static const TypeInfoBase& Get() noexcept {
        static const TypeInfoBase s_info {
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

// -----------------------------------------------------------------------------
// Reflect<T>() — TypeInfo<T> の static 取得 (短縮形)
// -----------------------------------------------------------------------------
template<typename T>
inline const TypeInfoBase& Reflect() noexcept {
    return TypeInfo<T>::Get();
}

} // namespace acs::game

// =============================================================================
// ACS_GAME_FIELD(T, field, type_str)
//   — FieldInfo の構造体初期化子を返す。ACS_GAME_REFLECT の引数として使う。
// =============================================================================
#define ACS_GAME_FIELD(T, field, type_str)                                       \
    ::acs::game::FieldInfo{                                                      \
        #field,                                                                  \
        (type_str),                                                              \
        static_cast<::acs::usize>(offsetof(T, field)),                           \
        static_cast<::acs::usize>(sizeof(((T*)0)->field))                        \
    }

// =============================================================================
// ACS_GAME_REFLECT(T, fields...)
//   — 型 T を反射する。`ReflectedFields<T>` と `TypeInfo<T>` の特殊化を生成。
//   グローバル名前空間で呼ぶこと (template 特殊化のため)。
//
// 例:
//   ACS_GAME_REFLECT(PlayerState,
//       ACS_GAME_FIELD(PlayerState, x, "f32"),
//       ACS_GAME_FIELD(PlayerState, y, "f32"))
// =============================================================================
#define ACS_GAME_REFLECT(T, ...)                                                 \
    namespace acs::game {                                                        \
    template<>                                                                   \
    struct ReflectedFields<T> {                                                  \
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
    struct TypeInfo<T> {                                                         \
        static const ::acs::game::TypeInfoBase& Get() noexcept {                 \
            static const ::acs::game::TypeInfoBase s_info {                      \
                /* type_name   */ #T,                                            \
                /* size        */ sizeof(T),                                     \
                /* alignment   */ alignof(T),                                    \
                /* fields      */ ReflectedFields<T>::Fields(),                  \
                /* field_count */ ReflectedFields<T>::Count(),                   \
                /* type_tag    */ ::acs::game::TypeTag<T>(),                     \
            };                                                                   \
            return s_info;                                                       \
        }                                                                        \
    };                                                                           \
    } // namespace acs::game
