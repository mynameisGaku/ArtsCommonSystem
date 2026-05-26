// SPDX-License-Identifier: Apache-2.0
// HelloTextured — Application 派生クラス。
//
// 学習ポイント:
//   - IRhiTexture 作成 → SetTexture でバインド
//   - FPipelineDesc::static_samplers で固定サンプラを指定 (PSO に焼き付け、bind 不要)
//   - PixelShader からテクスチャ・サンプラを読む HLSL 構文
#pragma once

#include "app/Application.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiTexture.h"
#include "render/IRhiSampler.h"
#include "math/Camera.h"
#include "memory/UniquePtr.h"

namespace hellotextured {

class HelloTexturedApp : public acs::Application {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::TUniquePtr<acs::IRhiShader>   _vs;
    acs::TUniquePtr<acs::IRhiShader>   _ps;
    acs::TUniquePtr<acs::IRhiBuffer>   _vb;
    acs::TUniquePtr<acs::IRhiBuffer>   _ib;
    acs::TUniquePtr<acs::IRhiBuffer>   _cb;
    acs::TUniquePtr<acs::IRhiTexture>  _tex;
    acs::TUniquePtr<acs::IRhiPipeline> _pipeline;

    acs::FCamera _camera;
    acs::f32    _angle   = 0.0f;
    acs::f32    _cam_yaw = 0.0f;
};

} // namespace hellotextured
