// SPDX-License-Identifier: Apache-2.0
// HelloMesh — FApplication 派生クラス。
//
// 学習ポイント:
//   - 定数バッファ (b0) で MVP 行列を毎フレーム更新する流れ
//   - 深度バッファによる正しい遮蔽 (depth_test / depth_write)
//   - FPipelineDesc::cbuffer_slots / cull_mode の設定
#pragma once

#include "app/Application.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "math/Camera.h"
#include "memory/UniquePtr.h"

namespace hellomesh {

class FHelloMeshApp : public acs::FApplication {
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
    acs::TUniquePtr<acs::IRhiPipeline> m_Pipeline;

    acs::CCamera m_Camera;
    acs::f32    m_Angle   = 0.0f;
    acs::f32    m_CamYaw = 0.0f;
};

} // namespace hellomesh
