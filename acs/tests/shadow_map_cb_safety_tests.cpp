// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"

#include "math/Math.h"
#include "render/IRhiDevice.h"
#include "render/ShadowMap.h"

using namespace acs;

ACS_TEST(ShadowMap, CascadeAndCasterBuffersRemainDrawImmutable)
{
    FDeviceConfig config{};
    auto device_result = CreateRhiDevice(config);
    if (device_result.IsErr()) return; // Headless CI may not expose a GPU.

    FShadowMap shadow;
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

    shadow.BeginFrame();
    EXPECT_TRUE(shadow.TrySetCaster(FMat4::Translation(FVec3{1, 0, 0})));
    IRhiBuffer* caster0 = shadow.CasterObjectCB();
    EXPECT_TRUE(shadow.TrySetCaster(FMat4::Translation(FVec3{2, 0, 0})));
    IRhiBuffer* caster1 = shadow.CasterObjectCB();
    EXPECT_TRUE(caster0 != nullptr);
    EXPECT_TRUE(caster0 != caster1);
    EXPECT_EQ(shadow.CasterDrawCount(), 2u);

    // A new frame reuses slot 0 only after the previous frame's RHI slot advances.
    shadow.BeginFrame();
    EXPECT_TRUE(shadow.TrySetCaster(FMat4::Translation(FVec3{3, 0, 0})));
    EXPECT_TRUE(shadow.CasterObjectCB() == caster0);
    EXPECT_EQ(shadow.CasterDrawCount(), 1u);
    EXPECT_FALSE(shadow.CasterOverflowed());

    shadow.Shutdown();
}

ACS_TEST(ShadowMap, CasterRingNeverWrapsWithinFrame)
{
    FDeviceConfig config{};
    auto device_result = CreateRhiDevice(config);
    if (device_result.IsErr()) return;

    FShadowMap shadow;
    EXPECT_TRUE(shadow.Init(*device_result.Value(), 64, 1).IsOk());
    if (!shadow.DepthTexture()) return;

    shadow.BeginFrame();
    for (u32 draw = 0; draw < FShadowMap::kMaxCasterDrawsPerCascade; ++draw) {
        EXPECT_TRUE(shadow.TrySetCaster(
            FMat4::Translation(FVec3{static_cast<f32>(draw), 0, 0})));
    }
    IRhiBuffer* last_valid = shadow.CasterObjectCB();
    EXPECT_FALSE(shadow.TrySetCaster(FMat4::Identity()));
    EXPECT_TRUE(shadow.CasterOverflowed());
    EXPECT_TRUE(shadow.CasterOverflowed(0));
    EXPECT_EQ(shadow.CasterDrawCount(),
              FShadowMap::kMaxCasterDrawsPerCascade);
    EXPECT_EQ(shadow.CasterDrawCount(0),
              FShadowMap::kMaxCasterDrawsPerCascade);
    EXPECT_TRUE(shadow.CasterObjectCB() == last_valid);

    shadow.Shutdown();
}

ACS_TEST(ShadowMap, EveryCascadeHasAnIndependentCasterRing)
{
    FDeviceConfig config{};
    auto device_result = CreateRhiDevice(config);
    if (device_result.IsErr()) return;

    FShadowMap shadow;
    EXPECT_TRUE(shadow.Init(*device_result.Value(), 64,
                            FShadowMap::kMaxCascades).IsOk());
    if (!shadow.DepthTexture()) return;

    // 4 * 65 exceeds the old shared 256-slot ring by four draws. Every cascade
    // must retain its complete caster set instead of dropping the final one.
    constexpr u32 kCastersPerCascade = 65;
    IRhiBuffer* first_buffer[FShadowMap::kMaxCascades] = {};
    shadow.BeginFrame();
    for (u32 cascade = 0; cascade < FShadowMap::kMaxCascades; ++cascade) {
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
              kCastersPerCascade * FShadowMap::kMaxCascades);
    EXPECT_FALSE(shadow.CasterOverflowed());
    for (u32 cascade = 1; cascade < FShadowMap::kMaxCascades; ++cascade)
        EXPECT_TRUE(first_buffer[cascade] != first_buffer[cascade - 1]);

    shadow.Shutdown();
}
