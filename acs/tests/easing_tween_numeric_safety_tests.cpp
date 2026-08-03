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

FSeqHandle StartSequence(
    CSequenceRunner& runner, FSequence& sequence) noexcept {
    return runner.Start(Move(sequence));
}

} // namespace

ACS_TEST(EasingTweenNumericSafety, NonFiniteEndpointsPreserveStateForEveryValueType) {
    const f32 invalid_values[] = {
        std::numeric_limits<f32>::quiet_NaN(),
        std::numeric_limits<f32>::infinity(),
        -std::numeric_limits<f32>::infinity(),
    };

    CTweenManager manager;
    f32 active_target = 0.0f;
    const FTweenHandle active_handle = manager.Tween(
        &active_target, 0.0f, 1.0f, 1.0f,
        Easing::EEasingType::Linear);
    EXPECT_TRUE(active_handle.IsValid());

    f32 scalar_target = 11.0f;
    FVec2 vector2_target{21.0f, 22.0f};
    FVec3 vector3_target{31.0f, 32.0f, 33.0f};
    for (usize index = 0u;
         index < sizeof(invalid_values) / sizeof(invalid_values[0]); ++index) {
        const f32 invalid = invalid_values[index];
        EXPECT_FALSE(manager.Tween(
            &scalar_target, invalid, 1.0f, 1.0f,
            Easing::EEasingType::Linear).IsValid());
        EXPECT_FALSE(manager.Tween(
            &scalar_target, 0.0f, invalid, 1.0f,
            Easing::EEasingType::Linear).IsValid());
        EXPECT_FALSE(manager.Tween(
            &vector2_target, FVec2{invalid, 0.0f}, FVec2{1.0f, 2.0f}, 1.0f,
            Easing::EEasingType::Linear).IsValid());
        EXPECT_FALSE(manager.Tween(
            &vector2_target, FVec2{0.0f, 1.0f}, FVec2{2.0f, invalid}, 1.0f,
            Easing::EEasingType::Linear).IsValid());
        EXPECT_FALSE(manager.Tween(
            &vector3_target, FVec3{0.0f, invalid, 2.0f},
            FVec3{3.0f, 4.0f, 5.0f}, 1.0f,
            Easing::EEasingType::Linear).IsValid());
        EXPECT_FALSE(manager.Tween(
            &vector3_target, FVec3{0.0f, 1.0f, 2.0f},
            FVec3{3.0f, 4.0f, invalid}, 1.0f,
            Easing::EEasingType::Linear).IsValid());
    }
    EXPECT_EQ(manager.ActiveCount(), 1u);
    EXPECT_TRUE(manager.IsActive(active_handle));
    EXPECT_EQ(active_target, 0.0f);
    EXPECT_EQ(scalar_target, 11.0f);
    EXPECT_EQ(vector2_target.x, 21.0f);
    EXPECT_EQ(vector2_target.y, 22.0f);
    EXPECT_EQ(vector3_target.x, 31.0f);
    EXPECT_EQ(vector3_target.y, 32.0f);
    EXPECT_EQ(vector3_target.z, 33.0f);

    FSequence sequence;
    sequence.Wait(1.0f);
    const usize original_action_count = sequence.Actions().Num();
    for (usize index = 0u;
         index < sizeof(invalid_values) / sizeof(invalid_values[0]); ++index) {
        const f32 invalid = invalid_values[index];
        sequence.Tween(
            &scalar_target, invalid, 1.0f, 1.0f,
            Easing::EEasingType::Linear);
        sequence.Tween(
            &scalar_target, 0.0f, invalid, 1.0f,
            Easing::EEasingType::Linear);
        sequence.Tween(
            &vector2_target, FVec2{invalid, 0.0f}, FVec2{1.0f, 2.0f}, 1.0f,
            Easing::EEasingType::Linear);
        sequence.Tween(
            &vector2_target, FVec2{0.0f, 1.0f}, FVec2{2.0f, invalid}, 1.0f,
            Easing::EEasingType::Linear);
        sequence.Tween(
            &vector3_target, FVec3{0.0f, invalid, 2.0f},
            FVec3{3.0f, 4.0f, 5.0f}, 1.0f,
            Easing::EEasingType::Linear);
        sequence.Tween(
            &vector3_target, FVec3{0.0f, 1.0f, 2.0f},
            FVec3{3.0f, 4.0f, invalid}, 1.0f,
            Easing::EEasingType::Linear);
    }
    EXPECT_EQ(sequence.Actions().Num(), original_action_count);
    EXPECT_EQ(scalar_target, 11.0f);
    EXPECT_EQ(vector2_target.x, 21.0f);
    EXPECT_EQ(vector2_target.y, 22.0f);
    EXPECT_EQ(vector3_target.x, 31.0f);
    EXPECT_EQ(vector3_target.y, 32.0f);
    EXPECT_EQ(vector3_target.z, 33.0f);
}

