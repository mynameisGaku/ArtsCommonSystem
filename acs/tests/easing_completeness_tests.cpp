// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Easing.h"

#include <limits>

using namespace acs;
using namespace acs::game;

namespace {

struct FEasingCase {
    Easing::EEasingType type;
    Easing::EasingFn function;
    const char* name;
    bool monotonic;
    bool overshoots;
};

struct FEasingGoldenCase {
    Easing::EEasingType type;
    f32 t;
    f32 expected;
};

constexpr FEasingCase kCases[] = {
    {Easing::EEasingType::Linear,       Easing::Linear,       "Linear",       true,  false},
    {Easing::EEasingType::InQuad,       Easing::InQuad,       "InQuad",       true,  false},
    {Easing::EEasingType::OutQuad,      Easing::OutQuad,      "OutQuad",      true,  false},
    {Easing::EEasingType::InOutQuad,    Easing::InOutQuad,    "InOutQuad",    true,  false},
    {Easing::EEasingType::InCubic,      Easing::InCubic,      "InCubic",      true,  false},
    {Easing::EEasingType::OutCubic,     Easing::OutCubic,     "OutCubic",     true,  false},
    {Easing::EEasingType::InOutCubic,   Easing::InOutCubic,   "InOutCubic",   true,  false},
    {Easing::EEasingType::InQuart,      Easing::InQuart,      "InQuart",      true,  false},
    {Easing::EEasingType::OutQuart,     Easing::OutQuart,     "OutQuart",     true,  false},
    {Easing::EEasingType::InOutQuart,   Easing::InOutQuart,   "InOutQuart",   true,  false},
    {Easing::EEasingType::InQuint,      Easing::InQuint,      "InQuint",      true,  false},
    {Easing::EEasingType::OutQuint,     Easing::OutQuint,     "OutQuint",     true,  false},
    {Easing::EEasingType::InOutQuint,   Easing::InOutQuint,   "InOutQuint",   true,  false},
    {Easing::EEasingType::InSine,       Easing::InSine,       "InSine",       true,  false},
    {Easing::EEasingType::OutSine,      Easing::OutSine,      "OutSine",      true,  false},
    {Easing::EEasingType::InOutSine,    Easing::InOutSine,    "InOutSine",    true,  false},
    {Easing::EEasingType::InExpo,       Easing::InExpo,       "InExpo",       true,  false},
    {Easing::EEasingType::OutExpo,      Easing::OutExpo,      "OutExpo",      true,  false},
    {Easing::EEasingType::InOutExpo,    Easing::InOutExpo,    "InOutExpo",    true,  false},
    {Easing::EEasingType::InCirc,       Easing::InCirc,       "InCirc",       true,  false},
    {Easing::EEasingType::OutCirc,      Easing::OutCirc,      "OutCirc",      true,  false},
    {Easing::EEasingType::InOutCirc,    Easing::InOutCirc,    "InOutCirc",    true,  false},
    {Easing::EEasingType::InBack,       Easing::InBack,       "InBack",       false, true},
    {Easing::EEasingType::OutBack,      Easing::OutBack,      "OutBack",      false, true},
    {Easing::EEasingType::InOutBack,    Easing::InOutBack,    "InOutBack",    false, true},
    {Easing::EEasingType::InElastic,    Easing::InElastic,    "InElastic",    false, true},
    {Easing::EEasingType::OutElastic,   Easing::OutElastic,   "OutElastic",   false, true},
    {Easing::EEasingType::InOutElastic, Easing::InOutElastic, "InOutElastic", false, true},
    {Easing::EEasingType::InBounce,     Easing::InBounce,     "InBounce",     false, false},
    {Easing::EEasingType::OutBounce,    Easing::OutBounce,    "OutBounce",    false, false},
    {Easing::EEasingType::InOutBounce,  Easing::InOutBounce,  "InOutBounce",  false, false},
    {Easing::EEasingType::SmoothStep,    Easing::SmoothStep,   "SmoothStep",    true,  false},
    {Easing::EEasingType::SmootherStep,  Easing::SmootherStep, "SmootherStep",  true,  false},
};

// 文書化されたPenner形式の式から得た独立参照値。
// 各組で異なる入出力点を使い、互いに整合していても誤ったIn/Out実装が
// 対称性だけで表を満たせないようにする。
constexpr FEasingGoldenCase kGoldenCases[] = {
    {Easing::EEasingType::Linear,       0.50f,  0.500000000f},
    {Easing::EEasingType::InQuad,       0.25f,  0.062500000f},
    {Easing::EEasingType::OutQuad,      0.75f,  0.937500000f},
    {Easing::EEasingType::InOutQuad,    0.25f,  0.125000000f},
    {Easing::EEasingType::InCubic,      0.25f,  0.015625000f},
    {Easing::EEasingType::OutCubic,     0.75f,  0.984375000f},
    {Easing::EEasingType::InOutCubic,   0.25f,  0.062500000f},
    {Easing::EEasingType::InQuart,      0.25f,  0.003906250f},
    {Easing::EEasingType::OutQuart,     0.75f,  0.996093750f},
    {Easing::EEasingType::InOutQuart,   0.25f,  0.031250000f},
    {Easing::EEasingType::InQuint,      0.25f,  0.000976563f},
    {Easing::EEasingType::OutQuint,     0.75f,  0.999023438f},
    {Easing::EEasingType::InOutQuint,   0.25f,  0.015625000f},
    {Easing::EEasingType::InSine,       0.25f,  0.076120467f},
    {Easing::EEasingType::OutSine,      0.75f,  0.923879533f},
    {Easing::EEasingType::InOutSine,    0.25f,  0.146446609f},
    {Easing::EEasingType::InExpo,       0.25f,  0.005524272f},
    {Easing::EEasingType::OutExpo,      0.75f,  0.994475728f},
    {Easing::EEasingType::InOutExpo,    0.25f,  0.015625000f},
    {Easing::EEasingType::InCirc,       0.25f,  0.031754163f},
    {Easing::EEasingType::OutCirc,      0.75f,  0.968245837f},
    {Easing::EEasingType::InOutCirc,    0.25f,  0.066987298f},
    {Easing::EEasingType::InBack,       0.25f, -0.064136563f},
    {Easing::EEasingType::OutBack,      0.75f,  1.064136563f},
    {Easing::EEasingType::InOutBack,    0.25f, -0.099681844f},
    {Easing::EEasingType::InElastic,    0.25f, -0.005524272f},
    {Easing::EEasingType::OutElastic,   0.75f,  1.005524272f},
    {Easing::EEasingType::InOutElastic, 0.25f,  0.011969444f},
    {Easing::EEasingType::InBounce,     0.25f,  0.027343750f},
    {Easing::EEasingType::OutBounce,    0.75f,  0.972656250f},
    {Easing::EEasingType::InOutBounce,  0.25f,  0.117187500f},
    {Easing::EEasingType::SmoothStep,   0.25f,  0.156250000f},
    {Easing::EEasingType::SmootherStep, 0.25f,  0.103515625f},
};

constexpr usize kCaseCount = sizeof(kCases) / sizeof(kCases[0]);
constexpr usize kGoldenCaseCount =
    sizeof(kGoldenCases) / sizeof(kGoldenCases[0]);

static_assert(kCaseCount == 33u,
              "The complete easing catalog must contain exactly 33 curves");
static_assert(kGoldenCaseCount == kCaseCount,
              "Every easing curve must have an independent golden value");
static_assert(static_cast<u32>(Easing::EEasingType::Count) == kCaseCount,
              "EEasingType::Count and the test catalog must stay synchronized");
static_assert(static_cast<u32>(Easing::EEasingType::Linear) == 0u);
static_assert(static_cast<u32>(Easing::EEasingType::InQuad) == 1u);
static_assert(static_cast<u32>(Easing::EEasingType::OutQuad) == 2u);
static_assert(static_cast<u32>(Easing::EEasingType::InOutQuad) == 3u);
static_assert(static_cast<u32>(Easing::EEasingType::InCubic) == 4u);
static_assert(static_cast<u32>(Easing::EEasingType::OutCubic) == 5u);
static_assert(static_cast<u32>(Easing::EEasingType::InOutCubic) == 6u);
static_assert(static_cast<u32>(Easing::EEasingType::InQuart) == 7u);
static_assert(static_cast<u32>(Easing::EEasingType::OutQuart) == 8u);
static_assert(static_cast<u32>(Easing::EEasingType::InOutQuart) == 9u);
static_assert(static_cast<u32>(Easing::EEasingType::InQuint) == 10u);
static_assert(static_cast<u32>(Easing::EEasingType::OutQuint) == 11u);
static_assert(static_cast<u32>(Easing::EEasingType::InOutQuint) == 12u);
static_assert(static_cast<u32>(Easing::EEasingType::InSine) == 13u);
static_assert(static_cast<u32>(Easing::EEasingType::OutSine) == 14u);
static_assert(static_cast<u32>(Easing::EEasingType::InOutSine) == 15u);
static_assert(static_cast<u32>(Easing::EEasingType::InExpo) == 16u);
static_assert(static_cast<u32>(Easing::EEasingType::OutExpo) == 17u);
static_assert(static_cast<u32>(Easing::EEasingType::InOutExpo) == 18u);
static_assert(static_cast<u32>(Easing::EEasingType::InCirc) == 19u);
static_assert(static_cast<u32>(Easing::EEasingType::OutCirc) == 20u);
static_assert(static_cast<u32>(Easing::EEasingType::InOutCirc) == 21u);
static_assert(static_cast<u32>(Easing::EEasingType::InBack) == 22u);
static_assert(static_cast<u32>(Easing::EEasingType::OutBack) == 23u);
static_assert(static_cast<u32>(Easing::EEasingType::InOutBack) == 24u);
static_assert(static_cast<u32>(Easing::EEasingType::InElastic) == 25u);
static_assert(static_cast<u32>(Easing::EEasingType::OutElastic) == 26u);
static_assert(static_cast<u32>(Easing::EEasingType::InOutElastic) == 27u);
static_assert(static_cast<u32>(Easing::EEasingType::InBounce) == 28u);
static_assert(static_cast<u32>(Easing::EEasingType::OutBounce) == 29u);
static_assert(static_cast<u32>(Easing::EEasingType::InOutBounce) == 30u);
static_assert(static_cast<u32>(Easing::EEasingType::SmoothStep) == 31u);
static_assert(static_cast<u32>(Easing::EEasingType::SmootherStep) == 32u);
static_assert(static_cast<u32>(Easing::EEasingType::Count) == 33u);

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

bool IsFinite(f32 value) noexcept {
    const f32 maximum = std::numeric_limits<f32>::max();
    return value == value && value >= -maximum && value <= maximum;
}

f32 NonFiniteAfterHalf(f32 t) noexcept {
    return t >= 0.5f
        ? std::numeric_limits<f32>::infinity()
        : t;
}

usize MirrorIndex(usize index) noexcept {
    if (index == 0u || index >= 31u) {
        return index;
    }
    const usize group_begin = 1u + ((index - 1u) / 3u) * 3u;
    const usize offset = (index - group_begin);
    if (offset == 0u) {
        return group_begin + 1u;
    }
    if (offset == 1u) {
        return group_begin;
    }
    return index;
}

} // namespace

