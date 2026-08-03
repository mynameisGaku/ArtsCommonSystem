// SPDX-License-Identifier: Apache-2.0
// HelloTriggers - Package B 検証コンソール: 衝突レイヤ/マスク + トリガ enter/exit。
//
// 1) CCollisionWorld2D のレイヤ/マスク絞り込み (player/pickup/wall) を assert。
// 2) ATriggerComponent をノードに付け、world.Tick 経由で OnEnter/OnExit が
//    マスクに従って正しく発火することを assert。GPU 不要・ヘッドレスで完結。
#include "gameframework/GameFramework.h"
#include "container/Array.h"

#include <cstdio>

using namespace acs;
using namespace acs::game;

namespace {

int g_fail = 0;
void Check(bool cond, const char* what) noexcept {
    std::printf("  [%s] %s\n", cond ? "OK" : "FAIL", what);
    if (!cond) g_fail = 1;
}

constexpr u32 kWall   = 1u << 0;
constexpr u32 kPickup = 1u << 1;
constexpr u32 kEnemy  = 1u << 2;
constexpr u32 kPlayer = 1u << 3;

// トリガ enter/exit 回数を数える user 構造体。
struct FCounter { int enter = 0; int exit = 0; };

void OnEnterCb(ATriggerComponent& /*self*/, ATriggerComponent* /*other*/, void* user) noexcept {
    static_cast<FCounter*>(user)->enter++;
}
void OnExitCb(ATriggerComponent& /*self*/, ATriggerComponent* /*other*/, void* user) noexcept {
    static_cast<FCounter*>(user)->exit++;
}

} // namespace

int main() {
    std::printf("=== ACS HelloTriggers ===\n");

    // ---- 1) CollisionWorld2D layer / mask ----
    {
        CCollisionWorld2D world;
        world.Init(1.0f);
        world.AddAabb(FAabb2{FVec2{0, 0}, FVec2{1, 1}}, kWall);
        world.AddCircle(FCircle{FVec2{0, 0}, 0.5f}, kPickup);

        TArray<FShapeId> hits;
        world.OverlapCircle(FCircle{FVec2{0, 0}, 3.0f}, hits, {}, kWall);
        Check(hits.Num() == 1, "mask=Wall -> 1 hit");
        world.OverlapCircle(FCircle{FVec2{0, 0}, 3.0f}, hits, {}, kPickup);
        Check(hits.Num() == 1, "mask=Pickup -> 1 hit");
        world.OverlapCircle(FCircle{FVec2{0, 0}, 3.0f}, hits, {}, kWall | kPickup);
        Check(hits.Num() == 2, "mask=Wall|Pickup -> 2 hits");
        world.OverlapCircle(FCircle{FVec2{0, 0}, 3.0f}, hits, {}, kEnemy);
        Check(hits.Num() == 0, "mask=Enemy -> 0 hits");
        // default mask = all → both
        world.OverlapCircle(FCircle{FVec2{0, 0}, 3.0f}, hits);
        Check(hits.Num() == 2, "mask=default(all) -> 2 hits");
    }

    // ---- 2) node tree + world.Tick 経由の ATriggerComponent enter/exit ----
    {
        CTriggerWorld2D tw;
        tw.Init();
        ANode root;

        FCounter playerC, pickupC, enemyC;

        // player: layer=Player, mask=Pickup (pickups にのみ反応)
        auto pnode = NewObject<ANode>();
        pnode->SetPosition2D(FVec2{0.0f, 0.0f});
        auto& ptrig = pnode->AddComponent<ATriggerComponent>(tw, kPlayer, kPickup);
        ptrig.SetCircle(0.5f);
        ptrig.SetOnEnter(&OnEnterCb, &playerC);
        ptrig.SetOnExit(&OnExitCb, &playerC);
        ANode* player = &root.AddChild(Move(pnode));

        // pickup: layer=Pickup, mask=Player
        auto knode = NewObject<ANode>();
        knode->SetPosition2D(FVec2{0.3f, 0.0f});   // player と重なる
        auto& ktrig = knode->AddComponent<ATriggerComponent>(tw, kPickup, kPlayer);
        ktrig.SetCircle(0.5f);
        ktrig.SetOnEnter(&OnEnterCb, &pickupC);
        ktrig.SetOnExit(&OnExitCb, &pickupC);
        root.AddChild(Move(knode));

        // enemy: layer=Enemy, mask=0 (何にも反応しない)。player の mask にも含まれない。
        auto enode = NewObject<ANode>();
        enode->SetPosition2D(FVec2{0.2f, 0.0f});    // player と重なる
        auto& etrig = enode->AddComponent<ATriggerComponent>(tw, kEnemy, 0u);
        etrig.SetCircle(0.5f);
        etrig.SetOnEnter(&OnEnterCb, &enemyC);
        etrig.SetOnExit(&OnExitCb, &enemyC);
        root.AddChild(Move(enode));

        // frame 1: 登録 + 形状同期 → overlap 計算 → ENTER
        root.UpdateTree(1.0f / 60.0f);
        tw.Tick(1.0f / 60.0f);
        Check(playerC.enter == 1, "player entered pickup (mask hit)");
        Check(pickupC.enter == 1, "pickup entered player (mask hit)");
        // player は enemy と幾何 overlap するが mask=Pickup のため反応しない。
        // enemy は mask=0 のため何にも反応しない。
        Check(enemyC.enter == 0, "enemy mask=0 -> no enter");
        // player が enemy(layer=Enemy) に反応しないことは player.enter==1 (pickup の
        // 1 回のみ) で確認済み。

        // player を遠くへ動かす → EXIT
        player->SetPosition2D(FVec2{10.0f, 10.0f});
        root.UpdateTree(1.0f / 60.0f);
        tw.Tick(1.0f / 60.0f);
        Check(playerC.exit == 1, "player exited pickup");
        Check(pickupC.exit == 1, "pickup exited player");
    }

    // ---- 3) RequireComponent: 依存コンポーネントの自動追加 ----
    {
        // ASpriteAnimComponent は ASprite2DComponent を要求する。sprite が無い
        // ノードに anim を足すと、OnRequire 経由で sprite が自動追加されるはず。
        ANode node;
        Check(!node.HasComponent<ASprite2DComponent>(), "initially no sprite");
        node.AddComponent<ASpriteAnimComponent>();
        Check(node.HasComponent<ASprite2DComponent>(),
              "RequireComponent auto-added ASprite2DComponent");
        Check(node.HasComponent<ASpriteAnimComponent>(), "anim component present");

        // 既に sprite があれば重複追加せず既存を再利用する。
        ANode node2;
        auto& spr = node2.AddComponent<ASprite2DComponent>(FVec2{1.0f, 1.0f});
        node2.AddComponent<ASpriteAnimComponent>();
        Check(node2.GetComponent<ASprite2DComponent>() == &spr,
              "existing sprite reused (no replacement)");
    }

    std::printf("result: %s\n", g_fail == 0 ? "ALL PASS" : "FAIL");
    return g_fail;
}
