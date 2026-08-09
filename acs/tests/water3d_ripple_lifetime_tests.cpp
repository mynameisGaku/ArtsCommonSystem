// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "asset/MeshAsset.h"
#include "render/WaterSurface3D.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using namespace acs;

namespace {

void AddWaterMeshVertex(
    AMeshAsset& mesh, FVec3 position,
    FVec3 normal = FVec3{0.0f, 1.0f, 0.0f}) {
    mesh.Vertices().Add(
        FMeshVertex{position, normal, 0.0f, 0.0f});
}

void AddWaterMeshQuadIndices(AMeshAsset& mesh) {
    mesh.Indices().Add(0u);
    mesh.Indices().Add(1u);
    mesh.Indices().Add(2u);
    mesh.Indices().Add(0u);
    mesh.Indices().Add(2u);
    mesh.Indices().Add(3u);
}

std::string ReadWaterRepositorySource(const char* relative_path) {
    std::string path = __FILE__;
    const std::size_t separator = path.find_last_of("\\/");
    if (separator == std::string::npos) return {};
    path.resize(separator);
    path += "/../";
    path += relative_path;
    std::FILE* file = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&file, path.c_str(), "rb") != 0) return {};
#else
    file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return {};
#endif
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return {};
    }
    const long size = std::ftell(file);
    if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return {};
    }
    std::string source(static_cast<std::size_t>(size), '\0');
    const std::size_t read = source.empty()
        ? 0u : std::fread(source.data(), 1u, source.size(), file);
    std::fclose(file);
    if (read != source.size()) return {};
    return source;
}

} // namespace

ACS_TEST(Water3DRippleLifetime, ReservedPoolsNeverOverwriteEachOther) {
    CWaterSurface3D water;
    FWaterSurface3DParams params{};
    params.ripple_lifetime = 1.0f;
    params.ripple_damping = 0.0f;
    water.SetParams(params);

    for (u32 i = 0; i < CWaterSurface3D::kImpactRippleSlots; ++i) {
        EXPECT_TRUE(water.AddDisturbance(
            FVec3{static_cast<f32>(i), 0.0f, 0.0f}, 0.15f, 0.20f));
    }
    EXPECT_FALSE(water.AddDisturbance(
        FVec3{100.0f, 0.0f, 0.0f}, 0.15f, 0.20f));
    EXPECT_EQ(
        water.ActiveRippleCount(), CWaterSurface3D::kImpactRippleSlots);

    for (u32 i = 0; i < CWaterSurface3D::kWakeRippleSlots; ++i) {
        EXPECT_TRUE(water.AddWake(
            FVec3{static_cast<f32>(i), 0.0f, 1.0f},
            FVec3{4.0f, 0.0f, 1.0f}, 0.20f, 0.18f));
    }
    EXPECT_FALSE(water.AddWake(
        FVec3{200.0f, 0.0f, 0.0f},
        FVec3{4.0f, 0.0f, 1.0f}, 0.20f, 0.18f));
    EXPECT_EQ(water.ActiveRippleCount(), CWaterSurface3D::kMaxRipples);
}

