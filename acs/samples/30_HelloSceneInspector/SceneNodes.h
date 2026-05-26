// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — Scene 上に置く Node2D サブクラス。
//
// PlayerNode: Node2D かつ IInspectableProvider (= Inspector に field を公開する
//             典型実装)。fields は static 配列で保有し、`GetObject` で配列ポインタ
//             ごと返す。`OnFieldChanged` では値の clamp / 再バリデーションだけ行う
//             想定 (現状は Log だけ出す)。
// WheelNode:  回転する Node2D サブクラス (Hierarchy で複数階層を見せるため)。
#pragma once

#include "gameframework/GameFramework.h"
#include "gameframework/InspectorSeam.h"

#include "foundation/Log.h"

namespace helloscene {

class PlayerNode : public acs::game::Node2D, public acs::game::IInspectableProvider {
public:
    PlayerNode() noexcept = default;

    void OnSpawn() noexcept override {
        ACS_LOG_INFO("[SceneInspector] PlayerNode spawned (provider exposed)");
    }
    void OnDespawn() noexcept override {
        ACS_LOG_INFO("[SceneInspector] PlayerNode despawn");
    }

    // ---- IInspectableProvider 実装 ----
    acs::u32 ObjectCount() noexcept override { return 1; }

    acs::game::InspectableObject GetObject(acs::u32 /*index*/) noexcept override {
        // fields 配列の所有は Provider (= 本 PlayerNode インスタンス)。
        // InspectorSeam はコピーしない (InspectorSeam.h §53 設計選択)。
        _fields[0] = { "speed",      acs::game::EFieldKind::F32,  &_speed,      0, nullptr };
        _fields[1] = { "hp",         acs::game::EFieldKind::I32,  &_hp,         0, nullptr };
        _fields[2] = { "invincible", acs::game::EFieldKind::Bool, &_invincible, 0, nullptr };
        _fields[3] = { "color",      acs::game::EFieldKind::Vec3, &_color,      0, nullptr };
        _fields[4] = { "position",   acs::game::EFieldKind::Vec2, &_position,   0, nullptr };
        acs::game::InspectableObject obj{};
        obj.type_name     = "Player";
        obj.instance_name = "P1";
        obj.fields        = _fields;
        obj.field_count   = kFieldCount;
        return obj;
    }

    void OnFieldChanged(acs::u32 /*obj_index*/, acs::u32 field_index) noexcept override {
        // 値変更通知: 現状は log のみ。本番では clamp / 派生値再計算をここで行う。
        const char* name = (field_index < kFieldCount) ? _fields[field_index].name : "?";
        ACS_LOG_INFO("[SceneInspector] PlayerNode field changed: %s", name);
    }

private:
    static constexpr acs::u32 kFieldCount = 5;

    // Inspector で編集される public-ish state (`InspectableField::data` が指す)。
    acs::f32  _speed      = 5.0f;
    acs::i32  _hp         = 100;
    bool      _invincible = false;
    acs::Vec3 _color      { 0.2f, 0.8f, 0.3f };
    acs::Vec2 _position   { 0.0f, 0.0f };

    // 永続所有 fields (GetObject が毎回同じ配列のポインタを返す)。
    acs::game::InspectableField _fields[kFieldCount]{};
};

class WheelNode : public acs::game::Node2D {
public:
    explicit WheelNode(acs::f32 speed_rps) noexcept : _speed(speed_rps) {}
    void OnUpdate(acs::f32 dt) noexcept override {
        Local().rotation += _speed * dt;
    }
private:
    acs::f32 _speed;
};

} // namespace helloscene
