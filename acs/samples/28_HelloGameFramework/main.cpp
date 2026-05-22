// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — GameFramework Pillar A (Phase 1+2) + Pillar C (Phase 1+2) のデモ
//
// 動作 (3 シーン):
//   Title (青、FSM で 2s 毎に明滅) ─ Space ─→ Gameplay (緑、Tween で呼吸)
//                                      ←──── Backspace
//   Gameplay ─ P ─→ Pause (薄灰、Sequence でログを定期出力) ─ P ─→ Gameplay
//   Esc でいつでも終了
//
// Phase 2 確認:
//   ・**AppState** (PlayerProfile): セッション越しの hi_score。Gameplay で
//     Space を押すたびに +1、Title に戻ってもリセットされない。
//   ・**ChangeScene / PushScene / PopScene** の 3 種遷移を 1 サンプルで網羅。
//   ・**OnPause / OnResume**: Pause を Push すると Gameplay の OnPause がログ、
//     Pop で OnResume がログ。
//   ・**OnFixedUpdate** (1/60s): Gameplay で経過時間を _clock.Time() に累積して
//     コンソールに `fixed: X.XX` と出力 (variable rate と同期しない一定刻み)。
//   ・**3 frame GPU 遅延削除**: 退場した Scene が 3 フレーム保持される (内部、
//     直接見えないが Pop 直後にユーザーがリソースを触っても GPU が安全)。
//
// Phase 3 (Pillar C Phase 1) 確認:
//   ・**SceneClock**: Gameplay は `_clock.Tick(dt)` で時間管理。OnPause で
//     `_clock.Pause()` → tween が止まる、OnResume で再開。
//   ・**TweenManager + Easing**: 背景色 Vec3 を 2 色の間を `Easing::InOutSine`
//     で ping-pong (完了したら逆向きを再開)。Pause 中は dt=0 で凍結。
//
// Phase 4 (Pillar C Phase 2) 確認:
//   ・**StateMachine&lt;Owner&gt;**: Title が `Idle ⇄ Blink` の 2 状態 FSM で 2s 毎に
//     0.3s 明るく光る (clear color 切替)。状態関数は静的、Owner& 経由で self 参照。
//   ・**Sequence + SequenceRunner**: Pause で `Wait→Call→Wait→Call` を Loop(0)
//     (= 無限) で実行 → コンソールに 0.5s/1.5s 周期で「still paused...」が出続ける。
//
// Phase 5 (Pillar B Phase 1) 確認:
//   ・**Transform2D + Node2D ツリー**: Gameplay に root → wheel (1rps 回転) →
//     spoke[0/1] (オフセット位置) の 3 階層ツリーを構築。FixedUpdate 1 秒毎に
//     spoke[0] の world transform 位置を出力して、親回転が子位置に正しく伝播
//     していることを確認。AddChild 即時 spawn / Destroy 遅延 reap も実装済
//
// Phase 6 (Pillar D Phase 1) 確認:
//   ・**InputMap**: Gameplay でアクション名 (`"Pause"`, `"Score"`, `"ToTitle"`,
//     `"Quit"`, `"MoveX"`) をキーに割り当て。`Pause` は P と Enter の 2 binding
//     (OR 動作)。`MoveX` は A/D の 1D axis (-1 / 0 / +1)。1 秒毎の log で `MoveX`
//     の現在値も出力される
//
// Phase 7 (Pillar B Phase 2) 確認:
//   ・**Component2D**: 自作 `RotateComponent` (角速度を Owner().Local().rotation
//     に毎フレーム加算) を、**プレーンな Node2D** に attach。RotatingNode の
//     継承パターンに対し、composition パターンの対比デモ。GetComponent&lt;T&gt;()
//     で取得して値変更も可能。1 秒毎ログに rotator (composition 経由) の
//     world rotation を出力
//
// Phase 8 (Pillar A v3 §3.1 SceneServices) 確認:
//   ・**SceneServices ハブ**: GameplayScene が `WantedServices()` で
//     `Svc::Default2D` (Clock|Tweens|Sequences|Input) を宣言。フレームワークが
//     SceneServices を構築 → attach → 自動 tick (PreUpdate Clock、scene
//     OnUpdate、PostUpdate Tweens/Sequences の 2 phase)。シーン側は
//     `Services().Tweens()` 等で取得、`_clock.Tick(dt)` 系の手動 tick 不要に。
//     Title/Pause は inline 直接保持 (旧パターン) を維持し、両方の使い方を対比。
//
// Phase 9 (Pillar E Phase 1 = Camera2D) 確認:
//   ・**Camera2D**: GameplayScene の WantedServices に `Svc::Camera2D` 追加。
//     spoke[0] の world 位置を毎フレーム SetTargetPos して **追従**、Score
//     アクション (Space) で `AddShake(0.5)` で **画面振動** (trauma 方式)。
//     1 秒毎ログにカメラ位置と trauma 値を出力 → 値変化で追従/減衰を観察可能。
//
// Phase 10 (Pillar F Phase 1 = CollisionWorld2D) 確認:
//   ・**CollisionWorld2D + SpatialGrid**: GameplayScene の WantedServices に
//     `Svc::Physics2D` 追加。spoke[0/1] の世界位置に Circle を登録、毎フレーム
//     `UpdateCircle` で位置追従。原点中心の Aabb と `OverlapAabb` で重なり
//     検出、原点向きの Ray で `Raycast` の最近接 hit を 1s 毎ログ。
//
// Phase 11 (Pillar F Phase 2 = PhysicsBody2D) 確認:
//   ・**PhysicsBody2D component**: 落下する ball (Node2D + PhysicsBody2D) を
//     gravity=(0,-10) で初期位置 y=8 に配置、ground (静的 Aabb shape) に向か
//     って自由落下。軸独立衝突で Y velocity が地面接触で 0 になり ball が
//     乗る。1s 毎ログに ball.y と velocity.y を出力。

