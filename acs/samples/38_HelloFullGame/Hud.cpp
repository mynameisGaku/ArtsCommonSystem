// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — HUD 実装。
#include "Hud.h"
#include "GameplayScene.h"
#include "FullGameApp.h"
#include "Player.h"
#include "HitEffects.h"

#include "render/SpriteBatch.h"
#include "render/Font.h"

#include <cstdio>

using namespace acs;
using namespace acs::game;

namespace hellofg {

void Hud::Draw(GameplayScene& scene, FSpriteBatch& sb, u32 sw, u32 sh,
               f32 last_dt, f32 fps_ema) const noexcept {
    auto& app    = static_cast<FullGameApp&>(scene.GetGame());
    auto& player = scene.GetPlayer();
    auto& health = scene.GetHealth();
    auto& score  = scene.GetScore();
    auto& waves  = scene.GetWaves();
    auto& fx     = scene.GetHitEffects().Fx();

    // ----- HP バー (左上) -----
    {
        const f32 frac = health.GetHpFraction(player.HealthHandle());
        sb.DrawRect(20.0f, 20.0f, 240.0f, 24.0f, FVec4{0.1f, 0.1f, 0.1f, 0.7f});
        sb.DrawRect(24.0f, 24.0f, 232.0f, 16.0f, FVec4{0.25f, 0.05f, 0.05f, 0.85f});
        sb.DrawRect(24.0f, 24.0f, 232.0f * frac, 16.0f,
                    FVec4{0.95f, 0.20f, 0.20f, 1.0f});
    }

    // ----- スコアバー (右上、score/10 を 24 個までドット表示) -----
    {
        const u64 sc = score.CurrentScore();
        const u32 bars = static_cast<u32>(sc / 10u);
        const u32 capped = bars < 24u ? bars : 24u;
        const f32 right = static_cast<f32>(sw) - 20.0f;
        sb.DrawRect(right - 240.0f, 20.0f, 240.0f, 24.0f, FVec4{0.1f, 0.1f, 0.1f, 0.7f});
        for (u32 i = 0; i < capped; ++i) {
            sb.DrawRect(right - 232.0f + static_cast<f32>(i) * 9.5f, 24.0f, 8.0f, 16.0f,
                        FVec4{1.0f, 0.85f, 0.20f, 1.0f});
        }
    }

    // ----- Wave 進捗 (中央上) -----
    {
        const u32 wave  = waves.CurrentWaveIndex();
        const u32 total = waves.TotalWaves();
        const f32 cx = static_cast<f32>(sw) * 0.5f;
        sb.DrawRect(cx - 120.0f, 20.0f, 240.0f, 24.0f, FVec4{0.1f, 0.1f, 0.1f, 0.7f});
        for (u32 i = 0; i < total; ++i) {
            const FVec4 c = (i < wave) ? FVec4{0.30f, 0.55f, 1.00f, 1.0f}
                                       : FVec4{0.20f, 0.20f, 0.25f, 1.0f};
            sb.DrawRect(cx - 116.0f + static_cast<f32>(i) * 47.5f, 24.0f, 44.0f, 16.0f, c);
        }
    }

    // ----- Flash オーバーレイ (画面全体に半透明色をかぶせる) -----
    {
        const f32 fi = fx.FlashIntensity();
        if (fi > 0.001f) {
            const FVec3 fc = fx.FlashColor();
            sb.DrawRect(0.0f, 0.0f, static_cast<f32>(sw), static_cast<f32>(sh),
                        FVec4{fc.x, fc.y, fc.z, fi});
        }
    }

    // ----- HUD テキストラベル (フォントが ready のときだけ) -----
    if (!app.FontReady()) return;
    FFont& body = app.FontBody();
    char buf[64];

    const f32 hp_cur = health.GetCurrentHp(player.HealthHandle());
    std::snprintf(buf, sizeof(buf), "HP %.0f / %.0f",
                  static_cast<double>(hp_cur), static_cast<double>(kPlayerHp));
    sb.DrawString(body, buf, 28.0f, 50.0f, FVec4{1, 1, 1, 1});

    const u64 sc = score.CurrentScore();
    std::snprintf(buf, sizeof(buf), "Score: %llu",
                  static_cast<unsigned long long>(sc));
    const f32 sw_w = body.MeasureWidth(buf);
    sb.DrawString(body, buf, static_cast<f32>(sw) - 20.0f - sw_w, 50.0f,
                  FVec4{1, 1, 0.4f, 1});

    std::snprintf(buf, sizeof(buf), "Wave %u / %u",
                  waves.CurrentWaveIndex() + 1u, waves.TotalWaves());
    const f32 ww = body.MeasureWidth(buf);
    sb.DrawString(body, buf,
                  static_cast<f32>(sw) * 0.5f - ww * 0.5f, 50.0f,
                  FVec4{0.5f, 0.8f, 1, 1});

    // FPS 表示 (左下、診断用)。色は >=55 緑 / >=30 黄 / else 赤。
    std::snprintf(buf, sizeof(buf), "FPS: %.0f  (dt: %.1f ms)",
                  static_cast<double>(fps_ema),
                  static_cast<double>(last_dt * 1000.0f));
    const FVec4 fps_col = (fps_ema >= 55.0f) ? FVec4{0.5f, 1, 0.5f, 1}
                        : (fps_ema >= 30.0f) ? FVec4{1, 1, 0.4f, 1}
                                               : FVec4{1, 0.4f, 0.4f, 1};
    sb.DrawString(body, buf, 20.0f, static_cast<f32>(sh) - 30.0f, fps_col);
}

} // namespace hellofg
