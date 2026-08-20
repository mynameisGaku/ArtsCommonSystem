// SPDX-License-Identifier: Apache-2.0
// actionと各入力deviceのbinding登録、明示状態評価、互換pollを実装する。
#include "gameframework/InputMap.h"

#include "platform/Input.h"

namespace acs::game {
namespace {

/** 現在のplatform入力をIInputStateViewとして公開する非所有アダプター。 */
class FPlatformInputStateView final : public IInputStateView {
public:
    /** 指定キーが現在押されているか返す。 */
    bool IsKeyDown(EKey key) const noexcept override
    {
        return CInput::IsKeyDown(key);
    }

    /** 指定キーが今回押されたか返す。 */
    bool IsKeyPressed(EKey key) const noexcept override
    {
        return CInput::IsKeyPressed(key);
    }

    /** 指定キーが今回離されたか返す。 */
    bool IsKeyReleased(EKey key) const noexcept override
    {
        return CInput::IsKeyReleased(key);
    }

    /** 指定マウスボタンが現在押されているか返す。 */
    bool IsMouseButtonDown(EMouseButton button) const noexcept override
    {
        return CInput::IsMouseButtonDown(button);
    }

    /** 指定マウスボタンが今回押されたか返す。 */
    bool IsMouseButtonPressed(EMouseButton button) const noexcept override
    {
        return CInput::IsMouseButtonPressed(button);
    }

    /** 指定マウスボタンが今回離されたか返す。 */
    bool IsMouseButtonReleased(EMouseButton button) const noexcept override
    {
        return CInput::IsMouseButtonReleased(button);
    }

    /** 指定プレイヤーのゲームパッドボタンが現在押されているか返す。 */
    bool IsGamepadButtonDown(u32 player_index, EGamepadButton button) const noexcept override
    {
        return CInput::IsGamepadButtonDown(player_index, button);
    }

    /** 指定プレイヤーのゲームパッドボタンが今回押されたか返す。 */
    bool IsGamepadButtonPressed(u32 player_index, EGamepadButton button) const noexcept override
    {
        return CInput::IsGamepadButtonPressed(player_index, button);
    }

    /** 指定プレイヤーのゲームパッドボタンが今回離されたか返す。 */
    bool IsGamepadButtonReleased(u32 player_index, EGamepadButton button) const noexcept override
    {
        return CInput::IsGamepadButtonReleased(player_index, button);
    }

