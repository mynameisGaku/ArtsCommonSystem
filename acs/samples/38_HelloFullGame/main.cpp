// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — GameFramework Pillar A-X の dogfood 完結サンプル (Phase 25)
//
// 完結 mini top-down shoot-em-up。5 分くらいで遊べる規模で、Pillar 群が「実際の
// ゲームの中で噛み合うか」を 1 サンプルで網羅検証する。
//
// 構成:
//   Title   ── Space ─→  Gameplay  ── HP 0 or 5 wave clear ─→  GameOver
//                            ↑                                      │
//                            └───────────── R (Title へ) ←──────────┘
//   Esc でいつでも終了
//
// 操作:
//   WASD       : 移動
//   マウス     : 照準 (player → cursor 方向に弾を撃つ)
//   左クリック : 発射 (ProjectileSystem 経由)
//   Esc        : ゲーム終了 (GameOver で実行)
//   R          : GameOver / Title 戻し
//
// ゲームルール:
//   ・5 wave 撃破で VICTORY (= GameOver scene、画面に "VICTORY!" 表示)
//   ・プレイヤー HP=3、敵接触で -1 + 1.0s 無敵時間
//   ・敵 HP=2、弾 1 発で -1 ダメージ、HP 0 で despawn + score+10
//   ・各 wave で 5 体 + wave×2 体 (= 1 wave 7 体、5 wave 15 体まで)
//
// 使う ACS API:
//   ・Game / Scene / SceneManager (Pillar A)
//   ・Node2D + PhysicsBody2D + CollisionWorld2D (Pillar B/F)
//   ・InputMap (Pillar D)
//   ・Camera2D + AddShake (Pillar E)
//   ・HealthSystem + ScoreSystem + WaveSpawner + ProjectileSystem (Pillar I/L/R)
//   ・AudioDirector + MusicDirector (Pillar H、state-only)
//   ・ParticleEffectSystem + EffectSystem (Pillar I)
//   ・SaveSlot<HighScore> (Pillar J)
//   ・GameFlow (完成度 v7)
//   ・Tilemap (Pillar Q)
//   ・SceneServices::Default2D (Clock/Tweens/Input/Camera2D/Physics2D)
//
// 描画: SpriteBatch の単色矩形のみ。テクスチャ無しで以下のキャラクタを表現する:
//   player = 赤円 (実装は赤矩形回転)、 enemy = 青矩形、 弾 = 黄点矩形、
//   tile = 灰色矩形、 particle = 橙赤グラデ矩形、 HP/HUD = SpriteBatch.DrawRect

#include "gameframework/GameFramework.h"
#include "render/SpriteBatch.h"
#include "render/Renderer.h"
#include "render/IRhiSwapchain.h"
#include "render/IRhiCommandList.h"
#include "platform/Input.h"
#include "foundation/Log.h"
#include "math/Math.h"
#include "math/Vec.h"

using namespace acs;
using namespace acs::game;

// =============================================================================
// 永続データ (SaveSlot で .acssave に保存される POD)
// =============================================================================
struct HighScore {
    u64 best_score = 0;   // これまでで最高のスコア
    u64 timestamp  = 0;   // 達成時刻 (Clock::Ticks() の値)、参考表示用
};
static_assert(__is_trivially_copyable(HighScore),
              "HighScore must be trivially copyable for SaveSlot");

// =============================================================================
// 定数
// =============================================================================
namespace {
constexpr f32 kWorldUnit = 32.0f;   // 1 world unit = 32 px (Camera2D の zoom 用)

// プレイヤー
constexpr f32 kPlayerHp     = 3.0f;
constexpr f32 kPlayerSpeed  = 6.0f;     // world units / sec
constexpr f32 kPlayerRadius = 0.5f;
constexpr f32 kPlayerInvuln = 1.0f;     // 被弾後の無敵秒数
constexpr f32 kFireCooldown = 0.18f;    // 弾連射の最小間隔 (sec)

// 弾
constexpr f32 kBulletSpeed    = 16.0f;
constexpr f32 kBulletLifetime = 1.5f;
constexpr f32 kBulletRadius   = 0.15f;
constexpr f32 kBulletDamage   = 1.0f;

// 敵
constexpr f32 kEnemyHp      = 2.0f;
constexpr f32 kEnemySpeed   = 2.2f;
constexpr f32 kEnemyRadius  = 0.5f;
constexpr u32 kMaxEnemies   = 64;
constexpr u32 kMaxBullets   = 256;
constexpr u32 kMaxParticles = 1024;

// wave
constexpr u32 kTotalWaves          = 5;
constexpr f32 kIntermissionSec     = 2.0f;
constexpr f32 kSpawnIntervalSec    = 0.6f;

// world
constexpr f32 kWorldHalfW = 16.0f;   // [-16, +16]
constexpr f32 kWorldHalfH = 9.0f;    // [-9,  +9]

// 弾種 ID (文字列リテラルなので strcmp / アドレス比較で安全)
constexpr const char* kBulletDefId = "player_bullet";

// SaveSlot のファイル名 (相対パス wide string)。
const wchar_t kSaveFile[] = L"hello_full_game_highscore.acssave";

// HSL を rgb に変換せず、ベタの色定数だけ使う。
constexpr Vec4 kColorPlayer      {0.95f, 0.20f, 0.20f, 1.0f};
constexpr Vec4 kColorPlayerHurt  {1.0f,  0.6f,  0.6f,  1.0f};
constexpr Vec4 kColorEnemy       {0.30f, 0.45f, 0.95f, 1.0f};
constexpr Vec4 kColorBullet      {1.0f,  0.95f, 0.20f, 1.0f};
constexpr Vec4 kColorTileLight   {0.18f, 0.20f, 0.22f, 1.0f};
constexpr Vec4 kColorTileDark    {0.12f, 0.14f, 0.16f, 1.0f};

constexpr Vec3 kParticleStart{1.0f, 0.65f, 0.15f};
constexpr Vec3 kParticleEnd  {0.6f, 0.05f, 0.05f};
} // namespace

// =============================================================================
// 前方宣言
// =============================================================================
class FullGameApp;
class TitleScene;
class GameplayScene;
class GameOverScene;