ACS_TEST(Water3DRippleLifetime,
         ConservativeDisplacementBoundTracksTheSubmittedSurface) {
    CWaterSurface3D water;
    FWaterSurface3DParams params{};
    params.wave_amplitude = 2.0f;
    params.ripple_lifetime = 2.0f;
    params.ripple_damping = 0.0f;
    water.SetParams(params);

    EXPECT_TRUE(water.AddDisturbanceForSurface(
        42u, FVec3{0.0f, 0.0f, 0.0f}, 0.2f, 0.30f));
    EXPECT_TRUE(water.AddDisturbanceForSurface(
        42u, FVec3{1.0f, 0.0f, 0.0f}, 0.2f, -0.20f));
    EXPECT_TRUE(water.AddDisturbanceForSurface(
        7u, FVec3{2.0f, 0.0f, 0.0f}, 0.2f, 8.0f));

    // The VS ambient weights sum to 1.02. Only the two ripples submitted for
    // surface 42 may inflate its bound; another surface cannot disable useful
    // culling here.
    EXPECT_NEAR(
        water.ConservativeDisplacementBoundForSurface(
            42u, params),
        2.0f * 1.02f + 0.30f + 0.20f, 1e-5f);
    EXPECT_NEAR(
        water.ConservativeDisplacementBoundForSurface(
            99u, params),
        2.0f * 1.02f, 1e-5f);

    water.Update(1.5f);
    const f32 amplitude_scale =
        CWaterSurface3D::EvaluateRippleAmplitudeScale(
            1.5f, 2.0f, 0.0f);
    EXPECT_NEAR(
        water.ConservativeDisplacementBoundForSurface(
            42u, params),
        2.0f * 1.02f +
            (0.30f + 0.20f) * amplitude_scale,
        1e-5f);
}

ACS_TEST(Water3DRippleLifetime,
         LowFrequencyMotionResamplesAContinuousThreeDimensionalWake) {
    CWaterSurface3D water;
    FWaterSurface3DParams params{};
    params.ripple_lifetime = 2.0f;
    params.ripple_damping = 0.0f;
    water.SetParams(params);

    const u32 accepted = water.AddWakeSegmentForSurface(
        42u,
        FVec3{-2.0f, 1.0f, -3.0f},
        FVec3{ 2.0f, 3.0f,  3.0f},
        0.20f, 1.0f, 0.18f, 0.16f);

    // sqrt(4^2 + 2^2 + 6^2) = sqrt(56): ceil at unit spacing is 8.
    EXPECT_EQ(accepted, 8u);
    EXPECT_EQ(water.ActiveRippleCountForSurface(42u), 8u);
    EXPECT_EQ(water.ActiveRippleCount(), 8u);
}

ACS_TEST(Water3DRippleLifetime,
         SegmentResamplingUsesReservedCapacityWithoutReplacingImpacts) {
    CWaterSurface3D water;
    FWaterSurface3DParams params{};
    params.ripple_lifetime = 4.0f;
    params.ripple_damping = 0.0f;
    water.SetParams(params);

    for (u32 impact = 0u;
         impact < CWaterSurface3D::kImpactRippleSlots; ++impact) {
        EXPECT_TRUE(water.AddDisturbanceForSurface(
            7u, FVec3{static_cast<f32>(impact), 0.0f, 0.0f},
            0.15f, 0.20f));
    }
    const u32 wakes = water.AddWakeSegmentForSurface(
        7u, FVec3{0.0f, 0.0f, 0.0f},
        FVec3{100.0f, 0.0f, 0.0f},
        0.25f, 0.10f, 0.18f, 0.16f);

    EXPECT_EQ(wakes, CWaterSurface3D::kWakeRippleSlots);
    EXPECT_EQ(
        water.ActiveRippleCountForSurface(7u),
        CWaterSurface3D::kMaxRipples);
    EXPECT_EQ(water.AddWakeSegmentForSurface(
                  7u, FVec3{100.0f, 0.0f, 0.0f},
                  FVec3{101.0f, 0.0f, 0.0f},
                  0.1f, 0.1f, 0.18f, 0.16f),
              0u);
    EXPECT_EQ(
        water.ActiveRippleCountForSurface(7u),
        CWaterSurface3D::kMaxRipples);
}

