// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — FSceneServices 実装
#include "gameframework/SceneServices.h"
#include "foundation/Assert.h"

namespace acs::game {

/** wanted bit を見て該当サービスだけを alloc する (Physics/Triggers は Init も呼ぶ)。 */
FSceneServices::FSceneServices(ESvc wanted) noexcept : m_Wanted(wanted)
{
    if (Has(ESvc::Clock)) m_Clock = MakeUnique<FSceneClock>();
    if (Has(ESvc::Tweens)) m_Tweens = MakeUnique<FTweenManager>();
    if (Has(ESvc::Sequences)) m_Sequences = MakeUnique<FSequenceRunner>();
    if (Has(ESvc::Input)) m_Input = MakeUnique<FInputServiceState>();
    if (Has(ESvc::Camera2D)) m_Camera = MakeUnique<acs::game::FCamera2D>();
    if (Has(ESvc::Physics2D)) {
        m_Physics = MakeUnique<FCollisionWorld2D>();
        m_Physics->Init(); // 既定 cell_size=64
    }
    if (Has(ESvc::Triggers)) {
        m_Triggers = MakeUnique<FTriggerWorld2D>();
        m_Triggers->Init();
    }
}

/** FSceneClock への参照を返す (未要求なら ACS_CHECK で停止)。 */
FSceneClock& FSceneServices::Clock() noexcept
{
    ACS_CHECKF(m_Clock.Get() != nullptr,
               "FSceneServices::Clock() called but ESvc::Clock not requested in WantedServices()");
    return *m_Clock;
}

/** FTweenManager への参照を返す (未要求なら ACS_CHECK で停止)。 */
FTweenManager& FSceneServices::Tweens() noexcept
{
    ACS_CHECKF(m_Tweens.Get() != nullptr,
               "FSceneServices::Tweens() called but ESvc::Tweens not requested in WantedServices()");
    return *m_Tweens;
}

/** FSequenceRunner への参照を返す (未要求なら ACS_CHECK で停止)。 */
FSequenceRunner& FSceneServices::Sequences() noexcept
{
    ACS_CHECKF(m_Sequences.Get() != nullptr,
               "FSceneServices::Sequences() called but ESvc::Sequences not requested in WantedServices()");
    return *m_Sequences;
}

/** FInputMap への参照を返す (未要求なら ACS_CHECK で停止)。 */
FInputMap& FSceneServices::Input() noexcept
{
    ACS_CHECKF(m_Input.Get() != nullptr,
               "FSceneServices::Input() called but ESvc::Input not requested in WantedServices()");
    return m_Input->input_map;
}

/** 現在の固定 tick へ割り当てられた読み取り専用入力状態を返す。 */
const IInputStateView& FSceneServices::FixedInput() const noexcept
{
    ACS_CHECKF(m_Input.Get() != nullptr,
               "FSceneServices::FixedInput() called but ESvc::Input not requested in WantedServices()");
    return m_Input->fixed_input;
}

/** FCamera2D への参照を返す (未要求なら ACS_CHECK で停止)。 */
acs::game::FCamera2D& FSceneServices::Camera() noexcept
{
    ACS_CHECKF(m_Camera.Get() != nullptr,
               "FSceneServices::Camera() called but ESvc::Camera2D not requested in WantedServices()");
    return *m_Camera;
}

/** FCollisionWorld2D への参照を返す (未要求なら ACS_CHECK で停止)。 */
FCollisionWorld2D& FSceneServices::Physics() noexcept
{
    ACS_CHECKF(m_Physics.Get() != nullptr,
               "FSceneServices::Physics() called but ESvc::Physics2D not requested in WantedServices()");
    return *m_Physics;
}

/** FTriggerWorld2D への参照を返す (未要求なら ACS_CHECK で停止)。 */
FTriggerWorld2D& FSceneServices::Triggers() noexcept
{
    ACS_CHECKF(m_Triggers.Get() != nullptr,
               "FSceneServices::Triggers() called but ESvc::Triggers not requested in WantedServices()");
    return *m_Triggers;
}

/** 一フレーム分の入力を検証し、次の固定 tick まで蓄積する。 */
bool FSceneServices::SubmitFrameInput_Internal(const IInputStateView& input) noexcept
{
    return m_Input.Get() == nullptr || m_Input->fixed_input_buffer.TryPushFrame(input);
}

/** 次の固定 tick 入力を確定し、未入力なら安全な無入力状態を公開する。 */
void FSceneServices::BeginFixedStepInput_Internal() noexcept
{
    if (m_Input.Get() == nullptr) return;
    if (!m_Input->fixed_input_buffer.TryConsumeFixedStep(m_Input->fixed_input)) {
        m_Input->fixed_input.Clear();
    }
}

/** シーン境界を越えて古い入力エッジを再生しないよう固定入力を初期化する。 */
void FSceneServices::ResetFixedInput_Internal() noexcept
{
    if (m_Input.Get() == nullptr) return;
    m_Input->fixed_input_buffer.Reset();
    m_Input->fixed_input.Clear();
}

/** 未消費の固定入力を検証付きで保存値へ複製する。 */
bool FSceneServices::TryCaptureFixedInputSnapshot_Internal(FFixedStepInputBufferSnapshot& snapshot) const noexcept
{
    return m_Input.Get() != nullptr && m_Input->fixed_input_buffer.TryCaptureSnapshot(snapshot);
}

/** 未消費の固定入力を復元し、前回 tick の公開入力を破棄する。 */
bool FSceneServices::TryRestoreFixedInputSnapshot_Internal(const FFixedStepInputBufferSnapshot& snapshot) noexcept
{
    if (m_Input.Get() == nullptr || !m_Input->fixed_input_buffer.TryRestoreSnapshot(snapshot)) return false;
    m_Input->fixed_input.Clear();
    return true;
}

/** Clock を Tick して scaled dt を確定する前段 tick。 */
void FSceneServices::PreUpdate_Internal(f32 raw_dt) noexcept
{
    // Clock 進行 (= scaled dt 確定)。他サービスは scene.OnUpdate の後で tick。
    if (m_Clock) m_Clock->Tick(raw_dt);
}

/** Tweens/Sequences/Camera/Triggers を scaled dt で Tick する後段 tick。 */
void FSceneServices::PostUpdate_Internal(f32 scaled_dt) noexcept
{
    if (m_Tweens) m_Tweens->Tick(scaled_dt);
    if (m_Sequences) m_Sequences->Tick(scaled_dt);
    if (m_Camera) m_Camera->Tick(scaled_dt);
    // Triggers は overlap 比較 + enter/stay/exit 発火。物理 body の move 後に
    // 評価したいので Tweens/Camera と同じ PostUpdate 段で tick する。
    if (m_Triggers) m_Triggers->Tick(scaled_dt);
}

/** OnUpdate へ渡す dt を返す (Clock があればその scaled dt、無ければ raw_dt)。 */
f32 FSceneServices::ScaledDt_Internal(f32 raw_dt) const noexcept
{
    return m_Clock ? m_Clock->Dt() : raw_dt;
}

} // namespace acs::game
