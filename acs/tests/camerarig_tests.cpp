// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// math/CameraRig.h の検証 (軌道カメラ + 射影/逆射影):
//   符号・ハンドネス・スクリーンY向きが «逆になりがち» な箇所なので、行列ベースで
//   実装した world→screen と screen→ray が «往復で厳密一致» することを自己検証する
//   (= 期待値を手で当てずに、射影→逆射影→元の点に戻るかで正しさを保証)。
//   検証項目:
//     - OrbitEye の規約 (yaw/pitch/dist → eye 位置)
//     - 注視点はスクリーン中央へ射影される
//     - 射影→逆射影レイが元のワールド点を厳密に通る (往復・前方判定)
//     - カメラ後方の点は射影が false
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "math/CameraRig.h"
#include "math/Camera.h"
#include "math/Vec.h"
#include "math/Math.h"
#include "foundation/TypeTraits.h"

using namespace acs;

namespace {

static_assert(IsSameV<FCamera, CCamera>, "旧カメラ名は正規型の互換別名である必要があります");

void ExpectVec3Near(FVec3 a, FVec3 b, f32 eps) noexcept {
    EXPECT_NEAR(a.x, b.x, eps);
    EXPECT_NEAR(a.y, b.y, eps);
    EXPECT_NEAR(a.z, b.z, eps);
}

// 点 P からレイ (origin,dir 正規化) への垂直距離。
f32 PointRayDistance(FVec3 p, const FRay3& r) noexcept {
    const FVec3 op{ p.x - r.origin.x, p.y - r.origin.y, p.z - r.origin.z };
    const f32 t = op.x * r.direction.x + op.y * r.direction.y + op.z * r.direction.z;
    const FVec3 closest{ r.origin.x + r.direction.x * t,
                         r.origin.y + r.direction.y * t,
                         r.origin.z + r.direction.z * t };
    const FVec3 d{ p.x - closest.x, p.y - closest.y, p.z - closest.z };
    return Sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

constexpr f32 kFov = 50.0f * kPi / 180.0f;

} // namespace

// --- OrbitEye の規約 (target→eye 方向) --------------------------------------
ACS_TEST(CameraRig, OrbitEyeConventions) {
    const FVec3 t{ 0, 0, 0 };
    // yaw=0, pitch=0 → eye は +Z 方向 (dir = (0,0,1))
    ExpectVec3Near(OrbitEye(t, 0.0f, 0.0f, 10.0f), FVec3{ 0, 0, 10 }, 1e-4f);
    // pitch=+90° (見下ろし) → eye は真上 (dir = (0,1,0))
    ExpectVec3Near(OrbitEye(t, 0.0f, kHalfPi, 10.0f), FVec3{ 0, 10, 0 }, 1e-4f);
    // yaw=+90° → eye は +X 方向 (dir = (1,0,0))
    ExpectVec3Near(OrbitEye(t, kHalfPi, 0.0f, 10.0f), FVec3{ 10, 0, 0 }, 1e-4f);
    // 距離が反映される
    ExpectVec3Near(OrbitEye(FVec3{ 1, 2, 3 }, 0.0f, 0.0f, 5.0f), FVec3{ 1, 2, 8 }, 1e-4f);
}

// --- 注視点はスクリーン中央へ射影される -------------------------------------
ACS_TEST(CameraRig, TargetProjectsToScreenCenter) {
    const f32 w = 1280.0f, h = 720.0f;
    const FVec3 target{ 2, 1, -3 };
    const CCamera cam = MakeOrbitCamera(target, 0.7f, 0.45f, 12.0f, kFov, w / h, 0.05f, 500.0f);
    FVec2 px;
    EXPECT_TRUE(WorldToScreen(cam, target, w, h, px));
    EXPECT_NEAR(px.x, w * 0.5f, 0.5f);
    EXPECT_NEAR(px.y, h * 0.5f, 0.5f);
}

// --- 射影 → 逆射影レイが元のワールド点を厳密に通る (往復・自己検証) ----------
ACS_TEST(CameraRig, ProjectUnprojectRoundTrip) {
    const f32 w = 1280.0f, h = 720.0f;
    const FVec3 target{ 0, 0, 0 };
    // いくつかの軌道アングルで検証 (ハンドネス/符号が崩れていれば往復が破綻する)
    struct FAng { f32 yaw, pitch, dist; };
    const FAng angs[] = { { 0.0f, 0.0f, 12.0f }, { 0.78f, 0.55f, 14.0f },
                         { -1.2f, 0.30f, 10.0f }, { 2.4f, -0.40f, 16.0f } };
    // 視界内に入るワールド点 (注視点近傍)
    const FVec3 pts[] = { { 0, 0, 0 }, { 1.5f, 0.5f, -1.0f }, { -2.0f, 1.0f, 0.5f },
                          { 0.5f, -1.5f, 1.0f }, { 2.0f, 2.0f, -2.0f } };

    for (const FAng& a : angs) {
        const CCamera cam = MakeOrbitCamera(target, a.yaw, a.pitch, a.dist, kFov, w / h, 0.05f, 500.0f);
        const FVec3 eye = cam.Eye();
        for (const FVec3 p : pts) {
            FVec2 px;
            EXPECT_TRUE(WorldToScreen(cam, p, w, h, px));        // 前方で射影できる
            const FRay3 ray = ScreenPointToRay(cam, px.x, px.y, w, h);
            // 逆射影レイは元の点を «厳密に» 通る (往復一致)
            EXPECT_TRUE(PointRayDistance(p, ray) < 1e-2f);
            // レイは前方を向く (点はレイ origin より前方 = t>0)
            const FVec3 op{ p.x - ray.origin.x, p.y - ray.origin.y, p.z - ray.origin.z };
            const f32 t = op.x * ray.direction.x + op.y * ray.direction.y + op.z * ray.direction.z;
            EXPECT_TRUE(t > 0.0f);
            // レイは eye から外側へ (eye→点 と同じ向き)
            const FVec3 ep{ p.x - eye.x, p.y - eye.y, p.z - eye.z };
            const f32 dot = ep.x * ray.direction.x + ep.y * ray.direction.y + ep.z * ray.direction.z;
            EXPECT_TRUE(dot > 0.0f);
        }
    }
}

// --- スクリーン中央レイは注視点を通る ---------------------------------------
ACS_TEST(CameraRig, CenterRayHitsTarget) {
    const f32 w = 1280.0f, h = 720.0f;
    const FVec3 target{ -1, 2, 3 };
    const CCamera cam = MakeOrbitCamera(target, 0.5f, 0.6f, 13.0f, kFov, w / h, 0.05f, 500.0f);
    const FRay3 ray = ScreenPointToRay(cam, w * 0.5f, h * 0.5f, w, h);
    EXPECT_TRUE(PointRayDistance(target, ray) < 1e-2f);
}

// --- カメラ後方の点は射影が false -------------------------------------------
ACS_TEST(CameraRig, BehindCameraReturnsFalse) {
    const f32 w = 1280.0f, h = 720.0f;
    const FVec3 target{ 0, 0, 0 };
    const CCamera cam = MakeOrbitCamera(target, 0.0f, 0.0f, 10.0f, kFov, w / h, 0.05f, 500.0f);
    // eye は (0,0,10) で -Z を向く。eye の «後ろ» (z>10) はカメラ後方。
    FVec2 px;
    EXPECT_FALSE(WorldToScreen(cam, FVec3{ 0, 0, 20 }, w, h, px));
}
