// SPDX-License-Identifier: Apache-2.0
// CMotionVector 実装
#include "render/MotionVector.h"
#include "render/NormalMatrix.h"
#include "asset/MeshAsset.h"          // MeshVertex の input layout 用
#include "foundation/Move.h"
#include "foundation/Log.h"           // ACS_ERR

namespace acs {

namespace {

// motion + normal G-buffer の geometry pass。
// MRT 2 枚出力:
//   SV_Target0 (RG16F)        = screen-space motion vector (prev_uv - curr_uv)
//   SV_Target1 (RGBA16F .xyz) = world-space normal
// 法線は頂点法線を model で world 変換 → ピクセル補間 → 正規化。曲面でも
// 補間されるので非 faceted (depth-derivative の cross(ddx,ddy) と違い段差が出ない)。
// SSR/SSGI/SSAO はこの normal を sample して品質の高い screen-space 効果を得る。
//   curr_mvp = curr_model * view_proj、prev_mvp = prev_model * prev_view_proj
const char* kMotionHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer MotionCB : register(b0) {
    float4x4 curr_mvp;
    float4x4 prev_mvp;
    float4 normal_row0;
    float4 normal_row1;
    float4 normal_row2;
};

struct VSIn {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD0;
};
struct VSOut {
    float4 pos       : SV_POSITION;
    float4 curr_clip : TEXCOORD0;
    float4 prev_clip : TEXCOORD1;
    float3 world_n   : TEXCOORD2;
};
struct PSOut {
    float4 motion : SV_TARGET0;   // RG16F:   prev_uv - curr_uv
    float4 normal : SV_TARGET1;   // RGBA16F: world-space normal (.xyz)
};

VSOut VSMain(VSIn v) {
    VSOut o;
    o.pos       = mul(float4(v.pos, 1.0), curr_mvp);
    o.curr_clip = o.pos;
    o.prev_clip = mul(float4(v.pos, 1.0), prev_mvp);
    // Row-vector inverse-transpose transform. This remains correct under
    // non-uniform scale, unlike multiplying normals by the model matrix.
    o.world_n = float3(
        v.nrm.x * normal_row0.x + v.nrm.y * normal_row1.x + v.nrm.z * normal_row2.x,
        v.nrm.x * normal_row0.y + v.nrm.y * normal_row1.y + v.nrm.z * normal_row2.y,
        v.nrm.x * normal_row0.z + v.nrm.y * normal_row1.z + v.nrm.z * normal_row2.z);
    return o;
}

PSOut PSMain(VSOut i) {
    // perspective divide で NDC、それを UV に変換 (y は反転)
    float2 curr_ndc = i.curr_clip.xy / max(i.curr_clip.w, 1e-6);
    float2 prev_ndc = i.prev_clip.xy / max(i.prev_clip.w, 1e-6);
    float2 curr_uv  = float2(curr_ndc.x * 0.5 + 0.5, -curr_ndc.y * 0.5 + 0.5);
    float2 prev_uv  = float2(prev_ndc.x * 0.5 + 0.5, -prev_ndc.y * 0.5 + 0.5);
    PSOut o;
    // TAA は hist_uv = uv + motion で前フレームを引く → motion = prev_uv - curr_uv
    float2 velocity = i.prev_clip.w > 1e-5 ? prev_uv - curr_uv : float2(0, 0);
    o.motion = float4(clamp(velocity, -1.0, 1.0), 0.0, 0.0);
    // 補間後に正規化 → 曲面でも滑らかな per-pixel 法線
    // normal_row0.wはC++側の選択mask。既存consumerは.xyzだけを読む。
    o.normal = float4(normalize(i.world_n), saturate(normal_row0.w));
    return o;
}
)";

// per-object 定数バッファ。curr/prev とも CPU 側で model*VP を合成して渡す。
struct FMotionCb {
    FMat4 curr_mvp;
    FMat4 prev_mvp;
    FVec4 normal_row0;
    FVec4 normal_row1;
    FVec4 normal_row2;
};

} // namespace

TResult<void> CMotionVector::Init(IRhiDevice& device, u32 width, u32 height) noexcept {
    Shutdown();
    m_PassActive = false;
    m_Device = &device;
    m_Width  = width  > 0 ? width  : 1;
    m_Height = height > 0 ? height : 1;

    if (auto r = CreateTargets(device, m_Width, m_Height); r.IsErr()) {
        const auto error = r.Error();
        Shutdown();
        return Err<void>(error);
    }
    if (auto r = CreatePipeline(device); r.IsErr()) {
        const auto error = r.Error();
        Shutdown();
        return Err<void>(error);
    }

    FBufferDesc cbd{};
    cbd.size         = 256;          // MotionCB (192B) を 256 アラインで確保
    cbd.usage        = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (!m_Cbs.TryReserve(kInitialObjectBufferCapacity)) {
        Shutdown();
        return ACS_ERR(
            Render, 367,
            "CMotionVector object-buffer pool allocation failed");
    }
    for (u32 i = 0; i < kInitialObjectBufferCapacity; ++i) {
        auto cbr = CreateRhiBuffer(device, cbd);
        if (cbr.IsErr()) {
            const auto error = cbr.Error();
            Shutdown();
            return Err<void>(error);
        }
        if (!m_Cbs.TryAdd(Move(cbr.Value()))) {
            Shutdown();
            return ACS_ERR(
                Render, 368,
                "CMotionVector object-buffer pool commit failed");
        }
    }

    return Ok();
}