#include "gameframework/GameFramework.h"
#include "platform/Input.h"
#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

// シーン跨ぎ永続状態 (AppState のサンプル)
struct PlayerProfile {
    u32 hi_score = 0;
    u32 sessions  = 0;
};

// Phase 7: 同じ「回転させる」振る舞いを **コンポーネント** で実装したもの。
// 継承 (RotatingNode) では Node2D を Subclass 化する必要があったが、
// Component2D で書けばプレーン Node2D の attach 経由で同じ機能が手に入る。
class RotateComponent : public Component2D {
public:
    ACS_GAME_COMPONENT_KIND(RotateComponent)
    explicit RotateComponent(f32 speed_rps) noexcept : _speed(speed_rps) {}

    void OnAttach(Node2D& /*owner*/) noexcept override {
        ACS_LOG_INFO("[Component] RotateComponent attached (speed=%.2f rad/s)",
                     static_cast<double>(_speed));
    }
    void OnUpdate(f32 dt) noexcept override {
        Owner().Local().rotation += _speed * dt;
    }
    void OnDetach() noexcept override {
        ACS_LOG_INFO("[Component] RotateComponent detached");
    }

private:
    f32 _speed;
};

// Phase 5: 回転する Node2D サブクラス (Local().rotation を毎フレーム加算)。
// OnSpawn でログ、OnDespawn でログを出して lifecycle を観察できる。
class RotatingNode : public Node2D {
public:
    explicit RotatingNode(f32 speed_rps, const char* label) noexcept
        : _speed(speed_rps), _label(label) {}

    void OnSpawn() noexcept override {
        const auto w = World();
        ACS_LOG_INFO("[Node] %s spawned at world (%.2f, %.2f)",
                     _label, static_cast<double>(w.position.x),
                     static_cast<double>(w.position.y));
    }
    void OnUpdate(f32 dt) noexcept override {
        Local().rotation += _speed * dt;
    }
    void OnDespawn() noexcept override {
        ACS_LOG_INFO("[Node] %s despawn", _label);
    }

private:
    f32         _speed;
    const char* _label;
};

class TitleScene    : public Scene {
public:
    enum States : u32 { Idle = 0, Blink };

