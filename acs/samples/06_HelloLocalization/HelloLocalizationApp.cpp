// SPDX-License-Identifier: Apache-2.0
// HelloLocalization — Application 実装。
#include "HelloLocalizationApp.h"
#include "Types.h"

#include "app/Sample.h"
#include "platform/Input.h"

#include "memory/UniquePtr.h"
#include "foundation/Log.h"

#include <cstring>
#include <cstdio>

using namespace acs;

namespace helloloc {

void HelloLocalizationApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    // Fallback は英語固定。Active だけ切替。
    ACS_SAMPLE_INIT(_loc.LoadFallbackBytes(reinterpret_cast<const u8*>(kLangEn),
                                             std::strlen(kLangEn)));
    SwitchTo(0);    // 起動時は日本語

    ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)Sample::TryLoadDefaultUIFont(_font_big,   *dev, 36.0f, 1024, true);
    (void)Sample::TryLoadDefaultUIFont(_font_small, *dev, 20.0f, 1024, true);
}

void HelloLocalizationApp::OnUpdate(f32 /*dt*/) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    if (Input::IsKeyPressed(EKey::F1)) SwitchTo(0);
    if (Input::IsKeyPressed(EKey::F2)) SwitchTo(1);
    if (Input::IsKeyPressed(EKey::F3)) SwitchTo(2);
}

void HelloLocalizationApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;
    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();

    _batch.Begin(*cl, sw, sh);
    _batch.DrawRect(0, 0, static_cast<f32>(sw), static_cast<f32>(sh),
                    Vec4{0.10f, 0.13f, 0.20f, 1});
    _batch.DrawRect(40, 40, static_cast<f32>(sw - 80), static_cast<f32>(sh - 80),
                    Vec4{0.16f, 0.20f, 0.30f, 1});

    if (_font_big.AtlasTexture()) {
        _batch.DrawString(_font_big, _loc.Tr("title"),
                        80, 80, Vec4{1, 0.85f, 0.4f, 1});
    }
    if (_font_small.AtlasTexture()) {
        _batch.DrawString(_font_small, _loc.Tr("greeting"),
                        80, 150, Vec4{0.95f, 0.95f, 1, 1});

        const char* lang_name = (_lang == 0) ? "(日本語 / Japanese)"
                              : (_lang == 1) ? "(English)"
                              : "(Français)";
        _batch.DrawString(_font_small, lang_name,
                        80, 180, Vec4{0.6f, 0.7f, 0.85f, 1});

        _batch.DrawString(_font_small, _loc.Tr("menu.start"),   80, 240, Vec4{1,1,1,1});
        _batch.DrawString(_font_small, _loc.Tr("menu.options"), 80, 270, Vec4{1,1,1,1});
        _batch.DrawString(_font_small, _loc.Tr("menu.exit"),    80, 300, Vec4{1,1,1,1});

        _batch.DrawString(_font_small, _loc.Tr("note"),
                        80, static_cast<f32>(sh - 130), Vec4{0.7f, 0.7f, 0.8f, 1});
        _batch.DrawString(_font_small, _loc.Tr("hint"),
                        80, static_cast<f32>(sh - 90), Vec4{0.6f, 0.7f, 0.95f, 1});
    }
    _batch.End();
}

void HelloLocalizationApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    _font_small.Shutdown();
    _font_big.Shutdown();
    _batch.Shutdown();
}

void HelloLocalizationApp::SwitchTo(u32 idx) noexcept {
    const char* src = (idx == 0) ? kLangJa : (idx == 1) ? kLangEn : kLangFr;
    _loc.LoadActiveBytes(reinterpret_cast<const u8*>(src), std::strlen(src));
    _lang = idx;
}

} // namespace helloloc
