// SPDX-License-Identifier: Apache-2.0
// HelloTriangle — Application 実装。
#include "HelloTriangleApp.h"
#include "Shaders.h"
#include "Types.h"

#include "platform/Input.h"
#include "foundation/Log.h"

using namespace acs;

namespace hellotri {

void HelloTriangleApp::OnStart() noexcept {
    // 1. シェーダコンパイル (VS + PS、同じ HLSL ソースから entry を変えて 2 回)
    ShaderDesc vs_desc{};
    vs_desc.stage = EShaderStage::Vertex;
    vs_desc.hlsl_source = kHLSL;
    vs_desc.entry_point = "VSMain";
    vs_desc.debug_name  = "Triangle.VS";
    auto vs_r = CreateRhiShader(*GetRenderer().Device(), vs_desc);
    if (vs_r.IsErr()) {
        ACS_LOG_ERROR("VS compile failed: %s", vs_r.Error().message);
        Quit();
        return;
    }
    _vs = Move(vs_r.Value());

    ShaderDesc ps_desc{};
    ps_desc.stage = EShaderStage::Pixel;
    ps_desc.hlsl_source = kHLSL;
    ps_desc.entry_point = "PSMain";
    ps_desc.debug_name  = "Triangle.PS";
    auto ps_r = CreateRhiShader(*GetRenderer().Device(), ps_desc);
    if (ps_r.IsErr()) {
        ACS_LOG_ERROR("PS compile failed: %s", ps_r.Error().message);
        Quit();
        return;
    }
    _ps = Move(ps_r.Value());

    // 2. 頂点バッファ作成 (3 頂点ぶん)
    BufferDesc vb_desc{};
    vb_desc.size = sizeof(kTriangleVertices);
    vb_desc.usage = EBufferUsage::Vertex;
    vb_desc.cpu_writable = true;
    vb_desc.initial_data = kTriangleVertices;
    auto vb_r = CreateRhiBuffer(*GetRenderer().Device(), vb_desc);
    if (vb_r.IsErr()) {
        ACS_LOG_ERROR("Vertex buffer create failed");
        Quit();
        return;
    }
    _vb = Move(vb_r.Value());

    // 3. パイプライン作成 (VS + PS + 入力レイアウト)
    PipelineDesc pd{};
    pd.vs = _vs.Get();
    pd.ps = _ps.Get();
    pd.topology = EPrimitiveTopology::TriangleList;
    pd.rt_format = EFormat::B8G8R8A8_UNorm;
    pd.depth_format = EFormat::Unknown;
    pd.vertex_stride = sizeof(Vertex);
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0 };
    pd.layout[1] = { "COLOR",    0, EFormat::R32G32B32_Float, sizeof(f32) * 3 };
    pd.layout_count = 2;
    auto pl_r = CreateRhiPipeline(*GetRenderer().Device(), pd);
    if (pl_r.IsErr()) {
        ACS_LOG_ERROR("Pipeline create failed");
        Quit();
        return;
    }
    _pipeline = Move(pl_r.Value());

    ACS_LOG_INFO("HelloTriangle initialized");
}

void HelloTriangleApp::OnUpdate(f32 /*dt*/) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
}

void HelloTriangleApp::OnRender() noexcept {
    // BeginFrame は基底クラスが先に呼んでくれる (クリア済み)。
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl || !_pipeline || !_vb) return;

    cl->SetPipeline(*_pipeline);
    cl->SetVertexBuffer(*_vb, sizeof(Vertex));
    cl->Draw(3);
}

void HelloTriangleApp::OnShutdown() noexcept {
    // GPU が描画完了するまで待ってからリソースを解放
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    _pipeline.Reset();
    _vb.Reset();
    _ps.Reset();
    _vs.Reset();
}

} // namespace hellotri
