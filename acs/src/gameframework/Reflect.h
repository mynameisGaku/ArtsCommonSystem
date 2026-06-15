// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — 統一リフレクション / 型レジストリ (Reflect)
// -----------------------------------------------------------------------------
// 「エンジンの型 (Object / Component / Node / Scene / Asset / System / Enum /
// Interface / Event / Command …) をマーク + 反射 (名前・サイズ・フィールド・列挙値・
// ファクトリ・カテゴリ・能力) して、エンジン全体から 1 つのレジストリで列挙・
// 名前/ID 検索・生成できる」土台。
//
// 設計の要点 (ACS の型は Object/Component に留まらない — 実コードから網羅):
//   ・**カテゴリ** ETypeCategory … 何の種類か。Struct/Enum/Object/Component/Node/
//     Scene/Asset/System/Prefab/Interface/Event/Command/Resource/Service/Custom。
//     新種は Custom + category_name で拡張できる (将来の種類に耐える)。
//   ・**能力トレイト** ETypeTraits … 何ができるか (直交)。Serializable/Networked/
//     EditorVisible/Scriptable/Abstract/Instantiable のビット OR。
//   ・**Enum 反射** … 列挙体は value↔name を持つ (editor コンボ/シリアライズ/スクリプト)。
//   ・既存の TypeInfo.h (TU ローカル・コンパイル時のみ) を補完し、グローバル自動登録の
//     FTypeRegistry を提供。フィールド種別は InspectorSeam.h の EFieldKind を共通利用。
//
// 使い方:
//   struct FHealth { acs::f32 hp; acs::f32 max_hp; bool alive; };
//   ACS_COMPONENT(FHealth,
//       ACS_RFIELD(FHealth, hp,     acs::game::EFieldKind::F32),
//       ACS_RFIELD(FHealth, max_hp, acs::game::EFieldKind::F32),
//       ACS_RFIELD(FHealth, alive,  acs::game::EFieldKind::Bool))
//
//   enum class EWeapon : acs::u8 { Sword, Bow, Staff };
//   ACS_REFLECT_ENUM(EWeapon,
//       ACS_EVAL(EWeapon, Sword), ACS_EVAL(EWeapon, Bow), ACS_EVAL(EWeapon, Staff))
//
//   auto& reg = acs::game::FTypeRegistry::Get();
//   const auto* d = reg.FindByName("FHealth");      // 名前で型
//   void* obj     = reg.Create("FHealth");          // factory 生成
//   reg.Destroy(d->id, obj);
//
// 規約: no-STL / no-exceptions / 固定長 / 全 noexcept。型 ID は型名の FNV-1a (u32)。
// マクロは C++20 inline 変数で生成し、生成シンボルは __LINE__ ではなく「型名」でキー付け
// する → 別ヘッダで同名・同種別の型を反射しても全 TU 単一実体 = 1 度だけ登録 (同名衝突や
// ODR 違反が起きない。同じ型を 2 ヘッダで反射した場合も token 一致で inline マージされる)。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "gameframework/InspectorSeam.h"   // EFieldKind (フィールド種別を共通化)
#include "memory/Allocator.h"              // FAllocator / DefaultAllocator
#include "memory/New.h"                    // New<T> / Delete (engine allocator factory)

#include <cstddef>   // offsetof
#include <cstring>   // std::strcmp

