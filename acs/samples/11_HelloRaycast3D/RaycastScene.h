// SPDX-License-Identifier: Apache-2.0
// HelloRaycast3D — シーンのオーケストレーション。
//
// 役割:
//   ・GPU メッシュ (球 / 立方体 / 地面) の所有
//   ・RaycastTargets + RayCaster + HudRenderer の協調
//   ・FStandardShader への描画コマンド発行
//
// FApplication 派生はリソース所有 (FStandardShader / FSpriteBatch / FFont) を担当し、
// 毎フレームの update / render を RaycastScene に委譲する。
#pragma once

#include "Types.h"
#include "RaycastTargets.h"
#include "RayCaster.h"

#include "render/StandardShader.h"
#include "render/RenderAssets.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/IRhiCommandList.h"

namespace helloraycast3d {

class RaycastScene {
public:
    // プリミティブを GPU にアップロードしてカメラ + オブジェクト配置を構成。
    bool Init(acs::IRhiDevice& dev, acs::f32 aspect) noexcept;

    void Shutdown() noexcept;

    // カメラ入力 + レイキャスト判定。
    void Update(acs::f32 dt) noexcept;

    // 3D + HUD を描画。HUD 用の screen_w / screen_h は FSpriteBatch::Begin に必要。
    void Render(acs::FStandardShader& shader,
                acs::IRhiCommandList& cl,
                acs::FSpriteBatch& batch,
                acs::FFont& font,
                acs::u32 screen_w,
                acs::u32 screen_h) noexcept;

private:
    void _render_targets(acs::FStandardShader& shader,
                         acs::IRhiCommandList& cl) noexcept;

    acs::FGpuMesh    _gm_sphere{};
    acs::FGpuMesh    _gm_cube{};
    acs::FGpuMesh    _gm_plane{};
    RaycastTargets  _targets;
    RayCaster       _caster;
};

} // namespace helloraycast3d
