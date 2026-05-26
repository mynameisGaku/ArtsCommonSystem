// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — SceneServices 実装 (Phase 8)
#include "gameframework/SceneServices.h"
#include "foundation/Assert.h"

namespace acs::game {

SceneServices::SceneServices(ESvc wanted) noexcept
    : _wanted(wanted) {
    if (Has(ESvc::Clock))     _clock     = MakeUnique<SceneClock>();
    if (Has(ESvc::Tweens))    _tweens    = MakeUnique<TweenManager>();
    if (Has(ESvc::Sequences)) _sequences = MakeUnique<SequenceRunner>();
    if (Has(ESvc::Input))     _input     = MakeUnique<InputMap>();
    if (Has(ESvc::Camera2D))  _camera    = MakeUnique<acs::game::Camera2D>();
    if (Has(ESvc::Physics2D)) {
        _physics = MakeUnique<CollisionWorld2D>();
        _physics->Init();   // 既定 cell_size=64
    }
}

SceneClock& SceneServices::Clock() noexcept {
    ACS_ASSERTF(_clock.Get() != nullptr,
                "SceneServices::Clock() called but ESvc::Clock not requested in WantedServices()");
    return *_clock;
}

TweenManager& SceneServices::Tweens() noexcept {
    ACS_ASSERTF(_tweens.Get() != nullptr,
                "SceneServices::Tweens() called but ESvc::Tweens not requested in WantedServices()");
    return *_tweens;
}

SequenceRunner& SceneServices::Sequences() noexcept {
    ACS_ASSERTF(_sequences.Get() != nullptr,
                "SceneServices::Sequences() called but ESvc::Sequences not requested in WantedServices()");
    return *_sequences;
}

InputMap& SceneServices::Input() noexcept {
    ACS_ASSERTF(_input.Get() != nullptr,
                "SceneServices::Input() called but ESvc::Input not requested in WantedServices()");
    return *_input;
}

acs::game::Camera2D& SceneServices::Camera() noexcept {
    ACS_ASSERTF(_camera.Get() != nullptr,
                "SceneServices::Camera() called but ESvc::Camera2D not requested in WantedServices()");
    return *_camera;
}

CollisionWorld2D& SceneServices::Physics() noexcept {
    ACS_ASSERTF(_physics.Get() != nullptr,
                "SceneServices::Physics() called but ESvc::Physics2D not requested in WantedServices()");
    return *_physics;
}

void SceneServices::_PreUpdate(f32 raw_dt) noexcept {
    // Clock 進行 (= scaled dt 確定)。他サービスは scene.OnUpdate の後で tick。
    if (_clock) _clock->Tick(raw_dt);
}

void SceneServices::_PostUpdate(f32 scaled_dt) noexcept {
    if (_tweens)    _tweens->Tick(scaled_dt);
    if (_sequences) _sequences->Tick(scaled_dt);
    if (_camera)    _camera->Tick(scaled_dt);
}

f32 SceneServices::_ScaledDt(f32 raw_dt) const noexcept {
    return _clock ? _clock->Dt() : raw_dt;
}

} // namespace acs::game
