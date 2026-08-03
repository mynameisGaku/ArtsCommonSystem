// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "foundation/Move.h"
#include "gameframework/Easing.h"
#include "gameframework/Sequence.h"
#include "gameframework/Tween.h"

#include <limits>

using namespace acs;
using namespace acs::game;

namespace {

f32 NonFiniteCustomEasing(f32) noexcept {
    return std::numeric_limits<f32>::quiet_NaN();
}

FSeqHandle StartSequence(
    CSequenceRunner& runner, FSequence& sequence) noexcept {
    return runner.Start(Move(sequence));
}

bool IsFiniteValue(f32 value) noexcept {
    const f32 maximum = std::numeric_limits<f32>::max();
    return value == value && value >= -maximum && value <= maximum;
}

} // namespace

ACS_TEST(EasingTweenIntegration, TweenEnumOverloadsDriveAllValueTypes) {
    CTweenManager manager;
    f32 scalar = 0.0f;
    FVec2 vector2{0.0f, 2.0f};
    FVec3 vector3{0.0f, 10.0f, 20.0f};

    const FTweenHandle scalar_handle = manager.Tween(
        &scalar, 0.0f, 8.0f, 2.0f, Easing::EEasingType::InQuad);
    const FTweenHandle vector2_handle = manager.Tween(
        &vector2, FVec2{0.0f, 2.0f}, FVec2{4.0f, 6.0f}, 2.0f,
        Easing::EEasingType::OutQuad);
    const FTweenHandle vector3_handle = manager.Tween(
        &vector3, FVec3{0.0f, 10.0f, 20.0f},
        FVec3{10.0f, 20.0f, 30.0f}, 2.0f,
        Easing::EEasingType::SmoothStep);

    EXPECT_TRUE(scalar_handle.IsValid());
    EXPECT_TRUE(vector2_handle.IsValid());
    EXPECT_TRUE(vector3_handle.IsValid());
    EXPECT_EQ(manager.ActiveCount(), 3u);

    manager.Tick(1.0f);
    EXPECT_TRUE(IsFiniteValue(scalar));
    EXPECT_TRUE(IsFiniteValue(vector2.x));
    EXPECT_TRUE(IsFiniteValue(vector2.y));
    EXPECT_TRUE(IsFiniteValue(vector3.x));
    EXPECT_TRUE(IsFiniteValue(vector3.y));
    EXPECT_TRUE(IsFiniteValue(vector3.z));
    EXPECT_NEAR(scalar, 2.0f, 1.0e-5f);
    EXPECT_NEAR(vector2.x, 3.0f, 1.0e-5f);
    EXPECT_NEAR(vector2.y, 5.0f, 1.0e-5f);
    EXPECT_NEAR(vector3.x, 5.0f, 1.0e-5f);
    EXPECT_NEAR(vector3.y, 15.0f, 1.0e-5f);
    EXPECT_NEAR(vector3.z, 25.0f, 1.0e-5f);
    EXPECT_EQ(manager.ActiveCount(), 3u);

    manager.Tick(1.0f);
    EXPECT_EQ(scalar, 8.0f);
    EXPECT_EQ(vector2.x, 4.0f);
    EXPECT_EQ(vector2.y, 6.0f);
    EXPECT_EQ(vector3.x, 10.0f);
    EXPECT_EQ(vector3.y, 20.0f);
    EXPECT_EQ(vector3.z, 30.0f);
    EXPECT_EQ(manager.ActiveCount(), 0u);
    EXPECT_FALSE(manager.IsActive(scalar_handle));
    EXPECT_FALSE(manager.IsActive(vector2_handle));
    EXPECT_FALSE(manager.IsActive(vector3_handle));
}