namespace acs::game {

/** 型 ID (型名の FNV-1a ハッシュ、実行をまたいで安定)。 */
using FTypeId = u32;

/**
 * 型のカテゴリ (何の種類の型か)。ACS の実型から網羅。新種は Custom で拡張する。
 */
enum class ETypeCategory : u8 {
    Struct    = 0,   /**< 単純な値型 / POD (PlayerState, material params)。 */
    Enum      = 1,   /**< 列挙体 (value↔name の反射を持つ)。 */
    Object    = 2,   /**< FObject 派生のエンジンオブジェクト。 */
    Component = 3,   /**< ECS / 2D コンポーネント。 */
    Node      = 4,   /**< シーングラフのノード (FNode2D)。 */
    Scene     = 5,   /**< ゲームシーン。 */
    Asset     = 6,   /**< アセット型 (Image/Mesh/Audio/Text/Binary…)。 */
    System    = 7,   /**< ゲームプレイ / ECS システム。 */
    Prefab    = 8,   /**< ノードテンプレート。 */
    Interface = 9,   /**< 差し替え可能なバックエンド interface (IRhiDevice, IAssetLoader…)。 */
    Event     = 10,  /**< イベント / メッセージ型。 */
    Command   = 11,  /**< undo/redo コマンド。 */
    Resource  = 12,  /**< GPU/RHI リソース (texture/buffer/pipeline)。 */
    Service   = 13,  /**< エンジンのサブシステム / サービス。 */
    Custom    = 255, /**< ユーザー定義カテゴリ (category_name を併用)。 */
};

/** 型の能力 (カテゴリと直交するビットフラグ)。 */
enum ETypeTraits : u32 {
    TRAIT_NONE           = 0u,
    TRAIT_SERIALIZABLE   = 1u << 0,  /**< save/load 対象。 */
    TRAIT_NETWORKED      = 1u << 1,  /**< レプリケーション対象。 */
    TRAIT_EDITOR_VISIBLE = 1u << 2,  /**< editor に表示する。 */
    TRAIT_SCRIPTABLE     = 1u << 3,  /**< スクリプトへ公開する。 */
    TRAIT_ABSTRACT       = 1u << 4,  /**< default 構築不可 (interface/基底)。 */
    TRAIT_INSTANTIABLE   = 1u << 5,  /**< default 構築できる (factory あり)。 */
};

/** フィールド属性フラグ (editor / serializer のヒント)。 */
enum EFieldFlags : u32 {
    FIELD_NONE      = 0u,
    FIELD_READONLY  = 1u << 0,   /**< 編集不可 (表示のみ)。 */
    FIELD_HIDDEN    = 1u << 1,   /**< UI に出さない。 */
    FIELD_TRANSIENT = 1u << 2,   /**< シリアライズ対象外。 */
};

/** 反射された 1 フィールド (名前 + 種別 + オフセット + サイズ + フラグ + 既定値 + カテゴリ)。 */
struct FReflectField {
    const char* name;          /**< フィールド名 (例: "hp")。 */
    EFieldKind  kind;          /**< データ種別 (Bool/I32/.../Enum)。 */
    u32         offset;        /**< 型先頭からのバイトオフセット (名前のみ反射では 0)。 */
    u32         size;          /**< フィールドのバイトサイズ (名前のみ反射では 0)。 */
    u32         flags;         /**< EFieldFlags の OR。 */
    f32         defaults[4];   /**< スキーマ既定値 (最大 4 成分。ACS_RPROP* 用、offsetof 反射は 0)。 */
    const char* category = nullptr;  /**< UPROPERTY(Category="…") の分類名。インスペクタのグループ見出し (既定 nullptr)。 */
};

/** 反射された列挙値 1 件 (名前 + 整数値)。 */
struct FEnumValue {
    const char* name;      /**< 列挙子名 (例: "Sword")。 */
    i64         value;     /**< 整数値。 */
};

/** 1 つの型のメタ情報 (レジストリに 1 件登録される)。 */
struct FTypeDesc {
    const char*          name;          /**< 型名。 */
    FTypeId              id;            /**< 型名ハッシュ。 */
    u32                  size;          /**< sizeof(T)。 */
    u32                  align;         /**< alignof(T)。 */
    ETypeCategory        category;      /**< 型カテゴリ。 */
    u32                  traits;        /**< ETypeTraits の OR。 */
    FTypeId              base;          /**< 基底型 ID (無ければ 0)。 */
    const FReflectField* fields;        /**< フィールド配列 (Enum 以外)。 */
    u32                  field_count;   /**< フィールド数。 */
    const FEnumValue*    enum_values;   /**< 列挙値配列 (Enum のみ、他は nullptr)。 */
    u32                  enum_count;    /**< 列挙値数。 */
    void* (*construct)();              /**< default 構築 (不可なら nullptr)。 */
    void  (*destruct)(void*);          /**< construct の戻り値を破棄。 */
    const char*          category_name; /**< Custom カテゴリ名 (他は nullptr)。 */
};

/** 型名の FNV-1a ハッシュ (FTypeId 生成、コンパイル時可)。 */
constexpr FTypeId AcsTypeHash(const char* s) noexcept {
    u32 h = 2166136261u;
    for (; *s != '\0'; ++s) h = (h ^ static_cast<u32>(static_cast<unsigned char>(*s))) * 16777619u;
    return h;
}

/** 型 T を default 構築して void* で返す (不可なら nullptr)。エンジンアロケータで確保する。 */
template<class T> inline void* AcsConstruct() noexcept {
    if constexpr (__is_constructible(T)) return New<T>(DefaultAllocator());
    else                                 return nullptr;
}
/** 型 T のインスタンスを破棄する (AcsConstruct と同じエンジンアロケータで解放)。 */
template<class T> inline void AcsDestruct(void* p) noexcept { Delete(DefaultAllocator(), static_cast<T*>(p)); }

/** 型 T の既定トレイト (構築可否で Instantiable/Abstract を決める)。 */
template<class T> constexpr u32 AcsAutoTraits() noexcept {
    u32 t = TRAIT_SERIALIZABLE | TRAIT_EDITOR_VISIBLE;
    if constexpr (__is_constructible(T)) t |= TRAIT_INSTANTIABLE;
    else                                 t |= TRAIT_ABSTRACT;
    return t;
}

/**
 * エンジン全体で 1 つの型レジストリ。全 ACS_REFLECT* 型が静的初期化時に自動登録される。
 *
 * @details
 * 名前 / ID で型を引き、フィールド / 列挙値を列挙し、ファクトリで生成できる。固定長
 * (kMax) で STL 不使用。エディタ・シリアライザ・スクリプト束縛・ネットワークなどが
 * 「型を知らずに」横断的に使うための単一の真実点。
 */
class FTypeRegistry {
public:
    /** 登録できる型数の上限。 */
    static constexpr u32 kMax = 2048u;

