// SPDX-License-Identifier: Apache-2.0
// HelloLocalization — FApplication 実装。
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
    ACS_SAMPLE_INIT(m_Loc.LoadFallbackBytes(reinterpret_cast<const u8*>(kLangEn),
                                             std::strlen(kLangEn)));
    SwitchTo(0);    // 起動時は日本語

    ACS_SAMPLE_INIT(m_Batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)FSample::TryLoadDefaultUIFont(m_FontBig,   *dev, 36.0f, 1024, true);
    (void)FSample::TryLoadDefaultUIFont(m_FontSmall, *dev, 20.0f, 1024, true);
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

    m_Batch.Begin(*cl, sw, sh);
    m_Batch.DrawRect(0, 0, static_cast<f32>(sw), static_cast<f32>(sh),
                    FVec4{0.10f, 0.13f, 0.20f, 1});
    m_Batch.DrawRect(40, 40, static_cast<f32>(sw - 80), static_cast<f32>(sh - 80),
                    FVec4{0.16f, 0.20f, 0.30f, 1});

    if (m_FontBig.AtlasTexture()) {
        m_Batch.DrawString(m_FontBig, m_Loc.Tr("title"),
                        80, 80, FVec4{1, 0.85f, 0.4f, 1});
    }
    if (m_FontSmall.AtlasTexture()) {
        m_Batch.DrawString(m_FontSmall, m_Loc.Tr("greeting"),
                        80, 150, FVec4{0.95f, 0.95f, 1, 1});

        const char* lang_name = (m_Lang == 0) ? "(日本語 / Japanese)"
                              : (m_Lang == 1) ? "(English)"
                              : "(Français)";
        m_Batch.DrawString(m_FontSmall, lang_name,
                        80, 180, FVec4{0.6f, 0.7f, 0.85f, 1});

        m_Batch.DrawString(m_FontSmall, m_Loc.Tr("menu.start"),   80, 240, FVec4{1,1,1,1});
        m_Batch.DrawString(m_FontSmall, m_Loc.Tr("menu.options"), 80, 270, FVec4{1,1,1,1});
        m_Batch.DrawString(m_FontSmall, m_Loc.Tr("menu.exit"),    80, 300, FVec4{1,1,1,1});

        m_Batch.DrawString(m_FontSmall, m_Loc.Tr("note"),
                        80, static_cast<f32>(sh - 130), FVec4{0.7f, 0.7f, 0.8f, 1});
        m_Batch.DrawString(m_FontSmall, m_Loc.Tr("hint"),
                        80, static_cast<f32>(sh - 90), FVec4{0.6f, 0.7f, 0.95f, 1});
    }
    m_Batch.End();
}

void HelloLocalizationApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    m_FontSmall.Shutdown();
    m_FontBig.Shutdown();
    m_Batch.Shutdown();
}

void HelloLocalizationApp::SwitchTo(u32 idx) noexcept {
    const char* src = (idx == 0) ? kLangJa : (idx == 1) ? kLangEn : kLangFr;
    m_Loc.LoadActiveBytes(reinterpret_cast<const u8*>(src), std::strlen(src));
    m_Lang = idx;
}

} // namespace helloloc