ACS_TEST(EasingTweenIntegration, SequenceEnumOverloadsDriveAllValueTypes) {
    CSequenceRunner runner;
    f32 scalar = 0.0f;
    FVec2 vector2{0.0f, 2.0f};
    FVec3 vector3{0.0f, 10.0f, 20.0f};

    FSequence scalar_sequence;
    scalar_sequence.Tween(
        &scalar, 0.0f, 8.0f, 2.0f, Easing::EEasingType::InQuad);
    FSequence vector2_sequence;
    vector2_sequence.Tween(
        &vector2, FVec2{0.0f, 2.0f}, FVec2{4.0f, 6.0f}, 2.0f,
        Easing::EEasingType::OutQuad);
    FSequence vector3_sequence;
    vector3_sequence.Tween(
        &vector3, FVec3{0.0f, 10.0f, 20.0f},
        FVec3{10.0f, 20.0f, 30.0f}, 2.0f,
        Easing::EEasingType::SmoothStep);

    const FSeqHandle scalar_handle =
        StartSequence(runner, scalar_sequence);
    const FSeqHandle vector2_handle =
        StartSequence(runner, vector2_sequence);
    const FSeqHandle vector3_handle =
        StartSequence(runner, vector3_sequence);
    EXPECT_TRUE(scalar_handle.IsValid());
    EXPECT_TRUE(vector2_handle.IsValid());
    EXPECT_TRUE(vector3_handle.IsValid());
    EXPECT_EQ(runner.ActiveCount(), 3u);

    runner.Tick(1.0f);
    EXPECT_TRUE(IsFiniteValue(scalar));
    EXPECT_TRUE(IsFiniteValue(vector2.x));
    EXPECT_TRUE(IsFiniteValue(vector2.y));
    EXPECT_TRUE(IsFiniteValue(vector3.x));
    EXPECT_TRUE(IsFiniteValue(vector3.y));
    EXPECT_TRUE(IsFiniteValue(vector3.z));
    EXPECT_NEAR(scalar, 2.0f, 1.0e-5f);
    EXPECT_NEAR(vector2.x, 3.0f, 1.0e-5f);
    EXPECT_NEAR(vector2.y, 5.0f, 1.0e-5f);
    EXPECT_NEAR(vector3.x, 5.0f, 1.0e-5f);
    EXPECT_NEAR(vector3.y, 15.0f, 1.0e-5f);
    EXPECT_NEAR(vector3.z, 25.0f, 1.0e-5f);

    runner.Tick(1.0f);
    EXPECT_EQ(scalar, 8.0f);
    EXPECT_EQ(vector2.x, 4.0f);
    EXPECT_EQ(vector2.y, 6.0f);
    EXPECT_EQ(vector3.x, 10.0f);
    EXPECT_EQ(vector3.y, 20.0f);
    EXPECT_EQ(vector3.z, 30.0f);
    EXPECT_EQ(runner.ActiveCount(), 0u);
    EXPECT_FALSE(runner.IsActive(scalar_handle));
    EXPECT_FALSE(runner.IsActive(vector2_handle));
    EXPECT_FALSE(runner.IsActive(vector3_handle));
}

ACS_TEST(EasingTweenIntegration, InvalidEnumFallsBackToLinear) {
    const Easing::EEasingType invalid =
        static_cast<Easing::EEasingType>(0xffu);

    CTweenManager manager;
    f32 tween_value = 2.0f;
    const FTweenHandle tween_handle =
        manager.Tween(&tween_value, 2.0f, 10.0f, 2.0f, invalid);
    EXPECT_TRUE(tween_handle.IsValid());
    manager.Tick(0.5f);
    EXPECT_TRUE(IsFiniteValue(tween_value));
    EXPECT_NEAR(tween_value, 4.0f, 1.0e-5f);

    FSequence sequence;
    f32 sequence_value = 2.0f;
    sequence.Tween(
        &sequence_value, 2.0f, 10.0f, 2.0f, invalid);
    EXPECT_EQ(sequence.Actions().Num(), 1u);
    CSequenceRunner runner;
    const FSeqHandle sequence_handle =
        StartSequence(runner, sequence);
    EXPECT_TRUE(sequence_handle.IsValid());
    runner.Tick(0.5f);
    EXPECT_TRUE(IsFiniteValue(sequence_value));
    EXPECT_NEAR(sequence_value, 4.0f, 1.0e-5f);
}

ACS_TEST(EasingTweenIntegration, NonFiniteDurationPreservesState) {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();

    CTweenManager manager;
    f32 active_value = 0.0f;
    const FTweenHandle active_handle = manager.Tween(
        &active_value, 0.0f, 1.0f, 1.0f,
        Easing::EEasingType::Linear);
    f32 rejected_scalar = 10.0f;
    FVec2 rejected_vector2{20.0f, 30.0f};
    FVec3 rejected_vector3{40.0f, 50.0f, 60.0f};
    EXPECT_FALSE(manager.Tween(
        &rejected_scalar, 0.0f, 1.0f, nan,
        Easing::EEasingType::Linear).IsValid());
    EXPECT_FALSE(manager.Tween(
        &rejected_vector2, FVec2{}, FVec2{1.0f, 1.0f}, infinity,
        Easing::EEasingType::Linear).IsValid());
    EXPECT_FALSE(manager.Tween(
        &rejected_vector3, FVec3{}, FVec3{1.0f, 1.0f, 1.0f}, -infinity,
        Easing::EEasingType::Linear).IsValid());
    EXPECT_EQ(manager.ActiveCount(), 1u);
    EXPECT_TRUE(manager.IsActive(active_handle));
    EXPECT_EQ(active_value, 0.0f);
    EXPECT_EQ(rejected_scalar, 10.0f);
    EXPECT_EQ(rejected_vector2.x, 20.0f);
    EXPECT_EQ(rejected_vector2.y, 30.0f);
    EXPECT_EQ(rejected_vector3.x, 40.0f);
    EXPECT_EQ(rejected_vector3.y, 50.0f);
    EXPECT_EQ(rejected_vector3.z, 60.0f);

    FSequence sequence;
    sequence.Wait(1.0f);
    const usize original_action_count = sequence.Actions().Num();
    sequence.Tween(
        &rejected_scalar, 0.0f, 1.0f, nan,
        Easing::EEasingType::Linear);
    sequence.Tween(
        &rejected_vector2, FVec2{}, FVec2{1.0f, 1.0f}, infinity,
        Easing::EEasingType::Linear);
    sequence.Tween(
        &rejected_vector3, FVec3{}, FVec3{1.0f, 1.0f, 1.0f}, -infinity,
        Easing::EEasingType::Linear);
    EXPECT_EQ(sequence.Actions().Num(), original_action_count);
    EXPECT_EQ(rejected_scalar, 10.0f);
    EXPECT_EQ(rejected_vector2.x, 20.0f);
    EXPECT_EQ(rejected_vector3.x, 40.0f);
}

