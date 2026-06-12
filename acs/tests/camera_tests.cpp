// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework/Camera2D の検証:
//   target 追従 (snap / 指数 smoothing / デッドゾーン)、bounds clamp、trauma shake 減衰、
//   world↔screen 座標往復を確認する (これまで未テストだった FCamera2D を固定)。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Camera2D.h"
#include "math/Vec.h"

#include <cmath>

using namespace acs;
using namespace acs::game;

// smoothing<=0 は即スナップ。
ACS_TEST(Camera2D, SnapFollow) {
    FCamera2D cam;
    cam.SetTargetPos(FVec2{ 10.0f, 5.0f }, 0.0f);
    cam.Tick(1.0f / 60.0f);
    EXPECT_NEAR(cam.Position().x, 10.0f, 1e-4f);
    EXPECT_NEAR(cam.Position().y, 5.0f, 1e-4f);
}

// 指数 smoothing: dt 後に 1-exp(-s·dt) の割合だけ詰める。
ACS_TEST(Camera2D, ExponentialSmoothing) {
    FCamera2D cam;
    cam.SetTargetPos(FVec2{ 10.0f, 0.0f }, 5.0f);
    cam.Tick(0.2f);                                   // 1-exp(-1)=0.632
    EXPECT_NEAR(cam.Position().x, 6.32f, 0.05f);
    EXPECT_TRUE(cam.Position().x > 0.0f && cam.Position().x < 10.0f);
}

// bounds clamp: target が範囲外でも position は矩形内。
ACS_TEST(Camera2D, BoundsClamp) {
    FCamera2D cam;
    cam.SetBounds(FVec2{ 0.0f, 0.0f }, FVec2{ 5.0f, 5.0f });
    cam.SetTargetPos(FVec2{ 10.0f, 10.0f }, 0.0f);
    cam.Tick(1.0f / 60.0f);
    EXPECT_NEAR(cam.Position().x, 5.0f, 1e-4f);
    EXPECT_NEAR(cam.Position().y, 5.0f, 1e-4f);
}

// デッドゾーン: 箱の中は動かず、外に出たら target を箱の縁に保つ。
ACS_TEST(Camera2D, Deadzone) {
    FCamera2D cam;   // position (0,0)
    cam.SetDeadzone(FVec2{ 2.0f, 2.0f });

    cam.SetTargetPos(FVec2{ 1.0f, 0.0f }, 0.0f);      // 箱の中 (|1|<2)
    cam.Tick(1.0f / 60.0f);
    EXPECT_NEAR(cam.Position().x, 0.0f, 1e-4f);       // 動かない

    cam.SetTargetPos(FVec2{ 5.0f, 0.0f }, 0.0f);      // 箱の外 (|5|>2)
    cam.Tick(1.0f / 60.0f);
    EXPECT_NEAR(cam.Position().x, 3.0f, 1e-4f);       // target を縁(2)に保つ → 5-2=3
}

// trauma shake: shake が出て、減衰して 0 に戻ると offset も消える。
ACS_TEST(Camera2D, ShakeDecays) {
    FCamera2D cam;
    cam.SetShakeAmplitude(0.5f);
    cam.SetShakeDecayRate(1.0f);
    cam.AddShake(1.0f);

    cam.Tick(0.1f);
    EXPECT_TRUE(cam.TraumaLevel() < 1.0f);            // 減衰開始
    EXPECT_TRUE(cam.TraumaLevel() > 0.0f);

    for (int i = 0; i < 200; ++i) cam.Tick(1.0f / 60.0f);   // > 1s 経過
    EXPECT_NEAR(cam.TraumaLevel(), 0.0f, 1e-4f);      // trauma 枯渇
    EXPECT_NEAR(cam.EffectiveViewCenter().x, cam.Position().x, 1e-4f);   // shake offset も 0
    EXPECT_NEAR(cam.EffectiveViewCenter().y, cam.Position().y, 1e-4f);
}

// world↔screen 往復 (zoom 込み、shake なし)。
ACS_TEST(Camera2D, ScreenWorldRoundTrip) {
    FCamera2D cam;
    cam.SetPosition(FVec2{ 10.0f, 20.0f });
    cam.SetZoom(2.0f);
    const FVec2 world{ 15.0f, 25.0f };
    const FVec2 screen = cam.WorldToScreen(world, 800u, 600u);
    const FVec2 back   = cam.ScreenToWorld(screen, 800u, 600u);
    EXPECT_NEAR(back.x, world.x, 1e-3f);
    EXPECT_NEAR(back.y, world.y, 1e-3f);
}
