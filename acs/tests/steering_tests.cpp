// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework/Steering2D の検証:
//   操舵力で速度を更新する小さな積分ループを回し、Seek が目標へ寄る・Arrive が
//   目標で停止する・Flee が離れる・PathFollower が経路末端へ到達することを確認する。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Steering2D.h"
#include "math/Vec.h"

#include <cmath>

using namespace acs;
using namespace acs::game;

namespace {
// 操舵力で 1 ステップ進める (vel += steer·dt → max_speed clamp → pos += vel·dt)。
void Integrate(FVec2& pos, FVec2& vel, FVec2 steer, f32 max_speed, f32 dt) noexcept {
    vel.x += steer.x * dt; vel.y += steer.y * dt;
    const f32 vl = std::sqrt(vel.x * vel.x + vel.y * vel.y);
    if (vl > max_speed && vl > 1e-9f) { vel.x *= max_speed / vl; vel.y *= max_speed / vl; }
    pos.x += vel.x * dt; pos.y += vel.y * dt;
}
} // namespace

// Seek: 目標へ寄っていく。
ACS_TEST(Steering2D, SeekApproachesTarget) {
    const FSteerParams sp{ 4.0f, 8.0f };
    FVec2 pos{ 0.0f, 0.0f }, vel{ 0.0f, 0.0f };
    const FVec2 target{ 10.0f, 0.0f };
    for (int i = 0; i < 300; ++i) Integrate(pos, vel, SteerSeek(pos, vel, target, sp), sp.max_speed, 1.0f / 30.0f);
    EXPECT_TRUE(pos.x > 8.0f);          // 目標 (10,0) 近傍まで到達
    EXPECT_NEAR(pos.y, 0.0f, 0.01f);    // 軸上の運動は軸上に留まる
}

// Arrive: 目標で減速し停止する。
ACS_TEST(Steering2D, ArriveStopsAtTarget) {
    const FSteerParams sp{ 4.0f, 8.0f };
    FVec2 pos{ 0.0f, 0.0f }, vel{ 0.0f, 0.0f };
    const FVec2 target{ 10.0f, 0.0f };
    for (int i = 0; i < 600; ++i) Integrate(pos, vel, SteerArrive(pos, vel, target, sp, 3.0f), sp.max_speed, 1.0f / 30.0f);
    EXPECT_NEAR(pos.x, 10.0f, 0.3f);                       // 目標で停止
    EXPECT_TRUE(std::sqrt(vel.x * vel.x + vel.y * vel.y) < 0.3f);
}

// Flee: 捕食者から離れる。
ACS_TEST(Steering2D, FleeMovesAway) {
    const FSteerParams sp{ 4.0f, 8.0f };
    FVec2 pos{ 1.0f, 0.0f }, vel{ 0.0f, 0.0f };
    const FVec2 predator{ 0.0f, 0.0f };
    for (int i = 0; i < 100; ++i) Integrate(pos, vel, SteerFlee(pos, vel, predator, sp), sp.max_speed, 1.0f / 30.0f);
    EXPECT_TRUE(pos.x > 3.0f);          // 離れた
}

// PathFollower: waypoint 列を辿って末端へ到達し done になる。
ACS_TEST(Steering2D, PathFollowerReachesEnd) {
    const FSteerParams sp{ 4.0f, 8.0f };
    const FVec2 wp[] = { FVec2{ 5.0f, 0.0f }, FVec2{ 5.0f, 5.0f }, FVec2{ 0.0f, 5.0f } };
    CPathFollower pf;
    pf.SetPath(wp, 3u, 0.5f);

    FVec2 pos{ 0.0f, 0.0f }, vel{ 0.0f, 0.0f };
    bool done = false;
    int steps = 0;
    for (; steps < 3000 && !done; ++steps)
        Integrate(pos, vel, pf.Steer(pos, vel, sp, done), sp.max_speed, 1.0f / 30.0f);

    EXPECT_TRUE(done);
    EXPECT_TRUE(steps < 3000);
    EXPECT_NEAR(pos.x, 0.0f, 0.6f);     // 最後の waypoint (0,5)
    EXPECT_NEAR(pos.y, 5.0f, 0.6f);
}
