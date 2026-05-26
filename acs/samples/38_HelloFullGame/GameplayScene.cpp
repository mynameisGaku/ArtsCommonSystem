// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — GameplayScene 実装。シーン skeleton + モジュール orchestration。
#include "GameplayScene.h"
#include "FullGameApp.h"
#include "GameOverScene.h"
#include "WaveCallbacks.h"

#include "render/SpriteBatch.h"
#include "render/Renderer.h"
#include "render/IRhiSwapchain.h"
#include "platform/Input.h"
#include "foundation/Log.h"
#include "math/Math.h"

using namespace acs;
using namespace acs::game;

namespace hellofg {

void GameplayScene::OnEnter() noexcept {
    // ----- input -----
    InputMap& im = Services().Input();
    im.ClearAll();
    im.BindAxisKeys(ActionId("MoveX"), EKey::A, EKey::D);
    im.BindAxisKeys(ActionId("MoveY"), EKey::S, EKey::W);  // 上向き = +Y
    im.BindMouseButton(ActionId("Fire"), EMouseButton::Left);
    im.BindKey(ActionId("Pause"), EKey::P);
    im.BindKey(ActionId("Quit"),  EKey::Escape);

    // ----- subsystem 初期化 -----
    _health.ClearAll();
    _score.Init();
    _score.SetComboDuration(2.5f);

    _bullets_mod.Init(_bullets);
    _bullets.SetHitTestFn(&ProjectileOnHitTest, this);
    _bullets.SetOnHitCallback(&ProjectileOnHit, this);

    _particles.Init(kMaxParticles);
    _waves.Init();
    _waves.SetOnSpawnCallback(&WaveOnSpawn, this);
    _waves.SetOnWaveStateChangeCallback(&WaveOnState, this);

    // ----- collision world -----
    CollisionWorld2D& phy = Services().Physics();
    phy.Init(2.0f);

    // ----- camera -----
    Camera2D& cam = Services().Camera();
    cam.SetPosition(FVec2{0.0f, 0.0f});
    cam.SetZoom(kWorldUnit);
    cam.SetShakeAmplitude(0.4f);
    cam.SetShakeDecayRate(2.0f);
    cam.SetBounds(FVec2{-kWorldHalfW + 6.0f, -kWorldHalfH + 4.0f},
                  FVec2{ kWorldHalfW - 6.0f,  kWorldHalfH - 4.0f});

    // ----- floor tilemap -----
    const u32 tw = static_cast<u32>(kWorldHalfW * 2.0f);
    const u32 th = static_cast<u32>(kWorldHalfH * 2.0f);
    _floor.Init(tw, th, /*layer=*/1, /*tile_size=*/1.0f);
    // チェッカー柄 (隣接タイルで色が交互になるよう ((x+y)&1)+1 をタイル ID にする)。
    for (u32 y = 0; y < th; ++y) {
        for (u32 x = 0; x < tw; ++x) {
            const u16 t = static_cast<u16>(((x + y) & 1) + 1);
            _floor.SetTile(x, y, TileId{t}, 0);
        }
    }

    // ----- 各機能モジュールの init -----
    _player.Init(*this, _root, _health);
    _enemies.Reset();
    _hit_effects.Init(_particles);

    // ----- perception -----
    SenseConfig sc;
    sc.sight_range   = 10.0f;
    sc.sight_fov_rad = kPi;
    sc.hearing_range = 8.0f;
    _perception.SetConfig(sc);
    _perception.AddTarget(/*id=*/1u, FVec2{0.0f, 0.0f});

    // ----- wave 定義 (難易度カーブ: 5+2*i 体、間隔は固定) -----
    for (u32 i = 0; i < kTotalWaves; ++i) {
        _wave_rules[i].enemy_id           = "enemy_basic";
        _wave_rules[i].count              = 5u + i * 2u;
        _wave_rules[i].spawn_interval_sec = kSpawnIntervalSec;
        _wave_rules[i].initial_delay_sec  = 0.8f;
        _wave_rules[i].spawn_position     = FVec2{0.0f, 0.0f};
        _wave_defs[i].wave_id               = "wave";
        _wave_defs[i].rule_count            = 1;
        _wave_defs[i].rules                 = &_wave_rules[i];
        _wave_defs[i].wave_intermission_sec = kIntermissionSec;
        _waves.AddWave(_wave_defs[i]);
    }
    _waves.StartWaves();

    // ----- BGM -----
    auto& app = static_cast<FullGameApp&>(GetGame());
    app.Music().SetState(EMusicState::Combat, 1.5f);
    app.Audio().PlayBgm("bgm_gameplay", 1.5f, true);

    GetGame().SetClearColor(0.04f, 0.05f, 0.06f);
    ACS_LOG_INFO("[Gameplay] enter - WASD move, Mouse aim, LMB fire");
}

void GameplayScene::OnExit() noexcept {
    // 敵 → player の順で root の Node を Destroy。ResolveStructuralChanges でまとめ反映。
    _enemies.Shutdown();
    _player.Shutdown();
    _root.ResolveStructuralChanges();

    _bullets_mod.Shutdown(_bullets);
    _hit_effects.Shutdown(_particles);

    if (HasServices()) {
        Services().Physics().ClearAll();
        Services().Tweens().CancelAll();
    }
    ACS_LOG_INFO("[Gameplay] exit");
}

void GameplayScene::OnUpdate(f32 dt) noexcept {
    // ----- FPS 計測 (EMA 0.1 で平滑化、HUD に出す) -----
    _last_dt = dt;
    if (dt > 0.0f) {
        const f32 inst_fps = 1.0f / dt;
        _fps_ema = _fps_ema * 0.9f + inst_fps * 0.1f;
    }

    const InputMap& im = Services().Input();
    if (im.IsPressed(ActionId("Quit"))) {
        GetGame().Quit();
        return;
    }

    // ----- player 入力 + 射撃 -----
    const FVec2 player_pos = _player.UpdateMovement(*this, dt);
    _perception.UpdateTarget(1u, player_pos);
    Services().Camera().SetTargetPos(player_pos, 6.0f);
    _player.UpdateFire(*this, dt);

    // ----- AI (敵全員でプレイヤー追跡 + 接触ダメージ判定) -----
    _perception.SetEyePos(player_pos, FVec2{1.0f, 0.0f});
    _perception.Tick(dt);
    const bool lethal_contact = _enemies.TickChaseAndContact(*this, _health, player_pos, dt);
    if (lethal_contact) {
        RequestGameOver(/*victory=*/false);
        return;
    }

    // ----- subsystem の per-frame tick -----
    _health.Tick(dt);
    _score.Tick(dt);
    _bullets.Tick(dt);
    _particles.Tick(dt);
    _hit_effects.Tick(*this, dt);    // camera shake もここで吸う
    _waves.Tick(dt);
}

void GameplayScene::OnFixedUpdate(f32 /*dt*/) noexcept {
    // 固定 step は PhysicsBody2D の決定性のため Game 側で 1/60 に固定済み。
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

    // ===== world layer (camera 投影) =====
    const FVec2 cam_pos = Services().Camera().EffectiveViewCenter();
    sb.SetView(cam_pos.x, cam_pos.y, kWorldUnit);

    // floor tiles (チェッカー柄)
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
                const FVec4 col = (t.value == 1) ? kColorTileLight : kColorTileDark;
                sb.DrawRect(ox + static_cast<f32>(x) * ts,
                            oy + static_cast<f32>(y) * ts,
                            ts * 0.96f, ts * 0.96f, col);
            }
        }
    }

    // 敵 / 弾 / パーティクル / プレイヤーの順で重ね描き。
    _enemies.DrawAll(sb);
    _bullets_mod.DrawAll(_bullets, sb, _last_dt);
    _hit_effects.DrawParticles(_particles, sb);

    if (_player.Node()) {
        const FVec2 p = _player.Position();
        const FVec4 col = _player.IsInvulnerable(_health) ? kColorPlayerHurt : kColorPlayer;
        const f32 sz = kPlayerRadius * 2.0f;
        sb.DrawRect(p.x - kPlayerRadius, p.y - kPlayerRadius, sz, sz, col);
    }

    // ===== HUD layer (ピクセル座標、camera 無関係) =====
    sb.SetView(static_cast<f32>(sw) * 0.5f, static_cast<f32>(sh) * 0.5f, 1.0f);
    _hud.Draw(*this, sb, sw, sh, _last_dt, _fps_ema);

    sb.End();
}

