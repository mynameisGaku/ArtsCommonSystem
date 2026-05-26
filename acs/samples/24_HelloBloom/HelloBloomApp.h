// SPDX-License-Identifier: Apache-2.0
// HelloBloom — Application 派生クラス。
//
// HDR シーン + Bloom + ACES Tonemap のミニデモ。OnCustomFrame() を override
// して HDR RT 経由のレンダリングを構築する。Bloom 強度を 3 段切替できる。
//
// メンバ:
//   PostProcess    : HDR RT + Bloom mip chain + Tonemap pipeline を内包する。
//                    Render() が swapchain への合成まで担当する。
//   Sky            : Night preset の手続き的空。HDR RT format に合わせて Init。
//   StandardShader : シーン本体 (地面 + HDR 球) の描画。RT format = HDR。
//   SpriteBatch    : HUD を Tonemap 後の LDR backbuffer に直書きする。
//   Font           : 18px UI フォント。AtlasTexture が null のときは HUD 描画を skip。
//   GpuMesh        : 球と平面の VB/IB を保持。
//   Camera         : 60° FOV、半径 7 の周回視点。
//   PostProcessParams : Bloom intensity を 3 段で切替 (0 / 0.6 / 1.5)。
#pragma once

#include "app/Application.h"
#include "render/StandardShader.h"
#include "render/RenderAssets.h"
#include "render/Sky.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/PostProcess.h"
#include "math/Camera.h"
#include "foundation/Types.h"

namespace hellobloom {

class HelloBloomApp : public acs::Application {
public:
    void OnStart() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    bool OnCustomFrame() noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::PostProcess       _post;
    acs::Sky               _sky;
    acs::StandardShader    _shader;
    acs::SpriteBatch       _batch;
    acs::Font              _font;
    acs::GpuMesh           _gm_sphere;
    acs::GpuMesh           _gm_plane;
    acs::Camera            _camera;
    acs::PostProcessParams _params;
    acs::f32               _cam_yaw = 0.5f;
    acs::f32               _angle   = 0.0f;
};

} // namespace hellobloom
