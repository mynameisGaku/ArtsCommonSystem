// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework/RigidWorld2D.{h,cpp} の検証:
//   線形インパルスベースの 2D 剛体ソルバが、落下→静止・弾性衝突の速度交換・
//   質量比に応じた運動量移動を物理的に正しく再現することを確認する。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/RigidWorld2D.h"
#include "math/Vec.h"

#include <cmath>   // std::fabs

using namespace acs;
using namespace acs::game;

// 動的円が静的床に落ちて静止する (めり込みなし・速度 ~0)。+Y=画面下。
ACS_TEST(RigidWorld2D, DynamicCircleRestsOnFloor) {
    CRigidWorld2D w;
    w.AddStaticAabb(FVec2{ 0.0f, 10.0f }, FVec2{ 10.0f, 0.5f }, 0.0f);  // 上端 y=9.5
    const u32 ball = w.AddCircle(FVec2{ 0.0f, 0.0f }, 0.5f, 1.0f, 0.0f);

    for (int i = 0; i < 600; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 10.0f });

    const FVec2 p = w.Position(ball);
    const FVec2 v = w.Velocity(ball);
    EXPECT_NEAR(p.y, 9.0f, 0.05f);            // 底面 (p.y+0.5) が床上端 9.5 に接する
    EXPECT_TRUE(std::fabs(v.y) < 0.3f);       // ほぼ静止
}

// 等質量・正面衝突・反発 1 → 速度が入れ替わる (弾性衝突、運動量保存)。
ACS_TEST(RigidWorld2D, ElasticHeadOnSwapsVelocity) {
    CRigidWorld2D w;
    const u32 a = w.AddCircle(FVec2{ -2.0f, 0.0f }, 0.5f, 1.0f, 1.0f);
    const u32 b = w.AddCircle(FVec2{  2.0f, 0.0f }, 0.5f, 1.0f, 1.0f);
    w.SetVelocity(a, FVec2{  3.0f, 0.0f });
    w.SetVelocity(b, FVec2{ -3.0f, 0.0f });

    for (int i = 0; i < 200; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 0.0f });

    EXPECT_NEAR(w.Velocity(a).x, -3.0f, 0.3f);   // 速度交換
    EXPECT_NEAR(w.Velocity(b).x,  3.0f, 0.3f);
    EXPECT_TRUE(w.Position(a).x < w.Position(b).x);   // すり抜けず分離
}

// 軽いボディが重いボディに衝突: 重い方はほとんど動かない (運動量はわずかしか移らない)。
ACS_TEST(RigidWorld2D, HeavyBodyBarelyMoves) {
    CRigidWorld2D w;
    const u32 light = w.AddCircle(FVec2{ -2.0f, 0.0f }, 0.5f,    1.0f, 0.0f);
    const u32 heavy = w.AddCircle(FVec2{  0.0f, 0.0f }, 0.5f, 1000.0f, 0.0f);
    w.SetVelocity(light, FVec2{ 5.0f, 0.0f });

    for (int i = 0; i < 120; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 0.0f });

    EXPECT_TRUE(std::fabs(w.Velocity(heavy).x) < 0.2f);   // 重い方はほぼ不動
    EXPECT_TRUE(w.Velocity(light).x <= 0.6f);             // 軽い方は止まる (前進し続けない)
}

// 動的 AABB (箱) が静的床に落ちて静止する。
ACS_TEST(RigidWorld2D, DynamicBoxRestsOnFloor) {
    CRigidWorld2D w;
    w.AddStaticAabb(FVec2{ 0.0f, 10.0f }, FVec2{ 10.0f, 0.5f }, 0.0f);   // 上端 y=9.5
    const u32 box = w.AddDynamicAabb(FVec2{ 0.0f, 0.0f }, FVec2{ 0.5f, 0.5f }, 1.0f, 0.0f);

    for (int i = 0; i < 600; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 10.0f });

    EXPECT_NEAR(w.Position(box).y, 9.0f, 0.05f);          // 底面 (y+0.5)=9.5 で床上端に接する
    EXPECT_TRUE(std::fabs(w.Velocity(box).y) < 0.3f);
}