// =============================================================================
// FullGameApp — Game 派生 (AudioDirector / MusicDirector / SaveSlot を持つ)
// =============================================================================
class FullGameApp : public Game {
public:
    void OnStart() noexcept override {
        // 1) BGM トラック登録 (state-only、backend 未接続なのでログのみ)
        _music.RegisterTrack(EMusicState::Calm,
            MusicTrack{ /*asset=*/"bgm_title.ogg",    0.0f, 1.0f, true });
        _music.RegisterTrack(EMusicState::Combat,
            MusicTrack{ /*asset=*/"bgm_gameplay.ogg", 0.0f, 1.0f, true });
        _music.RegisterTrack(EMusicState::GameOver,
            MusicTrack{ /*asset=*/"bgm_gameover.ogg", 0.0f, 1.0f, true });
        _music.RegisterTrack(EMusicState::Victory,
            MusicTrack{ /*asset=*/"bgm_victory.ogg",  0.0f, 1.0f, false });

        // 2) AudioDirector volume バス初期化 (backend 未接続でも state はもつ)
        _audio.SetMasterVolume(0.8f);
        _audio.SetBgmVolume(0.7f);
        _audio.SetSfxVolume(0.9f);

        // 3) SaveSlot 初期化 + 既存 HighScore のロード
        _highscore_slot.Init(kSaveFile);
        if (_highscore_slot.Exists()) {
            auto r = _highscore_slot.Load();
            if (r.IsOk()) {
                _highscore = r.Value();
                ACS_LOG_INFO("[FullGame] HighScore loaded: best=%llu",
                             static_cast<unsigned long long>(_highscore.best_score));
            } else {
                ACS_LOG_WARN("[FullGame] HighScore load failed: %s", r.Error().message);
            }
        } else {
            ACS_LOG_INFO("[FullGame] No HighScore file yet (first run)");
        }

        // 4) GameFlow を Splash → MainTitle (1 step) で起動
        _flow.Init(EFlowState::Splash);
        _flow.RequestTransition(EFlowState::MainTitle, 0.0f);

        // 5) 固定 step を明示 (PhysicsBody2D の決定性のため)
        SetFixedTimestep(1.0f / 60.0f, /*max_steps_per_frame=*/8);

        // 6) 基底に初期 Scene を push してもらう
        Game::OnStart();
    }

    void OnUpdate(f32 dt) noexcept override {
        // Game ループに乗せる前に AudioDirector / MusicDirector / GameFlow を tick。
        // (Pause の影響を受けず常に動かす — 例えば BGM クロスフェードはシーン
        //  切替で途切れさせたくない)
        _audio.Tick(dt);
        _music.Tick(dt);
        _flow.Tick(dt);
        Game::OnUpdate(dt);
    }

    void OnShutdown() noexcept override {
        if (_sprite_initialized) {
            if (auto* dev = GetRenderer().Device()) dev->WaitIdle();
            _sprites.Shutdown();
        }
        Game::OnShutdown();
    }

    // ----- 共通サービスへのアクセサ -----
    AudioDirector& Audio() noexcept { return _audio; }
    MusicDirector& Music() noexcept { return _music; }
    GameFlow&      Flow()  noexcept { return _flow; }
    SpriteBatch&   Sprites() noexcept { return _sprites; }
    bool           SpritesReady() const noexcept { return _sprite_initialized; }

    // SpriteBatch を初回 OnRender で 1 度だけ初期化。Init は Renderer がフレーム
    // を始めて Device が生きてからでないと安全に呼べないので、遅延 init とする。
    void EnsureSpritesInitialized() noexcept {
        if (_sprite_initialized) return;
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) return;
        auto r = _sprites.Init(*dev, GetRenderer().ColorFormat(), /*max_sprites=*/4096);
        if (r.IsErr()) {
            ACS_LOG_ERROR("[FullGame] SpriteBatch::Init failed: %s", r.Error().message);
            return;
        }
        _sprite_initialized = true;
        ACS_LOG_INFO("[FullGame] SpriteBatch initialized");
    }

    HighScore&         GetHighScore() noexcept { return _highscore; }
    SaveSlot<HighScore>& HighScoreSlot() noexcept { return _highscore_slot; }

    void SaveHighScoreIfBetter(u64 final_score) noexcept {
        if (final_score <= _highscore.best_score) return;
        _highscore.best_score = final_score;
        _highscore.timestamp  = static_cast<u64>(0);  // Clock 依存を避け 0 でも OK
        auto r = _highscore_slot.Save(_highscore);
        if (r.IsErr()) {
            ACS_LOG_WARN("[FullGame] HighScore save failed: %s", r.Error().message);
        } else {
            ACS_LOG_INFO("[FullGame] HighScore saved (best=%llu)",
                         static_cast<unsigned long long>(_highscore.best_score));
        }
    }

protected:
    UniquePtr<Scene> InitialScene() noexcept override;

private:
    AudioDirector       _audio;
    MusicDirector       _music;
    GameFlow            _flow;
    SaveSlot<HighScore> _highscore_slot;
    HighScore           _highscore{};
    SpriteBatch         _sprites;
    bool                _sprite_initialized = false;
};

// =============================================================================
// TitleScene — Press Space to Start。FSM 不使用 (= Phase 28 で実演済)、
// 代わりに Tween で背景色を呼吸させる。
// =============================================================================
class TitleScene : public Scene {
public:
    ESvc WantedServices() const noexcept override {
        return ESvc::Default2D;   // Clock | Tweens | Sequences | Input
    }

    void OnEnter() noexcept override;
    void OnExit()  noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnRender(RenderContext& rc) noexcept override;

private:
    Vec3        _bg_color  {0.06f, 0.08f, 0.16f};
    TweenHandle _bg_tween  {};
    bool        _to_bright = true;
    f32         _pulse_sec = 0.0f;   // "Press Space" の点滅位相

    static constexpr Vec3 kBgDark   {0.06f, 0.08f, 0.16f};
    static constexpr Vec3 kBgBright {0.16f, 0.20f, 0.35f};
};

// =============================================================================
// GameplayScene — 本編 (most logic)
// =============================================================================
class GameplayScene : public Scene {
public:
    ESvc WantedServices() const noexcept override {
        return ESvc::Default2D | ESvc::Camera2D | ESvc::Physics2D;
    }

    void OnEnter() noexcept override;
    void OnExit()  noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnFixedUpdate(f32 dt) noexcept override;
    void OnRender(RenderContext& rc) noexcept override;

    // 公開: GameOverScene が遷移時に最終 score / 勝敗を読むため
    u64  FinalScore() const noexcept { return _score.CurrentScore(); }
    bool DidWin()     const noexcept { return _victory; }

private:
    // ----- 敵を 1 体生成 -----
    void SpawnEnemy(Vec2 pos) noexcept;

