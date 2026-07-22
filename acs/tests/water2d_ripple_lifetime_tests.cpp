// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Effects2D.h"

using namespace acs;
using namespace acs::game;

ACS_TEST(Water2DRippleLifetime, ImpactPoolNeverOverwritesActiveRipples) {
    AWater2DComponent water;

    for (u32 i = 0; i < 16; ++i) {
        water.AddDisturbance(
            FVec2{static_cast<f32>(i), 0.0f}, 0.1f, 0.7f);
    }
    EXPECT_EQ(water.ActiveRippleCount(), 16u);

    water.OnUpdate(1.0f);
    water.AddDisturbance(FVec2{100.0f, 0.0f}, 0.1f, 0.7f);
    EXPECT_EQ(water.ActiveRippleCount(), 16u);

    // 満杯時に active 枠を上書きしていれば、1 秒遅れで追加した波紋だけが残る。
    water.OnUpdate(2.01f);
    EXPECT_EQ(water.ActiveRippleCount(), 0u);
}

ACS_TEST(Water2DRippleLifetime, NegativeStrengthDecaysNaturally) {
    AWater2DComponent water;
    water.AddDisturbance(FVec2{0.0f, 0.0f}, 0.1f, -0.7f);
    EXPECT_EQ(water.ActiveRippleCount(), 1u);

    // Signed amplitude controls crest/trough phase; its sign must not end the
    // ripple on the first update.
    water.OnUpdate(0.01f);
    EXPECT_EQ(water.ActiveRippleCount(), 1u);

    water.OnUpdate(3.0f);
    EXPECT_EQ(water.ActiveRippleCount(), 0u);
}

ACS_TEST(Water2DRippleLifetime, WakePoolCannotEraseImpactAndNeverOverwritesItself) {
    AWater2DComponent water;
    water.AddDisturbance(FVec2{0.0f, 0.0f}, 0.18f, 0.72f);

    for (u32 i = 0; i < 32; ++i) {
        water.AddWake(
            FVec2{static_cast<f32>(i), 0.0f},
            FVec2{4.0f, 0.0f}, 0.2f, 0.7f);
    }
    EXPECT_EQ(water.ActiveRippleCount(), 33u);

    water.OnUpdate(1.0f);
    for (u32 i = 0; i < 8; ++i) {
        water.AddWake(
            FVec2{100.0f + static_cast<f32>(i), 0.0f},
            FVec2{4.0f, 0.0f}, 0.2f, 0.7f);
    }
    EXPECT_EQ(water.ActiveRippleCount(), 33u);

    // 後発 wake が古い wake / impact を追い出していれば、ここで残存数が 0 にならない。
    water.OnUpdate(2.01f);
    EXPECT_EQ(water.ActiveRippleCount(), 0u);
}
