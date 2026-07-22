// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — FGame 派生のアプリケーションクラス。
// FAudioDirector / FMusicDirector / FGameFlow / TSaveSlot<FHighScore> /
// FSpriteBatch / FFont を共有資産として保持し、各 scene からアクセスさせる。
#pragma once

#include "gameframework/GameFramework.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "GameTypes.h"

namespace hellofg {

class FFullGameApp : public acs::game::FGame {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnShutdown() noexcept override;

    // ----- 共通サービスへのアクセサ -----
    acs::game::FAudioDirector& Audio()   noexcept { return m_Audio; }
    acs::game::FMusicDirector& Music()   noexcept { return m_Music; }
    acs::game::FGameFlow&      Flow()    noexcept { return m_Flow; }
    acs::FSpriteBatch&         Sprites() noexcept { return m_Sprites; }
    bool                      SpritesReady() const noexcept { return m_SpriteInitialized; }

    // FSpriteBatch + FFont を初回 OnRender で 1 度だけ初期化。Init は FRenderer
    // がフレームを始めて Device が生きてからでないと安全に呼べないため遅延。
    void EnsureSpritesInitialized() noexcept;

    acs::FFont& FontTitle() noexcept { return m_FontTitle; }
    acs::FFont& FontBody()  noexcept { return m_FontBody; }
    bool       FontReady() const noexcept { return m_FontInitialized; }

    FHighScore&                       GetHighScore()    noexcept { return m_Highscore; }
    acs::game::TSaveSlot<FHighScore>&  HighScoreSlot()   noexcept { return m_HighscoreSlot; }
    void SaveHighScoreIfBetter(acs::u64 final_score) noexcept;

protected:
    acs::TUniquePtr<acs::game::FScene> InitialScene() noexcept override;

private:
    acs::game::FAudioDirector            m_Audio;
    acs::game::FMusicDirector            m_Music;
    acs::game::FGameFlow                 m_Flow;
    acs::game::TSaveSlot<FHighScore>      m_HighscoreSlot;
    FHighScore                           m_Highscore{};
    acs::FSpriteBatch                    m_Sprites;
    acs::FFont                           m_FontTitle;
    acs::FFont                           m_FontBody;
    bool                                m_SpriteInitialized = false;
    bool                                m_FontInitialized   = false;
};

} // namespace hellofg
