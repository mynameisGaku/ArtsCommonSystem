// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — FSceneServices 実装 (Phase 8)
#include "gameframework/SceneServices.h"
#include "foundation/Assert.h"

namespace acs::game {

FSceneServices::FSceneServices(ESvc wanted) noexcept
    : _wanted(wanted) {
    if (Has(ESvc::Clock))     _clock     = MakeUnique<FSceneClock>();
    if (Has(ESvc::Tweens))    _tweens    = MakeUnique<FTweenManager>();
    if (Has(ESvc::Sequences)) _sequences = MakeUnique<FSequenceRunner>();
    if (Has(ESvc::Input))     _input     = MakeUnique<FInputMap>();
    if (Has(ESvc::Camera2D))  _camera    = MakeUnique<acs::game::FCamera2D>();
    if (Has(ESvc::Physics2D)) {
        _physics = MakeUnique<FCollisionWorld2D>();
        _physics->Init();   // 既定 cell_size=64
    }
}

FSceneClock& FSceneServices::Clock() noexcept {
    ACS_ASSERTF(_clock.Get() != nullptr,
                "FSceneServices::Clock() called but ESvc::Clock not requested in WantedServices()");
    return *_clock;
}

FTweenManager& FSceneServices::Tweens() noexcept {
    ACS_ASSERTF(_tweens.Get() != nullptr,
                "FSceneServices::Tweens() called but ESvc::Tweens not requested in WantedServices()");
    return *_tweens;
}

FSequenceRunner& FSceneServices::Sequences() noexcept {
    ACS_ASSERTF(_sequences.Get() != nullptr,
                "FSceneServices::Sequences() called but ESvc::Sequences not requested in WantedServices()");
    return *_sequences;
}

FInputMap& FSceneServices::Input() noexcept {
    ACS_ASSERTF(_input.Get() != nullptr,
                "FSceneServices::Input() called but ESvc::Input not requested in WantedServices()");
    return *_input;
}

acs::game::FCamera2D& FSceneServices::Camera() noexcept {
    ACS_ASSERTF(_camera.Get() != nullptr,
                "FSceneServices::Camera() called but ESvc::Camera2D not requested in WantedServices()");
    return *_camera;
}

FCollisionWorld2D& FSceneServices::Physics() noexcept {
    ACS_ASSERTF(_physics.Get() != nullptr,
                "FSceneServices::Physics() called but ESvc::Physics2D not requested in WantedServices()");
    return *_physics;
}

void FSceneServices::_PreUpdate(f32 raw_dt) noexcept {
    // Clock 進行 (= scaled dt 確定)。他サービスは scene.OnUpdate の後で tick。
    if (_clock) _clock->Tick(raw_dt);
}

void FSceneServices::_PostUpdate(f32 scaled_dt) noexcept {
    if (_tweens)    _tweens->Tick(scaled_dt);
    if (_sequences) _sequences->Tick(scaled_dt);
    if (_camera)    _camera->Tick(scaled_dt);
}

f32 FSceneServices::_ScaledDt(f32 raw_dt) const noexcept {
    return _clock ? _clock->Dt() : raw_dt;
}

} // namespace acs::game
