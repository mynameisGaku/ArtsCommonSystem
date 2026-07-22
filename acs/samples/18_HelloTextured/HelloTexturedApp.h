// SPDX-License-Identifier: Apache-2.0
// HelloTextured — FApplication 派生クラス。
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

class FHelloTexturedApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::TUniquePtr<acs::IRhiShader>   m_Vs;
    acs::TUniquePtr<acs::IRhiShader>   m_Ps;
    acs::TUniquePtr<acs::IRhiBuffer>   m_Vb;
    acs::TUniquePtr<acs::IRhiBuffer>   m_Ib;
    acs::TUniquePtr<acs::IRhiBuffer>   m_Cb;
    acs::TUniquePtr<acs::IRhiTexture>  m_Tex;
    acs::TUniquePtr<acs::IRhiPipeline> m_Pipeline;

    acs::FCamera m_Camera;
    acs::f32    m_Angle   = 0.0f;
    acs::f32    m_CamYaw = 0.0f;
};

} // namespace hellotextured