ACS_TEST(EasingCompleteness, CatalogHasAllThirtyThreeNamedFunctions) {
    for (usize index = 0u; index < kCaseCount; ++index) {
        const FEasingCase& entry = kCases[index];
        EXPECT_TRUE(StringsEqual(Easing::GetName(entry.type), entry.name));
        EXPECT_EQ(Easing::GetFunction(entry.type), entry.function);

        Easing::EEasingType parsed = Easing::EEasingType::Count;
        EXPECT_TRUE(Easing::TryParseName(entry.name, parsed));
        EXPECT_EQ(parsed, entry.type);

        f32 start = -1.0f;
        f32 finish = -1.0f;
        EXPECT_TRUE(Easing::TryEvaluate(entry.type, 0.0f, start).Succeeded());
        EXPECT_TRUE(Easing::TryEvaluate(entry.type, 1.0f, finish).Succeeded());
        EXPECT_EQ(start, 0.0f);
        EXPECT_EQ(finish, 1.0f);
        EXPECT_EQ(entry.function(0.0f), 0.0f);
        EXPECT_EQ(entry.function(1.0f), 1.0f);
    }
}

ACS_TEST(EasingCompleteness, EveryCurveMatchesIndependentGoldenValue) {
    for (usize index = 0u; index < kGoldenCaseCount; ++index) {
        const FEasingGoldenCase& golden = kGoldenCases[index];
        EXPECT_EQ(golden.type, kCases[index].type);

        f32 checked_value = -100.0f;
        EXPECT_TRUE(Easing::TryEvaluate(
            golden.type, golden.t, checked_value).Succeeded());
        EXPECT_TRUE(IsFinite(checked_value));
        EXPECT_NEAR(checked_value, golden.expected, 2.0e-5f);

        const Easing::EasingFn function = Easing::GetFunction(golden.type);
        EXPECT_TRUE(function != nullptr);
        const f32 direct_value = function(golden.t);
        EXPECT_TRUE(IsFinite(direct_value));
        EXPECT_NEAR(direct_value, golden.expected, 2.0e-5f);
    }
}

