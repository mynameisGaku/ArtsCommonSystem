// SPDX-License-Identifier: Apache-2.0
// HelloSave — FApplication 実装。
#include "HelloSaveApp.h"

#include "app/Sample.h"
#include "platform/Input.h"

#include "memory/UniquePtr.h"
#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

namespace hellosave {

void FHelloSaveApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    // %APPDATA% 配下なら UAC でもユーザ書き込み可。Load 失敗は初回起動の正常系。
    ACS_SAMPLE_INIT(FStorage::GetAppDataPath(L"acs_demo", L"hello_save.ini",
                                             m_SavePath, 260));
    ACS_LOG_INFO("Save file: %ls", m_SavePath);
    if (auto r = m_Store.Load(m_SavePath); r.IsErr()) {
        ACS_LOG_WARN("FStorage::Load failed: %s (continuing with empty)", r.Error().message);
    }

    i64 launches = m_Store.GetInt("launches", 0) + 1;
    m_Store.SetInt("launches", launches);

    m_Clicks    = m_Store.GetInt("clicks", 0);
    m_HighScore = m_Store.GetInt("high_score", 0);
    const char* name = m_Store.GetString("player_name", "");
    if (name[0] == 0) {
        m_Store.SetString("player_name", "プレイヤー");
    }

    ACS_SAMPLE_INIT(m_Batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)FSample::TryLoadDefaultUIFont(m_FontBig,   *dev, 32.0f, 1024, true);
    (void)FSample::TryLoadDefaultUIFont(m_FontSmall, *dev, 18.0f, 1024, true);

    ACS_LOG_INFO("HelloSave: launches=%lld, clicks=%lld, high_score=%lld",
                 launches, static_cast<long long>(m_Clicks),
                 static_cast<long long>(m_HighScore));
}

void FHelloSaveApp::OnUpdate(f32 /*dt*/) noexcept {
    if (FInput::IsKeyPressed(EKey::Escape)) Quit();

    if (FInput::IsKeyPressed(EKey::Space)) {
        ++m_Clicks;
        if (m_Clicks > m_HighScore) m_HighScore = m_Clicks;
        m_Dirty = true;
    }
    if (FInput::IsKeyPressed(EKey::R)) {
        // Clear() は launches も消すので、本セッションを 1 回目として再播種する。
        m_Store.Clear();
        m_Clicks = 0;
        m_HighScore = 0;
        m_Store.SetInt("launches", 1);
        m_Store.SetString("player_name", "プレイヤー");
        m_Dirty = true;
    }
    if (FInput::IsKeyPressed(EKey::S)) {
        FlushAndSave();
    }
}

void FHelloSaveApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;
    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();

    m_Batch.Begin(*cl, sw, sh);
    m_Batch.DrawRect(0, 0, static_cast<f32>(sw), static_cast<f32>(sh),
                    FVec4{0.10f, 0.13f, 0.18f, 1});
    m_Batch.DrawRect(40, 40, static_cast<f32>(sw - 80), static_cast<f32>(sh - 80),
                    FVec4{0.16f, 0.20f, 0.28f, 1});

    if (m_FontBig.AtlasTexture()) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s さん、ようこそ！",
                      m_Store.GetString("player_name", "プレイヤー"));
        m_Batch.DrawString(m_FontBig, buf, 80, 80, FVec4{1, 0.95f, 0.7f, 1});
    }
    if (m_FontSmall.AtlasTexture()) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "起動回数: %lld",
                      static_cast<long long>(m_Store.GetInt("launches", 0)));
        m_Batch.DrawString(m_FontSmall, buf, 80, 150, FVec4{0.9f,0.95f,1,1});

        std::snprintf(buf, sizeof(buf), "今回のクリック数: %lld",
                      static_cast<long long>(m_Clicks));
        m_Batch.DrawString(m_FontSmall, buf, 80, 180, FVec4{0.9f,0.95f,1,1});

        std::snprintf(buf, sizeof(buf), "ハイスコア: %lld",
                      static_cast<long long>(m_HighScore));
        m_Batch.DrawString(m_FontSmall, buf, 80, 210,
                        (m_Clicks == m_HighScore && m_Clicks > 0)
                        ? FVec4{1, 0.85f, 0.4f, 1}    // ハイスコア更新中は黄色で強調
                        : FVec4{0.9f, 0.95f, 1, 1});

        m_Batch.DrawString(m_FontSmall,
                        "Space: クリック  S: 手動保存  R: リセット  Esc: 終了 (自動保存)",
                        80, static_cast<f32>(sh - 80),
                        FVec4{0.6f, 0.7f, 0.85f, 1});

        if (m_Dirty) {
            m_Batch.DrawString(m_FontSmall, "● 未保存（終了か S で保存）",
                            80, static_cast<f32>(sh - 110),
                            FVec4{1, 0.5f, 0.5f, 1});
        } else {
            m_Batch.DrawString(m_FontSmall, "○ 保存済み",
                            80, static_cast<f32>(sh - 110),
                            FVec4{0.5f, 1, 0.5f, 1});
        }
    }
    m_Batch.End();
}

void FHelloSaveApp::OnShutdown() noexcept {
    // GPU リソース破棄前に保存しておけば、保存中クラッシュでもプレイデータは無事。
    FlushAndSave();
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    m_FontSmall.Shutdown();
    m_FontBig.Shutdown();
    m_Batch.Shutdown();
}

void FHelloSaveApp::FlushAndSave() noexcept {
    m_Store.SetInt("clicks",     m_Clicks);
    m_Store.SetInt("high_score", m_HighScore);
    if (auto r = m_Store.Save(m_SavePath); r.IsErr()) {
        ACS_LOG_ERROR("Save failed: %s", r.Error().message);
    } else {
        m_Dirty = false;
        ACS_LOG_INFO("Saved to %ls", m_SavePath);
    }
}

} // namespace hellosave
