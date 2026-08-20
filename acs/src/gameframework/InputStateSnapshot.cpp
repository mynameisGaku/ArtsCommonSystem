// SPDX-License-Identifier: Apache-2.0
#include "gameframework/InputStateSnapshot.h"

#include <cmath>

namespace acs::game {
namespace {

/** 有効なキーの配列位置を返す。Unknownと範囲外は拒否する。 */
bool TryKeyIndex(EKey key, usize& out_index) noexcept
{
    /** 検証するキーの配列位置。 */
    const usize index = static_cast<usize>(key);
    if (index <= static_cast<usize>(EKey::Unknown) || index >= static_cast<usize>(EKey::_Count)) return false;
    out_index = index;
    return true;
}

/** 有効なマウスボタンの配列位置を返す。 */
bool TryMouseButtonIndex(EMouseButton button, usize& out_index) noexcept
{
    /** 検証するマウスボタンの配列位置。 */
    const usize index = static_cast<usize>(button);
    if (index >= static_cast<usize>(EMouseButton::_Count)) return false;
    out_index = index;
    return true;
}

/** 有効なゲームパッドボタンの配列位置を返す。 */
bool TryGamepadButtonIndex(EGamepadButton button, usize& out_index) noexcept
{
    /** 検証するゲームパッドボタンの配列位置。 */
    const usize index = static_cast<usize>(button);
    if (index >= static_cast<usize>(EGamepadButton::_Count)) return false;
    out_index = index;
    return true;
}

/** 有効なゲームパッド軸の配列位置を返す。 */
bool TryGamepadAxisIndex(EGamepadAxis axis, usize& out_index) noexcept
{
    /** 検証するゲームパッド軸の配列位置。 */
    const usize index = static_cast<usize>(axis);
    if (index >= static_cast<usize>(EGamepadAxis::_Count)) return false;
    out_index = index;
    return true;
}

} // namespace

void FInputStateSnapshot::Clear() noexcept
{
    for (usize index = 0; index < static_cast<usize>(EKey::_Count); ++index) {
        m_KeysDown[index] = false;
        m_KeysPressed[index] = false;
        m_KeysReleased[index] = false;
    }
    for (usize index = 0; index < static_cast<usize>(EMouseButton::_Count); ++index) {
        m_MouseButtonsDown[index] = false;
        m_MouseButtonsPressed[index] = false;
        m_MouseButtonsReleased[index] = false;
    }
    for (usize player_index = 0; player_index < kGamepadPlayerCount; ++player_index) {
        for (usize button_index = 0; button_index < static_cast<usize>(EGamepadButton::_Count); ++button_index) {
            m_GamepadButtonsDown[player_index][button_index] = false;
            m_GamepadButtonsPressed[player_index][button_index] = false;
            m_GamepadButtonsReleased[player_index][button_index] = false;
        }
        for (usize axis_index = 0; axis_index < static_cast<usize>(EGamepadAxis::_Count); ++axis_index) {
            m_GamepadAxes[player_index][axis_index] = 0.0f;
        }
    }
}

bool FInputStateSnapshot::TrySetKeyState(EKey key, bool down, bool pressed, bool released) noexcept
{
    usize index = 0;
    if (!TryKeyIndex(key, index)) return false;
    m_KeysDown[index] = down;
    m_KeysPressed[index] = pressed;
    m_KeysReleased[index] = released;
    return true;
}

bool FInputStateSnapshot::TrySetMouseButtonState(EMouseButton button, bool down, bool pressed, bool released) noexcept
{
    usize index = 0;
    if (!TryMouseButtonIndex(button, index)) return false;
    m_MouseButtonsDown[index] = down;
    m_MouseButtonsPressed[index] = pressed;
    m_MouseButtonsReleased[index] = released;
    return true;
}

bool FInputStateSnapshot::TrySetGamepadButtonState(u32 player_index, EGamepadButton button, bool down, bool pressed,
                                                   bool released) noexcept
{
    usize button_index = 0;
    if (player_index >= kGamepadPlayerCount || !TryGamepadButtonIndex(button, button_index)) return false;
    m_GamepadButtonsDown[player_index][button_index] = down;
    m_GamepadButtonsPressed[player_index][button_index] = pressed;
    m_GamepadButtonsReleased[player_index][button_index] = released;
    return true;
}

bool FInputStateSnapshot::TrySetGamepadAxis(u32 player_index, EGamepadAxis axis, f32 value) noexcept
{
    usize axis_index = 0;
    if (player_index >= kGamepadPlayerCount || !TryGamepadAxisIndex(axis, axis_index) || !std::isfinite(value))
        return false;
    /** トリガーにだけ適用する片方向の値域判定。 */
    const bool is_trigger = axis == EGamepadAxis::LeftTrigger || axis == EGamepadAxis::RightTrigger;
    if ((is_trigger && (value < 0.0f || value > 1.0f)) || (!is_trigger && (value < -1.0f || value > 1.0f)))
        return false;
    m_GamepadAxes[player_index][axis_index] = value;
    return true;
}

bool FInputStateSnapshot::IsKeyDown(EKey key) const noexcept
{
    usize index = 0;
    return TryKeyIndex(key, index) && m_KeysDown[index];
}

bool FInputStateSnapshot::IsKeyPressed(EKey key) const noexcept
{
    usize index = 0;
    return TryKeyIndex(key, index) && m_KeysPressed[index];
}

bool FInputStateSnapshot::IsKeyReleased(EKey key) const noexcept
{
    usize index = 0;
    return TryKeyIndex(key, index) && m_KeysReleased[index];
}

bool FInputStateSnapshot::IsMouseButtonDown(EMouseButton button) const noexcept
{
    usize index = 0;
    return TryMouseButtonIndex(button, index) && m_MouseButtonsDown[index];
}

bool FInputStateSnapshot::IsMouseButtonPressed(EMouseButton button) const noexcept
{
    usize index = 0;
    return TryMouseButtonIndex(button, index) && m_MouseButtonsPressed[index];
}

bool FInputStateSnapshot::IsMouseButtonReleased(EMouseButton button) const noexcept
{
    usize index = 0;
    return TryMouseButtonIndex(button, index) && m_MouseButtonsReleased[index];
}

bool FInputStateSnapshot::IsGamepadButtonDown(u32 player_index, EGamepadButton button) const noexcept
{
    usize button_index = 0;
    return player_index < kGamepadPlayerCount && TryGamepadButtonIndex(button, button_index) &&
           m_GamepadButtonsDown[player_index][button_index];
}

bool FInputStateSnapshot::IsGamepadButtonPressed(u32 player_index, EGamepadButton button) const noexcept
{
    usize button_index = 0;
    return player_index < kGamepadPlayerCount && TryGamepadButtonIndex(button, button_index) &&
           m_GamepadButtonsPressed[player_index][button_index];
}

bool FInputStateSnapshot::IsGamepadButtonReleased(u32 player_index, EGamepadButton button) const noexcept
{
    usize button_index = 0;
    return player_index < kGamepadPlayerCount && TryGamepadButtonIndex(button, button_index) &&
           m_GamepadButtonsReleased[player_index][button_index];
}

f32 FInputStateSnapshot::GamepadAxisValue(u32 player_index, EGamepadAxis axis) const noexcept
{
    usize axis_index = 0;
    return player_index < kGamepadPlayerCount && TryGamepadAxisIndex(axis, axis_index)
               ? m_GamepadAxes[player_index][axis_index]
               : 0.0f;
}

} // namespace acs::game