ACS_TEST(EasingCompleteness, EveryCurveIsFiniteAndHasExpectedSymmetry) {
    for (usize index = 0u; index < kCaseCount; ++index) {
        const usize mirror_index = MirrorIndex(index);
        for (u32 sample = 0u; sample <= 128u; ++sample) {
            const f32 t = static_cast<f32>(sample) / 128.0f;
            f32 value = -100.0f;
            f32 mirrored = -100.0f;
            EXPECT_TRUE(
                Easing::TryEvaluate(kCases[index].type, t, value).Succeeded());
            EXPECT_TRUE(Easing::TryEvaluate(
                kCases[mirror_index].type, 1.0f - t, mirrored).Succeeded());
            EXPECT_TRUE(IsFinite(value));
            EXPECT_TRUE(IsFinite(mirrored));
            EXPECT_NEAR(value + mirrored, 1.0f, 3.0e-4f);
        }
    }
}

ACS_TEST(EasingCompleteness, NonOscillatingCurvesAreMonotonic) {
    for (usize index = 0u; index < kCaseCount; ++index) {
        if (!kCases[index].monotonic) {
            continue;
        }
        f32 previous = 0.0f;
        EXPECT_TRUE(
            Easing::TryEvaluate(kCases[index].type, 0.0f, previous).Succeeded());
        for (u32 sample = 1u; sample <= 512u; ++sample) {
            const f32 t = static_cast<f32>(sample) / 512.0f;
            f32 current = -1.0f;
            EXPECT_TRUE(
                Easing::TryEvaluate(kCases[index].type, t, current).Succeeded());
            EXPECT_TRUE(current + 2.0e-5f >= previous);
            previous = current;
        }
    }
}

