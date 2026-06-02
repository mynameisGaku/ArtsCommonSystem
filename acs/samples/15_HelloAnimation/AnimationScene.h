// SPDX-License-Identifier: Apache-2.0
// HelloAnimation — シーン描画ロジック + スキンメッシュ手続き生成。
// FApplication からリソース参照を受け取り「FSky → 地面 → スキンメッシュ」を 1
// フレーム分まとめて描く。状態は持たず純粋な描画ユーティリティに近い。
#pragma once

#include "Types.h"

#include "asset/SkinnedMesh.h"
#include "math/Camera.h"
#include "math/Mat.h"
#include "memory/SharedPtr.h"
#include "render/IRhiCommandList.h"
#include "render/RenderAssets.h"
#include "render/SkinnedShader.h"
#include "render/Sky.h"
#include "render/StandardShader.h"

namespace helloanim {

class AnimationScene {
public:
    // 4 ボーンの「ヘビ風」円柱メッシュを手続き生成 (アニメ含む)。
    // 失敗時は空の TSharedPtr を返す (呼び出し元で null チェック)。
    static acs::TSharedPtr<acs::FSkinnedMeshAsset> BuildSnake() noexcept;

    // 1 フレームの描画 (FSky → 地面 → スキンメッシュ)。
    //   sky        : 描画する FSky (App 所有、SunDirection / SunColor 取得用)
    //   std_shader : 地面用 FStandardShader
    //   skin_shader: スキンメッシュ用 FSkinnedShader (palette を流す)
    //   cl         : 現在の Render Pass の CommandList
    //   camera     : ViewProjection / Eye 取得用
    //   plane      : 床 (PrimitivePlane の GPU mesh)
    //   snake_gpu  : スキンメッシュ GPU 表現
    //   palette    : 現在のボーンパレット (bones の世界変換)
    //   palette_n  : palette の有効ボーン数
    void Render(acs::FSky&                sky,
                acs::FStandardShader&     std_shader,
                acs::FSkinnedShader&      skin_shader,
                acs::IRhiCommandList&    cl,
                const acs::FCamera&       camera,
                const acs::GpuMesh&      plane,
                const acs::SkinnedGpuMesh& snake_gpu,
                const acs::FMat4*         palette,
                acs::u32                 palette_n) noexcept;
};

} // namespace helloanim
