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
    FInputMap& im = Services().Input();
    im.ClearAll();
    im.BindAxisKeys(ActionId("MoveX"), EKey::A, EKey::D);
    im.BindAxisKeys(ActionId("MoveY"), EKey::W, EKey::S);  // W=画面上=-Y (Y-down: 画面上ほど小さい Y)
    im.BindMouseButton(ActionId("Fire"), EMouseButton::Left);
    im.BindKey(ActionId("Pause"), EKey::P);
    im.BindKey(ActionId("Quit"),  EKey::Escape);

    // ----- subsystem 初期化 -----
    m_Health.ClearAll();
    m_Score.Init();
    m_Score.SetComboDuration(2.5f);

    m_BulletsMod.Init(m_Bullets);
    m_Bullets.SetHitTestFn(&ProjectileOnHitTest, this);
    m_Bullets.SetOnHitCallback(&ProjectileOnHit, this);

    m_Particles.Init(kMaxParticles);
    m_Waves.Init();
    m_Waves.SetOnSpawnCallback(&WaveOnSpawn, this);
    m_Waves.SetOnWaveStateChangeCallback(&WaveOnState, this);

    // ----- collision world -----
    FCollisionWorld2D& phy = Services().Physics();
    phy.Init(2.0f);

    // ----- camera -----
    FCamera2D& cam = Services().Camera();
    cam.SetPosition(FVec2{0.0f, 0.0f});
    cam.SetZoom(kWorldUnit);
    cam.SetShakeAmplitude(0.4f);
    cam.SetShakeDecayRate(2.0f);
    cam.SetBounds(FVec2{-kWorldHalfW + 6.0f, -kWorldHalfH + 4.0f},
                  FVec2{ kWorldHalfW - 6.0f,  kWorldHalfH - 4.0f});

    // ----- floor tilemap -----
    const u32 tw = static_cast<u32>(kWorldHalfW * 2.0f);
    const u32 th = static_cast<u32>(kWorldHalfH * 2.0f);
    m_Floor.Init(tw, th, /*layer=*/1, /*tile_size=*/1.0f);
    // チェッカー柄 (隣接タイルで色が交互になるよう ((x+y)&1)+1 をタイル ID にする)。
    for (u32 y = 0; y < th; ++y) {
        for (u32 x = 0; x < tw; ++x) {
            const u16 t = static_cast<u16>(((x + y) & 1) + 1);
            m_Floor.SetTile(x, y, FTileId{t}, 0);
        }
    }

    // ----- 各機能モジュールの init -----
    m_Player.Init(*this, m_Root, m_Health);
    m_Enemies.Reset();
    m_HitEffects.Init(m_Particles);

    // ----- perception -----
    SenseConfig sc;
    sc.sight_range   = 10.0f;
    sc.sight_fov_rad = kPi;
    sc.hearing_range = 8.0f;
    m_Perception.SetConfig(sc);
    m_Perception.AddTarget(/*id=*/1u, FVec2{0.0f, 0.0f});

    // ----- wave 定義 (難易度カーブ: 5+2*i 体、間隔は固定) -----
    for (u32 i = 0; i < kTotalWaves; ++i) {
        m_WaveRules[i].enemy_id           = "enemy_basic";
        m_WaveRules[i].count              = 5u + i * 2u;
        m_WaveRules[i].spawn_interval_sec = kSpawnIntervalSec;
        m_WaveRules[i].initial_delay_sec  = 0.8f;
        m_WaveRules[i].spawn_position     = FVec2{0.0f, 0.0f};
        m_WaveDefs[i].wave_id               = "wave";
        m_WaveDefs[i].rule_count            = 1;
        m_WaveDefs[i].rules                 = &m_WaveRules[i];
        m_WaveDefs[i].wave_intermission_sec = kIntermissionSec;
        m_Waves.AddWave(m_WaveDefs[i]);
    }
    m_Waves.StartWaves();

    // ----- BGM -----
    auto& app = static_cast<FullGameApp&>(GetGame());
    app.Music().SetState(EMusicState::Combat, 1.5f);
    app.Audio().PlayBgm("bgm_gameplay", 1.5f, true);

    GetGame().SetClearColor(0.04f, 0.05f, 0.06f);
    ACS_LOG_INFO("[Gameplay] enter - WASD move, Mouse aim, LMB fire");
}

void GameplayScene::OnExit() noexcept {
    // 敵 → player の順で root の Node を Destroy。ResolveStructuralChanges でまとめ反映。
    m_Enemies.Shutdown();
    m_Player.Shutdown();
    m_Root.ResolveStructuralChanges();

    m_BulletsMod.Shutdown(m_Bullets);
    m_HitEffects.Shutdown(m_Particles);

    if (HasServices()) {
        Services().Physics().ClearAll();
        Services().Tweens().CancelAll();
    }
    ACS_LOG_INFO("[Gameplay] exit");
}

