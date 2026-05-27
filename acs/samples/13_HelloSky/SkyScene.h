// SPDX-License-Identifier: Apache-2.0
// HelloSky — シーン描画ロジック。
// 「リソース所有 / 入力」と「フレーム描画」を分離するため、App 側に持たせず
// 別クラスにしている。テストで描画だけ差し替えやすくする狙いもある。
#pragma once

#include "Types.h"

#include "math/Camera.h"
#include "render/IRhiCommandList.h"
#include "render/RenderAssets.h"
#include "render/Sky.h"
#include "render/StandardShader.h"

namespace hellosky {

class SkyScene {
public:
    void SetPreset(acs::FSky& sky, SkyPreset p) noexcept;
    SkyPreset CurrentPreset() const noexcept { return m_Preset; }

    // 1 フレームの描画 (FSky → 地面 → 球)。
    // sky / shader / camera / mesh は App が所有、引数で借りる形にして
    // SkyScene 側を状態の少ない関数オブジェクトに保つ。
    void Render(acs::FSky&             sky,
                acs::FStandardShader&  shader,
                acs::IRhiCommandList& cl,
                const acs::FCamera&    camera,
                const acs::GpuMesh&   plane,
                const acs::GpuMesh&   sphere,
                acs::f32              angle) noexcept;

private:
    SkyPreset m_Preset = SkyPreset::Day;
};

} // namespace hellosky