    void OnEnter()        noexcept override;
    void OnExit()         noexcept override;
    void OnUpdate(f32 dt) noexcept override;

    // FSM 状態関数 (静的、Owner& 経由で self 参照)
    static void EnterIdle (TitleScene& s)         noexcept;
    static void UpdateIdle(TitleScene& s, f32 dt) noexcept;
    static void EnterBlink(TitleScene& s)         noexcept;
    static void UpdateBlink(TitleScene& s, f32 dt) noexcept;

private:
    StateMachine<TitleScene> _fsm;
    SceneClock _clock;
    f32        _state_secs = 0.0f;
};

class GameplayScene : public Scene {
public:
    // Phase 8+9+10: services 宣言。Default2D + Camera2D + Physics2D。
    Svc WantedServices() const noexcept override {
        return Svc::Default2D | Svc::Camera2D | Svc::Physics2D;
    }

    void OnEnter()                noexcept override;
    void OnExit()                 noexcept override;
    void OnPause()                noexcept override;
    void OnResume()               noexcept override;
    void OnUpdate(f32 dt)         noexcept override;
    void OnFixedUpdate(f32 dt)    noexcept override;

private:
    // Tween/Clock/Sequences/Input は Services() 経由 (member 不要)
    Vec3          _color {0.05f, 0.20f, 0.10f};
    TweenHandle   _color_tween;
    bool          _to_bright = true;
    f32           _fixed_secs = 0.0f;
    u32           _fixed_step_log_counter = 0;

    // Phase 5: Node2D ツリー (root → wheel → spoke[0/1])
    Node2D        _root;
    RotatingNode* _wheel    = nullptr;
    RotatingNode* _spoke[2] = {nullptr, nullptr};
    // Phase 7: composition 版 (Node2D + RotateComponent)
    Node2D*       _rotator  = nullptr;
    // Phase 10: CollisionWorld2D shape handles (spoke[0/1] の world pos に追従する円)
    ShapeId       _spoke_shape[2];
    // Phase 11: 落下 ball + 床
    Node2D*       _ball       = nullptr;        // 弱参照
    ShapeId       _ground_shape;

    static constexpr Vec3 kColorDark  {0.05f, 0.20f, 0.10f};
    static constexpr Vec3 kColorBright{0.10f, 0.32f, 0.18f};
};

class PauseScene : public Scene {
public:
    void OnEnter()        noexcept override;
    void OnExit()         noexcept override;
    void OnUpdate(f32 dt) noexcept override;

    // Sequence Call action 用 (関数ポインタ)
    static void LogStillPaused1(void* user) noexcept;
    static void LogStillPaused2(void* user) noexcept;

private:
    SequenceRunner _seqs;
    SceneClock     _clock;
};