    /** 指定プレイヤーのゲームパッド軸値を返す。 */
    f32 GamepadAxisValue(u32 player_index, EGamepadAxis axis) const noexcept override
    {
        return CInput::GamepadAxisValue(player_index, axis);
    }
};

/** 公開列挙に含まれるゲームパッド軸ならtrueを返す。 */
bool IsKnownGamepadAxis(EGamepadAxis axis) noexcept
{
    return static_cast<usize>(axis) < static_cast<usize>(EGamepadAxis::_Count);
}

} // namespace

void FInputMap::BindKey(FActionId action, EKey key) noexcept
{
    FBinding binding;
    binding.action = action;
    binding.kind = EBindKind::Key;
    binding.code = static_cast<u32>(key);
    m_Bindings.Add(binding);
}

void FInputMap::BindMouseButton(FActionId action, EMouseButton button) noexcept
{
    FBinding binding;
    binding.action = action;
    binding.kind = EBindKind::MouseButton;
    binding.code = static_cast<u32>(button);
    m_Bindings.Add(binding);
}

void FInputMap::BindGamepad(FActionId action, EGamepadButton button, u32 player_index) noexcept
{
    FBinding binding;
    binding.action = action;
    binding.kind = EBindKind::GamepadButton;
    binding.code = static_cast<u32>(button);
    binding.player = player_index;
    m_Bindings.Add(binding);
}

void FInputMap::BindAxisKeys(FActionId action, EKey negative, EKey positive) noexcept
{
    FBinding binding;
    binding.action = action;
    binding.kind = EBindKind::Axis1D;
    binding.code = static_cast<u32>(negative);
    binding.code_pos = static_cast<u32>(positive);
    m_Bindings.Add(binding);
}

void FInputMap::BindGamepadAxis(FActionId action, EGamepadAxis axis, u32 player_index, f32 scale) noexcept
{
    (void)TryBindGamepadAxis(action, axis, player_index, FInputAxisOptions{0.0f, scale, false});
}

bool FInputMap::TryBindGamepadAxis(FActionId action, EGamepadAxis axis, u32 player_index,
                                   const FInputAxisOptions& options) noexcept
{
    if (action.value == 0u || !IsKnownGamepadAxis(axis) || player_index >= 4u || !options.IsValid()) return false;
    FBinding binding;
    binding.action = action;
    binding.kind = EBindKind::GamepadAxis;
    binding.code = static_cast<u32>(axis);
    binding.player = player_index;
    binding.axis_options = options;
    m_Bindings.Add(binding);
    return true;
}

void FInputMap::Unbind(FActionId action) noexcept
{
    u32 write_index = 0u;
    for (u32 read_index = 0u; read_index < m_Bindings.Num(); ++read_index) {
        if (m_Bindings[read_index].action != action) {
            if (write_index != read_index) m_Bindings[write_index] = m_Bindings[read_index];
            ++write_index;
        }
    }
    while (m_Bindings.Num() > write_index)
        m_Bindings.Pop();
}

void FInputMap::ClearAll() noexcept
{
    m_Bindings.Reset();
}

FInputActionState FInputMap::Evaluate(FActionId action, const IInputStateView& input) const noexcept
{
    FInputActionState result{};
    for (u32 index = 0u; index < m_Bindings.Num(); ++index) {
        const FBinding& binding = m_Bindings[index];
        if (binding.action != action) continue;
        switch (binding.kind) {
        case EBindKind::Key: {
            const EKey key = static_cast<EKey>(binding.code);
            result.pressed = result.pressed || input.IsKeyPressed(key);
            result.held = result.held || input.IsKeyDown(key);
            result.released = result.released || input.IsKeyReleased(key);
            break;
        }
        case EBindKind::MouseButton: {
            const EMouseButton button = static_cast<EMouseButton>(binding.code);
            result.pressed = result.pressed || input.IsMouseButtonPressed(button);
            result.held = result.held || input.IsMouseButtonDown(button);
            result.released = result.released || input.IsMouseButtonReleased(button);
            break;
        }
        case EBindKind::GamepadButton: {
            const EGamepadButton button = static_cast<EGamepadButton>(binding.code);
            result.pressed = result.pressed || input.IsGamepadButtonPressed(binding.player, button);
            result.held = result.held || input.IsGamepadButtonDown(binding.player, button);
            result.released = result.released || input.IsGamepadButtonReleased(binding.player, button);
            break;
        }
        case EBindKind::GamepadAxis: {
            const f32 value = binding.axis_options.Apply(
                input.GamepadAxisValue(binding.player, static_cast<EGamepadAxis>(binding.code)));
            result.axis += value;
            result.held = result.held || value < -0.0001f || value > 0.0001f;
            break;
        }
        case EBindKind::Axis1D: {
            const bool negative = input.IsKeyDown(static_cast<EKey>(binding.code));
            const bool positive = input.IsKeyDown(static_cast<EKey>(binding.code_pos));
            result.held = result.held || negative || positive;
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

bool FInputMap::IsPressed(FActionId action) const noexcept
{
    const FPlatformInputStateView input;
    return Evaluate(action, input).pressed;
}

bool FInputMap::IsHeld(FActionId action) const noexcept
{
    const FPlatformInputStateView input;
    return Evaluate(action, input).held;
}

bool FInputMap::IsReleased(FActionId action) const noexcept
{
    const FPlatformInputStateView input;
    return Evaluate(action, input).released;
}

f32 FInputMap::Axis(FActionId action) const noexcept
{
    const FPlatformInputStateView input;
    return Evaluate(action, input).axis;
}

f32 FInputMap::AxisValue(FActionId action, FInputAxisOptions options) const noexcept
{
    return options.Apply(Axis(action));
}

} // namespace acs::game
