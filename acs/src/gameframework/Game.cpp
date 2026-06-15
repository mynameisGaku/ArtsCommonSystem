// SPDX-License-Identifier: Apache-2.0
// FGame 実装
#include "gameframework/Game.h"
#include "gameframework/Scene.h"

#include "render/Renderer.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiSwapchain.h"
#include "app/Sample.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs::game {

/** 起動時に InitialScene() を push して即時適用する。 */
void FGame::OnStart() noexcept {
    // サブシステムを先に初期化(最初のシーンの World サブシステム/OnEnter が参照できるように)。
    // Engine が最上位、GameInstance はその子(Get<T> が Engine へフォールバックする)。owner=FGame。
    m_EngineSubsystems.Initialize(ESubsystemScope::Engine, nullptr, this);
    m_GameInstanceSubsystems.Initialize(ESubsystemScope::GameInstance, &m_EngineSubsystems, this);

    TUniquePtr<Scene> first = InitialScene();
    if (!first) {
        ACS_LOG_ERROR("FGame::InitialScene() returned null — Quit");
        Quit();
        return;
    }
    m_Scenes.PushScene(Move(first));
    m_Scenes._ApplyPending(*this);     // 起動時の最初の遷移は即時適用
}

/** フェード進行と固定 / 可変タイムステップ update を毎フレーム駆動する。 */
void FGame::OnUpdate(f32 dt) noexcept {
    // フェード遷移は実時間で進める (time_scale / pause の影響を受けない)。
    // 暗転 (MidPause) になった瞬間に次 Scene へ差し替え、そのまま fade-in する。
    if (m_Fade.IsActive()) {
        m_Fade.Tick(dt);
        if (m_Fade.IsMidPause() && m_PendingScene) {
            m_Scenes.ChangeScene(Move(m_PendingScene));   // deferred → 下の _ApplyPending で適用
        }
    }

    const f32 scaled_dt = dt * m_TimeScale;
    m_Scenes._ApplyPending(*this);

    // 固定タイムステップ accumulator (Scene::OnFixedUpdate を呼ぶ)。
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

    // Engine / GameInstance サブシステムを tick(シーンより先 = ゲーム全体の状態を先に更新)。
    m_EngineSubsystems.Tick(scaled_dt);
    m_GameInstanceSubsystems.Tick(scaled_dt);

    // variable-rate update (毎フレーム dt)。Scene 内で World サブシステムも tick される。
    m_Scenes._Update(scaled_dt);
}

/** 初回呼び出しで default UI フォントを 1 回だけ遅延ロードする。 */
void FGame::EnsureUiFont() noexcept {
    if (m_UiFontTried) return;
    IRhiDevice* dev = GetRenderer().Device();
    if (dev == nullptr) return;     // device 未準備 → 次フレーム再試行
    m_UiFontTried = true;
    const auto r = FSample::TryLoadDefaultUIFont(m_UiFont, *dev, 18.0f);
    m_UiFontReady = r.IsOk();
    if (!m_UiFontReady) {
        ACS_LOG_WARN("FGame: default UI font のロードに失敗 (HUD テキストは無効)");
    }
}

/** フレームを開始し、シーン描画の上にフェード幕を重ねて終了する。 */
void FGame::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    IRhiSwapchain*   sc = GetRenderer().Swapchain();
    if (!cl || !sc) return;
    m_RenderCtx._BeginFrame(GetRenderer(), *cl, sc->Width(), sc->Height());
    // 全シーン共有の UI フォントを毎フレーム配線 (初回に遅延ロード)。
    EnsureUiFont();
    if (m_UiFontReady) m_RenderCtx._SetFont(&m_UiFont);
    m_Scenes._Render(m_RenderCtx);
    DrawFadeOverlay();                  // シーン描画の上にフェード幕を重ねる
    m_RenderCtx._EndFrame();
}

/** 初回呼び出しでフェード overlay 用 SpriteBatch を 1 回だけ遅延 init する。 */
void FGame::EnsureOverlay() noexcept {
    if (m_OverlayTried) return;
    IRhiDevice* dev = GetRenderer().Device();
    if (dev == nullptr) return;
    m_OverlayTried = true;
    const auto r = m_Overlay.Init(*dev, GetRenderer().ColorFormat(), 16);
    m_OverlayReady = r.IsOk();
    if (!m_OverlayReady) {
        ACS_LOG_WARN("FGame: fade overlay SpriteBatch の init に失敗 (遷移は無描画)");
    }
}

/** 進行中フェードの色と alpha で画面全体を覆う quad を描く。 */
void FGame::DrawFadeOverlay() noexcept {
    if (!m_Fade.IsActive()) return;
    EnsureOverlay();
    if (!m_OverlayReady) return;
    IRhiCommandList* cl = GetRenderer().CommandList();
    IRhiSwapchain*   sc = GetRenderer().Swapchain();
    if (cl == nullptr || sc == nullptr) return;
    m_Overlay.Begin(*cl, sc->Width(), sc->Height());   // Begin で view = 画面ピクセル
    const FVec3 col = m_Fade.OverlayColor();
    m_Overlay.DrawRect(0.0f, 0.0f,
                       static_cast<f32>(sc->Width()), static_cast<f32>(sc->Height()),
                       FVec4{col.x, col.y, col.z, m_Fade.OverlayAlpha()});
    m_Overlay.End();
}

/** 次 Scene を保留し FadeInOut を開始する (暗転中に切替)。 */
void FGame::TransitionTo(TUniquePtr<Scene> next, f32 out_sec, f32 in_sec) noexcept {
    if (!next) return;
    m_PendingScene = Move(next);
    m_Fade.StartFade(EFadeKind::FadeInOut, out_sec, in_sec, 0.0f);
}

/** 全シーンを shutdown し、サブシステムと UI フォント・overlay リソースを解放する。 */
void FGame::OnShutdown() noexcept {
    m_Scenes._ShutdownAll();                  // 各シーンが OnExit で World サブシステムを解体
    // サブシステムを下位スコープから順に解体(GameInstance → Engine)。
    m_GameInstanceSubsystems.Deinitialize();
    m_EngineSubsystems.Deinitialize();
    if (m_UiFontReady) m_UiFont.Shutdown();
    if (m_OverlayReady) m_Overlay.Shutdown();
}

/** 受け取ったイベントを FSceneManager にディスパッチする。 */
void FGame::OnEvent(const Event& e) noexcept {
    m_Scenes._DispatchEvent(e);
}

} // namespace acs::game
