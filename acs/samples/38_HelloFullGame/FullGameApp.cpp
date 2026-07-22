// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — FFullGameApp 実装。
#include "FullGameApp.h"
#include "TitleScene.h"

#include "render/Renderer.h"
#include "render/IRhiSwapchain.h"
#include "app/Sample.h"
#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellofg {

void FFullGameApp::OnStart() noexcept {
    // 1) BGM トラック登録 (state-only、backend 未接続なのでログのみ)
    m_Music.RegisterTrack(EMusicState::Calm,
        FMusicTrack{ /*asset=*/"bgm_title.ogg",    0.0f, 1.0f, true });
    m_Music.RegisterTrack(EMusicState::Combat,
        FMusicTrack{ /*asset=*/"bgm_gameplay.ogg", 0.0f, 1.0f, true });
    m_Music.RegisterTrack(EMusicState::GameOver,
        FMusicTrack{ /*asset=*/"bgm_gameover.ogg", 0.0f, 1.0f, true });
    m_Music.RegisterTrack(EMusicState::Victory,
        FMusicTrack{ /*asset=*/"bgm_victory.ogg",  0.0f, 1.0f, false });

    // 2) FAudioDirector volume バス初期化 (backend 未接続でも state はもつ)
    m_Audio.SetMasterVolume(0.8f);
    m_Audio.SetBgmVolume(0.7f);
    m_Audio.SetSfxVolume(0.9f);

    // 3) TSaveSlot 初期化 + 既存 FHighScore のロード
    m_HighscoreSlot.Init(kSaveFile);
    if (m_HighscoreSlot.Exists()) {
        auto r = m_HighscoreSlot.Load();
        if (r.IsOk()) {
            m_Highscore = r.Value();
            ACS_LOG_INFO("[FullGame] FHighScore loaded: best=%llu",
                         static_cast<unsigned long long>(m_Highscore.best_score));
        } else {
            ACS_LOG_WARN("[FullGame] FHighScore load failed: %s", r.Error().message);
        }
    } else {
        ACS_LOG_INFO("[FullGame] No FHighScore file yet (first run)");
    }

    // 4) FGameFlow を Splash → MainTitle (1 step) で起動
    m_Flow.Init(EFlowState::Splash);
    m_Flow.RequestTransition(EFlowState::MainTitle, 0.0f);

    // 5) 固定 step を明示 (APhysicsBody2D の決定性のため)
    SetFixedTimestep(1.0f / 60.0f, /*max_steps_per_frame=*/8);

    // 6) 基底に初期 Scene を push してもらう
    FGame::OnStart();
}

void FFullGameApp::OnUpdate(f32 dt) noexcept {
    // FGame ループに乗せる前に FAudioDirector / FMusicDirector / FGameFlow を tick。
    m_Audio.Tick(dt);
    m_Music.Tick(dt);
    m_Flow.Tick(dt);
    FGame::OnUpdate(dt);
}

void FFullGameApp::OnShutdown() noexcept {
    if (m_SpriteInitialized) {
        if (auto* dev = GetRenderer().Device()) dev->WaitIdle();
        m_Sprites.Shutdown();
    }
    if (m_FontInitialized) {
        m_FontTitle.Shutdown();
        m_FontBody.Shutdown();
    }
    FGame::OnShutdown();
}

void FFullGameApp::EnsureSpritesInitialized() noexcept
{
    if (m_SpriteInitialized) return;
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) return;
    auto r = m_Sprites.Init(*dev, GetRenderer().ColorFormat(), /*max_sprites=*/4096);
    if (r.IsErr()) {
        ACS_LOG_ERROR("[FullGame] FSpriteBatch::Init failed: %s", r.Error().message);
        return;
    }
    m_SpriteInitialized = true;
    ACS_LOG_INFO("[FullGame] FSpriteBatch initialized");

    // FFont も同じタイミングで遅延 init (Device 必須)。
    const auto title_font_result = FSample::TryLoadDefaultUIFont(m_FontTitle, *dev, 36.0f, 1024, false);
    const auto body_font_result = FSample::TryLoadDefaultUIFont(m_FontBody, *dev, 18.0f, 1024, false);
    if (title_font_result.IsErr() || body_font_result.IsErr()) {
        ACS_LOG_WARN("[FullGame] UI font initialization failed");
        m_FontTitle.Shutdown();
        m_FontBody.Shutdown();
        return;
    }
    m_FontInitialized = true;
}

void FFullGameApp::SaveHighScoreIfBetter(u64 final_score) noexcept
{
    if (final_score <= m_Highscore.best_score) return;
    m_Highscore.best_score = final_score;
    m_Highscore.timestamp = static_cast<u64>(0);
    auto r = m_HighscoreSlot.Save(m_Highscore);
    if (r.IsErr()) {
        ACS_LOG_WARN("[FullGame] FHighScore save failed: %s", r.Error().message);
    } else {
        ACS_LOG_INFO("[FullGame] FHighScore saved (best=%llu)", static_cast<unsigned long long>(m_Highscore.best_score));
    }
}

TUniquePtr<FScene> FFullGameApp::InitialScene() noexcept {
    return MakeUnique<FTitleScene>();
}

} // namespace hellofg