    // ----- 弾を 1 発発射 -----
    void FireBullet(Vec2 from, Vec2 dir_unit) noexcept;

    // ----- 命中エフェクト (粒子 + 画面振動 + SE) -----
    void TriggerHitEffect(Vec2 pos) noexcept;

    // ----- WaveSpawner -> 自身へのブリッジ (C 関数ポインタ用) -----
    static void OnWaveSpawn(void* user, const char* enemy_id, Vec2 spawn_pos) noexcept;
    static void OnWaveState(void* user, u32 wave_index, EWaveState from, EWaveState to) noexcept;

    // ----- ProjectileSystem -> 自身へのブリッジ -----
    static bool OnHitTest (void* user, const ProjectileInstance& p,
                            u32& out_target, f32& out_dmg) noexcept;
    static void OnHit     (void* user, ProjectileId id, const char* def_id,
                            u32 target_id, f32 dmg) noexcept;

    // ----- 自身 (= GameOver への遷移) -----
    void RequestGameOver(bool victory) noexcept;

    // ----- ヘルパ -----
    Vec2 MouseWorld() const noexcept;

    // ----- World state -----
    Node2D                _root;
    Node2D*               _player_node = nullptr;
    HealthId              _player_health{};
    HealthSystem          _health;
    ScoreSystem           _score;
    WaveSpawner           _waves;
    ProjectileSystem      _bullets;
    ParticleEffectSystem  _particles;
    EffectSystem          _fx;
    EmitterHandle         _hit_emitter;
    Perception            _perception;        // (sight デモ用、1 NPC 分まとめて使う)
    Tilemap               _floor;

    // 敵 instance テーブル (固定容量、Pool 風 swap-and-pop)
    struct Enemy {
        Node2D*  node     = nullptr;     // _root の子。Destroy() で遅延 reap される
        HealthId hp{};                   // _health の entity
        ShapeId  shape{};                // CollisionWorld 上の Circle
        u32      wave_idx_at_spawn = 0;  // この敵が属する wave (= 撃破通知時 hint)
        bool     alive = false;
    };
    Enemy _enemies[kMaxEnemies] {};

    // wave 用の SpawnRule 配列 (caller 所有、寿命 = scene 寿命)
    SpawnRule _wave_rules[kTotalWaves] {};
    WaveDef   _wave_defs [kTotalWaves] {};

    Random _rng{ 0x5A17C0DEu };      // 決定論 PRNG (spawn 位置に使う)

    ShapeId  _player_shape;          // CollisionWorld 上の player Circle
    f32      _fire_cd        = 0.0f; // 発射 cooldown timer
    bool     _victory        = false;
    bool     _game_over_req  = false;
};

// =============================================================================
// GameOverScene — VICTORY / GAME OVER の最終画面。R で Title 戻し。
// =============================================================================
class GameOverScene : public Scene {
public:
    GameOverScene(u64 final_score, bool did_win) noexcept
        : _final_score(final_score), _did_win(did_win) {}

    ESvc WantedServices() const noexcept override {
        return ESvc::Default2D;
    }

    void OnEnter() noexcept override;
    void OnExit()  noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnRender(RenderContext& rc) noexcept override;

private:
    u64 _final_score = 0;
    u64 _saved_best  = 0;
    bool _did_win    = false;
    bool _is_new_record = false;
    f32  _state_sec  = 0.0f;
};

// =============================================================================
// FullGameApp::InitialScene
// =============================================================================
UniquePtr<Scene> FullGameApp::InitialScene() noexcept {
    return MakeUnique<TitleScene>();
}

// =============================================================================
// TitleScene 実装
// =============================================================================
void TitleScene::OnEnter() noexcept {
    GetGame().SetClearColor(_bg_color.x, _bg_color.y, _bg_color.z);

    // 背景色を 2 秒 ping-pong する Tween (Services 経由で自動 tick される)
    _bg_tween = Services().Tweens().Tween(
        &_bg_color, kBgDark, kBgBright, /*duration=*/2.0f, Easing::InOutSine);
    _to_bright = true;

    // 入力バインド
    InputMap& im = Services().Input();
    im.ClearAll();
    im.BindKey(ActionId("Start"), EKey::Space);
    im.BindKey(ActionId("Quit"),  EKey::Escape);

    // BGM 切替 (state-only、ログのみ出る)
    auto& app = static_cast<FullGameApp&>(GetGame());
    app.Music().SetState(EMusicState::Calm, 1.0f);
    app.Audio().PlayBgm("bgm_title", 1.0f, true);

    ACS_LOG_INFO("[Title] enter — Press Space to start, Esc to quit");
}

void TitleScene::OnExit() noexcept {
    if (HasServices()) Services().Tweens().CancelAll();
    ACS_LOG_INFO("[Title] exit");
}

void TitleScene::OnUpdate(f32 dt) noexcept {
    const InputMap& im = Services().Input();
    if (im.IsPressed(ActionId("Quit"))) {
        GetGame().Quit();
        return;
    }
    if (im.IsPressed(ActionId("Start"))) {
        Scenes().ChangeScene(MakeUnique<GameplayScene>());
        return;
    }

    // ping-pong 完了で逆向き再開
    if (!Services().Tweens().IsActive(_bg_tween)) {
        _to_bright = !_to_bright;
        const Vec3 to = _to_bright ? kBgBright : kBgDark;
        _bg_tween = Services().Tweens().Tween(
            &_bg_color, _bg_color, to, /*duration=*/2.0f, Easing::InOutSine);
    }
    GetGame().SetClearColor(_bg_color.x, _bg_color.y, _bg_color.z);

    _pulse_sec += dt;
}

