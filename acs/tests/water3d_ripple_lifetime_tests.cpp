// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "render/WaterSurface3D.h"

using namespace acs;

ACS_TEST(Water3DRippleLifetime, ReservedPoolsNeverOverwriteEachOther) {
    FWaterSurface3D water;
    FWaterSurface3DParams params{};
    params.ripple_lifetime = 1.0f;
    params.ripple_damping = 0.0f;
    water.SetParams(params);

    for (u32 i = 0; i < FWaterSurface3D::kImpactRippleSlots; ++i) {
        EXPECT_TRUE(water.AddDisturbance(
            FVec3{static_cast<f32>(i), 0.0f, 0.0f}, 0.15f, 0.20f));
    }
    EXPECT_FALSE(water.AddDisturbance(
        FVec3{100.0f, 0.0f, 0.0f}, 0.15f, 0.20f));
    EXPECT_EQ(
        water.ActiveRippleCount(), FWaterSurface3D::kImpactRippleSlots);

    for (u32 i = 0; i < FWaterSurface3D::kWakeRippleSlots; ++i) {
        EXPECT_TRUE(water.AddWake(
            FVec3{static_cast<f32>(i), 0.0f, 1.0f},
            FVec3{4.0f, 0.0f, 1.0f}, 0.20f, 0.18f));
    }
    EXPECT_FALSE(water.AddWake(
        FVec3{200.0f, 0.0f, 0.0f},
        FVec3{4.0f, 0.0f, 1.0f}, 0.20f, 0.18f));
    EXPECT_EQ(water.ActiveRippleCount(), FWaterSurface3D::kMaxRipples);
}

ACS_TEST(Water3DRippleLifetime, UpdateConsumesTheFullDeltaTime) {
    FWaterSurface3D water;
    FWaterSurface3DParams params{};
    params.ripple_lifetime = 0.75f;
    params.ripple_damping = 0.0f;
    water.SetParams(params);

    EXPECT_TRUE(water.AddDisturbance(
        FVec3{0.0f, 0.0f, 0.0f}, 0.15f, 0.20f));
    EXPECT_TRUE(water.AddWake(
        FVec3{1.0f, 0.0f, 0.0f},
        FVec3{2.0f, 0.0f, 0.0f}, 0.20f, 0.18f));
    water.Update(0.80f);

    EXPECT_NEAR(water.Time(), 0.80f, 1e-6f);
    EXPECT_EQ(water.ActiveRippleCount(), 0u);
}
