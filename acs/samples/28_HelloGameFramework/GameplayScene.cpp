// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — GameplayScene 実装。
#include "GameplayScene.h"
#include "PlayerProfile.h"
#include "RotateComponent.h"
#include "RotatingNode.h"
#include "TitleScene.h"
#include "PauseScene.h"

#include "platform/Input.h"
#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellogf {

void GameplayScene::OnEnter() noexcept {
    m_Color = kColorDark;
    // FTween を起動 (Services 側で PostUpdate に自動 tick される)。
    m_ColorTween = Services().Tweens().Tween(
        &m_Color, kColorDark, kColorBright,
        /*duration=*/2.0f, Easing::InOutSine);
    m_ToBright = true;
    GetGame().SetClearColor(m_Color.x, m_Color.y, m_Color.z);

    // FNode2D ツリー: root → wheel → spoke 2 個。
    auto wheel_up = MakeUnique<RotatingNode>(/*speed=*/1.0f /*rad/s*/, "wheel");
    wheel_up->Local().position = FVec2{10.0f, 0.0f};
    FNode2D& wheel = m_Root.AddChild(Move(wheel_up));
    m_Wheel = static_cast<RotatingNode*>(&wheel);

    auto sp0_up = MakeUnique<RotatingNode>(/*speed=*/0.0f, "spoke[0]");
    sp0_up->Local().position = FVec2{2.0f, 0.0f};
    m_Spoke[0] = static_cast<RotatingNode*>(&wheel.AddChild(Move(sp0_up)));

    auto sp1_up = MakeUnique<RotatingNode>(/*speed=*/0.0f, "spoke[1]");
    sp1_up->Local().position = FVec2{0.0f, 2.0f};
    m_Spoke[1] = static_cast<RotatingNode*>(&wheel.AddChild(Move(sp1_up)));

    // composition 版: プレーン FNode2D に RotateComponent を attach (継承版との対比)。
    auto rotator_up = MakeUnique<FNode2D>();
    rotator_up->Local().position = FVec2{-10.0f, 0.0f};
    FNode2D& rotator = m_Root.AddChild(Move(rotator_up));
    rotator.AddComponent<RotateComponent>(/*speed_rps=*/2.0f);
    m_Rotator = &rotator;

    // spoke を Circle として CollisionWorld に登録 (世界位置は OnUpdate で追従)。
    FCollisionWorld2D& phy = Services().Physics();
    for (u32 i = 0; i < 2; ++i) {
        const FVec2 sp = m_Spoke[i] ? m_Spoke[i]->World().position : FVec2{};
        m_SpokeShape[i] = phy.AddCircle(Circle{sp, /*radius=*/0.5f});
    }

    // y=-2 中心の幅広 AABB を静的 ground として登録。
    m_GroundShape = phy.AddAabb(Aabb2{FVec2{0.0f, -2.0f}, FVec2{20.0f, 0.5f}});

    // 落下 ball: FNode2D + FPhysicsBody2D。Component が phy を握って自前更新する。
    auto ball_up = MakeUnique<FNode2D>();
    ball_up->Local().position = FVec2{0.0f, 8.0f};
    FNode2D& ball_ref = m_Root.AddChild(Move(ball_up));
    FPhysicsBody2D& body = ball_ref.AddComponent<FPhysicsBody2D>(phy);
    body.SetCircle(0.5f);
    body.gravity = FVec2{0.0f, -10.0f};
    m_Ball = &ball_ref;

    FInputMap& im = Services().Input();
    im.ClearAll();
    im.BindKey(ActionId("Pause"),   EKey::P);
    im.BindKey(ActionId("Pause"),   EKey::Enter);
    im.BindKey(ActionId("Score"),   EKey::Space);
    im.BindKey(ActionId("ToTitle"), EKey::Backspace);
    im.BindKey(ActionId("Quit"),    EKey::Escape);
    im.BindAxisKeys(ActionId("MoveX"), EKey::A, EKey::D);

    auto* prof = GetGame().AppState<PlayerProfile>();
    if (prof) {
        ++prof->sessions;
        ACS_LOG_INFO("[Gameplay] session #%u start (Space: score+1, P: pause, Backspace: title, Esc: quit) [via Services]",
                     prof->sessions);
    }
}

void GameplayScene::OnExit() noexcept {
    // Services は scene 破棄時に完全破棄される。FTween に残った dangling pointer
    // 経由でクラッシュしないよう、先に CancelAll しておく。
    if (HasServices()) Services().Tweens().CancelAll();
    for (u32 i = 0; i < m_Root.ChildCount(); ++i) {
        if (auto* c = m_Root.Child(i)) c->Destroy();
    }
    m_Root.ResolveStructuralChanges();
    ACS_LOG_INFO("[Gameplay] exit");
}

void GameplayScene::OnPause() noexcept {
    // Push/Pop の間は services が tick されない (= Clock も自然に止まる) ので
    // 明示的 Pause 呼出は不要。意味付けのためログだけ出しておく。
    ACS_LOG_INFO("[Gameplay] paused (services auto-frozen via stack pause)");
}

void GameplayScene::OnResume() noexcept {
    ACS_LOG_INFO("[Gameplay] resumed");
}

