// SPDX-License-Identifier: Apache-2.0
// CDebugDraw3D 実装。
#include "render/DebugDraw.h"
#include "foundation/Limits.h"
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

struct FDebugCbLayout {
    FMat4 view_proj;
};

template<typename T>
constexpr usize CBSize() noexcept
{
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

TResult<void> CDebugDraw3D::Init(IRhiDevice& device, EFormat rt_format, u32 max_lines) noexcept
{
    /** 0指定を従来どおり1本へ補正した線容量。 */
    const u32 line_capacity = max_lines < 1u ? 1u : max_lines;
    if (line_capacity > TNumLimits<u32>::Max() / 2u) {
        return ACS_ERR(Render, 300, "CDebugDraw3D line capacity overflow");
    }

    /** 線容量を2頂点単位へ変換した新しい上限。 */
    const u32 new_max_vertices = line_capacity * 2u;
    if (static_cast<usize>(new_max_vertices) > TNumLimits<usize>::Max() / sizeof(FLineVtx)) {
        return ACS_ERR(Render, 301, "CDebugDraw3D vertex buffer size overflow");
    }

    /** 全生成処理が成功するまで既存頂点列と分離して保持する領域。 */
    TArray<FLineVtx> new_vertices(*m_Verts.GetAllocator());
    if (!new_vertices.TryReserve(new_max_vertices)) {
        return ACS_ERR(Memory, 302, "CDebugDraw3D vertex allocation failed");
    }

    /** commit前に独立して保持する新しいGPU資源。 */
    TUniquePtr<IRhiShader> new_vs;
    TUniquePtr<IRhiShader> new_ps;
    TUniquePtr<IRhiPipeline> new_pipeline;
    TUniquePtr<IRhiBuffer> new_vb;
    TUniquePtr<IRhiBuffer> new_cb;

    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kDebugLineHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name = "CDebugDraw3D.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr())
        return Err<void>(r.Error());
    else
        new_vs = Move(r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kDebugLineHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name = "CDebugDraw3D.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr())
        return Err<void>(r.Error());
    else
        new_ps = Move(r.Value());

    FPipelineDesc pd{};
    pd.vs = new_vs.Get();
    pd.ps = new_ps.Get();
    pd.topology = EPrimitiveTopology::LineList;
    pd.rt_format = rt_format;
    pd.depth_format = EFormat::Unknown;
    pd.depth_test = false;
    pd.depth_write = false;
    pd.cull_mode = ECullMode::None;
    pd.blend_mode = EBlendMode::AlphaBlend;
    pd.cbuffer_slots = 1;
    pd.cbuffer_names[0] = "DebugCb";
    pd.texture_slots = 0;
    pd.vertex_stride = sizeof(FLineVtx);
    pd.layout_count = 2;
    pd.layout[0].semantic_name = "POSITION";
    pd.layout[0].format = EFormat::R32G32B32_Float;
    pd.layout[0].offset = 0;
    pd.layout[1].semantic_name = "COLOR";
    pd.layout[1].format = EFormat::R32G32B32A32_Float;
    pd.layout[1].offset = 12;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr())
        return Err<void>(r.Error());
    else
        new_pipeline = Move(r.Value());

    FBufferDesc vbd{};
    vbd.size = static_cast<usize>(new_max_vertices) * sizeof(FLineVtx);
    vbd.usage = EBufferUsage::Vertex;
    vbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, vbd); r.IsErr())
        return Err<void>(r.Error());
    else
        new_vb = Move(r.Value());

    FBufferDesc cbd{};
    cbd.size = CBSize<FDebugCbLayout>();
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr())
        return Err<void>(r.Error());
    else
        new_cb = Move(r.Value());

    /** commit中も依存順を保って解放する旧GPU資源とCPU頂点列。 */
    TUniquePtr<IRhiShader> old_vs = Move(m_Vs);
    TUniquePtr<IRhiShader> old_ps = Move(m_Ps);
    TUniquePtr<IRhiPipeline> old_pipeline = Move(m_Pipeline);
    TUniquePtr<IRhiBuffer> old_vb = Move(m_Vb);
    TUniquePtr<IRhiBuffer> old_cb = Move(m_Cb);
    TArray<FLineVtx> old_vertices = Move(m_Verts);

    m_Vs = Move(new_vs);
    m_Ps = Move(new_ps);
    m_Pipeline = Move(new_pipeline);
    m_Vb = Move(new_vb);
    m_Cb = Move(new_cb);
    m_Verts = Move(new_vertices);
    m_MaxVerts = new_max_vertices;

    old_pipeline.Reset();
    old_cb.Reset();
    old_vb.Reset();
    old_ps.Reset();
    old_vs.Reset();
    old_vertices.Empty();
    return Ok();
}

void CDebugDraw3D::Shutdown() noexcept
{
    m_Pipeline.Reset();
    m_Cb.Reset();
    m_Vb.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
    m_Verts.Empty();
    m_MaxVerts = 0u;
}

