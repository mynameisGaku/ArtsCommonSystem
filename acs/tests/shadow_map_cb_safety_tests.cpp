// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"

#include "math/Math.h"
#include "render/IRhiDevice.h"
#include "render/ShadowMap.h"

#include <cmath>
#include <limits>

using namespace acs;

namespace {

bool MatrixIsFinite(const FMat4& matrix) noexcept
{
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            if (!std::isfinite(matrix.m[row][column])) return false;
        }
    }
    return true;
}

} // namespace

ACS_TEST(ShadowMap, InvalidAuthoringInputsKeepFiniteProjection)
{
    CShadowMap shadow;
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();
    shadow.SetDirectionalLight(
        FVec3{nan, infinity, 0.0f}, FVec3{nan, 0.0f, infinity}, nan);
    EXPECT_TRUE(MatrixIsFinite(shadow.LightViewProjection()));

    FMat4 invalid_view = FMat4::Identity();
    invalid_view.m[2][1] = nan;
    shadow.SetDirectionalLightCascades(
        FVec3{nan, 0.0f, 0.0f}, invalid_view, FMat4::Identity(),
        nan, infinity, nan);
    EXPECT_TRUE(MatrixIsFinite(shadow.LightViewProjection()));

    // This finite, invertible projective permutation maps the NDC near plane
    // to homogeneous w=0. Element/inverse finiteness alone is insufficient.
    const FMat4 zero_near_w{
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 0, 1,
        0, 0, 1, 0};
    shadow.SetDirectionalLightCascades(
        FVec3{0.3f, 0.8f, -0.4f}, FMat4::Identity(), zero_near_w,
        0.1f, 100.0f, 0.5f);
    EXPECT_EQ(shadow.CascadeCount(), 1u);
    EXPECT_TRUE(MatrixIsFinite(shadow.LightViewProjection()));

    // Finite directions near FLT_MAX must normalize by scale instead of
    // overflowing a float length-squared and silently becoming vertical.
    const f32 maximum = std::numeric_limits<f32>::max();
    shadow.SetDirectionalLight(
        FVec3{maximum, maximum * 0.5f, -maximum * 0.25f},
        FVec3{0, 0, 0}, 20.0f);
    const FMat4 huge_direction = shadow.LightViewProjection();
    shadow.SetDirectionalLight(
        FVec3{1.0f, 0.5f, -0.25f}, FVec3{0, 0, 0}, 20.0f);
    const FMat4 reference_direction = shadow.LightViewProjection();
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            EXPECT_NEAR(huge_direction.m[row][column],
                        reference_direction.m[row][column], 1e-5f);
        }
    }

    shadow.SetDirectionalLight(
        FVec3{0.3f, 0.8f, -0.4f},
        FVec3{maximum, maximum, maximum}, maximum);
    EXPECT_TRUE(MatrixIsFinite(shadow.LightViewProjection()));
}

ACS_TEST(ShadowMap, CascadeAndCasterBuffersRemainDrawImmutable)
{
    FDeviceConfig config{};
    auto device_result = CreateRhiDevice(config);
    if (device_result.IsErr()) return; // Headless CI may not expose a GPU.

    CShadowMap shadow;
    EXPECT_TRUE(shadow.Init(*device_result.Value(), 64, 3).IsOk());
    EXPECT_TRUE(shadow.Init(*device_result.Value(), 64, 3).IsOk());
    if (!shadow.DepthTexture()) return;

    shadow.SetDirectionalLightCascades(
        FVec3{0.3f, 0.8f, -0.4f},
        FMat4::LookAtLH(FVec3{0.0f, 2.0f, -8.0f},
                        FVec3{0.0f, 1.0f, 0.0f},
                        FVec3{0.0f, 1.0f, 0.0f}),
        FMat4::PerspectiveFovLH(kPi / 3.0f, 1.0f, 0.1f, 50.0f),
        0.1f, 40.0f);

    shadow.SetCurrentCascade(0);
    IRhiBuffer* cascade0 = shadow.LightCB();
    shadow.SetCurrentCascade(1);
    IRhiBuffer* cascade1 = shadow.LightCB();
    shadow.SetCurrentCascade(2);
    IRhiBuffer* cascade2 = shadow.LightCB();
    EXPECT_TRUE(cascade0 != nullptr);
    EXPECT_TRUE(cascade0 != cascade1);
    EXPECT_TRUE(cascade1 != cascade2);
    shadow.SetCurrentCascade(99);
    EXPECT_TRUE(shadow.LightCB() == cascade0);

    EXPECT_TRUE(shadow.BeginFrame(2u));
    FMat4 invalid_model = FMat4::Identity();
    invalid_model.m[1][2] = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_FALSE(shadow.TrySetCaster(invalid_model));
    EXPECT_EQ(shadow.CasterDrawCount(), 0u);
    EXPECT_TRUE(shadow.TrySetCaster(FMat4::Translation(FVec3{1, 0, 0})));
    IRhiBuffer* caster0 = shadow.CasterObjectCB();
    EXPECT_TRUE(shadow.TrySetCaster(FMat4::Translation(FVec3{2, 0, 0})));
    IRhiBuffer* caster1 = shadow.CasterObjectCB();
    EXPECT_TRUE(caster0 != nullptr);
    EXPECT_TRUE(caster0 != caster1);
    EXPECT_EQ(shadow.CasterDrawCount(), 2u);

    // A new frame reuses slot 0 only after the previous frame's RHI slot advances.
    EXPECT_TRUE(shadow.BeginFrame(1u));
    EXPECT_TRUE(shadow.TrySetCaster(FMat4::Translation(FVec3{3, 0, 0})));
    EXPECT_TRUE(shadow.CasterObjectCB() == caster0);
    EXPECT_EQ(shadow.CasterDrawCount(), 1u);
    EXPECT_FALSE(shadow.CasterOverflowed());

    shadow.Shutdown();
}