// ----- モジュール連携 -----

void GameplayScene::OnPlayerHurt() noexcept {
    _hit_effects.TriggerPlayerHurt(*this, _player.Position());
}

void GameplayScene::OnEnemyKilled() noexcept {
    _score.NotifyHit();
    _score.AddScore("enemy.basic", 10);
    _waves.NotifyEnemyKilled("enemy_basic");
}

void GameplayScene::RequestGameOver(bool victory) noexcept {
    if (_game_over_req) return;       // 多重遷移ガード
    _game_over_req = true;
    _victory = victory;

    auto& app = static_cast<FullGameApp&>(GetGame());
    app.SaveHighScoreIfBetter(_score.CurrentScore());
    app.Music().SetState(victory ? EMusicState::Victory : EMusicState::GameOver, 1.0f);
    app.Audio().PlayBgm(victory ? "bgm_victory" : "bgm_gameover",
                        1.0f, victory ? false : true);
    Scenes().ChangeScene(MakeUnique<GameOverScene>(_score.CurrentScore(), victory));
}

FVec2 GameplayScene::MouseWorld() const noexcept {
    const FVec2 m = Input::MousePos();
    auto& app = static_cast<FullGameApp&>(GetGame());
    IRhiSwapchain* sc = app.GetRenderer().Swapchain();
    if (!sc) {
        // swapchain が出てくる前のごく初期フレームでは player 座標を返してフリーズ防止。
        return _player.Node() ? _player.Node()->Local().position : FVec2{0.0f, 0.0f};
    }
    return Services().Camera().ScreenToWorld(m, sc->Width(), sc->Height());
}

} // namespace hellofg
