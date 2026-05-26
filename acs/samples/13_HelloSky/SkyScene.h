// SPDX-License-Identifier: Apache-2.0
// HelloSky — シーン描画ロジック。
// Application 派生はリソース所有 (sky / shader / sprite / font / camera) と
// 入力ハンドリングを担当し、毎フレームの「Sky → ライト計算 → 地面 + 球の
// 描画」は SkyScene に委譲する。
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
    // Sky の preset を切り替える。
    void SetPreset(acs::Sky& sky, SkyPreset p) noexcept;
    SkyPreset CurrentPreset() const noexcept { return _preset; }

    // 1 フレームの描画 (Sky → 地面 → 球)。
    //   sky     : 描画する Sky (App 所有、SunDirection / SunColor を使う)
    //   shader  : 描画に使う StandardShader (App 所有)
    //   cl      : 現在の Render Pass の CommandList
    //   camera  : ViewProjection / Eye / Sky 描画用
    //   plane   : 床 (PrimitivePlane の GPU mesh)
    //   sphere  : 球の GPU mesh
    //   angle   : 球の回転角 (経過秒 * 0.5)
    void Render(acs::Sky&             sky,
                acs::StandardShader&  shader,
                acs::IRhiCommandList& cl,
                const acs::Camera&    camera,
                const acs::GpuMesh&   plane,
                const acs::GpuMesh&   sphere,
                acs::f32              angle) noexcept;

private:
    SkyPreset _preset = SkyPreset::Day;
};

} // namespace hellosky
