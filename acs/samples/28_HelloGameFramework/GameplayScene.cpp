// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — GameplayScene 実装。
#include "GameplayScene.h"
#include "GameTypes.h"
#include "TitleScene.h"
#include "PauseScene.h"

#include "platform/Input.h"
#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellogf {

// --- Gameplay (Phase 8: Services() 経由) ---
void GameplayScene::OnEnter() noexcept {
    _color = kColorDark;
    // Phase 8: Tween 開始 (Services() のフレームワーク自動 tick で進行)
    _color_tween = Services().Tweens().Tween(
        &_color, kColorDark, kColorBright,
        /*duration=*/2.0f, Easing::InOutSine);
    _to_bright = true;
    GetGame().SetClearColor(_color.x, _color.y, _color.z);

    // Phase 5: Node2D ツリー組立 (root → wheel → spoke 2 個)
    auto wheel_up = MakeUnique<RotatingNode>(/*speed=*/1.0f /*rad/s*/, "wheel");
    wheel_up->Local().position = Vec2{10.0f, 0.0f};
    Node2D& wheel = _root.AddChild(Move(wheel_up));
    _wheel = static_cast<RotatingNode*>(&wheel);

    auto sp0_up = MakeUnique<RotatingNode>(/*speed=*/0.0f, "spoke[0]");
    sp0_up->Local().position = Vec2{2.0f, 0.0f};
    _spoke[0] = static_cast<RotatingNode*>(&wheel.AddChild(Move(sp0_up)));

    auto sp1_up = MakeUnique<RotatingNode>(/*speed=*/0.0f, "spoke[1]");
    sp1_up->Local().position = Vec2{0.0f, 2.0f};
    _spoke[1] = static_cast<RotatingNode*>(&wheel.AddChild(Move(sp1_up)));

    // Phase 7: composition 版 (プレーン Node2D + RotateComponent)
    auto rotator_up = MakeUnique<Node2D>();
    rotator_up->Local().position = Vec2{-10.0f, 0.0f};
    Node2D& rotator = _root.AddChild(Move(rotator_up));
    rotator.AddComponent<RotateComponent>(/*speed_rps=*/2.0f);
    _rotator = &rotator;

    // Phase 10: CollisionWorld2D に spoke を Circle として登録 (初期位置)
    CollisionWorld2D& phy = Services().Physics();
    for (u32 i = 0; i < 2; ++i) {
        const Vec2 sp = _spoke[i] ? _spoke[i]->World().position : Vec2{};
        _spoke_shape[i] = phy.AddCircle(Circle{sp, /*radius=*/0.5f});
    }

    // Phase 11: 静的 ground (y=-2 を中心とする幅広 AABB)
    _ground_shape = phy.AddAabb(Aabb2{Vec2{0.0f, -2.0f}, Vec2{20.0f, 0.5f}});

    // Phase 11: 落下 ball (Node2D + PhysicsBody2D)
    auto ball_up = MakeUnique<Node2D>();
    ball_up->Local().position = Vec2{0.0f, 8.0f};
    Node2D& ball_ref = _root.AddChild(Move(ball_up));
    PhysicsBody2D& body = ball_ref.AddComponent<PhysicsBody2D>(phy);
    body.SetCircle(0.5f);
    body.gravity = Vec2{0.0f, -10.0f};
    _ball = &ball_ref;

    // Phase 8: InputMap も Services 経由
    InputMap& im = Services().Input();
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
    // Services 経由なので CancelAll が必要 (Services は scene 死後も完全に破棄される)
    if (HasServices()) Services().Tweens().CancelAll();
    for (u32 i = 0; i < _root.ChildCount(); ++i) {
        if (auto* c = _root.Child(i)) c->Destroy();
    }
    _root.ResolveStructuralChanges();
    ACS_LOG_INFO("[Gameplay] exit");
}

void GameplayScene::OnPause() noexcept {
    // Phase 8: Push/Pop で services が tick されないので Clock も自然停止する
    // (= 明示的 Pause 呼出は不要。意味付けのためログだけ出す)
    ACS_LOG_INFO("[Gameplay] paused (services auto-frozen via stack pause)");
}

void GameplayScene::OnResume() noexcept {
    ACS_LOG_INFO("[Gameplay] resumed");
}