void CMotionVector::Shutdown() noexcept {
    m_PassActive = false;
    m_Cbs.Empty();
    m_Pipeline.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
    m_Depth.Reset();
    m_Normal.Reset();
    m_Motion.Reset();
    m_Device = nullptr;
    m_Width  = 0;
    m_Height = 0;
    m_DrawCursor = 0;
    m_CapacityFailureLogged = false;
}

TResult<void> CMotionVector::Resize(u32 width, u32 height) noexcept {
    if (!m_Device) return ACS_ERR(Render, 360, "CMotionVector::Resize before Init");
    if (width == 0 || height == 0) return Ok();
    if (width == m_Width && height == m_Height) return Ok();
    m_PassActive = false;
    m_Motion.Reset();
    m_Normal.Reset();
    m_Depth.Reset();
    m_Width  = width;
    m_Height = height;
    return CreateTargets(*m_Device, width, height);
}

TResult<void> CMotionVector::CreateTargets(IRhiDevice& device, u32 w, u32 h) noexcept {
    // motion RT: RG16F。.rg に screen-space motion (prev_uv - curr_uv)。
    FTextureDesc md{};
    md.width  = w;
    md.height = h;
    md.format = EFormat::R16G16_Float;
    md.is_render_target = true;
    auto mr = CreateRhiTexture(device, md);
    if (mr.IsErr()) return Err<void>(mr.Error());
    m_Motion = Move(mr.Value());

    // normal RT: RGBA16F。.xyz に world-space normal。SSR/SSGI/SSAO が sample する。
    FTextureDesc nd{};
    nd.width  = w;
    nd.height = h;
    nd.format = EFormat::R16G16B16A16_Float;
    nd.is_render_target = true;
    auto nr = CreateRhiTexture(device, nd);
    if (nr.IsErr()) return Err<void>(nr.Error());
    m_Normal = Move(nr.Value());

    // 内部 depth: occlusion 判定用。SRV も張る。
    //
    // このパスは normal と «同じ幾何・同じ VP (jitter なし)» で深度を書くので、
    // **SSAO/GTAO が要求する深度そのもの**になっている。読めないと、SSAO を使う側が
    // 深度だけのために同じ幾何をもう一度描く羽目になる。
    FTextureDesc dd{};
    dd.width  = w;
    dd.height = h;
    dd.format = EFormat::D32_Float;
    dd.is_depth_target = true;
    dd.shader_visible_depth = true;
    auto dr = CreateRhiTexture(device, dd);
    if (dr.IsErr()) return Err<void>(dr.Error());
    m_Depth = Move(dr.Value());

    return Ok();
}

TResult<void> CMotionVector::CreatePipeline(IRhiDevice& device) noexcept {
    FShaderDesc vs_d{};
    vs_d.stage       = EShaderStage::Vertex;
    vs_d.hlsl_source = kMotionHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "CMotionVector.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    m_Vs = Move(vs_r.Value());

    FShaderDesc ps_d{};
    ps_d.stage       = EShaderStage::Pixel;
    ps_d.hlsl_source = kMotionHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "CMotionVector.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    m_Ps = Move(ps_r.Value());

    FPipelineDesc pd{};
    pd.vs            = m_Vs.Get();
    pd.ps            = m_Ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    // MRT: t0 = motion (RG16F)、t1 = world normal (RGBA16F)
    pd.rt_count      = 2;
    pd.rt_formats[0] = EFormat::R16G16_Float;
    pd.rt_formats[1] = EFormat::R16G16B16A16_Float;
    pd.depth_format  = EFormat::D32_Float;
    pd.depth_test    = true;
    pd.depth_write   = true;
    // 全面ラスタライズ + depth test で frontmost が勝つ。winding 依存を避けるため
    // cull は無効 (closed mesh では裏面が depth で落ちるので実害なし)。
    pd.cull_mode     = ECullMode::None;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 0;
    pd.cbuffer_names[0] = "MotionCB";
    pd.vertex_stride = sizeof(FMeshVertex);
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, EFormat::R32G32B32_Float, 16 };
    pd.layout[2] = { "TEXCOORD", 0, EFormat::R32G32_Float,    32 };
    pd.layout_count = 3;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    m_Pipeline = Move(pl_r.Value());

    return Ok();
}

