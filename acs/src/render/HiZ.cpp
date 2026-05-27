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
    FVec4 params;
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

TResult<void> FHiZ::Init(IRhiDevice& device, u32 src_width, u32 src_height) noexcept {
    m_Device = &device;
    m_SrcW  = src_width;
    m_SrcH  = src_height;

    if (auto r = CreateRT(device, src_width, src_height); r.IsErr()) return r;
    if (auto r = CreatePipeline(device); r.IsErr()) return r;

    FBufferDesc cbd{};
    cbd.size = CBSize<HiZCBLayout>();
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else m_Cb = Move(r.Value());

    return Ok();
}

TResult<void> FHiZ::CreateRT(IRhiDevice& device, u32 src_w, u32 src_h) noexcept {
    m_Hiz.Reset();
    m_HizW = (src_w + kBlockSize - 1u) / kBlockSize;
    m_HizH = (src_h + kBlockSize - 1u) / kBlockSize;
    if (m_HizW < 1u) m_HizW = 1u;
    if (m_HizH < 1u) m_HizH = 1u;

    FTextureDesc td{};
    td.width  = m_HizW;
    td.height = m_HizH;
    // R16_Float が enum 未定義のため R16G16_Float (RG 2ch half) を採用。
    // .r に min depth、.g は未使用 (PS は float4 を返すが .g 以降は捨てられる)。
    // 16-bit half は [0,1] NDC depth に対し 10-bit mantissa = ~0.1% 精度 → skip
    // 距離計算用途には十分。32-bit が要るなら R16G16B16A16_Float か EFormat に
    // R32_Float を新規追加する必要がある (将来課題)。
    td.format = EFormat::R16G16_Float;
    td.is_render_target = true;
    auto r = CreateRhiTexture(device, td);
    if (r.IsErr()) return Err<void>(r.Error());
    m_Hiz = Move(r.Value());
    return Ok();
}

TResult<void> FHiZ::CreatePipeline(IRhiDevice& device) noexcept {
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kHiZHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "FHiZ.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else m_Vs = Move(r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kHiZHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "FHiZ.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else m_Ps = Move(r.Value());

    FPipelineDesc pd{};
    pd.vs            = m_Vs.Get();
    pd.ps            = m_Ps.Get();
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
    else m_Pipeline = Move(r.Value());

    return Ok();
}

void FHiZ::Shutdown() noexcept {
    m_Pipeline.Reset();
    m_Cb.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
    m_Hiz.Reset();
    m_Device = nullptr;
}

TResult<void> FHiZ::Resize(u32 src_width, u32 src_height) noexcept {
    if (!m_Device) return ACS_ERR(Render, 320, "FHiZ::Resize before Init");
    if (src_width == m_SrcW && src_height == m_SrcH) return Ok();
    m_SrcW = src_width;
    m_SrcH = src_height;
    return CreateRT(*m_Device, src_width, src_height);
}

void FHiZ::Build(IRhiDevice& /*device*/, IRhiCommandList& cl,
                IRhiTexture& scene_depth) noexcept {
    if (!m_Hiz || !m_Pipeline || !m_Cb) return;

    HiZCBLayout data{};
    data.params = FVec4{1.0f / static_cast<f32>(m_SrcW),
                        1.0f / static_cast<f32>(m_SrcH), 0, 0};
    m_Cb->Update(&data, sizeof(data));

    cl.BeginRenderToTexture(*m_Hiz, ClearColor{1, 0, 0, 0}, nullptr, 1.0f);
    cl.SetPipeline(*m_Pipeline);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.SetTexture(0, scene_depth);
    cl.Draw(3);
    cl.EndRenderToTexture(*m_Hiz);
}

} // namespace acs