void CDebugDraw3D::Begin() noexcept
{
    m_Verts.Reset();
}

bool CDebugDraw3D::TryLine(FVec3 a, FVec3 b, FVec4 c) noexcept
{
    /** 失敗時に維持する追加前の頂点数。 */
    const usize old_size = m_Verts.Num();
    /** 初期化時に確保した頂点上限。 */
    const usize max_vertices = static_cast<usize>(m_MaxVerts);
    /** 1本の線に必要な頂点数。 */
    constexpr usize vertex_count = 2u;
    if (old_size > max_vertices || vertex_count > max_vertices - old_size) return false;
    if (!m_Verts.TrySetNum(old_size + vertex_count)) return false;

    /** 一括確保した追記先。 */
    FLineVtx* const output = m_Verts.GetData() + old_size;
    output[0] = FLineVtx{a.x, a.y, a.z, c.x, c.y, c.z, c.w};
    output[1] = FLineVtx{b.x, b.y, b.z, c.x, c.y, c.z, c.w};
    return true;
}

void CDebugDraw3D::Line(FVec3 a, FVec3 b, FVec4 c) noexcept
{
    (void)TryLine(a, b, c);
}

bool CDebugDraw3D::TryAabb(const FAabb3& box, FVec4 color) noexcept
{
    const FVec3 mn = box.Min(), mx = box.Max();
    const FVec3 c[8] = {
        {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
        {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z},
    };
    const int e[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    /** AABB全辺を追加できる場合だけ配列を拡張する。 */
    const usize old_size = m_Verts.Num();
    const usize max_vertices = static_cast<usize>(m_MaxVerts);
    constexpr usize vertex_count = 24u;
    if (old_size > max_vertices || vertex_count > max_vertices - old_size) return false;
    if (!m_Verts.TrySetNum(old_size + vertex_count)) return false;

    /** 一括確保した追記先。 */
    FLineVtx* output = m_Verts.GetData() + old_size;
    for (int i = 0; i < 12; ++i) {
        const FVec3 a = c[e[i][0]];
        const FVec3 b = c[e[i][1]];
        output[0] = FLineVtx{a.x, a.y, a.z, color.x, color.y, color.z, color.w};
        output[1] = FLineVtx{b.x, b.y, b.z, color.x, color.y, color.z, color.w};
        output += 2;
    }
    return true;
}

void CDebugDraw3D::Aabb(const FAabb3& box, FVec4 color) noexcept
{
    (void)TryAabb(box, color);
}

bool CDebugDraw3D::TryWireframe(const FVec3* positions, u32 vertex_count, const u32* indices, u32 index_count, FVec4 color) noexcept
{
    if (!positions || !indices) return false;
    if (index_count < 3u) return true;

    /** 範囲外indexを含まない完全な三角形数。 */
    usize valid_triangle_count = 0u;
    /** u32加算をoverflowさせずに走査できる最後の先頭位置。 */
    const u32 last_triangle_start = index_count - 3u;
    for (u32 t = 0; t <= last_triangle_start; t += 3u) {
        const u32 i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
        if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) continue;
        ++valid_triangle_count;
    }

    /** 有効な全三角形の3辺に必要な頂点数を検査する。 */
    constexpr usize vertices_per_triangle = 6u;
    if (valid_triangle_count == 0u) return true;
    if (valid_triangle_count > TNumLimits<usize>::Max() / vertices_per_triangle) return false;
    const usize append_count = valid_triangle_count * vertices_per_triangle;
    const usize old_size = m_Verts.Num();
    const usize max_vertices = static_cast<usize>(m_MaxVerts);
    if (old_size > max_vertices || append_count > max_vertices - old_size) return false;
    if (!m_Verts.TrySetNum(old_size + append_count)) return false;

    /** 一括確保した追記先へ、従来と同じ辺順で格納する。 */
    FLineVtx* output = m_Verts.GetData() + old_size;
    for (u32 t = 0; t <= last_triangle_start; t += 3u) {
        const u32 i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
        if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) continue;
        const FVec3 triangle[3] = {positions[i0], positions[i1], positions[i2]};
        const u32 edges[3][2] = {{0u, 1u}, {1u, 2u}, {2u, 0u}};
        for (u32 edge = 0u; edge < 3u; ++edge) {
            const FVec3 a = triangle[edges[edge][0]];
            const FVec3 b = triangle[edges[edge][1]];
            output[0] = FLineVtx{a.x, a.y, a.z, color.x, color.y, color.z, color.w};
            output[1] = FLineVtx{b.x, b.y, b.z, color.x, color.y, color.z, color.w};
            output += 2;
        }
    }
    return true;
}

void CDebugDraw3D::Wireframe(const FVec3* positions, u32 vertex_count, const u32* indices, u32 index_count, FVec4 color) noexcept
{
    (void)TryWireframe(positions, vertex_count, indices, index_count, color);
}

void CDebugDraw3D::End(IRhiCommandList& cl, const FMat4& view_proj) noexcept
{
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