// 床上を滑る箱は摩擦で減速し、平らなまま停止する (ボールのように転がらない)。
// 2 点接触マニフォルドにより箱は安定して水平を保ち、不自然な回転 (こけ・スピン) をしない
// = 物理的に正しい (箱は転がり剛体でない)。
ACS_TEST(RigidWorld2D, FrictionSlowsSlidingBox) {
    CRigidWorld2D w;
    w.AddStaticAabb(FVec2{ 0.0f, 10.0f }, FVec2{ 50.0f, 0.5f }, 0.0f, 0.6f);
    const u32 box = w.AddDynamicAabb(FVec2{ 0.0f, 9.0f }, FVec2{ 0.5f, 0.5f }, 1.0f, 0.0f, 0.6f);
    w.SetVelocity(box, FVec2{ 3.0f, 0.0f });

    for (int i = 0; i < 240; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 10.0f });

    const f32 vx = w.Velocity(box).x;
    const f32 wv = w.AngularVelocity(box);
    EXPECT_TRUE(vx < 1.5f && vx > -0.3f);          // 摩擦で大きく減速 (逆走しない)
    EXPECT_TRUE(std::fabs(wv) < 0.5f);             // 箱は平らなまま (2 点接触で暴れない)
    EXPECT_NEAR(w.Position(box).y, 9.0f, 0.15f);   // 床上に留まる
}

// 箱が箱の上に積み上がって安定する (沈み込み/爆発しない)。
ACS_TEST(RigidWorld2D, BoxStacksStably) {
    CRigidWorld2D w;
    w.AddStaticAabb(FVec2{ 0.0f, 10.0f }, FVec2{ 10.0f, 0.5f }, 0.0f);   // 床上端 9.5
    const u32 b1 = w.AddDynamicAabb(FVec2{ 0.0f, 9.0f }, FVec2{ 0.5f, 0.5f }, 1.0f, 0.0f);  // 床上
    const u32 b2 = w.AddDynamicAabb(FVec2{ 0.0f, 8.0f }, FVec2{ 0.5f, 0.5f }, 1.0f, 0.0f);  // b1 上

    for (int i = 0; i < 600; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 10.0f });

    EXPECT_NEAR(w.Position(b1).y, 9.0f, 0.15f);   // 床上で安定
    EXPECT_NEAR(w.Position(b2).y, 8.0f, 0.2f);    // b1 の上で安定 (沈み込みわずか)
    EXPECT_TRUE(std::fabs(w.Velocity(b2).y) < 0.4f);   // 爆発していない
}

// 角速度は接触が無ければ保存され、角度が積分される。
ACS_TEST(RigidWorld2D, SpinPersistsWithoutContact) {
    CRigidWorld2D w;
    const u32 c = w.AddCircle(FVec2{ 0.0f, 0.0f }, 0.5f, 1.0f);
    w.SetAngularVelocity(c, 2.0f);
    for (int i = 0; i < 60; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 0.0f });   // 1.0s, 接触なし
    EXPECT_NEAR(w.Angle(c), 2.0f, 0.05f);                  // 2 rad/s × 1s
    EXPECT_NEAR(w.AngularVelocity(c), 2.0f, 1e-4f);        // 接触なし → 保存
}

// 床上を滑る円は摩擦で回転を生じる (転がり始める)。
ACS_TEST(RigidWorld2D, CircleRollsFromFriction) {
    CRigidWorld2D w;
    w.AddStaticAabb(FVec2{ 0.0f, 10.0f }, FVec2{ 50.0f, 0.5f }, 0.0f, 0.6f);
    const u32 ball = w.AddCircle(FVec2{ 0.0f, 9.0f }, 0.5f, 1.0f, 0.0f, 0.6f);
    w.SetVelocity(ball, FVec2{ 3.0f, 0.0f });
    for (int i = 0; i < 120; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 10.0f });
    EXPECT_TRUE(std::fabs(w.AngularVelocity(ball)) > 0.1f);   // 摩擦で回転が発生
}

