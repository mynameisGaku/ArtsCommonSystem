// SPDX-License-Identifier: Apache-2.0
// HelloTriangle — Application 派生クラス。
//
// 低レベル RHI トラックの 1 本目。FShaderDesc → CreateRhiShader、
// FBufferDesc → CreateRhiBuffer、FPipelineDesc → CreateRhiPipeline と
// 最小構成のオブジェクトを 1 度だけ用意し、毎フレーム同じ Draw を呼ぶ。
#pragma once

#include "app/Application.h"
#include "render/Render.h"
#include "memory/UniquePtr.h"

namespace hellotri {

class HelloTriangleApp : public acs::Application {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::TUniquePtr<acs::IRhiShader>   _vs;
    acs::TUniquePtr<acs::IRhiShader>   _ps;
    acs::TUniquePtr<acs::IRhiBuffer>   _vb;
    acs::TUniquePtr<acs::IRhiPipeline> _pipeline;
};

} // namespace hellotri
