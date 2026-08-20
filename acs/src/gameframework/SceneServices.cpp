// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — CSceneServices 実装
#include "gameframework/SceneServices.h"
#include "foundation/Assert.h"
#include "foundation/Move.h"

namespace acs::game {

/** wanted bit を見て該当サービスだけを alloc する (Physics/Triggers は Init も呼ぶ)。 */
CSceneServices::CSceneServices(ESvc wanted) noexcept
    : m_Wanted(wanted) {
    /** 成功時だけmemberへ移すclock候補。 */
    TUniquePtr<CSceneClock> Clock;
    /** 成功時だけmemberへ移すtween候補。 */
    TUniquePtr<CTweenManager> Tweens;
    /** 成功時だけmemberへ移すsequence候補。 */
    TUniquePtr<CSequenceRunner> Sequences;
    /** 成功時だけmemberへ移すinput候補。 */
    TUniquePtr<FInputServiceState> Input;
    /** 成功時だけmemberへ移すcamera候補。 */
    TUniquePtr<acs::game::CCamera2D> Camera;
    /** 成功時だけmemberへ移すphysics候補。 */
    TUniquePtr<CCollisionWorld2D> Physics;
    /** 成功時だけmemberへ移すtrigger候補。 */
    TUniquePtr<CTriggerWorld2D> Triggers;

    if (Has(ESvc::Clock)) {
        Clock = MakeUnique<CSceneClock>();
        if (!Clock) return;
    }
    if (Has(ESvc::Tweens)) {
        Tweens = MakeUnique<CTweenManager>();
        if (!Tweens) return;
    }
    if (Has(ESvc::Sequences)) {
        Sequences = MakeUnique<CSequenceRunner>();
        if (!Sequences) return;
    }
    if (Has(ESvc::Input)) {
        Input = MakeUnique<FInputServiceState>();
        if (!Input) return;
    }
    if (Has(ESvc::Camera2D)) {
        Camera = MakeUnique<acs::game::CCamera2D>();
        if (!Camera) return;
    }
    if (Has(ESvc::Physics2D)) {
        Physics = MakeUnique<CCollisionWorld2D>();
        if (!Physics) return;
        Physics->Init();   // 既定 cell_size=64
    }
    if (Has(ESvc::Triggers)) {
        Triggers = MakeUnique<CTriggerWorld2D>();
        if (!Triggers) return;
        Triggers->Init();
    }

    m_Clock = Move(Clock);
    m_Tweens = Move(Tweens);
    m_Sequences = Move(Sequences);
    m_Input = Move(Input);
    m_Camera = Move(Camera);
    m_Physics = Move(Physics);
    m_Triggers = Move(Triggers);
}

/** 未知bitを含まず、要求された全サービスが生成済みならtrueを返す。 */
bool CSceneServices::IsReady_Internal() const noexcept
{
    constexpr u32 KnownBits =
        static_cast<u32>(ESvc::Clock) | static_cast<u32>(ESvc::Tweens) |
        static_cast<u32>(ESvc::Sequences) | static_cast<u32>(ESvc::Input) |
        static_cast<u32>(ESvc::Camera2D) | static_cast<u32>(ESvc::Physics2D) |
        static_cast<u32>(ESvc::Triggers);
    if ((static_cast<u32>(m_Wanted) & ~KnownBits) != 0u) return false;
    return (!Has(ESvc::Clock) || m_Clock) && (!Has(ESvc::Tweens) || m_Tweens) &&
           (!Has(ESvc::Sequences) || m_Sequences) && (!Has(ESvc::Input) || m_Input) &&
           (!Has(ESvc::Camera2D) || m_Camera) && (!Has(ESvc::Physics2D) || m_Physics) &&
           (!Has(ESvc::Triggers) || m_Triggers);
}

/** CSceneClock への参照を返す (未要求なら ACS_CHECK で停止)。 */
CSceneClock& CSceneServices::Clock() noexcept {
    ACS_CHECKF(m_Clock.Get() != nullptr,
               "CSceneServices::Clock() called but ESvc::Clock not requested in WantedServices()");
    return *m_Clock;
}

/** CTweenManager への参照を返す (未要求なら ACS_CHECK で停止)。 */
CTweenManager& CSceneServices::Tweens() noexcept {
    ACS_CHECKF(m_Tweens.Get() != nullptr,
               "CSceneServices::Tweens() called but ESvc::Tweens not requested in WantedServices()");
    return *m_Tweens;
}

/** CSequenceRunner への参照を返す (未要求なら ACS_CHECK で停止)。 */
CSequenceRunner& CSceneServices::Sequences() noexcept {
    ACS_CHECKF(m_Sequences.Get() != nullptr,
               "CSceneServices::Sequences() called but ESvc::Sequences not requested in WantedServices()");
    return *m_Sequences;
}

/** FInputMap への参照を返す (未要求なら ACS_CHECK で停止)。 */
FInputMap& CSceneServices::Input() noexcept {
    ACS_CHECKF(m_Input.Get() != nullptr,
               "CSceneServices::Input() called but ESvc::Input not requested in WantedServices()");
    return m_Input->input_map;
}

/** 現在の固定tickへ割り当てられた読み取り専用入力状態を返す。 */
const IInputStateView& CSceneServices::FixedInput() const noexcept
{
    ACS_CHECKF(m_Input.Get() != nullptr,
               "CSceneServices::FixedInput() called but ESvc::Input not requested in WantedServices()");
    return m_Input->fixed_input;
}

/** CCamera2D への参照を返す (未要求なら ACS_CHECK で停止)。 */
acs::game::CCamera2D& CSceneServices::Camera() noexcept {
    ACS_CHECKF(m_Camera.Get() != nullptr,
               "CSceneServices::Camera() called but ESvc::Camera2D not requested in WantedServices()");
    return *m_Camera;
}

/** CCollisionWorld2D への参照を返す (未要求なら ACS_CHECK で停止)。 */
CCollisionWorld2D& CSceneServices::Physics() noexcept {
    ACS_CHECKF(m_Physics.Get() != nullptr,
               "CSceneServices::Physics() called but ESvc::Physics2D not requested in WantedServices()");
    return *m_Physics;
}

/** CTriggerWorld2D への参照を返す (未要求なら ACS_CHECK で停止)。 */
CTriggerWorld2D& CSceneServices::Triggers() noexcept {
    ACS_CHECKF(m_Triggers.Get() != nullptr,
               "CSceneServices::Triggers() called but ESvc::Triggers not requested in WantedServices()");
    return *m_Triggers;
}

/** 一フレーム分の入力を検証し、次の固定tickまで蓄積する。 */
bool CSceneServices::SubmitFrameInput_Internal(const IInputStateView& input) noexcept
{
    return m_Input.Get() == nullptr || m_Input->fixed_input_buffer.TryPushFrame(input);
}

/** 次の固定tick入力を確定し、未入力なら安全な無入力状態を公開する。 */
void CSceneServices::BeginFixedStepInput_Internal() noexcept
{
    if (m_Input.Get() == nullptr) return;
    if (!m_Input->fixed_input_buffer.TryConsumeFixedStep(m_Input->fixed_input)) m_Input->fixed_input.Clear();
}

/** シーン境界を越えて古い入力エッジを再生しないよう固定入力を初期化する。 */
void CSceneServices::ResetFixedInput_Internal() noexcept
{
    if (m_Input.Get() == nullptr) return;
    m_Input->fixed_input_buffer.Reset();
    m_Input->fixed_input.Clear();
}

/** 未消費の固定入力を検証付きで保存値へ複製する。 */
bool CSceneServices::TryCaptureFixedInputSnapshot_Internal(FFixedStepInputBufferSnapshot& snapshot) const noexcept
{
    return m_Input.Get() != nullptr && m_Input->fixed_input_buffer.TryCaptureSnapshot(snapshot);
}

/** 未消費の固定入力を復元し、前回tickの公開入力を破棄する。 */
bool CSceneServices::TryRestoreFixedInputSnapshot_Internal(const FFixedStepInputBufferSnapshot& snapshot) noexcept
{
    if (m_Input.Get() == nullptr || !m_Input->fixed_input_buffer.TryRestoreSnapshot(snapshot)) return false;
    m_Input->fixed_input.Clear();
    return true;
}

/** Clock を Tick して scaled dt を確定する前段 tick。 */
void CSceneServices::PreUpdate_Internal(f32 raw_dt) noexcept {
    // Clock 進行 (= scaled dt 確定)。他サービスは scene.OnUpdate の後で tick。
    if (m_Clock) m_Clock->Tick(raw_dt);
}

/** Tweens/Sequences/Camera/Triggers を scaled dt で Tick する後段 tick。 */
void CSceneServices::PostUpdate_Internal(f32 scaled_dt) noexcept {
    if (m_Tweens)    m_Tweens->Tick(scaled_dt);
    if (m_Sequences) m_Sequences->Tick(scaled_dt);
    if (m_Camera)    m_Camera->Tick(scaled_dt);
    // Triggers は overlap 比較 + enter/stay/exit 発火。物理 body の move 後に
    // 評価したいので Tweens/Camera と同じ PostUpdate 段で tick する。
    if (m_Triggers)  m_Triggers->Tick(scaled_dt);
}

/** OnUpdate へ渡す dt を返す (Clock があればその scaled dt、無ければ raw_dt)。 */
f32 CSceneServices::ScaledDt_Internal(f32 raw_dt) const noexcept {
    return m_Clock ? m_Clock->Dt() : raw_dt;
}

} // namespace acs::game
