// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// render/SpriteSortList.h の検証:
//   FSpriteBatch 用の明示的 depth/layer 順序レイヤ — コマンドを layer/depth 付きで
//   積み、Sort() が (layer 昇順, depth 昇順, 挿入順) で安定ソートすることを確認する。
//   GPU 不要 (Sort/Ordered/Count は純 logic。Replay のみ GPU 依存なので非対象)。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "render/SpriteSortList.h"

using namespace acs;

// IRhiTexture は forward 宣言のみ。Sort/Ordered では一切 dereference しないので
// テクスチャ付きコマンドの検証用にダミーポインタ (非 null・非アクセス) を使う。
static IRhiTexture* FakeTex(unsigned long long tag) noexcept {
    return reinterpret_cast<IRhiTexture*>(static_cast<acs::usize>(tag));
}

// --- layer 昇順ソート (小さい層が先=奥) -----------------------------------
ACS_TEST(SpriteSortList, SortByLayer) {
    FSpriteSortList list;
    list.SubmitRect(0, 0, 1, 1, FVec4{1,0,0,1}, /*layer*/10, /*depth*/0);  // 手前
    list.SubmitRect(0, 0, 1, 1, FVec4{0,1,0,1}, /*layer*/0,  /*depth*/0);  // 奥
    list.SubmitRect(0, 0, 1, 1, FVec4{0,0,1,1}, /*layer*/5,  /*depth*/0);  // 中
    EXPECT_EQ(list.Count(), 3u);

    list.Sort();
    EXPECT_EQ(list.Ordered(0).layer, 0);
    EXPECT_EQ(list.Ordered(1).layer, 5);
    EXPECT_EQ(list.Ordered(2).layer, 10);
}

// --- 同 layer 内は depth 昇順 ---------------------------------------------
ACS_TEST(SpriteSortList, SortByDepthWithinLayer) {
    FSpriteSortList list;
    list.SubmitRect(0, 0, 1, 1, FVec4{1,1,1,1}, 1, 9.0f);
    list.SubmitRect(0, 0, 1, 1, FVec4{1,1,1,1}, 1, 1.0f);
    list.SubmitRect(0, 0, 1, 1, FVec4{1,1,1,1}, 1, 5.0f);
    list.Sort();
    EXPECT_NEAR(list.Ordered(0).depth, 1.0f, 1e-4f);
    EXPECT_NEAR(list.Ordered(1).depth, 5.0f, 1e-4f);
    EXPECT_NEAR(list.Ordered(2).depth, 9.0f, 1e-4f);
}

// --- layer が depth より優先 ----------------------------------------------
ACS_TEST(SpriteSortList, LayerBeatsDepth) {
    FSpriteSortList list;
    // layer 0 だが depth 大、layer 1 だが depth 小 → layer 0 が先。
    list.SubmitRect(0, 0, 1, 1, FVec4{1,1,1,1}, /*layer*/1, /*depth*/-100.0f);
    list.SubmitRect(0, 0, 1, 1, FVec4{1,1,1,1}, /*layer*/0, /*depth*/ 100.0f);
    list.Sort();
    EXPECT_EQ(list.Ordered(0).layer, 0);   // layer 優先
    EXPECT_EQ(list.Ordered(1).layer, 1);
}

// --- 同一 layer/depth は挿入順を保持 (安定) -------------------------------
ACS_TEST(SpriteSortList, StableForEqualKeys) {
    FSpriteSortList list;
    // 全て同じ layer/depth。color.x をマーカに挿入順を確認する。
    list.SubmitRect(0, 0, 1, 1, FVec4{0,0,0,1}, 0, 0.0f);
    list.SubmitRect(0, 0, 1, 1, FVec4{1,0,0,1}, 0, 0.0f);
    list.SubmitRect(0, 0, 1, 1, FVec4{2,0,0,1}, 0, 0.0f);
    list.SubmitRect(0, 0, 1, 1, FVec4{3,0,0,1}, 0, 0.0f);
    list.Sort();
    EXPECT_NEAR(list.Ordered(0).color.x, 0.0f, 1e-4f);
    EXPECT_NEAR(list.Ordered(1).color.x, 1.0f, 1e-4f);
    EXPECT_NEAR(list.Ordered(2).color.x, 2.0f, 1e-4f);
    EXPECT_NEAR(list.Ordered(3).color.x, 3.0f, 1e-4f);
}

