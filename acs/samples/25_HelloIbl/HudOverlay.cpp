// SPDX-License-Identifier: Apache-2.0
// HelloIbl — LDR backbuffer 上の HUD 描画 (SpriteBatch)。
//
// PostProcess.Render で tonemap → LDR backbuffer に書き出した後の SpriteBatch pass。
// 右上に BRDF LUT サムネ、左下と右下に SSR / SSAO デバッグ overlay (有効時のみ)、
// 左上に状態テキスト群 (preset / display mode / toggle 一覧 / 操作ヒント)。
#include "HelloIblApp.h"

#include <cstdio>

using namespace acs;

namespace helloibl {

namespace {

void DrawBrdfLutOverlay(HelloIblApp& app, IRhiTexture* lut, u32 sw) noexcept {
    if (!lut) return;
    app._batch.DrawRect(static_cast<f32>(sw) - 280, 20,
                        260, 320, FVec4{0, 0, 0, 0.55f});
    app._batch.Draw(*lut,
                    static_cast<f32>(sw) - 270, 60,
                    240, 240);
}

void DrawSsrDebugOverlay(HelloIblApp& app, u32 sh) noexcept {
    if (!app._show_ssr || !app._ssr.OutputTexture()) return;
    app._batch.DrawRect(20, static_cast<f32>(sh) - 280,
                        420, 260, FVec4{0, 0, 0, 0.6f});
    app._batch.Draw(*app._ssr.OutputTexture(), 30, static_cast<f32>(sh) - 270,
                    400, 240);
    if (app._font.AtlasTexture()) {
        app._batch.DrawString(app._font, "SSR debug overlay",
                              30, static_cast<f32>(sh) - 268, FVec4{1, 1, 1, 1});
    }
}

void DrawSsaoDebugOverlay(HelloIblApp& app, u32 sw, u32 sh) noexcept {
    if (!app._use_ssao || !app._ssao.OutputTexture()) return;
    const f32 ax = static_cast<f32>(sw) - 440;
    const f32 ay = static_cast<f32>(sh) - 280;
    app._batch.DrawRect(ax, ay, 420, 260, FVec4{0, 0, 0, 0.6f});
    app._batch.Draw(*app._ssao.OutputTexture(), ax + 10, ay + 10,
                    400, 240);
    if (app._font.AtlasTexture()) {
        app._batch.DrawString(app._font, "SSAO debug (R=AO  G=contact shadow)",
                              ax + 10, ay + 12, FVec4{1, 1, 1, 1});
    }
}

void DrawStatusText(HelloIblApp& app, IRhiTexture* lut, u32 sw) noexcept {
    if (!app._font.AtlasTexture()) return;

    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "IBL + HDR  FPS: %.1f", static_cast<double>(app.FPS()));
    app._batch.DrawString(app._font, buf, 20, 20, FVec4{1, 1, 1, 1});

    const char* preset =
        (app._current_preset == 0) ? "Day" :
        (app._current_preset == 1) ? "Sunset" :
        (app._current_preset == 2) ? "Night" :
        (app._current_preset == 3) ? "Studio HDR" : "Hillaire Atmosphere";
    std::snprintf(buf, sizeof(buf),
                  "Env preset: [%s]   (1/2/3 sky / 4 Studio HDR / 5 Atmosphere)", preset);
    app._batch.DrawString(app._font, buf, 20, 44, FVec4{0.85f, 0.95f, 1.0f, 1});

    const char* view_label = nullptr;
    switch (app._display_mode) {
        case 0: view_label = "Env cubemap";                            break;
        case 1: view_label = "Irradiance (Lambert 半球積分)";          break;
        case 2: view_label = "Prefilter mip 0 (roughness 0.00)";       break;
        case 3: view_label = "Prefilter mip 1 (roughness 0.25)";       break;
        case 4: view_label = "Prefilter mip 2 (roughness 0.50)";       break;
        case 5: view_label = "Prefilter mip 3 (roughness 0.75)";       break;
        case 6: view_label = "Prefilter mip 4 (roughness 1.00)";       break;
        default: view_label = "?";                                      break;
    }
    std::snprintf(buf, sizeof(buf),
                  "Display: %s   (I で切替)", view_label);
    app._batch.DrawString(app._font, buf, 20, 68, FVec4{1.0f, 0.95f, 0.7f, 1});

    char exp_label[48];
    if (app._use_auto_exposure) {
        std::snprintf(exp_label, sizeof(exp_label), "AUTO (key %.2f)",
                      static_cast<double>(app._auto_key));
    } else {
        std::snprintf(exp_label, sizeof(exp_label), "%.2f (manual)",
                      static_cast<double>(app._post_params.exposure));
    }
    std::snprintf(buf, sizeof(buf),
                  "Exposure: %s   Bloom: %s   Diffuse: %s   (U auto, Q/E exp, B bloom)",
                  exp_label,
                  app._post_params.bloom_enabled ? "ON" : "OFF",
                  app._use_sh9 ? "SH9 (light probe)" : "Irradiance cube");
    app._batch.DrawString(app._font, buf, 20, 92, FVec4{0.9f, 0.9f, 0.9f, 1});

    std::snprintf(buf, sizeof(buf),
                  "CC=%s Aniso=%s Area=%s ProbeG=%s Fog=%s Shadow=%s SSAO=%s TAA=%s SSGI=%s LM=%s MV=%s Refract=%s",
                  app._use_clearcoat ? "ON" : "OFF",
                  app._use_anisotropy ? "ON" : "OFF",
                  app._use_area_light ? "ON" : "OFF",
                  app._use_probe_grid ? "ON" : "OFF",
                  app._use_fog ? "ON" : "OFF",
                  app._use_shadows ? "ON" : "OFF",
                  app._use_ssao ? "ON" : "OFF",
                  app._use_taa ? "ON" : "OFF",
                  app._use_ssgi ? "ON" : "OFF",
                  app._use_lightmap ? "ON" : "OFF",
                  app._use_motion_vec ? "ON" : "OFF",
                  app._show_refraction ? "ON" : "OFF");
    app._batch.DrawString(app._font, buf, 20, 116, FVec4{0.9f, 0.9f, 0.9f, 1});
    app._batch.DrawString(app._font, "WASD: 移動  矢印: 視点  X: 屈折demo  Esc: 終了",
                          20, 140, FVec4{0.7f, 0.85f, 1.0f, 1});
    if (lut) {
        app._batch.DrawString(app._font, "BRDF LUT",
                              static_cast<f32>(sw) - 260, 36, FVec4{1, 1, 1, 1});
        app._batch.DrawString(app._font, "X:NdotV  Y:roughness",
                              static_cast<f32>(sw) - 265, 308, FVec4{0.85f, 0.85f, 0.85f, 1});
    }
}

} // anonymous namespace

void DrawHud(HelloIblApp& app, u32 sw, u32 sh) noexcept {
    IRhiCommandList* cl = app.GetRenderer().CommandList();
    if (!cl) return;

    IRhiTexture* lut = app._ibl.BrdfLut();
    app._batch.Begin(*cl, sw, sh);
    DrawBrdfLutOverlay(app, lut, sw);
    DrawSsrDebugOverlay(app, sh);
    DrawSsaoDebugOverlay(app, sw, sh);
    DrawStatusText(app, lut, sw);
    app._batch.End();
}

} // namespace helloibl
