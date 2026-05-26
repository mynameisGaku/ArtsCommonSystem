// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — HUD overlay の実装。
#include "HudPass.h"

#include "math/Vec.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiSwapchain.h"

#include <cstdio>

using namespace acs;

namespace helloshowcase {

void ExecuteHudPass(Assets& a, IRhiCommandList& cl, IRhiSwapchain& sc,
                    bool paused, bool show_ssr, bool show_refraction) noexcept {
    a.batch.Begin(cl, sc.Width(), sc.Height());
    if (a.font.AtlasTexture()) {
        a.batch.DrawString(a.font,
                           "ACS Showcase — PBR / IBL / Refraction / Bloom / ACES",
                           20, 20, FVec4{1, 1, 1, 1});
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "Pause=%s  SSR=%s  Refract=%s",
                      paused          ? "ON" : "OFF",
                      show_ssr        ? "ON" : "OFF",
                      show_refraction ? "ON" : "OFF");
        a.batch.DrawString(a.font, buf, 20, 44, FVec4{0.85f, 0.85f, 0.85f, 1});
        a.batch.DrawString(a.font, "P: pause  R: SSR  X: refract  Esc: 終了",
                           20, 68, FVec4{0.7f, 0.85f, 1.0f, 1});
    }
    a.batch.End();
}

} // namespace helloshowcase
