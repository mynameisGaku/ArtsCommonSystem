// SPDX-License-Identifier: Apache-2.0
#include "gameframework/LegacyKitEaseCodec.h"

namespace acs::game {
namespace {

using Easing::EEasingType;

// 添字は旧kit::Easeの固定保存値。列挙順をACS側へ流用してはならない。
constexpr EEasingType kLegacyToAcs[] = {EEasingType::Linear, EEasingType::SmoothStep, EEasingType::OutElastic, EEasingType::InQuad, EEasingType::OutQuad, EEasingType::InCubic, EEasingType::OutCubic, EEasingType::InOutSine, EEasingType::InBack, EEasingType::OutBack, EEasingType::OutBounce, EEasingType::SmootherStep, EEasingType::InSine, EEasingType::OutSine, EEasingType::InOutQuad, EEasingType::InOutCubic, EEasingType::InQuart, EEasingType::OutQuart, EEasingType::InOutQuart, EEasingType::InQuint, EEasingType::OutQuint, EEasingType::InOutQuint, EEasingType::InExpo, EEasingType::OutExpo, EEasingType::InOutExpo, EEasingType::InCirc, EEasingType::OutCirc, EEasingType::InOutCirc, EEasingType::InOutBack, EEasingType::InElastic, EEasingType::InOutElastic, EEasingType::InBounce, EEasingType::InOutBounce,};

constexpr usize kMappingCount = sizeof(kLegacyToAcs) / sizeof(kLegacyToAcs[0]); // kMappingCountの共有状態を保持する。
static_assert(kMappingCount == static_cast<usize>(FLegacyKitEaseCodec::kLegacyValueCount));
static_assert(kMappingCount == static_cast<usize>(EEasingType::Count));

} // namespace

bool FLegacyKitEaseCodec::TryDecode(i32 legacy_value, Easing::EEasingType& out_type) noexcept { // TryDecode、legacy_value、out_typeで使う値を示す。
    if (legacy_value < 0 || legacy_value >= kLegacyValueCount) {
        return false;
    }

    out_type = kLegacyToAcs[static_cast<usize>(legacy_value)];
    return true;
}

bool FLegacyKitEaseCodec::TryEncode(Easing::EEasingType type, i32& out_legacy_value) noexcept { // TryEncode、type、out_legacy_valueで使う値を示す。
    for (usize legacy_value = 0u; legacy_value < kMappingCount; ++legacy_value) { // legacy_valueの一時値を保持する。
        if (kLegacyToAcs[legacy_value] == type) {
            out_legacy_value = static_cast<i32>(legacy_value);
            return true;
        }
    }
    return false;
}

} // namespace acs::game