void TitleScene::OnRender(RenderContext& rc) noexcept {
    auto& app = static_cast<FullGameApp&>(GetGame());
    app.EnsureSpritesInitialized();
    if (!app.SpritesReady()) return;

    SpriteBatch& sb = app.Sprites();
    const u32 sw = rc.Width();
    const u32 sh = rc.Height();
    sb.Begin(rc.Cmd(), sw, sh);

    // タイトル板 — 中央に大きめ矩形を 2 段重ね
    const f32 cx = static_cast<f32>(sw) * 0.5f;
    const f32 cy = static_cast<f32>(sh) * 0.4f;
    sb.DrawRect(cx - 360.0f, cy - 60.0f, 720.0f, 120.0f,
                Vec4{0.08f, 0.10f, 0.20f, 0.85f});
    sb.DrawRect(cx - 340.0f, cy - 40.0f, 680.0f, 14.0f, Vec4{1.0f, 0.85f, 0.20f, 0.9f});
    sb.DrawRect(cx - 340.0f, cy + 26.0f, 680.0f, 14.0f, Vec4{0.30f, 0.55f, 1.00f, 0.9f});

    // "Press Space" の点滅板。Sin で 0..1 を作って α に流す
    const f32 alpha = 0.35f + 0.45f * (0.5f + 0.5f * Sin(_pulse_sec * 3.5f));
    sb.DrawRect(cx - 220.0f, cy + 120.0f, 440.0f, 36.0f,
                Vec4{0.95f, 0.95f, 0.95f, alpha});

    // HighScore があれば右下に表示する小バー (色濃度で「ある / ない」を伝達)
    const HighScore& hs = app.GetHighScore();
    if (hs.best_score > 0) {
        sb.DrawRect(static_cast<f32>(sw) - 260.0f, static_cast<f32>(sh) - 50.0f,
                    240.0f, 30.0f, Vec4{0.1f, 0.1f, 0.1f, 0.7f});
        sb.DrawRect(static_cast<f32>(sw) - 254.0f, static_cast<f32>(sh) - 44.0f,
                    228.0f, 18.0f, Vec4{1.0f, 0.85f, 0.20f, 0.9f});
    }

    sb.End();
}

// =============================================================================
// GameplayScene 実装
// =============================================================================
void GameplayScene::OnEnter() noexcept {
    // ----- input -----
    InputMap& im = Services().Input();
    im.ClearAll();
    im.BindAxisKeys(ActionId("MoveX"), EKey::A, EKey::D);
    im.BindAxisKeys(ActionId("MoveY"), EKey::S, EKey::W);  // 上が +Y
    im.BindMouseButton(ActionId("Fire"), EMouseButton::Left);
    im.BindKey(ActionId("Pause"), EKey::P);   // 念のため
    im.BindKey(ActionId("Quit"),  EKey::Escape);

    // ----- subsystems -----
    _health.ClearAll();
    _score.Init();
    _score.SetComboDuration(2.5f);
    _bullets.Init(kMaxBullets);
    _particles.Init(kMaxParticles);
    _waves.Init();

    // 弾の def を登録 (1 種類だけ)
    ProjectileDef bdef{};
    bdef.id              = kBulletDefId;
    bdef.kind            = EProjectileKind::Bullet;
    bdef.speed           = kBulletSpeed;
    bdef.lifetime_sec    = kBulletLifetime;
    bdef.radius          = kBulletRadius;
    bdef.gravity_y       = 0.0f;
    bdef.pierces         = false;
    bdef.max_pierces     = 0;
    bdef.homing          = false;
    bdef.homing_strength = 0.0f;
    _bullets.RegisterDef(bdef);

    // hit / spawn 用 callback ブリッジ
    _bullets.SetHitTestFn(&GameplayScene::OnHitTest, this);
    _bullets.SetOnHitCallback(&GameplayScene::OnHit, this);
    _waves.SetOnSpawnCallback(&GameplayScene::OnWaveSpawn, this);
    _waves.SetOnWaveStateChangeCallback(&GameplayScene::OnWaveState, this);

    // ----- collision world -----
    CollisionWorld2D& phy = Services().Physics();
    phy.Init(2.0f);  // cell_size = 2 world units

    // ----- camera -----
    Camera2D& cam = Services().Camera();
    cam.SetPosition(Vec2{0.0f, 0.0f});
    cam.SetZoom(kWorldUnit);                       // 1 unit = 32 px
    cam.SetShakeAmplitude(0.4f);
    cam.SetShakeDecayRate(2.0f);
    cam.SetBounds(Vec2{-kWorldHalfW + 6.0f, -kWorldHalfH + 4.0f},
                  Vec2{ kWorldHalfW - 6.0f,  kWorldHalfH - 4.0f});

    // ----- floor tilemap (4x4 = checker pattern) -----
    const u32 tw = static_cast<u32>(kWorldHalfW * 2.0f);
    const u32 th = static_cast<u32>(kWorldHalfH * 2.0f);
    _floor.Init(tw, th, /*layer=*/1, /*tile_size=*/1.0f);
    for (u32 y = 0; y < th; ++y) {
        for (u32 x = 0; x < tw; ++x) {
            const u16 t = static_cast<u16>(((x + y) & 1) + 1);  // 1 or 2
            _floor.SetTile(x, y, TileId{t}, 0);
        }
    }

    // ----- player -----
    auto player_up = MakeUnique<Node2D>();
    player_up->Local().position = Vec2{0.0f, 0.0f};
    _player_node = &_root.AddChild(Move(player_up));
    _player_node->_SetId(NodeId{1u, static_cast<u8>(1)});

    _player_health = _health.Spawn(kPlayerHp);
    _player_shape  = phy.AddCircle(Circle{Vec2{0.0f, 0.0f}, kPlayerRadius});

    // ----- particle emitter (ヒット時 Burst で爆発) -----
    ParticleEmitterDef pdef{};
    pdef.color_start       = kParticleStart;
    pdef.color_end         = kParticleEnd;
    pdef.lifetime_sec      = 0.55f;
    pdef.emit_rate_per_sec = 0.0f;
    pdef.burst_count       = 16.0f;
    pdef.speed_min         = 1.5f;
    pdef.speed_max         = 4.5f;
    pdef.scale_start       = 0.3f;
    pdef.scale_end         = 0.0f;
    pdef.gravity           = Vec2{0.0f, 0.0f};
    _hit_emitter = _particles.CreateEmitter(pdef, Vec2{0.0f, 0.0f});
    _particles.SetEmitterActive(_hit_emitter, false);  // 連続放出オフ、Burst のみ

    // ----- perception (player を target に登録、wave で gameover 演出に使う) -----
    SenseConfig sc;
    sc.sight_range   = 10.0f;
    sc.sight_fov_rad = kPi;     // 180 度
    sc.hearing_range = 8.0f;
    _perception.SetConfig(sc);
    _perception.AddTarget(/*id=*/1u, Vec2{0.0f, 0.0f});

    // ----- wave 設定 -----
    // wave 0 : 5 体、wave 1 : 7 体、wave 2 : 9 体、wave 3 : 11 体、wave 4 : 13 体
    for (u32 i = 0; i < kTotalWaves; ++i) {
        _wave_rules[i].enemy_id           = "enemy_basic";
        _wave_rules[i].count              = 5u + i * 2u;
        _wave_rules[i].spawn_interval_sec = kSpawnIntervalSec;
        _wave_rules[i].initial_delay_sec  = 0.8f;
        _wave_rules[i].spawn_position     = Vec2{0.0f, 0.0f};  // OnWaveSpawn で再計算
        _wave_defs[i].wave_id              = "wave";
        _wave_defs[i].rule_count           = 1;
        _wave_defs[i].rules                = &_wave_rules[i];
        _wave_defs[i].wave_intermission_sec = kIntermissionSec;
        _waves.AddWave(_wave_defs[i]);
    }
    _waves.StartWaves();

    // ----- BGM / Flow -----
    auto& app = static_cast<FullGameApp&>(GetGame());
    app.Music().SetState(EMusicState::Combat, 1.5f);
    app.Audio().PlayBgm("bgm_gameplay", 1.5f, true);

    GetGame().SetClearColor(0.04f, 0.05f, 0.06f);
    ACS_LOG_INFO("[Gameplay] enter — WASD move, Mouse aim, LMB fire");
}

