// SPDX-License-Identifier: Apache-2.0
#include "gameframework/LegacyKitEaseIdCodec.h"

namespace acs::game {
namespace {

using Easing::EEasingType;

/** 旧形式に対応しないことを表す逆引き値。 */
constexpr i32 kUnmappedLegacyId = -1;

/** 旧形式の ID 順に対応する ACS の正規型。 */
constexpr EEasingType kLegacyToAcs[] = {EEasingType::Linear, EEasingType::SmoothStep, EEasingType::OutElastic, EEasingType::InQuad, EEasingType::OutQuad, EEasingType::InCubic, EEasingType::OutCubic, EEasingType::InOutSine, EEasingType::InBack, EEasingType::OutBack, EEasingType::OutBounce, EEasingType::SmootherStep, EEasingType::InSine, EEasingType::OutSine, EEasingType::InOutQuad, EEasingType::InOutCubic, EEasingType::InQuart, EEasingType::OutQuart, EEasingType::InOutQuart, EEasingType::InQuint, EEasingType::OutQuint, EEasingType::InOutQuint, EEasingType::InExpo, EEasingType::OutExpo, EEasingType::InOutExpo, EEasingType::InCirc, EEasingType::OutCirc, EEasingType::InOutCirc, EEasingType::InOutBack, EEasingType::InElastic, EEasingType::InOutElastic, EEasingType::InBounce, EEasingType::InOutBounce};

/** 旧形式から正規型への固定対応数。 */
constexpr usize kLegacyToAcsCount = sizeof(kLegacyToAcs) / sizeof(kLegacyToAcs[0]);

/** 現在の正規型数。 */
constexpr usize kAcsEasingTypeCount = static_cast<usize>(EEasingType::Count);

/** 正規型から旧形式 ID を引く固定長の内部表。 */
struct FAcsToLegacyTable {
    /** 正規型の数値順に保持する旧形式 ID。 */
    i32 values[kAcsEasingTypeCount];
};

/** 対応表の全要素が現在の正規型として有効かを返す。 */
constexpr bool AreMappedTypesValid() noexcept {
    // 対応表を先頭から検証する正規型。
    for (EEasingType type : kLegacyToAcs) {
        if (static_cast<usize>(type) >= kAcsEasingTypeCount) return false;
    }
    return true;
}

/** 対応表の正規型に重複がないかを返す。 */
constexpr bool AreMappedTypesUnique() noexcept {
    // 比較元の正規型を選ぶ位置。
    for (usize first = 0u; first < kLegacyToAcsCount; ++first) {
        // 比較元より後ろの正規型を選ぶ位置。
        for (usize second = first + 1u; second < kLegacyToAcsCount; ++second) {
            if (kLegacyToAcs[first] == kLegacyToAcs[second]) return false;
        }
    }
    return true;
}

/** 正規型から旧形式 ID を引く固定表を生成する。 */
constexpr FAcsToLegacyTable BuildAcsToLegacy() noexcept {
    /** 全正規型を未対応で初期化する逆引き表。 */
    FAcsToLegacyTable result{};
    // 逆引き表を未対応値で初期化する位置。
    for (usize index = 0u; index < kAcsEasingTypeCount; ++index) {
        result.values[index] = kUnmappedLegacyId;
    }
    // 旧形式の対応を逆引き表へ登録する位置。
    for (usize legacy_index = 0u; legacy_index < kLegacyToAcsCount; ++legacy_index) {
        result.values[static_cast<usize>(kLegacyToAcs[legacy_index])] = static_cast<i32>(legacy_index);
    }
    return result;
}

static_assert(kLegacyToAcsCount == static_cast<usize>(FLegacyKitEaseIdCodec::kLegacyIdCount));
static_assert(AreMappedTypesValid());
static_assert(AreMappedTypesUnique());

/** 正規型から旧形式 ID を引く固定表。 */
constexpr auto kAcsToLegacy = BuildAcsToLegacy();

} // namespace

bool FLegacyKitEaseIdCodec::TryDecode(i32 legacy_id, Easing::EEasingType& out_type) noexcept {
    if (legacy_id < 0 || legacy_id >= kLegacyIdCount) return false;

    /** 確認済み ID を対応表へ渡す位置。 */
    const usize legacy_index = static_cast<usize>(legacy_id);
    out_type = kLegacyToAcs[legacy_index];
    return true;
}

bool FLegacyKitEaseIdCodec::TryEncode(Easing::EEasingType type, i32& out_legacy_id) noexcept {
    /** 正規型を逆引き表へ渡す位置。 */
    const usize type_index = static_cast<usize>(type);
    if (type_index >= kAcsEasingTypeCount) return false;

    /** 正規型に対応する旧形式 ID。 */
    const i32 legacy_id = kAcsToLegacy.values[type_index];
    if (legacy_id == kUnmappedLegacyId) return false;

    out_legacy_id = legacy_id;
    return true;
}

} // namespace acs::game
