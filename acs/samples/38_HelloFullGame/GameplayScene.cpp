// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — GameplayScene 実装。本編のゲームロジック。
#include "GameplayScene.h"
#include "FullGameApp.h"
#include "GameOverScene.h"

#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/Renderer.h"
#include "render/IRhiSwapchain.h"
#include "platform/Input.h"
#include "foundation/Log.h"
#include "math/Math.h"

#include <cstdio>

using namespace acs;
using namespace acs::game;

namespace hellofg {

void GameplayScene::OnEnter() noexcept {
    // ----- input -----
    InputMap& im = Services().Input();
    im.ClearAll();
    im.BindAxisKeys(ActionId("MoveX"), EKey::A, EKey::D);
    im.BindAxisKeys(ActionId("MoveY"), EKey::S, EKey::W);  // 上が +Y
    im.BindMouseButton(ActionId("Fire"), EMouseButton::Left);
    im.BindKey(ActionId("Pause"), EKey::P);
    im.BindKey(ActionId("Quit"),  EKey::Escape);

    // ----- subsystems -----
    _health.ClearAll();
    _score.Init();
    _score.SetComboDuration(2.5f);
    _bullets.Init(kMaxBullets);
    _particles.Init(kMaxParticles);
    _waves.Init();

    // 弾の def を登録
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

    _bullets.SetHitTestFn(&GameplayScene::OnHitTest, this);
    _bullets.SetOnHitCallback(&GameplayScene::OnHit, this);
    _waves.SetOnSpawnCallback(&GameplayScene::OnWaveSpawn, this);
    _waves.SetOnWaveStateChangeCallback(&GameplayScene::OnWaveState, this);

    // ----- collision world -----
    CollisionWorld2D& phy = Services().Physics();
    phy.Init(2.0f);

    // ----- camera -----
    Camera2D& cam = Services().Camera();
    cam.SetPosition(Vec2{0.0f, 0.0f});
    cam.SetZoom(kWorldUnit);
    cam.SetShakeAmplitude(0.4f);
    cam.SetShakeDecayRate(2.0f);
    cam.SetBounds(Vec2{-kWorldHalfW + 6.0f, -kWorldHalfH + 4.0f},
                  Vec2{ kWorldHalfW - 6.0f,  kWorldHalfH - 4.0f});

    // ----- floor tilemap -----
    const u32 tw = static_cast<u32>(kWorldHalfW * 2.0f);
    const u32 th = static_cast<u32>(kWorldHalfH * 2.0f);
    _floor.Init(tw, th, /*layer=*/1, /*tile_size=*/1.0f);
    for (u32 y = 0; y < th; ++y) {
        for (u32 x = 0; x < tw; ++x) {
            const u16 t = static_cast<u16>(((x + y) & 1) + 1);
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

    // ----- particle emitter -----
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
    _particles.SetEmitterActive(_hit_emitter, false);

    // ----- perception -----
    SenseConfig sc;
    sc.sight_range   = 10.0f;
    sc.sight_fov_rad = kPi;
    sc.hearing_range = 8.0f;
    _perception.SetConfig(sc);
    _perception.AddTarget(/*id=*/1u, Vec2{0.0f, 0.0f});

    // ----- wave 設定 -----
    for (u32 i = 0; i < kTotalWaves; ++i) {
        _wave_rules[i].enemy_id           = "enemy_basic";
        _wave_rules[i].count              = 5u + i * 2u;
        _wave_rules[i].spawn_interval_sec = kSpawnIntervalSec;
        _wave_rules[i].initial_delay_sec  = 0.8f;
        _wave_rules[i].spawn_position     = Vec2{0.0f, 0.0f};
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
    // ----- FPS 計測 -----
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

    // ----- player の移動 -----
    Vec2 move{ im.Axis(ActionId("MoveX")), im.Axis(ActionId("MoveY")) };
    if (move.x != 0.0f || move.y != 0.0f) {
        const f32 len = Length(move);
        if (len > 0.0f) move = move * (1.0f / len);
        _player_node->Local().position = _player_node->Local().position
            + move * (kPlayerSpeed * dt);

        Vec2& p = _player_node->Local().position;
        if (p.x < -kWorldHalfW + kPlayerRadius) p.x = -kWorldHalfW + kPlayerRadius;
        if (p.x >  kWorldHalfW - kPlayerRadius) p.x =  kWorldHalfW - kPlayerRadius;
        if (p.y < -kWorldHalfH + kPlayerRadius) p.y = -kWorldHalfH + kPlayerRadius;
        if (p.y >  kWorldHalfH - kPlayerRadius) p.y =  kWorldHalfH - kPlayerRadius;

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

    // ----- 敵の AI -----
    const Vec2 player_pos = _player_node->Local().position;
    _perception.SetEyePos(player_pos, Vec2{1.0f, 0.0f});
    _perception.Tick(dt);

    CollisionWorld2D& phy = Services().Physics();
    for (u32 i = 0; i < kMaxEnemies; ++i) {
        Enemy& e = _enemies[i];
        if (!e.alive || e.node == nullptr) continue;
        Vec2 ep   = e.node->Local().position;
        Vec2 dir  = Normalize(player_pos - ep);
        if (LengthSq(dir) > 0.0f) {
            ep = ep + dir * (kEnemySpeed * dt);
            e.node->Local().position = ep;
            phy.UpdateCircle(e.shape, Circle{ep, kEnemyRadius});
        }

        const f32 dx = ep.x - player_pos.x;
        const f32 dy = ep.y - player_pos.y;
        const f32 d2 = dx*dx + dy*dy;
        const f32 r  = kPlayerRadius + kEnemyRadius;
        if (d2 <= r * r && !_health.IsInvulnerable(_player_health)) {
            const bool lethal = _health.ApplyDamage(_player_health, 1.0f,
                                                     EDamageType::Physical);
            _health.SetInvulnerable(_player_health, kPlayerInvuln);
            _fx.TriggerShake(0.9f);
            _particles.SetEmitterPosition(_hit_emitter, player_pos);
            _particles.Burst(_hit_emitter);
            static_cast<FullGameApp&>(GetGame()).Audio().PlaySfx("sfx_player_hurt", 1.0f);
            ACS_LOG_INFO("[Gameplay] player hit! hp=%.0f",
                         static_cast<double>(_health.GetCurrentHp(_player_health)));
            if (lethal || _health.GetCurrentHp(_player_health) <= 0.0f) {
                RequestGameOver(/*victory=*/false);
                return;
            }
        }
    }

    // ----- subsystems per-frame tick -----
    _health.Tick(dt);
    _score.Tick(dt);
    _bullets.Tick(dt);
    _particles.Tick(dt);
    _fx.Tick(dt);
    _waves.Tick(dt);

    if (_fx.PendingShakeTrauma() > 0.0f) {
        Services().Camera().AddShake(_fx.PendingShakeTrauma());
        _fx.ConsumeShake();
    }
}

void GameplayScene::OnFixedUpdate(f32 /*dt*/) noexcept {
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

    // ===== world layer =====
    const Vec2 cam_pos = Services().Camera().EffectiveViewCenter();
    sb.SetView(cam_pos.x, cam_pos.y, kWorldUnit);

    // floor tiles
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

    // 敵
    for (u32 i = 0; i < kMaxEnemies; ++i) {
        const Enemy& e = _enemies[i];
        if (!e.alive || e.node == nullptr) continue;
        const Vec2 p = e.node->Local().position;
        const f32 sz = kEnemyRadius * 2.0f;
        sb.DrawRect(p.x - kEnemyRadius, p.y - kEnemyRadius, sz, sz, kColorEnemy);
    }

    // 弾: 直前フレーム分の経路を 32 段の連続矩形で連続線化
    {
        u32 n = 0;
        const ProjectileInstance* bs = _bullets.AllAlive(n);
        const f32 sz = kBulletRadius * 2.0f;
        const f32 trail_sec = _last_dt * 1.2f;
        constexpr u32 kSteps = 32;
        for (u32 i = 0; i < n; ++i) {
            const Vec2 p = bs[i].position;
            const Vec2 v = bs[i].velocity;
            for (u32 s = 1; s <= kSteps; ++s) {
                const f32 t = static_cast<f32>(s) / static_cast<f32>(kSteps);
                const Vec2 tp = Vec2{ p.x - v.x * trail_sec * t,
                                       p.y - v.y * trail_sec * t };
                const f32 a = (1.0f - t) * 0.65f;
                sb.DrawRect(tp.x - kBulletRadius, tp.y - kBulletRadius, sz, sz,
                            Vec4{ kColorBullet.x, kColorBullet.y, kColorBullet.z,
                                  kColorBullet.w * a });
            }
            sb.DrawRect(p.x - kBulletRadius, p.y - kBulletRadius, sz, sz, kColorBullet);
        }
    }

    // パーティクル
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

    // player
    {
        const Vec2 p = _player_node->Local().position;
        const Vec4 col = _health.IsInvulnerable(_player_health)
                            ? kColorPlayerHurt : kColorPlayer;
        const f32 sz = kPlayerRadius * 2.0f;
        sb.DrawRect(p.x - kPlayerRadius, p.y - kPlayerRadius, sz, sz, col);
    }

    // ===== HUD layer =====
    sb.SetView(static_cast<f32>(sw) * 0.5f, static_cast<f32>(sh) * 0.5f, 1.0f);

    // HP bar (左上)
    {
        const f32 frac = _health.GetHpFraction(_player_health);
        sb.DrawRect(20.0f, 20.0f, 240.0f, 24.0f, Vec4{0.1f, 0.1f, 0.1f, 0.7f});
        sb.DrawRect(24.0f, 24.0f, 232.0f, 16.0f, Vec4{0.25f, 0.05f, 0.05f, 0.85f});
        sb.DrawRect(24.0f, 24.0f, 232.0f * frac, 16.0f,
                    Vec4{0.95f, 0.20f, 0.20f, 1.0f});
    }

    // Score bar (右上)
    {
        const u64 sc = _score.CurrentScore();
        const u32 bars = static_cast<u32>(sc / 10u);
        const u32 capped = bars < 24u ? bars : 24u;
        const f32 right = static_cast<f32>(sw) - 20.0f;
        sb.DrawRect(right - 240.0f, 20.0f, 240.0f, 24.0f, Vec4{0.1f, 0.1f, 0.1f, 0.7f});
        for (u32 i = 0; i < capped; ++i) {
            sb.DrawRect(right - 232.0f + static_cast<f32>(i) * 9.5f, 24.0f, 8.0f, 16.0f,
                        Vec4{1.0f, 0.85f, 0.20f, 1.0f});
        }
    }

    // Wave 番号 (中央上)
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

    // Flash overlay
    {
        const f32 fi = _fx.FlashIntensity();
        if (fi > 0.001f) {
            const Vec3 fc = _fx.FlashColor();
            sb.DrawRect(0.0f, 0.0f, static_cast<f32>(sw), static_cast<f32>(sh),
                        Vec4{fc.x, fc.y, fc.z, fi});
        }
    }

    // ----- HUD テキストラベル -----
    if (app.FontReady()) {
        Font& body = app.FontBody();
        char buf[64];

        const f32 hp_cur = _health.GetCurrentHp(_player_health);
        std::snprintf(buf, sizeof(buf), "HP %.0f / %.0f",
                      static_cast<double>(hp_cur), static_cast<double>(kPlayerHp));
        sb.DrawString(body, buf, 28.0f, 50.0f, Vec4{1, 1, 1, 1});

        const u64 sc = _score.CurrentScore();
        std::snprintf(buf, sizeof(buf), "Score: %llu",
                      static_cast<unsigned long long>(sc));
        const f32 sw_w = body.MeasureWidth(buf);
        sb.DrawString(body, buf, static_cast<f32>(sw) - 20.0f - sw_w, 50.0f,
                      Vec4{1, 1, 0.4f, 1});

        std::snprintf(buf, sizeof(buf), "Wave %u / %u",
                      _waves.CurrentWaveIndex() + 1u, _waves.TotalWaves());
        const f32 ww = body.MeasureWidth(buf);
        sb.DrawString(body, buf,
                      static_cast<f32>(sw) * 0.5f - ww * 0.5f, 50.0f,
                      Vec4{0.5f, 0.8f, 1, 1});

        // FPS (左下、診断用)
        std::snprintf(buf, sizeof(buf), "FPS: %.0f  (dt: %.1f ms)",
                      static_cast<double>(_fps_ema),
                      static_cast<double>(_last_dt * 1000.0f));
        const Vec4 fps_col = (_fps_ema >= 55.0f) ? Vec4{0.5f, 1, 0.5f, 1}
                            : (_fps_ema >= 30.0f) ? Vec4{1, 1, 0.4f, 1}
                                                   : Vec4{1, 0.4f, 0.4f, 1};
        sb.DrawString(body, buf, 20.0f, static_cast<f32>(sh) - 30.0f, fps_col);
    }

    sb.End();
}

void GameplayScene::SpawnEnemy(Vec2 pos) noexcept {
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
    const u32 edge = static_cast<u32>(self->_rng.RangeInt(0, 3));
    Vec2 pos{0.0f, 0.0f};
    const f32 margin = 1.0f;
    switch (edge) {
    case 0:
        pos.x = self->_rng.RangeF32(-kWorldHalfW + margin, kWorldHalfW - margin);
        pos.y =  kWorldHalfH - margin;
        break;
    case 1:
        pos.x = self->_rng.RangeF32(-kWorldHalfW + margin, kWorldHalfW - margin);
        pos.y = -kWorldHalfH + margin;
        break;
    case 2:
        pos.x = -kWorldHalfW + margin;
        pos.y = self->_rng.RangeF32(-kWorldHalfH + margin, kWorldHalfH - margin);
        break;
    default:
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
    for (u32 i = 0; i < kMaxEnemies; ++i) {
        const Enemy& e = self->_enemies[i];
        if (!e.alive || e.node == nullptr) continue;
        const Vec2 ep = e.node->Local().position;
        const f32 dx = p.position.x - ep.x;
        const f32 dy = p.position.y - ep.y;
        const f32 d2 = dx*dx + dy*dy;
        const f32 r  = kEnemyRadius + kBulletRadius;
        if (d2 <= r * r) {
            out_target = i;
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

    const bool lethal = self->_health.ApplyDamage(e.hp, dmg, EDamageType::Physical);
    if (lethal) {
        const Vec2 ep = e.node ? e.node->Local().position : Vec2{0.0f, 0.0f};
        self->TriggerHitEffect(ep);
        self->_score.NotifyHit();
        self->_score.AddScore("enemy.basic", 10);
        self->_waves.NotifyEnemyKilled("enemy_basic");

        self->_health.Despawn(e.hp);
        if (self->HasServices()) self->Services().Physics().Remove(e.shape);
        if (e.node) e.node->Destroy();
        e.alive = false;
        e.node  = nullptr;
    } else {
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
    auto& app = static_cast<FullGameApp&>(GetGame());
    IRhiSwapchain* sc = app.GetRenderer().Swapchain();
    if (!sc) return _player_node->Local().position;
    const u32 sw = sc->Width();
    const u32 sh = sc->Height();
    return Services().Camera().ScreenToWorld(m, sw, sh);
}

} // namespace hellofg
