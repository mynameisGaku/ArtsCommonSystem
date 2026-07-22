// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/AnimationCurve.h"
#include "gameframework/Easing.h"
#include "memory/SystemAllocator.h"

#include <limits>

using namespace acs;
using namespace acs::game;

namespace {

class FSwitchableCurveAllocator final : public FAllocator {
public:
    explicit FSwitchableCurveAllocator(FAllocator& backing) noexcept
        : m_Backing(&backing) {}

    void SetFailing(bool failing) noexcept { m_Failing = failing; }

    void* Alloc(
        usize size, usize alignment,
        FSourceLoc location) noexcept override {
        return m_Failing
            ? nullptr
            : m_Backing->Alloc(size, alignment, location);
    }

    void Free(void* pointer) noexcept override {
        m_Backing->Free(pointer);
    }

private:
    FAllocator* m_Backing = nullptr;
    bool m_Failing = false;
};

FCurveKey MakeLinearKey(f32 time, f32 value) noexcept {
    FCurveKey key{};
    key.time = time;
    key.value = value;
    key.in_interp = ECurveInterpolation::Linear;
    key.out_interp = ECurveInterpolation::Linear;
    return key;
}

} // namespace

ACS_TEST(AnimationCurveSafety, CheckedAddSortsAndUpdatesDuplicates) {
    FAnimationCurve curve;
    EXPECT_TRUE(curve.TryAddKey(2.0f, 20.0f).Succeeded());
    EXPECT_TRUE(curve.TryAddKey(0.0f, 0.0f).Succeeded());
    EXPECT_TRUE(curve.TryAddKey(1.0f, 10.0f).Succeeded());

    EXPECT_EQ(curve.KeyCount(), 3u);
    EXPECT_NEAR(curve.Key(0)->time, 0.0f, 1e-6f);
    EXPECT_NEAR(curve.Key(1)->time, 1.0f, 1e-6f);
    EXPECT_NEAR(curve.Key(2)->time, 2.0f, 1e-6f);

    const FAnimationCurveResult updated = curve.TryAddKey(
        1.0f, 15.0f, ECurveInterpolation::Step);
    EXPECT_TRUE(updated.Succeeded());
    EXPECT_EQ(updated.key_index, 1u);
    EXPECT_EQ(curve.KeyCount(), 3u);
    EXPECT_NEAR(curve.Key(1)->value, 15.0f, 1e-6f);
    EXPECT_EQ(curve.Key(1)->out_interp, ECurveInterpolation::Step);
}

ACS_TEST(AnimationCurveSafety, BulkImportIsTransactional) {
    FAnimationCurve curve;
    FCurveKey initial[2] = {
        MakeLinearKey(0.0f, 1.0f),
        MakeLinearKey(2.0f, 5.0f),
    };
    EXPECT_TRUE(curve.TrySetKeys(
        initial, 2u, FAnimationCurve::EWrapMode::Loop,
        FAnimationCurve::EWrapMode::PingPong).Succeeded());

    FCurveKey duplicate[2] = {
        MakeLinearKey(3.0f, 30.0f),
        MakeLinearKey(3.0f, 40.0f),
    };
    FAnimationCurveResult result = curve.TrySetKeys(duplicate, 2u);
    EXPECT_EQ(result.error, EAnimationCurveError::DuplicateKeyTime);

    FCurveKey non_finite[1] = {MakeLinearKey(
        std::numeric_limits<f32>::quiet_NaN(), 9.0f)};
    result = curve.TrySetKeys(non_finite, 1u);
    EXPECT_EQ(result.error, EAnimationCurveError::NonFiniteValue);

    FCurveKey invalid_enum[1] = {MakeLinearKey(4.0f, 9.0f)};
    invalid_enum[0].out_interp = static_cast<ECurveInterpolation>(99u);
    result = curve.TrySetKeys(invalid_enum, 1u);
    EXPECT_EQ(result.error, EAnimationCurveError::InvalidInterpolation);

    EXPECT_EQ(curve.KeyCount(), 2u);
    EXPECT_NEAR(curve.Key(0)->value, 1.0f, 1e-6f);
    EXPECT_NEAR(curve.Key(1)->value, 5.0f, 1e-6f);
    EXPECT_EQ(curve.PreWrap(), FAnimationCurve::EWrapMode::Loop);
    EXPECT_EQ(curve.PostWrap(), FAnimationCurve::EWrapMode::PingPong);
}

