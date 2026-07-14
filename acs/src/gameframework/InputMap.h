// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar D — FInputMap
//
// 物理入力 (EKey/EMouseButton/EGamepadButton) を「名前付きアクション」に束ねる
// マッピング層。ゲームロジックが物理キーから疎結合になり、キーコンフィグ UI も
// 後付けで書けるようになる。
//
// 使い方:
//   FInputMap im;
//   im.BindKey         (ActionId("Jump"),  EKey::Space);
//   im.BindGamepad     (ActionId("Jump"),  EGamepadButton::A);
//   im.BindAxisKeys    (ActionId("MoveX"), EKey::A, EKey::D);
//
//   if (im.IsPressed(ActionId("Jump"))) DoJump();
//   f32 mv_x = im.Axis(ActionId("MoveX"));  // -1, 0, +1
//
// 設計選択 (Pillar D):
//   ・**compile-time hash**: ActionId は `constexpr` FNV-1a で生成、`ActionId("name")`
//     は配置で完結 (実行時 string compare なし)。衝突は 32bit hash で実用上無視。
//   ・**複数 bind OR セマンティクス**: 1 アクションに複数の物理入力を bind 可能。
//     1 つでも該当すれば Pressed/Held/Released は true。
//   ・**1D axis**: neg/pos キーまたはゲームパッド軸を束ねる。複数の axis binding は
//     累積 + clamp(-1, +1) (例: AD + LStick で同方向に重ねられる)。
//   ・**poll-based**: 状態取得時に `acs::Input::*` を呼ぶ。アクション側に状態は持たない。
//
// 範囲外:
//   ・player_index 完全対応 (現状は gamepad bind 時のみ受ける、digital は 0 固定)
//   ・FSettings (`Storage`) への永続化
//   ・input context スタック (gameplay/menu/dialogue でバインド集を push/pop)
//   ・event 配送 (現状は OnUpdate からの polling 前提)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "platform/InputCodes.h"

namespace acs::game {

/**
 * 文字列から 32bit の FNV-1a ハッシュを compile-time に計算する。
 *
 * @details null 終端まで 1 文字ずつ XOR + 素数乗算する標準的な FNV-1a。
 * @param s ハッシュ対象の null 終端文字列。
 * @return 計算した 32bit ハッシュ値。
 */
constexpr u32 ActionHash(const char* s) noexcept {
    u32 h = 2166136261u;
    while (*s != '\0') {
        h ^= static_cast<u32>(static_cast<unsigned char>(*s));
        h *= 16777619u;
        ++s;
    }
    return h;
}

/**
 * アクション識別子。
 *
 * @details 文字列リテラルから constexpr で生成され、内部は ActionHash の u32 値で持つ。
 */
struct ActionId {
    /** ActionHash で計算したアクションのハッシュ値。 */
    u32 value = 0;

    /** invalid (value=0) な識別子を構築する。 */
    constexpr ActionId() noexcept = default;

    /**
     * 既存のハッシュ値から識別子を構築する。
     *
     * @param v ハッシュ値。
     */
    constexpr explicit ActionId(u32 v) noexcept : value(v) {}

    /**
     * 名前文字列から識別子を構築する (compile-time ハッシュ)。
     *
     * @param name アクション名の null 終端文字列。
     */
    constexpr ActionId(const char* name) noexcept : value(ActionHash(name)) {}

    /**
     * 等価比較する。
     *
     * @param o 比較相手。
     * @return ハッシュ値が一致すれば true。
     */
    constexpr bool operator==(ActionId o) const noexcept { return value == o.value; }

    /**
     * 非等価比較する。
     *
     * @param o 比較相手。
     * @return ハッシュ値が異なれば true。
     */
    constexpr bool operator!=(ActionId o) const noexcept { return value != o.value; }
};

/**
 * 物理入力を名前付きアクションに束ねるマッピング層。
 *
 * @details
 * 1 アクションに複数の物理入力を bind でき、いずれか 1 つでも該当すれば
 * Pressed/Held/Released が true になる (OR セマンティクス)。状態は持たず、query
 * 時に acs::Input::* を poll する。
 */
class FInputMap {
public:
    /** 空状態で構築する (binding は未登録)。 */
    FInputMap() noexcept = default;

    /** 破棄する。 */
    ~FInputMap() noexcept = default;

