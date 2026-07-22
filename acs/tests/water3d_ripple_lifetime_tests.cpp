// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "render/WaterSurface3D.h"

#include <cmath>
#include <limits>

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

ACS_TEST(Water3DRippleLifetime, MalformedEventsNeverPoisonPersistentPool) {
    FWaterSurface3D water;
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();

    EXPECT_FALSE(water.AddDisturbance(FVec3{nan, 0.0f, 0.0f},
                                      0.2f, 0.3f));
    EXPECT_FALSE(water.AddDisturbance(FVec3{0.0f, 0.0f, 0.0f},
                                      infinity, 0.3f));
    EXPECT_FALSE(water.AddDisturbance(FVec3{0.0f, 0.0f, 0.0f},
                                      0.2f, 0.0f));
    EXPECT_FALSE(water.AddWake(FVec3{0.0f, 0.0f, 0.0f},
                               FVec3{infinity, 0.0f, 0.0f},
                               0.2f, 0.3f));
    EXPECT_EQ(water.ActiveRippleCount(), 0u);

    EXPECT_TRUE(water.AddDisturbance(FVec3{0.0f, 0.0f, 0.0f},
                                     0.2f, 0.3f));
    EXPECT_EQ(water.ActiveRippleCount(), 1u);
}

ACS_TEST(Water3DRippleLifetime, AuthoringParamsAreFiniteAndPhysical) {
    FWaterSurface3D water;
    FWaterSurface3DParams params{};
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();
    params.shallow_color = FVec3{-1.0f, nan, infinity};
    params.absorption = FVec3{-2.0f, nan, 0.25f};
    params.scattering = FVec3{infinity, -1.0f, 0.08f};
    params.phase_anisotropy = 4.0f;
    params.flow_direction = FVec2{nan, infinity};
    params.roughness = -1.0f;
    params.normal_strength = infinity;
    params.ripple_wavelength = 0.0f;
    params.ripple_lifetime = -4.0f;
    params.ripple_damping = nan;
    water.SetParams(params);

    const FWaterSurface3DParams& safe = water.Params();
    EXPECT_NEAR(safe.shallow_color.x, 0.0f, 1e-6f);
    EXPECT_TRUE(std::isfinite(safe.shallow_color.y));
    EXPECT_TRUE(std::isfinite(safe.shallow_color.z));
    EXPECT_NEAR(safe.absorption.x, 0.0f, 1e-6f);
    EXPECT_TRUE(std::isfinite(safe.absorption.y));
    EXPECT_NEAR(safe.absorption.z, 0.25f, 1e-6f);
    EXPECT_TRUE(std::isfinite(safe.scattering.x));
    EXPECT_NEAR(safe.scattering.y, 0.0f, 1e-6f);
    EXPECT_NEAR(safe.scattering.z, 0.08f, 1e-6f);
    EXPECT_NEAR(safe.phase_anisotropy, 0.95f, 1e-6f);
    EXPECT_NEAR(safe.roughness, 0.02f, 1e-6f);
    EXPECT_TRUE(std::isfinite(safe.normal_strength));
    EXPECT_NEAR(safe.ripple_wavelength, 0.025f, 1e-6f);
    EXPECT_NEAR(safe.ripple_lifetime, 0.1f, 1e-6f);
    EXPECT_TRUE(std::isfinite(safe.ripple_damping));
    EXPECT_NEAR(safe.flow_direction.x * safe.flow_direction.x
                    + safe.flow_direction.y * safe.flow_direction.y,
                1.0f, 1e-5f);
}

ACS_TEST(Water3DRippleLifetime, HugeFiniteFlowDirectionNormalizesWithoutOverflow) {
    FWaterSurface3D water;
    FWaterSurface3DParams params{};
    const f32 maximum = std::numeric_limits<f32>::max();
    params.flow_direction = FVec2{maximum, maximum};
    water.SetParams(params);

    const FVec2 flow = water.Params().flow_direction;
    EXPECT_TRUE(std::isfinite(flow.x));
    EXPECT_TRUE(std::isfinite(flow.y));
    EXPECT_NEAR(flow.x, 0.70710678f, 1e-5f);
    EXPECT_NEAR(flow.y, 0.70710678f, 1e-5f);
}

ACS_TEST(Water3DRippleLifetime, HugeFiniteDeltaNeverPoisonsAnimationState) {
    FWaterSurface3D water;
    FWaterSurface3DParams params{};
    params.ripple_lifetime = 3600.0f;
    params.ripple_damping = 0.0f;
    water.SetParams(params);

    EXPECT_TRUE(water.AddDisturbance(
        FVec3{0.0f, 0.0f, 0.0f},
        std::numeric_limits<f32>::max(),
        std::numeric_limits<f32>::max()));
    water.Update(std::numeric_limits<f32>::max());
    EXPECT_TRUE(std::isfinite(water.Time()));
    EXPECT_TRUE(water.Time() >= 0.0f);
    EXPECT_TRUE(water.Time() < 65536.0f);
    water.Update(std::numeric_limits<f32>::max());

    EXPECT_TRUE(std::isfinite(water.Time()));
    EXPECT_TRUE(water.Time() >= 0.0f);
    EXPECT_TRUE(water.Time() < 65536.0f);
    EXPECT_EQ(water.ActiveRippleCount(), 0u);
}

ACS_TEST(Water3DRippleLifetime, HugeFiniteEventCoordinatesStayBounded) {
    FWaterSurface3D water;
    const f32 maximum = std::numeric_limits<f32>::max();

    EXPECT_TRUE(water.AddDisturbance(
        FVec3{maximum, -maximum, maximum}, maximum, maximum));
    EXPECT_EQ(water.ActiveRippleCount(), 1u);

    // The wake offset is computed before the event reaches the shared pool;
    // overflow there must be rejected without consuming a reserved wake slot.
    EXPECT_FALSE(water.AddWake(
        FVec3{-maximum, 0.0f, -maximum},
        FVec3{maximum, 0.0f, maximum}, maximum, 1.0f));
    EXPECT_EQ(water.ActiveRippleCount(), 1u);
}