// 中心を通る正面衝突 (摩擦0) は回転を生じない (法線の腕が 0)。
ACS_TEST(RigidWorld2D, CentralCollisionNoSpin) {
    CRigidWorld2D w;
    const u32 a = w.AddCircle(FVec2{ -2.0f, 0.0f }, 0.5f, 1.0f, 1.0f, 0.0f);
    const u32 b = w.AddCircle(FVec2{  2.0f, 0.0f }, 0.5f, 1.0f, 1.0f, 0.0f);
    w.SetVelocity(a, FVec2{  3.0f, 0.0f });
    w.SetVelocity(b, FVec2{ -3.0f, 0.0f });
    for (int i = 0; i < 200; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 0.0f });
    EXPECT_NEAR(w.AngularVelocity(a), 0.0f, 1e-3f);
    EXPECT_NEAR(w.AngularVelocity(b), 0.0f, 1e-3f);
}

// 斜面 (回転した静的ボックス OBB) に乗せた動的箱は、低摩擦なら斜面に沿って滑り落ちる。
// AABB では「見えない水平面」で止まってしまうが、OBB なら傾いた面で正しく滑り、x も y も
// 斜面方向へ動く (= 現実の斜面の挙動)。
ACS_TEST(RigidWorld2D, BoxSlidesDownSlope) {
    CRigidWorld2D w;
    const u32 slope = w.AddStaticAabb(FVec2{ 0.0f, 10.0f }, FVec2{ 40.0f, 0.5f }, 0.0f, 0.05f);
    w.SetAngle(slope, 0.45f);   // 約 26° の斜面
    const u32 box = w.AddDynamicAabb(FVec2{ 0.0f, 8.0f }, FVec2{ 0.5f, 0.5f }, 1.0f, 0.0f, 0.05f);

    for (int i = 0; i < 150; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 10.0f });   // 2.5s

    EXPECT_TRUE(std::fabs(w.Position(box).x) > 3.0f);   // 斜面を滑って横へ大きく移動 (ピタッと止まらない)
    EXPECT_TRUE(w.Velocity(box).y > 0.5f);              // 斜面に沿って下方 (+Y) へも進んでいる
    EXPECT_TRUE(w.Position(box).y < 24.0f);             // 斜面上を滑っている (端から落ちて自由落下していない)
}

// SetDamping の角減衰で、接触が無くても角速度が時間とともに自然に弱まる (逆転はしない)。
ACS_TEST(RigidWorld2D, AngularDampingSlowsSpin) {
    CRigidWorld2D w;
    const u32 c = w.AddCircle(FVec2{ 0.0f, 0.0f }, 0.5f, 1.0f);
    w.SetAngularVelocity(c, 4.0f);
    w.SetDamping(c, 0.0f, 2.0f);   // 角減衰 2/s

    for (int i = 0; i < 60; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 0.0f });   // 1s, 接触なし

    EXPECT_TRUE(w.AngularVelocity(c) < 2.0f && w.AngularVelocity(c) > 0.0f);   // 減衰したが正のまま
}

// 凸ポリゴン (三角形) が静的床に落ちて底辺で安定して静止する。
ACS_TEST(RigidWorld2D, PolygonRestsOnFloor) {
    CRigidWorld2D w;
    w.AddStaticAabb(FVec2{ 0.0f, 10.0f }, FVec2{ 20.0f, 0.5f }, 0.0f, 0.5f);   // 上端 y=9.5
    const FVec2 tri[3] = { { 0.0f, -0.6f }, { 0.6f, 0.5f }, { -0.6f, 0.5f } }; // 上頂点・底辺 y=0.5
    const u32 p = w.AddPolygon(FVec2{ 0.0f, 5.0f }, tri, 3u, 1.0f, 0.0f, 0.5f);

    for (int i = 0; i < 400; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 10.0f });

    EXPECT_NEAR(w.Position(p).y, 9.0f, 0.2f);          // 底辺 (pos.y+0.5)=9.5 で床上端に接する
    EXPECT_TRUE(std::fabs(w.Velocity(p).y) < 0.4f);    // 静止
}