void GameplayScene::OnUpdate(f32 dt) noexcept {
    // ----- FPS 計測 (EMA 0.1 で平滑化、HUD に出す) -----
    m_LastDt = dt;
    if (dt > 0.0f) {
        const f32 inst_fps = 1.0f / dt;
        m_FpsEma = m_FpsEma * 0.9f + inst_fps * 0.1f;
    }

    const FInputMap& im = Services().Input();
    if (im.IsPressed(ActionId("Quit"))) {
        GetGame().Quit();
        return;
    }

    // ----- player 入力 + 射撃 -----
    const FVec2 player_pos = m_Player.UpdateMovement(*this, dt);
    m_Perception.UpdateTarget(1u, player_pos);
    Services().Camera().SetTargetPos(player_pos, 6.0f);
    m_Player.UpdateFire(*this, dt);

    // ----- AI (敵全員でプレイヤー追跡 + 接触ダメージ判定) -----
    m_Perception.SetEyePos(player_pos, FVec2{1.0f, 0.0f});
    m_Perception.Tick(dt);
    const bool lethal_contact = m_Enemies.TickChaseAndContact(*this, m_Health, player_pos, dt);
    if (lethal_contact) {
        RequestGameOver(/*victory=*/false);
        return;
    }

    // ----- subsystem の per-frame tick -----
    m_Health.Tick(dt);
    m_Score.Tick(dt);
    m_Bullets.Tick(dt);
    m_Particles.Tick(dt);
    m_HitEffects.Tick(*this, dt);    // camera shake もここで吸う
    m_Waves.Tick(dt);
}

void GameplayScene::OnFixedUpdate(f32 /*dt*/) noexcept {
    // 固定 step は FPhysicsBody2D の決定性のため FGame 側で 1/60 に固定済み。
    m_Root.FixedUpdateTree(1.0f / 60.0f);
    m_Root.ResolveStructuralChanges();
}

void GameplayScene::OnRender(RenderContext& rc) noexcept {
    auto& app = static_cast<FullGameApp&>(GetGame());
    app.EnsureSpritesInitialized();
    if (!app.SpritesReady()) return;

    FSpriteBatch& sb = app.Sprites();
    const u32 sw = rc.Width();
    const u32 sh = rc.Height();
    sb.Begin(rc.Cmd(), sw, sh);

    // ===== world layer (camera 投影) =====
    const FVec2 cam_pos = Services().Camera().EffectiveViewCenter();
    sb.SetView(cam_pos.x, cam_pos.y, kWorldUnit);

    // floor tiles (チェッカー柄)
    const f32 ts   = m_Floor.TileSize();
    const u32 tw   = m_Floor.Width();
    const u32 th   = m_Floor.Height();
    const f32 ox   = -kWorldHalfW;
    const f32 oy   = -kWorldHalfH;
    const FTileId* layer0 = m_Floor.LayerData(0);
    if (layer0) {
        for (u32 y = 0; y < th; ++y) {
            for (u32 x = 0; x < tw; ++x) {
                const FTileId t = layer0[y * tw + x];
                if (t.IsEmpty()) continue;
                const FVec4 col = (t.value == 1) ? kColorTileLight : kColorTileDark;
                sb.DrawRect(ox + static_cast<f32>(x) * ts,
                            oy + static_cast<f32>(y) * ts,
                            ts * 0.96f, ts * 0.96f, col);
            }
        }
    }

    // 敵 / 弾 / パーティクル / プレイヤーの順で重ね描き。
    m_Enemies.DrawAll(sb);
    m_BulletsMod.DrawAll(m_Bullets, sb, m_LastDt);
    m_HitEffects.DrawParticles(m_Particles, sb);

    if (m_Player.Node()) {
        const FVec2 p = m_Player.Position();
        const FVec4 col = m_Player.IsInvulnerable(m_Health) ? kColorPlayerHurt : kColorPlayer;
        const f32 sz = kPlayerRadius * 2.0f;
        sb.DrawRect(p.x - kPlayerRadius, p.y - kPlayerRadius, sz, sz, col);
    }

    // ===== HUD layer (ピクセル座標、camera 無関係) =====
    sb.SetView(static_cast<f32>(sw) * 0.5f, static_cast<f32>(sh) * 0.5f, 1.0f);
    m_Hud.Draw(*this, sb, sw, sh, m_LastDt, m_FpsEma);

    sb.End();
}

// ----- モジュール連携 -----

void GameplayScene::OnPlayerHurt() noexcept {
    m_HitEffects.TriggerPlayerHurt(*this, m_Player.Position());
}

void GameplayScene::OnEnemyKilled() noexcept {
    m_Score.NotifyHit();
    m_Score.AddScore("enemy.basic", 10);
    m_Waves.NotifyEnemyKilled("enemy_basic");
}

void GameplayScene::RequestGameOver(bool victory) noexcept {
    if (m_bGameOverReq) return;       // 多重遷移ガード
    m_bGameOverReq = true;
    m_Victory = victory;

    auto& app = static_cast<FullGameApp&>(GetGame());
    app.SaveHighScoreIfBetter(m_Score.CurrentScore());
    app.Music().SetState(victory ? EMusicState::Victory : EMusicState::GameOver, 1.0f);
    app.Audio().PlayBgm(victory ? "bgm_victory" : "bgm_gameover",
                        1.0f, victory ? false : true);
    Scenes().ChangeScene(MakeUnique<GameOverScene>(m_Score.CurrentScore(), victory));
}

FVec2 GameplayScene::MouseWorld() const noexcept {
    const FVec2 m = Input::MousePos();
    auto& app = static_cast<FullGameApp&>(GetGame());
    IRhiSwapchain* sc = app.GetRenderer().Swapchain();
    if (!sc) {
        // swapchain が出てくる前のごく初期フレームでは player 座標を返してフリーズ防止。
        return m_Player.Node() ? m_Player.Node()->Local().position : FVec2{0.0f, 0.0f};
    }
    return Services().Camera().ScreenToWorld(m, sc->Width(), sc->Height());
}

} // namespace hellofg