ACS_TEST(Water3DRippleLifetime,
         SeparatelyResampledSegmentsKeepIndependentAges) {
    CWaterSurface3D water;
    FWaterSurface3DParams params{};
    params.ripple_lifetime = 1.0f;
    params.ripple_damping = 0.0f;
    water.SetParams(params);

    EXPECT_EQ(water.AddWakeSegment(
                  FVec3{0.0f, 0.0f, 0.0f},
                  FVec3{4.0f, 0.0f, 0.0f},
                  0.1f, 0.5f, 0.18f, 0.16f),
              8u);
    water.Update(0.40f);
    EXPECT_EQ(water.AddWakeSegment(
                  FVec3{4.0f, 0.0f, 0.0f},
                  FVec3{8.0f, 0.0f, 0.0f},
                  0.1f, 0.5f, 0.18f, 0.16f),
              8u);
    EXPECT_EQ(water.ActiveRippleCount(), 16u);

    water.Update(0.61f);
    // The first segment reaches its own endpoint. A refresh/overwrite scheme
    // would either keep all 16 alive or erase old samples at insertion time.
    EXPECT_EQ(water.ActiveRippleCount(), 8u);
    water.Update(0.40f);
    EXPECT_EQ(water.ActiveRippleCount(), 0u);
}

ACS_TEST(Water3DRippleLifetime,
         SamplesWithinOneSegmentRetainTheirHistoricalAges) {
    CWaterSurface3D water;
    FWaterSurface3DParams params{};
    params.ripple_lifetime = 1.0f;
    params.ripple_damping = 0.0f;
    water.SetParams(params);

    EXPECT_EQ(water.AddWakeSegment(
                  FVec3{0.0f, 0.0f, 0.0f},
                  FVec3{4.0f, 0.0f, 0.0f},
                  0.8f, 0.5f, 0.18f, 0.16f),
              8u);
    // Sample ages are 0.7, 0.6, ... 0.0 seconds. Advancing 0.31 seconds
    // retires only the oldest sample instead of refreshing the whole trail.
    water.Update(0.31f);
    EXPECT_EQ(water.ActiveRippleCount(), 7u);
    water.Update(0.70f);
    EXPECT_EQ(water.ActiveRippleCount(), 0u);
}

ACS_TEST(Water3DRippleLifetime,
         SegmentOlderThanLifetimeKeepsOnlyItsVisibleTail) {
    CWaterSurface3D water;
    FWaterSurface3DParams params{};
    params.ripple_lifetime = 1.0f;
    params.ripple_damping = 0.0f;
    water.SetParams(params);

    // Only the final one world unit occurred within the one-second lifetime.
    EXPECT_EQ(water.AddWakeSegment(
                  FVec3{0.0f, 0.0f, 0.0f},
                  FVec3{4.0f, 0.0f, 0.0f},
                  4.0f, 0.5f, 0.18f, 0.16f),
              2u);
    EXPECT_EQ(water.ActiveRippleCount(), 2u);
}

ACS_TEST(Water3DRippleLifetime,
         MalformedWakeSegmentsNeverConsumePersistentSlots) {
    CWaterSurface3D water;
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();

    EXPECT_EQ(water.AddWakeSegment(
                  FVec3{0.0f, 0.0f, 0.0f},
                  FVec3{1.0f, 0.0f, 0.0f},
                  0.0f, 0.1f, 0.18f, 0.16f),
              0u);
    EXPECT_EQ(water.AddWakeSegment(
                  FVec3{0.0f, 0.0f, 0.0f},
                  FVec3{1.0f, 0.0f, 0.0f},
                  0.1f, nan, 0.18f, 0.16f),
              0u);
    EXPECT_EQ(water.AddWakeSegment(
                  FVec3{0.0f, 0.0f, 0.0f},
                  FVec3{nan, 0.0f, 0.0f},
                  0.1f, 0.1f, 0.18f, 0.16f),
              0u);
    EXPECT_EQ(water.ActiveRippleCount(), 0u);
}