void GameplayScene::OnUpdate(f32 dt) noexcept {
    // dt はフレームワーク側で Clock.Dt() (scaled) が渡される。
    // services の Tweens/Sequences は本関数の後、PostUpdate で自動 tick される。
    const FInputMap& im = Services().Input();
    if (im.IsPressed(ActionId("Quit"))) GetGame().Quit();
    if (im.IsPressed(ActionId("ToTitle"))) {
        Scenes().ChangeScene(MakeUnique<TitleScene>());
        return;
    }
    if (im.IsPressed(ActionId("Pause"))) {
        Scenes().PushScene(MakeUnique<PauseScene>());
        return;
    }
    if (im.IsPressed(ActionId("Score"))) {
        if (auto* prof = GetGame().AppState<PlayerProfile>()) {
            ++prof->hi_score;
            ACS_LOG_INFO("[Gameplay] score+1 → hi_score=%u", prof->hi_score);
        }
        // スコア取得イベントで trauma を蓄積 → camera が自然減衰する shake を出す。
        Services().Camera().AddShake(0.5f);
    }

    // spoke[0] の world 位置をカメラに毎フレーム渡してターゲット追従させる。
    if (m_Spoke[0] != nullptr) {
        Services().Camera().SetTargetPos(m_Spoke[0]->World().position);
    }

    // spoke 円の CollisionWorld 上の表現を world 位置で同期する。
    {
        FCollisionWorld2D& phy = Services().Physics();
        for (u32 i = 0; i < 2; ++i) {
            if (m_Spoke[i] != nullptr && m_SpokeShape[i].IsValid()) {
                const FVec2 p = m_Spoke[i]->World().position;
                phy.UpdateCircle(m_SpokeShape[i], Circle{p, 0.5f});
            }
        }
    }

    // FTween 完了で ping-pong を逆方向に張り直す (Tweens.Tick は PostUpdate で自動)。
    if (!Services().Tweens().IsActive(m_ColorTween)) {
        m_ToBright = !m_ToBright;
        const FVec3 from = m_Color;
        const FVec3 to   = m_ToBright ? kColorBright : kColorDark;
        m_ColorTween = Services().Tweens().Tween(&m_Color, from, to,
                                                  /*duration=*/2.0f, Easing::InOutSine);
    }
    GetGame().SetClearColor(m_Color.x, m_Color.y, m_Color.z);

    // FNode2D tree は手動 tick (Tweens/Sequences とは別系統)。
    m_Root.UpdateTree(dt);
    m_Root.ResolveStructuralChanges();
}

void GameplayScene::OnFixedUpdate(f32 dt) noexcept {
    // dt は固定 (FGame::SetFixedTimestep、既定 1/60)。60 step 毎 = 約 1 秒毎に
    // 状態をログ出力して OnFixedUpdate の呼出と transform 伝播を観察する。
    m_FixedSecs += dt;
    if (++m_FixedStepLogCounter >= 60) {
        m_FixedStepLogCounter = 0;
        const f32 move_x  = HasServices() ? Services().Input().Axis(ActionId("MoveX")) : 0.0f;
        const f32 sp_rot  = m_Spoke[0]    ? m_Spoke[0]->World().rotation    : 0.0f;
        const f32 rt_rot  = m_Rotator     ? m_Rotator->World().rotation     : 0.0f;
        const FVec2 cam_p  = HasServices() ? Services().Camera().Position()      : FVec2{};
        const f32  trauma = HasServices() ? Services().Camera().TraumaLevel()  : 0.0f;
        // 原点付近 4x4 の FAabb と overlap、+X 向き FRay で最近 hit までの t を取得。
        u32 overlap_count = 0;
        f32 ray_t = -1.0f;
        if (HasServices()) {
            TArray<FShapeId> hits;
            Services().Physics().OverlapAabb(Aabb2{FVec2{0,0}, FVec2{2.0f,2.0f}}, hits);
            overlap_count = static_cast<u32>(hits.Size());
            Ray2 r{FVec2{0,0}, FVec2{1,0}};
            RayHit2 rh{};
            FShapeId rid{};
            if (Services().Physics().Raycast(r, /*max_t=*/20.0f, rh, rid)) {
                ray_t = rh.t;
            }
        }
        const f32 ball_y  = m_Ball ? m_Ball->Local().position.y : 0.0f;
        const FPhysicsBody2D* body = m_Ball ? m_Ball->GetComponent<FPhysicsBody2D>() : nullptr;
        const f32 ball_vy = body ? body->velocity.y : 0.0f;
        ACS_LOG_INFO("[Gameplay] %.2fs  cam=(%.2f,%.2f) trauma=%.2f  overlap=%u rayT=%.2f  ball y=%.2f vy=%.2f",
                     static_cast<double>(m_FixedSecs),
                     static_cast<double>(cam_p.x),
                     static_cast<double>(cam_p.y),
                     static_cast<double>(trauma),
                     overlap_count,
                     static_cast<double>(ray_t),
                     static_cast<double>(ball_y),
                     static_cast<double>(ball_vy));
        (void)sp_rot; (void)rt_rot; (void)move_x;
    }
}

} // namespace hellogf
