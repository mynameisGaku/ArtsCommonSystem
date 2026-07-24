// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "asset/MeshAsset.h"
#include "render/WaterSurface3D.h"

#include <cmath>
#include <limits>

using namespace acs;

namespace {

void AddWaterMeshVertex(
    FMeshAsset& mesh, FVec3 position,
    FVec3 normal = FVec3{0.0f, 1.0f, 0.0f}) {
    mesh.Vertices().PushBack(
        FMeshVertex{position, normal, 0.0f, 0.0f});
}

void AddWaterMeshQuadIndices(FMeshAsset& mesh) {
    mesh.Indices().PushBack(0u);
    mesh.Indices().PushBack(1u);
    mesh.Indices().PushBack(2u);
    mesh.Indices().PushBack(0u);
    mesh.Indices().PushBack(2u);
    mesh.Indices().PushBack(3u);
}

} // namespace

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

ACS_TEST(Water3DRippleLifetime, FullImpactPoolDropsNewEventWithoutRefreshingOldOne) {
    FWaterSurface3D water;
    FWaterSurface3DParams params{};
    params.ripple_lifetime = 1.0f;
    params.ripple_damping = 0.0f;
    water.SetParams(params);

    for (u32 i = 0; i < FWaterSurface3D::kImpactRippleSlots; ++i) {
        EXPECT_TRUE(water.AddDisturbance(
            FVec3{static_cast<f32>(i), 0.0f, 0.0f},
            0.15f, 0.20f));
    }
    water.Update(0.40f);
    EXPECT_FALSE(water.AddDisturbance(
        FVec3{100.0f, 0.0f, 0.0f}, 0.15f, 0.20f));
    water.Update(0.61f);

    // An overwrite/refresh implementation would leave the last inserted
    // event alive for another 0.39 seconds. Persistence requires all original
    // events to reach their own continuous endpoint instead.
    EXPECT_EQ(water.ActiveRippleCount(), 0u);
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

ACS_TEST(Water3DRippleLifetime, LifetimeTailIsContinuousAndMonotonic) {
    constexpr f32 lifetime = 4.0f;
    f32 previous = 1.0f;
    for (u32 sample = 0; sample <= 400u; ++sample) {
        const f32 age = lifetime * static_cast<f32>(sample) / 400.0f;
        const f32 scale = FWaterSurface3D::EvaluateRippleAmplitudeScale(
            age, lifetime, 0.0f);
        EXPECT_TRUE(std::isfinite(scale));
        EXPECT_TRUE(scale >= 0.0f);
        EXPECT_TRUE(scale <= previous + 1e-6f);
        previous = scale;
    }

    // The physical response is unchanged until the final 35% of the lifetime.
    EXPECT_NEAR(FWaterSurface3D::EvaluateRippleAmplitudeScale(
                    lifetime * 0.65f, lifetime, 0.0f),
                1.0f, 1e-6f);
    EXPECT_TRUE(FWaterSurface3D::EvaluateRippleAmplitudeScale(
                    lifetime * 0.90f, lifetime, 0.0f) > 0.0f);
    EXPECT_NEAR(FWaterSurface3D::EvaluateRippleAmplitudeScale(
                    lifetime, lifetime, 0.0f),
                0.0f, 1e-7f);

    // Smootherstep has zero slope at release. A finite-difference sample near
    // the endpoint must therefore already be visually negligible.
    EXPECT_TRUE(FWaterSurface3D::EvaluateRippleAmplitudeScale(
                    lifetime * 0.999f, lifetime, 0.0f) < 1e-6f);

    const f32 h = lifetime * 0.001f;
    const f32 at_end = FWaterSurface3D::EvaluateRippleAmplitudeScale(
        lifetime, lifetime, 0.0f);
    const f32 before_end = FWaterSurface3D::EvaluateRippleAmplitudeScale(
        lifetime - h, lifetime, 0.0f);
    const f32 twice_before_end =
        FWaterSurface3D::EvaluateRippleAmplitudeScale(
            lifetime - 2.0f * h, lifetime, 0.0f);
    const f32 endpoint_slope = (at_end - before_end) / h;
    const f32 endpoint_curvature =
        (at_end - 2.0f * before_end + twice_before_end) / (h * h);
    EXPECT_TRUE(std::abs(endpoint_slope) < 1e-4f);
    EXPECT_TRUE(std::abs(endpoint_curvature) < 0.12f);
}

ACS_TEST(Water3DRippleLifetime, SmallRippleFadesBeforeSlotIsReleased) {
    FWaterSurface3D water;
    FWaterSurface3DParams params{};
    params.ripple_lifetime = 1.0f;
    params.ripple_damping = 0.0f;
    water.SetParams(params);

    // This strength used to trip the absolute 0.0015 cutoff on the first
    // update and disappear even though essentially its whole lifetime remained.
    EXPECT_TRUE(water.AddDisturbance(
        FVec3{0.0f, 0.0f, 0.0f}, 0.15f, 0.001f));
    water.Update(0.01f);
    EXPECT_EQ(water.ActiveRippleCount(), 1u);
    water.Update(0.89f);
    EXPECT_EQ(water.ActiveRippleCount(), 1u);
    water.Update(0.099f);
    EXPECT_EQ(water.ActiveRippleCount(), 1u);
    // Cross the endpoint by a tiny epsilon; accumulated f32 frame deltas are
    // not required to sum to an exactly representable 1.0.
    water.Update(0.002f);
    EXPECT_EQ(water.ActiveRippleCount(), 0u);
}

ACS_TEST(Water3DRippleLifetime, DampingAndLifetimeEnvelopeRemainFinite) {
    const f32 scale = FWaterSurface3D::EvaluateRippleAmplitudeScale(
        1.0f, 4.0f, 0.78f);
    EXPECT_NEAR(scale, std::exp(-0.78f), 1e-6f);
    EXPECT_NEAR(FWaterSurface3D::EvaluateRippleAmplitudeScale(
                    1.0f, 0.0f, 0.78f),
                0.0f, 1e-7f);
    EXPECT_NEAR(FWaterSurface3D::EvaluateRippleAmplitudeScale(
                    std::numeric_limits<f32>::max(), 3600.0f, 64.0f),
                0.0f, 1e-7f);
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();
    EXPECT_NEAR(FWaterSurface3D::EvaluateRippleAmplitudeScale(
                    nan, 4.0f, 0.78f),
                0.0f, 1e-7f);
    EXPECT_NEAR(FWaterSurface3D::EvaluateRippleAmplitudeScale(
                    1.0f, infinity, 0.78f),
                0.0f, 1e-7f);
    EXPECT_NEAR(FWaterSurface3D::EvaluateRippleAmplitudeScale(
                    1.0f, 4.0f, nan),
                0.0f, 1e-7f);
    EXPECT_NEAR(FWaterSurface3D::EvaluateRippleAmplitudeScale(
                    -1.0f, 4.0f, -1.0f),
                1.0f, 1e-7f);
}

ACS_TEST(Water3DRippleLifetime, ZeroContributionReleasesSlotWithoutCutoffPop) {
    FWaterSurface3D water;
    FWaterSurface3DParams params{};
    params.ripple_lifetime = 3600.0f;
    params.ripple_damping = 64.0f;
    water.SetParams(params);

    EXPECT_TRUE(water.AddDisturbance(
        FVec3{0.0f, 0.0f, 0.0f}, 0.15f, 1.0f));
    water.Update(2.0f);
    // exp(-128) is below float range, so the exact CB amplitude is zero and
    // retaining the slot for another hour would only starve the event pool.
    EXPECT_EQ(water.ActiveRippleCount(), 0u);
}

ACS_TEST(Water3DRippleLifetime, NegativeAmplitudeUsesTheSameLifetimeTail) {
    FWaterSurface3D water;
    FWaterSurface3DParams params{};
    params.ripple_lifetime = 1.0f;
    params.ripple_damping = 0.0f;
    water.SetParams(params);

    EXPECT_TRUE(water.AddDisturbance(
        FVec3{0.0f, 0.0f, 0.0f}, 0.15f, -0.001f));
    water.Update(0.99f);
    EXPECT_EQ(water.ActiveRippleCount(), 1u);
    water.Update(0.02f);
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

ACS_TEST(Water3DRippleLifetime, WakeAcceptsFullThreeDimensionalVelocity) {
    FWaterSurface3D water;
    EXPECT_TRUE(water.AddWake(
        FVec3{2.0f, 3.0f, 4.0f},
        FVec3{0.0f, 6.0f, 0.0f}, 0.2f, 0.3f));
    EXPECT_EQ(water.ActiveRippleCount(), 1u);
}

ACS_TEST(Water3DRippleLifetime, SixtyFourSurfacesOwnIndependentEventPools) {
    FWaterSurface3D water;
    for (u64 surface = 1u;
         surface <= FWaterSurface3D::kMaxTrackedSurfaces;
         ++surface) {
        for (u32 impact = 0u;
             impact < FWaterSurface3D::kImpactRippleSlots;
             ++impact) {
            EXPECT_TRUE(water.AddDisturbanceForSurface(
                surface,
                FVec3{static_cast<f32>(surface), 0.0f,
                      static_cast<f32>(impact)},
                0.2f, 0.3f));
        }
        for (u32 wake = 0u;
             wake < FWaterSurface3D::kWakeRippleSlots;
             ++wake) {
            EXPECT_TRUE(water.AddWakeForSurface(
                surface,
                FVec3{static_cast<f32>(surface), 0.0f,
                      static_cast<f32>(wake)},
                FVec3{1.0f, 0.0f, 0.25f},
                0.2f, 0.2f));
        }
        EXPECT_EQ(
            water.ActiveRippleCountForSurface(surface),
            FWaterSurface3D::kMaxRipples);
    }
    EXPECT_EQ(
        water.ActiveRippleCount(),
        FWaterSurface3D::kMaxStoredRipples);
    EXPECT_FALSE(water.AddDisturbanceForSurface(
        65u, FVec3{65.0f, 0.0f, 0.0f}, 0.2f, 0.3f));

    water.ClearDisturbancesForSurface(17u);
    EXPECT_EQ(water.ActiveRippleCountForSurface(17u), 0u);
    EXPECT_EQ(
        water.ActiveRippleCount(),
        FWaterSurface3D::kMaxStoredRipples -
            FWaterSurface3D::kMaxRipples);
    EXPECT_TRUE(water.AddDisturbanceForSurface(
        65u, FVec3{65.0f, 0.0f, 0.0f}, 0.2f, 0.3f));
    EXPECT_EQ(water.ActiveRippleCountForSurface(65u), 1u);
    EXPECT_EQ(
        water.ActiveRippleCountForSurface(1u),
        FWaterSurface3D::kMaxRipples);
}

ACS_TEST(Water3DMeshContract, AcceptsFiniteLocalXzSurface) {
    FMeshAsset mesh;
    AddWaterMeshVertex(mesh, FVec3{-1.0f, 2.0f, -1.0f});
    AddWaterMeshVertex(mesh, FVec3{ 1.0f, 2.0f, -1.0f});
    AddWaterMeshVertex(mesh, FVec3{ 1.0f, 2.0f,  1.0f});
    AddWaterMeshVertex(mesh, FVec3{-1.0f, 2.0f,  1.0f});
    AddWaterMeshQuadIndices(mesh);

    EXPECT_TRUE(FWaterSurface3D::IsLocalXzSurfaceMesh(mesh));
}

ACS_TEST(Water3DMeshContract, RejectsWarpedOrVerticalCustomSurface) {
    FMeshAsset warped;
    AddWaterMeshVertex(warped, FVec3{-1.0f, 0.0f, -1.0f});
    AddWaterMeshVertex(warped, FVec3{ 1.0f, 0.0f, -1.0f});
    AddWaterMeshVertex(warped, FVec3{ 1.0f, 0.4f,  1.0f});
    AddWaterMeshVertex(warped, FVec3{-1.0f, 0.0f,  1.0f});
    AddWaterMeshQuadIndices(warped);
    EXPECT_FALSE(FWaterSurface3D::IsLocalXzSurfaceMesh(warped));

    FMeshAsset vertical;
    AddWaterMeshVertex(
        vertical, FVec3{-1.0f, -1.0f, 0.0f},
        FVec3{0.0f, 0.0f, 1.0f});
    AddWaterMeshVertex(
        vertical, FVec3{ 1.0f, -1.0f, 0.0f},
        FVec3{0.0f, 0.0f, 1.0f});
    AddWaterMeshVertex(
        vertical, FVec3{ 1.0f,  1.0f, 0.0f},
        FVec3{0.0f, 0.0f, 1.0f});
    vertical.Indices().PushBack(0u);
    vertical.Indices().PushBack(1u);
    vertical.Indices().PushBack(2u);
    EXPECT_FALSE(FWaterSurface3D::IsLocalXzSurfaceMesh(vertical));
}

ACS_TEST(Water3DMeshContract, RejectsMalformedCustomSurfaceIndices) {
    FMeshAsset mesh;
    AddWaterMeshVertex(mesh, FVec3{-1.0f, 0.0f, -1.0f});
    AddWaterMeshVertex(mesh, FVec3{ 1.0f, 0.0f, -1.0f});
    AddWaterMeshVertex(mesh, FVec3{ 0.0f, 0.0f,  1.0f});
    mesh.Indices().PushBack(0u);
    mesh.Indices().PushBack(1u);
    mesh.Indices().PushBack(9u);

    EXPECT_FALSE(FWaterSurface3D::IsLocalXzSurfaceMesh(mesh));
}