// 円が凸ポリゴン (静的) の傾いた辺に当たって止まる (すり抜けない)。
ACS_TEST(RigidWorld2D, CircleVsPolygonBlocks) {
    CRigidWorld2D w;
    const FVec2 quad[4] = { { -3.0f, 0.0f }, { 3.0f, 0.0f }, { 3.0f, 1.0f }, { -3.0f, 1.0f } };  // 横長台
    w.AddPolygon(FVec2{ 0.0f, 10.0f }, quad, 4u, 0.0f, 0.0f, 0.5f);          // 静的ポリゴン (mass 0)
    const u32 ball = w.AddCircle(FVec2{ 0.0f, 5.0f }, 0.5f, 1.0f, 0.0f);     // 上から落ちる

    for (int i = 0; i < 400; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 10.0f });

    EXPECT_TRUE(w.Position(ball).y < 10.0f);           // 台 (上端 y=9) ですり抜けず止まる
    EXPECT_TRUE(std::fabs(w.Velocity(ball).y) < 0.4f);
}

// point / overlap クエリ。
ACS_TEST(RigidWorld2D, QueryPointAndOverlap) {
    CRigidWorld2D w;
    const u32 c0 = w.AddCircle(FVec2{ 0.0f, 0.0f }, 1.0f, 1.0f);
    const u32 b1 = w.AddStaticAabb(FVec2{ 5.0f, 0.0f }, FVec2{ 1.0f, 1.0f });

    EXPECT_EQ(w.QueryPoint(FVec2{ 0.5f, 0.0f }), static_cast<i32>(c0));   // 円内
    EXPECT_EQ(w.QueryPoint(FVec2{ 5.0f, 0.0f }), static_cast<i32>(b1));   // 箱内
    EXPECT_EQ(w.QueryPoint(FVec2{ 10.0f, 10.0f }), -1);                   // 空

    u32 out[8];
    EXPECT_EQ(w.OverlapCircle(FVec2{ 0.0f, 0.0f }, 2.0f, out, 8u), 1u);   // 円だけ
    EXPECT_EQ(w.OverlapCircle(FVec2{ 0.0f, 0.0f }, 10.0f, out, 8u), 2u);  // 両方
}

// レイキャスト: 最近接ヒット / 逆向き miss / 距離外 miss。
ACS_TEST(RigidWorld2D, RaycastHitsNearest) {
    CRigidWorld2D w;
    w.AddCircle(FVec2{ 5.0f, 0.0f }, 1.0f, 1.0f);          // x=5 の円 (左端 x=4)
    w.AddStaticAabb(FVec2{ 10.0f, 0.0f }, FVec2{ 1.0f, 1.0f });

    const FRayHit2D h = w.Raycast(FVec2{ 0.0f, 0.0f }, FVec2{ 1.0f, 0.0f }, 100.0f);
    EXPECT_TRUE(h.hit);
    EXPECT_NEAR(h.distance, 4.0f, 0.01f);
    EXPECT_NEAR(h.point.x, 4.0f, 0.01f);
    EXPECT_NEAR(h.normal.x, -1.0f, 0.01f);                 // こちら向きの法線

    EXPECT_TRUE(!w.Raycast(FVec2{ 0.0f, 0.0f }, FVec2{ -1.0f, 0.0f }, 100.0f).hit);  // 逆向き
    EXPECT_TRUE(!w.Raycast(FVec2{ 0.0f, 0.0f }, FVec2{  1.0f, 0.0f },   2.0f).hit);  // 距離外
}

// ボディ削除 (tombstone) と slot 再利用。
ACS_TEST(RigidWorld2D, RemoveBodyAndReuse) {
    CRigidWorld2D w;
    const u32 a = w.AddCircle(FVec2{ 0.0f, 0.0f }, 0.5f, 1.0f);
    const u32 b = w.AddCircle(FVec2{ 3.0f, 0.0f }, 0.5f, 1.0f);
    EXPECT_EQ(w.Count(), 2u);

    w.RemoveBody(a);
    EXPECT_EQ(w.Count(), 1u);
    EXPECT_TRUE(!w.IsActive(a));
    EXPECT_TRUE(w.IsActive(b));
    EXPECT_EQ(w.QueryPoint(FVec2{ 0.0f, 0.0f }), -1);      // 削除済みはクエリに出ない

    const u32 c = w.AddCircle(FVec2{ 9.0f, 0.0f }, 0.5f, 1.0f);
    EXPECT_EQ(c, a);                                       // 解放 slot を再利用
    EXPECT_EQ(w.Count(), 2u);
    EXPECT_TRUE(w.IsActive(c));
}

