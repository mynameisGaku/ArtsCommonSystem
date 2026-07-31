// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/LegacyKitEaseIdCodec.h"

#include <limits>

using namespace acs;
using namespace acs::game;

namespace {

/** 旧形式の固定 ID と対応する正規型。 */
struct FLegacyEaseIdCase {
    /** 旧形式で保存された数値 ID。 */
    i32 legacy_id;

    /** ID が表す ACS の正規型。 */
    Easing::EEasingType type;
};

/** 旧形式で固定された全 33 ID の独立した期待表。 */
constexpr FLegacyEaseIdCase kLegacyEaseIdCases[] = {{0, Easing::EEasingType::Linear}, {1, Easing::EEasingType::SmoothStep}, {2, Easing::EEasingType::OutElastic}, {3, Easing::EEasingType::InQuad}, {4, Easing::EEasingType::OutQuad}, {5, Easing::EEasingType::InCubic}, {6, Easing::EEasingType::OutCubic}, {7, Easing::EEasingType::InOutSine}, {8, Easing::EEasingType::InBack}, {9, Easing::EEasingType::OutBack}, {10, Easing::EEasingType::OutBounce}, {11, Easing::EEasingType::SmootherStep}, {12, Easing::EEasingType::InSine}, {13, Easing::EEasingType::OutSine}, {14, Easing::EEasingType::InOutQuad}, {15, Easing::EEasingType::InOutCubic}, {16, Easing::EEasingType::InQuart}, {17, Easing::EEasingType::OutQuart}, {18, Easing::EEasingType::InOutQuart}, {19, Easing::EEasingType::InQuint}, {20, Easing::EEasingType::OutQuint}, {21, Easing::EEasingType::InOutQuint}, {22, Easing::EEasingType::InExpo}, {23, Easing::EEasingType::OutExpo}, {24, Easing::EEasingType::InOutExpo}, {25, Easing::EEasingType::InCirc}, {26, Easing::EEasingType::OutCirc}, {27, Easing::EEasingType::InOutCirc}, {28, Easing::EEasingType::InOutBack}, {29, Easing::EEasingType::InElastic}, {30, Easing::EEasingType::InOutElastic}, {31, Easing::EEasingType::InBounce}, {32, Easing::EEasingType::InOutBounce}};

/** 独立した期待表の要素数。 */
constexpr usize kLegacyEaseIdCaseCount = sizeof(kLegacyEaseIdCases) / sizeof(kLegacyEaseIdCases[0]);

static_assert(kLegacyEaseIdCaseCount == static_cast<usize>(FLegacyKitEaseIdCodec::kLegacyIdCount));
static_assert(noexcept(FLegacyKitEaseIdCodec::TryDecode(0, *static_cast<Easing::EEasingType*>(nullptr))));
static_assert(noexcept(FLegacyKitEaseIdCodec::TryEncode(Easing::EEasingType::Linear, *static_cast<i32*>(nullptr))));

} // namespace

ACS_TEST(LegacyKitEaseIdCodec, DecodesEveryFixedLegacyId) {
    // 独立した期待表を先頭から検証する位置。
    for (usize index = 0u; index < kLegacyEaseIdCaseCount; ++index) {
        /** 旧形式 ID から復元する正規型。 */
        Easing::EEasingType decoded = Easing::EEasingType::Count;
        EXPECT_TRUE(FLegacyKitEaseIdCodec::TryDecode(kLegacyEaseIdCases[index].legacy_id, decoded));
        EXPECT_EQ(decoded, kLegacyEaseIdCases[index].type);
    }
}

ACS_TEST(LegacyKitEaseIdCodec, RoundTripsEveryLegacyAndMappedAcsValue) {
    // 両方向の往復を先頭から検証する位置。
    for (usize index = 0u; index < kLegacyEaseIdCaseCount; ++index) {
        /** 旧形式 ID から復元する正規型。 */
        Easing::EEasingType decoded = Easing::EEasingType::Count;
        EXPECT_TRUE(FLegacyKitEaseIdCodec::TryDecode(kLegacyEaseIdCases[index].legacy_id, decoded));

        /** 復元した正規型から得る旧形式 ID。 */
        i32 encoded = -1;
        EXPECT_TRUE(FLegacyKitEaseIdCodec::TryEncode(decoded, encoded));
        EXPECT_EQ(encoded, kLegacyEaseIdCases[index].legacy_id);

        /** 再び旧形式 ID から復元する正規型。 */
        Easing::EEasingType round_tripped = Easing::EEasingType::Count;
        EXPECT_TRUE(FLegacyKitEaseIdCodec::TryDecode(encoded, round_tripped));
        EXPECT_EQ(round_tripped, kLegacyEaseIdCases[index].type);
    }
}

ACS_TEST(LegacyKitEaseIdCodec, RejectsInvalidValuesWithoutChangingOutputs) {
    /** 旧形式の範囲外を網羅する入力。 */
    const i32 invalid_legacy_ids[] = {-1, FLegacyKitEaseIdCodec::kLegacyIdCount, std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max()};

    // 範囲外 ID を先頭から検証する値。
    for (i32 legacy_id : invalid_legacy_ids) {
        /** 失敗時に保持される正規型。 */
        Easing::EEasingType decoded = Easing::EEasingType::OutBack;
        EXPECT_FALSE(FLegacyKitEaseIdCodec::TryDecode(legacy_id, decoded));
        EXPECT_EQ(decoded, Easing::EEasingType::OutBack);
    }

    /** 旧形式へ対応しない正規型。 */
    const Easing::EEasingType invalid_types[] = {Easing::EEasingType::Count, static_cast<Easing::EEasingType>(0xffu)};

    // 未対応型を先頭から検証する値。
    for (Easing::EEasingType type : invalid_types) {
        /** 失敗時に保持される旧形式 ID。 */
        i32 encoded = 12345;
        EXPECT_FALSE(FLegacyKitEaseIdCodec::TryEncode(type, encoded));
        EXPECT_EQ(encoded, 12345);
    }
}

ACS_TEST(LegacyKitEaseIdCodec, DirectCastDoesNotMigrateLegacyIds) {
    /** 旧 ID 1 を誤って直接変換した場合の型。 */
    const Easing::EEasingType directly_cast = static_cast<Easing::EEasingType>(1);
    EXPECT_EQ(directly_cast, Easing::EEasingType::InQuad);

    /** 明示 codec で旧 ID 1 から復元した型。 */
    Easing::EEasingType decoded = Easing::EEasingType::Linear;
    EXPECT_TRUE(FLegacyKitEaseIdCodec::TryDecode(1, decoded));
    EXPECT_EQ(decoded, Easing::EEasingType::SmoothStep);
    EXPECT_NE(decoded, directly_cast);
}