bool CMotionVector::EnsureObjectCapacity(u32 required_draws) noexcept {
    if (required_draws == kInvalidObjectBuffer) return false;
    if (!m_Device) return false;
    if (required_draws <= m_Cbs.Num()) return true;

    u32 target = static_cast<u32>(m_Cbs.Num());
    if (target < kInitialObjectBufferCapacity) {
        target = kInitialObjectBufferCapacity;
    }
    while (target < required_draws) {
        const u32 growth = target > 1u ? target / 2u : 1u;
        if (target > kInvalidObjectBuffer - growth) {
            target = required_draws;
            break;
        }
        target += growth;
    }

    // Reserving the owner array is transactional: all already-created GPU
    // buffers stay valid if the allocation fails, and a later frame can retry.
    if (!m_Cbs.TryReserve(target)) return false;

    FBufferDesc desc{};
    desc.size         = 256;
    desc.usage        = EBufferUsage::Uniform;
    desc.cpu_writable = true;
    // Geometric reserve avoids repeatedly reallocating the owner array, while
    // GPU buffers themselves are created only for draws actually requested.
    while (m_Cbs.Num() < required_draws) {
        auto created = CreateRhiBuffer(*m_Device, desc);
        if (created.IsErr()) return m_Cbs.Num() >= required_draws;
        if (!m_Cbs.TryAdd(Move(created.Value()))) {
            return m_Cbs.Num() >= required_draws;
        }
    }
    return true;
}

bool CMotionVector::BeginFrame(u32 required_draws) noexcept {
    m_DrawCursor = 0;
    m_CapacityFailureLogged = false;
    if (EnsureObjectCapacity(required_draws)) return true;

    ACS_LOG_WARN("CMotionVector: could not reserve %u per-object buffers "
                 "(retained %u); motion output will be skipped this frame",
                 required_draws, static_cast<u32>(m_Cbs.Num()));
    m_CapacityFailureLogged = true;
    return false;
}

bool CMotionVector::Begin(IRhiCommandList& cl,
                         const FMat4& view_proj, const FMat4& prev_view_proj) noexcept {
    m_PassActive = false;
    if (!m_Motion || !m_Normal || !m_Depth || !m_Pipeline) return false;
    m_Vp      = view_proj;
    m_PrevVp = prev_view_proj;
    m_DrawCursor = 0;
    m_CapacityFailureLogged = false;
    // motion / normal RT を (0,0,0,0) クリア → 描かれない pixel (= sky 等) は
    // motion 0 (TAA は hist_uv = uv で reproject 無し)、normal 0 (SSR/SSGI/SSAO は
    // sky を depth で先に弾くので未使用)。
    IRhiTexture* rts[2] = { m_Motion.Get(), m_Normal.Get() };
    if (!cl.BeginRenderToTextureMrt(
            rts, 2, FClearColor{0, 0, 0, 0}, m_Depth.Get(), 1.0f)) {
        return false;
    }
    m_PassActive = true;
    cl.SetPipeline(*m_Pipeline);
    return true;
}

bool CMotionVector::DrawMesh(IRhiCommandList& cl, const FGpuMesh& mesh,
                             const FMat4& model, const FMat4& prev_model) noexcept {
    return DrawMesh(cl, mesh, model, prev_model, false);
}

bool CMotionVector::DrawMesh(IRhiCommandList& cl, const FGpuMesh& mesh, const FMat4& model, const FMat4& prev_model, bool selection_mask) noexcept {
    if (!m_PassActive) return false;
    if (!mesh.vertex_buffer || !mesh.index_buffer) return false;
    if (m_DrawCursor == kInvalidObjectBuffer ||
        (m_DrawCursor >= m_Cbs.Num() &&
         !EnsureObjectCapacity(m_DrawCursor + 1u))) {
        if (!m_CapacityFailureLogged) {
            ACS_LOG_WARN("CMotionVector: object-buffer growth failed at draw %u; "
                         "motion output is incomplete",
                         m_DrawCursor);
            m_CapacityFailureLogged = true;
        }
        return false;
    }
    IRhiBuffer* cb_buffer = m_Cbs[m_DrawCursor].Get();
    if (!cb_buffer) return false;
    ++m_DrawCursor;
    FMotionCb cb{};
    cb.curr_mvp   = model      * m_Vp;
    cb.prev_mvp   = prev_model * m_PrevVp;
    const FMat4 normal_matrix = MakeSafeNormalMatrix(model);
    cb.normal_row0 = FVec4{normal_matrix.m[0][0], normal_matrix.m[0][1],
                           normal_matrix.m[0][2], selection_mask ? 1.0f : 0.0f};
    cb.normal_row1 = FVec4{normal_matrix.m[1][0], normal_matrix.m[1][1],
                           normal_matrix.m[1][2], 0};
    cb.normal_row2 = FVec4{normal_matrix.m[2][0], normal_matrix.m[2][1],
                           normal_matrix.m[2][2], 0};
    cb_buffer->Update(&cb, sizeof(cb));

    cl.SetConstantBuffer(0, *cb_buffer);
    cl.SetVertexBuffer(*mesh.vertex_buffer, mesh.vertex_stride);
    cl.SetIndexBuffer(*mesh.index_buffer);
    cl.DrawIndexed(mesh.index_count);
    return true;
}

void CMotionVector::End(IRhiCommandList& cl) noexcept {
    if (!m_PassActive || !m_Motion || !m_Normal) return;
    IRhiTexture* rts[2] = {m_Motion.Get(), m_Normal.Get()};
    cl.EndRenderToTextureMrt(rts, 2u);
    m_PassActive = false;
}

} // namespace acs
