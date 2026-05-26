// SPDX-License-Identifier: Apache-2.0
// HelloMesh — Application 派生クラス。
//
// 学習ポイント:
//   - 定数バッファ (b0) で MVP 行列を毎フレーム更新する流れ
//   - 深度バッファによる正しい遮蔽 (depth_test / depth_write)
//   - PipelineDesc::cbuffer_slots / cull_mode の設定
#pragma once

#include "app/Application.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "math/Camera.h"
#include "memory/UniquePtr.h"

namespace hellomesh {

class HelloMeshApp : public acs::Application {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::UniquePtr<acs::IRhiShader>   _vs;
    acs::UniquePtr<acs::IRhiShader>   _ps;
    acs::UniquePtr<acs::IRhiBuffer>   _vb;
    acs::UniquePtr<acs::IRhiBuffer>   _ib;
    acs::UniquePtr<acs::IRhiBuffer>   _cb;
    acs::UniquePtr<acs::IRhiPipeline> _pipeline;

    acs::Camera _camera;
    acs::f32    _angle   = 0.0f;
    acs::f32    _cam_yaw = 0.0f;
};

} // namespace hellomesh