void GameplayScene::OnExit() noexcept {
    // 子ノードを全部 Destroy で reap させる
    for (u32 i = 0; i < kMaxEnemies; ++i) {
        if (_enemies[i].alive && _enemies[i].node) {
            _enemies[i].node->Destroy();
        }
        _enemies[i] = Enemy{};
    }
    if (_player_node) _player_node->Destroy();
    _root.ResolveStructuralChanges();
    _player_node = nullptr;

    _bullets.ClearAll();
    _particles.ClearAll();
    if (HasServices()) {
        Services().Physics().ClearAll();
        Services().Tweens().CancelAll();
    }
    ACS_LOG_INFO("[Gameplay] exit");
}

void GameplayScene::OnUpdate(f32 dt) noexcept {
    const InputMap& im = Services().Input();
    if (im.IsPressed(ActionId("Quit"))) {
        GetGame().Quit();
        return;
    }

    // ----- player の移動 -----
    Vec2 move{ im.Axis(ActionId("MoveX")), im.Axis(ActionId("MoveY")) };
    if (move.x != 0.0f || move.y != 0.0f) {
        const f32 len = Length(move);
        if (len > 0.0f) move = move * (1.0f / len);
        _player_node->Local().position = _player_node->Local().position
            + move * (kPlayerSpeed * dt);

        // world 端で clamp
        Vec2& p = _player_node->Local().position;
        if (p.x < -kWorldHalfW + kPlayerRadius) p.x = -kWorldHalfW + kPlayerRadius;
        if (p.x >  kWorldHalfW - kPlayerRadius) p.x =  kWorldHalfW - kPlayerRadius;
        if (p.y < -kWorldHalfH + kPlayerRadius) p.y = -kWorldHalfH + kPlayerRadius;
        if (p.y >  kWorldHalfH - kPlayerRadius) p.y =  kWorldHalfH - kPlayerRadius;

        // collision shape も追従
        Services().Physics().UpdateCircle(_player_shape,
                                          Circle{p, kPlayerRadius});
        _perception.UpdateTarget(1u, p);
    }

    // ----- camera 追従 -----
    Services().Camera().SetTargetPos(_player_node->Local().position, 6.0f);

    // ----- 発射 -----
    _fire_cd -= dt;
    if (im.IsHeld(ActionId("Fire")) && _fire_cd <= 0.0f) {
        const Vec2 from = _player_node->Local().position;
        const Vec2 dir  = Normalize(MouseWorld() - from);
        if (LengthSq(dir) > 0.0f) {
            FireBullet(from, dir);
            _fire_cd = kFireCooldown;
        }
    }

    // ----- 敵の AI: player に向かって直進 (Perception で sight 判定) -----
    const Vec2 player_pos = _player_node->Local().position;
    _perception.SetEyePos(player_pos, Vec2{1.0f, 0.0f});  // 全方位扱い (fov=180)
    _perception.Tick(dt);

    CollisionWorld2D& phy = Services().Physics();
    for (u32 i = 0; i < kMaxEnemies; ++i) {
        Enemy& e = _enemies[i];
        if (!e.alive || e.node == nullptr) continue;
        // 単純 chase
        Vec2 ep   = e.node->Local().position;
        Vec2 dir  = Normalize(player_pos - ep);
        if (LengthSq(dir) > 0.0f) {
            ep = ep + dir * (kEnemySpeed * dt);
            e.node->Local().position = ep;
            phy.UpdateCircle(e.shape, Circle{ep, kEnemyRadius});
        }

        // 接触判定 (player vs enemy)
        const f32 dx = ep.x - player_pos.x;
        const f32 dy = ep.y - player_pos.y;
        const f32 d2 = dx*dx + dy*dy;
        const f32 r  = kPlayerRadius + kEnemyRadius;
        if (d2 <= r * r && !_health.IsInvulnerable(_player_health)) {
            // ダメージ。i-frame 1.0s
            const bool lethal = _health.ApplyDamage(_player_health, 1.0f,
                                                     EDamageType::Physical);
            _health.SetInvulnerable(_player_health, kPlayerInvuln);
            // 被弾エフェクト (大きな画面振動 + 強いフラッシュ気味の粒子)
            _fx.TriggerShake(0.9f);
            _particles.SetEmitterPosition(_hit_emitter, player_pos);
            _particles.Burst(_hit_emitter);
            // SFX (state-only)
            static_cast<FullGameApp&>(GetGame()).Audio().PlaySfx("sfx_player_hurt", 1.0f);
            ACS_LOG_INFO("[Gameplay] player hit! hp=%.0f", static_cast<double>(_health.GetCurrentHp(_player_health)));
            if (lethal || _health.GetCurrentHp(_player_health) <= 0.0f) {
                RequestGameOver(/*victory=*/false);
                return;
            }
        }
    }

    // ----- subsystems の per-frame tick -----
    _health.Tick(dt);
    _score.Tick(dt);
    _bullets.Tick(dt);
    _particles.Tick(dt);
    _fx.Tick(dt);
    _waves.Tick(dt);

    // EffectSystem → Camera2D の trauma 配線 (EffectSystem は pull バス)
    if (_fx.PendingShakeTrauma() > 0.0f) {
        Services().Camera().AddShake(_fx.PendingShakeTrauma());
        _fx.ConsumeShake();
    }
}