ACS_TEST(AnimationCurveSafety, AllocationFailurePreservesExistingCurve) {
    FSystemAllocator backing;
    FSwitchableCurveAllocator allocator(backing);
    FAnimationCurve curve(allocator);
    EXPECT_TRUE(curve.TryAddKey(0.0f, 7.0f).Succeeded());

    FCurveKey replacement[2] = {
        MakeLinearKey(0.0f, 100.0f),
        MakeLinearKey(1.0f, 200.0f),
    };
    allocator.SetFailing(true);
    const FAnimationCurveResult result =
        curve.TrySetKeys(replacement, 2u);
    EXPECT_EQ(result.error, EAnimationCurveError::AllocationFailure);
    EXPECT_EQ(curve.KeyCount(), 1u);
    EXPECT_NEAR(curve.Key(0)->value, 7.0f, 1e-6f);
}

ACS_TEST(AnimationCurveSafety, CheckedEvaluationPreservesOutputOnFailure) {
    FAnimationCurve curve;
    FCurveKey keys[2] = {
        MakeLinearKey(0.0f, -std::numeric_limits<f32>::max()),
        MakeLinearKey(1.0f, std::numeric_limits<f32>::max()),
    };
    EXPECT_TRUE(curve.TrySetKeys(keys, 2u).Succeeded());

    f32 output = 123.0f;
    FAnimationCurveResult result = curve.TryEvaluate(
        std::numeric_limits<f32>::quiet_NaN(), output);
    EXPECT_EQ(result.error, EAnimationCurveError::NonFiniteValue);
    EXPECT_NEAR(output, 123.0f, 1e-6f);

    result = curve.TryEvaluate(0.5f, output);
    EXPECT_EQ(result.error, EAnimationCurveError::ResultOutOfRange);
    EXPECT_NEAR(output, 123.0f, 1e-6f);
    EXPECT_NEAR(
        curve.Evaluate(std::numeric_limits<f32>::infinity()),
        0.0f, 1e-6f);
}

ACS_TEST(AnimationCurveSafety, WrapAndSegmentOverflowCannotBypassStepChecks) {
    FAnimationCurve curve;
    FCurveKey wrapped_keys[2] = {
        MakeLinearKey(2.0e38f, 10.0f),
        MakeLinearKey(3.0e38f, 20.0f),
    };
    wrapped_keys[0].out_interp = ECurveInterpolation::Step;
    EXPECT_TRUE(curve.TrySetKeys(
        wrapped_keys, 2u, FAnimationCurve::EWrapMode::Loop,
        FAnimationCurve::EWrapMode::Clamp).Succeeded());

    f32 output = 77.0f;
    FAnimationCurveResult result = curve.TryEvaluate(-3.0e38f, output);
    EXPECT_EQ(result.error, EAnimationCurveError::ResultOutOfRange);
    EXPECT_NEAR(output, 77.0f, 1e-6f);

    FCurveKey wide_keys[2] = {
        MakeLinearKey(-std::numeric_limits<f32>::max(), 1.0f),
        MakeLinearKey(std::numeric_limits<f32>::max(), 2.0f),
    };
    wide_keys[0].out_interp = ECurveInterpolation::Step;
    EXPECT_TRUE(curve.TrySetKeys(wide_keys, 2u).Succeeded());
    result = curve.TryEvaluate(0.0f, output);
    EXPECT_EQ(result.error, EAnimationCurveError::ResultOutOfRange);
    EXPECT_NEAR(output, 77.0f, 1e-6f);
}

ACS_TEST(AnimationCurveSafety, InvalidLegacyAddsAreSafeNoOps) {
    FAnimationCurve curve;
    curve.AddKey(0.0f, 1.0f);
    curve.SetPreWrap(FAnimationCurve::EWrapMode::Loop);
    curve.SetPostWrap(FAnimationCurve::EWrapMode::PingPong);
    curve.AddKey(
        std::numeric_limits<f32>::quiet_NaN(), 2.0f);
    curve.AddKey(
        1.0f, 2.0f, static_cast<ECurveInterpolation>(200u));
    curve.AddKeyHermite(
        2.0f, 3.0f, std::numeric_limits<f32>::infinity(), 0.0f);
    curve.SetPreWrap(static_cast<FAnimationCurve::EWrapMode>(99u));
    curve.SetPostWrap(static_cast<FAnimationCurve::EWrapMode>(99u));

    EXPECT_EQ(curve.KeyCount(), 1u);
    EXPECT_NEAR(curve.Evaluate(0.0f), 1.0f, 1e-6f);
    EXPECT_EQ(curve.PreWrap(), FAnimationCurve::EWrapMode::Loop);
    EXPECT_EQ(curve.PostWrap(), FAnimationCurve::EWrapMode::PingPong);
    EXPECT_TRUE(
        FAnimationCurveResult::ErrorName(
            EAnimationCurveError::AllocationFailure)[0] == 'A');
}

