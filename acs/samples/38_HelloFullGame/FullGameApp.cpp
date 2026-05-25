// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — FullGameApp 実装。
#include "FullGameApp.h"
#include "TitleScene.h"

#include "render/Renderer.h"
#include "render/IRhiSwapchain.h"
#include "app/Sample.h"
#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellofg {

void FullGameApp::OnStart() noexcept {
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

void FullGameApp::OnUpdate(f32 dt) noexcept {
    // Game ループに乗せる前に AudioDirector / MusicDirector / GameFlow を tick。
    _audio.Tick(dt);
    _music.Tick(dt);
    _flow.Tick(dt);
    Game::OnUpdate(dt);
}

void FullGameApp::OnShutdown() noexcept {
    if (_sprite_initialized) {
        if (auto* dev = GetRenderer().Device()) dev->WaitIdle();
        _sprites.Shutdown();
    }
    if (_font_initialized) {
        _font_title.Shutdown();
        _font_body.Shutdown();
    }
    Game::OnShutdown();
}

void FullGameApp::EnsureSpritesInitialized() noexcept {
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

    // Font も同じタイミングで遅延 init (Device 必須)。
    Sample::TryLoadDefaultUIFont(_font_title, *dev, 36.0f, 1024, false);
    Sample::TryLoadDefaultUIFont(_font_body,  *dev, 18.0f, 1024, false);
    _font_initialized = true;
}

void FullGameApp::SaveHighScoreIfBetter(u64 final_score) noexcept {
    if (final_score <= _highscore.best_score) return;
    _highscore.best_score = final_score;
    _highscore.timestamp  = static_cast<u64>(0);
    auto r = _highscore_slot.Save(_highscore);
    if (r.IsErr()) {
        ACS_LOG_WARN("[FullGame] HighScore save failed: %s", r.Error().message);
    } else {
        ACS_LOG_INFO("[FullGame] HighScore saved (best=%llu)",
                     static_cast<unsigned long long>(_highscore.best_score));
    }
}

UniquePtr<Scene> FullGameApp::InitialScene() noexcept {
    return MakeUnique<TitleScene>();
}

} // namespace hellofg
