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
    // fields 配列は Provider (= 本インスタンス) が所有する。FInspectorSeam は
    // ポインタだけ握る (= コピーしない) ので、書き戻し時に Inspector の値が
    // そのまま m_Speed/m_Hp/... に反映される。
    m_Fields[0] = { "speed",      EFieldKind::F32,  &m_Speed,      0, nullptr };
    m_Fields[1] = { "hp",         EFieldKind::I32,  &m_Hp,         0, nullptr };
    m_Fields[2] = { "invincible", EFieldKind::Bool, &m_bInvincible, 0, nullptr };
    m_Fields[3] = { "color",      EFieldKind::FVec3, &m_Color,      0, nullptr };
    m_Fields[4] = { "position",   EFieldKind::FVec2, &m_Position,   0, nullptr };

    InspectableObject obj{};
    obj.type_name     = "Player";
    obj.instance_name = "P1";
    obj.fields        = m_Fields;
    obj.field_count   = kFieldCount;
    return obj;
}

void PlayerNode::OnFieldChanged(u32 /*obj_index*/, u32 field_index) noexcept {
    // 値変更通知。本番ではここで clamp / 派生値再計算を行う想定 (例:
    // m_Hp を clamp(0, m_MaxHp) するなど)。サンプルでは log のみ。
    const char* name = (field_index < kFieldCount) ? m_Fields[field_index].name : "?";
    ACS_LOG_INFO("[SceneInspector] PlayerNode field changed: %s", name);
}

} // namespace helloscene