// --- Title ---
void TitleScene::OnEnter() noexcept {
    _fsm.Configure(Idle,  { &TitleScene::EnterIdle,  &TitleScene::UpdateIdle,  nullptr });
    _fsm.Configure(Blink, { &TitleScene::EnterBlink, &TitleScene::UpdateBlink, nullptr });
    _fsm.Start(Idle, *this);
    auto* prof = GetGame().AppState<PlayerProfile>();
    if (prof) {
        ACS_LOG_INFO("[Title] hi_score=%u sessions=%u  (Space: start, Esc: quit) FSM Idle/Blink",
                     prof->hi_score, prof->sessions);
    } else {
        ACS_LOG_INFO("[Title] (Space: start, Esc: quit) FSM Idle/Blink");
    }
}
void TitleScene::OnExit() noexcept { ACS_LOG_INFO("[Title] exit"); }
void TitleScene::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(Key::Escape)) GetGame().Quit();
    if (Input::IsKeyPressed(Key::Space)) {
        Scenes().ChangeScene(MakeUnique<GameplayScene>());
        return;
    }
    _clock.Tick(dt);
    _fsm.Update(*this, _clock.Dt());
}
// FSM 状態 (= Phase 4 デモ): 2 秒間 Idle (dark blue) → 0.3 秒 Blink (明るい青) を繰返
void TitleScene::EnterIdle(TitleScene& s) noexcept {
    s.GetGame().SetClearColor(0.10f, 0.12f, 0.25f);
    s._state_secs = 0.0f;
}
void TitleScene::UpdateIdle(TitleScene& s, f32 dt) noexcept {
    s._state_secs += dt;
    if (s._state_secs > 2.0f) s._fsm.ChangeState(Blink, s);
}
void TitleScene::EnterBlink(TitleScene& s) noexcept {
    s.GetGame().SetClearColor(0.20f, 0.25f, 0.50f);
    s._state_secs = 0.0f;
}
void TitleScene::UpdateBlink(TitleScene& s, f32 dt) noexcept {
    s._state_secs += dt;
    if (s._state_secs > 0.3f) s._fsm.ChangeState(Idle, s);
}

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
    im.BindKey(ActionId("Pause"),   Key::P);
    im.BindKey(ActionId("Pause"),   Key::Enter);
    im.BindKey(ActionId("Score"),   Key::Space);
    im.BindKey(ActionId("ToTitle"), Key::Backspace);
    im.BindKey(ActionId("Quit"),    Key::Escape);
    im.BindAxisKeys(ActionId("MoveX"), Key::A, Key::D);

    auto* prof = GetGame().AppState<PlayerProfile>();
    if (prof) {
        ++prof->sessions;
        ACS_LOG_INFO("[Gameplay] session #%u start (Space: score+1, P: pause, Backspace: title, Esc: quit) [via Services]",
                     prof->sessions);
    }
}
void GameplayScene::OnExit()   noexcept {
    // Services 経由なので CancelAll が必要 (Services は scene 死後も完全に破棄される)
    if (HasServices()) Services().Tweens().CancelAll();
    for (u32 i = 0; i < _root.ChildCount(); ++i) {
        if (auto* c = _root.Child(i)) c->Destroy();
    }
    _root.ResolveStructuralChanges();
    ACS_LOG_INFO("[Gameplay] exit");
}
void GameplayScene::OnPause()  noexcept {
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

// --- Pause ---
void PauseScene::OnEnter() noexcept {
    GetGame().SetClearColor(0.18f, 0.18f, 0.20f);   // grey
    // Phase 4 Sequence デモ: Wait → Call → Wait → Call を Loop(0)=無限。
    // Pause を抜けるまでコンソールに「still paused...」が周期的に出力される。
    Sequence s;
    s.Wait(0.5f)
     .Call(&PauseScene::LogStillPaused1, this)
     .Wait(1.5f)
     .Call(&PauseScene::LogStillPaused2, this)
     .Loop(0);
    _seqs.Start(Move(s));
    ACS_LOG_INFO("[Pause] (P: resume, Esc: quit) Sequence loop start");
}
void PauseScene::OnExit() noexcept {
    _seqs.CancelAll();
    ACS_LOG_INFO("[Pause] exit");
}
void PauseScene::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(Key::Escape)) GetGame().Quit();
    if (Input::IsKeyPressed(Key::P)) {
        Scenes().PopScene();
        return;
    }
    _clock.Tick(dt);
    _seqs.Tick(_clock.Dt());
}
void PauseScene::LogStillPaused1(void* /*user*/) noexcept {
    ACS_LOG_INFO("[Pause] still paused...");
}
void PauseScene::LogStillPaused2(void* /*user*/) noexcept {
    ACS_LOG_INFO("[Pause] still paused longer...");
}

// --- Game ---
class MyGame : public Game {
public:
    void OnStart() noexcept override {
        // Phase 2: AppState を構築 (Title/Gameplay/Pause 全てから見える)。
        EmplaceAppState<PlayerProfile>();
        // Phase 2: 固定 step を明示 (既定値と同じだが API デモ目的で明示)。
        SetFixedTimestep(1.0f / 60.0f, /*max_steps_per_frame=*/8);
        // 基底の OnStart は InitialScene() を push する
        Game::OnStart();
    }

protected:
    UniquePtr<Scene> InitialScene() noexcept override {
        return MakeUnique<TitleScene>();
    }
};

ACS_GAME_MAIN(MyGame)