void GameplayScene::OnFixedUpdate(f32 /*dt*/) noexcept {
    // PhysicsBody2D の OnFixedUpdate 駆動。本サンプルは player が kinematic で
    // FixedUpdate 内では velocity を直接動かさないため、Node2D::FixedUpdateTree
    // を呼ぶだけにする (= 各 Component の OnFixedUpdate を伝搬)。
    _root.FixedUpdateTree(1.0f / 60.0f);
    _root.ResolveStructuralChanges();
}

void GameplayScene::OnRender(RenderContext& rc) noexcept {
    auto& app = static_cast<FullGameApp&>(GetGame());
    app.EnsureSpritesInitialized();
    if (!app.SpritesReady()) return;

    SpriteBatch& sb = app.Sprites();
    const u32 sw = rc.Width();
    const u32 sh = rc.Height();
    sb.Begin(rc.Cmd(), sw, sh);

    // ===== world layer (camera 適用) =====
    const Vec2 cam_pos = Services().Camera().EffectiveViewCenter();
    sb.SetView(cam_pos.x, cam_pos.y, kWorldUnit);

    // floor tile を描く (灰色 checker)
    const f32 ts   = _floor.TileSize();
    const u32 tw   = _floor.Width();
    const u32 th   = _floor.Height();
    const f32 ox   = -kWorldHalfW;
    const f32 oy   = -kWorldHalfH;
    const TileId* layer0 = _floor.LayerData(0);
    if (layer0) {
        for (u32 y = 0; y < th; ++y) {
            for (u32 x = 0; x < tw; ++x) {
                const TileId t = layer0[y * tw + x];
                if (t.IsEmpty()) continue;
                const Vec4 col = (t.value == 1) ? kColorTileLight : kColorTileDark;
                sb.DrawRect(ox + static_cast<f32>(x) * ts,
                            oy + static_cast<f32>(y) * ts,
                            ts * 0.96f, ts * 0.96f, col);
            }
        }
    }

    // 敵 (青い 矩形)
    for (u32 i = 0; i < kMaxEnemies; ++i) {
        const Enemy& e = _enemies[i];
        if (!e.alive || e.node == nullptr) continue;
        const Vec2 p = e.node->Local().position;
        const f32 sz = kEnemyRadius * 2.0f;
        sb.DrawRect(p.x - kEnemyRadius, p.y - kEnemyRadius, sz, sz, kColorEnemy);
    }

    // 弾 (黄色い小矩形)
    {
        u32 n = 0;
        const ProjectileInstance* bs = _bullets.AllAlive(n);
        for (u32 i = 0; i < n; ++i) {
            const Vec2 p = bs[i].position;
            const f32 sz = kBulletRadius * 2.0f;
            sb.DrawRect(p.x - kBulletRadius, p.y - kBulletRadius, sz, sz, kColorBullet);
        }
    }

    // パーティクル (橙赤グラデ、scale_start → scale_end で線形補間)
    {
        u32 n = 0;
        const Particle* ps = _particles.AllParticles(n);
        for (u32 i = 0; i < n; ++i) {
            const Particle& p = ps[i];
            if (!p.IsAlive()) continue;
            const f32 t   = p.NormalizedAge();
            const f32 inv = 1.0f - t;
            const f32 scale = p.scale_start * inv + p.scale_end * t;
            const Vec3 col  = Vec3{
                p.color_start.x * inv + p.color_end.x * t,
                p.color_start.y * inv + p.color_end.y * t,
                p.color_start.z * inv + p.color_end.z * t,
            };
            const f32 alpha = inv;
            const f32 hs    = scale * 0.5f;
            sb.DrawRect(p.position.x - hs, p.position.y - hs, scale, scale,
                        Vec4{col.x, col.y, col.z, alpha});
        }
    }

    // player (赤い円 = ここは矩形で代用、被弾無敵中は色を白寄せ)
    {
        const Vec2 p = _player_node->Local().position;
        const Vec4 col = _health.IsInvulnerable(_player_health)
                            ? kColorPlayerHurt : kColorPlayer;
        const f32 sz = kPlayerRadius * 2.0f;
        sb.DrawRect(p.x - kPlayerRadius, p.y - kPlayerRadius, sz, sz, col);
    }

    // ===== HUD layer (camera 解除して左上原点画面座標で描く) =====
    sb.SetView(static_cast<f32>(sw) * 0.5f, static_cast<f32>(sh) * 0.5f, 1.0f);

    // HP bar (左上)
    {
        const f32 frac = _health.GetHpFraction(_player_health);
        sb.DrawRect(20.0f, 20.0f, 240.0f, 24.0f, Vec4{0.1f, 0.1f, 0.1f, 0.7f});
        sb.DrawRect(24.0f, 24.0f, 232.0f, 16.0f, Vec4{0.25f, 0.05f, 0.05f, 0.85f});
        sb.DrawRect(24.0f, 24.0f, 232.0f * frac, 16.0f,
                    Vec4{0.95f, 0.20f, 0.20f, 1.0f});
    }

    // Score (右上、桁数を bar 本数で可視化)
    {
        const u64 sc = _score.CurrentScore();
        const u32 bars = static_cast<u32>(sc / 10u);   // 1 bar = 10 score
        const u32 capped = bars < 24u ? bars : 24u;
        const f32 right = static_cast<f32>(sw) - 20.0f;
        sb.DrawRect(right - 240.0f, 20.0f, 240.0f, 24.0f, Vec4{0.1f, 0.1f, 0.1f, 0.7f});
        for (u32 i = 0; i < capped; ++i) {
            sb.DrawRect(right - 232.0f + static_cast<f32>(i) * 9.5f, 24.0f, 8.0f, 16.0f,
                        Vec4{1.0f, 0.85f, 0.20f, 1.0f});
        }
    }

    // Wave 番号 (中央上、現在 wave + 全 wave をバー個数で表現)
    {
        const u32 wave = _waves.CurrentWaveIndex();
        const u32 total = _waves.TotalWaves();
        const f32 cx = static_cast<f32>(sw) * 0.5f;
        sb.DrawRect(cx - 120.0f, 20.0f, 240.0f, 24.0f, Vec4{0.1f, 0.1f, 0.1f, 0.7f});
        for (u32 i = 0; i < total; ++i) {
            const Vec4 c = (i < wave) ? Vec4{0.30f, 0.55f, 1.00f, 1.0f}
                                       : Vec4{0.20f, 0.20f, 0.25f, 1.0f};
            sb.DrawRect(cx - 116.0f + static_cast<f32>(i) * 47.5f, 24.0f, 44.0f, 16.0f, c);
        }
    }

    // 全画面 Flash overlay (EffectSystem)
    {
        const f32 fi = _fx.FlashIntensity();
        if (fi > 0.001f) {
            const Vec3 fc = _fx.FlashColor();
            sb.DrawRect(0.0f, 0.0f, static_cast<f32>(sw), static_cast<f32>(sh),
                        Vec4{fc.x, fc.y, fc.z, fi});
        }
    }

    sb.End();
}