ACS_TEST(EasingTweenIntegration, NonFiniteDeltaTimeIsIgnored) {
    CTweenManager manager;
    f32 tween_value = 0.0f;
    const FTweenHandle tween_handle = manager.Tween(
        &tween_value, 0.0f, 1.0f, 1.0f,
        Easing::EEasingType::Linear);

    FSequence sequence;
    f32 sequence_value = 0.0f;
    sequence.Tween(
        &sequence_value, 0.0f, 1.0f, 1.0f,
        Easing::EEasingType::Linear);
    CSequenceRunner runner;
    const FSeqHandle sequence_handle =
        StartSequence(runner, sequence);

    const f32 invalid_deltas[] = {
        std::numeric_limits<f32>::quiet_NaN(),
        std::numeric_limits<f32>::infinity(),
        -std::numeric_limits<f32>::infinity(),
    };
    for (usize index = 0u;
         index < sizeof(invalid_deltas) / sizeof(invalid_deltas[0]); ++index) {
        manager.Tick(invalid_deltas[index]);
        runner.Tick(invalid_deltas[index]);
    }
    EXPECT_EQ(tween_value, 0.0f);
    EXPECT_EQ(sequence_value, 0.0f);
    EXPECT_TRUE(manager.IsActive(tween_handle));
    EXPECT_TRUE(runner.IsActive(sequence_handle));

    manager.Tick(0.25f);
    runner.Tick(0.25f);
    EXPECT_TRUE(IsFiniteValue(tween_value));
    EXPECT_TRUE(IsFiniteValue(sequence_value));
    EXPECT_NEAR(tween_value, 0.25f, 1.0e-6f);
    EXPECT_NEAR(sequence_value, 0.25f, 1.0e-6f);
}

ACS_TEST(EasingTweenIntegration, NonFiniteCustomEasingFallsBackToLinear) {
    CTweenManager manager;
    f32 tween_value = 2.0f;
    const FTweenHandle tween_handle = manager.Tween(
        &tween_value, 2.0f, 10.0f, 2.0f, &NonFiniteCustomEasing);
    EXPECT_TRUE(tween_handle.IsValid());

    FSequence sequence;
    f32 sequence_value = 2.0f;
    sequence.Tween(
        &sequence_value, 2.0f, 10.0f, 2.0f, &NonFiniteCustomEasing);
    CSequenceRunner runner;
    const FSeqHandle sequence_handle =
        StartSequence(runner, sequence);
    EXPECT_TRUE(sequence_handle.IsValid());

    manager.Tick(0.5f);
    runner.Tick(0.5f);
    EXPECT_TRUE(IsFiniteValue(tween_value));
    EXPECT_TRUE(IsFiniteValue(sequence_value));
    EXPECT_NEAR(tween_value, 4.0f, 1.0e-5f);
    EXPECT_NEAR(sequence_value, 4.0f, 1.0e-5f);
    EXPECT_TRUE(manager.IsActive(tween_handle));
    EXPECT_TRUE(runner.IsActive(sequence_handle));
}

ACS_TEST(EasingTweenIntegration, ElapsedOverflowCompletesExactly) {
    const f32 duration = std::numeric_limits<f32>::max();
    const f32 large_delta = duration * 0.75f;

    CTweenManager manager;
    f32 tween_value = 0.0f;
    const FTweenHandle tween_handle = manager.Tween(
        &tween_value, 0.0f, 1.0f, duration,
        Easing::EEasingType::Linear);

    FSequence sequence;
    f32 sequence_value = 0.0f;
    sequence.Tween(
        &sequence_value, 0.0f, 1.0f, duration,
        Easing::EEasingType::Linear);
    CSequenceRunner runner;
    const FSeqHandle sequence_handle =
        StartSequence(runner, sequence);

    manager.Tick(large_delta);
    runner.Tick(large_delta);
    EXPECT_TRUE(IsFiniteValue(tween_value));
    EXPECT_TRUE(IsFiniteValue(sequence_value));
    EXPECT_NEAR(tween_value, 0.75f, 1.0e-5f);
    EXPECT_NEAR(sequence_value, 0.75f, 1.0e-5f);
    EXPECT_TRUE(manager.IsActive(tween_handle));
    EXPECT_TRUE(runner.IsActive(sequence_handle));

    manager.Tick(large_delta);
    runner.Tick(large_delta);
    EXPECT_EQ(tween_value, 1.0f);
    EXPECT_EQ(sequence_value, 1.0f);
    EXPECT_FALSE(manager.IsActive(tween_handle));
    EXPECT_FALSE(runner.IsActive(sequence_handle));
    EXPECT_EQ(manager.ActiveCount(), 0u);
    EXPECT_EQ(runner.ActiveCount(), 0u);
}
