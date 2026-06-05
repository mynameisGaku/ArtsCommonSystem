// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — FSceneServices 実装
#include "gameframework/SceneServices.h"
#include "foundation/Assert.h"

namespace acs::game {

/** wanted bit を見て該当サービスだけを alloc する (Physics/Triggers は Init も呼ぶ)。 */
FSceneServices::FSceneServices(ESvc wanted) noexcept
    : m_Wanted(wanted) {
    if (Has(ESvc::Clock))     m_Clock     = MakeUnique<FSceneClock>();
    if (Has(ESvc::Tweens))    m_Tweens    = MakeUnique<FTweenManager>();
    if (Has(ESvc::Sequences)) m_Sequences = MakeUnique<FSequenceRunner>();
    if (Has(ESvc::Input))     m_Input     = MakeUnique<FInputMap>();
    if (Has(ESvc::Camera2D))  m_Camera    = MakeUnique<acs::game::FCamera2D>();
    if (Has(ESvc::Physics2D)) {
        m_Physics = MakeUnique<FCollisionWorld2D>();
        m_Physics->Init();   // 既定 cell_size=64
    }
    if (Has(ESvc::Triggers)) {
        m_Triggers = MakeUnique<FTriggerWorld2D>();
        m_Triggers->Init();
    }
}

/** FSceneClock への参照を返す (未要求なら ACS_ASSERT)。 */
FSceneClock& FSceneServices::Clock() noexcept {
    ACS_ASSERTF(m_Clock.Get() != nullptr,
                "FSceneServices::Clock() called but ESvc::Clock not requested in WantedServices()");
    return *m_Clock;
}

/** FTweenManager への参照を返す (未要求なら ACS_ASSERT)。 */
FTweenManager& FSceneServices::Tweens() noexcept {
    ACS_ASSERTF(m_Tweens.Get() != nullptr,
                "FSceneServices::Tweens() called but ESvc::Tweens not requested in WantedServices()");
    return *m_Tweens;
}

/** FSequenceRunner への参照を返す (未要求なら ACS_ASSERT)。 */
FSequenceRunner& FSceneServices::Sequences() noexcept {
    ACS_ASSERTF(m_Sequences.Get() != nullptr,
                "FSceneServices::Sequences() called but ESvc::Sequences not requested in WantedServices()");
    return *m_Sequences;
}

/** FInputMap への参照を返す (未要求なら ACS_ASSERT)。 */
FInputMap& FSceneServices::Input() noexcept {
    ACS_ASSERTF(m_Input.Get() != nullptr,
                "FSceneServices::Input() called but ESvc::Input not requested in WantedServices()");
    return *m_Input;
}

/** FCamera2D への参照を返す (未要求なら ACS_ASSERT)。 */
acs::game::FCamera2D& FSceneServices::Camera() noexcept {
    ACS_ASSERTF(m_Camera.Get() != nullptr,
                "FSceneServices::Camera() called but ESvc::Camera2D not requested in WantedServices()");
    return *m_Camera;
}

/** FCollisionWorld2D への参照を返す (未要求なら ACS_ASSERT)。 */
FCollisionWorld2D& FSceneServices::Physics() noexcept {
    ACS_ASSERTF(m_Physics.Get() != nullptr,
                "FSceneServices::Physics() called but ESvc::Physics2D not requested in WantedServices()");
    return *m_Physics;
}

/** FTriggerWorld2D への参照を返す (未要求なら ACS_ASSERT)。 */
FTriggerWorld2D& FSceneServices::Triggers() noexcept {
    ACS_ASSERTF(m_Triggers.Get() != nullptr,
                "FSceneServices::Triggers() called but ESvc::Triggers not requested in WantedServices()");
    return *m_Triggers;
}

/** Clock を Tick して scaled dt を確定する前段 tick。 */
void FSceneServices::_PreUpdate(f32 raw_dt) noexcept {
    // Clock 進行 (= scaled dt 確定)。他サービスは scene.OnUpdate の後で tick。
    if (m_Clock) m_Clock->Tick(raw_dt);
}

/** Tweens/Sequences/Camera/Triggers を scaled dt で Tick する後段 tick。 */
void FSceneServices::_PostUpdate(f32 scaled_dt) noexcept {
    if (m_Tweens)    m_Tweens->Tick(scaled_dt);
    if (m_Sequences) m_Sequences->Tick(scaled_dt);
    if (m_Camera)    m_Camera->Tick(scaled_dt);
    // Triggers は overlap 比較 + enter/stay/exit 発火。物理 body の move 後に
    // 評価したいので Tweens/Camera と同じ PostUpdate 段で tick する。
    if (m_Triggers)  m_Triggers->Tick(scaled_dt);
}

/** OnUpdate へ渡す dt を返す (Clock があればその scaled dt、無ければ raw_dt)。 */
f32 FSceneServices::_ScaledDt(f32 raw_dt) const noexcept {
    return m_Clock ? m_Clock->Dt() : raw_dt;
}

} // namespace acs::game
