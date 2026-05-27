// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — FGame 派生のアプリケーションクラス。
// FAudioDirector / FMusicDirector / FGameFlow / FSaveSlot<HighScore> /
// FSpriteBatch / Font を共有資産として保持し、各 scene からアクセスさせる。
#pragma once

#include "gameframework/GameFramework.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "GameTypes.h"

namespace hellofg {

class FullGameApp : public acs::game::FGame {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnShutdown() noexcept override;

    // ----- 共通サービスへのアクセサ -----
    acs::game::FAudioDirector& Audio()   noexcept { return _audio; }
    acs::game::FMusicDirector& Music()   noexcept { return _music; }
    acs::game::FGameFlow&      Flow()    noexcept { return _flow; }
    acs::FSpriteBatch&         Sprites() noexcept { return _sprites; }
    bool                      SpritesReady() const noexcept { return _sprite_initialized; }

    // FSpriteBatch + Font を初回 OnRender で 1 度だけ初期化。Init は FRenderer
    // がフレームを始めて Device が生きてからでないと安全に呼べないため遅延。
    void EnsureSpritesInitialized() noexcept;

    acs::Font& FontTitle() noexcept { return _font_title; }
    acs::Font& FontBody()  noexcept { return _font_body; }
    bool       FontReady() const noexcept { return _font_initialized; }

    HighScore&                       GetHighScore()    noexcept { return _highscore; }
    acs::game::FSaveSlot<HighScore>&  HighScoreSlot()   noexcept { return _highscore_slot; }
    void SaveHighScoreIfBetter(acs::u64 final_score) noexcept;

protected:
    acs::TUniquePtr<acs::game::Scene> InitialScene() noexcept override;

private:
    acs::game::FAudioDirector            _audio;
    acs::game::FMusicDirector            _music;
    acs::game::FGameFlow                 _flow;
    acs::game::FSaveSlot<HighScore>      _highscore_slot;
    HighScore                           _highscore{};
    acs::FSpriteBatch                    _sprites;
    acs::Font                           _font_title;
    acs::Font                           _font_body;
    bool                                _sprite_initialized = false;
    bool                                _font_initialized   = false;
};

} // namespace hellofg
