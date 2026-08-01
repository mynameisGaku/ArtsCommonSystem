// SPDX-License-Identifier: Apache-2.0
// Hi-Z 実装
#include "render/HiZ.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs {

namespace {

// fullscreen triangle:
//   PSBase   : scene depth の 8x8 block min → level 0
//   PSReduce : level N-1 の厳密 2x2 min → level N
//
// pyramid は同じ mip layout を持つ 2 texture に偶奇 level を分ける。同一 texture の
// mip を読みながら別 mipへ書かないため、whole-resource transition の RHI でも安全。
const char* kHiZHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer HiZCB : register(b0) {
    uint4 params;      // x=source mip, y=destination mip
};

Texture2D source_tex : register(t0);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.uv  = uv;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.pos.y = -o.pos.y;
    return o;
}

float4 PSBase(VSOut v) : SV_TARGET {
    uint src_w, src_h;
    source_tex.GetDimensions(src_w, src_h);
    uint2 src_size = uint2(src_w, src_h);
    // SV_POSITION identifies the integer destination block exactly. Deriving
    // the block from interpolated fullscreen UV shifts the final blocks when
    // the source dimensions are not multiples of eight.
    uint2 block = uint2(v.pos.xy);
    uint2 base = block * 8u;
    float mn = 1.0;
    [unroll]
    for (int y = 0; y < 8; ++y) {
        [unroll]
        for (int x = 0; x < 8; ++x) {
            uint2 pixel = base + uint2(x, y);
            if (pixel.x >= src_size.x || pixel.y >= src_size.y) continue;
            float d = source_tex.Load(int3(pixel, 0)).r;
            // sky 除外: SSR は sky 方向に反射先が無いので skip 距離を空けたい
            if (d < 0.9999) mn = min(mn, d);
        }
    }
    return float4(mn, 0, 0, 1);
}

float4 PSReduce(VSOut v) : SV_TARGET {
    uint src_mip = params.x;
    uint dst_mip = params.y;

    uint src_w, src_h, src_levels;
    source_tex.GetDimensions(src_mip, src_w, src_h, src_levels);

    uint2 src_size = uint2(src_w, src_h);
    uint2 dst_pixel = uint2(v.pos.xy);

    // Physical base dimensions are powers of two, so every destination texel
    // corresponds to exactly 2x2 source texels. When one axis has already
    // reached 1, the virtual second sample is padding depth 1.0 and is skipped.
    uint2 begin = dst_pixel * 2u;
    float mn = 1.0;
    [unroll]
    for (uint y = 0; y < 2; ++y) {
        [unroll]
        for (uint x = 0; x < 2; ++x) {
            uint2 pixel = begin + uint2(x, y);
            if (pixel.x < src_size.x && pixel.y < src_size.y) {
                mn = min(mn, source_tex.Load(int3(pixel, src_mip)).r);
            }
        }
    }
    return float4(mn, 0, 0, 1);
}
)";

struct FHiZcbLayout {
    u32 source_mip = 0;
    u32 destination_mip = 0;
    u32 pad0 = 0;
    u32 pad1 = 0;
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

constexpr u32 NextPowerOfTwo(u32 value) noexcept {
    if (value <= 1u) return 1u;
    --value;
    value |= value >> 1u;
    value |= value >> 2u;
    value |= value >> 4u;
    value |= value >> 8u;
    value |= value >> 16u;
    return value + 1u;
}

} // namespace

