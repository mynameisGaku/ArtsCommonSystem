// SPDX-License-Identifier: Apache-2.0
#include "gameframework/FixedStepInputBuffer.h"
#include "gameframework/InputMap.h"
#include "gameframework/InputStateSnapshot.h"
#include "test/Expect.h"
#include "test/Test.h"

#include <limits>

using namespace acs;
using namespace acs::game;

namespace {

/** 非有限なゲームパッド軸を返し、入力全体の拒否を再現する参照。 */
class CNonFiniteAxisInput final : public IInputStateView {
public:
    bool IsKeyDown(EKey) const noexcept override
    {
        return false;
    }
    bool IsKeyPressed(EKey) const noexcept override
    {
        return false;
    }
    bool IsKeyReleased(EKey) const noexcept override
    {
        return false;
    }
    bool IsMouseButtonDown(EMouseButton) const noexcept override
    {
        return false;
    }
    bool IsMouseButtonPressed(EMouseButton) const noexcept override
    {
        return false;
    }
    bool IsMouseButtonReleased(EMouseButton) const noexcept override
    {
        return false;
    }
    bool IsGamepadButtonDown(u32, EGamepadButton) const noexcept override
    {
        return false;
    }
    bool IsGamepadButtonPressed(u32, EGamepadButton) const noexcept override
    {
        return false;
    }
    bool IsGamepadButtonReleased(u32, EGamepadButton) const noexcept override
    {
        return false;
    }

    /** 先頭プレイヤーの左X軸だけ非有限値を返す。 */
    f32 GamepadAxisValue(u32 player_index, EGamepadAxis axis) const noexcept override
    {
        return player_index == 0u && axis == EGamepadAxis::LeftX ? std::numeric_limits<f32>::quiet_NaN() : 0.0f;
    }
};

} // namespace

ACS_TEST(FixedStepRuntimeInput, ExplicitStateEvaluatesThreeDimensionalControls)
{
    FInputMap input_map;
    const FActionId move_forward("MoveForward");
    const FActionId look_yaw("LookYaw");
    input_map.BindAxisKeys(move_forward, EKey::S, EKey::W);
    EXPECT_TRUE(input_map.TryBindGamepadAxis(look_yaw, EGamepadAxis::RightX, 0u, FInputAxisOptions{0.1f, 1.0f, false}));

    FInputStateSnapshot input;
    EXPECT_TRUE(input.TrySetKeyState(EKey::W, true, true, false));
    EXPECT_TRUE(input.TrySetGamepadAxis(0u, EGamepadAxis::RightX, 0.55f));

    const FInputActionState movement = input_map.Evaluate(move_forward, input);
    const FInputActionState look = input_map.Evaluate(look_yaw, input);
    EXPECT_TRUE(movement.held);
    EXPECT_NEAR(movement.axis, 1.0f, 1.0e-6f);
    EXPECT_TRUE(look.held);
    EXPECT_NEAR(look.axis, 0.5f, 1.0e-6f);
}

ACS_TEST(FixedStepRuntimeInput, CatchUpConsumesEdgesOnlyOnce)
{
    FFixedStepInputBuffer buffer;
    FInputStateSnapshot frame;
    EXPECT_TRUE(frame.TrySetKeyState(EKey::Space, true, true, false));
    EXPECT_TRUE(buffer.TryPushFrame(frame));

    FInputStateSnapshot first_tick;
    FInputStateSnapshot second_tick;
    EXPECT_TRUE(buffer.TryConsumeFixedStep(first_tick));
    EXPECT_TRUE(buffer.TryConsumeFixedStep(second_tick));
    EXPECT_TRUE(first_tick.IsKeyDown(EKey::Space));
    EXPECT_TRUE(first_tick.IsKeyPressed(EKey::Space));
    EXPECT_TRUE(second_tick.IsKeyDown(EKey::Space));
    EXPECT_FALSE(second_tick.IsKeyPressed(EKey::Space));
}

ACS_TEST(FixedStepRuntimeInput, ShortTapSurvivesFramesWithoutTick)
{
    FFixedStepInputBuffer buffer;
    FInputStateSnapshot pressed;
    FInputStateSnapshot released;
    EXPECT_TRUE(pressed.TrySetKeyState(EKey::Enter, true, true, false));
    EXPECT_TRUE(released.TrySetKeyState(EKey::Enter, false, false, true));
    EXPECT_TRUE(buffer.TryPushFrame(pressed));
    EXPECT_TRUE(buffer.TryPushFrame(released));

    FInputStateSnapshot tick;
    EXPECT_TRUE(buffer.TryConsumeFixedStep(tick));
    EXPECT_FALSE(tick.IsKeyDown(EKey::Enter));
    EXPECT_TRUE(tick.IsKeyPressed(EKey::Enter));
    EXPECT_TRUE(tick.IsKeyReleased(EKey::Enter));
}

ACS_TEST(FixedStepRuntimeInput, InvalidFramePreservesPendingSnapshot)
{
    FFixedStepInputBuffer buffer;
    FInputStateSnapshot valid;
    EXPECT_TRUE(valid.TrySetKeyState(EKey::Space, true, true, false));
    EXPECT_TRUE(valid.TrySetGamepadAxis(0u, EGamepadAxis::LeftX, 0.4f));
    EXPECT_TRUE(buffer.TryPushFrame(valid));

    const CNonFiniteAxisInput invalid;
    EXPECT_FALSE(buffer.TryPushFrame(invalid));

    FFixedStepInputBufferSnapshot saved;
    EXPECT_TRUE(buffer.TryCaptureSnapshot(saved));
    EXPECT_TRUE(saved.has_input_state);
    EXPECT_TRUE(saved.pending_input.IsKeyPressed(EKey::Space));
    EXPECT_NEAR(saved.pending_input.GamepadAxisValue(0u, EGamepadAxis::LeftX), 0.4f, 1.0e-6f);

    buffer.Reset();
    EXPECT_TRUE(buffer.TryRestoreSnapshot(saved));
    FInputStateSnapshot restored;
    EXPECT_TRUE(buffer.TryConsumeFixedStep(restored));
    EXPECT_TRUE(restored.IsKeyPressed(EKey::Space));
}
