// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework/DebugDraw (DrawArrow) + RigidWorldDebug の検証:
//   描画副作用を持たない線分蓄積バッファなので、積まれた本数を検査する。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/DebugDraw.h"
#include "gameframework/RigidWorld2D.h"
#include "gameframework/RigidWorldDebug.h"
#include "math/Vec.h"

using namespace acs;
using namespace acs::game;

// 矢印は軸 1 本 + 矢じり 2 本 = 3 線。退化 (始点==終点) は軸のみ。
ACS_TEST(DebugDraw, ArrowEmitsThreeLines) {
    CDebugDraw dd;
    dd.DrawArrow(FVec2{ 0.0f, 0.0f }, FVec2{ 1.0f, 0.0f }, FVec4{ 1.0f, 1.0f, 1.0f, 1.0f });
    EXPECT_EQ(dd.LineCount(), 3u);

    dd.Clear();
    dd.DrawArrow(FVec2{ 2.0f, 2.0f }, FVec2{ 2.0f, 2.0f }, FVec4{ 1.0f, 1.0f, 1.0f, 1.0f });
    EXPECT_EQ(dd.LineCount(), 1u);   // 退化 → 軸のみ
}

// 剛体ワールド可視化: 円24線 + 箱4線、速度矢印は動的ボディのみ、削除済みは描かない。
ACS_TEST(DebugDraw, RigidWorldVisualizationLineCounts) {
    const FVec4 col{ 1.0f, 1.0f, 1.0f, 1.0f };
    const FVec4 vcol{ 1.0f, 0.0f, 0.0f, 1.0f };

    // 静的箱 (4) + 静的円 (24)、速度なし → 28。
    CRigidWorld2D w;
    w.AddStaticAabb(FVec2{ 5.0f, 0.0f }, FVec2{ 1.0f, 1.0f });
    w.AddCircle(FVec2{ 0.0f, 0.0f }, 1.0f, 0.0f);
    CDebugDraw dd;
    DebugDrawRigidWorld(w, dd, col, vcol);
    EXPECT_EQ(dd.LineCount(), 28u);

    // 動的円 + 速度 → 円24 + 矢印3 = 27。
    CRigidWorld2D w2;
    const u32 d = w2.AddCircle(FVec2{ 0.0f, 0.0f }, 1.0f, 1.0f);
    w2.SetVelocity(d, FVec2{ 3.0f, 0.0f });
    CDebugDraw dd2;
    DebugDrawRigidWorld(w2, dd2, col, vcol);
    EXPECT_EQ(dd2.LineCount(), 27u);

    // 削除済みボディは描かれない (箱のみ 4)。
    CRigidWorld2D w3;
    const u32 a = w3.AddCircle(FVec2{ 0.0f, 0.0f }, 1.0f, 0.0f);
    w3.AddStaticAabb(FVec2{ 5.0f, 0.0f }, FVec2{ 1.0f, 1.0f });
    w3.RemoveBody(a);
    CDebugDraw dd3;
    DebugDrawRigidWorld(w3, dd3, col, vcol);
    EXPECT_EQ(dd3.LineCount(), 4u);
}
