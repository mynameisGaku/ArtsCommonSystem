// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar D — FInputMap 実装
#include "gameframework/InputMap.h"
#include "platform/Input.h"

namespace acs::game {

namespace {

/** 現在の platform 入力を IInputStateView として公開する非所有アダプター。 */
class FPlatformInputStateView final : public IInputStateView {
public:
    /** 指定キーが現在押されているか返す。 */
    bool IsKeyDown(EKey key) const noexcept override
    {
        return FInput::IsKeyDown(key);
    }

    /** 指定キーが今回押されたか返す。 */
    bool IsKeyPressed(EKey key) const noexcept override
    {
        return FInput::IsKeyPressed(key);
    }

    /** 指定キーが今回離されたか返す。 */
    bool IsKeyReleased(EKey key) const noexcept override
    {
        return FInput::IsKeyReleased(key);
    }

    /** 指定マウスボタンが現在押されているか返す。 */
    bool IsMouseButtonDown(EMouseButton button) const noexcept override
    {
        return FInput::IsMouseButtonDown(button);
    }

    /** 指定マウスボタンが今回押されたか返す。 */
    bool IsMouseButtonPressed(EMouseButton button) const noexcept override
    {
        return FInput::IsMouseButtonPressed(button);
    }

    /** 指定マウスボタンが今回離されたか返す。 */
    bool IsMouseButtonReleased(EMouseButton button) const noexcept override
    {
        return FInput::IsMouseButtonReleased(button);
    }

    /** 指定プレイヤーのゲームパッドボタンが現在押されているか返す。 */
    bool IsGamepadButtonDown(u32 player_index, EGamepadButton button) const noexcept override
    {
        return FInput::IsGamepadButtonDown(player_index, button);
    }

    /** 指定プレイヤーのゲームパッドボタンが今回押されたか返す。 */
    bool IsGamepadButtonPressed(u32 player_index, EGamepadButton button) const noexcept override
    {
        return FInput::IsGamepadButtonPressed(player_index, button);
    }

    /** 指定プレイヤーのゲームパッドボタンが今回離されたか返す。 */
    bool IsGamepadButtonReleased(u32 player_index, EGamepadButton button) const noexcept override
    {
        return FInput::IsGamepadButtonReleased(player_index, button);
    }