ACS_TEST(EasingTweenNumericSafety, InvalidEndpointsCannotUseImmediateCompletion) {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();

    CTweenManager manager;
    f32 scalar_target = 10.0f;
    FVec2 vector2_target{20.0f, 21.0f};
    FVec3 vector3_target{30.0f, 31.0f, 32.0f};
    EXPECT_FALSE(manager.Tween(
        &scalar_target, nan, 1.0f, 0.0f,
        Easing::EEasingType::Linear).IsValid());
    EXPECT_FALSE(manager.Tween(
        &scalar_target, 0.0f, infinity, -1.0f,
        Easing::EEasingType::Linear).IsValid());
    EXPECT_FALSE(manager.Tween(
        &vector2_target, FVec2{0.0f, nan}, FVec2{1.0f, 2.0f}, 0.0f,
        Easing::EEasingType::Linear).IsValid());
    EXPECT_FALSE(manager.Tween(
        &vector2_target, FVec2{0.0f, 1.0f}, FVec2{-infinity, 2.0f}, -1.0f,
        Easing::EEasingType::Linear).IsValid());
    EXPECT_FALSE(manager.Tween(
        &vector3_target, FVec3{0.0f, infinity, 2.0f},
        FVec3{3.0f, 4.0f, 5.0f}, 0.0f,
        Easing::EEasingType::Linear).IsValid());
    EXPECT_FALSE(manager.Tween(
        &vector3_target, FVec3{0.0f, 1.0f, 2.0f},
        FVec3{3.0f, nan, 5.0f}, -1.0f,
        Easing::EEasingType::Linear).IsValid());
    EXPECT_EQ(manager.ActiveCount(), 0u);
    EXPECT_EQ(scalar_target, 10.0f);
    EXPECT_EQ(vector2_target.x, 20.0f);
    EXPECT_EQ(vector2_target.y, 21.0f);
    EXPECT_EQ(vector3_target.x, 30.0f);
    EXPECT_EQ(vector3_target.y, 31.0f);
    EXPECT_EQ(vector3_target.z, 32.0f);

    FSequence sequence;
    sequence.Tween(
        &scalar_target, nan, 1.0f, 0.0f,
        Easing::EEasingType::Linear);
    sequence.Tween(
        &scalar_target, 0.0f, infinity, -1.0f,
        Easing::EEasingType::Linear);
    sequence.Tween(
        &vector2_target, FVec2{0.0f, nan}, FVec2{1.0f, 2.0f}, 0.0f,
        Easing::EEasingType::Linear);
    sequence.Tween(
        &vector2_target, FVec2{0.0f, 1.0f}, FVec2{-infinity, 2.0f}, -1.0f,
        Easing::EEasingType::Linear);
    sequence.Tween(
        &vector3_target, FVec3{0.0f, infinity, 2.0f},
        FVec3{3.0f, 4.0f, 5.0f}, 0.0f,
        Easing::EEasingType::Linear);
    sequence.Tween(
        &vector3_target, FVec3{0.0f, 1.0f, 2.0f},
        FVec3{3.0f, nan, 5.0f}, -1.0f,
        Easing::EEasingType::Linear);
    EXPECT_EQ(sequence.Actions().Num(), 0u);
    EXPECT_EQ(scalar_target, 10.0f);
    EXPECT_EQ(vector2_target.x, 20.0f);
    EXPECT_EQ(vector2_target.y, 21.0f);
    EXPECT_EQ(vector3_target.x, 30.0f);
    EXPECT_EQ(vector3_target.y, 31.0f);
    EXPECT_EQ(vector3_target.z, 32.0f);
}

