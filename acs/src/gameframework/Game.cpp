// SPDX-License-Identifier: Apache-2.0
// CGame 実装
#include "gameframework/Game.h"
#include "gameframework/FixedTickInputSource.h"
#include "gameframework/InputFrameSource.h"
#include "gameframework/InputStateSnapshot.h"
#include "gameframework/PlatformInputStateAdapter.h"
#include "gameframework/Scene.h"
#include "gameframework/SubsystemCatalog.h"

#include "render/Renderer.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiSwapchain.h"
#include "app/UiFontDefaults.h"
#include "foundation/Move.h"
#include "foundation/Log.h"
#include "threading/Atomic.h"

#include <cmath>

namespace acs::game {
namespace {

/** CGame instanceへ重複せず割り当てるprocess内runtime token。 */
TAtomic<u64> g_NextFixedStepRuntimeOwnerToken{1u};

/** process内で一意なruntime tokenを取得し、u64を使い切った場合は0を返す。 */
u64 AcquireFixedStepRuntimeOwnerToken() noexcept
{
    u64 current = g_NextFixedStepRuntimeOwnerToken.Load(EMemoryOrder::Acquire);
    for (;;) {
        if (current == 0u || current == ~u64{0}) return 0u;
        if (g_NextFixedStepRuntimeOwnerToken.CompareExchange(current, current + 1u)) return current;
    }
}

/** 最大回数を実行した後も1step未満の剰余を残せる蓄積上限を求める。 */
f64 FixedStepCapacity(f64 step_seconds, u32 maximum_steps) noexcept
{
    const f64 requested_capacity = step_seconds * (static_cast<f64>(maximum_steps) + 1.0);
    return requested_capacity < timing::kMaximumFixedStepAccumulatedSeconds
               ? requested_capacity
               : timing::kMaximumFixedStepAccumulatedSeconds;
}

} // namespace

CGame::CGame() noexcept : m_FixedStepRuntimeOwnerToken(AcquireFixedStepRuntimeOwnerToken())
{
    m_BuiltinCatalogReady = AcsRegisterGameFrameworkSubsystems();
}

/** 互換APIから固定時計を設定し、0以下のstepまたは0回指定では無効化する。 */
void CGame::SetFixedTimestep(f32 fixed_dt, u32 max_steps_per_frame) noexcept
{
    if (fixed_dt <= 0.0f || max_steps_per_frame == 0u) {
        DisableFixedTimestep();
        return;
    }
    timing::FFixedStepOptions options{};
    options.step_seconds = static_cast<f64>(fixed_dt);
    options.maximum_steps_per_advance = max_steps_per_frame;
    options.maximum_accumulated_seconds = FixedStepCapacity(options.step_seconds, max_steps_per_frame);
    if (!TrySetFixedTimestep(options)) {
        ACS_LOG_WARN("CGame::SetFixedTimestep rejected step=%g max_steps=%u", static_cast<double>(fixed_dt),
                     max_steps_per_frame);
    }
}

/** 完全な固定更新設定を検証し、成功時だけ時計と有効状態を更新する。 */
bool CGame::TrySetFixedTimestep(const timing::FFixedStepOptions& options) noexcept
{
    if (!m_FixedStepClock.Configure(options)) return false;
    m_Scenes.ExecutionAccess().ResetFixedInput();
    m_FixedStepEnabled = true;
    return true;
}

/** 固定更新を無効化し、以前の設定を残したまま累積状態だけを初期化する。 */
void CGame::DisableFixedTimestep() noexcept
{
    m_FixedStepClock.Reset();
    m_Scenes.ExecutionAccess().ResetFixedInput();
    m_FixedStepEnabled = false;
}

/** 有効な固定更新時計の状態を取得する。 */
bool CGame::TryCaptureFixedStepSnapshot(timing::FFixedStepClockSnapshot& snapshot) const noexcept
{
    return m_FixedStepEnabled && m_FixedStepClock.TryCaptureSnapshot(snapshot);
}

/** 検証済みsnapshotを復元し、成功時だけ固定更新を有効化する。 */
bool CGame::TryRestoreFixedStepSnapshot(const timing::FFixedStepClockSnapshot& snapshot) noexcept
{
    if (!m_FixedStepClock.TryRestoreSnapshot(snapshot)) return false;
    m_Scenes.ExecutionAccess().ResetFixedInput();
    m_FixedStepEnabled = true;
    return true;
}

/** 固定時計とactive sceneの未消費入力を同じ境界で保存する。 */
bool CGame::TryCaptureFixedStepRuntimeSnapshot(FFixedStepRuntimeSnapshot& snapshot) const noexcept
{
    FFixedStepRuntimeSnapshot candidate{};
    candidate.runtime_owner_token = m_FixedStepRuntimeOwnerToken;
    candidate.active_scene_epoch = m_Scenes.ActiveSceneEpoch();
    candidate.input_source_epoch = m_FixedInputSourceEpoch;
    if (candidate.runtime_owner_token == 0u || candidate.active_scene_epoch == 0u || candidate.input_source_epoch == 0u)
        return false;
    candidate.fixed_step_enabled = m_FixedStepEnabled;
    if (!m_FixedStepClock.TryCaptureSnapshot(candidate.clock)) return false;
    if (!m_Scenes.TryCaptureActiveFixedInputSnapshot(candidate.input)) return false;
    snapshot = candidate;
    return true;
}

/** 時計を隔離検証し、入力復元の成功後に固定実行状態を一括反映する。 */
bool CGame::TryRestoreFixedStepRuntimeSnapshot(const FFixedStepRuntimeSnapshot& snapshot) noexcept
{
    const bool same_runtime_owner = snapshot.runtime_owner_token != 0u &&
                                    snapshot.runtime_owner_token == m_FixedStepRuntimeOwnerToken;
    const bool same_active_scene = snapshot.active_scene_epoch != 0u &&
                                   snapshot.active_scene_epoch == m_Scenes.ActiveSceneEpoch();
    const bool same_input_source = snapshot.input_source_epoch != 0u &&
                                   snapshot.input_source_epoch == m_FixedInputSourceEpoch;
    if (!same_runtime_owner || !same_active_scene || !same_input_source) return false;

    timing::CFixedStepClock validated_clock;
    if (!validated_clock.TryRestoreSnapshot(snapshot.clock)) return false;
    if (!m_Scenes.TryRestoreActiveFixedInputSnapshot(snapshot.input)) return false;
    m_FixedStepClock = validated_clock;
    m_FixedStepEnabled = snapshot.fixed_step_enabled;
    return true;
}

/** 入力source境界の世代を進め、u64を使い切った後は照合不能な0へ固定する。 */
void CGame::AdvanceFixedInputSourceEpoch_Internal() noexcept
{
    if (m_FixedInputSourceEpoch == 0u || m_FixedInputSourceEpoch == ~u64{0}) {
        m_FixedInputSourceEpoch = 0u;
        return;
    }
    ++m_FixedInputSourceEpoch;
}

/** 描画フレーム入力へ切り替え、以前の取得元から残った未消費入力を破棄する。 */
void CGame::SetFixedStepInputSource(IInputFrameSource& source) noexcept
{
    if (m_FixedStepInputSource == &source && m_FixedTickInputSource == nullptr) return;
    m_FixedStepInputSource = &source;
    m_FixedTickInputSource = nullptr;
    AdvanceFixedInputSourceEpoch_Internal();
    m_Scenes.ExecutionAccess().ResetFixedInput();
}

/** 固定tick入力へ切り替え、frame入力から残った未消費状態を破棄する。 */
void CGame::SetFixedTickInputSource(IFixedTickInputSource& source) noexcept
{
    if (m_FixedTickInputSource == &source && m_FixedStepInputSource == nullptr) return;
    m_FixedTickInputSource = &source;
    m_FixedStepInputSource = nullptr;
    AdvanceFixedInputSourceEpoch_Internal();
    m_Scenes.ExecutionAccess().ResetFixedInput();
}

/** platform入力へ戻し、差し替え元から残った未消費入力を破棄する。 */
void CGame::ResetFixedStepInputSource() noexcept
{
    if (m_FixedStepInputSource == nullptr && m_FixedTickInputSource == nullptr) return;
    m_FixedStepInputSource = nullptr;
    m_FixedTickInputSource = nullptr;
    AdvanceFixedInputSourceEpoch_Internal();
    m_Scenes.ExecutionAccess().ResetFixedInput();
}

/** 起動時に InitialScene() を push して即時適用する。 */
void CGame::OnStart() noexcept {
    m_BuiltinCatalogReady = AcsRegisterGameFrameworkSubsystems();
    if (!m_BuiltinCatalogReady) {
        ACS_LOG_ERROR("CGame: builtin subsystem registration failed");
        Quit();
        return;
    }
    // GameInstance は CApplication 所有の Engine を親にし、最初の World より先に初期化する。
    if (!m_GameInstanceSubsystems.TryInitialize(
            ESubsystemScope::GameInstance, &CApplication::EngineSubsystems(),
            FSubsystemOwner{this, ESubsystemOwnerKind::Game})) {
        ACS_LOG_ERROR("CGame: GameInstance subsystem initialization failed");
        Quit();
        return;
    }

    TUniquePtr<AScene> first = InitialScene();
    if (!first) {
        ACS_LOG_ERROR("CGame::InitialScene() returned null — Quit");
        Quit();
        return;
    }
    m_Scenes.PushScene(Move(first));
    m_Scenes.ExecutionAccess().ApplyPending(*this); // 起動時の最初の遷移は即時適用
    if (m_Scenes.IsEmpty()) {
        ACS_LOG_ERROR("CGame: initial scene subsystem initialization failed");
        Quit();
    }
}

/** フェード進行と固定 / 可変タイムステップ update を毎フレーム駆動する。 */
void CGame::OnUpdate(f32 dt) noexcept {
    // フェード遷移は実時間で進める (time_scale / pause の影響を受けない)。
    // 暗転 (MidPause) になった瞬間に次 Scene へ差し替え、そのまま fade-in する。
    if (m_Fade.IsActive()) {
        m_Fade.Tick(dt);
        if (m_Fade.IsMidPause() && m_PendingScene) {
            // deferred → 下の ApplyPending_Internal で適用 (context も一緒に渡す)
            m_Scenes.ChangeScene(Move(m_PendingScene), Move(m_PendingSceneContext));
        }
    }

    const f32 scaled_dt = dt * m_TimeScale;
    CSceneManager::FExecutionAdapter scene_execution = m_Scenes.ExecutionAccess();
    scene_execution.ApplyPending(*this);

    // frame入力はPollEvents後に一度だけ取得する。固定tick入力は各tick直前に取得する。
    const bool can_advance_fixed = m_FixedStepEnabled && m_TimeScale > 0.0f && scaled_dt >= 0.0f &&
                                   std::isfinite(scaled_dt);
    if (can_advance_fixed && m_FixedTickInputSource == nullptr) {
        FInputStateSnapshot frame_input;
        const bool captured = m_FixedStepInputSource != nullptr
                                  ? m_FixedStepInputSource->TryCaptureFrameInput(frame_input)
                                  : CPlatformInputStateAdapter::TryCapture(frame_input);
        if (!captured || !scene_execution.SubmitFrameInput(frame_input)) {
            scene_execution.ResetFixedInput();
            ACS_LOG_WARN("CGame: fixed-step input capture was rejected; neutral input will be used");
        }
    } else {
        scene_execution.ResetFixedInput();
    }

    // 固定更新時計が確定した回数だけAScene::OnFixedUpdateを呼ぶ。
    if (m_FixedStepEnabled) {
        const u64 first_fixed_tick = m_FixedStepClock.TotalStepCount();
        const timing::FFixedStepAdvanceResult advance = m_FixedStepClock.Advance(static_cast<f64>(scaled_dt));
        if (advance.accepted) {
            const f32 fixed_dt = static_cast<f32>(m_FixedStepClock.Options().step_seconds);
            for (u32 step_index = 0u; step_index < advance.step_count; ++step_index) {
                if (m_FixedTickInputSource != nullptr) {
                    const u64 fixed_tick = first_fixed_tick > ~u64{0} - static_cast<u64>(step_index)
                                               ? ~u64{0}
                                               : first_fixed_tick + static_cast<u64>(step_index);
                    FInputStateSnapshot fixed_input;
                    const bool captured = m_FixedTickInputSource->TryCaptureFixedTickInput(fixed_tick, fixed_input);
                    if (!captured || !scene_execution.SubmitFrameInput(fixed_input)) {
                        scene_execution.ResetFixedInput();
                        ACS_LOG_WARN("CGame: fixed-tick input %llu was rejected; neutral input will be used",
                                     static_cast<unsigned long long>(fixed_tick));
                    }
                }
                scene_execution.FixedUpdate(fixed_dt);
            }
        }
    }

    // 既存の観測順どおり、固定stepを完了してからGameInstance→Worldを進める。
    /** GameInstance の更新前コンテキスト。 */
    const FSubsystemFrameContext PreContext{
        scaled_dt, dt, FrameCount(), ESubsystemTickPhase::PreUpdate};
    m_GameInstanceSubsystems.TickFrame(PreContext);

    // variable-rate update。Scene 内では World Pre → Scene → World Post の順に進む。
    scene_execution.Update(scaled_dt, dt, FrameCount());

    /** GameInstance の更新後コンテキスト。 */
    const FSubsystemFrameContext PostContext{
        scaled_dt, dt, FrameCount(), ESubsystemTickPhase::PostUpdate};
    m_GameInstanceSubsystems.TickFrame(PostContext);
}

/** 初回呼び出しで default UI フォントを 1 回だけ遅延ロードする。 */
void CGame::EnsureUiFont() noexcept {
    if (m_UiFontTried) return;
    IRhiDevice* dev = GetRenderer().Device();
    if (dev == nullptr) return;     // device 未準備 → 次フレーム再試行
    m_UiFontTried = true;
    const auto r = UiFontDefaults::TryLoad(m_UiFont, *dev, 18.0f);
    m_UiFontReady = r.IsOk();
    if (!m_UiFontReady) {
        ACS_LOG_WARN("CGame: default UI font のロードに失敗 (HUD テキストは無効)");
    }
}

/** フレームを開始し、シーン描画の上にフェード幕を重ねて終了する。 */
void CGame::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    IRhiSwapchain*   sc = GetRenderer().Swapchain();
    if (!cl || !sc) return;
    auto wiring = m_RenderCtx.WiringAccess();
    wiring.BeginFrame(GetRenderer(), *cl, sc->Width(), sc->Height());
    // 全シーン共有の UI フォントを毎フレーム配線 (初回に遅延ロード)。
    EnsureUiFont();
    if (m_UiFontReady) wiring.SetFont(&m_UiFont);
    m_Scenes.ExecutionAccess().Render(m_RenderCtx);
    DrawFadeOverlay(); // シーン描画の上にフェード幕を重ねる
    wiring.EndFrame();
}

