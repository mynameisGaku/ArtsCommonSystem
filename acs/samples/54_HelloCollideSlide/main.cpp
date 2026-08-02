// SPDX-License-Identifier: Apache-2.0
// HelloCollideSlide — APhysicsBody2D の collide-and-slide を検証するコンソール。
// 重力で床に落ちる body が、垂直方向は床で止まりつつ水平方向は滑り続ける
// (= 衝突応答 + スライド) ことを assert する。ヘッドレス完結。
#include "gameframework/PhysicsBody2D.h"
#include "gameframework/ANode.h"
#include "gameframework/CollisionWorld2D.h"
#include "math/Collision2D.h"

#include <cstdio>
#include <cmath>

using namespace acs;
using namespace acs::game;

namespace { bool Near(f32 a, f32 b, f32 e) noexcept { return std::fabs(a - b) < e; } }

int main() {
    std::puts("=== ACS HelloCollideSlide ===");
    bool ok = true;

    // ---- (0) ResolveCircle 直接: 床にめり込んだ円は上に押し出される ----
    {
        CCollisionWorld2D w; w.Init(8.0f);
        w.AddAabb(FAabb2{ FVec2{0, 10}, FVec2{100, 10} });         // 上面 y=0 (床は y∈[0,20]=画面下)
        const FVec2 push = w.ResolveCircle(FCircle{ FVec2{0, -0.5f}, 1.0f }); // 中心 y=-0.5 → 0.5 めり込み
        const bool t0 = push.y < -0.3f && std::fabs(push.x) < 0.01f;
        std::printf("  ResolveCircle: push=(%.2f, %.2f) (expect ~0,-0.5)\n", push.x, push.y);
        ok = ok && t0;
    }

    // ---- (1) 円 body: 重力で落下 → 床で静止しつつ右へスライド ----
    {
        CCollisionWorld2D w; w.Init(8.0f);
        w.AddAabb(FAabb2{ FVec2{0, 10}, FVec2{100, 10} });         // 床、上面 y=0 (本体 y∈[0,20]=画面下)
        ANode node;
        node.SetPosition2D(FVec2{ 0, -5 });                   // 床より上 = 小さい Y
        auto& body = node.AddComponent<APhysicsBody2D>(w);
        body.SetCircle(1.0f);
        body.gravity  = FVec2{ 0, 20 };                           // +Y = 画面下へ落下
        body.velocity = FVec2{ 3, 0 };                            // 右へ
        for (int i = 0; i < 90; ++i) node.UpdateTree(1.0f / 60.0f);
        const FVec2 p = node.Position2D();
        const bool rest  = Near(p.y, -1.0f, 0.25f);               // 床上 (中心 = top - r、画面上側)
        const bool slid  = p.x > 3.0f;                            // 水平に滑った
        const bool nopen = p.y < -0.7f;                           // 貫通なし (床上面 0 より上)
        std::printf("  circle: pos=(%.2f, %.2f) vel=(%.2f, %.2f)  rest=%d slid=%d\n",
                    p.x, p.y, body.velocity.x, body.velocity.y, rest ? 1 : 0, slid ? 1 : 0);
        ok = ok && rest && slid && nopen && Near(body.velocity.y, 0.0f, 0.5f) && body.velocity.x > 2.5f;
    }

    // ---- (2) ポリゴン body も同様にスライド ----
    {
        CCollisionWorld2D w; w.Init(8.0f);
        w.AddAabb(FAabb2{ FVec2{0, 10}, FVec2{100, 10} });        // 床は y∈[0,20]=画面下
        ANode node;
        node.SetPosition2D(FVec2{ 0, -5 });                  // 床より上 = 小さい Y
        auto& body = node.AddComponent<APhysicsBody2D>(w);
        FConvexPoly2 local;                                        // 2x2 ボックス (中心原点)
        local.Add(FVec2{ -1, -1 }); local.Add(FVec2{ 1, -1 });
        local.Add(FVec2{ 1, 1 });   local.Add(FVec2{ -1, 1 });
        body.SetPolygon(local);
        body.gravity  = FVec2{ 0, 20 };                          // +Y = 画面下へ落下
        body.velocity = FVec2{ 4, 0 };
        for (int i = 0; i < 90; ++i) node.UpdateTree(1.0f / 60.0f);
        const FVec2 p = node.Position2D();
        const bool rest  = Near(p.y, -1.0f, 0.3f);                // box 落下側の辺 = 0 → 中心 y=-1
        const bool slid  = p.x > 3.0f;
        std::printf("  poly  : pos=(%.2f, %.2f) vel=(%.2f, %.2f)  rest=%d slid=%d\n",
                    p.x, p.y, body.velocity.x, body.velocity.y, rest ? 1 : 0, slid ? 1 : 0);
        ok = ok && rest && slid && p.y < -0.6f;
    }

    std::printf("\nresult: %s\n", ok ? "ALL PASS" : "FAILED");
    return ok ? 0 : 1;
}
