// SPDX-License-Identifier: Apache-2.0
// foundation/Cast.h の検証: RTTI 不使用の Cast<T> / CastChecked<T> / IsA + checkf。
//
// 失敗パス (CastChecked / checkf の panic) は FPanic がプロセスを終了させるため
// ユニットテストでは踏めない。ここでは成功パス・nullptr・型不一致 (nullptr 返却)・
// 階層判定・const オーバーロードを検証する。
#include "test/Test.h"
#include "test/Expect.h"
#include "foundation/Cast.h"

using namespace acs;

namespace {

// 3 階層 + 兄弟のトイ階層: FShape ← FRound ← FCircle、FShape ← FSquare。
struct FShape {
    virtual ~FShape() noexcept = default;
    ACS_RTTI_ROOT(FShape)
    int shape_tag = 1;
};
struct FRound : public FShape {
    ACS_RTTI(FRound, FShape)
    int round_tag = 2;
};
struct FCircle : public FRound {
    ACS_RTTI(FCircle, FRound)
    int circle_tag = 3;
};
struct FSquare : public FShape {
    ACS_RTTI(FSquare, FShape)
    int square_tag = 4;
};

} // namespace

ACS_TEST(Cast, DowncastToActualType) {
    FCircle circle;
    FShape* base = &circle;
    FCircle* c = Cast<FCircle>(base);
    EXPECT_TRUE(c != nullptr);
    EXPECT_TRUE(c == &circle);
    EXPECT_EQ(c->circle_tag, 3);
}

ACS_TEST(Cast, DowncastToIntermediateAncestor) {
    FCircle circle;
    FShape* base = &circle;
    // FCircle is-a FRound is-a FShape — どちらも成功する。
    EXPECT_TRUE(Cast<FRound>(base) != nullptr);
    EXPECT_TRUE(Cast<FShape>(base) != nullptr);
}

ACS_TEST(Cast, MismatchReturnsNull) {
    FCircle circle;
    FShape* base = &circle;
    // FCircle は FSquare ではない → nullptr。
    EXPECT_TRUE(Cast<FSquare>(base) == nullptr);

    FSquare square;
    FShape* b2 = &square;
    EXPECT_TRUE(Cast<FCircle>(b2) == nullptr);
    EXPECT_TRUE(Cast<FRound>(b2) == nullptr);
    EXPECT_TRUE(Cast<FSquare>(b2) != nullptr);
}

ACS_TEST(Cast, NullSourceReturnsNull) {
    FShape* base = nullptr;
    EXPECT_TRUE(Cast<FCircle>(base) == nullptr);
}

ACS_TEST(Cast, IsAHierarchyWalk) {
    FCircle circle;
    const FShape* base = &circle;
    EXPECT_TRUE(IsA<FCircle>(base));
    EXPECT_TRUE(IsA<FRound>(base));
    EXPECT_TRUE(IsA<FShape>(base));
    EXPECT_FALSE(IsA<FSquare>(base));

    FShape* nul = nullptr;
    EXPECT_FALSE(IsA<FShape>(nul));   // null は常に false
}

ACS_TEST(Cast, CastCheckedSuccessPath) {
    FCircle circle;
    FShape* base = &circle;
    FCircle* c = CastChecked<FCircle>(base);   // 成功 → 非 null
    EXPECT_TRUE(c == &circle);
    EXPECT_EQ(c->circle_tag, 3);
}

ACS_TEST(Cast, ConstOverload) {
    FCircle circle;
    const FShape* base = &circle;
    const FCircle* c = Cast<FCircle>(base);
    EXPECT_TRUE(c != nullptr);
    EXPECT_TRUE(Cast<FSquare>(base) == nullptr);
    const FCircle* cc = CastChecked<FCircle>(base);
    EXPECT_TRUE(cc == &circle);
}

ACS_TEST(Cast, ClassIdsAreDistinctAndStable) {
    EXPECT_TRUE(FCircle::StaticClassId() != FSquare::StaticClassId());
    EXPECT_TRUE(FCircle::StaticClassId() != FRound::StaticClassId());
    EXPECT_TRUE(FCircle::StaticClassId() == FCircle::StaticClassId());

    FCircle circle;
    FShape* base = &circle;
    // 動的型 ID は最派生 (FCircle) を返す。
    EXPECT_TRUE(base->GetClassId() == FCircle::StaticClassId());
}

ACS_TEST(Cast, CheckfSuccessPathDoesNotPanic) {
    // checkf は成功式なら何もしない (panic 路はプロセス終了のため未検証)。
    checkf(1 + 1 == 2, "math is broken: %d", 1 + 1);
    ACS_CHECK(true);
    EXPECT_TRUE(true);
}
