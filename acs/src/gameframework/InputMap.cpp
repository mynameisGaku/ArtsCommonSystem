// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar D — FInputMap 実装
#include "gameframework/InputMap.h"
#include "platform/Input.h"

namespace acs::game {

/** キーボードキーの binding を末尾に追加する。 */
void FInputMap::BindKey(FActionId action, EKey key) noexcept {
    FBinding b;
    b.action = action;
    b.kind   = EBindKind::Key;
    b.code   = static_cast<u32>(key);
    m_Bindings.PushBack(b);
}

/** マウスボタンの binding を末尾に追加する。 */
void FInputMap::BindMouseButton(FActionId action, EMouseButton mb) noexcept {
    FBinding b;
    b.action = action;
    b.kind   = EBindKind::MouseButton;
    b.code   = static_cast<u32>(mb);
    m_Bindings.PushBack(b);
}

/** ゲームパッドボタンの binding を player_index 付きで末尾に追加する。 */
void FInputMap::BindGamepad(FActionId action, EGamepadButton gb, u32 player_index) noexcept {
    FBinding b;
    b.action = action;
    b.kind   = EBindKind::GamepadButton;
    b.code   = static_cast<u32>(gb);
    b.player = player_index;
    m_Bindings.PushBack(b);
}

/** neg/pos キーのペアを 1D axis binding として末尾に追加する。 */
void FInputMap::BindAxisKeys(FActionId action, EKey neg, EKey pos) noexcept {
    FBinding b;
    b.action   = action;
    b.kind     = EBindKind::Axis1D;
    b.code     = static_cast<u32>(neg);
    b.code_pos = static_cast<u32>(pos);
    m_Bindings.PushBack(b);
}

/** ゲームパッドのアナログ軸 binding を倍率付きで末尾へ追加する。 */
void FInputMap::BindGamepadAxis(FActionId action, EGamepadAxis axis, u32 player_index, f32 scale) noexcept
{
    FBinding b;
    b.action = action;
    b.kind = EBindKind::GamepadAxis;
    b.code = static_cast<u32>(axis);
    b.player = player_index;
    b.scale = scale;
    m_Bindings.PushBack(b);
}

/** 指定アクションの binding を in-place の compaction で全削除する。 */
void FInputMap::Unbind(FActionId action) noexcept {
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
bool FInputMap::IsPressed(FActionId action) const noexcept {
    for (u32 i = 0; i < m_Bindings.Size(); ++i) {
        const FBinding& b = m_Bindings[i];
        if (b.action != action) continue;
        switch (b.kind) {
        case EBindKind::Key:
            if (CInput::IsKeyPressed(static_cast<EKey>(b.code))) return true;
            break;
        case EBindKind::MouseButton:
            if (CInput::IsMouseButtonPressed(static_cast<EMouseButton>(b.code))) return true;
            break;
        case EBindKind::GamepadButton:
            if (CInput::IsGamepadButtonPressed(b.player, static_cast<EGamepadButton>(b.code))) return true;
            break;
        case EBindKind::GamepadAxis:
            break;
        case EBindKind::Axis1D:
            // axis は Pressed の概念なし (常に false)
            break;
        }
    }
    return false;
}

/** 該当アクションの各 binding を走査し、いずれかが押下中か判定する (axis は |値|>0 で Held)。 */
bool FInputMap::IsHeld(FActionId action) const noexcept {
    for (u32 i = 0; i < m_Bindings.Size(); ++i) {
        const FBinding& b = m_Bindings[i];
        if (b.action != action) continue;
        switch (b.kind) {
        case EBindKind::Key:
            if (CInput::IsKeyDown(static_cast<EKey>(b.code))) return true;
            break;
        case EBindKind::MouseButton:
            if (CInput::IsMouseButtonDown(static_cast<EMouseButton>(b.code))) return true;
            break;
        case EBindKind::GamepadButton:
            if (CInput::IsGamepadButtonDown(b.player, static_cast<EGamepadButton>(b.code))) return true;
            break;
        case EBindKind::GamepadAxis: {
            const f32 value = CInput::GamepadAxisValue(b.player, static_cast<EGamepadAxis>(b.code)) * b.scale;
            if (value < -0.0001f || value > 0.0001f) return true;
            break;
        }
        case EBindKind::Axis1D:
            // axis は |Axis| > 0 で Held とみなす
            if (CInput::IsKeyDown(static_cast<EKey>(b.code)) ||
                CInput::IsKeyDown(static_cast<EKey>(b.code_pos))) return true;
            break;
        }
    }
    return false;
}

/** 該当アクションの各 binding を走査し、いずれかがこのフレームで離されたか判定する。 */
bool FInputMap::IsReleased(FActionId action) const noexcept {
    for (u32 i = 0; i < m_Bindings.Size(); ++i) {
        const FBinding& b = m_Bindings[i];
        if (b.action != action) continue;
        switch (b.kind) {
        case EBindKind::Key:
            if (CInput::IsKeyReleased(static_cast<EKey>(b.code))) return true;
            break;
        case EBindKind::MouseButton:
            if (CInput::IsMouseButtonReleased(static_cast<EMouseButton>(b.code))) return true;
            break;
        case EBindKind::GamepadButton:
            if (CInput::IsGamepadButtonReleased(b.player, static_cast<EGamepadButton>(b.code))) return true;
            break;
        case EBindKind::GamepadAxis:
            break;
        case EBindKind::Axis1D:
            break;
        }
    }
    return false;
}

/** 該当アクションの axis binding を累積し、clamp(-1, +1) した値を返す。 */
f32 FInputMap::Axis(FActionId action) const noexcept {
    f32 acc = 0.0f;
    for (u32 i = 0; i < m_Bindings.Size(); ++i) {
        const FBinding& b = m_Bindings[i];
        if (b.action != action) continue;
        if (b.kind == EBindKind::Axis1D) {
            const bool n = CInput::IsKeyDown(static_cast<EKey>(b.code));
            const bool p = CInput::IsKeyDown(static_cast<EKey>(b.code_pos));
            // 両方押下は 0 (相殺)、片方なら ±1
            if (n && !p)
                acc -= 1.0f;
            else if (p && !n)
                acc += 1.0f;
        } else if (b.kind == EBindKind::GamepadAxis) {
            acc += CInput::GamepadAxisValue(b.player, static_cast<EGamepadAxis>(b.code)) * b.scale;
        }
    }
    if (acc >  1.0f) acc =  1.0f;
    if (acc < -1.0f) acc = -1.0f;
    return acc;
}

/** 既存axis値へ明示された入力補正を適用する。 */
f32 FInputMap::AxisValue(FActionId action, FInputAxisOptions options) const noexcept
{
    return options.Apply(Axis(action));
}

} // namespace acs::game