ACS_TEST(EasingCompleteness, BackAndElasticOvershootWhileBounceStaysBounded) {
    for (usize index = 0u; index < kCaseCount; ++index) {
        bool saw_outside = false;
        bool saw_decrease = false;
        f32 previous = 0.0f;
        for (u32 sample = 0u; sample <= 1024u; ++sample) {
            const f32 t = static_cast<f32>(sample) / 1024.0f;
            f32 value = 0.0f;
            EXPECT_TRUE(
                Easing::TryEvaluate(kCases[index].type, t, value).Succeeded());
            saw_outside = saw_outside || value < -1.0e-5f || value > 1.00001f;
            if (sample != 0u && value + 1.0e-5f < previous) {
                saw_decrease = true;
            }
            if (index >= 28u && index <= 30u) {
                EXPECT_TRUE(value >= -1.0e-5f);
                EXPECT_TRUE(value <= 1.00001f);
            }
            previous = value;
        }
        if (kCases[index].overshoots) {
            EXPECT_TRUE(saw_outside);
        }
        if (index >= 28u && index <= 30u) {
            EXPECT_TRUE(saw_decrease);
            EXPECT_FALSE(saw_outside);
        }
    }
}

ACS_TEST(EasingCompleteness, CheckedDispatcherClampsFiniteInputs) {
    for (usize index = 0u; index < kCaseCount; ++index) {
        f32 before = 99.0f;
        f32 after = 99.0f;
        EXPECT_TRUE(
            Easing::TryEvaluate(kCases[index].type, -100.0f, before).Succeeded());
        EXPECT_TRUE(
            Easing::TryEvaluate(kCases[index].type, 100.0f, after).Succeeded());
        EXPECT_EQ(before, 0.0f);
        EXPECT_EQ(after, 1.0f);
        EXPECT_EQ(kCases[index].function(-100.0f), 0.0f);
        EXPECT_EQ(kCases[index].function(100.0f), 1.0f);
    }
}

