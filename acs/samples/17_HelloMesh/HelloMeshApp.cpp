// SPDX-License-Identifier: Apache-2.0
// HelloMesh — FApplication 実装。
#include "HelloMeshApp.h"
#include "Shaders.h"
#include "Types.h"

#include "platform/Input.h"
#include "math/Mat.h"
#include "foundation/Log.h"

using namespace acs;

namespace hellomesh {

void FHelloMeshApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    // === シェーダ ===
    FShaderDesc vs_desc{};
    vs_desc.stage = EShaderStage::Vertex;
    vs_desc.hlsl_source = kHLSL;
    vs_desc.entry_point = "VSMain";
    vs_desc.debug_name  = "Mesh.VS";
    if (auto r = CreateRhiShader(*dev, vs_desc); r.IsErr()) {
        ACS_LOG_ERROR("VS compile: %s", r.Error().message); Quit(); return;
    } else m_Vs = Move(r.Value());

    FShaderDesc ps_desc{};
    ps_desc.stage = EShaderStage::Pixel;
    ps_desc.hlsl_source = kHLSL;
    ps_desc.entry_point = "PSMain";
    ps_desc.debug_name  = "Mesh.PS";
    if (auto r = CreateRhiShader(*dev, ps_desc); r.IsErr()) {
        ACS_LOG_ERROR("PS compile: %s", r.Error().message); Quit(); return;
    } else m_Ps = Move(r.Value());

    // === 頂点 / インデックスバッファ ===
    FBufferDesc vb_desc{};
    vb_desc.size = sizeof(kCubeVertices);
    vb_desc.usage = EBufferUsage::Vertex;
    vb_desc.cpu_writable = true;
    vb_desc.initial_data = kCubeVertices;
    if (auto r = CreateRhiBuffer(*dev, vb_desc); r.IsErr()) {
        ACS_LOG_ERROR("VB create: %s", r.Error().message); Quit(); return;
    } else m_Vb = Move(r.Value());

    FBufferDesc ib_desc{};
    ib_desc.size = sizeof(kCubeIndices);
    ib_desc.usage = EBufferUsage::Index16;
    ib_desc.cpu_writable = true;
    ib_desc.initial_data = kCubeIndices;
    if (auto r = CreateRhiBuffer(*dev, ib_desc); r.IsErr()) {
        ACS_LOG_ERROR("IB create: %s", r.Error().message); Quit(); return;
    } else m_Ib = Move(r.Value());

    // === 定数バッファ (MVP) 256B にアライン ===
    FBufferDesc cb_desc{};
    cb_desc.size = 256;
    cb_desc.usage = EBufferUsage::Uniform;
    cb_desc.cpu_writable = true;
    if (auto r = CreateRhiBuffer(*dev, cb_desc); r.IsErr()) {
        ACS_LOG_ERROR("CB create: %s", r.Error().message); Quit(); return;
    } else m_Cb = Move(r.Value());

    // === パイプライン ===
    FPipelineDesc pd{};
    pd.vs = m_Vs.Get();
    pd.ps = m_Ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = GetRenderer().ColorFormat();
    pd.depth_format  = GetRenderer().DepthFormat();
    pd.depth_test    = true;
    pd.depth_write   = true;
    pd.cull_mode     = ECullMode::Back;
    pd.cbuffer_slots = 1;       // b0 = MVP
    pd.cbuffer_names[0] = "Frame";  // Diligent では cbuffer 名で resolve するため必須
    pd.vertex_stride = sizeof(FVertex);
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0 };
    pd.layout[1] = { "COLOR",    0, EFormat::R32G32B32_Float, sizeof(f32) * 3 };
    pd.layout_count = 2;
    if (auto r = CreateRhiPipeline(*dev, pd); r.IsErr()) {
        ACS_LOG_ERROR("Pipeline create: %s", r.Error().message); Quit(); return;
    } else m_Pipeline = Move(r.Value());

    // === カメラ ===
    const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                       static_cast<f32>(GetRenderer().Swapchain()->Height());
    m_Camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);

    ACS_LOG_INFO("HelloMesh initialized");
}

void FHelloMeshApp::OnUpdate(f32 dt) noexcept {
    if (FInput::IsKeyPressed(EKey::Escape)) Quit();

    m_Angle += dt * 0.8f;

    // 矢印キーでカメラを左右回転 (キューブを公転する視点)
    if (FInput::IsKeyDown(EKey::Left))  m_CamYaw -= dt * 1.5f;
    if (FInput::IsKeyDown(EKey::Right)) m_CamYaw += dt * 1.5f;

    const f32 cam_dist = 5.0f;
    FVec3 eye{ Sin(m_CamYaw) * cam_dist, 2.0f, -Cos(m_CamYaw) * cam_dist };
    m_Camera.SetLookAt(eye, {0, 0, 0});

    FMat4 model = FMat4::RotationY(m_Angle);
    FMat4 mvp   = model * m_Camera.View() * m_Camera.Projection();
    m_Cb->Update(&mvp, sizeof(FMat4));
}

void FHelloMeshApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl || !m_Pipeline) return;

    cl->SetPipeline(*m_Pipeline);
    cl->SetConstantBuffer(0, *m_Cb);
    cl->SetVertexBuffer(*m_Vb, sizeof(FVertex));
    cl->SetIndexBuffer(*m_Ib);
    cl->DrawIndexed(36);
}

void FHelloMeshApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    m_Pipeline.Reset();
    m_Cb.Reset();
    m_Ib.Reset();
    m_Vb.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
}

} // namespace hellomesh
