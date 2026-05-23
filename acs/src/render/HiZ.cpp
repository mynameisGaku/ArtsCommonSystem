// SPDX-License-Identifier: Apache-2.0
// Hi-Z 実装 (Phase 36-3a)
#include "render/HiZ.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs {

namespace {

// fullscreen triangle → 1/8 解像度の min-depth RT。
// 各 dst pixel = src の 8x8 ブロック min。sky (depth>=0.9999) は除外して
// "skip 可能距離" を最大化する (= ground/sky の反射 SSR が高速化)。
const char* kHiZHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer HiZCB : register(b0) {
    float4 params;     // x=inv_src_w, y=inv_src_h, zw=pad
};

Texture2D    scene_depth         : register(t0);
SamplerState scene_depth_sampler : register(s0);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.uv  = uv;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.pos.y = -o.pos.y;
    return o;
}

float4 PSMain(VSOut v) : SV_TARGET {
    float2 inv_src = params.xy;
    // dst pixel center (v.uv) は src 上の 8x8 ブロック中心。8x8 = 64 tap min。
    // base = 左上 texel の中心 = uv - 4 texel + 0.5 texel
    float2 base = v.uv + inv_src * (-3.5);
    float mn = 1.0;
    [unroll]
    for (int y = 0; y < 8; ++y) {
        [unroll]
        for (int x = 0; x < 8; ++x) {
            float2 uv = base + float2(x, y) * inv_src;
            float d = scene_depth.SampleLevel(scene_depth_sampler, uv, 0).r;
            // sky 除外: SSR は sky 方向に反射先が無いので skip 距離を空けたい
            if (d < 0.9999) mn = min(mn, d);
        }
    }
    return float4(mn, 0, 0, 1);
}
)";

struct HiZCBLayout {
    Vec4 params;
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

Result<void> HiZ::Init(IRhiDevice& device, u32 src_width, u32 src_height) noexcept {
    _device = &device;
    _src_w  = src_width;
    _src_h  = src_height;

    if (auto r = CreateRT(device, src_width, src_height); r.IsErr()) return r;
    if (auto r = CreatePipeline(device); r.IsErr()) return r;

    BufferDesc cbd{};
    cbd.size = CBSize<HiZCBLayout>();
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else _cb = Move(r.Value());

    return Ok();
}

Result<void> HiZ::CreateRT(IRhiDevice& device, u32 src_w, u32 src_h) noexcept {
    _hiz.Reset();
    _hiz_w = (src_w + kBlockSize - 1u) / kBlockSize;
    _hiz_h = (src_h + kBlockSize - 1u) / kBlockSize;
    if (_hiz_w < 1u) _hiz_w = 1u;
    if (_hiz_h < 1u) _hiz_h = 1u;

    TextureDesc td{};
    td.width  = _hiz_w;
    td.height = _hiz_h;
    // R16_Float が enum 未定義のため R16G16_Float (RG 2ch half) を採用。
    // .r に min depth、.g は未使用 (PS は float4 を返すが .g 以降は捨てられる)。
    // 16-bit half は [0,1] NDC depth に対し 10-bit mantissa = ~0.1% 精度 → skip
    // 距離計算用途には十分。32-bit が要るなら R16G16B16A16_Float か EFormat に
    // R32_Float を新規追加する必要がある (将来課題)。
    td.format = EFormat::R16G16_Float;
    td.is_render_target = true;
    auto r = CreateRhiTexture(device, td);
    if (r.IsErr()) return Err<void>(r.Error());
    _hiz = Move(r.Value());
    return Ok();
}

Result<void> HiZ::CreatePipeline(IRhiDevice& device) noexcept {
    ShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kHiZHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "HiZ.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else _vs = Move(r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kHiZHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "HiZ.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else _ps = Move(r.Value());

    PipelineDesc pd{};
    pd.vs            = _vs.Get();
    pd.ps            = _ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = EFormat::R16G16_Float;
    pd.depth_format  = EFormat::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 1;
    pd.cbuffer_names[0] = "HiZCB";
    pd.texture_names[0] = "scene_depth";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = ESamplerFilter::Point;
    pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else _pipeline = Move(r.Value());

    return Ok();
}

void HiZ::Shutdown() noexcept {
    _pipeline.Reset();
    _cb.Reset();
    _ps.Reset();
    _vs.Reset();
    _hiz.Reset();
    _device = nullptr;
}

Result<void> HiZ::Resize(u32 src_width, u32 src_height) noexcept {
    if (!_device) return ACS_ERR(Render, 320, "HiZ::Resize before Init");
    if (src_width == _src_w && src_height == _src_h) return Ok();
    _src_w = src_width;
    _src_h = src_height;
    return CreateRT(*_device, src_width, src_height);
}

void HiZ::Build(IRhiDevice& /*device*/, IRhiCommandList& cl,
                IRhiTexture& scene_depth) noexcept {
    if (!_hiz || !_pipeline || !_cb) return;

    HiZCBLayout data{};
    data.params = Vec4{1.0f / static_cast<f32>(_src_w),
                        1.0f / static_cast<f32>(_src_h), 0, 0};
    _cb->Update(&data, sizeof(data));

    cl.BeginRenderToTexture(*_hiz, ClearColor{1, 0, 0, 0}, nullptr, 1.0f);
    cl.SetPipeline(*_pipeline);
    cl.SetConstantBuffer(0, *_cb);
    cl.SetTexture(0, scene_depth);
    cl.Draw(3);
    cl.EndRenderToTexture(*_hiz);
}

} // namespace acs
