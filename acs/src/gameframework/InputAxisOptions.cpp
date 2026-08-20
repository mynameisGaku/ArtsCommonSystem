// SPDX-License-Identifier: Apache-2.0
#include "gameframework/InputAxisOptions.h"

#include <cmath>

namespace acs::game {

bool FInputAxisOptions::IsValid() const noexcept {
    return std::isfinite(dead_zone) && dead_zone >= 0.0f && dead_zone < 1.0f &&
           std::isfinite(scale) && scale >= 0.0f;
}

f32 FInputAxisOptions::Apply(f32 value) const noexcept {
    if (!IsValid() || !std::isfinite(value)) return 0.0f;

    const f32 magnitude = std::fabs(value);
    if (magnitude <= dead_zone) return 0.0f;

    const f32 normalized = (magnitude - dead_zone) / (1.0f - dead_zone);
    const f32 direction = value < 0.0f ? -1.0f : 1.0f;
    const f32 inverted_direction = inverted ? -direction : direction;
    return normalized * inverted_direction * scale;
}

} // namespace acs::game