    /** 指定プレイヤーのゲームパッド軸値を返す。 */
    f32 GamepadAxisValue(u32 player_index, EGamepadAxis axis) const noexcept override
    {
        return FInput::GamepadAxisValue(player_index, axis);
    }
};

} // namespace

/** キーボードキーの binding を末尾に追加する。 */
void FInputMap::BindKey(FActionId action, EKey key) noexcept
{
    FBinding b;
    b.action = action;
    b.kind = EBindKind::Key;
    b.code = static_cast<u32>(key);
    m_Bindings.PushBack(b);
}

/** マウスボタンの binding を末尾に追加する。 */
void FInputMap::BindMouseButton(FActionId action, EMouseButton mb) noexcept
{
    FBinding b;
    b.action = action;
    b.kind = EBindKind::MouseButton;
    b.code = static_cast<u32>(mb);
    m_Bindings.PushBack(b);
}

/** ゲームパッドボタンの binding を player_index 付きで末尾に追加する。 */
void FInputMap::BindGamepad(FActionId action, EGamepadButton gb, u32 player_index) noexcept
{
    FBinding b;
    b.action = action;
    b.kind = EBindKind::GamepadButton;
    b.code = static_cast<u32>(gb);
    b.player = player_index;
    m_Bindings.PushBack(b);
}

/** neg/pos キーのペアを 1D axis binding として末尾に追加する。 */
void FInputMap::BindAxisKeys(FActionId action, EKey neg, EKey pos) noexcept
{
    FBinding b;
    b.action = action;
    b.kind = EBindKind::Axis1D;
    b.code = static_cast<u32>(neg);
    b.code_pos = static_cast<u32>(pos);
    m_Bindings.PushBack(b);
}

/** ゲームパッドのアナログ軸 binding を倍率付きで末尾へ追加する。 */
void FInputMap::BindGamepadAxis(FActionId action, EGamepadAxis axis, u32 player_index, f32 scale) noexcept
{
    (void)TryBindGamepadAxis(action, axis, player_index, FInputAxisOptions{0.0f, scale, false});
}

/** ゲームパッド軸 binding を検証し、成功時だけ追加する。 */
bool FInputMap::TryBindGamepadAxis(FActionId action, EGamepadAxis axis, u32 player_index,
                                   const FInputAxisOptions& options) noexcept
{
    bool known_axis = false;
    switch (axis) {
    case EGamepadAxis::LeftX:
    case EGamepadAxis::LeftY:
    case EGamepadAxis::RightX:
    case EGamepadAxis::RightY:
    case EGamepadAxis::LeftTrigger:
    case EGamepadAxis::RightTrigger:
        known_axis = true;
        break;
    case EGamepadAxis::_Count:
        break;
    }
    if (action.value == 0u || !known_axis || player_index >= 4u || !options.IsValid()) return false;

    FBinding b;
    b.action = action;
    b.kind = EBindKind::GamepadAxis;
    b.code = static_cast<u32>(axis);
    b.player = player_index;
    b.axis_options = options;
    m_Bindings.PushBack(b);
    return true;
}

/** 指定アクションの binding を in-place の compaction で全削除する。 */
void FInputMap::Unbind(FActionId action) noexcept
{
    u32 w = 0;
    for (u32 r = 0; r < m_Bindings.Size(); ++r) {
        if (m_Bindings[r].action != action) {
            if (w != r) m_Bindings[w] = m_Bindings[r];
            ++w;
        }
    }
    while (m_Bindings.Size() > w)
        m_Bindings.PopBack();
}

/** 全 binding を破棄する。 */
void FInputMap::ClearAll() noexcept
{
    m_Bindings.Clear();
}

/** 明示入力からデジタル状態と軸値を一度の binding 走査で評価する。 */
FInputActionState FInputMap::Evaluate(FActionId action, const IInputStateView& input) const noexcept
{
    FInputActionState result{};
    for (u32 i = 0; i < m_Bindings.Size(); ++i) {
        const FBinding& b = m_Bindings[i];
        if (b.action != action) continue;
        switch (b.kind) {
        case EBindKind::Key: {
            const EKey key = static_cast<EKey>(b.code);
            if (input.IsKeyPressed(key)) result.pressed = true;
            if (input.IsKeyDown(key)) result.held = true;
            if (input.IsKeyReleased(key)) result.released = true;
            break;
        }
        case EBindKind::MouseButton: {
            const EMouseButton button = static_cast<EMouseButton>(b.code);
            if (input.IsMouseButtonPressed(button)) result.pressed = true;
            if (input.IsMouseButtonDown(button)) result.held = true;
            if (input.IsMouseButtonReleased(button)) result.released = true;
            break;
        }
        case EBindKind::GamepadButton: {
            const EGamepadButton button = static_cast<EGamepadButton>(b.code);
            if (input.IsGamepadButtonPressed(b.player, button)) result.pressed = true;
            if (input.IsGamepadButtonDown(b.player, button)) result.held = true;
            if (input.IsGamepadButtonReleased(b.player, button)) result.released = true;
            break;
        }
        case EBindKind::GamepadAxis: {
            const f32 value = b.axis_options.Apply(input.GamepadAxisValue(b.player, static_cast<EGamepadAxis>(b.code)));
            result.axis += value;
            if (value < -0.0001f || value > 0.0001f) result.held = true;
            break;
        }
        case EBindKind::Axis1D: {
            const bool negative = input.IsKeyDown(static_cast<EKey>(b.code));
            const bool positive = input.IsKeyDown(static_cast<EKey>(b.code_pos));
            if (negative || positive) result.held = true;
            if (negative && !positive)
                result.axis -= 1.0f;
            else if (positive && !negative)
                result.axis += 1.0f;
            break;
        }
        }
    }

    if (result.axis > 1.0f) result.axis = 1.0f;
    if (result.axis < -1.0f) result.axis = -1.0f;
    return result;
}

/** 現在の platform 入力でアクションの押下開始を評価する互換 API。 */
bool FInputMap::IsPressed(FActionId action) const noexcept
{
    const FPlatformInputStateView input;
    return Evaluate(action, input).pressed;
}

/** 現在の platform 入力でアクションの保持状態を評価する互換 API。 */
bool FInputMap::IsHeld(FActionId action) const noexcept
{
    const FPlatformInputStateView input;
    return Evaluate(action, input).held;
}

/** 現在の platform 入力でアクションの解放を評価する互換 API。 */
bool FInputMap::IsReleased(FActionId action) const noexcept
{
    const FPlatformInputStateView input;
    return Evaluate(action, input).released;
}

/** 現在の platform 入力でアクションの軸値を評価する互換 API。 */
f32 FInputMap::Axis(FActionId action) const noexcept
{
    const FPlatformInputStateView input;
    return Evaluate(action, input).axis;
}

} // namespace acs::game