    /** プロセス唯一のレジストリを返す (Meyers singleton)。 */
    static FTypeRegistry& Get() noexcept;

    /**
     * 型を登録する (ACS_REFLECT* の自動登録から呼ばれる。同 ID は無視)。
     *
     * @param d 登録する型記述 (静的寿命、非所有)。
     * @return 登録 (または既存) なら true、上限到達で false。
     */
    bool Register(const FTypeDesc* d) noexcept;

    /** 登録型数。 */
    u32 Count() const noexcept { return m_Count; }

    /** i 番目の型記述 (範囲外は nullptr)。 */
    const FTypeDesc* At(u32 i) const noexcept { return (i < m_Count) ? m_Types[i] : nullptr; }

    /** 名前で型を引く (無ければ nullptr)。 */
    const FTypeDesc* FindByName(const char* name) const noexcept;

    /** ID で型を引く (無ければ nullptr)。 */
    const FTypeDesc* FindById(FTypeId id) const noexcept;

    /** カテゴリの型数を数える。 */
    u32 CountOfCategory(ETypeCategory cat) const noexcept;

    /** カテゴリで絞って nth 番目を返す (列挙用、無ければ nullptr)。 */
    const FTypeDesc* AtOfCategory(ETypeCategory cat, u32 nth) const noexcept;

    /** 名前で型を default 構築する (factory、不可/未登録は nullptr)。 */
    void* Create(const char* name) const noexcept;

    /** ID で型を default 構築する。 */
    void* CreateById(FTypeId id) const noexcept;

    /** ID の型のインスタンスを破棄する (Create の戻り値を渡す)。 */
    void Destroy(FTypeId id, void* obj) const noexcept;

private:
    FTypeRegistry() noexcept = default;
    const FTypeDesc* m_Types[kMax] = {};
    u32              m_Count       = 0u;
};

/** ACS_REFLECT* が生成する自動登録ヘルパ (静的初期化時に Register を呼ぶ)。 */
struct FTypeAutoRegister {
    explicit FTypeAutoRegister(const FTypeDesc* d) noexcept { FTypeRegistry::Get().Register(d); }
};

/** 型 T の FTypeDesc を返す (ACS_REFLECT* が特殊化する。未反射型は nullptr)。 */
template<class T> inline const FTypeDesc* AcsTypeDescOf() noexcept { return nullptr; }

} // namespace acs::game