ACS_TEST(EasingCompleteness, NonFiniteAndInvalidInputsPreserveOutput) {
    const f32 invalid_inputs[] = {
        std::numeric_limits<f32>::quiet_NaN(),
        std::numeric_limits<f32>::infinity(),
        -std::numeric_limits<f32>::infinity(),
    };
    for (usize index = 0u;
         index < sizeof(invalid_inputs) / sizeof(invalid_inputs[0]); ++index) {
        f32 output = 123.25f;
        const Easing::FEasingResult result = Easing::TryEvaluate(
            Easing::EEasingType::Linear, invalid_inputs[index], output);
        EXPECT_FALSE(result.Succeeded());
        EXPECT_EQ(result.error, Easing::EEasingError::NonFiniteInput);
        EXPECT_EQ(output, 123.25f);
        EXPECT_EQ(
            Easing::Evaluate(
                Easing::EEasingType::Linear, invalid_inputs[index], 17.5f),
            17.5f);
    }

    const Easing::EEasingType invalid_types[] = {
        Easing::EEasingType::Count,
        static_cast<Easing::EEasingType>(0xffu),
    };
    for (usize index = 0u;
         index < sizeof(invalid_types) / sizeof(invalid_types[0]); ++index) {
        f32 output = 222.5f;
        const Easing::FEasingResult result =
            Easing::TryEvaluate(invalid_types[index], 0.5f, output);
        EXPECT_FALSE(result.Succeeded());
        EXPECT_EQ(result.error, Easing::EEasingError::InvalidType);
        EXPECT_EQ(output, 222.5f);
        EXPECT_EQ(
            Easing::Evaluate(invalid_types[index], 0.5f, 42.25f),
            42.25f);
        EXPECT_EQ(Easing::GetFunction(invalid_types[index]), nullptr);
        EXPECT_TRUE(
            StringsEqual(Easing::GetName(invalid_types[index]), "Invalid"));
    }
}

ACS_TEST(EasingCompleteness, CompatibilityFunctionsAreTotalForNonFiniteInput) {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 positive_infinity = std::numeric_limits<f32>::infinity();
    const f32 negative_infinity = -std::numeric_limits<f32>::infinity();
    for (usize index = 0u; index < kCaseCount; ++index) {
        EXPECT_EQ(kCases[index].function(nan), 0.0f);
        EXPECT_EQ(kCases[index].function(negative_infinity), 0.0f);
        EXPECT_EQ(kCases[index].function(positive_infinity), 1.0f);
    }
}

