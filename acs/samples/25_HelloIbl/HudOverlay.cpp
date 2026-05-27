// SPDX-License-Identifier: Apache-2.0
// HelloIbl — LDR backbuffer 上の HUD 描画 (FSpriteBatch)。
//
// FPostProcess.Render で tonemap → LDR backbuffer に書き出した後の FSpriteBatch pass。
// 右上に BRDF LUT サムネ、左下と右下に SSR / SSAO デバッグ overlay (有効時のみ)、
// 左上に状態テキスト群 (preset / display mode / toggle 一覧 / 操作ヒント)。
#include "HelloIblApp.h"

#include <cstdio>

using namespace acs;

namespace helloibl {

namespace {

void DrawBrdfLutOverlay(HelloIblApp& app, IRhiTexture* lut, u32 sw) noexcept {
    if (!lut) return;
    app.m_Batch.DrawRect(static_cast<f32>(sw) - 280, 20,
                        260, 320, FVec4{0, 0, 0, 0.55f});
    app.m_Batch.Draw(*lut,
                    static_cast<f32>(sw) - 270, 60,
                    240, 240);
}

void DrawSsrDebugOverlay(HelloIblApp& app, u32 sh) noexcept {
    if (!app.m_ShowSsr || !app.m_Ssr.OutputTexture()) return;
    app.m_Batch.DrawRect(20, static_cast<f32>(sh) - 280,
                        420, 260, FVec4{0, 0, 0, 0.6f});
    app.m_Batch.Draw(*app.m_Ssr.OutputTexture(), 30, static_cast<f32>(sh) - 270,
                    400, 240);
    if (app.m_Font.AtlasTexture()) {
        app.m_Batch.DrawString(app.m_Font, "SSR debug overlay",
                              30, static_cast<f32>(sh) - 268, FVec4{1, 1, 1, 1});
    }
}

void DrawSsaoDebugOverlay(HelloIblApp& app, u32 sw, u32 sh) noexcept {
    if (!app.m_bUseSsao || !app.m_Ssao.OutputTexture()) return;
    const f32 ax = static_cast<f32>(sw) - 440;
    const f32 ay = static_cast<f32>(sh) - 280;
    app.m_Batch.DrawRect(ax, ay, 420, 260, FVec4{0, 0, 0, 0.6f});
    app.m_Batch.Draw(*app.m_Ssao.OutputTexture(), ax + 10, ay + 10,
                    400, 240);
    if (app.m_Font.AtlasTexture()) {
        app.m_Batch.DrawString(app.m_Font, "SSAO debug (R=AO  G=contact shadow)",
                              ax + 10, ay + 12, FVec4{1, 1, 1, 1});
    }
}

void DrawStatusText(HelloIblApp& app, IRhiTexture* lut, u32 sw) noexcept {
    if (!app.m_Font.AtlasTexture()) return;

    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "IBL + HDR  FPS: %.1f", static_cast<double>(app.FPS()));
    app.m_Batch.DrawString(app.m_Font, buf, 20, 20, FVec4{1, 1, 1, 1});

    const char* preset =
        (app.m_CurrentPreset == 0) ? "Day" :
        (app.m_CurrentPreset == 1) ? "Sunset" :
        (app.m_CurrentPreset == 2) ? "Night" :
        (app.m_CurrentPreset == 3) ? "Studio HDR" : "Hillaire FAtmosphere";
    std::snprintf(buf, sizeof(buf),
                  "Env preset: [%s]   (1/2/3 sky / 4 Studio HDR / 5 FAtmosphere)", preset);
    app.m_Batch.DrawString(app.m_Font, buf, 20, 44, FVec4{0.85f, 0.95f, 1.0f, 1});

    const char* view_label = nullptr;
    switch (app.m_DisplayMode) {
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
    app.m_Batch.DrawString(app.m_Font, buf, 20, 68, FVec4{1.0f, 0.95f, 0.7f, 1});

    char exp_label[48];
    if (app.m_bUseAutoExposure) {
        std::snprintf(exp_label, sizeof(exp_label), "AUTO (key %.2f)",
                      static_cast<double>(app.m_AutoKey));
    } else {
        std::snprintf(exp_label, sizeof(exp_label), "%.2f (manual)",
                      static_cast<double>(app.m_PostParams.exposure));
    }
    std::snprintf(buf, sizeof(buf),
                  "Exposure: %s   Bloom: %s   Diffuse: %s   (U auto, Q/E exp, B bloom)",
                  exp_label,
                  app.m_PostParams.bloom_enabled ? "ON" : "OFF",
                  app.m_bUseSh9 ? "SH9 (light probe)" : "Irradiance cube");
    app.m_Batch.DrawString(app.m_Font, buf, 20, 92, FVec4{0.9f, 0.9f, 0.9f, 1});

    std::snprintf(buf, sizeof(buf),
                  "CC=%s Aniso=%s Area=%s ProbeG=%s Fog=%s Shadow=%s SSAO=%s TAA=%s SSGI=%s LM=%s MV=%s Refract=%s",
                  app.m_bUseClearcoat ? "ON" : "OFF",
                  app.m_bUseAnisotropy ? "ON" : "OFF",
                  app.m_bUseAreaLight ? "ON" : "OFF",
                  app.m_bUseProbeGrid ? "ON" : "OFF",
                  app.m_bUseFog ? "ON" : "OFF",
                  app.m_bUseShadows ? "ON" : "OFF",
                  app.m_bUseSsao ? "ON" : "OFF",
                  app.m_bUseTaa ? "ON" : "OFF",
                  app.m_bUseSsgi ? "ON" : "OFF",
                  app.m_bUseLightmap ? "ON" : "OFF",
                  app.m_bUseMotionVec ? "ON" : "OFF",
                  app.m_ShowRefraction ? "ON" : "OFF");
    app.m_Batch.DrawString(app.m_Font, buf, 20, 116, FVec4{0.9f, 0.9f, 0.9f, 1});
    app.m_Batch.DrawString(app.m_Font, "WASD: 移動  矢印: 視点  X: 屈折demo  Esc: 終了",
                          20, 140, FVec4{0.7f, 0.85f, 1.0f, 1});
    if (lut) {
        app.m_Batch.DrawString(app.m_Font, "BRDF LUT",
                              static_cast<f32>(sw) - 260, 36, FVec4{1, 1, 1, 1});
        app.m_Batch.DrawString(app.m_Font, "X:NdotV  Y:roughness",
                              static_cast<f32>(sw) - 265, 308, FVec4{0.85f, 0.85f, 0.85f, 1});
    }
}

} // anonymous namespace

void DrawHud(HelloIblApp& app, u32 sw, u32 sh) noexcept {
    IRhiCommandList* cl = app.GetRenderer().CommandList();
    if (!cl) return;

    IRhiTexture* lut = app.m_Ibl.BrdfLut();
    app.m_Batch.Begin(*cl, sw, sh);
    DrawBrdfLutOverlay(app, lut, sw);
    DrawSsrDebugOverlay(app, sh);
    DrawSsaoDebugOverlay(app, sw, sh);
    DrawStatusText(app, lut, sw);
    app.m_Batch.End();
}

} // namespace helloibl
