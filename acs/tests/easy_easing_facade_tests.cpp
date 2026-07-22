// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "easy/Easy.h"

#include <limits>

using namespace acs;

namespace Easy = acs::easy;
namespace GameEasing = acs::game::Easing;

namespace {

static_assert(
    static_cast<u32>(Easy::EEasingType::Count) == 33u,
    "The Easy facade must expose the complete 33-curve catalog");

bool StringsEqual(const char* lhs, const char* rhs) noexcept {
    if (lhs == nullptr || rhs == nullptr) {
        return lhs == rhs;
    }
    while (*lhs != '\0' && *rhs != '\0') {
        if (*lhs != *rhs) {
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return *lhs == *rhs;
}

bool IsFiniteValue(f32 value) noexcept {
    const f32 maximum = std::numeric_limits<f32>::max();
    return value == value && value >= -maximum && value <= maximum;
}

} // namespace

ACS_TEST(EasyEasingFacade, AllThirtyThreeCurvesMatchGameCatalog) {
    const f32 samples[] = {
        0.0f, 0.125f, 0.25f, 0.5f, 0.75f, 0.875f, 1.0f,
    };
    for (u32 value = 0u;
         value < static_cast<u32>(Easy::EEasingType::Count);
         ++value) {
        const Easy::EEasingType type =
            static_cast<Easy::EEasingType>(value);
        for (usize sample = 0u;
             sample < sizeof(samples) / sizeof(samples[0]); ++sample) {
            const f32 expected =
                GameEasing::Evaluate(type, samples[sample], -100.0f);
            const f32 facade_value =
                Easy::Ease(samples[sample], type);
            EXPECT_TRUE(IsFiniteValue(facade_value));
            EXPECT_NEAR(facade_value, expected, 1.0e-6f);

            f32 checked = -200.0f;
            const Easy::FEasingResult result =
                Easy::TryEase(samples[sample], type, checked);
            EXPECT_TRUE(result.Succeeded());
            EXPECT_TRUE(IsFiniteValue(checked));
            EXPECT_NEAR(checked, expected, 1.0e-6f);
        }
    }
}

ACS_TEST(EasyEasingFacade, EveryNameRoundTripsThroughFacade) {
    for (u32 value = 0u;
         value < static_cast<u32>(Easy::EEasingType::Count);
         ++value) {
        const Easy::EEasingType type =
            static_cast<Easy::EEasingType>(value);
        const char* name = Easy::EasingName(type);
        EXPECT_TRUE(StringsEqual(name, GameEasing::GetName(type)));

        Easy::EEasingType parsed = Easy::EEasingType::Count;
        EXPECT_TRUE(Easy::TryParseEasingName(name, parsed));
        EXPECT_EQ(parsed, type);
    }
}

ACS_TEST(EasyEasingFacade, FiniteOutsideInputClampsForEveryCurve) {
    for (u32 value = 0u;
         value < static_cast<u32>(Easy::EEasingType::Count);
         ++value) {
        const Easy::EEasingType type =
            static_cast<Easy::EEasingType>(value);
        EXPECT_EQ(Easy::Ease(-100.0f, type), 0.0f);
        EXPECT_EQ(Easy::Ease(100.0f, type), 1.0f);

        f32 before = 99.0f;
        f32 after = 99.0f;
        EXPECT_TRUE(Easy::TryEase(-100.0f, type, before).Succeeded());
        EXPECT_TRUE(Easy::TryEase(100.0f, type, after).Succeeded());
        EXPECT_EQ(before, 0.0f);
        EXPECT_EQ(after, 1.0f);
    }
}

ACS_TEST(EasyEasingFacade, NonFiniteInputPreservesOutputAndUsesFallback) {
    const f32 invalid_inputs[] = {
        std::numeric_limits<f32>::quiet_NaN(),
        std::numeric_limits<f32>::infinity(),
        -std::numeric_limits<f32>::infinity(),
    };
    for (usize index = 0u;
         index < sizeof(invalid_inputs) / sizeof(invalid_inputs[0]); ++index) {
        f32 output = 123.25f;
        const Easy::FEasingResult result = Easy::TryEase(
            invalid_inputs[index], Easy::EEasingType::OutCubic, output);
        EXPECT_FALSE(result.Succeeded());
        EXPECT_EQ(result.error, Easy::EEasingError::NonFiniteInput);
        EXPECT_EQ(output, 123.25f);
        EXPECT_EQ(
            Easy::Ease(
                invalid_inputs[index], Easy::EEasingType::OutCubic, 17.5f),
            17.5f);
    }
}

ACS_TEST(EasyEasingFacade, InvalidTypeAndNamePreserveOutputs) {
    const Easy::EEasingType invalid_types[] = {
        Easy::EEasingType::Count,
        static_cast<Easy::EEasingType>(0xffu),
    };
    for (usize index = 0u;
         index < sizeof(invalid_types) / sizeof(invalid_types[0]); ++index) {
        f32 output = 222.5f;
        const Easy::FEasingResult result =
            Easy::TryEase(0.5f, invalid_types[index], output);
        EXPECT_FALSE(result.Succeeded());
        EXPECT_EQ(result.error, Easy::EEasingError::InvalidType);
        EXPECT_EQ(output, 222.5f);
        EXPECT_EQ(
            Easy::Ease(0.5f, invalid_types[index], 42.25f),
            42.25f);
        EXPECT_TRUE(
            StringsEqual(Easy::EasingName(invalid_types[index]), "Invalid"));
    }

    Easy::EEasingType parsed = Easy::EEasingType::OutElastic;
    EXPECT_FALSE(Easy::TryParseEasingName(nullptr, parsed));
    EXPECT_EQ(parsed, Easy::EEasingType::OutElastic);
    EXPECT_FALSE(Easy::TryParseEasingName("", parsed));
    EXPECT_EQ(parsed, Easy::EEasingType::OutElastic);
    EXPECT_FALSE(Easy::TryParseEasingName("NotAnEasing", parsed));
    EXPECT_EQ(parsed, Easy::EEasingType::OutElastic);
}

ACS_TEST(EasyEasingFacade, CheckedNameFacadePreservesDiagnostics) {
    for (u32 value = 0u;
         value < static_cast<u32>(Easy::EEasingType::Count);
         ++value) {
        const Easy::EEasingType type =
            static_cast<Easy::EEasingType>(value);
        const char* name = "sentinel";
        const Easy::FEasingResult name_result =
            Easy::TryGetEasingName(type, name);
        EXPECT_TRUE(name_result.Succeeded());
        EXPECT_TRUE(StringsEqual(name, GameEasing::GetName(type)));

        Easy::EEasingType parsed = Easy::EEasingType::Count;
        const Easy::FEasingResult parse_result =
            Easy::TryParseEasingNameChecked(name, parsed);
        EXPECT_TRUE(parse_result.Succeeded());
        EXPECT_EQ(parsed, type);
    }

    const char* unchanged_name = "sentinel";
    const Easy::FEasingResult invalid_name_result =
        Easy::TryGetEasingName(Easy::EEasingType::Count, unchanged_name);
    EXPECT_EQ(invalid_name_result.error, Easy::EEasingError::InvalidType);
    EXPECT_TRUE(StringsEqual(unchanged_name, "sentinel"));

    Easy::EEasingType unchanged_type = Easy::EEasingType::OutBounce;
    const Easy::FEasingResult null_result =
        Easy::TryParseEasingNameChecked(nullptr, unchanged_type);
    EXPECT_EQ(null_result.error, Easy::EEasingError::NullName);
    EXPECT_EQ(unchanged_type, Easy::EEasingType::OutBounce);

    const Easy::FEasingResult unknown_result =
        Easy::TryParseEasingNameChecked("unknown", unchanged_type);
    EXPECT_EQ(unknown_result.error, Easy::EEasingError::UnknownName);
    EXPECT_EQ(unchanged_type, Easy::EEasingType::OutBounce);
}

ACS_TEST(EasyEasingFacade, SamplingFacadeMatchesGameCatalog) {
    constexpr usize sample_count = 7u;
    f32 facade_samples[sample_count]{};
    f32 game_samples[sample_count]{};

    for (u32 value = 0u;
         value < static_cast<u32>(Easy::EEasingType::Count);
         ++value) {
        const Easy::EEasingType type =
            static_cast<Easy::EEasingType>(value);
        EXPECT_TRUE(Easy::TrySampleEasing(
            type, facade_samples, sample_count).Succeeded());
        EXPECT_TRUE(GameEasing::TrySampleCurve(
            type, game_samples, sample_count).Succeeded());
        for (usize index = 0u; index < sample_count; ++index) {
            EXPECT_EQ(facade_samples[index], game_samples[index]);
        }
    }

    f32 unchanged[] = {3.0f, 4.0f};
    const Easy::FEasingResult invalid_count = Easy::TrySampleEasing(
        Easy::EEasingType::Linear, unchanged, 1u);
    EXPECT_EQ(
        invalid_count.error, Easy::EEasingError::InvalidSampleCount);
    EXPECT_EQ(unchanged[0], 3.0f);
    EXPECT_EQ(unchanged[1], 4.0f);
}

ACS_TEST(EasyEasingFacade, LegacyShortcutsKeepCatalogValues) {
    const f32 samples[] = {
        -1.0f, 0.0f, 0.125f, 0.25f, 0.5f, 0.75f, 1.0f, 2.0f,
    };
    for (usize index = 0u;
         index < sizeof(samples) / sizeof(samples[0]); ++index) {
        const f32 t = samples[index];
        const f32 in = Easy::EaseIn(t);
        const f32 out = Easy::EaseOut(t);
        const f32 in_out = Easy::EaseInOut(t);
        const f32 back = Easy::EaseOutBack(t);
        const f32 bounce = Easy::EaseOutBounce(t);
        const f32 elastic = Easy::EaseOutElastic(t);
        EXPECT_TRUE(IsFiniteValue(in));
        EXPECT_TRUE(IsFiniteValue(out));
        EXPECT_TRUE(IsFiniteValue(in_out));
        EXPECT_TRUE(IsFiniteValue(back));
        EXPECT_TRUE(IsFiniteValue(bounce));
        EXPECT_TRUE(IsFiniteValue(elastic));
        EXPECT_NEAR(
            in,
            GameEasing::Evaluate(Easy::EEasingType::InQuad, t),
            1.0e-6f);
        EXPECT_NEAR(
            out,
            GameEasing::Evaluate(Easy::EEasingType::OutQuad, t),
            1.0e-6f);
        EXPECT_NEAR(
            in_out,
            GameEasing::Evaluate(Easy::EEasingType::InOutQuad, t),
            1.0e-6f);
        EXPECT_NEAR(
            back,
            GameEasing::Evaluate(Easy::EEasingType::OutBack, t),
            1.0e-6f);
        EXPECT_NEAR(
            bounce,
            GameEasing::Evaluate(Easy::EEasingType::OutBounce, t),
            1.0e-6f);
        EXPECT_NEAR(
            elastic,
            GameEasing::Evaluate(Easy::EEasingType::OutElastic, t),
            1.0e-6f);
    }
}