ACS_TEST(Water3DRippleLifetime, FullImpactPoolDropsNewEventWithoutRefreshingOldOne) {
    CWaterSurface3D water;
    FWaterSurface3DParams params{};
    params.ripple_lifetime = 1.0f;
    params.ripple_damping = 0.0f;
    water.SetParams(params);

    for (u32 i = 0; i < CWaterSurface3D::kImpactRippleSlots; ++i) {
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
    CWaterSurface3D water;
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
        const f32 scale = CWaterSurface3D::EvaluateRippleAmplitudeScale(
            age, lifetime, 0.0f);
        EXPECT_TRUE(std::isfinite(scale));
        EXPECT_TRUE(scale >= 0.0f);
        EXPECT_TRUE(scale <= previous + 1e-6f);
        previous = scale;
    }

    // The physical response is unchanged until the final 35% of the lifetime.
    EXPECT_NEAR(CWaterSurface3D::EvaluateRippleAmplitudeScale(
                    lifetime * 0.65f, lifetime, 0.0f),
                1.0f, 1e-6f);
    EXPECT_TRUE(CWaterSurface3D::EvaluateRippleAmplitudeScale(
                    lifetime * 0.90f, lifetime, 0.0f) > 0.0f);
    EXPECT_NEAR(CWaterSurface3D::EvaluateRippleAmplitudeScale(
                    lifetime, lifetime, 0.0f),
                0.0f, 1e-7f);

    // Smootherstep has zero slope at release. A finite-difference sample near
    // the endpoint must therefore already be visually negligible.
    EXPECT_TRUE(CWaterSurface3D::EvaluateRippleAmplitudeScale(
                    lifetime * 0.999f, lifetime, 0.0f) < 1e-6f);

    const f32 h = lifetime * 0.001f;
    const f32 at_end = CWaterSurface3D::EvaluateRippleAmplitudeScale(
        lifetime, lifetime, 0.0f);
    const f32 before_end = CWaterSurface3D::EvaluateRippleAmplitudeScale(
        lifetime - h, lifetime, 0.0f);
    const f32 twice_before_end =
        CWaterSurface3D::EvaluateRippleAmplitudeScale(
            lifetime - 2.0f * h, lifetime, 0.0f);
    const f32 endpoint_slope = (at_end - before_end) / h;
    const f32 endpoint_curvature =
        (at_end - 2.0f * before_end + twice_before_end) / (h * h);
    EXPECT_TRUE(std::abs(endpoint_slope) < 1e-4f);
    EXPECT_TRUE(std::abs(endpoint_curvature) < 0.12f);
}

ACS_TEST(Water3DRippleLifetime, SmallRippleFadesBeforeSlotIsReleased) {
    CWaterSurface3D water;
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
    const f32 scale = CWaterSurface3D::EvaluateRippleAmplitudeScale(
        1.0f, 4.0f, 0.78f);
    EXPECT_NEAR(scale, std::exp(-0.78f), 1e-6f);
    EXPECT_NEAR(CWaterSurface3D::EvaluateRippleAmplitudeScale(
                    1.0f, 0.0f, 0.78f),
                0.0f, 1e-7f);
    EXPECT_NEAR(CWaterSurface3D::EvaluateRippleAmplitudeScale(
                    std::numeric_limits<f32>::max(), 3600.0f, 64.0f),
                0.0f, 1e-7f);
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();
    EXPECT_NEAR(CWaterSurface3D::EvaluateRippleAmplitudeScale(
                    nan, 4.0f, 0.78f),
                0.0f, 1e-7f);
    EXPECT_NEAR(CWaterSurface3D::EvaluateRippleAmplitudeScale(
                    1.0f, infinity, 0.78f),
                0.0f, 1e-7f);
    EXPECT_NEAR(CWaterSurface3D::EvaluateRippleAmplitudeScale(
                    1.0f, 4.0f, nan),
                0.0f, 1e-7f);
    EXPECT_NEAR(CWaterSurface3D::EvaluateRippleAmplitudeScale(
                    -1.0f, 4.0f, -1.0f),
                1.0f, 1e-7f);
}

ACS_TEST(Water3DRippleLifetime, ZeroContributionReleasesSlotWithoutCutoffPop) {
    CWaterSurface3D water;
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
    CWaterSurface3D water;
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
    CWaterSurface3D water;
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
    CWaterSurface3D water;
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
    CWaterSurface3D water;
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
    CWaterSurface3D water;
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
    CWaterSurface3D water;
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
    CWaterSurface3D water;
    EXPECT_TRUE(water.AddWake(
        FVec3{2.0f, 3.0f, 4.0f},
        FVec3{0.0f, 6.0f, 0.0f}, 0.2f, 0.3f));
    EXPECT_EQ(water.ActiveRippleCount(), 1u);
}