// --- 混在 (Rect + Textured) コマンドのソートとフィールド保持 ---------------
ACS_TEST(SpriteSortList, MixedKindsAndFields) {
    FSpriteSortList list;
    IRhiTexture* t = FakeTex(0xABCD);
    list.SubmitSub(*t, 10, 20, 64, 48, 0.25f, 0.5f, 0.75f, 1.0f, /*layer*/2, /*depth*/3.0f,
                   FVec4{0.5f, 0.6f, 0.7f, 0.8f});
    list.SubmitRect(0, 0, 100, 32, FVec4{0,0,0,1}, /*layer*/0, /*depth*/0.0f);
    list.Sort();

    // layer 0 の Rect が先。
    const FSpriteCmd& c0 = list.Ordered(0);
    EXPECT_TRUE(c0.kind == ESpriteCmdKind::Rect);
    EXPECT_EQ(c0.layer, 0);
    EXPECT_TRUE(c0.tex == nullptr);

    // layer 2 の Textured が後。UV/サイズ/tex が保持されている。
    const FSpriteCmd& c1 = list.Ordered(1);
    EXPECT_TRUE(c1.kind == ESpriteCmdKind::Textured);
    EXPECT_EQ(c1.layer, 2);
    EXPECT_TRUE(c1.tex == t);
    EXPECT_NEAR(c1.x, 10.0f, 1e-4f);
    EXPECT_NEAR(c1.w, 64.0f, 1e-4f);
    EXPECT_NEAR(c1.u0, 0.25f, 1e-4f);
    EXPECT_NEAR(c1.v1, 1.0f, 1e-4f);
    EXPECT_NEAR(c1.color.y, 0.6f, 1e-4f);
}

// --- Clear と未ソート時 Ordered は提出順 -----------------------------------
ACS_TEST(SpriteSortList, ClearAndUnsortedOrder) {
    FSpriteSortList list;
    list.SubmitRect(0, 0, 1, 1, FVec4{7,0,0,1}, 3, 0.0f);
    list.SubmitRect(0, 0, 1, 1, FVec4{9,0,0,1}, 1, 0.0f);
    // Sort 前: Ordered は提出順。
    EXPECT_NEAR(list.Ordered(0).color.x, 7.0f, 1e-4f);
    EXPECT_NEAR(list.Ordered(1).color.x, 9.0f, 1e-4f);

    list.Sort();   // layer 1 が先 → color.x 9 が先頭。
    EXPECT_NEAR(list.Ordered(0).color.x, 9.0f, 1e-4f);

    list.Clear();
    EXPECT_EQ(list.Count(), 0u);
    // Clear 後に積み直すと seq も 0 起点 (安定性が次フレームに引きずられない)。
    list.SubmitRect(0, 0, 1, 1, FVec4{1,0,0,1}, 0, 0.0f);
    list.SubmitRect(0, 0, 1, 1, FVec4{2,0,0,1}, 0, 0.0f);
    list.Sort();
    EXPECT_NEAR(list.Ordered(0).color.x, 1.0f, 1e-4f);
    EXPECT_NEAR(list.Ordered(1).color.x, 2.0f, 1e-4f);
}

// --- 空リスト / 単一要素の Sort は安全 ------------------------------------
ACS_TEST(SpriteSortList, EmptyAndSingle) {
    FSpriteSortList empty;
    empty.Sort();                       // no-op、クラッシュ無し
    EXPECT_EQ(empty.Count(), 0u);

    FSpriteSortList one;
    one.SubmitRect(0, 0, 1, 1, FVec4{5,0,0,1}, 0, 0.0f);
    one.Sort();
    EXPECT_EQ(one.Count(), 1u);
    EXPECT_NEAR(one.Ordered(0).color.x, 5.0f, 1e-4f);
}
