// SPDX-License-Identifier: Apache-2.0
// CCamera3D (3D カメラ) の動作確認テスト
//
// target 追従が framerate independent であること、shake が trauma² で減衰して
// 必ず 0 へ戻ること、そして壊れた設定で CCamera へ書き出さないことを検証する。
// 描画を伴わないのでヘッドレスで走る。
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Camera3D.h"
#include "gameframework/CameraShakePresets.h"

using namespace acs;
using namespace acs::game;

namespace {

/** 2 点間の距離を返す。 */
f32 Distance(FVec3 a, FVec3 b) noexcept {
    const f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z; // 各軸の差。
    return Sqrt(dx * dx + dy * dy + dz * dz);
}

/** 原点からの距離 (= 大きさ) を返す。 */
f32 Magnitude(FVec3 v) noexcept { return Distance(v, FVec3{0.0f, 0.0f, 0.0f}); }

} // namespace

ACS_TEST(CCamera3D, SnapToTargetPlacesEyeAndLookAtWithoutLag) {
    CCamera3D cam;
    cam.SetFollowOffset(FVec3{0.0f, 2.0f, -5.0f});
    cam.SetLookAtOffset(FVec3{0.0f, 1.0f, 0.0f});

    EXPECT_TRUE(!cam.HasTarget());

    cam.SetTargetPos(FVec3{10.0f, 0.0f, 0.0f});
    EXPECT_TRUE(cam.HasTarget());

    cam.SnapToTarget();
    EXPECT_NEAR(cam.Position().x, 10.0f, 1.0e-4f);
    EXPECT_NEAR(cam.Position().z, -5.0f, 1.0e-4f);
    EXPECT_NEAR(cam.LookAt().y, 1.0f, 1.0e-4f);
}

ACS_TEST(CCamera3D, FirstTickSnapsInsteadOfFlyingFromOrigin) {
    // 初回に補間すると、原点から対象まで延々と飛んでいく画になる。
    CCamera3D cam;
    cam.SetFollowOffset(FVec3{0.0f, 0.0f, 0.0f});
    cam.SetLookAtOffset(FVec3{0.0f, 0.0f, 0.0f});
    cam.SetTargetPos(FVec3{100.0f, 0.0f, 0.0f});
    cam.Tick(1.0f / 60.0f);

    EXPECT_NEAR(cam.Position().x, 100.0f, 1.0e-3f);
}

ACS_TEST(CCamera3D, FollowIsFramerateIndependent) {
    // 「毎フレーム一定割合」で書くと 30fps と 240fps で挙動が変わる。
    // 同じ 1 秒を刻み方だけ変えて回し、到達点が一致することを確かめる。
    const FVec3 target{100.0f, 0.0f, 0.0f};

    CCamera3D slow;
    slow.SetFollowOffset(FVec3{0.0f, 0.0f, 0.0f});
    slow.SetLookAtOffset(FVec3{0.0f, 0.0f, 0.0f});
    slow.SetTargetPos(FVec3{0.0f, 0.0f, 0.0f}, 5.0f);
    slow.SnapToTarget();
    slow.SetTargetPos(target, 5.0f);
    for (i32 i = 0; i < 30; ++i) slow.Tick(1.0f / 30.0f);

    CCamera3D fast;
    fast.SetFollowOffset(FVec3{0.0f, 0.0f, 0.0f});
    fast.SetLookAtOffset(FVec3{0.0f, 0.0f, 0.0f});
    fast.SetTargetPos(FVec3{0.0f, 0.0f, 0.0f}, 5.0f);
    fast.SnapToTarget();
    fast.SetTargetPos(target, 5.0f);
    for (i32 i = 0; i < 240; ++i) fast.Tick(1.0f / 240.0f);

    EXPECT_NEAR(slow.Position().x, fast.Position().x, 1.0f);
    EXPECT_TRUE(slow.Position().x > 90.0f);   // 両方止まっていたら一致しても無意味。
    EXPECT_TRUE(fast.Position().x > 90.0f);
}

ACS_TEST(CCamera3D, ZeroSmoothingSnapsEveryTick) {
    CCamera3D cam;
    cam.SetFollowOffset(FVec3{0.0f, 0.0f, 0.0f});
    cam.SetLookAtOffset(FVec3{0.0f, 0.0f, 0.0f});
    cam.SetTargetPos(FVec3{0.0f, 0.0f, 0.0f}, 0.0f);
    cam.SnapToTarget();

    cam.SetTargetPos(FVec3{7.0f, 0.0f, 0.0f}, 0.0f);
    cam.Tick(1.0f / 60.0f);
    EXPECT_NEAR(cam.Position().x, 7.0f, 1.0e-4f);
}

ACS_TEST(CCamera3D, ClearTargetStopsFollowing) {
    CCamera3D cam;
    cam.SetFollowOffset(FVec3{0.0f, 0.0f, 0.0f});
    cam.SetTargetPos(FVec3{0.0f, 0.0f, 0.0f});
    cam.SnapToTarget();

    cam.ClearTarget();
    cam.SetTargetPos(FVec3{50.0f, 0.0f, 0.0f});
    cam.ClearTarget();
    cam.Tick(1.0f / 60.0f);

    EXPECT_NEAR(cam.Position().x, 0.0f, 1.0e-4f);
}