    /** コピー禁止。 */
    FInputMap(const FInputMap&)            = delete;

    /** コピー代入も禁止。 */
    FInputMap& operator=(const FInputMap&) = delete;

    /**
     * アクションにキーボードキーを bind する。
     *
     * @param action 対象アクション。
     * @param key bind する物理キー。
     */
    void BindKey         (ActionId action, EKey key) noexcept;

    /**
     * アクションにマウスボタンを bind する。
     *
     * @param action 対象アクション。
     * @param mb bind するマウスボタン。
     */
    void BindMouseButton (ActionId action, EMouseButton mb) noexcept;

    /**
     * アクションにゲームパッドボタンを bind する。
     *
     * @param action 対象アクション。
     * @param gb bind するゲームパッドボタン。
     * @param player_index 対象プレイヤー番号 (既定 0)。
     */
    void BindGamepad     (ActionId action, EGamepadButton gb, u32 player_index = 0) noexcept;

    /**
     * アクションに 1D axis (neg/pos キーのペア) を bind する。
     *
     * @param action 対象アクション。
     * @param neg -1 方向のキー。
     * @param pos +1 方向のキー。
     */
    void BindAxisKeys    (ActionId action, EKey neg, EKey pos) noexcept;

    /**
     * アクションにゲームパッドのアナログ軸を bind する。
     *
     * @param action 対象アクション。
     * @param axis bind する物理軸。
     * @param player_index 対象プレイヤー番号 (既定 0)。
     * @param scale 値へ乗算する倍率。負値で軸を反転できる。
     */
    void BindGamepadAxis(ActionId action, EGamepadAxis axis, u32 player_index = 0, f32 scale = 1.0f) noexcept;

    /**
     * 指定アクションの全 binding を削除する。
     *
     * @param action binding を削除するアクション。
     */
    void Unbind  (ActionId action) noexcept;

    /** 全アクションの全 binding を削除する。 */
    void ClearAll() noexcept;

    /**
     * このフレームで押されたかを返す (Input::* を内部で poll)。
     *
     * @param action 判定するアクション。
     * @return bind 済み入力のいずれかがこのフレームで押されたら true。
     */
    bool IsPressed (ActionId action) const noexcept;

    /**
     * 押されているかを返す (Input::* を内部で poll)。
     *
     * @param action 判定するアクション。
     * @return bind 済み入力のいずれかが押下中なら true。
     */
    bool IsHeld    (ActionId action) const noexcept;

    /**
     * このフレームで離されたかを返す (Input::* を内部で poll)。
     *
     * @details キーボード、マウス、ゲームパッドのデジタル入力を対象とする。
     * @param action 判定するアクション。
     * @return bind 済み入力のいずれかがこのフレームで離されたら true。
     */
    bool IsReleased(ActionId action) const noexcept;

    /**
     * 1D axis 値を返す (Input::* を内部で poll)。
     *
     * @details 全 axis binding を累積して clamp(-1, +1)。両方押下は相殺で 0。
     * @param action 判定するアクション。
     * @return [-1, +1] の axis 値。
     */
    f32  Axis      (ActionId action) const noexcept;

private:
    /** binding の種別 (物理入力の種類を判別する)。 */
    enum class BindKind : u8 {
        /** キーボードキー。 */
        EKey,

        /** マウスボタン。 */
        EMouseButton,

        /** ゲームパッドボタン。 */
        EGamepadButton,

        /** ゲームパッドのアナログ軸。 */
        EGamepadAxis,

        /** 1D axis (neg/pos キーのペア)。 */
        Axis1D,
    };

    /** 1 件の binding (アクションと物理入力の対応)。 */
    struct Binding {
        /** この binding が属するアクション。 */
        ActionId action;

        /** この binding の種別。 */
        BindKind kind;

        /** EKey/EMouseButton/EGamepadButton の enum 値 (Axis では neg 方向のキー)。 */
        u32      code      = 0;

        /** Axis 専用: pos 方向の EKey。 */
        u32      code_pos  = 0;

        /** Gamepad 専用: 対象プレイヤー番号。 */
        u32      player    = 0;

        /** アナログ軸へ乗算する倍率。 */
        f32 scale = 1.0f;
    };

    /** 登録済み binding の配列。 */
    TArray<Binding> m_Bindings;
};

} // namespace acs::game
