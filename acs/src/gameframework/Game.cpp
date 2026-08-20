// SPDX-License-Identifier: Apache-2.0
// FGame 実装
#include "gameframework/Game.h"
#include "gameframework/InputFrameSource.h"
#include "gameframework/InputStateSnapshot.h"
#include "gameframework/PlatformInputStateAdapter.h"
#include "gameframework/Scene.h"

#include "render/Renderer.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiSwapchain.h"
#include "app/Sample.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <cmath>

namespace acs::game {

/** 最大回数を実行した後も 1 step 未満の剰余を残せる蓄積上限を求める。 */
static f64 FixedStepCapacity(f64 step_seconds, u32 maximum_steps) noexcept
{
    const f64 requested_capacity = step_seconds * (static_cast<f64>(maximum_steps) + 1.0);
    return requested_capacity < kMaximumFixedStepAccumulatedSeconds ? requested_capacity
                                                                    : kMaximumFixedStepAccumulatedSeconds;
}

/** 互換 API から固定更新時計を設定し、0 以下の step または 0 回指定では無効化する。 */
void FGame::SetFixedTimestep(f32 fixed_dt, u32 max_steps_per_frame) noexcept
{
    if (fixed_dt <= 0.0f || max_steps_per_frame == 0u) {
        DisableFixedTimestep();
        return;
    }

    FFixedStepOptions options{};
    options.step_seconds = static_cast<f64>(fixed_dt);
    options.maximum_steps_per_advance = max_steps_per_frame;
    options.maximum_accumulated_seconds = FixedStepCapacity(options.step_seconds, max_steps_per_frame);
    if (!TrySetFixedTimestep(options)) {
        ACS_LOG_WARN("FGame::SetFixedTimestep rejected step=%g max_steps=%u", static_cast<double>(fixed_dt),
                     max_steps_per_frame);
    }
}

/** 完全な固定更新設定を検証し、成功時だけ時計と有効状態を更新する。 */
bool FGame::TrySetFixedTimestep(const FFixedStepOptions& options) noexcept
{
    if (!m_FixedStepClock.Configure(options)) return false;
    m_Scenes.ExecutionAccess().ResetFixedInput();
    m_FixedStepEnabled = true;
    return true;
}

/** 固定更新を無効化し、以前の設定を残したまま累積状態だけを初期化する。 */
void FGame::DisableFixedTimestep() noexcept
{
    m_FixedStepClock.Reset();
    m_Scenes.ExecutionAccess().ResetFixedInput();
    m_FixedStepEnabled = false;
}

/** 有効な固定更新時計の状態を取得する。 */
bool FGame::TryCaptureFixedStepSnapshot(FFixedStepClockSnapshot& snapshot) const noexcept
{
    return m_FixedStepEnabled && m_FixedStepClock.TryCaptureSnapshot(snapshot);
}

/** 検証済み snapshot を復元し、成功時だけ固定更新を有効化する。 */
bool FGame::TryRestoreFixedStepSnapshot(const FFixedStepClockSnapshot& snapshot) noexcept
{
    if (!m_FixedStepClock.TryRestoreSnapshot(snapshot)) return false;
    m_Scenes.ExecutionAccess().ResetFixedInput();
    m_FixedStepEnabled = true;
    return true;
}

/** 固定時計と active scene の未消費入力を同じ境界で保存する。 */
bool FGame::TryCaptureFixedStepRuntimeSnapshot(FFixedStepRuntimeSnapshot& snapshot) const noexcept
{
    FFixedStepRuntimeSnapshot candidate{};
    candidate.fixed_step_enabled = m_FixedStepEnabled;
    if (!m_FixedStepClock.TryCaptureSnapshot(candidate.clock)) return false;
    if (!m_Scenes.TryCaptureActiveFixedInputSnapshot(candidate.input)) return false;
    snapshot = candidate;
    return true;
}

/** 時計を隔離検証し、入力復元の成功後に固定実行状態を一括反映する。 */
bool FGame::TryRestoreFixedStepRuntimeSnapshot(const FFixedStepRuntimeSnapshot& snapshot) noexcept
{
    FFixedStepClock validated_clock;
    if (!validated_clock.TryRestoreSnapshot(snapshot.clock)) return false;
    if (!m_Scenes.TryRestoreActiveFixedInputSnapshot(snapshot.input)) return false;
    m_FixedStepClock = validated_clock;
    m_FixedStepEnabled = snapshot.fixed_step_enabled;
    return true;
}

/** 入力ソースを切り替え、以前の取得元から残った未消費入力を破棄する。 */
void FGame::SetFixedStepInputSource(IInputFrameSource& source) noexcept
{
    if (m_FixedStepInputSource == &source) return;
    m_FixedStepInputSource = &source;
    m_Scenes.ExecutionAccess().ResetFixedInput();
}

/** platform 入力へ戻し、差し替え元から残った未消費入力を破棄する。 */
void FGame::ResetFixedStepInputSource() noexcept
{
    if (m_FixedStepInputSource == nullptr) return;
    m_FixedStepInputSource = nullptr;
    m_Scenes.ExecutionAccess().ResetFixedInput();
}

/** 起動時に InitialScene() を push して即時適用する。 */
void FGame::OnStart() noexcept
{
    // サブシステムを先に初期化(最初のシーンの World サブシステム/OnEnter が参照できるように)。
    // Engine が最上位、GameInstance はその子(Get<T> が Engine へフォールバックする)。owner=FGame。
    m_EngineSubsystems.Initialize(ESubsystemScope::Engine, nullptr, this);
    m_GameInstanceSubsystems.Initialize(ESubsystemScope::GameInstance, &m_EngineSubsystems, this);

    TUniquePtr<FScene> first = InitialScene();
    if (!first) {
        ACS_LOG_ERROR("FGame::InitialScene() returned null — Quit");
        Quit();
        return;
    }
    m_Scenes.PushScene(Move(first));
    m_Scenes.ExecutionAccess().ApplyPending(*this); // 起動時の最初の遷移は即時適用
}

/** フェード進行と固定 / 可変タイムステップ update を毎フレーム駆動する。 */
void FGame::OnUpdate(f32 dt) noexcept
{
    // フェード遷移は実時間で進める (time_scale / pause の影響を受けない)。
    // 暗転 (MidPause) になった瞬間に次 Scene へ差し替え、そのまま fade-in する。
    if (m_Fade.IsActive()) {
        m_Fade.Tick(dt);
        if (m_Fade.IsMidPause() && m_PendingScene) {
            m_Scenes.ChangeScene(Move(m_PendingScene)); // deferred → 下の実行アダプターで適用
        }
    }

    const f32 scaled_dt = dt * m_TimeScale;
    FSceneManager::FExecutionAdapter scene_execution = m_Scenes.ExecutionAccess();
    scene_execution.ApplyPending(*this);

    // PollEvents 後の platform 入力を一度だけ所有 snapshot にし、active Scene へ提出する。
    if (m_FixedStepEnabled && m_TimeScale > 0.0f && scaled_dt >= 0.0f && std::isfinite(scaled_dt)) {
        FInputStateSnapshot frame_input;
        const bool captured = m_FixedStepInputSource != nullptr
                                  ? m_FixedStepInputSource->TryCaptureFrameInput(frame_input)
                                  : FPlatformInputStateAdapter::TryCapture(frame_input);
        if (!captured || !scene_execution.SubmitFrameInput(frame_input)) {
            scene_execution.ResetFixedInput();
            ACS_LOG_WARN("FGame: fixed-step input capture was rejected; neutral input will be used");
        }
    } else {
        scene_execution.ResetFixedInput();
    }

    // 固定更新時計が確定した回数だけ Scene::OnFixedUpdate を呼ぶ。
    if (m_FixedStepEnabled) {
        const FFixedStepAdvanceResult advance = m_FixedStepClock.Advance(static_cast<f64>(scaled_dt));
        if (advance.accepted) {
            const f32 fixed_dt = static_cast<f32>(m_FixedStepClock.Options().step_seconds);
            for (u32 step_index = 0; step_index < advance.step_count; ++step_index) {
                scene_execution.FixedUpdate(fixed_dt);
            }
        }
    }

    // Engine / GameInstance サブシステムを tick(シーンより先 = ゲーム全体の状態を先に更新)。
    m_EngineSubsystems.Tick(scaled_dt);
    m_GameInstanceSubsystems.Tick(scaled_dt);

    // variable-rate update (毎フレーム dt)。Scene 内で World サブシステムも tick される。
    scene_execution.Update(scaled_dt);
}

/** 初回呼び出しで default UI フォントを 1 回だけ遅延ロードする。 */
void FGame::EnsureUiFont() noexcept
{
    if (m_UiFontTried) return;
    IRhiDevice* dev = GetRenderer().Device();
    if (dev == nullptr) return; // device 未準備 → 次フレーム再試行
    m_UiFontTried = true;
    const auto r = FSample::TryLoadDefaultUIFont(m_UiFont, *dev, 18.0f);
    m_UiFontReady = r.IsOk();
    if (!m_UiFontReady) {
        ACS_LOG_WARN("FGame: default UI font のロードに失敗 (HUD テキストは無効)");
    }
}

/** フレームを開始し、シーン描画の上にフェード幕を重ねて終了する。 */
void FGame::OnRender() noexcept
{
    IRhiCommandList* cl = GetRenderer().CommandList();
    IRhiSwapchain* sc = GetRenderer().Swapchain();
    if (!cl || !sc) return;
    m_RenderCtx._BeginFrame(GetRenderer(), *cl, sc->Width(), sc->Height());
    // 全シーン共有の UI フォントを毎フレーム配線 (初回に遅延ロード)。
    EnsureUiFont();
    if (m_UiFontReady) m_RenderCtx._SetFont(&m_UiFont);
    m_Scenes.ExecutionAccess().Render(m_RenderCtx);
    DrawFadeOverlay(); // シーン描画の上にフェード幕を重ねる
    m_RenderCtx._EndFrame();
}

/** 初回呼び出しでフェード overlay 用 SpriteBatch を 1 回だけ遅延 init する。 */
void FGame::EnsureOverlay() noexcept
{
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
void FGame::DrawFadeOverlay() noexcept
{
    if (!m_Fade.IsActive()) return;
    EnsureOverlay();
    if (!m_OverlayReady) return;
    IRhiCommandList* cl = GetRenderer().CommandList();
    IRhiSwapchain* sc = GetRenderer().Swapchain();
    if (cl == nullptr || sc == nullptr) return;
    m_Overlay.Begin(*cl, sc->Width(), sc->Height()); // Begin で view = 画面ピクセル
    const FVec3 col = m_Fade.OverlayColor();
    m_Overlay.DrawRect(0.0f, 0.0f, static_cast<f32>(sc->Width()), static_cast<f32>(sc->Height()),
                       FVec4{col.x, col.y, col.z, m_Fade.OverlayAlpha()});
    m_Overlay.End();
}

/** 次 Scene を保留し FadeInOut を開始する (暗転中に切替)。 */
void FGame::TransitionTo(TUniquePtr<FScene> next, f32 out_sec, f32 in_sec) noexcept
{
    if (!next) return;
    m_PendingScene = Move(next);
    m_Fade.StartFade(EFadeKind::FadeInOut, out_sec, in_sec, 0.0f);
}

/** 全シーンを shutdown し、サブシステムと UI フォント・overlay リソースを解放する。 */
void FGame::OnShutdown() noexcept
{
    m_Scenes.ExecutionAccess().ShutdownAll(); // 各シーンが OnExit で World サブシステムを解体
    // サブシステムを下位スコープから順に解体(GameInstance → Engine)。
    m_GameInstanceSubsystems.Deinitialize();
    m_EngineSubsystems.Deinitialize();
    if (m_UiFontReady) m_UiFont.Shutdown();
    if (m_OverlayReady) m_Overlay.Shutdown();
}

/** 受け取ったイベントを FSceneManager にディスパッチする。 */
void FGame::OnEvent(const FEvent& e) noexcept
{
    m_Scenes.ExecutionAccess().DispatchEvent(e);
}

} // namespace acs::game
