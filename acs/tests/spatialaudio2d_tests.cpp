// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework/SpatialAudio2D.h の検証:
//   2D 平面向け空間化ヘルパ (純数学) — 距離 / 距離減衰 / 左右パン / 等パワーゲイン、
//   および FSpatialAudio2D マネージャ (listener 1 + source N) を確認する。
//   距離減衰式は 3D FSpatialAudio と同一なので代表点で値一致を検証する。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/SpatialAudio2D.h"
#include "math/Math.h"

using namespace acs;
using namespace acs::game;

// --- ComputeDistance2D: ピタゴラス (3,4)=5 ---
ACS_TEST(SpatialAudio2D, DistancePythagorean) {
    EXPECT_NEAR(ComputeDistance2D({0, 0}, {3, 4}), 5.0f, 1e-4f);
    EXPECT_NEAR(ComputeDistance2D({1, 1}, {4, 5}), 5.0f, 1e-4f);  // (3,4) シフト
    EXPECT_NEAR(ComputeDistance2D({2, 2}, {2, 2}), 0.0f, 1e-4f);  // 同一点
}

// --- ComputeVolume2D Linear: d=0→1, d=max/2→0.5, d>=max→0 ---
ACS_TEST(SpatialAudio2D, VolumeLinear) {
    EXPECT_NEAR(ComputeVolume2D(0.0f,  20.0f, EAttenuationCurve::Linear), 1.0f, 1e-4f);
    EXPECT_NEAR(ComputeVolume2D(10.0f, 20.0f, EAttenuationCurve::Linear), 0.5f, 1e-4f);
    EXPECT_NEAR(ComputeVolume2D(20.0f, 20.0f, EAttenuationCurve::Linear), 0.0f, 1e-4f);
    EXPECT_NEAR(ComputeVolume2D(99.0f, 20.0f, EAttenuationCurve::Linear), 0.0f, 1e-4f); // culling
}

// --- ComputeVolume2D Inverse/Exponential: 単調減少 + 端点 ---
ACS_TEST(SpatialAudio2D, VolumeInverseExponentialMonotonic) {
    // 端点: d=0 で 1、d>=max で 0 (tail blend のため)。
    EXPECT_NEAR(ComputeVolume2D(0.0f,  20.0f, EAttenuationCurve::Inverse),     1.0f, 1e-4f);
    EXPECT_NEAR(ComputeVolume2D(20.0f, 20.0f, EAttenuationCurve::Inverse),     0.0f, 1e-4f);
    EXPECT_NEAR(ComputeVolume2D(0.0f,  20.0f, EAttenuationCurve::Exponential), 1.0f, 1e-4f);
    EXPECT_NEAR(ComputeVolume2D(20.0f, 20.0f, EAttenuationCurve::Exponential), 0.0f, 1e-4f);

    // 単調減少: 近いほど大きい。
    const f32 i_near = ComputeVolume2D(4.0f,  20.0f, EAttenuationCurve::Inverse);
    const f32 i_far  = ComputeVolume2D(12.0f, 20.0f, EAttenuationCurve::Inverse);
    EXPECT_TRUE(i_near > i_far);
    EXPECT_TRUE(i_far > 0.0f);
    const f32 e_near = ComputeVolume2D(4.0f,  20.0f, EAttenuationCurve::Exponential);
    const f32 e_far  = ComputeVolume2D(12.0f, 20.0f, EAttenuationCurve::Exponential);
    EXPECT_TRUE(e_near > e_far);

    // Inverse: ref_d = max/8 = 2.5、base = 1/(1 + 2.5/2.5) = 0.5、tail = 1 - 2.5/20 = 0.875。
    EXPECT_NEAR(ComputeVolume2D(2.5f, 20.0f, EAttenuationCurve::Inverse), 0.5f * 0.875f, 1e-4f);
}

// --- max_range <= 0 は 0 ---
ACS_TEST(SpatialAudio2D, VolumeZeroRange) {
    EXPECT_NEAR(ComputeVolume2D(1.0f, 0.0f,  EAttenuationCurve::Linear), 0.0f, 1e-4f);
    EXPECT_NEAR(ComputeVolume2D(1.0f, -5.0f, EAttenuationCurve::Linear), 0.0f, 1e-4f);
}

// --- ComputePan2D: 正面→0, 右→+1, 左→-1 (Y+ up、東向き θ=0) ---
ACS_TEST(SpatialAudio2D, PanCardinalFacingEast) {
    const FVec2 ear{0, 0};
    const f32   facing_east = 0.0f;   // forward = (1,0)、right = (0,-1)
    // 正面 (+X)
    EXPECT_NEAR(ComputePan2D(ear, facing_east, {5, 0}),  0.0f, 1e-4f);
    // 真右 = 南 (-Y)
    EXPECT_NEAR(ComputePan2D(ear, facing_east, {0, -5}), 1.0f, 1e-4f);
    // 真左 = 北 (+Y)
    EXPECT_NEAR(ComputePan2D(ear, facing_east, {0, 5}), -1.0f, 1e-4f);
    // 真後ろ (-X) も 0 (head-shadow 無し)
    EXPECT_NEAR(ComputePan2D(ear, facing_east, {-5, 0}), 0.0f, 1e-4f);
    // 重なりは 0
    EXPECT_NEAR(ComputePan2D(ear, facing_east, {0, 0}),  0.0f, 1e-4f);
}