void GameplayScene::SpawnEnemy(Vec2 pos) noexcept {
    // 空き slot を線形探索
    u32 slot = kMaxEnemies;
    for (u32 i = 0; i < kMaxEnemies; ++i) {
        if (!_enemies[i].alive) { slot = i; break; }
    }
    if (slot >= kMaxEnemies) {
        ACS_LOG_WARN("[Gameplay] enemy pool full, drop spawn");
        return;
    }
    auto up = MakeUnique<Node2D>();
    up->Local().position = pos;
    Node2D& nref = _root.AddChild(Move(up));
    // NodeId は (slot+100, 1) で識別 (player=1, 敵=100..163)
    nref._SetId(NodeId{slot + 100u, static_cast<u8>(1)});

    Enemy& e = _enemies[slot];
    e.alive = true;
    e.node  = &nref;
    e.hp    = _health.Spawn(kEnemyHp);
    e.shape = Services().Physics().AddCircle(Circle{pos, kEnemyRadius});
    e.wave_idx_at_spawn = _waves.CurrentWaveIndex();
}

void GameplayScene::FireBullet(Vec2 from, Vec2 dir_unit) noexcept {
    const Vec2 vel = dir_unit * kBulletSpeed;
    const ProjectileId pid = _bullets.Spawn(kBulletDefId, from, vel,
                                              /*owner_id=*/1u, kBulletDamage);
    if (!pid.IsValid()) return;
    static_cast<FullGameApp&>(GetGame()).Audio().PlaySfx("sfx_fire", 0.6f);
}

void GameplayScene::TriggerHitEffect(Vec2 pos) noexcept {
    _particles.SetEmitterPosition(_hit_emitter, pos);
    _particles.Burst(_hit_emitter);
    _fx.TriggerShake(0.35f);
    _fx.Flash(Vec3{1.0f, 0.95f, 0.7f}, 0.25f, 0.08f);
    static_cast<FullGameApp&>(GetGame()).Audio().PlaySfx("sfx_enemy_hit", 0.8f);
}

void GameplayScene::OnWaveSpawn(void* user, const char* /*enemy_id*/, Vec2 /*ignored_pos*/) noexcept {
    auto* self = static_cast<GameplayScene*>(user);
    // 画面端のランダムな辺から spawn
    const u32 edge = static_cast<u32>(self->_rng.RangeInt(0, 3));
    Vec2 pos{0.0f, 0.0f};
    const f32 margin = 1.0f;
    switch (edge) {
    case 0: // top
        pos.x = self->_rng.RangeF32(-kWorldHalfW + margin, kWorldHalfW - margin);
        pos.y =  kWorldHalfH - margin;
        break;
    case 1: // bottom
        pos.x = self->_rng.RangeF32(-kWorldHalfW + margin, kWorldHalfW - margin);
        pos.y = -kWorldHalfH + margin;
        break;
    case 2: // left
        pos.x = -kWorldHalfW + margin;
        pos.y = self->_rng.RangeF32(-kWorldHalfH + margin, kWorldHalfH - margin);
        break;
    default: // right
        pos.x =  kWorldHalfW - margin;
        pos.y = self->_rng.RangeF32(-kWorldHalfH + margin, kWorldHalfH - margin);
        break;
    }
    self->SpawnEnemy(pos);
}

void GameplayScene::OnWaveState(void* user, u32 wave_index,
                                  EWaveState /*from*/, EWaveState to) noexcept {
    auto* self = static_cast<GameplayScene*>(user);
    if (to == EWaveState::AllComplete) {
        ACS_LOG_INFO("[Gameplay] all %u waves cleared! VICTORY!", wave_index + 1u);
        self->RequestGameOver(/*victory=*/true);
    } else if (to == EWaveState::Cleared) {
        ACS_LOG_INFO("[Gameplay] wave %u cleared", wave_index);
    } else if (to == EWaveState::Spawning) {
        ACS_LOG_INFO("[Gameplay] wave %u start", wave_index);
    }
}

bool GameplayScene::OnHitTest(void* user, const ProjectileInstance& p,
                                u32& out_target, f32& out_dmg) noexcept {
    auto* self = static_cast<GameplayScene*>(user);
    // 近接 enemy を線形探索 (敵数 <= kMaxEnemies = 64 で十分速い)
    for (u32 i = 0; i < kMaxEnemies; ++i) {
        const Enemy& e = self->_enemies[i];
        if (!e.alive || e.node == nullptr) continue;
        const Vec2 ep = e.node->Local().position;
        const f32 dx = p.position.x - ep.x;
        const f32 dy = p.position.y - ep.y;
        const f32 d2 = dx*dx + dy*dy;
        const f32 r  = kEnemyRadius + kBulletRadius;
        if (d2 <= r * r) {
            out_target = i;       // enemy index を target_id として返す
            out_dmg    = p.damage;
            return true;
        }
    }
    return false;
}

void GameplayScene::OnHit(void* user, ProjectileId /*pid*/, const char* /*def_id*/,
                            u32 target_id, f32 dmg) noexcept {
    auto* self = static_cast<GameplayScene*>(user);
    if (target_id >= kMaxEnemies) return;
    Enemy& e = self->_enemies[target_id];
    if (!e.alive) return;

    // ダメージ + 撃破判定
    const bool lethal = self->_health.ApplyDamage(e.hp, dmg, EDamageType::Physical);
    if (lethal) {
        // 撃破時のエフェクト + score
        const Vec2 ep = e.node ? e.node->Local().position : Vec2{0.0f, 0.0f};
        self->TriggerHitEffect(ep);
        self->_score.NotifyHit();
        self->_score.AddScore("enemy.basic", 10);
        self->_waves.NotifyEnemyKilled("enemy_basic");

        // 物理 / Node / Health を全部 reap
        self->_health.Despawn(e.hp);
        if (self->HasServices()) self->Services().Physics().Remove(e.shape);
        if (e.node) e.node->Destroy();
        e.alive = false;
        e.node  = nullptr;
    } else {
        // 致死ではない → 軽い hit エフェクト
        const Vec2 ep = e.node ? e.node->Local().position : Vec2{0.0f, 0.0f};
        self->TriggerHitEffect(ep);
    }
}

