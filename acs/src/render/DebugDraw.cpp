// SPDX-License-Identifier: Apache-2.0
// CDebugDraw3D 実装。
#include "render/DebugDraw.h"
#include "foundation/Move.h"

namespace acs {

namespace {

const char* kDebugLineHLSL = R"(
#pragma pack_matrix(row_major)
cbuffer DebugCb : register(b0) { float4x4 view_proj; };

struct VSIn  { float3 pos : POSITION; float4 col : COLOR; };
struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };

VSOut VSMain(VSIn i) {
    VSOut o;
    o.pos = mul(float4(i.pos, 1.0), view_proj);
    o.col = i.col;
    return o;
}
float4 PSMain(VSOut v) : SV_TARGET { return v.col; }
)";

struct FDebugCbLayout { FMat4 view_proj; };

template<typename T>
constexpr usize CBSize() noexcept { return (sizeof(T) + 255u) & ~static_cast<usize>(255u); }

} // namespace

TResult<void> CDebugDraw3D::Init(IRhiDevice& device, EFormat rt_format, u32 max_lines) noexcept {
    m_MaxVerts = (max_lines < 1u ? 1u : max_lines) * 2u;

    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kDebugLineHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "CDebugDraw3D.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else m_Vs = Move(r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kDebugLineHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "CDebugDraw3D.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else m_Ps = Move(r.Value());

    FPipelineDesc pd{};
    pd.vs            = m_Vs.Get();
    pd.ps            = m_Ps.Get();
    pd.topology      = EPrimitiveTopology::LineList;
    pd.rt_format     = rt_format;
    pd.depth_format  = EFormat::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::AlphaBlend;
    pd.cbuffer_slots = 1;
    pd.cbuffer_names[0] = "DebugCb";
    pd.texture_slots = 0;
    pd.vertex_stride = sizeof(FLineVtx);
    pd.layout_count  = 2;
    pd.layout[0].semantic_name = "POSITION"; pd.layout[0].format = EFormat::R32G32B32_Float;    pd.layout[0].offset = 0;
    pd.layout[1].semantic_name = "COLOR";    pd.layout[1].format = EFormat::R32G32B32A32_Float; pd.layout[1].offset = 12;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else m_Pipeline = Move(r.Value());

    FBufferDesc vbd{};
    vbd.size = static_cast<usize>(m_MaxVerts) * sizeof(FLineVtx);
    vbd.usage = EBufferUsage::Vertex;
    vbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, vbd); r.IsErr()) return Err<void>(r.Error());
    else m_Vb = Move(r.Value());

    FBufferDesc cbd{};
    cbd.size = CBSize<FDebugCbLayout>();
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else m_Cb = Move(r.Value());

    m_Verts.Reserve(m_MaxVerts);
    return Ok();
}

void CDebugDraw3D::Shutdown() noexcept {
    m_Pipeline.Reset();
    m_Cb.Reset();
    m_Vb.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
    m_Verts.Reset();
}

void CDebugDraw3D::Begin() noexcept { m_Verts.Reset(); }

void CDebugDraw3D::Line(FVec3 a, FVec3 b, FVec4 c) noexcept {
    if (m_Verts.Num() + 2 > m_MaxVerts) return;
    m_Verts.Add(FLineVtx{ a.x, a.y, a.z, c.x, c.y, c.z, c.w });
    m_Verts.Add(FLineVtx{ b.x, b.y, b.z, c.x, c.y, c.z, c.w });
}

void CDebugDraw3D::Aabb(const FAabb3& box, FVec4 color) noexcept {
    const FVec3 mn = box.Min(), mx = box.Max();
    const FVec3 c[8] = {
        {mn.x,mn.y,mn.z},{mx.x,mn.y,mn.z},{mx.x,mx.y,mn.z},{mn.x,mx.y,mn.z},
        {mn.x,mn.y,mx.z},{mx.x,mn.y,mx.z},{mx.x,mx.y,mx.z},{mn.x,mx.y,mx.z},
    };
    const int e[12][2] = {
        {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7}
    };
    for (int i = 0; i < 12; ++i) Line(c[e[i][0]], c[e[i][1]], color);
}

void CDebugDraw3D::Wireframe(const FVec3* positions, u32 vertex_count,
                             const u32* indices, u32 index_count, FVec4 color) noexcept {
    if (!positions || !indices) return;
    for (u32 t = 0; t + 3 <= index_count; t += 3) {
        const u32 i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
        if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) continue;
        Line(positions[i0], positions[i1], color);
        Line(positions[i1], positions[i2], color);
        Line(positions[i2], positions[i0], color);
    }
}

void CDebugDraw3D::End(IRhiCommandList& cl, const FMat4& view_proj) noexcept {
    if (!m_Pipeline || !m_Vb || !m_Cb || m_Verts.Num() == 0) return;
    FDebugCbLayout cb{};
    cb.view_proj = view_proj;
    m_Cb->Update(&cb, sizeof(cb));
    m_Vb->Update(m_Verts.GetData(), m_Verts.Num() * sizeof(FLineVtx), 0);

    cl.SetPipeline(*m_Pipeline);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.SetVertexBuffer(*m_Vb, sizeof(FLineVtx));
    cl.Draw(static_cast<u32>(m_Verts.Num()), 0);
}

} // namespace acs