ACS_TEST(Water3DRippleLifetime, SixtyFourSurfacesOwnIndependentEventPools) {
    CWaterSurface3D water;
    for (u64 surface = 1u;
         surface <= CWaterSurface3D::kMaxTrackedSurfaces;
         ++surface) {
        for (u32 impact = 0u;
             impact < CWaterSurface3D::kImpactRippleSlots;
             ++impact) {
            EXPECT_TRUE(water.AddDisturbanceForSurface(
                surface,
                FVec3{static_cast<f32>(surface), 0.0f,
                      static_cast<f32>(impact)},
                0.2f, 0.3f));
        }
        for (u32 wake = 0u;
             wake < CWaterSurface3D::kWakeRippleSlots;
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
            CWaterSurface3D::kMaxRipples);
    }
    EXPECT_EQ(
        water.ActiveRippleCount(),
        CWaterSurface3D::kMaxStoredRipples);
    EXPECT_FALSE(water.AddDisturbanceForSurface(
        65u, FVec3{65.0f, 0.0f, 0.0f}, 0.2f, 0.3f));

    water.ClearDisturbancesForSurface(17u);
    EXPECT_EQ(water.ActiveRippleCountForSurface(17u), 0u);
    EXPECT_EQ(
        water.ActiveRippleCount(),
        CWaterSurface3D::kMaxStoredRipples -
            CWaterSurface3D::kMaxRipples);
    EXPECT_TRUE(water.AddDisturbanceForSurface(
        65u, FVec3{65.0f, 0.0f, 0.0f}, 0.2f, 0.3f));
    EXPECT_EQ(water.ActiveRippleCountForSurface(65u), 1u);
    EXPECT_EQ(
        water.ActiveRippleCountForSurface(1u),
        CWaterSurface3D::kMaxRipples);
}

ACS_TEST(Water3DRippleLifetime,
         DenseActiveIterationDoesNotSkipSwapCompactedEvents) {
    CWaterSurface3D water;
    FWaterSurface3DParams short_params{};
    short_params.ripple_lifetime = 0.5f;
    short_params.ripple_damping = 0.0f;
    water.SetParams(short_params);
    EXPECT_TRUE(water.AddDisturbanceForSurface(
        1u, FVec3{0.0f, 0.0f, 0.0f}, 0.2f, 0.3f));

    FWaterSurface3DParams long_params = short_params;
    long_params.ripple_lifetime = 2.0f;
    water.SetParams(long_params);
    EXPECT_TRUE(water.AddDisturbanceForSurface(
        2u, FVec3{1.0f, 0.0f, 0.0f}, 0.2f, 0.3f));
    EXPECT_TRUE(water.AddWakeForSurface(
        2u, FVec3{2.0f, 1.0f, 3.0f},
        FVec3{1.0f, 2.0f, 3.0f}, 0.2f, 0.2f));

    // Retiring position zero swap-compacts one of surface 2's events into the
    // current iteration position. Both surviving events must still be visited
    // and retain their independent two-second lifetime.
    water.Update(0.6f);
    EXPECT_EQ(water.ActiveRippleCount(), 2u);
    EXPECT_EQ(water.ActiveRippleCountForSurface(1u), 0u);
    EXPECT_EQ(water.ActiveRippleCountForSurface(2u), 2u);
    water.ClearDisturbancesForSurface(2u);
    EXPECT_EQ(water.ActiveRippleCount(), 0u);

    // The optimized idle path still advances analytic animation exactly.
    const f32 before = water.Time();
    water.Update(0.25f);
    EXPECT_NEAR(water.Time(), before + 0.25f, 1e-6f);
}

