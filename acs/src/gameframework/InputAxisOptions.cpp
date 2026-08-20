// SPDX-License-Identifier: Apache-2.0
#include "gameframework/InputAxisOptions.h"

#include <cmath>

namespace acs::game {

/** 設定値が有限かつ公開範囲内ならtrueを返す。 */
bool FInputAxisOptions::IsValid() const noexcept
{
    return std::isfinite(dead_zone) && dead_zone >= 0.0f && dead_zone < 1.0f && std::isfinite(scale) && scale >= 0.0f;
}

/** 有限範囲を検査し、デッドゾーン外を再正規化して倍率と反転を適用する。 */
f32 FInputAxisOptions::Apply(f32 value) const noexcept
{
    if (!IsValid() || !std::isfinite(value)) {
        return 0.0f;
    }

    /** 公開範囲へ制限した入力値。 */
    const f64 clamped_value = value > 1.0f ? 1.0 : (value < -1.0f ? -1.0 : static_cast<f64>(value));
    /** 符号を除いた入力の強さ。 */
    const f64 magnitude = std::fabs(clamped_value);
    if (magnitude <= static_cast<f64>(dead_zone)) return 0.0f;

    /** デッドゾーン外を0から1へ再正規化した強さ。 */
    const f64 normalized = (magnitude - static_cast<f64>(dead_zone)) / (1.0 - static_cast<f64>(dead_zone));
    /** 倍率適用後に公開上限へ制限した強さ。 */
    const f64 scaled = normalized * static_cast<f64>(scale);
    const f32 result = static_cast<f32>(scaled >= 1.0 ? 1.0 : scaled);
    if (result <= 0.0f) return 0.0f;

    /** 元の符号と反転指定を合成した出力方向。 */
    const bool negative = (clamped_value < 0.0) != inverted;
    return negative ? -result : result;
}

} // namespace acs::game
