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

#include <limits>

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

ACS_TEST(SpriteSortList, CompileTimeKeyLayoutPlacesPriorityFields)
{
    /** pipeline、material、depthを表す検証layout。 */
    using FLayout = TDrawPacketSortKeyLayout<12u, 20u, 32u>;
    static_assert(FLayout::FieldCount() == 3u);
    static_assert(FLayout::TotalBits() == 64u);
    static_assert(FLayout::FieldBits<0u>() == 12u);
    static_assert(FLayout::FieldShift<0u>() == 52u);
    static_assert(FLayout::FieldShift<1u>() == 32u);
    static_assert(FLayout::FieldShift<2u>() == 0u);
    /** 各fieldへ配置した検証key。 */
    constexpr u64 kPacked = FLayout::Insert<0u>(0xabcu) | FLayout::Insert<1u>(0x54321u) | FLayout::Insert<2u>(0x89abcdefu);
    /** 64 bit field一つだけを持つ境界layout。 */
    using FFullWidthLayout = TDrawPacketSortKeyLayout<64u>;
    static_assert(kPacked == 0xabc5432189abcdefull);
    static_assert(FFullWidthLayout::Insert<0u>(~u64{0}) == ~u64{0});
    EXPECT_EQ(kPacked, 0xabc5432189abcdefull);
}

ACS_TEST(SpriteSortList, RadixSortPreservesOrderWithLinearBound)
{
    /** radix経路を十分に通すcommand数。 */
    constexpr u32 kCommandCount = 4096u;
    FSpriteSortList list;
    list.Reserve(kCommandCount);
    for (u32 index = 0u; index < kCommandCount; ++index) {
        /** 正負を含む再現可能なlayer。 */
        const i32 layer = static_cast<i32>((index * 37u) % 17u) - 8;
        /** 同一keyを繰り返す再現可能なdepth。 */
        const f32 depth = static_cast<f32>(static_cast<i32>((index * 29u) % 23u) - 11);
        list.SubmitRect(0.0f, 0.0f, 1.0f, 1.0f, FVec4{static_cast<f32>(index), 0.0f, 0.0f, 1.0f}, layer, depth);
    }

    list.Sort();
    EXPECT_EQ(list.Count(), kCommandCount);
    EXPECT_TRUE(list.LastSortPassCount() > 0u);
    EXPECT_TRUE(list.LastSortPassCount() <= 8u);
    EXPECT_TRUE(list.LastSortItemVisits() <= static_cast<u64>(kCommandCount) * 18u);
    for (u32 index = 1u; index < kCommandCount; ++index) {
        /** 一つ前の整列済みcommand。 */
        const FSpriteCmd& previous = list.Ordered(index - 1u);
        /** 現在の整列済みcommand。 */
        const FSpriteCmd& current = list.Ordered(index);
        EXPECT_TRUE(previous.layer < current.layer || (previous.layer == current.layer && previous.depth <= current.depth));
        if (previous.layer == current.layer && previous.depth == current.depth) EXPECT_TRUE(previous.color.x < current.color.x);
    }
}

ACS_TEST(SpriteSortList, SignedZeroAndNanOrderIsDeterministic)
{
    FSpriteSortList list;
    /** NaNを含む入力値。 */
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    /** 正の無限大を含む入力値。 */
    const f32 infinity = std::numeric_limits<f32>::infinity();
    list.SubmitRect(0, 0, 1, 1, FVec4{0, 0, 0, 1}, 0, nan);
    list.SubmitRect(0, 0, 1, 1, FVec4{1, 0, 0, 1}, 0, 0.0f);
    list.SubmitRect(0, 0, 1, 1, FVec4{2, 0, 0, 1}, 0, -0.0f);
    list.SubmitRect(0, 0, 1, 1, FVec4{3, 0, 0, 1}, 0, -3.0f);
    list.SubmitRect(0, 0, 1, 1, FVec4{4, 0, 0, 1}, 0, infinity);
    list.SubmitRect(0, 0, 1, 1, FVec4{5, 0, 0, 1}, 0, nan);

    list.Sort();
    EXPECT_NEAR(list.Ordered(0u).color.x, 3.0f, 1e-4f);
    EXPECT_NEAR(list.Ordered(1u).color.x, 1.0f, 1e-4f);
    EXPECT_NEAR(list.Ordered(2u).color.x, 2.0f, 1e-4f);
    EXPECT_NEAR(list.Ordered(3u).color.x, 0.0f, 1e-4f);
    EXPECT_NEAR(list.Ordered(4u).color.x, 4.0f, 1e-4f);
    EXPECT_NEAR(list.Ordered(5u).color.x, 5.0f, 1e-4f);
}

ACS_TEST(SpriteSortList, RadixSortCoversFullFloatDomainAndStableTies)
{
    /** 有限値、非正規化数、無限大、NaN、符号付きzeroを混在させるdepth集合。 */
    const f32 depth_cases[] = {std::numeric_limits<f32>::quiet_NaN(), -std::numeric_limits<f32>::denorm_min(), std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::infinity(), 0.0f, -std::numeric_limits<f32>::max(), std::numeric_limits<f32>::infinity(), -0.0f, std::numeric_limits<f32>::min(), -1.0f, std::numeric_limits<f32>::denorm_min(), -std::numeric_limits<f32>::min(), 1.0f, -std::numeric_limits<f32>::quiet_NaN()};
    /** radix経路と同値keyの安定性を同時に通すcommand数。 */
    constexpr u32 kCommandCount = 56u;
    /** depth集合の要素数。 */
    constexpr u32 kDepthCaseCount = static_cast<u32>(sizeof(depth_cases) / sizeof(depth_cases[0]));
    /** full-float順序を検証するsprite sort list。 */
    FSpriteSortList list;
    /** 数値比較で作る独立した期待提出番号列。 */
    u32 expected[kCommandCount]{};
    for (u32 index = 0u; index < kCommandCount; ++index) {
        expected[index] = index;
        list.SubmitRect(0.0f, 0.0f, 1.0f, 1.0f, FVec4{static_cast<f32>(index), 0.0f, 0.0f, 1.0f}, 0, depth_cases[index % kDepthCaseCount]);
    }

    /** NaNを正の無限大へ寄せる公開順序契約。 */
    const auto normalized_depth = [&](u32 submission_index) noexcept {
        /** 提出番号に対応するdepth。 */
        const f32 depth = depth_cases[submission_index % kDepthCaseCount];
        return depth != depth ? std::numeric_limits<f32>::infinity() : depth;
    };
    for (u32 index = 1u; index < kCommandCount; ++index) {
        /** 安定挿入する提出番号。 */
        const u32 inserted = expected[index];
        /** 挿入する正規化済みdepth。 */
        const f32 inserted_depth = normalized_depth(inserted);
        /** 期待列内の挿入位置。 */
        u32 position = index;
        while (position > 0u && normalized_depth(expected[position - 1u]) > inserted_depth) {
            expected[position] = expected[position - 1u];
            --position;
        }
        expected[position] = inserted;
    }

    list.Sort();
    EXPECT_TRUE(list.LastSortPassCount() > 0u);
    EXPECT_TRUE(list.LastSortPassCount() <= 8u);
    for (u32 index = 0u; index < kCommandCount; ++index) {
        EXPECT_NEAR(list.Ordered(index).color.x, static_cast<f32>(expected[index]), 1e-4f);
    }
}
