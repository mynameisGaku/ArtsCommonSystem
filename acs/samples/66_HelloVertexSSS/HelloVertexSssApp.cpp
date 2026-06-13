// SPDX-License-Identifier: Apache-2.0
// HelloVertexSSS — 実装。
#include "HelloVertexSssApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "asset/MeshPrimitive.h"
#include "math/Mat.h"
#include "math/Math.h"
#include "foundation/Log.h"

#include <cstdio>
#include <cmath>

using namespace acs;

namespace hellovertexsss {

namespace {

// 自前シェーダ: 位置+法線+散乱後放射照度 (sss) を受け、albedo*sss + ambient を出す。
// «散乱後放射照度» は CPU (FVertexScatter) が頂点ごとに計算して頂点バッファへ書く。
const char* kHLSL = R"(
#pragma pack_matrix(row_major)
cbuffer Frame : register(b0) {
    float4x4 view_proj;
    float4   light_dir;   // xyz = surface→light、w = ambient 強度
    float4   tint;        // 未使用 (将来拡張用)
};
cbuffer Object : register(b1) {
    float4x4 model;
    float4   albedo;      // xyz = 肌色
};
struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float3 sss : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float3 sss : TEXCOORD0; };

VSOut VSMain(VSIn v) {
    VSOut o;
    float4 wp = mul(float4(v.pos, 1.0), model);
    o.pos = mul(wp, view_proj);
    o.sss = v.sss;                       // 散乱後放射照度 (頂点間で補間)
    return o;
}
float4 PSMain(VSOut v) : SV_TARGET {
    float3 amb = albedo.xyz * light_dir.w;          // 環境光
    float3 dif = albedo.xyz * max(v.sss, 0.0.xxx);  // 肌色 × 散乱後放射照度
    float3 col = amb + dif;
    col = col / (col + 1.0.xxx);                     // 軽い Reinhard で LDR に収める
    return float4(pow(col, (1.0/2.2).xxx), 1.0);     // ガンマ
}
)";

struct FrameCB { FMat4 view_proj; FVec4 light_dir; FVec4 tint; };
struct ObjCB   { FMat4 model; FVec4 albedo; };

const FVec3 kSkin{ 0.95f, 0.66f, 0.58f };            // 肌色 albedo
// 肌の散乱プロファイル: 赤が最も遠くまで散る (terminator の赤いにじみ)。
const FScatterProfile kSkinProfile{ 34, 13, 6 };

} // namespace

void HelloVertexSssApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    m_Sphere = Primitive::MakeSphere(1.0f, 64, 32);
    if (!m_Sphere || m_Sphere->Vertices().Size() == 0) { ACS_LOG_ERROR("sphere make failed"); Quit(); return; }
    if (!m_Scatter.Build(*m_Sphere)) { ACS_LOG_ERROR("scatter build failed"); Quit(); return; }

    const u32 vc = static_cast<u32>(m_Sphere->Vertices().Size());
    m_CpuVtx.Resize(vc);
    m_Irr.Resize(vc);
    m_IrrOut.Resize(vc);

    // シェーダ
    FShaderDesc vs{}; vs.stage = EShaderStage::Vertex; vs.hlsl_source = kHLSL; vs.entry_point = "VSMain"; vs.debug_name = "VSSS.VS";
    FShaderDesc ps{}; ps.stage = EShaderStage::Pixel;  ps.hlsl_source = kHLSL; ps.entry_point = "PSMain"; ps.debug_name = "VSSS.PS";
    auto vr = CreateRhiShader(*dev, vs); auto pr = CreateRhiShader(*dev, ps);
    if (vr.IsErr() || pr.IsErr()) { ACS_LOG_ERROR("shader compile failed"); Quit(); return; }
    m_Vs = Move(vr.Value()); m_Ps = Move(pr.Value());

    // パイプライン (深度テスト有り、3 頂点属性 pos/nrm/sss)
    FPipelineDesc pd{};
    pd.vs = m_Vs.Get(); pd.ps = m_Ps.Get();
    pd.topology     = EPrimitiveTopology::TriangleList;
    pd.rt_format    = GetRenderer().ColorFormat();
    pd.depth_format = GetRenderer().DepthFormat();
    pd.depth_test   = true; pd.depth_write = true;
    pd.cull_mode    = ECullMode::None;   // 球の表裏どちらの巻きでも確実に出す (SSS は両面とも見せたい)
    pd.blend_mode   = EBlendMode::Opaque;
    pd.cbuffer_slots = 2; pd.cbuffer_names[0] = "Frame"; pd.cbuffer_names[1] = "Object";
    pd.vertex_stride = sizeof(VtxSSS);
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, EFormat::R32G32B32_Float, 12 };
    pd.layout[2] = { "TEXCOORD", 0, EFormat::R32G32B32_Float, 24 };
    pd.layout_count = 3;
    auto plr = CreateRhiPipeline(*dev, pd);
    if (plr.IsErr()) { ACS_LOG_ERROR("pipeline failed"); Quit(); return; }
    m_Pipe = Move(plr.Value());

    // インデックスバッファ (u32)
    const auto& idx = m_Sphere->Indices();
    m_IndexCount = static_cast<u32>(idx.Size());
    FBufferDesc ibd{}; ibd.size = sizeof(u32) * idx.Size(); ibd.usage = EBufferUsage::Index32;
    ibd.cpu_writable = true; ibd.initial_data = idx.Data();
    auto ibr = CreateRhiBuffer(*dev, ibd);
    if (ibr.IsErr()) { ACS_LOG_ERROR("ib failed"); Quit(); return; }
    m_Ib = Move(ibr.Value());

    // 動的頂点バッファ × 2 (左=Lambert / 右=SSS)
    auto mkVb = [&](TUniquePtr<IRhiBuffer>& out) {
        FBufferDesc vbd{}; vbd.size = sizeof(VtxSSS) * vc; vbd.usage = EBufferUsage::Vertex; vbd.cpu_writable = true;
        auto r = CreateRhiBuffer(*dev, vbd); if (r.IsOk()) out = Move(r.Value());
    };
    mkVb(m_VbLeft); mkVb(m_VbRight);

    // 定数バッファ
    auto mkCb = [&](TUniquePtr<IRhiBuffer>& out, usize sz) {
        FBufferDesc c{}; c.size = sz; c.usage = EBufferUsage::Uniform; c.cpu_writable = true;
        auto r = CreateRhiBuffer(*dev, c); if (r.IsOk()) out = Move(r.Value());
    };
    mkCb(m_FrameCb, 256); mkCb(m_ObjCbL, 256); mkCb(m_ObjCbR, 256);

    // 静的 model 行列 (左 -1.3 / 右 +1.3)
    ObjCB ol{}; ol.model = FMat4::Translation(FVec3{ -1.6f, 0, 0 }); ol.albedo = FVec4{ kSkin.x, kSkin.y, kSkin.z, 1 };
    ObjCB orr{}; orr.model = FMat4::Translation(FVec3{  1.6f, 0, 0 }); orr.albedo = FVec4{ kSkin.x, kSkin.y, kSkin.z, 1 };
    if (m_ObjCbL) m_ObjCbL->Update(&ol, sizeof(ol));
    if (m_ObjCbR) m_ObjCbR->Update(&orr, sizeof(orr));

    (void)m_Batch.Init(*dev, GetRenderer().ColorFormat());
    (void)FSample::TryLoadDefaultUIFont(m_Font, *dev, 18.0f, 1024, true);

    const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                       static_cast<f32>(GetRenderer().Swapchain()->Height());
    m_Camera.SetPerspective(48.0f * kDeg2Rad, aspect, 0.1f, 100.0f);
    m_Camera.SetLookAt(FVec3{ 0, 0, -4.8f }, FVec3{ 0, 0, 0 });
}