TResult<void> CHiZ::Init(IRhiDevice& device, u32 src_width, u32 src_height) noexcept {
    m_Device = &device;
    m_SrcW  = src_width;
    m_SrcH  = src_height;

    if (auto r = CreateRT(device, src_width, src_height); r.IsErr()) return r;
    if (auto r = CreatePipeline(device); r.IsErr()) return r;

    // A distinct CB per level avoids same-frame CPU overwrite hazards in Raw
    // DX12. Values are immutable across resize because they only contain mip ids.
    for (u32 level = 1; level < kMaxMipLevels; ++level) {
        FBufferDesc cbd{};
        cbd.size = CBSize<FHiZcbLayout>();
        cbd.usage = EBufferUsage::Uniform;
        cbd.cpu_writable = true;
        auto r = CreateRhiBuffer(device, cbd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_LevelCb[level] = Move(r.Value());

        FHiZcbLayout data{};
        data.source_mip = level - 1u;
        data.destination_mip = level;
        m_LevelCb[level]->Update(&data, sizeof(data));
    }

    return Ok();
}

TResult<void> CHiZ::CreateRT(IRhiDevice& device, u32 src_w, u32 src_h) noexcept {
    m_HizEven.Reset();
    m_HizOdd.Reset();
    m_HizW = (src_w + kBlockSize - 1u) / kBlockSize;
    m_HizH = (src_h + kBlockSize - 1u) / kBlockSize;
    if (m_HizW < 1u) m_HizW = 1u;
    if (m_HizH < 1u) m_HizH = 1u;
    m_PhysicalW = NextPowerOfTwo(m_HizW);
    m_PhysicalH = NextPowerOfTwo(m_HizH);

    m_MipCount = 1u;
    u32 largest = m_PhysicalW > m_PhysicalH ? m_PhysicalW : m_PhysicalH;
    while (largest > 1u && m_MipCount < kMaxMipLevels) {
        largest >>= 1u;
        ++m_MipCount;
    }

    FTextureDesc td{};
    td.width  = m_PhysicalW;
    td.height = m_PhysicalH;
    // R32_Float が enum 未定義のため R32G32_Float (RG 2ch float) を採用。
    // .r に min depth、.g は未使用 (PS は float4 を返すが .g 以降は捨てられる)。
    // Hi-Z は min を過大評価すると occluder を飛び越すため、half の round-up が
    // level ごとに累積しない 32-bit float を使い、階層 skip を保守的に保つ。
    td.format = EFormat::R32G32_Float;
    td.mip_levels = m_MipCount;
    td.is_render_target = true;
    td.per_slice_rtv = true;

    auto even = CreateRhiTexture(device, td);
    if (even.IsErr()) return Err<void>(even.Error());
    m_HizEven = Move(even.Value());

    auto odd = CreateRhiTexture(device, td);
    if (odd.IsErr()) {
        m_HizEven.Reset();
        return Err<void>(odd.Error());
    }
    m_HizOdd = Move(odd.Value());
    return Ok();
}

TResult<void> CHiZ::CreatePipeline(IRhiDevice& device) noexcept {
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kHiZHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "CHiZ.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else m_Vs = Move(r.Value());

    FShaderDesc base_ps_d{};
    base_ps_d.stage = EShaderStage::Pixel;
    base_ps_d.hlsl_source = kHiZHLSL;
    base_ps_d.entry_point = "PSBase";
    base_ps_d.debug_name  = "CHiZ.BasePS";
    if (auto r = CreateRhiShader(device, base_ps_d); r.IsErr()) return Err<void>(r.Error());
    else m_PsBase = Move(r.Value());

    FShaderDesc reduce_ps_d{};
    reduce_ps_d.stage = EShaderStage::Pixel;
    reduce_ps_d.hlsl_source = kHiZHLSL;
    reduce_ps_d.entry_point = "PSReduce";
    reduce_ps_d.debug_name  = "CHiZ.ReducePS";
    if (auto r = CreateRhiShader(device, reduce_ps_d); r.IsErr()) return Err<void>(r.Error());
    else m_PsReduce = Move(r.Value());

    FPipelineDesc base_pd{};
    base_pd.vs            = m_Vs.Get();
    base_pd.ps            = m_PsBase.Get();
    base_pd.topology      = EPrimitiveTopology::TriangleList;
    base_pd.rt_format     = EFormat::R32G32_Float;
    base_pd.depth_format  = EFormat::Unknown;
    base_pd.depth_test    = false;
    base_pd.depth_write   = false;
    base_pd.cull_mode     = ECullMode::None;
    base_pd.blend_mode    = EBlendMode::Opaque;
    base_pd.cbuffer_slots = 0;
    base_pd.texture_slots = 1;
    base_pd.texture_names[0] = "source_tex";
    base_pd.static_sampler_count = 0;
    base_pd.vertex_stride = 0;
    base_pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, base_pd); r.IsErr()) return Err<void>(r.Error());
    else m_BasePipeline = Move(r.Value());

    FPipelineDesc reduce_pd = base_pd;
    reduce_pd.ps = m_PsReduce.Get();
    reduce_pd.cbuffer_slots = 1;
    reduce_pd.cbuffer_names[0] = "HiZCB";
    if (auto r = CreateRhiPipeline(device, reduce_pd); r.IsErr()) return Err<void>(r.Error());
    else m_ReducePipeline = Move(r.Value());

    return Ok();
}

void CHiZ::Shutdown() noexcept {
    m_ReducePipeline.Reset();
    m_BasePipeline.Reset();
    for (auto& cb : m_LevelCb) cb.Reset();
    m_PsReduce.Reset();
    m_PsBase.Reset();
    m_Vs.Reset();
    m_HizOdd.Reset();
    m_HizEven.Reset();
    m_MipCount = 0;
    m_PhysicalH = 0;
    m_PhysicalW = 0;
    m_Device = nullptr;
}

TResult<void> CHiZ::Resize(u32 src_width, u32 src_height) noexcept {
    if (!m_Device) return ACS_ERR(Render, 320, "CHiZ::Resize before Init");
    if (src_width == m_SrcW && src_height == m_SrcH) return Ok();
    m_SrcW = src_width;
    m_SrcH = src_height;
    return CreateRT(*m_Device, src_width, src_height);
}

void CHiZ::Build(IRhiDevice& /*device*/, IRhiCommandList& cl,
                IRhiTexture& scene_depth) noexcept {
    if (!m_HizEven || !m_HizOdd || !m_BasePipeline || !m_ReducePipeline ||
        m_MipCount == 0) {
        return;
    }

    // L0: full-resolution scene depth -> ceil(width/8) x ceil(height/8).
    cl.BeginRenderToTextureSlice(*m_HizEven, 0, 0,
                                 FClearColor{1, 0, 0, 0});
    cl.SetPipeline(*m_BasePipeline);
    cl.SetTexture(0, scene_depth);
    cl.Draw(3);
    cl.EndRenderToTexture(*m_HizEven);

    // L1..1x1: alternate physical resources so the source SRV and destination
    // RTV never overlap, even though each view covers/owns a complete mip chain.
    for (u32 level = 1; level < m_MipCount; ++level) {
        IRhiTexture* source = (level & 1u) ? m_HizEven.Get() : m_HizOdd.Get();
        IRhiTexture* destination = (level & 1u) ? m_HizOdd.Get() : m_HizEven.Get();
        IRhiBuffer* cb = m_LevelCb[level].Get();
        if (!source || !destination || !cb) return;

        cl.BeginRenderToTextureSlice(*destination, 0, level,
                                     FClearColor{1, 0, 0, 0});
        cl.SetPipeline(*m_ReducePipeline);
        cl.SetConstantBuffer(0, *cb);
        cl.SetTexture(0, *source);
        cl.Draw(3);
        cl.EndRenderToTexture(*destination);
    }
}

} // namespace acs
