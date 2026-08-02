// SPDX-License-Identifier: Apache-2.0
#include "ViewerScenePipeline.h"
#include "ViewerCubeAssets.h"

#include "render/Renderer.h"
#include "render/IRhiCommandList.h"
#include "foundation/Log.h"

using namespace acs;

namespace hellomv {

bool CViewerScenePipeline::Init(CRenderer& renderer) noexcept {
    IRhiDevice* dev = renderer.Device();
    if (!dev) {
        ACS_LOG_ERROR("[ModelViewer] Device() == null");
        return false;
    }

    // ---- VS / PS ----
    FShaderDesc vs_desc{};
    vs_desc.stage       = EShaderStage::Vertex;
    vs_desc.hlsl_source = kHLSL;
    vs_desc.entry_point = "VSMain";
    vs_desc.debug_name  = "ModelViewer.VS";
    if (auto r = CreateRhiShader(*dev, vs_desc); r.IsErr()) {
        ACS_LOG_ERROR("[ModelViewer] VS compile failed");
        return false;
    } else { m_Vs = Move(r.Value()); }

    FShaderDesc ps_desc{};
    ps_desc.stage       = EShaderStage::Pixel;
    ps_desc.hlsl_source = kHLSL;
    ps_desc.entry_point = "PSMain";
    ps_desc.debug_name  = "ModelViewer.PS";
    if (auto r = CreateRhiShader(*dev, ps_desc); r.IsErr()) {
        ACS_LOG_ERROR("[ModelViewer] PS compile failed");
        return false;
    } else { m_Ps = Move(r.Value()); }

    // ---- VB / IB ----
    FBufferDesc vb_desc{};
    vb_desc.size = sizeof(kCubeVertices);
    vb_desc.usage = EBufferUsage::Vertex;
    vb_desc.cpu_writable = true;
    vb_desc.initial_data = kCubeVertices;
    if (auto r = CreateRhiBuffer(*dev, vb_desc); r.IsErr()) {
        ACS_LOG_ERROR("[ModelViewer] VB create failed");
        return false;
    } else { m_Vb = Move(r.Value()); }

    FBufferDesc ib_desc{};
    ib_desc.size = sizeof(kCubeIndices);
    ib_desc.usage = EBufferUsage::Index16;
    ib_desc.cpu_writable = true;
    ib_desc.initial_data = kCubeIndices;
    if (auto r = CreateRhiBuffer(*dev, ib_desc); r.IsErr()) {
        ACS_LOG_ERROR("[ModelViewer] IB create failed");
        return false;
    } else { m_Ib = Move(r.Value()); }

    // 定数バッファ (MVP 1 個分、256B アライン)。
    FBufferDesc cb_desc{};
    cb_desc.size = 256;
    cb_desc.usage = EBufferUsage::Uniform;
    cb_desc.cpu_writable = true;
    if (auto r = CreateRhiBuffer(*dev, cb_desc); r.IsErr()) {
        ACS_LOG_ERROR("[ModelViewer] CB create failed");
        return false;
    } else { m_Cb = Move(r.Value()); }

    // ---- Pipeline ----
    FPipelineDesc pd{};
    pd.vs = m_Vs.Get();
    pd.ps = m_Ps.Get();
    pd.topology         = EPrimitiveTopology::TriangleList;
    pd.rt_format        = renderer.ColorFormat();
    pd.depth_format     = renderer.DepthFormat();
    pd.depth_test       = true;
    pd.depth_write      = true;
    pd.cull_mode        = ECullMode::Back;
    pd.cbuffer_slots    = 1;
    pd.cbuffer_names[0] = "Frame";
    pd.vertex_stride    = sizeof(FVertex);
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0 };
    pd.layout[1] = { "COLOR",    0, EFormat::R32G32B32_Float, sizeof(f32) * 3 };
    pd.layout_count = 2;
    if (auto r = CreateRhiPipeline(*dev, pd); r.IsErr()) {
        ACS_LOG_ERROR("[ModelViewer] Pipeline create failed");
        return false;
    } else { m_Pipeline = Move(r.Value()); }

    return true;
}

void CViewerScenePipeline::Shutdown() noexcept {
    // GPU 側参照が消えていることは caller (Scene の WaitIdle) 側で保証する。
    // ここでは順序だけ揃えて Release: pipeline → buffer → shader。
    m_Pipeline.Reset();
    m_Cb.Reset();
    m_Ib.Reset();
    m_Vb.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
}

void CViewerScenePipeline::UpdateMvp(const FMat4& view, const FMat4& proj, f32 angle) noexcept {
    if (!m_Cb) return;
    const FMat4 model = FMat4::RotationY(angle);
    const FMat4 mvp = model * view * proj;
    m_Cb->Update(&mvp, sizeof(FMat4));
}

void CViewerScenePipeline::Render(IRhiCommandList& cl) noexcept {
    if (!m_Pipeline) return;
    cl.SetPipeline(*m_Pipeline);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.SetVertexBuffer(*m_Vb, sizeof(FVertex));
    cl.SetIndexBuffer(*m_Ib);
    cl.DrawIndexed(36);
}

} // namespace hellomv