void HelloVertexSssApp::UpdateSphere(IRhiBuffer* vb, FVec3 lightDir, bool scatter) noexcept {
    if (vb == nullptr) return;
    const auto& v = m_Sphere->Vertices();
    const u32 vc = static_cast<u32>(v.Size());
    // per-vertex Lambert 放射照度 (法線・光方向から)。RGB 同値で入れる。
    for (u32 i = 0; i < vc; ++i) {
        const FVec3 n = v[i].normal;
        f32 ndl = n.x * lightDir.x + n.y * lightDir.y + n.z * lightDir.z;
        if (ndl < 0.0f) ndl = 0.0f;
        m_Irr[i] = FVec3{ ndl, ndl, ndl };
    }
    const FVec3* src = m_Irr.Data();
    // 散乱は «見えない裏面» にもエネルギーを回すため可視面は全体に暗くなる。SSS らしい
    // «赤いにじみ» を «単なる減光» と見せないよう、可視面ぶんのゲインで明るさを補償する。
    f32 gain = 1.0f;
    if (scatter) { m_Scatter.Scatter(m_Irr.Data(), m_IrrOut.Data(), kSkinProfile); src = m_IrrOut.Data(); gain = 1.3f; }
    for (u32 i = 0; i < vc; ++i) {
        const FVec3 p = v[i].position, nn = v[i].normal, s = src[i];
        m_CpuVtx[i] = VtxSSS{ p.x, p.y, p.z, nn.x, nn.y, nn.z, s.x * gain, s.y * gain, s.z * gain };
    }
    vb->Update(m_CpuVtx.Data(), sizeof(VtxSSS) * vc);
}

void HelloVertexSssApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    m_Time += dt;
}

void HelloVertexSssApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl || !m_Pipe) return;

    // 方向光 (surface→light)。可視半球の右・手前から固定で当て、terminator (明暗境界) を
    // 可視面の中ほどに通す。影側 (左) への赤いにじみで «頂点空間 SSS» の効果が分かる。
    FVec3 L{ 0.62f, 0.30f, -0.45f };
    const f32 len = std::sqrt(L.x*L.x + L.y*L.y + L.z*L.z);
    L = FVec3{ L.x/len, L.y/len, L.z/len };

    UpdateSphere(m_VbLeft.Get(),  L, /*scatter*/false);
    UpdateSphere(m_VbRight.Get(), L, /*scatter*/true);

    FrameCB fcb{}; fcb.view_proj = m_Camera.ViewProjection();
    fcb.light_dir = FVec4{ L.x, L.y, L.z, 0.06f };    // w = ambient (低め: terminator を見せる)
    if (m_FrameCb) m_FrameCb->Update(&fcb, sizeof(fcb));

    cl->SetPipeline(*m_Pipe);
    cl->SetConstantBuffer(0, *m_FrameCb);
    cl->SetIndexBuffer(*m_Ib);

    // 左: 生 Lambert
    cl->SetConstantBuffer(1, *m_ObjCbL);
    cl->SetVertexBuffer(*m_VbLeft, sizeof(VtxSSS));
    cl->DrawIndexed(m_IndexCount);
    // 右: 頂点空間 SSS
    cl->SetConstantBuffer(1, *m_ObjCbR);
    cl->SetVertexBuffer(*m_VbRight, sizeof(VtxSSS));
    cl->DrawIndexed(m_IndexCount);

    if (m_Font.AtlasTexture()) {
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();
        m_Batch.Begin(*cl, sw, sh);
        m_Batch.DrawString(m_Font, "Lambert (no scatter)", 20, 24, FVec4{ 0.8f, 0.85f, 0.95f, 1 });
        m_Batch.DrawString(m_Font, "Vertex-Space SSS", static_cast<f32>(sw) - 230.0f, 24, FVec4{ 1.0f, 0.8f, 0.75f, 1 });
        char buf[96]; std::snprintf(buf, sizeof(buf), "FVertexScatter  R=%u G=%u B=%u iters  FPS %.0f",
                                    kSkinProfile.iterR, kSkinProfile.iterG, kSkinProfile.iterB, static_cast<double>(FPS()));
        m_Batch.DrawString(m_Font, buf, 20, static_cast<f32>(sh) - 36.0f, FVec4{ 0.7f, 0.8f, 0.9f, 1 });
        m_Batch.End();
    }
}

void HelloVertexSssApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    m_Font.Shutdown();
    m_Batch.Shutdown();
}

} // namespace hellovertexsss