/** 初回呼び出しでフェード overlay 用 SpriteBatch を 1 回だけ遅延 init する。 */
void CGame::EnsureOverlay() noexcept {
    if (m_OverlayTried) return;
    IRhiDevice* dev = GetRenderer().Device();
    if (dev == nullptr) return;
    m_OverlayTried = true;
    const auto r = m_Overlay.Init(*dev, GetRenderer().ColorFormat(), 16);
    m_OverlayReady = r.IsOk();
    if (!m_OverlayReady) {
        ACS_LOG_WARN("CGame: fade overlay SpriteBatch の init に失敗 (遷移は無描画)");
    }
}

/** 進行中フェードの色と alpha で画面全体を覆う quad を描く。 */
void CGame::DrawFadeOverlay() noexcept {
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
void CGame::TransitionTo(TUniquePtr<AScene> next, f32 out_sec, f32 in_sec) noexcept {
    TransitionTo(Move(next), TUniquePtr<CSceneTravelContext>{}, out_sec, in_sec);
}

/** travel context 付きでフェード遷移を開始する (切替は暗転中に行う)。 */
void CGame::TransitionTo(TUniquePtr<AScene> next,
                         TUniquePtr<CSceneTravelContext> context,
                         f32 out_sec, f32 in_sec) noexcept {
    if (!next) return;
    m_PendingScene        = Move(next);
    m_PendingSceneContext = Move(context);
    m_Fade.StartFade(EFadeKind::FadeInOut, out_sec, in_sec, 0.0f);
}

/** 全シーンを shutdown し、サブシステムと UI フォント・overlay リソースを解放する。 */
void CGame::OnShutdown() noexcept
{
    m_Scenes.ExecutionAccess().ShutdownAll(); // 各シーンが OnExit で World サブシステムを解体
    // Engine は CApplication がこの hook の復帰後に解体する。
    m_GameInstanceSubsystems.Deinitialize();
    if (m_UiFontReady) m_UiFont.Shutdown();
    if (m_OverlayReady) m_Overlay.Shutdown();
}

/** 受け取ったイベントを CSceneManager にディスパッチする。 */
void CGame::OnEvent(const FEvent& e) noexcept
{
    m_Scenes.ExecutionAccess().DispatchEvent(e);
}

} // namespace acs::game