ACS_TEST(ShadowMap, SingleProjectionFallbackMatchesActiveCascadeState)
{
    FDeviceConfig config{};
    auto device_result = CreateRhiDevice(config);
    if (device_result.IsErr()) return;

    CShadowMap shadow;
    EXPECT_TRUE(shadow.Init(*device_result.Value(), 64,
                            CShadowMap::kMaxCascades).IsOk());
    if (!shadow.DepthTexture()) return;

    shadow.SetCurrentCascade(0);
    IRhiBuffer* cascade0 = shadow.LightCB();
    shadow.SetCurrentCascade(3);
    IRhiBuffer* cascade3 = shadow.LightCB();
    EXPECT_TRUE(cascade0 != nullptr);
    EXPECT_TRUE(cascade3 != nullptr);
    EXPECT_TRUE(cascade0 != cascade3);

    shadow.SetDirectionalLight(
        FVec3{0.3f, 0.8f, -0.4f}, FVec3{0, 0, 0}, 20.0f);
    EXPECT_EQ(shadow.CascadeCount(), 1u);
    shadow.SetCurrentCascade(3);
    EXPECT_TRUE(shadow.LightCB() == cascade0);
    EXPECT_NEAR(shadow.CascadeViewport(3).x, 0.0f, 1e-6f);
    EXPECT_NEAR(
        shadow.CascadeViewport(3).width,
        64.0f * static_cast<f32>(CShadowMap::kMaxCascades),
        1e-6f);
    EXPECT_EQ(
        shadow.CascadeScissor(3).right,
        static_cast<i32>(64u * CShadowMap::kMaxCascades));

    // BeginFrame must reserve the complete Init-time cascade capacity even
    // while a single-volume fallback is active. A valid CSM update can restore
    // all cascades later in the same frame without allocating mid-pass.
    constexpr u32 kRestoredCsmCastersPerCascade = 96u;
    EXPECT_TRUE(shadow.BeginFrame(kRestoredCsmCastersPerCascade));
    for (u32 cascade = 0; cascade < CShadowMap::kMaxCascades; ++cascade) {
        EXPECT_TRUE(shadow.CasterBufferCapacity(cascade) >=
                    kRestoredCsmCastersPerCascade);
    }

    shadow.SetDirectionalLightCascades(
        FVec3{0.3f, 0.8f, -0.4f},
        FMat4::LookAtLH(FVec3{0.0f, 2.0f, -8.0f},
                        FVec3{0.0f, 1.0f, 0.0f},
                        FVec3{0.0f, 1.0f, 0.0f}),
        FMat4::PerspectiveFovLH(kPi / 3.0f, 1.0f, 0.1f, 50.0f),
        0.1f, 40.0f);
    EXPECT_EQ(shadow.CascadeCount(), CShadowMap::kMaxCascades);
    shadow.SetCurrentCascade(3);
    EXPECT_TRUE(shadow.LightCB() == cascade3);
    EXPECT_NEAR(shadow.CascadeViewport(3).x, 192.0f, 1e-6f);
    EXPECT_NEAR(shadow.CascadeViewport(3).width, 64.0f, 1e-6f);
    for (u32 cascade = 0; cascade < CShadowMap::kMaxCascades; ++cascade) {
        shadow.SetCurrentCascade(cascade);
        for (u32 draw = 0; draw < kRestoredCsmCastersPerCascade; ++draw) {
            EXPECT_TRUE(shadow.TrySetCaster(FMat4::Translation(FVec3{
                static_cast<f32>(draw), static_cast<f32>(cascade), 0.0f})));
        }
        EXPECT_EQ(shadow.CasterDrawCount(cascade),
                  kRestoredCsmCastersPerCascade);
        EXPECT_FALSE(shadow.CasterOverflowed(cascade));
    }
    EXPECT_EQ(shadow.CasterDrawCount(),
              kRestoredCsmCastersPerCascade * CShadowMap::kMaxCascades);
    EXPECT_FALSE(shadow.CasterOverflowed());

    // Invalid camera transforms use the same explicit single-volume state.
    FMat4 singular{
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0};
    shadow.SetDirectionalLightCascades(
        FVec3{0.3f, 0.8f, -0.4f}, singular, singular,
        0.1f, 40.0f);
    EXPECT_EQ(shadow.CascadeCount(), 1u);
    shadow.SetCurrentCascade(3);
    EXPECT_TRUE(shadow.LightCB() == cascade0);

    shadow.Shutdown();
}