// =============================================================================
// マクロ
// =============================================================================

/** トークン連結 (内部用)。 */
#define ACS_RCAT2(a, b) a##b
#define ACS_RCAT(a, b)  ACS_RCAT2(a, b)

/** 反射フィールド記述子を作る (Type のメンバ member、種別 fieldkind)。 */
#define ACS_RFIELD(Type, member, fieldkind)                                     \
    ::acs::game::FReflectField{ #member, (fieldkind),                           \
        static_cast<::acs::u32>(offsetof(Type, member)),                        \
        static_cast<::acs::u32>(sizeof(((Type*)0)->member)), ::acs::game::FIELD_NONE }

/** フラグ付き反射フィールド記述子。 */
#define ACS_RFIELD_F(Type, member, fieldkind, flags)                            \
    ::acs::game::FReflectField{ #member, (fieldkind),                           \
        static_cast<::acs::u32>(offsetof(Type, member)),                        \
        static_cast<::acs::u32>(sizeof(((Type*)0)->member)), (flags), {0.0f,0.0f,0.0f,0.0f} }

/**
 * offsetof で «実体メンバ» を反射しつつ «既定値» も持つフィールド記述子 (codegen 用)。
 *
 * @details ACS_RFIELD は defaults=0、ACS_RPROP は offset=0。両方が欲しい (= エディタの
 * 既定値表示 + 実インスタンスへの値書き込み) 場合に使う。member は public、Type は単一継承
 * (多態でも MSVC の offsetof は安定) であること。ACS_CLASS/ACS_PROPERTY のコード生成が出力する。
 */
#define ACS_RFIELD_D(Type, member, fieldkind, d0, d1, d2, d3)                   \
    ::acs::game::FReflectField{ #member, (fieldkind),                           \
        static_cast<::acs::u32>(offsetof(Type, member)),                        \
        static_cast<::acs::u32>(sizeof(((Type*)0)->member)),                    \
        ::acs::game::FIELD_NONE, { static_cast<::acs::f32>(d0),                 \
        static_cast<::acs::f32>(d1), static_cast<::acs::f32>(d2),               \
        static_cast<::acs::f32>(d3) } }

/**
 * offset + 既定値 + «フラグ» 付きフィールド記述子 (codegen が UPROPERTY 指定子から出力)。
 *
 * @details flags = EFieldFlags の OR。VisibleAnywhere→FIELD_READONLY、Hidden→FIELD_HIDDEN 等。
 */
#define ACS_RFIELD_DF(Type, member, fieldkind, flags, d0, d1, d2, d3)            \
    ::acs::game::FReflectField{ #member, (fieldkind),                           \
        static_cast<::acs::u32>(offsetof(Type, member)),                        \
        static_cast<::acs::u32>(sizeof(((Type*)0)->member)),                    \
        static_cast<::acs::u32>(flags), { static_cast<::acs::f32>(d0),          \
        static_cast<::acs::f32>(d1), static_cast<::acs::f32>(d2),               \
        static_cast<::acs::f32>(d3) } }

/** offset + 既定値 + フラグ + «カテゴリ» 付きフィールド記述子 (codegen が Category 指定子から出力)。 */
#define ACS_RFIELD_DFC(Type, member, fieldkind, flags, category, d0, d1, d2, d3)  \
    ::acs::game::FReflectField{ #member, (fieldkind),                           \
        static_cast<::acs::u32>(offsetof(Type, member)),                        \
        static_cast<::acs::u32>(sizeof(((Type*)0)->member)),                    \
        static_cast<::acs::u32>(flags), { static_cast<::acs::f32>(d0),          \
        static_cast<::acs::f32>(d1), static_cast<::acs::f32>(d2),               \
        static_cast<::acs::f32>(d3) }, (category) }

