// SPDX-License-Identifier: Apache-2.0
// HelloSpriteCollider — スプライトの alpha から形状コライダーを生成して検証する
// コンソールサンプル。手続き生成した画像 (円 / L 字) でコライダーを作り、
// 凸包・輪郭・AABB・内外判定をアサートする。視覚非依存・ヘッドレスで完結。
#include "collision/SpriteCollider.h"

#include <cstdio>
#include <cmath>

using namespace acs;

namespace {

constexpr u32 kW = 128, kH = 128;

void ClearImage(u8* img) noexcept {
    for (u32 i = 0; i < kW * kH * 4u; ++i) img[i] = 0;
}
void SetPixel(u8* img, u32 x, u32 y) noexcept {
    const u32 i = (y * kW + x) * 4u;
    img[i + 0] = 255; img[i + 1] = 255; img[i + 2] = 255; img[i + 3] = 255;
}
void FillCircle(u8* img, f32 cx, f32 cy, f32 r) noexcept {
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x < kW; ++x) {
            const f32 dx = static_cast<f32>(x) - cx, dy = static_cast<f32>(y) - cy;
            if (dx * dx + dy * dy <= r * r) SetPixel(img, x, y);
        }
}
void FillRect(u8* img, u32 x0, u32 y0, u32 x1, u32 y1) noexcept {
    for (u32 y = y0; y < y1; ++y)
        for (u32 x = x0; x < x1; ++x) SetPixel(img, x, y);
}
bool Near(f32 a, f32 b, f32 eps) noexcept { return std::fabs(a - b) < eps; }

} // namespace

int main() {
    std::puts("=== ACS HelloSpriteCollider ===");
    static u8 img[kW * kH * 4];
    bool ok = true;

    // ---- (1) 円 (凸) ----
    ClearImage(img);
    FillCircle(img, 64.0f, 64.0f, 40.0f);
    CSpriteCollider circle;
    if (circle.BuildFromAlpha(img, kW, kH, 128, 1.5f).IsErr()) {
        std::puts("circle: build FAILED"); return 2;
    }
    const FAabb2 cb = circle.Bounds();
    const bool c_in   = circle.ContainsPoint({64, 64});
    const bool c_out  = circle.ContainsPoint({5, 5});
    const bool c_band = Near(cb.center.x, 64, 2) && Near(cb.center.y, 64, 2) &&
                        Near(cb.half_size.x, 40, 2) && Near(cb.half_size.y, 40, 2);
    const bool c_hull = circle.HullCount() >= 8;   // 円の凸包は多角形
    std::printf("  circle : hull=%u outline=%u bounds(c=%.0f,%.0f hs=%.0f) in=%d out=%d\n",
                circle.HullCount(), circle.OutlineCount(),
                cb.center.x, cb.center.y, cb.half_size.x, c_in ? 1 : 0, c_out ? 1 : 0);
    ok = ok && c_in && !c_out && c_band && c_hull;

    // ---- (2) L 字 (凹、輪郭が凸包と異なるべき) ----
    ClearImage(img);
    FillRect(img, 20, 20, 50, 100);    // 縦棒
    FillRect(img, 20, 70, 100, 100);   // 横棒  → 右上 (55..100, 20..65) が欠ける
    CSpriteCollider lshape;
    if (lshape.BuildFromAlpha(img, kW, kH, 128, 1.5f).IsErr()) {
        std::puts("L: build FAILED"); return 3;
    }
    const bool l_vbar = lshape.ContainsPoint({35, 40});   // 縦棒内 → true
    const bool l_hbar = lshape.ContainsPoint({80, 85});   // 横棒内 → true
    const bool l_notch = lshape.ContainsPoint({80, 40});  // 欠け部分 → false (凹の証明)
    std::printf("  L-shape: outline=%u  vbar=%d hbar=%d notch=%d (notch should be 0)\n",
                lshape.OutlineCount(), l_vbar ? 1 : 0, l_hbar ? 1 : 0, l_notch ? 1 : 0);
    ok = ok && l_vbar && l_hbar && !l_notch;

    std::printf("\nresult: %s\n", ok ? "ALL PASS" : "FAILED");
    return ok ? 0 : 1;
}
