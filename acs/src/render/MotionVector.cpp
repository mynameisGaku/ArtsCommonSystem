// MotionVector 実装 (Phase 34f-3)
#include "render/MotionVector.h"
#include "asset/MeshAsset.h"          // MeshVertex の input layout 用
#include "foundation/Move.h"
#include "foundation/Log.h"           // ACS_ERR

namespace acs {

namespace {

// VS: 現フレーム / 前フレームの clip pos を計算。
// PS: それぞれを UV に直して motion vector (prev_uv - curr_uv) を出力。
//   curr_mvp = curr_model * view_proj
//   prev_mvp = prev_model * prev_view_proj  (いずれも jitter なし)
const char* kMotionHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer MotionCB : register(b0) {
    float4x4 curr_mvp;
    float4x4 prev_mvp;
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
};

VSOut VSMain(VSIn v) {
    VSOut o;
    o.pos       = mul(float4(v.pos, 1.0), curr_mvp);
    o.curr_clip = o.pos;
    o.prev_clip = mul(float4(v.pos, 1.0), prev_mvp);
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET {
    // perspective divide で NDC、それを UV に変換 (y は反転)
    float2 curr_ndc = i.curr_clip.xy / max(i.curr_clip.w, 1e-6);
    float2 prev_ndc = i.prev_clip.xy / max(i.prev_clip.w, 1e-6);
    float2 curr_uv  = float2(curr_ndc.x * 0.5 + 0.5, -curr_ndc.y * 0.5 + 0.5);
    float2 prev_uv  = float2(prev_ndc.x * 0.5 + 0.5, -prev_ndc.y * 0.5 + 0.5);
    // TAA は hist_uv = uv + motion で前フレームを引く → motion = prev_uv - curr_uv
    return float4(prev_uv - curr_uv, 0.0, 0.0);
}
)";

// per-object 定数バッファ。curr/prev とも CPU 側で model*VP を合成して渡す。
struct MotionCB {
    Mat4 curr_mvp;
    Mat4 prev_mvp;
};

} // namespace

Result<void> MotionVector::Init(IRhiDevice& device, u32 width, u32 height) noexcept {
    _device = &device;
    _width  = width  > 0 ? width  : 1;
    _height = height > 0 ? height : 1;

    if (auto r = CreateTargets(device, _width, _height); r.IsErr()) return r;
    if (auto r = CreatePipeline(device);                 r.IsErr()) return r;

    BufferDesc cbd{};
    cbd.size         = 256;          // MotionCB (128B) を 256 アラインで確保
    cbd.usage        = BufferUsage::Uniform;
    cbd.cpu_writable = true;
    auto cbr = CreateRhiBuffer(device, cbd);
    if (cbr.IsErr()) return Err<void>(cbr.Error());
    _cb = Move(cbr.Value());

    return Ok();
}

void MotionVector::Shutdown() noexcept {
    _cb.Reset();
    _pipeline.Reset();
    _ps.Reset();
    _vs.Reset();
    _depth.Reset();
    _motion.Reset();
    _device = nullptr;
    _width  = 0;
    _height = 0;
}

Result<void> MotionVector::Resize(u32 width, u32 height) noexcept {
    if (!_device) return ACS_ERR(Render, 360, "MotionVector::Resize before Init");
    if (width == 0 || height == 0) return Ok();
    if (width == _width && height == _height) return Ok();
    _motion.Reset();
    _depth.Reset();
    _width  = width;
    _height = height;
    return CreateTargets(*_device, width, height);
}

Result<void> MotionVector::CreateTargets(IRhiDevice& device, u32 w, u32 h) noexcept {
    // motion RT: RG16F。.rg に screen-space motion (prev_uv - curr_uv)。
    TextureDesc md{};
    md.width  = w;
    md.height = h;
    md.format = Format::R16G16_Float;
    md.is_render_target = true;
    auto mr = CreateRhiTexture(device, md);
    if (mr.IsErr()) return Err<void>(mr.Error());
    _motion = Move(mr.Value());

    // 内部 depth: occlusion 判定用 (SRV は不要)。
    TextureDesc dd{};
    dd.width  = w;
    dd.height = h;
    dd.format = Format::D32_Float;
    dd.is_depth_target = true;
    auto dr = CreateRhiTexture(device, dd);
    if (dr.IsErr()) return Err<void>(dr.Error());
    _depth = Move(dr.Value());

    return Ok();
}

Result<void> MotionVector::CreatePipeline(IRhiDevice& device) noexcept {
    ShaderDesc vs_d{};
    vs_d.stage       = ShaderStage::Vertex;
    vs_d.hlsl_source = kMotionHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "MotionVector.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    _vs = Move(vs_r.Value());

    ShaderDesc ps_d{};
    ps_d.stage       = ShaderStage::Pixel;
    ps_d.hlsl_source = kMotionHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "MotionVector.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    _ps = Move(ps_r.Value());

    PipelineDesc pd{};
    pd.vs            = _vs.Get();
    pd.ps            = _ps.Get();
    pd.topology      = PrimitiveTopology::TriangleList;
    pd.rt_format     = Format::R16G16_Float;
    pd.depth_format  = Format::D32_Float;
    pd.depth_test    = true;
    pd.depth_write   = true;
    // 全面ラスタライズ + depth test で frontmost が勝つ。winding 依存を避けるため
    // cull は無効 (closed mesh では裏面が depth で落ちるので実害なし)。
    pd.cull_mode     = CullMode::None;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 0;
    pd.cbuffer_names[0] = "MotionCB";
    pd.vertex_stride = sizeof(MeshVertex);
    pd.layout[0] = { "POSITION", 0, Format::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, Format::R32G32B32_Float, 16 };
    pd.layout[2] = { "TEXCOORD", 0, Format::R32G32_Float,    32 };
    pd.layout_count = 3;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    _pipeline = Move(pl_r.Value());

    return Ok();
}

void MotionVector::Begin(IRhiCommandList& cl,
                         const Mat4& view_proj, const Mat4& prev_view_proj) noexcept {
    if (!_motion || !_depth || !_pipeline) return;
    _vp      = view_proj;
    _prev_vp = prev_view_proj;
    // motion RT を (0,0) クリア → 描かれない pixel (= sky 等) は motion 0 になり、
    // TAA は hist_uv = uv で reproject 無し (= 従来の sky 挙動と同じ)。
    cl.BeginRenderToTexture(*_motion, ClearColor{0, 0, 0, 0}, _depth.Get(), 1.0f);
    cl.SetPipeline(*_pipeline);
}

void MotionVector::DrawMesh(IRhiCommandList& cl, const GpuMesh& mesh,
                            const Mat4& model, const Mat4& prev_model) noexcept {
    if (!_cb || !mesh.vertex_buffer || !mesh.index_buffer) return;
    MotionCB cb{};
    cb.curr_mvp = model      * _vp;
    cb.prev_mvp = prev_model * _prev_vp;
    _cb->Update(&cb, sizeof(cb));

    cl.SetConstantBuffer(0, *_cb);
    cl.SetVertexBuffer(*mesh.vertex_buffer, mesh.vertex_stride);
    cl.SetIndexBuffer(*mesh.index_buffer);
    cl.DrawIndexed(mesh.index_count);
}

void MotionVector::End(IRhiCommandList& cl) noexcept {
    if (!_motion) return;
    cl.EndRenderToTexture(*_motion);
}

} // namespace acs
