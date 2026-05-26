// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — SceneNodes 実装。
#include "SceneNodes.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace helloscene {

void PlayerNode::OnSpawn() noexcept {
    ACS_LOG_INFO("[SceneInspector] PlayerNode spawned (provider exposed)");
}

void PlayerNode::OnDespawn() noexcept {
    ACS_LOG_INFO("[SceneInspector] PlayerNode despawn");
}

u32 PlayerNode::ObjectCount() noexcept {
    return 1;
}

InspectableObject PlayerNode::GetObject(u32 /*index*/) noexcept {
    // fields 配列は Provider (= 本インスタンス) が所有する。InspectorSeam は
    // ポインタだけ握る (= コピーしない) ので、書き戻し時に Inspector の値が
    // そのまま _speed/_hp/... に反映される。
    _fields[0] = { "speed",      EFieldKind::F32,  &_speed,      0, nullptr };
    _fields[1] = { "hp",         EFieldKind::I32,  &_hp,         0, nullptr };
    _fields[2] = { "invincible", EFieldKind::Bool, &_invincible, 0, nullptr };
    _fields[3] = { "color",      EFieldKind::Vec3, &_color,      0, nullptr };
    _fields[4] = { "position",   EFieldKind::Vec2, &_position,   0, nullptr };

    InspectableObject obj{};
    obj.type_name     = "Player";
    obj.instance_name = "P1";
    obj.fields        = _fields;
    obj.field_count   = kFieldCount;
    return obj;
}

void PlayerNode::OnFieldChanged(u32 /*obj_index*/, u32 field_index) noexcept {
    // 値変更通知。本番ではここで clamp / 派生値再計算を行う想定 (例:
    // _hp を clamp(0, _max_hp) するなど)。サンプルでは log のみ。
    const char* name = (field_index < kFieldCount) ? _fields[field_index].name : "?";
    ACS_LOG_INFO("[SceneInspector] PlayerNode field changed: %s", name);
}

} // namespace helloscene
