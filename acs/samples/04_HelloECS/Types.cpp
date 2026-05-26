// SPDX-License-Identifier: Apache-2.0
// HelloECS — 共通ユーティリティ実装。
#include "Types.h"

#include "math/Math.h"
#include "foundation/Log.h"

using namespace acs;

namespace hello04 {

void GenerateBallTexture(u8* out) noexcept {
    for (u32 y = 0; y < kBallTexSize; ++y) {
        for (u32 x = 0; x < kBallTexSize; ++x) {
            const f32 cx = static_cast<f32>(x) - kBallTexSize * 0.5f;
            const f32 cy = static_cast<f32>(y) - kBallTexSize * 0.5f;
            const f32 r = Sqrt(cx*cx + cy*cy) / (kBallTexSize * 0.5f);
            f32 alpha = r > 1.0f ? 0.0f : (1.0f - r);
            const usize i = static_cast<usize>(y * kBallTexSize + x) * 4;
            out[i+0] = 255; out[i+1] = 255; out[i+2] = 255;
            out[i+3] = static_cast<u8>(alpha * alpha * 255);
        }
    }
}

void OnSpawnEvent(const void* payload, void* /*user*/) {
    const auto* e = static_cast<const SpawnEvent*>(payload);
    ACS_LOG_INFO("[SpawnEvent] entities now %u", e->total);
}

} // namespace hello04
