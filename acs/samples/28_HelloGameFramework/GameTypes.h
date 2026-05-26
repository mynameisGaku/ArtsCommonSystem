// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — 共通型 (PlayerProfile + RotateComponent + RotatingNode)
//
// シーン跨ぎ永続状態 (AppState) と、複数 scene で参照される Node2D / Component
// サブクラスをこのヘッダにまとめる。`inline` で複数 TU に include しても 1
// ストレージに resolve される。
#pragma once

#include "gameframework/GameFramework.h"
#include "foundation/Log.h"

namespace hellogf {

// シーン跨ぎ永続状態 (AppState のサンプル)
struct PlayerProfile {
    acs::u32 hi_score = 0;
    acs::u32 sessions = 0;
};

// Phase 7: 同じ「回転させる」振る舞いを **コンポーネント** で実装したもの。
// 継承 (RotatingNode) では Node2D を Subclass 化する必要があったが、
// Component2D で書けばプレーン Node2D の attach 経由で同じ機能が手に入る。
class RotateComponent : public acs::game::Component2D {
public:
    ACS_GAME_COMPONENT_KIND(RotateComponent)
    explicit RotateComponent(acs::f32 speed_rps) noexcept : _speed(speed_rps) {}

    void OnAttach(acs::game::Node2D& /*owner*/) noexcept override {
        ACS_LOG_INFO("[Component] RotateComponent attached (speed=%.2f rad/s)",
                     static_cast<double>(_speed));
    }
    void OnUpdate(acs::f32 dt) noexcept override {
        Owner().Local().rotation += _speed * dt;
    }
    void OnDetach() noexcept override {
        ACS_LOG_INFO("[Component] RotateComponent detached");
    }

private:
    acs::f32 _speed;
};

// Phase 5: 回転する Node2D サブクラス (Local().rotation を毎フレーム加算)。
// OnSpawn でログ、OnDespawn でログを出して lifecycle を観察できる。
class RotatingNode : public acs::game::Node2D {
public:
    explicit RotatingNode(acs::f32 speed_rps, const char* label) noexcept
        : _speed(speed_rps), _label(label) {}

    void OnSpawn() noexcept override {
        const auto w = World();
        ACS_LOG_INFO("[Node] %s spawned at world (%.2f, %.2f)",
                     _label, static_cast<double>(w.position.x),
                     static_cast<double>(w.position.y));
    }
    void OnUpdate(acs::f32 dt) noexcept override {
        Local().rotation += _speed * dt;
    }
    void OnDespawn() noexcept override {
        ACS_LOG_INFO("[Node] %s despawn", _label);
    }

private:
    acs::f32    _speed;
    const char* _label;
};

} // namespace hellogf