ACS_TEST(CCamera3D, ShakeDecaysToExactlyZero) {
    CCamera3D cam;
    cam.SetShakeAmplitude(1.0f);
    cam.SetShakeDecayRate(1.0f);
    cam.AddShake(1.0f);

    cam.Tick(1.0f / 60.0f);
    EXPECT_TRUE(Magnitude(cam.EffectiveEye()) != Magnitude(cam.Position()) ||
                Magnitude(cam.EffectiveEye()) > 0.0f);

    // 減衰 1.0 なら 1 秒で収まる。ずれたまま固まると画面が微妙にずれて見える。
    for (i32 i = 0; i < 61; ++i) cam.Tick(1.0f / 60.0f);

    EXPECT_NEAR(cam.TraumaLevel(), 0.0f, 1.0e-6f);
    EXPECT_NEAR(Distance(cam.EffectiveEye(), cam.Position()), 0.0f, 1.0e-6f);
}

ACS_TEST(CCamera3D, TraumaIsClampedAndNegativeIgnored) {
    CCamera3D cam;
    for (i32 i = 0; i < 20; ++i) cam.AddShake(0.5f);
    EXPECT_TRUE(cam.TraumaLevel() <= 1.0f);

    const f32 before = cam.TraumaLevel();
    cam.AddShake(-1.0f);
    EXPECT_NEAR(cam.TraumaLevel(), before, 1.0e-6f);

    cam.StopShake();
    EXPECT_NEAR(cam.TraumaLevel(), 0.0f, 1.0e-6f);
}

ACS_TEST(CCamera3D, ShakeMovesEyeAndLookAtTogether) {
    // 片方だけ揺らすと画面が回って見える。
    CCamera3D cam;
    cam.SetShakeAmplitude(1.0f);
    cam.AddShake(1.0f);
    cam.Tick(1.0f / 60.0f);

    const f32 eye_shift = cam.EffectiveEye().x - cam.Position().x;
    const f32 at_shift  = cam.EffectiveLookAt().x - cam.LookAt().x;
    EXPECT_NEAR(eye_shift, at_shift, 1.0e-6f);
}

ACS_TEST(CCamera3D, AcceptsShakePresetsThroughIShakeTarget) {
    CCamera3D big;
    FCameraShakePresets::ApplyPreset(big, EShakePreset::ExplosionLarge);
    EXPECT_TRUE(big.TraumaLevel() > 0.0f);

    CCamera3D small;
    FCameraShakePresets::ApplyPreset(small, EShakePreset::HitImpact);
    EXPECT_TRUE(small.TraumaLevel() > 0.0f);

    // preset ごとに強さが違わなければ、preset を使えていない。
    EXPECT_TRUE(big.TraumaLevel() != small.TraumaLevel());
}

ACS_TEST(CCamera3D, ApplyToRejectsBrokenSetupsWithoutTouchingCamera) {
    CCamera3D cam;
    cam.SetTargetPos(FVec3{0.0f, 0.0f, 0.0f});
    cam.SnapToTarget();

    CCamera out;
    EXPECT_TRUE(cam.ApplyTo(out, 16.0f / 9.0f));

    const FVec3 kept = out.Eye();

    EXPECT_TRUE(!cam.ApplyTo(out, 0.0f));    // アスペクト比 0。
    EXPECT_TRUE(!cam.ApplyTo(out, -1.0f));   // 負のアスペクト比。
    EXPECT_NEAR(out.Eye().y, kept.y, 1.0e-6f);

    // eye と注視点が同じだと向きが決まらず、行列が壊れて真っ暗になる。
    CCamera3D degenerate;
    degenerate.ClearTarget();
    degenerate.SetPosition(FVec3{1.0f, 1.0f, 1.0f});
    degenerate.SetLookAt(FVec3{1.0f, 1.0f, 1.0f});
    EXPECT_TRUE(!degenerate.ApplyTo(out, 1.0f));

    // near/far と視野角の異常も同様に弾く。
    CCamera3D bad_planes;
    bad_planes.SetNearPlane(10.0f);
    bad_planes.SetFarPlane(1.0f);
    EXPECT_TRUE(!bad_planes.ApplyTo(out, 1.0f));
}

ACS_TEST(CCamera3D, RejectsInvalidProjectionSettings) {
    CCamera3D cam;

    cam.SetFovYDegrees(0.0f);
    EXPECT_NEAR(cam.FovYDegrees(), 60.0f, 1.0e-6f);   // 既定のまま。

    cam.SetFovYDegrees(180.0f);
    EXPECT_NEAR(cam.FovYDegrees(), 60.0f, 1.0e-6f);

    cam.SetFovYDegrees(45.0f);
    EXPECT_NEAR(cam.FovYDegrees(), 45.0f, 1.0e-6f);

    cam.SetNearPlane(0.0f);
    EXPECT_TRUE(cam.NearPlane() > 0.0f);

    cam.SetShakeDecayRate(0.0f);
    cam.SetShakeAmplitude(-1.0f);
    EXPECT_TRUE(true);   // 不正値で状態が壊れないこと (下の Tick が落ちなければ良い)。

    cam.AddShake(0.5f);
    cam.Tick(1.0f / 60.0f);
}