ACS_TEST(EasingTweenNumericSafety, OverflowingIntermediateValuesCommitAtomicallyAtFinish) {
    const f32 maximum = std::numeric_limits<f32>::max();
    const FVec2 vector2_from{0.0f, maximum};
    const FVec2 vector2_to{2.0f, -maximum};
    const FVec3 vector3_from{0.0f, maximum, 4.0f};
    const FVec3 vector3_to{2.0f, -maximum, 8.0f};

    CTweenManager manager;
    f32 scalar_target = 10.0f;
    FVec2 vector2_target{20.0f, 21.0f};
    FVec3 vector3_target{30.0f, 31.0f, 32.0f};
    const FTweenHandle scalar_handle = manager.Tween(
        &scalar_target, maximum, -maximum, 2.0f,
        Easing::EEasingType::Linear);
    const FTweenHandle vector2_handle = manager.Tween(
        &vector2_target, vector2_from, vector2_to, 2.0f,
        Easing::EEasingType::Linear);
    const FTweenHandle vector3_handle = manager.Tween(
        &vector3_target, vector3_from, vector3_to, 2.0f,
        Easing::EEasingType::Linear);
    EXPECT_TRUE(scalar_handle.IsValid());
    EXPECT_TRUE(vector2_handle.IsValid());
    EXPECT_TRUE(vector3_handle.IsValid());

    CSequenceRunner runner;
    f32 sequence_scalar_target = 40.0f;
    FVec2 sequence_vector2_target{50.0f, 51.0f};
    FVec3 sequence_vector3_target{60.0f, 61.0f, 62.0f};
    FSequence scalar_sequence;
    scalar_sequence.Tween(
        &sequence_scalar_target, maximum, -maximum, 2.0f,
        Easing::EEasingType::Linear);
    FSequence vector2_sequence;
    vector2_sequence.Tween(
        &sequence_vector2_target, vector2_from, vector2_to, 2.0f,
        Easing::EEasingType::Linear);
    FSequence vector3_sequence;
    vector3_sequence.Tween(
        &sequence_vector3_target, vector3_from, vector3_to, 2.0f,
        Easing::EEasingType::Linear);
    const FSeqHandle sequence_scalar_handle =
        StartSequence(runner, scalar_sequence);
    const FSeqHandle sequence_vector2_handle =
        StartSequence(runner, vector2_sequence);
    const FSeqHandle sequence_vector3_handle =
        StartSequence(runner, vector3_sequence);
    EXPECT_TRUE(sequence_scalar_handle.IsValid());
    EXPECT_TRUE(sequence_vector2_handle.IsValid());
    EXPECT_TRUE(sequence_vector3_handle.IsValid());

    manager.Tick(1.0f);
    runner.Tick(1.0f);
    EXPECT_EQ(scalar_target, 10.0f);
    EXPECT_EQ(vector2_target.x, 20.0f);
    EXPECT_EQ(vector2_target.y, 21.0f);
    EXPECT_EQ(vector3_target.x, 30.0f);
    EXPECT_EQ(vector3_target.y, 31.0f);
    EXPECT_EQ(vector3_target.z, 32.0f);
    EXPECT_EQ(sequence_scalar_target, 40.0f);
    EXPECT_EQ(sequence_vector2_target.x, 50.0f);
    EXPECT_EQ(sequence_vector2_target.y, 51.0f);
    EXPECT_EQ(sequence_vector3_target.x, 60.0f);
    EXPECT_EQ(sequence_vector3_target.y, 61.0f);
    EXPECT_EQ(sequence_vector3_target.z, 62.0f);
    EXPECT_EQ(manager.ActiveCount(), 3u);
    EXPECT_EQ(runner.ActiveCount(), 3u);

    manager.Tick(1.0f);
    runner.Tick(1.0f);
    EXPECT_EQ(scalar_target, -maximum);
    EXPECT_EQ(vector2_target.x, vector2_to.x);
    EXPECT_EQ(vector2_target.y, vector2_to.y);
    EXPECT_EQ(vector3_target.x, vector3_to.x);
    EXPECT_EQ(vector3_target.y, vector3_to.y);
    EXPECT_EQ(vector3_target.z, vector3_to.z);
    EXPECT_EQ(sequence_scalar_target, -maximum);
    EXPECT_EQ(sequence_vector2_target.x, vector2_to.x);
    EXPECT_EQ(sequence_vector2_target.y, vector2_to.y);
    EXPECT_EQ(sequence_vector3_target.x, vector3_to.x);
    EXPECT_EQ(sequence_vector3_target.y, vector3_to.y);
    EXPECT_EQ(sequence_vector3_target.z, vector3_to.z);
    EXPECT_EQ(manager.ActiveCount(), 0u);
    EXPECT_EQ(runner.ActiveCount(), 0u);
    EXPECT_FALSE(manager.IsActive(scalar_handle));
    EXPECT_FALSE(manager.IsActive(vector2_handle));
    EXPECT_FALSE(manager.IsActive(vector3_handle));
    EXPECT_FALSE(runner.IsActive(sequence_scalar_handle));
    EXPECT_FALSE(runner.IsActive(sequence_vector2_handle));
    EXPECT_FALSE(runner.IsActive(sequence_vector3_handle));
}
