// SPDX-License-Identifier: Apache-2.0
// HelloLights — シーン描画ロジック。
// Application 派生はリソース所有 (shader / mesh / sprite / font / camera) と
// 入力ハンドリングを担当し、毎フレームの「ライト計算 + シーン描画」は
// LightsScene に委譲する。
#pragma once

#include "Types.h"

#include "container/Array.h"
#include "math/Camera.h"
#include "render/RenderAssets.h"
#include "render/StandardShader.h"
#include "render/IRhiCommandList.h"

namespace hellolights {

class LightsScene {
public:
    // 6 個のオブジェクト (cube/sphere mix) を円周上に配置。
    void Build() noexcept;

    // 1 フレームのライト計算 + 描画。
    //   shader     : 描画に使う StandardShader (App 所有)
    //   cl         : 現在の Render Pass の CommandList
    //   camera     : ViewProjection / Eye 取得用
    //   plane      : 床 (PrimitivePlane で生成済の GPU mesh)
    //   cube       : box の GPU mesh
    //   sphere     : sphere の GPU mesh
    //   time       : 経過秒 (光源の周回計算用)
    void Render(acs::StandardShader&    shader,
                acs::IRhiCommandList&   cl,
                const acs::Camera&      camera,
                const acs::GpuMesh&     plane,
                const acs::GpuMesh&     cube,
                const acs::GpuMesh&     sphere,
                acs::f32                time) noexcept;

private:
    acs::Array<ObjectInst> _objects;
};

} // namespace hellolights
