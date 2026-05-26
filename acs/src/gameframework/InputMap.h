// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar D — FInputMap (Phase 6)
//
// 物理入力 (EKey/EMouseButton/EGamepadButton) を「名前付きアクション」に束ねる
// マッピング層。ゲームロジックが物理キーから疎結合になり、キーコンフィグ UI も
// 後付けで書けるようになる。
//
// 使い方:
//   FInputMap im;
//   im.BindKey         (FActionId("Jump"),  EKey::Space);
//   im.BindGamepad     (FActionId("Jump"),  EGamepadButton::A);
//   im.BindAxisKeys    (FActionId("MoveX"), EKey::A, EKey::D);
//
//   if (im.IsPressed(FActionId("Jump"))) DoJump();
//   f32 mv_x = im.Axis(FActionId("MoveX"));  // -1, 0, +1
//
// 設計選択 (Phase 6 = Pillar D Phase 1):
//   ・**compile-time hash**: FActionId は `constexpr` FNV-1a で生成、`FActionId("name")`
//     は配置で完結 (実行時 string compare なし)。衝突は 32bit hash で実用上無視。
//   ・**複数 bind OR セマンティクス**: 1 アクションに複数の物理入力を bind 可能。
//     1 つでも該当すれば Pressed/Held/Released は true。
//   ・**1D axis**: neg/pos キー 2 つで -1/+1 を返す。両方押下は 0 (相殺)。複数の
//     axis binding は累積 + clamp(-1, +1) (例: AD + LStick で同方向に重ねられる)。
//   ・**poll-based**: 状態取得時に `acs::FInput::*` を呼ぶ。アクション側に状態は持たない。
//
// 範囲外 (Phase 2+ で):
//   ・analog axis (gamepad stick の生 f32 値、現状はキー → -1/+1 のみ)
//   ・player_index 完全対応 (Phase 1 は gamepad bind 時のみ受ける、digital は 0 固定)
//   ・FSettings (`FStorage`) への永続化
//   ・input context スタック (gameplay/menu/dialogue でバインド集を push/pop)
//   ・event 配送 (現状は OnUpdate からの polling 前提)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "platform/InputCodes.h"

namespace acs::game {

// Compile-time FNV-1a hash (32bit)
constexpr u32 ActionHash(const char* s) noexcept {
    u32 h = 2166136261u;
    while (*s != '\0') {
        h ^= static_cast<u32>(static_cast<unsigned char>(*s));
        h *= 16777619u;
        ++s;
    }
    return h;
}

// アクション識別子。文字列リテラルから constexpr で生成、内部は u32。
struct FActionId {
    u32 value = 0;

    constexpr FActionId() noexcept = default;
    constexpr explicit FActionId(u32 v) noexcept : value(v) {}
    constexpr FActionId(const char* name) noexcept : value(ActionHash(name)) {}

    constexpr bool operator==(FActionId o) const noexcept { return value == o.value; }
    constexpr bool operator!=(FActionId o) const noexcept { return value != o.value; }
};

class FInputMap {
public:
    FInputMap() noexcept = default;
    ~FInputMap() noexcept = default;

    FInputMap(const FInputMap&)            = delete;
    FInputMap& operator=(const FInputMap&) = delete;

    // ----- bind -----
    void BindKey         (FActionId action, EKey key) noexcept;
    void BindMouseButton (FActionId action, EMouseButton mb) noexcept;
    void BindGamepad     (FActionId action, EGamepadButton gb, u32 player_index = 0) noexcept;
    void BindAxisKeys    (FActionId action, EKey neg, EKey pos) noexcept;

    // 指定 action の全 binding を削除
    void Unbind  (FActionId action) noexcept;
    void ClearAll() noexcept;

    // ----- query (FInput::* を内部で polling) -----
    bool IsPressed (FActionId action) const noexcept;     // このフレームで押された
    bool IsHeld    (FActionId action) const noexcept;     // 押されている
    bool IsReleased(FActionId action) const noexcept;     // このフレームで離された
    f32  Axis      (FActionId action) const noexcept;     // 1D axis、累積 + clamp(-1,+1)

private:
    enum class BindKind : u8 {
        EKey,
        EMouseButton,
        EGamepadButton,
        Axis1D,
    };

    struct FBinding {
        FActionId action;
        BindKind kind;
        u32      code      = 0;     // EKey/EMouseButton/EGamepadButton enum value (= code_neg for Axis)
        u32      code_pos  = 0;     // Axis 専用 (pos 方向の EKey)
        u32      player    = 0;     // Gamepad 専用
    };

    TArray<FBinding> _bindings;
};

} // namespace acs::game
