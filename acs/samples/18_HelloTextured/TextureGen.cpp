// SPDX-License-Identifier: Apache-2.0
// HelloTextured — 手続き生成テクスチャの実装。
#include "TextureGen.h"
#include "Types.h"

using namespace acs;

namespace hellotextured {

void GenerateTexture(u8* dst) noexcept {
    // 8 ピクセル区切りのチェッカー (青 / 白) に、中央 1/4 半径の山吹色の円 (月) を上塗りする。
    for (u32 y = 0; y < kTexSize; ++y) {
        for (u32 x = 0; x < kTexSize; ++x) {
            const bool checker = ((x / 8) + (y / 8)) & 1;
            const f32 cx = static_cast<f32>(x) - kTexSize * 0.5f;
            const f32 cy = static_cast<f32>(y) - kTexSize * 0.5f;
            const f32 r2 = cx * cx + cy * cy;
            const bool circle = r2 < (kTexSize * 0.25f) * (kTexSize * 0.25f);

            u8 R, G, B;
            if (circle) {            R = 230; G = 200; B = 20;  }   // 中央: 山吹色の月
            else if (checker)      { R = 60;  G = 100; B = 220; }   // 青
            else                   { R = 230; G = 230; B = 230; }   // 白

            const usize idx = static_cast<usize>(y * kTexSize + x) * 4;
            dst[idx + 0] = R;
            dst[idx + 1] = G;
            dst[idx + 2] = B;
            dst[idx + 3] = 255;
        }
    }
}

} // namespace hellotextured