// ----- 名前のみの反射プロパティ (offsetof 不使用) ------------------------------
// private メンバや多態 (非 standard-layout) 型は offsetof が使えない。そこで「編集可能
// フィールドのスキーマ (名前 + 種別 + 既定値)」だけを反射する。値の実体はメンバではなく
// 利用側 (エディタ) がスキーマ順に保持する。型ヘッダを書き換えずに editor へ公開できる。
//   ACS_RPROP_V2("size", 1.0f, 1.0f)   // FVec2、既定 (1,1)
//   ACS_RPROP_V4("tint", 1,1,1,1)      // FVec4 (RGBA)、既定 白
//   ACS_RPROP_F ("intensity", 1.0f)    // F32
//   ACS_RPROP_I ("points", 24)         // I32
//   ACS_RPROP_B ("enabled", true)      // Bool (既定 1/0)

/** 名前のみ反射プロパティ (種別 + 4 成分既定値を明示)。通常は下の種別別ラッパを使う。 */
#define ACS_RPROP(propname, fieldkind, d0, d1, d2, d3)                          \
    ::acs::game::FReflectField{ (propname), (fieldkind), 0u, 0u,                \
        ::acs::game::FIELD_NONE, { static_cast<::acs::f32>(d0),                 \
        static_cast<::acs::f32>(d1), static_cast<::acs::f32>(d2),               \
        static_cast<::acs::f32>(d3) } }

/** F32 スカラのプロパティ。 */
#define ACS_RPROP_F(propname, d0)        ACS_RPROP(propname, ::acs::game::EFieldKind::F32,  d0, 0, 0, 0)
/** I32 整数のプロパティ。 */
#define ACS_RPROP_I(propname, d0)        ACS_RPROP(propname, ::acs::game::EFieldKind::I32,  d0, 0, 0, 0)
/** bool のプロパティ (既定は 1/0)。 */
#define ACS_RPROP_B(propname, d0)        ACS_RPROP(propname, ::acs::game::EFieldKind::Bool, (d0) ? 1 : 0, 0, 0, 0)
/** FVec2 のプロパティ。 */
#define ACS_RPROP_V2(propname, dx, dy)        ACS_RPROP(propname, ::acs::game::EFieldKind::FVec2, dx, dy, 0, 0)
/** FVec3 のプロパティ。 */
#define ACS_RPROP_V3(propname, dx, dy, dz)    ACS_RPROP(propname, ::acs::game::EFieldKind::FVec3, dx, dy, dz, 0)
/** FVec4 のプロパティ (色なら RGBA)。 */
#define ACS_RPROP_V4(propname, dx, dy, dz, dw) ACS_RPROP(propname, ::acs::game::EFieldKind::FVec4, dx, dy, dz, dw)
/** オブジェクト参照のプロパティ (値 = 参照先の安定 ID、既定 -1 = «なし»)。エディタはノードピッカーで編集。 */
#define ACS_RPROP_REF(propname)               ACS_RPROP(propname, ::acs::game::EFieldKind::ObjectRef, -1, 0, 0, 0)
/** オブジェクト参照のフィールド (offset 反射。実メンバ = i32 の参照先 ID。実行時 apply される)。 */
#define ACS_RFIELD_REF(Type, member)          ACS_RFIELD_D(Type, member, ::acs::game::EFieldKind::ObjectRef, -1, 0, 0, 0)
/** 名前のみ反射プロパティ + フラグ (FIELD_READONLY 等。種別 + 4 成分既定値 + flags を明示)。 */
#define ACS_RPROP_FLAGS(propname, fieldkind, flags, d0, d1, d2, d3)              \
    ::acs::game::FReflectField{ (propname), (fieldkind), 0u, 0u,                \
        static_cast<::acs::u32>(flags), { static_cast<::acs::f32>(d0),          \
        static_cast<::acs::f32>(d1), static_cast<::acs::f32>(d2),               \
        static_cast<::acs::f32>(d3) } }