// --- ComputePan2D: 向きを変えても基準が回る (北向き θ=π/2 → 東が右) ---
ACS_TEST(SpatialAudio2D, PanRotatedListener) {
    const FVec2 ear{0, 0};
    const f32   facing_north = kHalfPi;   // forward = (0,1)、right = (1,0)
    // 北を向くと東 (+X) が右
    EXPECT_NEAR(ComputePan2D(ear, facing_north, {5, 0}),  1.0f, 1e-4f);
    // 西 (-X) が左
    EXPECT_NEAR(ComputePan2D(ear, facing_north, {-5, 0}), -1.0f, 1e-4f);
    // 正面 (北 +Y) は 0
    EXPECT_NEAR(ComputePan2D(ear, facing_north, {0, 5}),  0.0f, 1e-4f);
}

// --- ComputeConstantPowerStereo2D: 中央で等パワー、端で片寄せ ---
ACS_TEST(SpatialAudio2D, ConstantPowerStereo) {
    f32 l, r;
    // 中央: L = R = cos(π/4) ≈ 0.7071、L²+R² = 1。
    ComputeConstantPowerStereo2D(0.0f, l, r);
    EXPECT_NEAR(l, 0.70710678f, 1e-4f);
    EXPECT_NEAR(r, 0.70710678f, 1e-4f);
    EXPECT_NEAR(l * l + r * r, 1.0f, 1e-4f);
    // 完全右: L=0, R=1。
    ComputeConstantPowerStereo2D(1.0f, l, r);
    EXPECT_NEAR(l, 0.0f, 1e-4f);
    EXPECT_NEAR(r, 1.0f, 1e-4f);
    // 完全左: L=1, R=0。
    ComputeConstantPowerStereo2D(-1.0f, l, r);
    EXPECT_NEAR(l, 1.0f, 1e-4f);
    EXPECT_NEAR(r, 0.0f, 1e-4f);
    // clamp: |pan|>1 は端に飽和。
    ComputeConstantPowerStereo2D(5.0f, l, r);
    EXPECT_NEAR(r, 1.0f, 1e-4f);
}

// --- FSpatialAudio2D マネージャ: 登録 → 取得 → 削除 ---
ACS_TEST(SpatialAudio2D, ManagerRegisterComputeRemove) {
    FSpatialAudio2D spatial;
    spatial.SetListener({{0, 0}, 0.0f});   // 原点・東向き

    const u32 a = spatial.RegisterSource({10, 0}, 20.0f, EAttenuationCurve::Linear);
    const u32 b = spatial.RegisterSource({0, -5}, 20.0f, EAttenuationCurve::Linear);
    EXPECT_EQ(spatial.SourceCount(), 2u);
    EXPECT_TRUE(a != b);          // 単調増加・一意

    // a: 距離 10 / max 20 の Linear = 0.5、基準ゲイン 1.0。
    EXPECT_NEAR(spatial.ComputeAttenuatedVolume(a), 0.5f, 1e-4f);
    // a は正面 → pan 0。
    EXPECT_NEAR(spatial.ComputePan(a), 0.0f, 1e-4f);
    // b は真右 (南) → pan +1。
    EXPECT_NEAR(spatial.ComputePan(b), 1.0f, 1e-4f);

    // 基準ゲインを下げると最終 volume も比例して下がる。
    spatial.SetSourceVolume(a, 0.5f);
    EXPECT_NEAR(spatial.ComputeAttenuatedVolume(a), 0.25f, 1e-4f);

    // 移動 → 距離 0 で最大。
    spatial.UpdateSource(a, {0, 0});
    EXPECT_NEAR(spatial.ComputeAttenuatedVolume(a), 0.5f, 1e-4f);  // vol 0.5 × gain 1.0

    // 削除 → count 減・stale ID は 0。
    spatial.RemoveSource(a);
    EXPECT_EQ(spatial.SourceCount(), 1u);
    EXPECT_NEAR(spatial.ComputeAttenuatedVolume(a), 0.0f, 1e-4f);  // stale
    EXPECT_NEAR(spatial.ComputePan(b), 1.0f, 1e-4f);               // b は健在

    spatial.Clear();
    EXPECT_EQ(spatial.SourceCount(), 0u);
}

// --- FSpatialAudio2D: inactive / stale ID は 0 ---
ACS_TEST(SpatialAudio2D, ManagerStaleAndInactive) {
    FSpatialAudio2D spatial;
    spatial.SetListener({{0, 0}, 0.0f});
    // 未登録 ID は 0。
    EXPECT_NEAR(spatial.ComputeAttenuatedVolume(999u), 0.0f, 1e-4f);
    EXPECT_NEAR(spatial.ComputePan(999u), 0.0f, 1e-4f);
    // max_distance <= 0 は 20m 既定にクランプ → 距離 10 で 0.5。
    const u32 s = spatial.RegisterSource({10, 0}, -1.0f, EAttenuationCurve::Linear);
    EXPECT_NEAR(spatial.ComputeAttenuatedVolume(s), 0.5f, 1e-4f);
}