void GameplayScene::OnUpdate(f32 dt) noexcept {
    // Phase 8: dt はフレームワークが既に Clock.Dt() (scaled) を渡している。
    // services の Tweens/Sequences は OnUpdate **の後** に PostUpdate で tick される。
    const InputMap& im = Services().Input();
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
        // Phase 9: スコア取得で screen shake (trauma 累積、自然減衰する)
        Services().Camera().AddShake(0.5f);
    }

    // Phase 9: spoke[0] が回転する世界位置をカメラに毎フレーム渡す → 追従
    if (_spoke[0] != nullptr) {
        Services().Camera().SetTargetPos(_spoke[0]->World().position);
    }

    // Phase 10: spoke の world 位置で CollisionWorld の Circle を更新
    {
        CollisionWorld2D& phy = Services().Physics();
        for (u32 i = 0; i < 2; ++i) {
            if (_spoke[i] != nullptr && _spoke_shape[i].IsValid()) {
                const Vec2 p = _spoke[i]->World().position;
                phy.UpdateCircle(_spoke_shape[i], Circle{p, 0.5f});
            }
        }
    }

    // Tween 完了で逆向き ping-pong を再開 (Tweens.Tick は PostUpdate で自動実行)
    if (!Services().Tweens().IsActive(_color_tween)) {
        _to_bright = !_to_bright;
        const Vec3 from = _color;
        const Vec3 to   = _to_bright ? kColorBright : kColorDark;
        _color_tween = Services().Tweens().Tween(&_color, from, to,
                                                  /*duration=*/2.0f, Easing::InOutSine);
    }
    GetGame().SetClearColor(_color.x, _color.y, _color.z);

    // Node2D tree update (Tween/Sequences とは別系統、こちらは手動 tick)
    _root.UpdateTree(dt);
    _root.ResolveStructuralChanges();
}

void GameplayScene::OnFixedUpdate(f32 dt) noexcept {
    // dt は固定 (= Game::SetFixedTimestep の値、既定 1/60)。
    _fixed_secs += dt;
    // 60 step に 1 回 ログ (= 1 秒に 1 回) で OnFixedUpdate が呼ばれていることを確認可能。
    // 合わせて Phase 5 Node2D の transform 伝播も観察できるよう spoke[0] の
    // world 位置を出力 (親 wheel が回転しているので X/Y がぐるぐる変化する)。
    if (++_fixed_step_log_counter >= 60) {
        _fixed_step_log_counter = 0;
        const f32 move_x  = HasServices() ? Services().Input().Axis(ActionId("MoveX")) : 0.0f;
        const f32 sp_rot  = _spoke[0]    ? _spoke[0]->World().rotation    : 0.0f;
        const f32 rt_rot  = _rotator     ? _rotator->World().rotation     : 0.0f;
        const Vec2 cam_p  = HasServices() ? Services().Camera().Position()      : Vec2{};
        const f32  trauma = HasServices() ? Services().Camera().TraumaLevel()  : 0.0f;
        // Phase 10: 原点周りの 4x4 Aabb と overlap、原点向きの Raycast で最近 hit を取得
        u32 overlap_count = 0;
        f32 ray_t = -1.0f;
        if (HasServices()) {
            Array<ShapeId> hits;
            Services().Physics().OverlapAabb(Aabb2{Vec2{0,0}, Vec2{2.0f,2.0f}}, hits);
            overlap_count = static_cast<u32>(hits.Size());
            // 原点から +X 向きの Ray (最大 20)
            Ray2 r{Vec2{0,0}, Vec2{1,0}};
            RayHit2 rh{};
            ShapeId rid{};
            if (Services().Physics().Raycast(r, /*max_t=*/20.0f, rh, rid)) {
                ray_t = rh.t;
            }
        }
        const f32 ball_y  = _ball ? _ball->Local().position.y : 0.0f;
        const PhysicsBody2D* body = _ball ? _ball->GetComponent<PhysicsBody2D>() : nullptr;
        const f32 ball_vy = body ? body->velocity.y : 0.0f;
        ACS_LOG_INFO("[Gameplay] %.2fs  cam=(%.2f,%.2f) trauma=%.2f  overlap=%u rayT=%.2f  ball y=%.2f vy=%.2f",
                     static_cast<double>(_fixed_secs),
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
