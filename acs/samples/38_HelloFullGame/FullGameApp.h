// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — CGame 派生のアプリケーションクラス。
// CAudioDirector / CMusicDirector / CGameFlow / TSaveSlot<FHighScore> /
// CSpriteBatch / FFont を共有資産として保持し、各 scene からアクセスさせる。
#pragma once

#include "gameframework/GameFramework.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "GameTypes.h"

namespace hellofg {

class CFullGameApp : public acs::game::CGame {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnShutdown() noexcept override;

    // ----- 共通サービスへのアクセサ -----
    acs::game::CAudioDirector& Audio()   noexcept { return m_Audio; }
    acs::game::CMusicDirector& Music()   noexcept { return m_Music; }
    acs::game::CGameFlow&      Flow()    noexcept { return m_Flow; }
    acs::CSpriteBatch&         Sprites() noexcept { return m_Sprites; }
    bool                      SpritesReady() const noexcept { return m_SpriteInitialized; }

    // CSpriteBatch + FFont を初回 OnRender で 1 度だけ初期化。Init は CRenderer
    // がフレームを始めて Device が生きてからでないと安全に呼べないため遅延。
    void EnsureSpritesInitialized() noexcept;

    acs::FFont& FontTitle() noexcept { return m_FontTitle; }
    acs::FFont& FontBody()  noexcept { return m_FontBody; }
    bool       FontReady() const noexcept { return m_FontInitialized; }

    FHighScore&                       GetHighScore()    noexcept { return m_Highscore; }
    acs::game::TSaveSlot<FHighScore>&  HighScoreSlot()   noexcept { return m_HighscoreSlot; }
    void SaveHighScoreIfBetter(acs::u64 final_score) noexcept;

protected:
    acs::TUniquePtr<acs::game::AScene> InitialScene() noexcept override;

private:
    acs::game::CAudioDirector            m_Audio;
    acs::game::CMusicDirector            m_Music;
    acs::game::CGameFlow                 m_Flow;
    acs::game::TSaveSlot<FHighScore>      m_HighscoreSlot;
    FHighScore                           m_Highscore{};
    acs::CSpriteBatch                    m_Sprites;
    acs::FFont                           m_FontTitle;
    acs::FFont                           m_FontBody;
    bool                                m_SpriteInitialized = false;
    bool                                m_FontInitialized   = false;
};

} // namespace hellofg
