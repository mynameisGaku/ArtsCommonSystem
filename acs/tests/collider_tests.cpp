// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Collision — CSpriteCollider (alpha → 凸包 / 輪郭 / AABB) 純 CPU ロジック
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "collision/MeshCollider.h"
#include "collision/SpriteCollider.h"

using namespace acs;

namespace {

static_assert(IsSameV<FMeshCollider, CMeshCollider>, "旧メッシュコライダー名は正規型の互換別名である必要があります");
static_assert(IsSameV<FSpriteCollider, CSpriteCollider>, "旧スプライトコライダー名は正規型の互換別名である必要があります");

/** 16x16 RGBA8 画像。FillRect で矩形領域を不透明 (alpha=255) にする簡易ヘルパ。 */
struct FImg {
    static constexpr u32 kW = 16;
    static constexpr u32 kH = 16;
    u8 px[kW * kH * 4] = {};

    /** [x0,x1) × [y0,y1) の画素の alpha を 255 にする。 */
    void FillRect(u32 x0, u32 y0, u32 x1, u32 y1) noexcept {
        for (u32 y = y0; y < y1; ++y)
            for (u32 x = x0; x < x1; ++x)
                px[(y * kW + x) * 4 + 3] = 255;
    }
};

} // namespace

// ---- 8x8 不透明ブロック → 凸包と AABB ------------------------------------
ACS_TEST(SpriteCollider, BuildSquareHullBounds) {
    FImg img;
    img.FillRect(4, 4, 12, 12);  // 画素 4..11 (8x8)
    CSpriteCollider col;
    auto r = col.BuildFromAlpha(img.px, FImg::kW, FImg::kH, 128, 1.5f);
    EXPECT_TRUE(r.IsOk());
    EXPECT_TRUE(col.HullCount() >= 3);
    EXPECT_TRUE(col.HullCount() <= CSpriteCollider::kMaxVertices);

    const FAabb2 b = col.Bounds();           // 不透明ブロックを包む AABB
    EXPECT_TRUE(b.center.x > 6.0f && b.center.x < 10.0f);
    EXPECT_TRUE(b.center.y > 6.0f && b.center.y < 10.0f);
    EXPECT_TRUE(b.half_size.x >= 3.0f && b.half_size.x <= 5.0f);
    EXPECT_TRUE(b.half_size.y >= 3.0f && b.half_size.y <= 5.0f);
}

// ---- ContainsPoint: 内側 true / 外側 false --------------------------------
ACS_TEST(SpriteCollider, ContainsPoint) {
    FImg img;
    img.FillRect(4, 4, 12, 12);
    CSpriteCollider col;
    EXPECT_TRUE(col.BuildFromAlpha(img.px, FImg::kW, FImg::kH).IsOk());
    EXPECT_TRUE(col.ContainsPoint(FVec2{8.0f, 8.0f}));    // 中心
    EXPECT_FALSE(col.ContainsPoint(FVec2{0.0f, 0.0f}));   // 左上の外
    EXPECT_FALSE(col.ContainsPoint(FVec2{15.0f, 15.0f})); // 右下の外
}

// ---- 不正入力: null / 全透明 (境界画素不足) はエラー ----------------------
ACS_TEST(SpriteCollider, RejectsBadInput) {
    CSpriteCollider col;
    EXPECT_TRUE(col.BuildFromAlpha(nullptr, 16, 16).IsErr());   // null 画像
    FImg empty;                                                  // 全透明
    EXPECT_TRUE(col.BuildFromAlpha(empty.px, FImg::kW, FImg::kH).IsErr());
}
