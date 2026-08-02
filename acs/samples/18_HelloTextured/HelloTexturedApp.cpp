// SPDX-License-Identifier: Apache-2.0
// HelloTextured — CApplication 実装。
#include "HelloTexturedApp.h"
#include "Shaders.h"
#include "TextureGen.h"
#include "Types.h"

#include "platform/Input.h"
#include "math/Mat.h"
#include "foundation/Log.h"

using namespace acs;

namespace hellotextured {

void CHelloTexturedApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    // === シェーダ ===
    FShaderDesc vs_desc{};
    vs_desc.stage = EShaderStage::Vertex;
    vs_desc.hlsl_source = kHLSL;
    vs_desc.entry_point = "VSMain";
    vs_desc.debug_name  = "Tex.VS";
    if (auto r = CreateRhiShader(*dev, vs_desc); r.IsErr()) {
        ACS_LOG_ERROR("VS: %s", r.Error().message); Quit(); return;
    } else m_Vs = Move(r.Value());

    FShaderDesc ps_desc{};
    ps_desc.stage = EShaderStage::Pixel;
    ps_desc.hlsl_source = kHLSL;
    ps_desc.entry_point = "PSMain";
    ps_desc.debug_name  = "Tex.PS";
    if (auto r = CreateRhiShader(*dev, ps_desc); r.IsErr()) {
        ACS_LOG_ERROR("PS: %s", r.Error().message); Quit(); return;
    } else m_Ps = Move(r.Value());

    // === バッファ ===
    FBufferDesc vb{};
    vb.size = sizeof(kCubeVertices);
    vb.usage = EBufferUsage::Vertex; vb.cpu_writable = true;
    vb.initial_data = kCubeVertices;
    if (auto r = CreateRhiBuffer(*dev, vb); r.IsErr()) {
        ACS_LOG_ERROR("頂点バッファ作成に失敗: %s", r.Error().message); Quit(); return;
    }
    else m_Vb = Move(r.Value());

    FBufferDesc ib{};
    ib.size = sizeof(kCubeIndices);
    ib.usage = EBufferUsage::Index16; ib.cpu_writable = true;
    ib.initial_data = kCubeIndices;
    if (auto r = CreateRhiBuffer(*dev, ib); r.IsErr()) {
        ACS_LOG_ERROR("インデックスバッファ作成に失敗: %s", r.Error().message); Quit(); return;
    }
    else m_Ib = Move(r.Value());

    FBufferDesc cb_d{};
    cb_d.size = 256; cb_d.usage = EBufferUsage::Uniform; cb_d.cpu_writable = true;
    if (auto r = CreateRhiBuffer(*dev, cb_d); r.IsErr()) {
        ACS_LOG_ERROR("定数バッファ作成に失敗: %s", r.Error().message); Quit(); return;
    }
    else m_Cb = Move(r.Value());

    // === テクスチャ (手続き生成) ===
    // CPU 側で kTexSize x kTexSize の RGBA8 を組み立て、initial_data で 1 度に GPU へ転送する。
    u8 pixels[kTexSize * kTexSize * 4];
    GenerateTexture(pixels);

    FTextureDesc td{};
    td.width  = kTexSize;
    td.height = kTexSize;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = pixels;
    td.initial_data_size = sizeof(pixels);
    if (auto r = CreateRhiTexture(*dev, td); r.IsErr()) {
        ACS_LOG_ERROR("Texture create: %s", r.Error().message); Quit(); return;
    } else m_Tex = Move(r.Value());

    // === パイプライン (CB 1 + Texture 1 + Sampler 1) ===
    FPipelineDesc pd{};
    pd.vs = m_Vs.Get();
    pd.ps = m_Ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = GetRenderer().ColorFormat();
    pd.depth_format  = GetRenderer().DepthFormat();
    pd.depth_test    = true;
    pd.depth_write   = true;
    pd.cull_mode     = ECullMode::Back;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 1;
    // 固定サンプラ: PSO に焼き付けるので、描画ごとに SetSampler する必要が無い。
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter      = ESamplerFilter::Point;     // ピクセルアートっぽい外観にするため Point
    pd.static_samplers[0].address_u   = ESamplerAddress::Wrap;
    pd.static_samplers[0].address_v   = ESamplerAddress::Wrap;
    pd.vertex_stride = sizeof(FVertex);
    pd.layout[0] = { "POSITION",  0, EFormat::R32G32B32_Float, 0 };
    pd.layout[1] = { "TEXCOORD",  0, EFormat::R32G32_Float,    sizeof(f32) * 3 };
    pd.layout_count = 2;
    if (auto r = CreateRhiPipeline(*dev, pd); r.IsErr()) {
        ACS_LOG_ERROR("Pipeline create: %s", r.Error().message); Quit(); return;
    } else m_Pipeline = Move(r.Value());

    // === カメラ ===
    const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                       static_cast<f32>(GetRenderer().Swapchain()->Height());
    m_Camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);

    ACS_LOG_INFO("HelloTextured initialized");
}

void CHelloTexturedApp::OnUpdate(f32 dt) noexcept {
    if (CInput::IsKeyPressed(EKey::Escape)) Quit();

    m_Angle += dt * 0.6f;
    if (CInput::IsKeyDown(EKey::Left))  m_CamYaw -= dt * 1.5f;
    if (CInput::IsKeyDown(EKey::Right)) m_CamYaw += dt * 1.5f;

    FVec3 eye{ Sin(m_CamYaw) * 5.0f, 1.5f, -Cos(m_CamYaw) * 5.0f };
    m_Camera.SetLookAt(eye, {0, 0, 0});

    FMat4 model = FMat4::RotationY(m_Angle) * FMat4::RotationX(m_Angle * 0.4f);
    FMat4 mvp   = model * m_Camera.View() * m_Camera.Projection();
    m_Cb->Update(&mvp, sizeof(FMat4));
}

void CHelloTexturedApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl || !m_Pipeline) return;
    cl->SetPipeline(*m_Pipeline);
    cl->SetConstantBuffer(0, *m_Cb);
    cl->SetTexture(0, *m_Tex);
    cl->SetVertexBuffer(*m_Vb, sizeof(FVertex));
    cl->SetIndexBuffer(*m_Ib);
    cl->DrawIndexed(36);
}

void CHelloTexturedApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    m_Pipeline.Reset();
    m_Tex.Reset();
    m_Cb.Reset();
    m_Ib.Reset();
    m_Vb.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
}

} // namespace hellotextured