ACS_TEST(ShadowMap, CasterPoolGrowsBeyondFormerFixedLimit)
{
    FDeviceConfig config{};
    auto device_result = CreateRhiDevice(config);
    if (device_result.IsErr()) return;

    CShadowMap shadow;
    EXPECT_FALSE(shadow.BeginFrame(0u));
    EXPECT_TRUE(shadow.Init(*device_result.Value(), 64, 1).IsOk());
    if (!shadow.DepthTexture()) return;

    EXPECT_FALSE(shadow.BeginFrame(std::numeric_limits<u32>::max()));
    EXPECT_FALSE(shadow.TrySetCaster(FMat4::Identity()));
    EXPECT_EQ(shadow.CasterDrawCount(), 0u);
    EXPECT_TRUE(shadow.CasterObjectCB() == nullptr);

    constexpr u32 kLargeCasterCount = 512u;
    EXPECT_TRUE(shadow.BeginFrame(kLargeCasterCount));
    EXPECT_TRUE(shadow.CasterBufferCapacity() >= kLargeCasterCount);
    IRhiBuffer* first_caster = nullptr;
    for (u32 draw = 0; draw < kLargeCasterCount; ++draw) {
        EXPECT_TRUE(shadow.TrySetCaster(
            FMat4::Translation(FVec3{static_cast<f32>(draw), 0, 0})));
        if (draw == 0u) first_caster = shadow.CasterObjectCB();
    }
    EXPECT_TRUE(first_caster != nullptr);
    EXPECT_TRUE(shadow.CasterObjectCB() != first_caster);
    EXPECT_FALSE(shadow.CasterOverflowed());
    EXPECT_EQ(shadow.CasterDrawCount(), kLargeCasterCount);
    EXPECT_EQ(shadow.CasterDrawCount(0), kLargeCasterCount);

    const u32 retained_capacity = shadow.CasterBufferCapacity();
    EXPECT_TRUE(shadow.BeginFrame(1u));
    EXPECT_EQ(shadow.CasterBufferCapacity(), retained_capacity);
    EXPECT_TRUE(shadow.TrySetCaster(FMat4::Identity()));
    EXPECT_TRUE(shadow.CasterObjectCB() == first_caster);

    shadow.Shutdown();
}

ACS_TEST(ShadowMap, EveryCascadeHasAnIndependentCasterRing)
{
    FDeviceConfig config{};
    auto device_result = CreateRhiDevice(config);
    if (device_result.IsErr()) return;

    CShadowMap shadow;
    EXPECT_TRUE(shadow.Init(*device_result.Value(), 64,
                            CShadowMap::kMaxCascades).IsOk());
    if (!shadow.DepthTexture()) return;

    // 4 * 65 exceeds the old shared 256-slot ring by four draws. Every cascade
    // must retain its complete caster set instead of dropping the final one.
    constexpr u32 kCastersPerCascade = 65;
    IRhiBuffer* first_buffer[CShadowMap::kMaxCascades] = {};
    EXPECT_TRUE(shadow.BeginFrame(kCastersPerCascade));
    for (u32 cascade = 0; cascade < CShadowMap::kMaxCascades; ++cascade) {
        shadow.SetCurrentCascade(cascade);
        for (u32 draw = 0; draw < kCastersPerCascade; ++draw) {
            EXPECT_TRUE(shadow.TrySetCaster(FMat4::Translation(
                FVec3{static_cast<f32>(draw), static_cast<f32>(cascade), 0})));
            if (draw == 0) first_buffer[cascade] = shadow.CasterObjectCB();
        }
        EXPECT_EQ(shadow.CasterDrawCount(cascade), kCastersPerCascade);
        EXPECT_FALSE(shadow.CasterOverflowed(cascade));
    }

    EXPECT_EQ(shadow.CasterDrawCount(),
              kCastersPerCascade * CShadowMap::kMaxCascades);
    EXPECT_FALSE(shadow.CasterOverflowed());
    for (u32 cascade = 1; cascade < CShadowMap::kMaxCascades; ++cascade)
        EXPECT_TRUE(first_buffer[cascade] != first_buffer[cascade - 1]);

    shadow.Shutdown();
}
