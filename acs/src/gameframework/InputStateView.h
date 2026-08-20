// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "platform/InputCodes.h"

namespace acs::game {

/** 名前付きアクションの評価に必要な物理入力状態を読み取る境界。 */
class IInputStateView {
public:
    /** 派生した入力参照を基底ポインターから安全に破棄する。 */
    virtual ~IInputStateView() noexcept = default;

    /** 指定キーが現在押されているか返す。 */
    virtual bool IsKeyDown(EKey key) const noexcept = 0;

    /** 指定キーが今回押されたか返す。 */
    virtual bool IsKeyPressed(EKey key) const noexcept = 0;

    /** 指定キーが今回離されたか返す。 */
    virtual bool IsKeyReleased(EKey key) const noexcept = 0;

    /** 指定マウスボタンが現在押されているか返す。 */
    virtual bool IsMouseButtonDown(EMouseButton button) const noexcept = 0;

    /** 指定マウスボタンが今回押されたか返す。 */
    virtual bool IsMouseButtonPressed(EMouseButton button) const noexcept = 0;

    /** 指定マウスボタンが今回離されたか返す。 */
    virtual bool IsMouseButtonReleased(EMouseButton button) const noexcept = 0;

    /** 指定プレイヤーのゲームパッドボタンが現在押されているか返す。 */
    virtual bool IsGamepadButtonDown(u32 player_index, EGamepadButton button) const noexcept = 0;

    /** 指定プレイヤーのゲームパッドボタンが今回押されたか返す。 */
    virtual bool IsGamepadButtonPressed(u32 player_index, EGamepadButton button) const noexcept = 0;

    /** 指定プレイヤーのゲームパッドボタンが今回離されたか返す。 */
    virtual bool IsGamepadButtonReleased(u32 player_index, EGamepadButton button) const noexcept = 0;

    /** 指定プレイヤーのゲームパッド軸値を返す。 */
    virtual f32 GamepadAxisValue(u32 player_index, EGamepadAxis axis) const noexcept = 0;
};

} // namespace acs::game