void GameplayScene::RequestGameOver(bool victory) noexcept {
    if (_game_over_req) return;
    _game_over_req = true;
    _victory = victory;
    auto& app = static_cast<FullGameApp&>(GetGame());
    app.SaveHighScoreIfBetter(_score.CurrentScore());
    app.Music().SetState(victory ? EMusicState::Victory : EMusicState::GameOver, 1.0f);
    app.Audio().PlayBgm(victory ? "bgm_victory" : "bgm_gameover", 1.0f, victory ? false : true);
    Scenes().ChangeScene(MakeUnique<GameOverScene>(_score.CurrentScore(), victory));
}

Vec2 GameplayScene::MouseWorld() const noexcept {
    const Vec2 m = Input::MousePos();
    // OnRender がまだ画面サイズを取れない可能性 (起動直後) を考慮、Renderer 直問合せ。
    // GetGame() / GetRenderer() はどちらも const noexcept の中でも非 const 参照を
    // 返す API (Scene/Application の設計上、世界は常に mutable 扱い) なので
    // const_cast を入れずに自然に書ける。
    auto& app = static_cast<FullGameApp&>(GetGame());
    IRhiSwapchain* sc = app.GetRenderer().Swapchain();
    if (!sc) return _player_node->Local().position;
    const u32 sw = sc->Width();
    const u32 sh = sc->Height();
    return Services().Camera().ScreenToWorld(m, sw, sh);
}

// =============================================================================
// GameOverScene 実装
// =============================================================================
void GameOverScene::OnEnter() noexcept {
    InputMap& im = Services().Input();
    im.ClearAll();
    im.BindKey(ActionId("Reset"), EKey::R);
    im.BindKey(ActionId("Quit"),  EKey::Escape);

    auto& app = static_cast<FullGameApp&>(GetGame());
    _saved_best    = app.GetHighScore().best_score;
    _is_new_record = _final_score >= _saved_best && _final_score > 0;

    GetGame().SetClearColor(_did_win ? 0.06f : 0.18f,
                            _did_win ? 0.16f : 0.04f,
                            _did_win ? 0.06f : 0.04f);
    ACS_LOG_INFO("[GameOver] enter — %s, score=%llu, best=%llu",
                 _did_win ? "VICTORY" : "DEFEAT",
                 static_cast<unsigned long long>(_final_score),
                 static_cast<unsigned long long>(_saved_best));
}

void GameOverScene::OnExit() noexcept {
    ACS_LOG_INFO("[GameOver] exit");
}

void GameOverScene::OnUpdate(f32 dt) noexcept {
    const InputMap& im = Services().Input();
    if (im.IsPressed(ActionId("Quit"))) {
        GetGame().Quit();
        return;
    }
    if (im.IsPressed(ActionId("Reset"))) {
        Scenes().ChangeScene(MakeUnique<TitleScene>());
        return;
    }
    _state_sec += dt;
}

void GameOverScene::OnRender(RenderContext& rc) noexcept {
    auto& app = static_cast<FullGameApp&>(GetGame());
    app.EnsureSpritesInitialized();
    if (!app.SpritesReady()) return;

    SpriteBatch& sb = app.Sprites();
    const u32 sw = rc.Width();
    const u32 sh = rc.Height();
    sb.Begin(rc.Cmd(), sw, sh);

    const f32 cx = static_cast<f32>(sw) * 0.5f;
    const f32 cy = static_cast<f32>(sh) * 0.35f;

    // 結果表示の大バー (勝利は緑系、敗北は赤系)
    const Vec4 result_col = _did_win ? Vec4{0.20f, 0.85f, 0.40f, 0.95f}
                                     : Vec4{0.85f, 0.20f, 0.20f, 0.95f};
    sb.DrawRect(cx - 400.0f, cy - 50.0f, 800.0f, 100.0f,
                Vec4{0.05f, 0.05f, 0.05f, 0.85f});
    sb.DrawRect(cx - 380.0f, cy - 30.0f, 760.0f, 60.0f, result_col);

    // 最終 score バー (黄色)
    const f32 score_y = cy + 110.0f;
    sb.DrawRect(cx - 240.0f, score_y, 480.0f, 30.0f, Vec4{0.05f, 0.05f, 0.05f, 0.8f});
    const u64 cap = _final_score < 240ULL ? _final_score : 240ULL;
    sb.DrawRect(cx - 234.0f, score_y + 5.0f, static_cast<f32>(cap) * 2.0f, 20.0f,
                Vec4{1.0f, 0.85f, 0.20f, 1.0f});

    // High score バー (青、新記録なら点滅)
    const f32 best_y = score_y + 60.0f;
    sb.DrawRect(cx - 240.0f, best_y, 480.0f, 30.0f, Vec4{0.05f, 0.05f, 0.05f, 0.8f});
    f32 best_alpha = 1.0f;
    if (_is_new_record) {
        best_alpha = 0.5f + 0.5f * Sin(_state_sec * 6.0f);
    }
    const u64 best = _saved_best;
    const u64 best_cap = best < 240ULL ? best : 240ULL;
    sb.DrawRect(cx - 234.0f, best_y + 5.0f, static_cast<f32>(best_cap) * 2.0f, 20.0f,
                Vec4{0.30f, 0.55f, 1.00f, best_alpha});

    // 操作ヒント (R / Esc を象徴する 2 つの矩形)
    sb.DrawRect(cx - 220.0f, static_cast<f32>(sh) - 80.0f, 200.0f, 40.0f,
                Vec4{0.85f, 0.85f, 0.85f, 0.7f});  // R: title へ
    sb.DrawRect(cx +  20.0f, static_cast<f32>(sh) - 80.0f, 200.0f, 40.0f,
                Vec4{0.50f, 0.50f, 0.50f, 0.7f});  // Esc: quit

    sb.End();
}

// =============================================================================
// エントリポイント
// =============================================================================
ACS_GAME_MAIN(FullGameApp)
