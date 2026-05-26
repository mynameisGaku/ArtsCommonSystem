// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — GameOverScene 実装。
#include "GameOverScene.h"
#include "FullGameApp.h"
#include "TitleScene.h"

#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "platform/Input.h"
#include "foundation/Log.h"
#include "math/Math.h"

#include <cstdio>

using namespace acs;
using namespace acs::game;

namespace hellofg {

void GameOverScene::OnEnter() noexcept {
    FInputMap& im = Services().Input();
    im.ClearAll();
    im.BindKey(FActionId("Reset"), EKey::R);
    im.BindKey(FActionId("Quit"),  EKey::Escape);

    auto& app = static_cast<FullGameApp&>(GetGame());
    _saved_best    = app.GetHighScore().best_score;
    _is_new_record = _final_score >= _saved_best && _final_score > 0;

    GetGame().SetClearColor(_did_win ? 0.06f : 0.18f,
                            _did_win ? 0.16f : 0.04f,
                            _did_win ? 0.06f : 0.04f);
    ACS_LOG_INFO("[GameOver] enter - %s, score=%llu, best=%llu",
                 _did_win ? "VICTORY" : "DEFEAT",
                 static_cast<unsigned long long>(_final_score),
                 static_cast<unsigned long long>(_saved_best));
}

void GameOverScene::OnExit() noexcept {
    ACS_LOG_INFO("[GameOver] exit");
}

void GameOverScene::OnUpdate(f32 dt) noexcept {
    const FInputMap& im = Services().Input();
    if (im.IsPressed(FActionId("Quit"))) {
        GetGame().Quit();
        return;
    }
    if (im.IsPressed(FActionId("Reset"))) {
        Scenes().ChangeScene(MakeUnique<TitleScene>());
        return;
    }
    _state_sec += dt;
}

void GameOverScene::OnRender(FRenderContext& rc) noexcept {
    auto& app = static_cast<FullGameApp&>(GetGame());
    app.EnsureSpritesInitialized();
    if (!app.SpritesReady()) return;

    FSpriteBatch& sb = app.Sprites();
    const u32 sw = rc.Width();
    const u32 sh = rc.Height();
    sb.Begin(rc.Cmd(), sw, sh);

    const f32 cx = static_cast<f32>(sw) * 0.5f;
    const f32 cy = static_cast<f32>(sh) * 0.35f;

    // 結果表示の大バー
    const FVec4 result_col = _did_win ? FVec4{0.20f, 0.85f, 0.40f, 0.95f}
                                     : FVec4{0.85f, 0.20f, 0.20f, 0.95f};
    sb.DrawRect(cx - 400.0f, cy - 50.0f, 800.0f, 100.0f,
                FVec4{0.05f, 0.05f, 0.05f, 0.85f});
    sb.DrawRect(cx - 380.0f, cy - 30.0f, 760.0f, 60.0f, result_col);

    // 最終 score バー
    const f32 score_y = cy + 110.0f;
    sb.DrawRect(cx - 240.0f, score_y, 480.0f, 30.0f, FVec4{0.05f, 0.05f, 0.05f, 0.8f});
    const u64 cap = _final_score < 240ULL ? _final_score : 240ULL;
    sb.DrawRect(cx - 234.0f, score_y + 5.0f, static_cast<f32>(cap) * 2.0f, 20.0f,
                FVec4{1.0f, 0.85f, 0.20f, 1.0f});

    // High score バー
    const f32 best_y = score_y + 60.0f;
    sb.DrawRect(cx - 240.0f, best_y, 480.0f, 30.0f, FVec4{0.05f, 0.05f, 0.05f, 0.8f});
    f32 best_alpha = 1.0f;
    if (_is_new_record) {
        best_alpha = 0.5f + 0.5f * Sin(_state_sec * 6.0f);
    }
    const u64 best = _saved_best;
    const u64 best_cap = best < 240ULL ? best : 240ULL;
    sb.DrawRect(cx - 234.0f, best_y + 5.0f, static_cast<f32>(best_cap) * 2.0f, 20.0f,
                FVec4{0.30f, 0.55f, 1.00f, best_alpha});

    // 操作ヒント
    sb.DrawRect(cx - 220.0f, static_cast<f32>(sh) - 80.0f, 200.0f, 40.0f,
                FVec4{0.85f, 0.85f, 0.85f, 0.7f});
    sb.DrawRect(cx +  20.0f, static_cast<f32>(sh) - 80.0f, 200.0f, 40.0f,
                FVec4{0.50f, 0.50f, 0.50f, 0.7f});

    // ----- テキストラベル -----
    if (app.FontReady()) {
        FFont& title_font = app.FontTitle();
        FFont& body       = app.FontBody();

        const char* result_text = _did_win ? "VICTORY!" : "GAME OVER";
        const f32 rw = title_font.MeasureWidth(result_text);
        sb.DrawString(title_font, result_text, cx - rw * 0.5f, cy - 18.0f,
                      FVec4{1, 1, 1, 1});

        char buf[64];
        std::snprintf(buf, sizeof(buf), "Final Score: %llu",
                      static_cast<unsigned long long>(_final_score));
        const f32 fw = body.MeasureWidth(buf);
        sb.DrawString(body, buf, cx - fw * 0.5f, score_y + 7.0f,
                      FVec4{0.1f, 0.1f, 0.1f, 1});

        std::snprintf(buf, sizeof(buf), "%sBest: %llu",
                      _is_new_record ? "[NEW!] " : "",
                      static_cast<unsigned long long>(_saved_best));
        const f32 bw = body.MeasureWidth(buf);
        sb.DrawString(body, buf, cx - bw * 0.5f, best_y + 7.0f,
                      FVec4{0.1f, 0.1f, 0.1f, best_alpha});

        const char* kR  = "R: Title";
        const char* kEs = "Esc: Quit";
        const f32 rrw = body.MeasureWidth(kR);
        const f32 erw = body.MeasureWidth(kEs);
        sb.DrawString(body, kR,
                      cx - 220.0f + (200.0f - rrw) * 0.5f,
                      static_cast<f32>(sh) - 70.0f,
                      FVec4{0.1f, 0.1f, 0.1f, 1});
        sb.DrawString(body, kEs,
                      cx +  20.0f + (200.0f - erw) * 0.5f,
                      static_cast<f32>(sh) - 70.0f,
                      FVec4{0.1f, 0.1f, 0.1f, 1});
    }

    sb.End();
}

} // namespace hellofg
