// SPDX-License-Identifier: Apache-2.0
// HelloShadows — シーン描画ロジック。
// CApplication 派生はリソース所有 (sky / shader / shadow / sprite / font /
// camera) と入力ハンドリングを担当し、毎フレームの「シャドウパス → 主パス
// (CSky → ライト計算 → 地面 → キャスタ)」は ShadowsScene に委譲する。
#pragma once

#include "Types.h"

#include "container/Array.h"
#include "math/Camera.h"
#include "render/IRhiCommandList.h"
#include "render/RenderAssets.h"
#include "render/ShadowMap.h"
#include "render/Sky.h"
#include "render/StandardShader.h"

namespace helloshadows {

class CShadowsScene {
public:
    // 中央の 4 本の柱 + 周囲の 4 個の色違い球を生成。
    void Build() noexcept;

    // 1 フレームのシャドウパス + 主パス描画。
    //   sky      : 描画する CSky (App 所有、SunColor 取得用)
    //   shader   : 主パスの CStandardShader
    //   shadow   : シャドウマップ
    //   cl       : 現在の Render Pass の CommandList
    //   camera   : ViewProjection / Eye 取得用
    //   plane    : 床 (PrimitivePlane の GPU mesh)
    //   cube     : box の GPU mesh (柱用)
    //   sphere   : 球の GPU mesh
    //   sun_dir  : 太陽方向 (方向 TO 光源、Y 上向き)
    void Render(acs::CSky&             sky,
                acs::CStandardShader&  shader,
                acs::CShadowMap&       shadow,
                acs::IRhiCommandList& cl,
                const acs::CCamera&    camera,
                const acs::FGpuMesh&   plane,
                const acs::FGpuMesh&   cube,
                const acs::FGpuMesh&   sphere,
                acs::FVec3             sun_dir) noexcept;

private:
    acs::TArray<FCasterInst> m_Casters;
};

} // namespace helloshadows
