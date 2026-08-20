// SPDX-License-Identifier: Apache-2.0
#include "gameframework/InputMap.h"
#include "gameframework/InputStateSnapshot.h"
#include "gameframework/PlatformInputStateAdapter.h"
#include "platform/Event.h"
#include "platform/Input.h"
#include "test/Expect.h"
#include "test/Test.h"

#include <limits>

using namespace acs;
using namespace acs::game;

ACS_TEST(InputMapState, ExplicitSnapshotEvaluatesDigitalBindings)
{
    FInputMap map;
    const FActionId jump("Jump");
    const FActionId fire("Fire");
    map.BindKey(jump, EKey::Space);
    map.BindGamepad(jump, EGamepadButton::A, 1u);
    map.BindMouseButton(fire, EMouseButton::Left);

    FInputStateSnapshot input;
    EXPECT_TRUE(input.TrySetKeyState(EKey::Space, false, true, true));
    EXPECT_TRUE(input.TrySetMouseButtonState(EMouseButton::Left, true, true, false));
    EXPECT_TRUE(input.TrySetGamepadButtonState(1u, EGamepadButton::A, true, false, false));

    const FInputActionState jump_state = map.Evaluate(jump, input);
    EXPECT_TRUE(jump_state.pressed);
    EXPECT_TRUE(jump_state.held);
    EXPECT_TRUE(jump_state.released);
    EXPECT_NEAR(jump_state.axis, 0.0f, 1.0e-6f);

    const FInputActionState fire_state = map.Evaluate(fire, input);
    EXPECT_TRUE(fire_state.pressed);
    EXPECT_TRUE(fire_state.held);
    EXPECT_FALSE(fire_state.released);
}

ACS_TEST(InputMapState, SameSnapshotProducesSameAxisState)
{
    FInputMap map;
    const FActionId move("MoveX");
    map.BindAxisKeys(move, EKey::A, EKey::D);
    EXPECT_TRUE(map.TryBindGamepadAxis(move, EGamepadAxis::LeftX, 0u, FInputAxisOptions{0.0f, 1.0f, false}));

    FInputStateSnapshot input;
    EXPECT_TRUE(input.TrySetKeyState(EKey::A, true, false, false));
    EXPECT_TRUE(input.TrySetKeyState(EKey::D, true, false, false));
    EXPECT_TRUE(input.TrySetGamepadAxis(0u, EGamepadAxis::LeftX, 0.25f));

    const FInputActionState first = map.Evaluate(move, input);
    const FInputActionState second = map.Evaluate(move, input);
    EXPECT_EQ(first.pressed, second.pressed);
    EXPECT_EQ(first.held, second.held);
    EXPECT_EQ(first.released, second.released);
    EXPECT_NEAR(first.axis, second.axis, 1.0e-6f);
    EXPECT_TRUE(first.held);
    EXPECT_NEAR(first.axis, 0.25f, 1.0e-6f);

    EXPECT_TRUE(input.TrySetKeyState(EKey::A, false, false, false));
    const FInputActionState clamped = map.Evaluate(move, input);
    EXPECT_NEAR(clamped.axis, 1.0f, 1.0e-6f);
}

ACS_TEST(InputMapState, InvalidSnapshotInputPreservesAcceptedState)
{
    FInputMap map;
    const FActionId look("LookX");
    EXPECT_TRUE(map.TryBindGamepadAxis(look, EGamepadAxis::RightX, 0u, FInputAxisOptions{}));

    FInputStateSnapshot input;
    EXPECT_TRUE(input.TrySetGamepadAxis(0u, EGamepadAxis::RightX, 0.5f));
    EXPECT_FALSE(input.TrySetKeyState(EKey::Unknown, true, true, false));
    EXPECT_FALSE(input.TrySetMouseButtonState(static_cast<EMouseButton>(255u), true, true, false));
    EXPECT_FALSE(input.TrySetGamepadButtonState(4u, EGamepadButton::A, true, true, false));
    EXPECT_FALSE(input.TrySetGamepadAxis(0u, EGamepadAxis::RightX, std::numeric_limits<f32>::quiet_NaN()));
    EXPECT_FALSE(input.TrySetGamepadAxis(0u, EGamepadAxis::RightX, 1.01f));
    EXPECT_FALSE(input.TrySetGamepadAxis(0u, EGamepadAxis::LeftTrigger, -0.01f));

    const FInputActionState state = map.Evaluate(look, input);
    EXPECT_NEAR(state.axis, 0.5f, 1.0e-6f);
    EXPECT_TRUE(state.held);
}

ACS_TEST(InputMapState, ClearResetsEveryInputKind)
{
    FInputMap map;
    const FActionId action("Action");
    map.BindKey(action, EKey::Enter);
    map.BindMouseButton(action, EMouseButton::Right);
    map.BindGamepad(action, EGamepadButton::B, 2u);
    EXPECT_TRUE(map.TryBindGamepadAxis(action, EGamepadAxis::RightY, 2u, FInputAxisOptions{}));

    FInputStateSnapshot input;
    EXPECT_TRUE(input.TrySetKeyState(EKey::Enter, true, true, false));
    EXPECT_TRUE(input.TrySetMouseButtonState(EMouseButton::Right, true, true, false));
    EXPECT_TRUE(input.TrySetGamepadButtonState(2u, EGamepadButton::B, true, true, false));
    EXPECT_TRUE(input.TrySetGamepadAxis(2u, EGamepadAxis::RightY, -0.75f));
    input.Clear();

    const FInputActionState state = map.Evaluate(action, input);
    EXPECT_FALSE(state.pressed);
    EXPECT_FALSE(state.held);
    EXPECT_FALSE(state.released);
    EXPECT_NEAR(state.axis, 0.0f, 1.0e-6f);
}

ACS_TEST(InputMapState, PlatformAdapterCapturesOneStableInputPoint)
{
    FEvent lost_focus{};
    lost_focus.type = EEventType::WindowLostFocus;
    FInput::OnEvent(lost_focus);
    FInput::Update();

    FEvent key_pressed{};
    key_pressed.type = EEventType::KeyPressed;
    key_pressed.key.key = EKey::Space;
    FInput::OnEvent(key_pressed);
    FEvent mouse_pressed{};
    mouse_pressed.type = EEventType::MouseButtonPressed;
    mouse_pressed.mouse_button.button = EMouseButton::Left;
    FInput::OnEvent(mouse_pressed);

    FInputStateSnapshot input;
    EXPECT_TRUE(FPlatformInputStateAdapter::TryCapture(input));
    FInput::Update();

    FInputMap map;
    const FActionId jump("Jump");
    const FActionId fire("Fire");
    map.BindKey(jump, EKey::Space);
    map.BindMouseButton(fire, EMouseButton::Left);
    EXPECT_TRUE(map.Evaluate(jump, input).pressed);
    EXPECT_TRUE(map.Evaluate(jump, input).held);
    EXPECT_TRUE(map.Evaluate(fire, input).pressed);
    EXPECT_TRUE(map.Evaluate(fire, input).held);
    EXPECT_FALSE(map.IsPressed(jump));
    EXPECT_TRUE(map.IsHeld(jump));

    FInput::OnEvent(lost_focus);
    FInput::Update();
}
