// SPDX-License-Identifier: Apache-2.0
// HelloShadows — シーン描画ロジック。
// FApplication 派生はリソース所有 (sky / shader / shadow / sprite / font /
// camera) と入力ハンドリングを担当し、毎フレームの「シャドウパス → 主パス
// (FSky → ライト計算 → 地面 → キャスタ)」は ShadowsScene に委譲する。
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

class ShadowsScene {
public:
    // 中央の 4 本の柱 + 周囲の 4 個の色違い球を生成。
    void Build() noexcept;

    // 1 フレームのシャドウパス + 主パス描画。
    //   sky      : 描画する FSky (App 所有、SunColor 取得用)
    //   shader   : 主パスの FStandardShader
    //   shadow   : シャドウマップ
    //   cl       : 現在の Render Pass の CommandList
    //   camera   : ViewProjection / Eye 取得用
    //   plane    : 床 (PrimitivePlane の GPU mesh)
    //   cube     : box の GPU mesh (柱用)
    //   sphere   : 球の GPU mesh
    //   sun_dir  : 太陽方向 (方向 TO 光源、Y 上向き)
    void Render(acs::FSky&             sky,
                acs::FStandardShader&  shader,
                acs::FShadowMap&       shadow,
                acs::IRhiCommandList& cl,
                const acs::FCamera&    camera,
                const acs::GpuMesh&   plane,
                const acs::GpuMesh&   cube,
                const acs::GpuMesh&   sphere,
                acs::FVec3             sun_dir) noexcept;

private:
    acs::TArray<CasterInst> m_Casters;
};

} // namespace helloshadows