ACS_TEST(Water3DShaderContract,
         AnalyticGeneratedAndAuthoredNormalsUseOnePhysicalSlope) {
    const std::string source =
        ReadWaterRepositorySource("src/render/WaterSurface3D.cpp");
    EXPECT_TRUE(!source.empty());
    if (source.empty()) return;

    EXPECT_TRUE(source.find(
        "Texture2D authored_normal : register(t1);") !=
        std::string::npos);
    EXPECT_TRUE(source.find(
        "float3 EvaluateAuthoredNormal(float2 mesh_uv)") !=
        std::string::npos);
    EXPECT_TRUE(source.find(
        "tangent * (micro_slope.x * normal_strength)") !=
        std::string::npos);
    EXPECT_TRUE(source.find(
        "bitangent * (micro_slope.y * normal_strength)") !=
        std::string::npos);
    EXPECT_TRUE(source.find(
        "float3 PerturbWaterNormal(") !=
        std::string::npos);
    EXPECT_TRUE(source.find(
        "float2 duv_dx = ddx(mesh_uv);") !=
        std::string::npos);
    EXPECT_TRUE(source.find(
        "input.uv, authored_tangent_normal") !=
        std::string::npos);
    EXPECT_TRUE(source.find(
        "EvaluateAmbientWaves(surface_position") !=
        std::string::npos);
    EXPECT_TRUE(source.find(
        "EvaluateRipples(surface_position") !=
        std::string::npos);
    EXPECT_TRUE(source.find(
        "world.xyz += base_normal * (ambient_height + ripple_height);") !=
        std::string::npos);
}

ACS_TEST(Water3DShaderContract,
         RippleFoamSharesTheContinuousDisplacementEnvelope) {
    const std::string source =
        ReadWaterRepositorySource("src/render/WaterSurface3D.cpp");
    EXPECT_TRUE(!source.empty());
    if (source.empty()) return;

    EXPECT_TRUE(source.find(
        "1.0 - exp(-max(input.ripple_energy, 0.0) * 5.4)") !=
        std::string::npos);
    EXPECT_TRUE(source.find(
        "smoothstep(0.075, 0.28, input.ripple_energy)") ==
        std::string::npos);
    EXPECT_TRUE(source.find(
        "gradient += derivative * world_gradient;") !=
        std::string::npos);
    EXPECT_TRUE(source.find(
        "energy += max(wave, 0.0)") !=
        std::string::npos);
}

ACS_TEST(Water3DShaderContract,
         StrictBackendBindsEveryDeclaredWaterTexture) {
    const std::string source =
        ReadWaterRepositorySource("src/render/WaterSurface3D.cpp");
    EXPECT_TRUE(!source.empty());
    if (source.empty()) return;

    EXPECT_TRUE(source.find(
        "pipeline_description.texture_slots = 6;") !=
        std::string::npos);
    EXPECT_TRUE(source.find(
        "pipeline_description.static_sampler_count = 6;") !=
        std::string::npos);
    EXPECT_TRUE(source.find(
        "command_list.SetTexture(0, *m_NormalMap);") !=
        std::string::npos);
    for (u32 slot = 1u; slot < 6u; ++slot) {
        const std::string binding =
            "command_list.SetTexture(\n        " +
            std::to_string(slot) + ",";
        EXPECT_TRUE(source.find(binding) != std::string::npos);
    }
}

