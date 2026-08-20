// SPDX-License-Identifier: Apache-2.0
#include "gameframework/FixedStepInputBuffer.h"
#include "gameframework/InputStateSnapshot.h"
#include "gameframework/OrbitCameraInputActionSet3D.h"
#include "test/Expect.h"
#include "test/Test.h"

using namespace acs;
using namespace acs::game;

ACS_TEST(OrbitCameraInputActionSet3D, DefaultActionsEvaluateSixAxes)
{
    FOrbitCameraInputActionSet3D actions{};
    FInputMap input_map;
    input_map.BindAxisKeys(actions.move_forward_action, EKey::S, EKey::W);
    input_map.BindAxisKeys(actions.move_right_action, EKey::A, EKey::D);
    input_map.BindAxisKeys(actions.move_up_action, EKey::Q, EKey::E);
    input_map.BindAxisKeys(actions.zoom_action, EKey::PageDown, EKey::PageUp);
    EXPECT_TRUE(input_map.TryBindGamepadAxis(actions.look_yaw_action, EGamepadAxis::RightX, 0u, FInputAxisOptions{}));
    EXPECT_TRUE(input_map.TryBindGamepadAxis(actions.look_pitch_action, EGamepadAxis::RightY, 0u, FInputAxisOptions{}));

    FInputStateSnapshot input;
    EXPECT_TRUE(input.TrySetKeyState(EKey::W, true, true, false));
    EXPECT_TRUE(input.TrySetKeyState(EKey::D, true, true, false));
    EXPECT_TRUE(input.TrySetKeyState(EKey::E, true, true, false));
    EXPECT_TRUE(input.TrySetKeyState(EKey::PageUp, true, true, false));
    EXPECT_TRUE(input.TrySetGamepadAxis(0u, EGamepadAxis::RightX, 0.5f));
    EXPECT_TRUE(input.TrySetGamepadAxis(0u, EGamepadAxis::RightY, -0.25f));

    COrbitCameraController3D::FOrbitCameraInput3D output{};
    EXPECT_TRUE(actions.TryEvaluate(input_map, input, output));
    EXPECT_NEAR(output.move_forward, 1.0f, 1.0e-6f);
    EXPECT_NEAR(output.move_right, 1.0f, 1.0e-6f);
    EXPECT_NEAR(output.move_up, 1.0f, 1.0e-6f);
    EXPECT_NEAR(output.look_yaw, 0.5f, 1.0e-6f);
    EXPECT_NEAR(output.look_pitch, -0.25f, 1.0e-6f);
    EXPECT_NEAR(output.zoom, 1.0f, 1.0e-6f);
}

ACS_TEST(OrbitCameraInputActionSet3D, InvalidActionsPreserveOutput)
{
    FOrbitCameraInputActionSet3D actions{};
    actions.look_pitch_action = actions.look_yaw_action;
    FInputMap input_map;
    FInputStateSnapshot input;
    COrbitCameraController3D::FOrbitCameraInput3D output{};
    output.move_forward = 0.75f;
    output.look_pitch = -0.5f;

    EXPECT_FALSE(actions.IsValid());
    EXPECT_FALSE(actions.TryEvaluate(input_map, input, output));
    EXPECT_NEAR(output.move_forward, 0.75f, 0.0f);
    EXPECT_NEAR(output.look_pitch, -0.5f, 0.0f);

    actions.look_pitch_action = FActionId{};
    EXPECT_FALSE(actions.IsValid());
}

ACS_TEST(OrbitCameraInputActionSet3D, CustomActionsFeedFixedOrbitStep)
{
    FOrbitCameraInputActionSet3D actions{};
    actions.move_forward_action = FActionId("EditorDolly");
    actions.move_right_action = FActionId("EditorTruck");
    actions.move_up_action = FActionId("EditorPedestal");
    actions.look_yaw_action = FActionId("EditorPan");
    actions.look_pitch_action = FActionId("EditorTilt");
    actions.zoom_action = FActionId("EditorZoom");
    EXPECT_TRUE(actions.IsValid());

    FInputMap input_map;
    input_map.BindAxisKeys(actions.move_forward_action, EKey::S, EKey::W);
    EXPECT_TRUE(input_map.TryBindGamepadAxis(actions.look_yaw_action, EGamepadAxis::RightX, 0u, FInputAxisOptions{}));

    FInputStateSnapshot frame_input;
    EXPECT_TRUE(frame_input.TrySetKeyState(EKey::W, true, true, false));
    EXPECT_TRUE(frame_input.TrySetGamepadAxis(0u, EGamepadAxis::RightX, 0.25f));
    FFixedStepInputBuffer fixed_input;
    EXPECT_TRUE(fixed_input.TryPushFrame(frame_input));
    FInputStateSnapshot fixed_tick;
    EXPECT_TRUE(fixed_input.TryConsumeFixedStep(fixed_tick));

    COrbitCameraController3D::FOrbitCameraInput3D camera_input{};
    EXPECT_TRUE(actions.TryEvaluate(input_map, fixed_tick, camera_input));
    COrbitCameraController3D controller;
    COrbitCameraController3D::FOrbitCameraState3D state{};
    state.pitch_radians = 0.0f;
    EXPECT_TRUE(controller.TryStep(camera_input, 1.0f / 60.0f, state));
    EXPECT_TRUE(state.target.z > 0.0f);
    EXPECT_TRUE(state.yaw_radians > 0.0f);
}

ACS_TEST(OrbitCameraInputActionSet3D, UnboundActionsProduceNeutralInput)
{
    const FOrbitCameraInputActionSet3D actions{};
    const FInputMap input_map;
    const FInputStateSnapshot input;
    COrbitCameraController3D::FOrbitCameraInput3D output{};
    output.move_forward = 1.0f;

    EXPECT_TRUE(actions.TryEvaluate(input_map, input, output));
    EXPECT_NEAR(output.move_forward, 0.0f, 0.0f);
    EXPECT_NEAR(output.move_right, 0.0f, 0.0f);
    EXPECT_NEAR(output.move_up, 0.0f, 0.0f);
    EXPECT_NEAR(output.look_yaw, 0.0f, 0.0f);
    EXPECT_NEAR(output.look_pitch, 0.0f, 0.0f);
    EXPECT_NEAR(output.zoom, 0.0f, 0.0f);
}