/** 列挙値記述子 (EnumType::val)。 */
#define ACS_EVAL(EnumType, val)                                                 \
    ::acs::game::FEnumValue{ #val, static_cast<::acs::i64>(EnumType::val) }

/**
 * 型を反射 + グローバル自動登録する (グローバル空間に置く)。通常は下の種類別マクロを使う。
 *
 * @param Type 反射する型。
 * @param Category ::acs::game::ETypeCategory の値。
 * @param ... ACS_RFIELD(...) のリスト (0 個でも可)。
 */
#define ACS_REFLECT(Type, Category, ...)                                                          \
    namespace acs::game {                                                                          \
        /* 先頭にセンチネル 1 件を置き、フィールド 0 個でも 0 長配列にならないようにする      */ \
        /* (MSVC は 0 長配列を拒否)。fields は +1、count は -1 で実フィールドのみを公開する。 */ \
        inline const ::acs::game::FReflectField ACS_RCAT(s_acs_rf_, Type)[] = {                \
            ::acs::game::FReflectField{ nullptr, ::acs::game::EFieldKind::Bool, 0u, 0u,            \
                                        ::acs::game::FIELD_NONE }, __VA_ARGS__ };                  \
        inline const ::acs::game::FTypeDesc ACS_RCAT(s_acs_rd_, Type) = {                       \
            #Type, ::acs::game::AcsTypeHash(#Type),                                                 \
            static_cast<::acs::u32>(sizeof(Type)), static_cast<::acs::u32>(alignof(Type)),          \
            (Category), ::acs::game::AcsAutoTraits<Type>(), 0u,                                      \
            ACS_RCAT(s_acs_rf_, Type) + 1,                                                      \
            static_cast<::acs::u32>(sizeof(ACS_RCAT(s_acs_rf_, Type)) /                          \
                                    sizeof(::acs::game::FReflectField) - 1u),                       \
            nullptr, 0u,                                                                             \
            &::acs::game::AcsConstruct<Type>, &::acs::game::AcsDestruct<Type>, nullptr };           \
        inline const ::acs::game::FTypeAutoRegister ACS_RCAT(s_acs_rr_, Type){                  \
            &ACS_RCAT(s_acs_rd_, Type) };                                                       \
        template<> inline const ::acs::game::FTypeDesc* AcsTypeDescOf<Type>() noexcept {            \
            return &ACS_RCAT(s_acs_rd_, Type); }                                                \
    } // namespace acs::game

/**
 * 列挙体を反射 + 登録する (グローバル空間)。
 *
 * @param Type enum 型。
 * @param ... ACS_EVAL(Type, value) のリスト。
 */
#define ACS_REFLECT_ENUM(Type, ...)                                                                \
    namespace acs::game {                                                                          \
        /* 先頭センチネルで 0 長配列を回避 (ACS_REFLECT と同様)。enum_values は +1、count は -1。*/ \
        inline const ::acs::game::FEnumValue ACS_RCAT(s_acs_ev_, Type)[] = {                    \
            ::acs::game::FEnumValue{ nullptr, 0 }, __VA_ARGS__ };                                   \
        inline const ::acs::game::FTypeDesc ACS_RCAT(s_acs_ed_, Type) = {                       \
            #Type, ::acs::game::AcsTypeHash(#Type),                                                 \
            static_cast<::acs::u32>(sizeof(Type)), static_cast<::acs::u32>(alignof(Type)),          \
            ::acs::game::ETypeCategory::Enum,                                                       \
            ::acs::game::TRAIT_SERIALIZABLE | ::acs::game::TRAIT_EDITOR_VISIBLE                      \
                | ::acs::game::TRAIT_SCRIPTABLE, 0u,                                                 \
            nullptr, 0u,                                                                             \
            ACS_RCAT(s_acs_ev_, Type) + 1,                                                      \
            static_cast<::acs::u32>(sizeof(ACS_RCAT(s_acs_ev_, Type)) /                          \
                                    sizeof(::acs::game::FEnumValue) - 1u),                          \
            nullptr, nullptr, nullptr };                                                            \
        inline const ::acs::game::FTypeAutoRegister ACS_RCAT(s_acs_er_, Type){                  \
            &ACS_RCAT(s_acs_ed_, Type) };                                                       \
        template<> inline const ::acs::game::FTypeDesc* AcsTypeDescOf<Type>() noexcept {            \
            return &ACS_RCAT(s_acs_ed_, Type); }                                                \
    } // namespace acs::game

// ----- 種類別の便利マクロ (クラスを「その種類」としてマーク + 反射 + 登録) ------
#define ACS_STRUCT(Type, ...)     ACS_REFLECT(Type, ::acs::game::ETypeCategory::Struct,    __VA_ARGS__)
#define ACS_OBJECT(Type, ...)     ACS_REFLECT(Type, ::acs::game::ETypeCategory::Object,    __VA_ARGS__)
#define ACS_COMPONENT(Type, ...)  ACS_REFLECT(Type, ::acs::game::ETypeCategory::Component, __VA_ARGS__)
#define ACS_NODE(Type, ...)       ACS_REFLECT(Type, ::acs::game::ETypeCategory::Node,      __VA_ARGS__)
#define ACS_SCENE(Type, ...)      ACS_REFLECT(Type, ::acs::game::ETypeCategory::Scene,     __VA_ARGS__)
#define ACS_ASSET_T(Type, ...)    ACS_REFLECT(Type, ::acs::game::ETypeCategory::Asset,     __VA_ARGS__)
#define ACS_SYSTEM(Type, ...)     ACS_REFLECT(Type, ::acs::game::ETypeCategory::System,    __VA_ARGS__)
#define ACS_PREFAB(Type, ...)     ACS_REFLECT(Type, ::acs::game::ETypeCategory::Prefab,    __VA_ARGS__)
#define ACS_INTERFACE(Type, ...)  ACS_REFLECT(Type, ::acs::game::ETypeCategory::Interface, __VA_ARGS__)
#define ACS_EVENT(Type, ...)      ACS_REFLECT(Type, ::acs::game::ETypeCategory::Event,     __VA_ARGS__)
#define ACS_COMMAND(Type, ...)    ACS_REFLECT(Type, ::acs::game::ETypeCategory::Command,   __VA_ARGS__)
#define ACS_SERVICE(Type, ...)    ACS_REFLECT(Type, ::acs::game::ETypeCategory::Service,   __VA_ARGS__)

// =============================================================================
// 集約カタログ用の登録マクロ (型ヘッダ非侵襲)
// -----------------------------------------------------------------------------
// ACS_REFLECT は型ヘッダに置く前提で AcsTypeDescOf<T>() の特殊化も生成するが、
// 既存のエンジン型に「型ヘッダを書き換えずに」リフレクションを後付けしたい場合、
// 専用の 1 つの .cpp (ReflectCatalog.cpp) に型を include して ACS_REGISTER* を
// 並べる。AcsTypeDescOf<T>() の特殊化は生成しない (登録 TU でしか見えない特殊化は
// 別 TU でプライマリ実体化されると ODR 違反になるため)。実行時の FindByName /
// FindById / カテゴリ列挙 / factory は完全に機能する (ここがエンジン横断利用の本筋)。
//
// 静的初期化の確実化: 静的ライブラリ内の自動登録子はリンカに落とされ得るので、
// 利用側 (テスト / エンジン起動 / editor ABI) は AcsRegisterEngineTypes() を 1 度
// 呼んでカタログ TU を確実にリンクへ引き込むこと (ReflectCatalog.h)。
// =============================================================================

/** カタログ用: 型を反射 + 自動登録する (AcsTypeDescOf 特殊化なし)。 */
#define ACS_REGISTER(Type, Category, ...)                                                          \
    namespace acs::game {                                                                          \
        inline const ::acs::game::FReflectField ACS_RCAT(s_acs_gf_, Type)[] = {                \
            ::acs::game::FReflectField{ nullptr, ::acs::game::EFieldKind::Bool, 0u, 0u,            \
                                        ::acs::game::FIELD_NONE }, __VA_ARGS__ };                  \
        inline const ::acs::game::FTypeDesc ACS_RCAT(s_acs_gd_, Type) = {                       \
            #Type, ::acs::game::AcsTypeHash(#Type),                                                 \
            static_cast<::acs::u32>(sizeof(Type)), static_cast<::acs::u32>(alignof(Type)),          \
            (Category), ::acs::game::AcsAutoTraits<Type>(), 0u,                                      \
            ACS_RCAT(s_acs_gf_, Type) + 1,                                                      \
            static_cast<::acs::u32>(sizeof(ACS_RCAT(s_acs_gf_, Type)) /                          \
                                    sizeof(::acs::game::FReflectField) - 1u),                       \
            nullptr, 0u,                                                                             \
            &::acs::game::AcsConstruct<Type>, &::acs::game::AcsDestruct<Type>, nullptr };           \
        inline const ::acs::game::FTypeAutoRegister ACS_RCAT(s_acs_gr_, Type){                  \
            &ACS_RCAT(s_acs_gd_, Type) };                                                       \
    } // namespace acs::game

/** カタログ用: 列挙体を反射 + 自動登録する (AcsTypeDescOf 特殊化なし)。 */
#define ACS_REGISTER_ENUM(Type, ...)                                                               \
    namespace acs::game {                                                                          \
        inline const ::acs::game::FEnumValue ACS_RCAT(s_acs_gv_, Type)[] = {                    \
            ::acs::game::FEnumValue{ nullptr, 0 }, __VA_ARGS__ };                                   \
        inline const ::acs::game::FTypeDesc ACS_RCAT(s_acs_gve_, Type) = {                      \
            #Type, ::acs::game::AcsTypeHash(#Type),                                                 \
            static_cast<::acs::u32>(sizeof(Type)), static_cast<::acs::u32>(alignof(Type)),          \
            ::acs::game::ETypeCategory::Enum,                                                       \
            ::acs::game::TRAIT_SERIALIZABLE | ::acs::game::TRAIT_EDITOR_VISIBLE                      \
                | ::acs::game::TRAIT_SCRIPTABLE, 0u,                                                 \
            nullptr, 0u,                                                                             \
            ACS_RCAT(s_acs_gv_, Type) + 1,                                                      \
            static_cast<::acs::u32>(sizeof(ACS_RCAT(s_acs_gv_, Type)) /                          \
                                    sizeof(::acs::game::FEnumValue) - 1u),                          \
            nullptr, nullptr, nullptr };                                                            \
        inline const ::acs::game::FTypeAutoRegister ACS_RCAT(s_acs_gvr_, Type){                 \
            &ACS_RCAT(s_acs_gve_, Type) };                                                      \
    } // namespace acs::game

// 種類別の集約登録ラッパ (フィールドは可変長、省略可)。
#define ACS_REGISTER_STRUCT(Type, ...)     ACS_REGISTER(Type, ::acs::game::ETypeCategory::Struct,    __VA_ARGS__)
#define ACS_REGISTER_OBJECT(Type, ...)     ACS_REGISTER(Type, ::acs::game::ETypeCategory::Object,    __VA_ARGS__)
#define ACS_REGISTER_COMPONENT(Type, ...)  ACS_REGISTER(Type, ::acs::game::ETypeCategory::Component, __VA_ARGS__)
#define ACS_REGISTER_NODE(Type, ...)       ACS_REGISTER(Type, ::acs::game::ETypeCategory::Node,      __VA_ARGS__)
#define ACS_REGISTER_SCENE(Type, ...)      ACS_REGISTER(Type, ::acs::game::ETypeCategory::Scene,     __VA_ARGS__)
#define ACS_REGISTER_ASSET(Type, ...)      ACS_REGISTER(Type, ::acs::game::ETypeCategory::Asset,     __VA_ARGS__)
#define ACS_REGISTER_SYSTEM(Type, ...)     ACS_REGISTER(Type, ::acs::game::ETypeCategory::System,    __VA_ARGS__)
#define ACS_REGISTER_PREFAB(Type, ...)     ACS_REGISTER(Type, ::acs::game::ETypeCategory::Prefab,    __VA_ARGS__)
#define ACS_REGISTER_INTERFACE(Type, ...)  ACS_REGISTER(Type, ::acs::game::ETypeCategory::Interface, __VA_ARGS__)
#define ACS_REGISTER_EVENT(Type, ...)      ACS_REGISTER(Type, ::acs::game::ETypeCategory::Event,     __VA_ARGS__)
#define ACS_REGISTER_COMMAND(Type, ...)    ACS_REGISTER(Type, ::acs::game::ETypeCategory::Command,   __VA_ARGS__)
#define ACS_REGISTER_SERVICE(Type, ...)    ACS_REGISTER(Type, ::acs::game::ETypeCategory::Service,   __VA_ARGS__)
