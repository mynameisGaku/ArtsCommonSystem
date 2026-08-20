// SPDX-License-Identifier: Apache-2.0
// 旧KitのEase保存値とACSのイージング型を安全に相互変換する。
#pragma once

#include "foundation/Types.h"
#include "gameframework/Easing.h"

namespace acs::game {

/**
 * 旧Kitが保存したEaseの数値IDを扱う互換codec。
 *
 * 旧KitとACSでは同じ33曲線でも数値順が異なる。保存値やenumを直接castせず、
 * 必ずこのcodecを通す。変換失敗時は出力を変更しない。
 */
class FLegacyKitEaseCodec final {
public:
    static constexpr i32 kLegacyValueCount = 33; // kLegacyValueCountの状態を保持する。

    /** 旧Kit保存値をACS型へ変換する。 */
    static bool TryDecode(i32 legacy_value, Easing::EEasingType& out_type) noexcept; // legacy_value、out_typeで使う値を示す。

    /** ACS型を旧Kit保存値へ変換する。 */
    static bool TryEncode(Easing::EEasingType type, i32& out_legacy_value) noexcept; // type、out_legacy_valueで使う値を示す。
};

} // namespace acs::game
