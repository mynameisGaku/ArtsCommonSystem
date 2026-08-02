// SPDX-License-Identifier: Apache-2.0
// HelloLights — シーン描画ロジック。
//
// App 側 (リソース所有 + 入力) と毎フレームの計算ロジックを分けたいので、
// ライト計算と物体描画はここに分離する。リソースはすべて App から
// const ref で受け取って所有しない。
#pragma once

#include "Types.h"

#include "container/Array.h"
#include "math/Camera.h"
#include "render/RenderAssets.h"
#include "render/StandardShader.h"
#include "render/IRhiCommandList.h"

namespace hellolights {

class CLightsScene {
public:
    void Build() noexcept;

    // 1 フレームのライト計算 + 描画。リソースはすべて App から借りる。
    // time は光源の周回位相に使うため、外部時計のまま渡してもらう。
    void Render(acs::CStandardShader&    shader,
                acs::IRhiCommandList&   cl,
                const acs::CCamera&      camera,
                const acs::FGpuMesh&     plane,
                const acs::FGpuMesh&     cube,
                const acs::FGpuMesh&     sphere,
                acs::f32                time) noexcept;

private:
    acs::TArray<FObjectInst> m_Objects;
};

} // namespace hellolights
