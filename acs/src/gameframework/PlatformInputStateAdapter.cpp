// SPDX-License-Identifier: Apache-2.0
#include "gameframework/PlatformInputStateAdapter.h"

#include "platform/Input.h"

namespace acs::game {

/** 現在のCInputを検証済みsnapshotとして一括取得する。 */
bool FPlatformInputStateAdapter::TryCapture(FInputStateSnapshot& output) noexcept
{
    /** 全入力の検証が完了するまで公開しない一時snapshot。 */
    FInputStateSnapshot staged;
    for (usize key_index = static_cast<usize>(EKey::Unknown) + 1u; key_index < static_cast<usize>(EKey::_Count); ++key_index) {
        /** 現在取得するキー。 */
        const EKey key = static_cast<EKey>(key_index);
        if (!staged.TrySetKeyState(key, CInput::IsKeyDown(key), CInput::IsKeyPressed(key), CInput::IsKeyReleased(key))) return false;
    }
    for (usize button_index = 0; button_index < static_cast<usize>(EMouseButton::_Count); ++button_index) {
        /** 現在取得するマウスボタン。 */
        const EMouseButton button = static_cast<EMouseButton>(button_index);
        if (!staged.TrySetMouseButtonState(button, CInput::IsMouseButtonDown(button), CInput::IsMouseButtonPressed(button), CInput::IsMouseButtonReleased(button))) return false;
    }
    for (u32 player_index = 0; player_index < 4u; ++player_index) {
        for (usize button_index = 0; button_index < static_cast<usize>(EGamepadButton::_Count); ++button_index) {
            /** 現在取得するゲームパッドボタン。 */
            const EGamepadButton button = static_cast<EGamepadButton>(button_index);
            if (!staged.TrySetGamepadButtonState(player_index, button, CInput::IsGamepadButtonDown(player_index, button), CInput::IsGamepadButtonPressed(player_index, button), CInput::IsGamepadButtonReleased(player_index, button))) return false;
        }
        for (usize axis_index = 0; axis_index < static_cast<usize>(EGamepadAxis::_Count); ++axis_index) {
            /** 現在取得するゲームパッド軸。 */
            const EGamepadAxis axis = static_cast<EGamepadAxis>(axis_index);
            if (!staged.TrySetGamepadAxis(player_index, axis, CInput::GamepadAxisValue(player_index, axis))) return false;
        }
    }
    output = staged;
    return true;
}

} // namespace acs::game