ACS_TEST(EasingCompleteness, InvalidNamesPreserveParsedType) {
    Easing::EEasingType parsed = Easing::EEasingType::OutElastic;
    EXPECT_FALSE(Easing::TryParseName(nullptr, parsed));
    EXPECT_EQ(parsed, Easing::EEasingType::OutElastic);
    EXPECT_FALSE(Easing::TryParseName("", parsed));
    EXPECT_EQ(parsed, Easing::EEasingType::OutElastic);
    EXPECT_FALSE(Easing::TryParseName("NotAnEasing", parsed));
    EXPECT_EQ(parsed, Easing::EEasingType::OutElastic);
    EXPECT_FALSE(Easing::TryParseName("linear", parsed));
    EXPECT_EQ(parsed, Easing::EEasingType::OutElastic);
}

ACS_TEST(EasingCompleteness, CheckedNamesClassifyFailuresAndPreserveOutputs) {
    for (usize index = 0u; index < kCaseCount; ++index) {
        const char* name = "sentinel";
        const Easing::FEasingResult name_result =
            Easing::TryGetName(kCases[index].type, name);
        EXPECT_TRUE(name_result.Succeeded());
        EXPECT_TRUE(StringsEqual(name, kCases[index].name));

        Easing::EEasingType parsed = Easing::EEasingType::Count;
        const Easing::FEasingResult parse_result =
            Easing::TryParseNameChecked(name, parsed);
        EXPECT_TRUE(parse_result.Succeeded());
        EXPECT_EQ(parsed, kCases[index].type);
    }

    const char* unchanged_name = "sentinel";
    const Easing::FEasingResult invalid_name_result =
        Easing::TryGetName(Easing::EEasingType::Count, unchanged_name);
    EXPECT_EQ(invalid_name_result.error, Easing::EEasingError::InvalidType);
    EXPECT_TRUE(StringsEqual(unchanged_name, "sentinel"));

    Easing::EEasingType unchanged_type = Easing::EEasingType::OutElastic;
    const Easing::FEasingResult null_result =
        Easing::TryParseNameChecked(nullptr, unchanged_type);
    EXPECT_EQ(null_result.error, Easing::EEasingError::NullName);
    EXPECT_EQ(unchanged_type, Easing::EEasingType::OutElastic);

    const Easing::FEasingResult empty_result =
        Easing::TryParseNameChecked("", unchanged_type);
    EXPECT_EQ(empty_result.error, Easing::EEasingError::UnknownName);
    EXPECT_EQ(unchanged_type, Easing::EEasingType::OutElastic);

    const Easing::FEasingResult unknown_result =
        Easing::TryParseNameChecked("NotAnEasing", unchanged_type);
    EXPECT_EQ(unknown_result.error, Easing::EEasingError::UnknownName);
    EXPECT_EQ(unchanged_type, Easing::EEasingType::OutElastic);

    EXPECT_TRUE(StringsEqual(
        Easing::FEasingResult::ErrorName(Easing::EEasingError::NullName),
        "NullName"));
    EXPECT_TRUE(StringsEqual(
        Easing::FEasingResult::ErrorName(Easing::EEasingError::UnknownName),
        "UnknownName"));
}

ACS_TEST(EasingCompleteness, UniformSamplingCoversEveryCurveAndBothEndpoints) {
    constexpr usize sample_count = 9u;
    f32 samples[sample_count]{};

    for (usize curve_index = 0u; curve_index < kCaseCount; ++curve_index) {
        for (usize sample_index = 0u;
             sample_index < sample_count; ++sample_index) {
            samples[sample_index] = -100.0f;
        }

        const Easing::FEasingResult result = Easing::TrySampleCurve(
            kCases[curve_index].type, samples, sample_count);
        EXPECT_TRUE(result.Succeeded());
        EXPECT_EQ(samples[0], 0.0f);
        EXPECT_EQ(samples[sample_count - 1u], 1.0f);

        for (usize sample_index = 0u;
             sample_index < sample_count; ++sample_index) {
            const f32 t = static_cast<f32>(sample_index) /
                          static_cast<f32>(sample_count - 1u);
            EXPECT_TRUE(IsFinite(samples[sample_index]));
            EXPECT_NEAR(
                samples[sample_index],
                kCases[curve_index].function(t),
                1.0e-6f);
        }
    }
}

