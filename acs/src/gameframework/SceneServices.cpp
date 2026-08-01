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
    TUniquePtr<FTweenManager> Tweens;
    /** 成功時だけmemberへ移すsequence候補。 */
    TUniquePtr<CSequenceRunner> Sequences;
    /** 成功時だけmemberへ移すinput候補。 */
    TUniquePtr<FInputMap> Input;
    /** 成功時だけmemberへ移すcamera候補。 */
    TUniquePtr<acs::game::CCamera2D> Camera;
    /** 成功時だけmemberへ移すphysics候補。 */
    TUniquePtr<CCollisionWorld2D> Physics;
    /** 成功時だけmemberへ移すtrigger候補。 */
    TUniquePtr<FTriggerWorld2D> Triggers;

    if (Has(ESvc::Clock)) {
        Clock = MakeUnique<CSceneClock>();
        if (!Clock) return;
    }
    if (Has(ESvc::Tweens)) {
        Tweens = MakeUnique<FTweenManager>();
        if (!Tweens) return;
    }
    if (Has(ESvc::Sequences)) {
        Sequences = MakeUnique<CSequenceRunner>();
        if (!Sequences) return;
    }
    if (Has(ESvc::Input)) {
        Input = MakeUnique<FInputMap>();
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
        Triggers = MakeUnique<FTriggerWorld2D>();
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
bool CSceneServices::IsReady() const noexcept
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
               "FSceneServices::Clock() called but ESvc::Clock not requested in WantedServices()");
    return *m_Clock;
}

/** FTweenManager への参照を返す (未要求なら ACS_CHECK で停止)。 */
FTweenManager& CSceneServices::Tweens() noexcept {
    ACS_CHECKF(m_Tweens.Get() != nullptr,
               "FSceneServices::Tweens() called but ESvc::Tweens not requested in WantedServices()");
    return *m_Tweens;
}

/** CSequenceRunner への参照を返す (未要求なら ACS_CHECK で停止)。 */
CSequenceRunner& CSceneServices::Sequences() noexcept {
    ACS_CHECKF(m_Sequences.Get() != nullptr,
               "FSceneServices::Sequences() called but ESvc::Sequences not requested in WantedServices()");
    return *m_Sequences;
}

/** FInputMap への参照を返す (未要求なら ACS_CHECK で停止)。 */
FInputMap& CSceneServices::Input() noexcept {
    ACS_CHECKF(m_Input.Get() != nullptr,
               "FSceneServices::Input() called but ESvc::Input not requested in WantedServices()");
    return *m_Input;
}

/** CCamera2D への参照を返す (未要求なら ACS_CHECK で停止)。 */
acs::game::CCamera2D& CSceneServices::Camera() noexcept {
    ACS_CHECKF(m_Camera.Get() != nullptr,
               "FSceneServices::Camera() called but ESvc::Camera2D not requested in WantedServices()");
    return *m_Camera;
}

/** CCollisionWorld2D への参照を返す (未要求なら ACS_CHECK で停止)。 */
CCollisionWorld2D& CSceneServices::Physics() noexcept {
    ACS_CHECKF(m_Physics.Get() != nullptr,
               "FSceneServices::Physics() called but ESvc::Physics2D not requested in WantedServices()");
    return *m_Physics;
}

/** FTriggerWorld2D への参照を返す (未要求なら ACS_CHECK で停止)。 */
FTriggerWorld2D& CSceneServices::Triggers() noexcept {
    ACS_CHECKF(m_Triggers.Get() != nullptr,
               "FSceneServices::Triggers() called but ESvc::Triggers not requested in WantedServices()");
    return *m_Triggers;
}

/** Clock を Tick して scaled dt を確定する前段 tick。 */
void CSceneServices::_PreUpdate(f32 raw_dt) noexcept {
    // Clock 進行 (= scaled dt 確定)。他サービスは scene.OnUpdate の後で tick。
    if (m_Clock) m_Clock->Tick(raw_dt);
}

/** Tweens/Sequences/Camera/Triggers を scaled dt で Tick する後段 tick。 */
void CSceneServices::_PostUpdate(f32 scaled_dt) noexcept {
    if (m_Tweens)    m_Tweens->Tick(scaled_dt);
    if (m_Sequences) m_Sequences->Tick(scaled_dt);
    if (m_Camera)    m_Camera->Tick(scaled_dt);
    // Triggers は overlap 比較 + enter/stay/exit 発火。物理 body の move 後に
    // 評価したいので Tweens/Camera と同じ PostUpdate 段で tick する。
    if (m_Triggers)  m_Triggers->Tick(scaled_dt);
}

/** OnUpdate へ渡す dt を返す (Clock があればその scaled dt、無ければ raw_dt)。 */
f32 CSceneServices::_ScaledDt(f32 raw_dt) const noexcept {
    return m_Clock ? m_Clock->Dt() : raw_dt;
}

} // namespace acs::game