// センサ: 衝突応答せず素通りしつつ、OverlapBody で重なりを検出できる。
ACS_TEST(RigidWorld2D, SensorDetectsButDoesNotBlock) {
    CRigidWorld2D w;
    const u32 sensor = w.AddSensorCircle(FVec2{ 0.0f, 5.0f }, 1.0f);   // (0,5) のトリガ域
    const u32 ball   = w.AddCircle(FVec2{ 0.0f, 0.0f }, 0.5f, 1.0f);   // 落下 (+Y=下)

    bool detected = false;
    for (int i = 0; i < 120; ++i) {
        w.Step(1.0f / 60.0f, FVec2{ 0.0f, 10.0f });
        u32 out[8];
        if (w.OverlapBody(sensor, out, 8u) > 0u) {
            detected = true;
            EXPECT_EQ(out[0], ball);   // 検出されたのはボール
        }
    }
    EXPECT_TRUE(detected);                       // 通過中にトリガが反応した
    EXPECT_TRUE(w.Position(ball).y > 6.0f);      // 素通りして落下し続けた (跳ね返らない)
}

// OverlapBody は自身を除き、重なっているボディだけを返す。
ACS_TEST(RigidWorld2D, OverlapBodyExcludesSelf) {
    CRigidWorld2D w;
    const u32 zone = w.AddSensorAabb(FVec2{ 0.0f, 0.0f }, FVec2{ 2.0f, 2.0f });
    w.AddCircle(FVec2{ 0.0f, 0.0f }, 0.5f, 1.0f);    // 域内
    w.AddCircle(FVec2{ 1.5f, 0.0f }, 0.5f, 1.0f);    // 域内
    w.AddCircle(FVec2{ 9.0f, 0.0f }, 0.5f, 1.0f);    // 域外
    u32 out[8];
    EXPECT_EQ(w.OverlapBody(zone, out, 8u), 2u);     // 域内の 2 つ (自身は除外)
}

// 有向ボックス同士 (OBB-OBB) の重なり判定が回転を考慮する (SAT)。
// 長い横棒と、その 1 上に置いた同形の棒: 軸並行なら隙間で重ならないが、90° 回すと
// 縦に 2.0 伸びて重なる = 衝突が angle を見ている (AABB 近似では誤検出/見逃しになる)。
ACS_TEST(RigidWorld2D, ObbOverlapRespectsRotation) {
    {
        CRigidWorld2D w;
        const u32 a = w.AddDynamicAabb(FVec2{ 0.0f, 0.0f }, FVec2{ 2.0f, 0.2f }, 1.0f);
        w.AddDynamicAabb(FVec2{ 0.0f, 1.0f }, FVec2{ 2.0f, 0.2f }, 1.0f);   // 軸並行 → y に隙間
        u32 out[8];
        EXPECT_EQ(w.OverlapBody(a, out, 8u), 0u);                          // 重ならない
    }
    {
        CRigidWorld2D w;
        const u32 a = w.AddDynamicAabb(FVec2{ 0.0f, 0.0f }, FVec2{ 2.0f, 0.2f }, 1.0f);
        const u32 b = w.AddDynamicAabb(FVec2{ 0.0f, 1.0f }, FVec2{ 2.0f, 0.2f }, 1.0f);
        w.SetAngle(b, 1.5707963f);                                         // 90° → 縦に伸びる
        u32 out[8];
        EXPECT_TRUE(w.OverlapBody(a, out, 8u) >= 1u);                      // 回転で重なる
    }
}