ACS_TEST(EasingCompleteness, SamplingFailuresPreserveTheWholeOutput) {
    f32 samples[] = {11.0f, 22.0f, 33.0f, 44.0f};

    const auto ExpectUnchanged = [&samples]() noexcept {
        EXPECT_EQ(samples[0], 11.0f);
        EXPECT_EQ(samples[1], 22.0f);
        EXPECT_EQ(samples[2], 33.0f);
        EXPECT_EQ(samples[3], 44.0f);
    };

    Easing::FEasingResult result = Easing::TrySampleCurve(
        Easing::EEasingType::Count, samples, 4u);
    EXPECT_EQ(result.error, Easing::EEasingError::InvalidType);
    ExpectUnchanged();

    result = Easing::TrySampleCurve(
        Easing::EEasingType::Linear, samples, 1u);
    EXPECT_EQ(result.error, Easing::EEasingError::InvalidSampleCount);
    ExpectUnchanged();

    result = Easing::TrySampleCurve(
        Easing::EEasingType::Linear, samples,
        Easing::kMaxSampleCount + 1u);
    EXPECT_EQ(result.error, Easing::EEasingError::InvalidSampleCount);
    ExpectUnchanged();

    result = Easing::TrySampleCurve(
        Easing::EEasingType::Linear, nullptr, 2u);
    EXPECT_EQ(result.error, Easing::EEasingError::NullOutput);
    ExpectUnchanged();

    EXPECT_TRUE(StringsEqual(
        Easing::FEasingResult::ErrorName(
            Easing::EEasingError::InvalidSampleCount),
        "InvalidSampleCount"));
    EXPECT_TRUE(StringsEqual(
        Easing::FEasingResult::ErrorName(Easing::EEasingError::NullOutput),
        "NullOutput"));
    EXPECT_TRUE(StringsEqual(
        Easing::FEasingResult::ErrorName(
            Easing::EEasingError::NonFiniteResult),
        "NonFiniteResult"));
}

ACS_TEST(EasingCompleteness, SamplingAcceptsDocumentedCountBounds) {
    f32 minimum_samples[Easing::kMinSampleCount] = {-1.0f, -1.0f};
    Easing::FEasingResult result = Easing::TrySampleCurve(
        Easing::EEasingType::Linear,
        minimum_samples,
        Easing::kMinSampleCount);
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(minimum_samples[0], 0.0f);
    EXPECT_EQ(minimum_samples[Easing::kMinSampleCount - 1u], 1.0f);

    static f32 maximum_samples[Easing::kMaxSampleCount]{};
    result = Easing::TrySampleCurve(
        Easing::EEasingType::Linear,
        maximum_samples,
        Easing::kMaxSampleCount);
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(maximum_samples[0], 0.0f);
    EXPECT_TRUE(IsFinite(
        maximum_samples[Easing::kMaxSampleCount / 2u]));
    EXPECT_EQ(maximum_samples[Easing::kMaxSampleCount - 1u], 1.0f);
}

ACS_TEST(EasingCompleteness, NonFiniteResultsPreserveOutputs) {
    f32 single_value = 42.0f;
    Easing::FEasingResult result = Easing::Detail::TryEvaluateFunction(
        NonFiniteAfterHalf, 0.75f, single_value);
    EXPECT_EQ(result.error, Easing::EEasingError::NonFiniteResult);
    EXPECT_EQ(single_value, 42.0f);

    f32 samples[] = {11.0f, 22.0f, 33.0f, 44.0f};
    result = Easing::Detail::TrySampleFunction(
        NonFiniteAfterHalf, samples, 4u);
    EXPECT_EQ(result.error, Easing::EEasingError::NonFiniteResult);
    EXPECT_EQ(samples[0], 11.0f);
    EXPECT_EQ(samples[1], 22.0f);
    EXPECT_EQ(samples[2], 33.0f);
    EXPECT_EQ(samples[3], 44.0f);
}
