// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — FTitleScene 実装。
#include "TitleScene.h"
#include "FullGameApp.h"
#include "GameplayScene.h"

#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "platform/Input.h"
#include "foundation/Log.h"
#include "math/Math.h"

#include <cstdio>

using namespace acs;
using namespace acs::game;

namespace hellofg {

void FTitleScene::OnEnter() noexcept {
    GetGame().SetClearColor(m_BgColor.x, m_BgColor.y, m_BgColor.z);

    // 背景色を 2 秒 ping-pong する FTween (Services 経由で自動 tick される)
    m_BgTween = Services().Tweens().Tween(
        &m_BgColor, kBgDark, kBgBright, /*duration=*/2.0f, Easing::InOutSine);
    m_bToBright = true;

    // 入力バインド
    FInputMap& im = Services().Input();
    im.ClearAll();
    im.BindKey(FActionId("Start"), EKey::Space);
    im.BindKey(FActionId("Quit"),  EKey::Escape);

    // BGM 切替 (state-only、ログのみ出る)
    auto& app = static_cast<FFullGameApp&>(GetGame());
    app.Music().SetState(EMusicState::Calm, 1.0f);
    app.Audio().PlayBgm("bgm_title", 1.0f, true);

    ACS_LOG_INFO("[Title] enter - Press Space to start, Esc to quit");
}

void FTitleScene::OnExit() noexcept {
    if (HasServices()) Services().Tweens().CancelAll();
    ACS_LOG_INFO("[Title] exit");
}

void FTitleScene::OnUpdate(f32 dt) noexcept {
    const FInputMap& im = Services().Input();
    if (im.IsPressed(FActionId("Quit"))) {
        GetGame().Quit();
        return;
    }
    if (im.IsPressed(FActionId("Start"))) {
        Scenes().ChangeScene(MakeUnique<FGameplayScene>());
        return;
    }

    // ping-pong 完了で逆向き再開
    if (!Services().Tweens().IsActive(m_BgTween)) {
        m_bToBright = !m_bToBright;
        const FVec3 to = m_bToBright ? kBgBright : kBgDark;
        m_BgTween = Services().Tweens().Tween(
            &m_BgColor, m_BgColor, to, /*duration=*/2.0f, Easing::InOutSine);
    }
    GetGame().SetClearColor(m_BgColor.x, m_BgColor.y, m_BgColor.z);

    m_PulseSec += dt;
}

void FTitleScene::OnRender(FRenderContext& rc) noexcept {
    auto& app = static_cast<FFullGameApp&>(GetGame());
    app.EnsureSpritesInitialized();
    if (!app.SpritesReady()) return;

    FSpriteBatch& sb = app.Sprites();
    const u32 sw = rc.Width();
    const u32 sh = rc.Height();
    sb.Begin(rc.Cmd(), sw, sh);

    // タイトル板 - 中央に大きめ矩形を 2 段重ね
    const f32 cx = static_cast<f32>(sw) * 0.5f;
    const f32 cy = static_cast<f32>(sh) * 0.4f;
    sb.DrawRect(cx - 360.0f, cy - 60.0f, 720.0f, 120.0f,
                FVec4{0.08f, 0.10f, 0.20f, 0.85f});
    sb.DrawRect(cx - 340.0f, cy - 40.0f, 680.0f, 14.0f, FVec4{1.0f, 0.85f, 0.20f, 0.9f});
    sb.DrawRect(cx - 340.0f, cy + 26.0f, 680.0f, 14.0f, FVec4{0.30f, 0.55f, 1.00f, 0.9f});

    // "Press Space" の点滅板。Sin で 0..1 を作って α に流す
    const f32 alpha = 0.35f + 0.45f * (0.5f + 0.5f * Sin(m_PulseSec * 3.5f));
    sb.DrawRect(cx - 220.0f, cy + 120.0f, 440.0f, 36.0f,
                FVec4{0.95f, 0.95f, 0.95f, alpha});

    // FHighScore があれば右下に表示する小バー
    const FHighScore& hs = app.GetHighScore();
    if (hs.best_score > 0) {
        sb.DrawRect(static_cast<f32>(sw) - 260.0f, static_cast<f32>(sh) - 50.0f,
                    240.0f, 30.0f, FVec4{0.1f, 0.1f, 0.1f, 0.7f});
        sb.DrawRect(static_cast<f32>(sw) - 254.0f, static_cast<f32>(sh) - 44.0f,
                    228.0f, 18.0f, FVec4{1.0f, 0.85f, 0.20f, 0.9f});
    }

    // ----- テキストラベル -----
    if (app.FontReady()) {
        FFont& title_font = app.FontTitle();
        FFont& body_font  = app.FontBody();
        const char* kTitle = "ACS Hello Full Game";
        const f32 tw = title_font.MeasureWidth(kTitle);
        sb.DrawString(title_font, kTitle, cx - tw * 0.5f, cy - 18.0f,
                      FVec4{1.0f, 1.0f, 1.0f, 1.0f});

        const char* kPress = "Press Space to Start";
        const f32 pw = body_font.MeasureWidth(kPress);
        sb.DrawString(body_font, kPress, cx - pw * 0.5f, cy + 128.0f,
                      FVec4{1.0f, 1.0f, 1.0f, alpha});

        const char* kHelp = "WASD: Move    Mouse: Aim    LMB: Fire    Esc: Quit";
        const f32 hw = body_font.MeasureWidth(kHelp);
        sb.DrawString(body_font, kHelp, cx - hw * 0.5f, static_cast<f32>(sh) - 60.0f,
                      FVec4{0.8f, 0.8f, 0.85f, 0.9f});

        if (hs.best_score > 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Best: %llu",
                          static_cast<unsigned long long>(hs.best_score));
            sb.DrawString(body_font, buf,
                          static_cast<f32>(sw) - 250.0f,
                          static_cast<f32>(sh) - 47.0f,
                          FVec4{0.1f, 0.1f, 0.1f, 1.0f});
        }
    }

    sb.End();
}

} // namespace hellofg
