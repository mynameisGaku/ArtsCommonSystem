// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/InputStateView.h"

namespace acs::game {

/**
 * 一入力時点のキー、マウスボタン、ゲームパッド状態を所有する値。
 *
 * 同じsnapshotとFInputMapから同じFInputActionStateを再現する。setterは範囲外の
 * 列挙値、プレイヤー番号、非有限または正規範囲外の軸値を拒否し、既存状態を保つ。
 */
class FInputStateSnapshot final : public IInputStateView {
public:
    /** 全入力を離された初期状態へ戻す。 */
    void Clear() noexcept;

    /** キーの保持状態と今回の変化を設定し、範囲外なら拒否する。 */
    bool TrySetKeyState(EKey key, bool down, bool pressed, bool released) noexcept;

    /** マウスボタンの保持状態と今回の変化を設定し、範囲外なら拒否する。 */
    bool TrySetMouseButtonState(EMouseButton button, bool down, bool pressed, bool released) noexcept;

    /** ゲームパッドボタンの保持状態と今回の変化を設定し、範囲外なら拒否する。 */
    bool TrySetGamepadButtonState(u32 player_index, EGamepadButton button, bool down, bool pressed,
                                  bool released) noexcept;

    /** ゲームパッド軸の正規化済み値を設定し、不正値なら拒否する。 */
    bool TrySetGamepadAxis(u32 player_index, EGamepadAxis axis, f32 value) noexcept;

    /** 指定キーが現在押されているか返す。 */
    bool IsKeyDown(EKey key) const noexcept override;

    /** 指定キーが今回押されたか返す。 */
    bool IsKeyPressed(EKey key) const noexcept override;

    /** 指定キーが今回離されたか返す。 */
    bool IsKeyReleased(EKey key) const noexcept override;

    /** 指定マウスボタンが現在押されているか返す。 */
    bool IsMouseButtonDown(EMouseButton button) const noexcept override;

    /** 指定マウスボタンが今回押されたか返す。 */
    bool IsMouseButtonPressed(EMouseButton button) const noexcept override;

    /** 指定マウスボタンが今回離されたか返す。 */
    bool IsMouseButtonReleased(EMouseButton button) const noexcept override;

    /** 指定プレイヤーのゲームパッドボタンが現在押されているか返す。 */
    bool IsGamepadButtonDown(u32 player_index, EGamepadButton button) const noexcept override;

    /** 指定プレイヤーのゲームパッドボタンが今回押されたか返す。 */
    bool IsGamepadButtonPressed(u32 player_index, EGamepadButton button) const noexcept override;

    /** 指定プレイヤーのゲームパッドボタンが今回離されたか返す。 */
    bool IsGamepadButtonReleased(u32 player_index, EGamepadButton button) const noexcept override;

    /** 指定プレイヤーのゲームパッド軸値を返す。 */
    f32 GamepadAxisValue(u32 player_index, EGamepadAxis axis) const noexcept override;

private:
    /** 対応するゲームパッドプレイヤー数。 */
    static constexpr usize kGamepadPlayerCount = 4u;

    /** キーの現在押下状態。 */
    bool m_KeysDown[static_cast<usize>(EKey::_Count)]{};

    /** キーの今回押下状態。 */
    bool m_KeysPressed[static_cast<usize>(EKey::_Count)]{};

    /** キーの今回解放状態。 */
    bool m_KeysReleased[static_cast<usize>(EKey::_Count)]{};

    /** マウスボタンの現在押下状態。 */
    bool m_MouseButtonsDown[static_cast<usize>(EMouseButton::_Count)]{};

    /** マウスボタンの今回押下状態。 */
    bool m_MouseButtonsPressed[static_cast<usize>(EMouseButton::_Count)]{};

    /** マウスボタンの今回解放状態。 */
    bool m_MouseButtonsReleased[static_cast<usize>(EMouseButton::_Count)]{};

    /** プレイヤー別ゲームパッドボタンの現在押下状態。 */
    bool m_GamepadButtonsDown[kGamepadPlayerCount][static_cast<usize>(EGamepadButton::_Count)]{};

    /** プレイヤー別ゲームパッドボタンの今回押下状態。 */
    bool m_GamepadButtonsPressed[kGamepadPlayerCount][static_cast<usize>(EGamepadButton::_Count)]{};

    /** プレイヤー別ゲームパッドボタンの今回解放状態。 */
    bool m_GamepadButtonsReleased[kGamepadPlayerCount][static_cast<usize>(EGamepadButton::_Count)]{};

    /** プレイヤー別ゲームパッド軸値。 */
    f32 m_GamepadAxes[kGamepadPlayerCount][static_cast<usize>(EGamepadAxis::_Count)]{};
};

} // namespace acs::game
