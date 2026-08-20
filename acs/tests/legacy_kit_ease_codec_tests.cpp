// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/LegacyKitEaseCodec.h"

#include <limits>

using namespace acs;
using namespace acs::game;

namespace {

/** 実測値と比較する正しい期待値。 */
constexpr Easing::EEasingType kExpectedTypes[] = {Easing::EEasingType::Linear, Easing::EEasingType::SmoothStep, Easing::EEasingType::OutElastic, Easing::EEasingType::InQuad, Easing::EEasingType::OutQuad, Easing::EEasingType::InCubic, Easing::EEasingType::OutCubic, Easing::EEasingType::InOutSine, Easing::EEasingType::InBack, Easing::EEasingType::OutBack, Easing::EEasingType::OutBounce, Easing::EEasingType::SmootherStep, Easing::EEasingType::InSine, Easing::EEasingType::OutSine, Easing::EEasingType::InOutQuad, Easing::EEasingType::InOutCubic, Easing::EEasingType::InQuart, Easing::EEasingType::OutQuart, Easing::EEasingType::InOutQuart, Easing::EEasingType::InQuint, Easing::EEasingType::OutQuint, Easing::EEasingType::InOutQuint, Easing::EEasingType::InExpo, Easing::EEasingType::OutExpo, Easing::EEasingType::InOutExpo, Easing::EEasingType::InCirc, Easing::EEasingType::OutCirc, Easing::EEasingType::InOutCirc, Easing::EEasingType::InOutBack, Easing::EEasingType::InElastic, Easing::EEasingType::InOutElastic, Easing::EEasingType::InBounce, Easing::EEasingType::InOutBounce,};

/** 実測値と比較する正しい期待値。 */
constexpr usize kExpectedTypeCount =
    sizeof(kExpectedTypes) / sizeof(kExpectedTypes[0]);
static_assert(kExpectedTypeCount == 33u);

} // namespace

ACS_TEST(LegacyKitEaseCodec, DecodesAllThirtyThreeFixedValues) {
    for (/* 現在走査している要素の位置。 */ usize index = 0u; index < kExpectedTypeCount; ++index) {
        /** 初期化または復元操作から得た状態を期待値と照合する。 */
        Easing::EEasingType decoded = Easing::EEasingType::Count;
        EXPECT_TRUE(FLegacyKitEaseCodec::TryDecode(static_cast<i32>(index), decoded));
        EXPECT_EQ(decoded, kExpectedTypes[index]);
    }
}

ACS_TEST(LegacyKitEaseCodec, RoundTripsEveryLegacyAndAcsValue) {
    for (/* 現在走査している要素の位置。 */ usize index = 0u; index < kExpectedTypeCount; ++index) {
        /** 初期化または復元操作から得た状態を期待値と照合する。 */
        Easing::EEasingType decoded = Easing::EEasingType::Count;
        EXPECT_TRUE(FLegacyKitEaseCodec::TryDecode(static_cast<i32>(index), decoded));

        /** 「encoded」の境界条件を再現する数値入力。 */
        i32 encoded = -1;
        EXPECT_TRUE(FLegacyKitEaseCodec::TryEncode(decoded, encoded));
        EXPECT_EQ(encoded, static_cast<i32>(index));
    }
}

ACS_TEST(LegacyKitEaseCodec, RejectsInvalidValuesWithoutChangingOutputs) {
    /** 正常系と失敗系を選び分ける検証値。 */
    const i32 invalid_legacy_values[] = {-1, FLegacyKitEaseCodec::kLegacyValueCount, std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max(),};
    for (/* 「legacy_value」へ保存した実測値を直後の期待値と照合する。 */ i32 legacy_value : invalid_legacy_values) {
        /** 初期化または復元操作から得た状態を期待値と照合する。 */
        Easing::EEasingType decoded = Easing::EEasingType::OutBack;
        EXPECT_FALSE(FLegacyKitEaseCodec::TryDecode(legacy_value, decoded));
        EXPECT_EQ(decoded, Easing::EEasingType::OutBack);
    }

    /** 正常系と失敗系を選び分ける検証値。 */
    const Easing::EEasingType invalid_types[] = {Easing::EEasingType::Count, static_cast<Easing::EEasingType>(0xffu),};
    for (/* 登録対象または要求を照合する識別値。 */ Easing::EEasingType type : invalid_types) {
        /** 「encoded」の境界条件を再現する数値入力。 */
        i32 encoded = 12345;
        EXPECT_FALSE(FLegacyKitEaseCodec::TryEncode(type, encoded));
        EXPECT_EQ(encoded, 12345);
    }
}
