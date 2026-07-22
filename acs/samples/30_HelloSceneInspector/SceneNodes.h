// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — Scene 上に置く ANode サブクラス。
//
// APlayerNode: Inspector に field を公開する典型実装 (ANode + IInspectableProvider
//             多重継承)。fields 配列は Provider が所有し、FInspectorSeam にはコピー
//             せずポインタだけ渡す契約 (FInspectorSeam.h 設計選択)。
// AWheelNode:  回転する ANode サブクラス。Hierarchy で複数階層の見え方を
//             デモするために用意。
#pragma once

#include "gameframework/GameFramework.h"
#include "gameframework/InspectorSeam.h"

namespace helloscene {

class APlayerNode : public acs::game::ANode, public acs::game::IInspectableProvider {
public:
    APlayerNode() noexcept = default;

    void OnSpawn()    noexcept override;
    void OnDespawn()  noexcept override;

    // ---- IInspectableProvider 実装 ----
    acs::u32                     ObjectCount()                                       noexcept override;
    acs::game::FInspectableObject GetObject(acs::u32 index)                            noexcept override;
    void                         OnFieldChanged(acs::u32 obj_index, acs::u32 field_index) noexcept override;

private:
    static constexpr acs::u32 kFieldCount = 5;

    // Inspector で編集される player の state (`InspectableField::data` が指す)。
    acs::f32  m_Speed      = 5.0f;
    acs::i32  m_Hp         = 100;
    bool      m_bInvincible = false;
    acs::FVec3 m_Color      { 0.2f, 0.8f, 0.3f };
    acs::FVec2 m_Position   { 0.0f, 0.0f };

    // GetObject が毎回同じ配列のポインタを返すため、永続所有する。
    acs::game::FInspectableField m_Fields[kFieldCount]{};
};

// 自転する ANode。Hierarchy 表示で親子関係 (wheel ← spoke) を見せる役。
class AWheelNode : public acs::game::ANode {
public:
    explicit AWheelNode(acs::f32 speed_rps) noexcept : m_SpeedRps(speed_rps) {}

    void OnUpdate(acs::f32 dt) noexcept override {
        SetRotation2D(Rotation2D() + m_SpeedRps * dt);
    }

private:
    acs::f32 m_SpeedRps;
};

} // namespace helloscene
