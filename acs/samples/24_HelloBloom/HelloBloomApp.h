// SPDX-License-Identifier: Apache-2.0
// HelloBloom — HDR シーン + Bloom + ACES Tonemap のミニデモ。
//
// OnCustomFrame() を override して HDR レンダーターゲット経由の描画パイプラインを
// 組む。HUD は Tonemap 後の LDR backbuffer に直接書く (HDR 値が HUD に乗らないように)。
// Bloom 強度は 1/2/3 キーで 3 段切替できる (Bloom の効きを目視確認しやすくするため)。
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

class HelloBloomApp : public acs::FApplication {
public:
    void OnStart() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    bool OnCustomFrame() noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::FPostProcess       _post;
    acs::FSky               _sky;
    acs::FStandardShader    _shader;
    acs::FSpriteBatch       _batch;
    acs::Font              _font;
    acs::GpuMesh           _gm_sphere;
    acs::GpuMesh           _gm_plane;
    acs::FCamera            _camera;
    acs::PostProcessParams _params;
    acs::f32               _cam_yaw = 0.5f;
    acs::f32               _angle   = 0.0f;
};

} // namespace hellobloom
