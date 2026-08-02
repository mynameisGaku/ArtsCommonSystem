// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework/Draw (batch を持たない即時描画) の安全境界を固定する:
//   描画パスの外や sprite batch 未配線のコンテキストでは、全関数が «何もせずに
//   戻る» ことを保証する。GPU 無しで踏めるのはこの契約なので、ここで固定する。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Draw.h"
#include "gameframework/RenderContext.h"
#include "math/Vec.h"

using namespace acs;
using namespace acs::game;

namespace {

/** 公開している描画関数を一通り呼ぶ (戻り値は無く、落ちないことが検査対象)。 */
void CallEveryDrawFunction() noexcept {
    const FVec4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
    DrawRect(0.0f, 0.0f, 10.0f, 10.0f, color);
    DrawRectOutline(0.0f, 0.0f, 10.0f, 10.0f, color, 2.0f);
    DrawRectRotated(5.0f, 5.0f, 10.0f, 10.0f, 0.5f, color);
    DrawCircle(5.0f, 5.0f, 4.0f, color);
    DrawCircleOutline(5.0f, 5.0f, 4.0f, color, 1.0f);
    DrawLine(0.0f, 0.0f, 10.0f, 10.0f, color, 1.0f);
    DrawTriangle(0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 10.0f, color);
    DrawString(0.0f, 0.0f, "text", color);
    DrawString(0.0f, 0.0f, nullptr, color);
}

} // namespace

// 描画パスの外では publish が無いので、描かないし落ちない。
ACS_TEST(Draw, WithoutContextEveryCallIsSafe) {
    _SetDrawContext(nullptr);
    EXPECT_TRUE(_CurrentDrawContext() == nullptr);
    EXPECT_TRUE(!IsDrawing());
    EXPECT_EQ(DrawWidth(), 0u);
    EXPECT_EQ(DrawHeight(), 0u);
    CallEveryDrawFunction();
}

// publish 済みでも batch が未配線なら «描ける» とは言わない (3D シーンの描画パス等)。
ACS_TEST(Draw, PublishedContextWithoutSpriteBatchDoesNotDraw) {
    FRenderContext context;
    _SetDrawContext(&context);
    EXPECT_TRUE(_CurrentDrawContext() == &context);
    EXPECT_TRUE(!context.HasSprites());
    EXPECT_TRUE(!IsDrawing());
    CallEveryDrawFunction();
    _SetDrawContext(nullptr);
    EXPECT_TRUE(_CurrentDrawContext() == nullptr);
}

// 退化した引数 (0 半径 / 0 長さ / 負の太さ) を渡しても安全に無視する。
ACS_TEST(Draw, DegenerateArgumentsAreIgnored) {
    FRenderContext context;
    _SetDrawContext(&context);
    const FVec4 color{ 1.0f, 0.0f, 0.0f, 1.0f };
    DrawCircle(0.0f, 0.0f, 0.0f, color);
    DrawCircle(0.0f, 0.0f, -1.0f, color, 0u);
    DrawCircleOutline(0.0f, 0.0f, 1.0f, color, -1.0f);
    DrawLine(3.0f, 3.0f, 3.0f, 3.0f, color, 1.0f);
    DrawRectOutline(0.0f, 0.0f, 4.0f, 4.0f, color, 0.0f);
    _SetDrawContext(nullptr);
}
