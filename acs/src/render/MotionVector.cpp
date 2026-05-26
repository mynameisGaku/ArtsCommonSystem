// SPDX-License-Identifier: Apache-2.0
// FMotionVector 実装 (Phase 34f-3)
#include "render/MotionVector.h"
#include "asset/MeshAsset.h"          // MeshVertex の input layout 用
#include "foundation/Move.h"
#include "foundation/Log.h"           // ACS_ERR

namespace acs {

namespace {

// motion + normal G-buffer の geometry pass (Phase 34f-3 + 34m)。
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
    float4x4 curr_model;     // 頂点法線を world へ変換するため (Phase 34m)
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
    // 法線を world へ (回転 / 一様スケールのみ前提、非一様スケールは未対応)
    o.world_n   = mul(float4(v.nrm, 0.0), curr_model).xyz;
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
    o.motion = float4(prev_uv - curr_uv, 0.0, 0.0);
    // 補間後に正規化 → 曲面でも滑らかな per-pixel 法線
    o.normal = float4(normalize(i.world_n), 0.0);
    return o;
}
)";

// per-object 定数バッファ。curr/prev とも CPU 側で model*VP を合成して渡す。
// curr_model は法線の world 変換用に別途渡す。
struct MotionCB {
    FMat4 curr_mvp;
    FMat4 prev_mvp;
    FMat4 curr_model;
};

} // namespace

TResult<void> FMotionVector::Init(IRhiDevice& device, u32 width, u32 height) noexcept {
    _device = &device;
    _width  = width  > 0 ? width  : 1;
    _height = height > 0 ? height : 1;

    if (auto r = CreateTargets(device, _width, _height); r.IsErr()) return r;
    if (auto r = CreatePipeline(device);                 r.IsErr()) return r;

    FBufferDesc cbd{};
    cbd.size         = 256;          // MotionCB (192B) を 256 アラインで確保
    cbd.usage        = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    auto cbr = CreateRhiBuffer(device, cbd);
    if (cbr.IsErr()) return Err<void>(cbr.Error());
    _cb = Move(cbr.Value());

    return Ok();
}

void FMotionVector::Shutdown() noexcept {
    _cb.Reset();
    _pipeline.Reset();
    _ps.Reset();
    _vs.Reset();
    _depth.Reset();
    _normal.Reset();
    _motion.Reset();
    _device = nullptr;
    _width  = 0;
    _height = 0;
}

TResult<void> FMotionVector::Resize(u32 width, u32 height) noexcept {
    if (!_device) return ACS_ERR(Render, 360, "FMotionVector::Resize before Init");
    if (width == 0 || height == 0) return Ok();
    if (width == _width && height == _height) return Ok();
    _motion.Reset();
    _normal.Reset();
    _depth.Reset();
    _width  = width;
    _height = height;
    return CreateTargets(*_device, width, height);
}

TResult<void> FMotionVector::CreateTargets(IRhiDevice& device, u32 w, u32 h) noexcept {
    // motion RT: RG16F。.rg に screen-space motion (prev_uv - curr_uv)。
    FTextureDesc md{};
    md.width  = w;
    md.height = h;
    md.format = EFormat::R16G16_Float;
    md.is_render_target = true;
    auto mr = CreateRhiTexture(device, md);
    if (mr.IsErr()) return Err<void>(mr.Error());
    _motion = Move(mr.Value());

    // normal RT: RGBA16F。.xyz に world-space normal。SSR/SSGI/SSAO が sample する。
    FTextureDesc nd{};
    nd.width  = w;
    nd.height = h;
    nd.format = EFormat::R16G16B16A16_Float;
    nd.is_render_target = true;
    auto nr = CreateRhiTexture(device, nd);
    if (nr.IsErr()) return Err<void>(nr.Error());
    _normal = Move(nr.Value());

    // 内部 depth: occlusion 判定用 (SRV は不要)。
    FTextureDesc dd{};
    dd.width  = w;
    dd.height = h;
    dd.format = EFormat::D32_Float;
    dd.is_depth_target = true;
    auto dr = CreateRhiTexture(device, dd);
    if (dr.IsErr()) return Err<void>(dr.Error());
    _depth = Move(dr.Value());

    return Ok();
}

TResult<void> FMotionVector::CreatePipeline(IRhiDevice& device) noexcept {
    FShaderDesc vs_d{};
    vs_d.stage       = EShaderStage::FVertex;
    vs_d.hlsl_source = kMotionHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "FMotionVector.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    _vs = Move(vs_r.Value());

    FShaderDesc ps_d{};
    ps_d.stage       = EShaderStage::Pixel;
    ps_d.hlsl_source = kMotionHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "FMotionVector.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    _ps = Move(ps_r.Value());

    FPipelineDesc pd{};
    pd.vs            = _vs.Get();
    pd.ps            = _ps.Get();
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
    _pipeline = Move(pl_r.Value());

    return Ok();
}

void FMotionVector::Begin(IRhiCommandList& cl,
                         const FMat4& view_proj, const FMat4& prev_view_proj) noexcept {
    if (!_motion || !_normal || !_depth || !_pipeline) return;
    _vp      = view_proj;
    _prev_vp = prev_view_proj;
    // motion / normal RT を (0,0,0,0) クリア → 描かれない pixel (= sky 等) は
    // motion 0 (TAA は hist_uv = uv で reproject 無し)、normal 0 (SSR/SSGI/SSAO は
    // sky を depth で先に弾くので未使用)。
    IRhiTexture* rts[2] = { _motion.Get(), _normal.Get() };
    cl.BeginRenderToTextureMrt(rts, 2, FClearColor{0, 0, 0, 0}, _depth.Get(), 1.0f);
    cl.SetPipeline(*_pipeline);
}

void FMotionVector::DrawMesh(IRhiCommandList& cl, const FGpuMesh& mesh,
                            const FMat4& model, const FMat4& prev_model) noexcept {
    if (!_cb || !mesh.vertex_buffer || !mesh.index_buffer) return;
    MotionCB cb{};
    cb.curr_mvp   = model      * _vp;
    cb.prev_mvp   = prev_model * _prev_vp;
    cb.curr_model = model;
    _cb->Update(&cb, sizeof(cb));

    cl.SetConstantBuffer(0, *_cb);
    cl.SetVertexBuffer(*mesh.vertex_buffer, mesh.vertex_stride);
    cl.SetIndexBuffer(*mesh.index_buffer);
    cl.DrawIndexed(mesh.index_count);
}

void FMotionVector::End(IRhiCommandList& cl) noexcept {
    if (!_motion) return;
    cl.EndRenderToTexture(*_motion);
}

} // namespace acs