ACS_TEST(Water3DEditorContract,
         CulledOrAbsentWaterAvoidsAllFullscreenPassWork) {
    const std::string source =
        ReadWaterRepositorySource("src/editor_abi/EditorAbi.cpp");
    EXPECT_TRUE(!source.empty());
    if (source.empty()) return;

    const std::size_t function_begin =
        source.find("void DrawInteractiveWater3DPass(");
    const std::size_t function_end =
        source.find("int ParentId3D(", function_begin);
    EXPECT_TRUE(function_begin != std::string::npos);
    EXPECT_TRUE(function_end != std::string::npos);
    if (function_begin == std::string::npos ||
        function_end == std::string::npos) {
        return;
    }
    const std::string body = source.substr(
        function_begin, function_end - function_begin);
    const std::size_t eligibility =
        body.find("if (!submission_mask.ShouldSubmit(i)) continue;");
    const std::size_t zero_work_gate =
        body.find("if (!any_water || !host.refr_bg");
    const std::size_t first_fullscreen =
        body.find("command_list.BeginRenderToTexture(");
    EXPECT_TRUE(eligibility != std::string::npos);
    EXPECT_TRUE(zero_work_gate != std::string::npos);
    EXPECT_TRUE(first_fullscreen != std::string::npos);
    EXPECT_TRUE(eligibility < zero_work_gate);
    EXPECT_TRUE(zero_work_gate < first_fullscreen);
    EXPECT_TRUE(body.find(
        "record->material_normal_tex.Get()") !=
        std::string::npos);
    EXPECT_TRUE(body.find(
        "true, authored_normal_map,\n"
        "            authored_normal_strength);") !=
        std::string::npos);
}

ACS_TEST(Water3DMeshContract, AcceptsFiniteLocalXzSurface) {
    AMeshAsset mesh;
    AddWaterMeshVertex(mesh, FVec3{-1.0f, 2.0f, -1.0f});
    AddWaterMeshVertex(mesh, FVec3{ 1.0f, 2.0f, -1.0f});
    AddWaterMeshVertex(mesh, FVec3{ 1.0f, 2.0f,  1.0f});
    AddWaterMeshVertex(mesh, FVec3{-1.0f, 2.0f,  1.0f});
    AddWaterMeshQuadIndices(mesh);

    EXPECT_TRUE(CWaterSurface3D::IsLocalXzSurfaceMesh(mesh));
}

ACS_TEST(Water3DMeshContract, RejectsWarpedOrVerticalCustomSurface) {
    AMeshAsset warped;
    AddWaterMeshVertex(warped, FVec3{-1.0f, 0.0f, -1.0f});
    AddWaterMeshVertex(warped, FVec3{ 1.0f, 0.0f, -1.0f});
    AddWaterMeshVertex(warped, FVec3{ 1.0f, 0.4f,  1.0f});
    AddWaterMeshVertex(warped, FVec3{-1.0f, 0.0f,  1.0f});
    AddWaterMeshQuadIndices(warped);
    EXPECT_FALSE(CWaterSurface3D::IsLocalXzSurfaceMesh(warped));

    AMeshAsset vertical;
    AddWaterMeshVertex(
        vertical, FVec3{-1.0f, -1.0f, 0.0f},
        FVec3{0.0f, 0.0f, 1.0f});
    AddWaterMeshVertex(
        vertical, FVec3{ 1.0f, -1.0f, 0.0f},
        FVec3{0.0f, 0.0f, 1.0f});
    AddWaterMeshVertex(
        vertical, FVec3{ 1.0f,  1.0f, 0.0f},
        FVec3{0.0f, 0.0f, 1.0f});
    vertical.Indices().Add(0u);
    vertical.Indices().Add(1u);
    vertical.Indices().Add(2u);
    EXPECT_FALSE(CWaterSurface3D::IsLocalXzSurfaceMesh(vertical));
}

ACS_TEST(Water3DMeshContract, RejectsMalformedCustomSurfaceIndices) {
    AMeshAsset mesh;
    AddWaterMeshVertex(mesh, FVec3{-1.0f, 0.0f, -1.0f});
    AddWaterMeshVertex(mesh, FVec3{ 1.0f, 0.0f, -1.0f});
    AddWaterMeshVertex(mesh, FVec3{ 0.0f, 0.0f,  1.0f});
    mesh.Indices().Add(0u);
    mesh.Indices().Add(1u);
    mesh.Indices().Add(9u);

    EXPECT_FALSE(CWaterSurface3D::IsLocalXzSurfaceMesh(mesh));
}