// CCD: 高速な円が薄い静的壁をすり抜けない (トンネリング防止)。CCD 無効だと貫通する。
ACS_TEST(RigidWorld2D, CcdPreventsWallTunneling) {
    // 薄い縦壁 (x=0, 厚み 0.2) に、左から超高速で円を撃ち込む (1 step で 20 units 移動)。
    auto setup = [](CRigidWorld2D& w) -> u32 {
        w.AddStaticAabb(FVec2{ 0.0f, 0.0f }, FVec2{ 0.1f, 5.0f }, 0.0f);
        const u32 ball = w.AddCircle(FVec2{ -5.0f, 0.0f }, 0.3f, 1.0f, 0.0f);
        w.SetVelocity(ball, FVec2{ 1200.0f, 0.0f });   // 1200 px/s → dt=1/60 で 20/step
        return ball;
    };

    // CCD 無効: 1 step で壁を貫通して反対側へ抜ける。
    {
        CRigidWorld2D w; const u32 ball = setup(w);
        for (int i = 0; i < 3; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 0.0f });
        EXPECT_TRUE(w.Position(ball).x > 1.0f);     // すり抜けた (右側へ)
    }
    // CCD 有効: 壁の手前で止まる。
    {
        CRigidWorld2D w; const u32 ball = setup(w);
        w.SetCcd(ball, true);
        for (int i = 0; i < 10; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 0.0f });
        const f32 x = w.Position(ball).x;
        EXPECT_TRUE(x < 0.0f);                       // 壁 (x=0) を越えていない
        EXPECT_TRUE(x > -1.0f);                       // 壁の直前で止まった (跳ね返って遠ざかっていない)
    }
}

// CCD: 高速落下する円が薄い床をすり抜けず、床上で止まる。
ACS_TEST(RigidWorld2D, CcdStopsFastFallOnThinFloor) {
    CRigidWorld2D w;
    w.AddStaticAabb(FVec2{ 0.0f, 10.0f }, FVec2{ 10.0f, 0.1f }, 0.0f);   // 薄い床 (上端 y=9.9)
    const u32 ball = w.AddCircle(FVec2{ 0.0f, 0.0f }, 0.3f, 1.0f, 0.0f);
    w.SetCcd(ball, true);
    w.SetVelocity(ball, FVec2{ 0.0f, 1500.0f });   // 高速下降 (1 step で 25 units)

    for (int i = 0; i < 30; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 10.0f });

    const f32 y = w.Position(ball).y;
    EXPECT_TRUE(y < 10.0f);                          // 床 (上端 9.9) をすり抜けていない
    EXPECT_TRUE(y > 9.0f);                            // 床の直上で止まった
}

// CCD: 低速な通常の落下では CCD が早すぎる停止を起こさず、従来どおり床上で静止する (非回帰)。
ACS_TEST(RigidWorld2D, CcdDoesNotBreakNormalRest) {
    CRigidWorld2D w;
    w.AddStaticAabb(FVec2{ 0.0f, 10.0f }, FVec2{ 10.0f, 0.5f }, 0.0f);   // 上端 y=9.5
    const u32 ball = w.AddCircle(FVec2{ 0.0f, 0.0f }, 0.5f, 1.0f, 0.0f);
    w.SetCcd(ball, true);                            // CCD 有効でも通常落下は変わらない

    for (int i = 0; i < 600; ++i) w.Step(1.0f / 60.0f, FVec2{ 0.0f, 10.0f });

    EXPECT_NEAR(w.Position(ball).y, 9.0f, 0.05f);   // 底面が床上端 9.5 に接して静止
    EXPECT_TRUE(std::fabs(w.Velocity(ball).y) < 0.3f);
}

// 空ワールド / 静的同士 / 範囲外アクセスが安全 (クラッシュしない・不動)。
ACS_TEST(RigidWorld2D, StaticAndEmptyAreSafe) {
    CRigidWorld2D w;
    w.Step(1.0f / 60.0f, FVec2{ 0.0f, 10.0f });   // 空ワールド

    w.AddStaticAabb(FVec2{ 0.0f,  0.0f }, FVec2{ 1.0f, 1.0f });
    w.AddStaticAabb(FVec2{ 0.5f,  0.0f }, FVec2{ 1.0f, 1.0f });   // 重なる静的 2 つ
    w.Step(1.0f / 60.0f, FVec2{ 0.0f, 10.0f });

    EXPECT_NEAR(w.Position(0).x, 0.0f, 1e-4f);    // 両方静的 → 動かない
    EXPECT_NEAR(w.Position(1).x, 0.5f, 1e-4f);
    EXPECT_EQ(w.Count(), 2u);
    EXPECT_TRUE(w.Body(99) == nullptr);           // 範囲外
}
