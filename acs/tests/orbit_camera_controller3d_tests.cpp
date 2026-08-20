// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/OrbitCameraController3D.h"
#include "math/Math.h"

#include <limits>

using namespace acs;
using namespace acs::game;

namespace {

/** 3D vectorの長さを返す。 */
f32 Magnitude(FVec3 value) noexcept
{
    return Sqrt(LengthSq(value));
}

/** controller状態の全項目が誤差内で一致することを確認する。 */
void ExpectStateNear(const COrbitCameraController3D::FOrbitCameraState3D& actual, const COrbitCameraController3D::FOrbitCameraState3D& expected, f32 tolerance) noexcept
{
    EXPECT_NEAR(actual.target.x, expected.target.x, tolerance);
    EXPECT_NEAR(actual.target.y, expected.target.y, tolerance);
    EXPECT_NEAR(actual.target.z, expected.target.z, tolerance);
    EXPECT_NEAR(actual.yaw_radians, expected.yaw_radians, tolerance);
    EXPECT_NEAR(actual.pitch_radians, expected.pitch_radians, tolerance);
    EXPECT_NEAR(actual.distance, expected.distance, tolerance);
}

} // namespace

ACS_TEST(OrbitCameraController3D, ForwardMovementUsesYawAndOrbitDistance)
{
    COrbitCameraController3D controller;
    COrbitCameraController3D::FOrbitCameraState3D state{};
    state.pitch_radians = 0.0f;
    COrbitCameraController3D::FOrbitCameraInput3D input{};
    input.move_forward = 1.0f;

    EXPECT_TRUE(controller.TryStep(input, 0.5f, state));
    EXPECT_NEAR(state.target.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(state.target.y, 0.0f, 1.0e-6f);
    EXPECT_NEAR(state.target.z, 2.2f, 1.0e-5f);
}

ACS_TEST(OrbitCameraController3D, DiagonalMovementKeepsConfiguredSpeed)
{
    COrbitCameraController3D controller;
    COrbitCameraController3D::FOrbitCameraSettings3D settings = controller.Settings();
    settings.normalize_movement = true;
    EXPECT_TRUE(controller.TryConfigure(settings));
    COrbitCameraController3D::FOrbitCameraState3D state{};
    state.pitch_radians = 0.0f;
    state.distance = 1.0f;
    COrbitCameraController3D::FOrbitCameraInput3D input{};
    input.move_forward = 1.0f;
    input.move_right = 1.0f;

    EXPECT_TRUE(controller.TryStep(input, 1.0f, state));
    EXPECT_NEAR(Magnitude(state.target), 0.55f, 1.0e-5f);
}

ACS_TEST(OrbitCameraController3D, LookClampsPitchAndWrapsYaw)
{
    COrbitCameraController3D controller;
    COrbitCameraController3D::FOrbitCameraState3D state{};
    state.yaw_radians = 3.1f;
    state.pitch_radians = 1.4f;
    COrbitCameraController3D::FOrbitCameraInput3D input{};
    input.look_yaw = 1.0f;
    input.look_pitch = 1.0f;

    EXPECT_TRUE(controller.TryStep(input, 1.0f, state));
    EXPECT_TRUE(state.yaw_radians >= -3.14159266f && state.yaw_radians <= 3.14159266f);
    EXPECT_NEAR(state.pitch_radians, controller.Settings().pitch_limit_radians, 1.0e-6f);
}

ACS_TEST(OrbitCameraController3D, InterpolatesStateAcrossShortestYawArc)
{
    /** 既定の安全範囲で補間するcontroller。 */
    COrbitCameraController3D controller;
    /** +pi側にある前回固定tick状態。 */
    COrbitCameraController3D::FOrbitCameraState3D previous{};
    previous.target = FVec3{0.0f, 2.0f, 4.0f};
    previous.yaw_radians = ToRadians(170.0f);
    previous.pitch_radians = -0.2f;
    previous.distance = 10.0f;
    /** -pi側にある現在固定tick状態。 */
    COrbitCameraController3D::FOrbitCameraState3D current{};
    current.target = FVec3{10.0f, 6.0f, 8.0f};
    current.yaw_radians = ToRadians(-170.0f);
    current.pitch_radians = 0.6f;
    current.distance = 2.0f;
    /** 二状態の中間を受け取る描画用状態。 */
    COrbitCameraController3D::FOrbitCameraState3D interpolated{};

    EXPECT_TRUE(controller.TryInterpolateState(previous, current, 0.5, interpolated));
    EXPECT_NEAR(interpolated.target.x, 5.0f, 1.0e-6f);
    EXPECT_NEAR(interpolated.target.y, 4.0f, 1.0e-6f);
    EXPECT_NEAR(interpolated.target.z, 6.0f, 1.0e-6f);
    EXPECT_NEAR(Cos(interpolated.yaw_radians), -1.0f, 1.0e-5f);
    EXPECT_NEAR(Sin(interpolated.yaw_radians), 0.0f, 1.0e-5f);
    EXPECT_NEAR(interpolated.pitch_radians, 0.2f, 1.0e-6f);
    EXPECT_NEAR(interpolated.distance, 6.0f, 1.0e-6f);
}

ACS_TEST(OrbitCameraController3D, InvalidInterpolationPreservesOutput)
{
    /** 不正入力をtransactionalに拒否するcontroller。 */
    COrbitCameraController3D controller;
    /** 有効な前回固定tick状態。 */
    COrbitCameraController3D::FOrbitCameraState3D previous{};
    /** 検証ごとに壊す現在固定tick状態。 */
    COrbitCameraController3D::FOrbitCameraState3D current{};
    /** 失敗時に維持されるsentinel出力。 */
    COrbitCameraController3D::FOrbitCameraState3D output{};
    output.target = FVec3{9.0f, 8.0f, 7.0f};
    output.yaw_radians = 0.5f;
    output.pitch_radians = 0.25f;
    output.distance = 6.0f;
    /** 各失敗後に比較する変更前出力。 */
    const COrbitCameraController3D::FOrbitCameraState3D original = output;

    EXPECT_FALSE(controller.TryInterpolateState(previous, current, std::numeric_limits<f64>::quiet_NaN(), output));
    ExpectStateNear(output, original, 0.0f);
    EXPECT_FALSE(controller.TryInterpolateState(previous, current, 1.01, output));
    ExpectStateNear(output, original, 0.0f);
    current.distance = 0.0f;
    EXPECT_FALSE(controller.TryInterpolateState(previous, current, 0.5, output));
    ExpectStateNear(output, original, 0.0f);
}

ACS_TEST(OrbitCameraController3D, SnapshotValidationChecksBothFixedTickStates)
{
    /** snapshotを現在設定に対して検証するcontroller。 */
    COrbitCameraController3D controller;
    /** previous/currentがともに有効な固定tick保存値。 */
    COrbitCameraController3D::FOrbitCameraFixedStepSnapshot3D snapshot{};
    snapshot.previous.target = FVec3{-2.0f, 1.0f, 3.0f};
    snapshot.current.target = FVec3{4.0f, 5.0f, 6.0f};

    EXPECT_TRUE(controller.IsSnapshotValid(snapshot));
    snapshot.previous.pitch_radians = controller.Settings().pitch_limit_radians + 0.01f;
    EXPECT_FALSE(controller.IsSnapshotValid(snapshot));
    snapshot.previous = COrbitCameraController3D::FOrbitCameraState3D{};
    snapshot.current.distance = controller.Settings().maximum_distance + 1.0f;
    EXPECT_FALSE(controller.IsSnapshotValid(snapshot));
}

ACS_TEST(OrbitCameraController3D, ZoomChangesDistanceAndClampsRange)
{
    COrbitCameraController3D controller;
    COrbitCameraController3D::FOrbitCameraSettings3D settings = controller.Settings();
    settings.zoom_distance_scale_per_second = 0.5f;
    settings.minimum_distance = 2.0f;
    settings.maximum_distance = 10.0f;
    EXPECT_TRUE(controller.TryConfigure(settings));
    COrbitCameraController3D::FOrbitCameraState3D state{};
    state.distance = 8.0f;
    COrbitCameraController3D::FOrbitCameraInput3D input{};
    input.zoom = 1.0f;

    EXPECT_TRUE(controller.TryStep(input, 1.0f, state));
    EXPECT_NEAR(state.distance, 4.0f, 1.0e-6f);
    EXPECT_TRUE(controller.TryStep(input, 10.0f, state));
    EXPECT_NEAR(state.distance, 2.0f, 0.0f);
    input.zoom = -1.0f;
    EXPECT_TRUE(controller.TryStep(input, 10.0f, state));
    EXPECT_NEAR(state.distance, 10.0f, 0.0f);
}

ACS_TEST(OrbitCameraController3D, InvalidInputAndTimeAreTransactional)
{
    COrbitCameraController3D controller;
    COrbitCameraController3D::FOrbitCameraState3D state{};
    state.target = FVec3{3.0f, 4.0f, 5.0f};
    const COrbitCameraController3D::FOrbitCameraState3D original = state;
    COrbitCameraController3D::FOrbitCameraInput3D invalid_input{};
    invalid_input.move_forward = std::numeric_limits<f32>::quiet_NaN();

    EXPECT_FALSE(controller.TryStep(invalid_input, 1.0f / 60.0f, state));
    ExpectStateNear(state, original, 0.0f);
    EXPECT_FALSE(controller.TryStep(COrbitCameraController3D::FOrbitCameraInput3D{}, -1.0f, state));
    ExpectStateNear(state, original, 0.0f);
}

ACS_TEST(OrbitCameraController3D, InvalidConfigurationKeepsPreviousSettings)
{
    COrbitCameraController3D controller;
    const COrbitCameraController3D::FOrbitCameraSettings3D original = controller.Settings();
    COrbitCameraController3D::FOrbitCameraSettings3D invalid = original;
    invalid.yaw_radians_per_second = std::numeric_limits<f32>::infinity();

    EXPECT_FALSE(controller.TryConfigure(invalid));
    EXPECT_NEAR(controller.Settings().yaw_radians_per_second, original.yaw_radians_per_second, 0.0f);
    EXPECT_NEAR(controller.Settings().pitch_limit_radians, original.pitch_limit_radians, 0.0f);

    invalid = original;
    invalid.minimum_distance = 10.0f;
    invalid.maximum_distance = 1.0f;
    EXPECT_FALSE(controller.TryConfigure(invalid));
    EXPECT_NEAR(controller.Settings().minimum_distance, original.minimum_distance, 0.0f);
    EXPECT_NEAR(controller.Settings().maximum_distance, original.maximum_distance, 0.0f);
}

ACS_TEST(OrbitCameraController3D, ReplayingFixedInputsProducesSameState)
{
    COrbitCameraController3D controller;
    COrbitCameraController3D::FOrbitCameraState3D first{};
    COrbitCameraController3D::FOrbitCameraState3D replay{};
    COrbitCameraController3D::FOrbitCameraInput3D inputs[3]{};
    inputs[0].move_forward = 1.0f;
    inputs[0].look_yaw = 0.25f;
    inputs[1].move_right = -0.5f;
    inputs[1].look_pitch = 0.75f;
    inputs[2].move_up = 1.0f;
    inputs[2].zoom = -0.25f;

    for (u32 tick = 0u; tick < 180u; ++tick) {
        EXPECT_TRUE(controller.TryStep(inputs[tick % 3u], 1.0f / 60.0f, first));
        EXPECT_TRUE(controller.TryStep(inputs[tick % 3u], 1.0f / 60.0f, replay));
    }
    ExpectStateNear(first, replay, 0.0f);
}

ACS_TEST(OrbitCameraController3D, BuildsLegacyCompatibleLeftHandedView)
{
    COrbitCameraController3D controller;
    COrbitCameraController3D::FOrbitCameraState3D state{};
    state.target = FVec3{2.0f, 3.0f, 4.0f};
    state.yaw_radians = 0.0f;
    state.pitch_radians = 0.0f;
    state.distance = 8.0f;
    COrbitCameraController3D::FOrbitCameraView3D view{};

    EXPECT_TRUE(controller.TryBuildView(state, view));
    EXPECT_NEAR(view.eye.x, 2.0f, 1.0e-6f);
    EXPECT_NEAR(view.eye.y, 3.0f, 1.0e-6f);
    EXPECT_NEAR(view.eye.z, -4.0f, 1.0e-6f);
    EXPECT_NEAR(view.look_at.z, 4.0f, 1.0e-6f);
    EXPECT_NEAR(view.up.y, 1.0f, 1.0e-6f);
}

ACS_TEST(OrbitCameraController3D, InvalidStateDoesNotOverwriteView)
{
    COrbitCameraController3D controller;
    COrbitCameraController3D::FOrbitCameraState3D invalid{};
    invalid.distance = 0.0f;
    COrbitCameraController3D::FOrbitCameraView3D view{};
    view.eye = FVec3{9.0f, 8.0f, 7.0f};

    EXPECT_FALSE(controller.TryBuildView(invalid, view));
    EXPECT_NEAR(view.eye.x, 9.0f, 0.0f);
    EXPECT_NEAR(view.eye.y, 8.0f, 0.0f);
    EXPECT_NEAR(view.eye.z, 7.0f, 0.0f);
}