ACS_TEST(AnimationCurveSafety, EveryEasingPresetBuildsAnEditableCurve) {
    FAnimationCurve curve;
    const u32 type_count =
        static_cast<u32>(Easing::EEasingType::Count);
    EXPECT_EQ(type_count, 33u);

    for (u32 type_index = 0u; type_index < type_count; ++type_index) {
        const Easing::EEasingType type =
            static_cast<Easing::EEasingType>(type_index);
        const FAnimationCurveResult result =
            curve.TrySetEasingPreset(type, 65u);
        EXPECT_TRUE(result.Succeeded());
        EXPECT_EQ(curve.KeyCount(), 65u);
        EXPECT_EQ(curve.PreWrap(), FAnimationCurve::EWrapMode::Clamp);
        EXPECT_EQ(curve.PostWrap(), FAnimationCurve::EWrapMode::Clamp);
        EXPECT_NEAR(curve.Key(0u)->time, 0.0f, 1e-6f);
        EXPECT_NEAR(curve.Key(0u)->value, 0.0f, 1e-6f);
        EXPECT_NEAR(curve.Key(64u)->time, 1.0f, 1e-6f);
        EXPECT_NEAR(curve.Key(64u)->value, 1.0f, 1e-6f);

        for (u32 key_index = 0u; key_index < curve.KeyCount();
             ++key_index) {
            const FCurveKey* key = curve.Key(key_index);
            EXPECT_TRUE(key != nullptr);
            if (key == nullptr) continue;
            EXPECT_TRUE(std::isfinite(key->time));
            EXPECT_TRUE(std::isfinite(key->value));
            EXPECT_EQ(key->out_interp, ECurveInterpolation::Linear);
            if (key_index != 0u) {
                const FCurveKey* previous = curve.Key(key_index - 1u);
                EXPECT_TRUE(previous != nullptr);
                if (previous != nullptr) {
                    EXPECT_TRUE(previous->time < key->time);
                }
            }
        }
    }
}

ACS_TEST(AnimationCurveSafety, InvalidEasingPresetPreservesCurveAndWrap) {
    FAnimationCurve curve;
    EXPECT_TRUE(curve.TryAddKey(0.0f, 7.0f).Succeeded());
    EXPECT_TRUE(curve.TrySetWrapModes(
        FAnimationCurve::EWrapMode::Loop,
        FAnimationCurve::EWrapMode::PingPong).Succeeded());

    FAnimationCurveResult result = curve.TrySetEasingPreset(
        static_cast<Easing::EEasingType>(255u), 65u);
    EXPECT_EQ(result.error, EAnimationCurveError::InvalidEasingType);
    result = curve.TrySetEasingPreset(
        Easing::EEasingType::OutBounce, 1u);
    EXPECT_EQ(result.error, EAnimationCurveError::InvalidSampleCount);
    result = curve.TrySetEasingPreset(
        Easing::EEasingType::OutBounce,
        FAnimationCurve::kMaxEasingPresetSamples + 1u);
    EXPECT_EQ(result.error, EAnimationCurveError::InvalidSampleCount);

    EXPECT_EQ(curve.KeyCount(), 1u);
    EXPECT_NEAR(curve.Key(0u)->value, 7.0f, 1e-6f);
    EXPECT_EQ(curve.PreWrap(), FAnimationCurve::EWrapMode::Loop);
    EXPECT_EQ(curve.PostWrap(), FAnimationCurve::EWrapMode::PingPong);
}

ACS_TEST(AnimationCurveSafety, EasingPresetAllocationFailureIsTransactional) {
    FSystemAllocator backing;
    FSwitchableCurveAllocator allocator(backing);
    FAnimationCurve curve(allocator);
    EXPECT_TRUE(curve.TryAddKey(0.0f, 9.0f).Succeeded());
    EXPECT_TRUE(curve.TrySetWrapModes(
        FAnimationCurve::EWrapMode::Loop,
        FAnimationCurve::EWrapMode::Loop).Succeeded());

    allocator.SetFailing(true);
    const FAnimationCurveResult result = curve.TrySetEasingPreset(
        Easing::EEasingType::InOutElastic, 65u);
    EXPECT_EQ(result.error, EAnimationCurveError::AllocationFailure);
    EXPECT_EQ(curve.KeyCount(), 1u);
    EXPECT_NEAR(curve.Key(0u)->value, 9.0f, 1e-6f);
    EXPECT_EQ(curve.PreWrap(), FAnimationCurve::EWrapMode::Loop);
    EXPECT_EQ(curve.PostWrap(), FAnimationCurve::EWrapMode::Loop);
}
