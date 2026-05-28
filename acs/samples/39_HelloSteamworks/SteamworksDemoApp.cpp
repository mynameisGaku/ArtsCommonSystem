// SPDX-License-Identifier: Apache-2.0
// HelloSteamworks — SteamworksDemoApp 実装。
#include "SteamworksDemoApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "foundation/Log.h"

#include <cstdio>
#include <cstring>

namespace hellosteam {

using namespace acs;

void SteamworksDemoApp::SetStatus(const char* msg) noexcept {
    std::strncpy(m_StatusLine, msg, sizeof(m_StatusLine) - 1);
    m_StatusLine[sizeof(m_StatusLine) - 1] = 0;
    ACS_LOG_INFO("[Steamworks demo] %s", m_StatusLine);
}

void SteamworksDemoApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(m_Batch.Init(*dev, GetRenderer().ColorFormat()));
    if (FSample::TryLoadDefaultUIFont(m_Font, *dev, 28.0f, 1024, false).IsOk() &&
        FSample::TryLoadDefaultUIFont(m_SmallFont, *dev, 18.0f, 1024, false).IsOk()) {
        m_bFontReady = true;
    }

    // backend 選択: real が使えてかつ Init 成功なら real、それ以外は stub。
#if WITH_ACS_STEAMWORKS
    if (m_RealSocial.Init().IsOk()) {
        m_Social     = &m_RealSocial;
        m_bUsingReal = true;
        SetStatus("Real Steamworks backend initialized");
    } else {
        m_Social     = &acs::game::SteamworksBridgeStub::GetStub();
        (void)m_Social->Init();
        m_bUsingReal = false;
        SetStatus("Real init failed -> fell back to Stub (Steam client?)");
    }
#else
    m_Social     = &acs::game::SteamworksBridgeStub::GetStub();
    (void)m_Social->Init();
    m_bUsingReal = false;
    SetStatus("Stub backend (build with ACS_BUILD_STEAMWORKS=ON for real)");
#endif

    // local player 取得
    const acs::game::PlayerIdentity me = m_Social->GetLocalPlayer();
    std::snprintf(m_PlayerLine, sizeof(m_PlayerLine), "Player: %s  (id=%s)",
                  me.display_name ? me.display_name : "?",
                  me.platform_id  ? me.platform_id  : "?");
}

void SteamworksDemoApp::OnUpdate(f32 dt) noexcept {
    if (!m_Social) return;
    m_Social->Tick(dt);  // 必須: callback ポンプ

    if (Input::IsKeyPressed(EKey::Escape)) { Quit(); return; }

    if (Input::IsKeyPressed(EKey::Num1)) {
        if (auto r = m_Social->UnlockAchievement("ACH_WIN_ONE_GAME"); r.IsOk()) {
            SetStatus("UnlockAchievement(ACH_WIN_ONE_GAME) OK");
        } else {
            SetStatus("UnlockAchievement failed (stub or no Steam)");
        }
    }
    if (Input::IsKeyPressed(EKey::Num2)) {
        if (auto g = m_Social->GetStat("demo_clicks"); g.IsOk()) {
            const i64 next = g.Value() + 1;
            if (m_Social->SetStat("demo_clicks", next).IsOk()) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "SetStat(demo_clicks) = %lld",
                              static_cast<long long>(next));
                SetStatus(buf);
            } else {
                SetStatus("SetStat failed");
            }
        } else {
            SetStatus("GetStat failed (stub or no Steam)");
        }
    }
    if (Input::IsKeyPressed(EKey::Num3)) {
        m_LastScore += 100;
        if (m_Social->SetLeaderboardScore("demo_board", m_LastScore).IsOk()) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "SetLeaderboardScore(demo_board, %lld) sent",
                          static_cast<long long>(m_LastScore));
            SetStatus(buf);
        } else {
            SetStatus("SetLeaderboardScore failed");
        }
    }
    if (Input::IsKeyPressed(EKey::Num4)) {
        const char* payload = "ACS Steamworks demo save v1";
        if (m_Social->CloudWriteFile("save.dat", payload,
                                     static_cast<u32>(std::strlen(payload))).IsOk()) {
            SetStatus("CloudWriteFile(save.dat) OK");
        } else {
            SetStatus("CloudWriteFile failed (stub or Cloud disabled)");
        }
    }
}

void SteamworksDemoApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;
    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();

    m_Batch.Begin(*cl, sw, sh);
    m_Batch.DrawRect(0, 0, static_cast<f32>(sw), static_cast<f32>(sh),
                    FVec4{0.07f, 0.09f, 0.13f, 1.0f});

    if (m_bFontReady) {
        const FVec4 white{0.95f, 0.96f, 1.0f, 1.0f};
        const FVec4 accent = m_bUsingReal ? FVec4{0.4f, 1.0f, 0.5f, 1.0f}
                                          : FVec4{1.0f, 0.8f, 0.35f, 1.0f};

        m_Batch.DrawString(m_Font, "ACS Steamworks Demo", 60.0f, 60.0f, white);
        m_Batch.DrawString(m_SmallFont,
                          m_bUsingReal ? "backend: REAL Steamworks SDK"
                                       : "backend: Stub (no-op)",
                          60.0f, 110.0f, accent);
        m_Batch.DrawString(m_SmallFont, m_PlayerLine, 60.0f, 140.0f, white);

        m_Batch.DrawString(m_SmallFont,
                          "[1] Unlock achievement   [2] +1 stat   "
                          "[3] Leaderboard +100   [4] Cloud save",
                          60.0f, 200.0f, FVec4{0.7f, 0.8f, 0.95f, 1.0f});

        m_Batch.DrawString(m_SmallFont, "Status:", 60.0f, 250.0f,
                          FVec4{0.6f, 0.65f, 0.75f, 1.0f});
        m_Batch.DrawString(m_SmallFont, m_StatusLine, 60.0f, 278.0f, white);

        m_Batch.DrawString(m_SmallFont, "Esc: 終了", 60.0f,
                          static_cast<f32>(sh) - 50.0f,
                          FVec4{0.5f, 0.5f, 0.55f, 1.0f});
    }

    m_Batch.End();
}

void SteamworksDemoApp::OnShutdown() noexcept {
    if (m_Social) m_Social->Shutdown();
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    if (m_bFontReady) {
        m_SmallFont.Shutdown();
        m_Font.Shutdown();
    }
    m_Batch.Shutdown();
}

} // namespace hellosteam
