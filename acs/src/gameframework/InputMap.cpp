// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar D — FInputMap 実装
#include "gameframework/InputMap.h"
#include "platform/Input.h"

namespace acs::game {

/** キーボードキーの binding を末尾に追加する。 */
void FInputMap::BindKey(ActionId action, EKey key) noexcept {
    Binding b;
    b.action = action;
    b.kind   = BindKind::EKey;
    b.code   = static_cast<u32>(key);
    m_Bindings.PushBack(b);
}

/** マウスボタンの binding を末尾に追加する。 */
void FInputMap::BindMouseButton(ActionId action, EMouseButton mb) noexcept {
    Binding b;
    b.action = action;
    b.kind   = BindKind::EMouseButton;
    b.code   = static_cast<u32>(mb);
    m_Bindings.PushBack(b);
}

/** ゲームパッドボタンの binding を player_index 付きで末尾に追加する。 */
void FInputMap::BindGamepad(ActionId action, EGamepadButton gb, u32 player_index) noexcept {
    Binding b;
    b.action = action;
    b.kind   = BindKind::EGamepadButton;
    b.code   = static_cast<u32>(gb);
    b.player = player_index;
    m_Bindings.PushBack(b);
}

/** neg/pos キーのペアを 1D axis binding として末尾に追加する。 */
void FInputMap::BindAxisKeys(ActionId action, EKey neg, EKey pos) noexcept {
    Binding b;
    b.action   = action;
    b.kind     = BindKind::Axis1D;
    b.code     = static_cast<u32>(neg);
    b.code_pos = static_cast<u32>(pos);
    m_Bindings.PushBack(b);
}

/** 指定アクションの binding を in-place の compaction で全削除する。 */
void FInputMap::Unbind(ActionId action) noexcept {
    u32 w = 0;
    for (u32 r = 0; r < m_Bindings.Size(); ++r) {
        if (m_Bindings[r].action != action) {
            if (w != r) m_Bindings[w] = m_Bindings[r];
            ++w;
        }
    }
    while (m_Bindings.Size() > w) m_Bindings.PopBack();
}

/** 全 binding を破棄する。 */
void FInputMap::ClearAll() noexcept {
    m_Bindings.Clear();
}

/** 該当アクションの各 binding を走査し、いずれかがこのフレームで押されたか判定する。 */
bool FInputMap::IsPressed(ActionId action) const noexcept {
    for (u32 i = 0; i < m_Bindings.Size(); ++i) {
        const Binding& b = m_Bindings[i];
        if (b.action != action) continue;
        switch (b.kind) {
        case BindKind::EKey:
            if (Input::IsKeyPressed(static_cast<EKey>(b.code))) return true;
            break;
        case BindKind::EMouseButton:
            if (Input::IsMouseButtonPressed(static_cast<EMouseButton>(b.code))) return true;
            break;
        case BindKind::EGamepadButton:
            if (Input::IsGamepadButtonPressed(b.player, static_cast<EGamepadButton>(b.code))) return true;
            break;
        case BindKind::Axis1D:
            // axis は Pressed の概念なし (常に false)
            break;
        }
    }
    return false;
}

/** 該当アクションの各 binding を走査し、いずれかが押下中か判定する (axis は |値|>0 で Held)。 */
bool FInputMap::IsHeld(ActionId action) const noexcept {
    for (u32 i = 0; i < m_Bindings.Size(); ++i) {
        const Binding& b = m_Bindings[i];
        if (b.action != action) continue;
        switch (b.kind) {
        case BindKind::EKey:
            if (Input::IsKeyDown(static_cast<EKey>(b.code))) return true;
            break;
        case BindKind::EMouseButton:
            if (Input::IsMouseButtonDown(static_cast<EMouseButton>(b.code))) return true;
            break;
        case BindKind::EGamepadButton:
            if (Input::IsGamepadButtonDown(b.player, static_cast<EGamepadButton>(b.code))) return true;
            break;
        case BindKind::Axis1D:
            // axis は |Axis| > 0 で Held とみなす
            if (Input::IsKeyDown(static_cast<EKey>(b.code)) ||
                Input::IsKeyDown(static_cast<EKey>(b.code_pos))) return true;
            break;
        }
    }
    return false;
}

/** 該当アクションの各 binding を走査し、いずれかがこのフレームで離されたか判定する。 */
bool FInputMap::IsReleased(ActionId action) const noexcept {
    for (u32 i = 0; i < m_Bindings.Size(); ++i) {
        const Binding& b = m_Bindings[i];
        if (b.action != action) continue;
        switch (b.kind) {
        case BindKind::EKey:
            if (Input::IsKeyReleased(static_cast<EKey>(b.code))) return true;
            break;
        case BindKind::EMouseButton:
            if (Input::IsMouseButtonReleased(static_cast<EMouseButton>(b.code))) return true;
            break;
        case BindKind::EGamepadButton:
            // GamepadButtonReleased が無いので Released は未対応 (always false)
            break;
        case BindKind::Axis1D:
            break;
        }
    }
    return false;
}

/** 該当アクションの axis binding を累積し、clamp(-1, +1) した値を返す。 */
f32 FInputMap::Axis(ActionId action) const noexcept {
    f32 acc = 0.0f;
    for (u32 i = 0; i < m_Bindings.Size(); ++i) {
        const Binding& b = m_Bindings[i];
        if (b.action != action) continue;
        if (b.kind != BindKind::Axis1D) continue;
        const bool n = Input::IsKeyDown(static_cast<EKey>(b.code));
        const bool p = Input::IsKeyDown(static_cast<EKey>(b.code_pos));
        // 両方押下は 0 (相殺)、片方なら ±1
        if (n && !p) acc -= 1.0f;
        else if (p && !n) acc += 1.0f;
    }
    if (acc >  1.0f) acc =  1.0f;
    if (acc < -1.0f) acc = -1.0f;
    return acc;
}

} // namespace acs::game
