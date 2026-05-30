// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — FGame 実装 (Phase 1 着手)
#include "gameframework/Game.h"
#include "gameframework/Scene.h"

#include "render/Renderer.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiSwapchain.h"
#include "app/Sample.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs::game {

void FGame::OnStart() noexcept {
    TUniquePtr<Scene> first = InitialScene();
    if (!first) {
        ACS_LOG_ERROR("FGame::InitialScene() returned null — Quit");
        Quit();
        return;
    }
    m_Scenes.PushScene(Move(first));
    m_Scenes._ApplyPending(*this);     // 起動時の最初の遷移は即時適用
}

void FGame::OnUpdate(f32 dt) noexcept {
    const f32 scaled_dt = dt * m_TimeScale;
    m_Scenes._ApplyPending(*this);

    // Phase 2 固定タイムステップ accumulator (Phase 1 で宣言だけだった
    // Scene::OnFixedUpdate が実際に呼ばれるようになる)。
    //   accumulator += dt → fixed_dt 単位で消費しつつ OnFixedUpdate を呼ぶ。
    //   max_fixed_steps を超える遅延は捨てる (= spiral of death 防止)。
    //   fixed_dt <= 0 なら固定 update は無効 (旧挙動)。
    if (m_FixedDt > 0.0f) {
        m_FixedAccum += scaled_dt;
        u32 steps = 0;
        while (m_FixedAccum >= m_FixedDt && steps < m_MaxFixedSteps) {
            m_Scenes._FixedUpdate(m_FixedDt);
            m_FixedAccum -= m_FixedDt;
            ++steps;
        }
        // 上限超え分は捨てる (遅延を引きずらない)
        if (steps == m_MaxFixedSteps && m_FixedAccum > m_FixedDt) {
            m_FixedAccum = 0.0f;
        }
    }

    // variable-rate update (毎フレーム dt)
    m_Scenes._Update(scaled_dt);
}

void FGame::EnsureUiFont() noexcept {
    if (m_UiFontTried) return;
    IRhiDevice* dev = GetRenderer().Device();
    if (dev == nullptr) return;     // device 未準備 → 次フレーム再試行
    m_UiFontTried = true;
    auto r = FSample::TryLoadDefaultUIFont(m_UiFont, *dev, 18.0f);
    m_UiFontReady = r.IsOk();
    if (!m_UiFontReady) {
        ACS_LOG_WARN("FGame: default UI font のロードに失敗 (HUD テキストは無効)");
    }
}

void FGame::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    IRhiSwapchain*   sc = GetRenderer().Swapchain();
    if (!cl || !sc) return;
    m_RenderCtx._BeginFrame(GetRenderer(), *cl, sc->Width(), sc->Height());
    // 全シーン共有の UI フォントを毎フレーム配線 (初回に遅延ロード)。
    EnsureUiFont();
    if (m_UiFontReady) m_RenderCtx._SetFont(&m_UiFont);
    m_Scenes._Render(m_RenderCtx);
    m_RenderCtx._EndFrame();
}

void FGame::OnShutdown() noexcept {
    m_Scenes._ShutdownAll();
    if (m_UiFontReady) m_UiFont.Shutdown();
}

void FGame::OnEvent(const Event& e) noexcept {
    m_Scenes._DispatchEvent(e);
}

} // namespace acs::game
