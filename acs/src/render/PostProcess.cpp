// PostProcess (Bloom + ACES Tonemap) 実装
#include "render/PostProcess.h"
#include "foundation/Move.h"
#include "foundation/Log.h"
#include "math/Vec.h"

#include <cstring>

namespace acs {

namespace {

// 全画面 3 角形 (頂点バッファ無しで 3 頂点を SV_VertexID から作る)
const char* kFullscreenVS = R"(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    VSOut o;
    // (0,0)→(0,0), (2,0)→(1,0), (0,2)→(0,1) で全画面を覆う
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.uv = uv;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.pos.y = -o.pos.y;
    return o;
}
)";

// 抽出: 輝度 > threshold のみを通す (Knee curve でソフトヒザ)
const char* kExtractPS = R"(
cbuffer Post : register(b0) {
    float4 params0;   // x=threshold, y=intensity, z=radius, w=exposure
    float4 params1;   // x=gamma, y=texel_w, z=texel_h, w=pad
};
Texture2D    src : register(t0);
SamplerState src_sampler : register(s0);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 PSMain(VSOut v) : SV_TARGET {
    float3 c = src.Sample(src_sampler, v.uv).rgb;
    // Karis weighted average で firefly 抑制
    float l = max(max(c.r, c.g), c.b);
    float t = params0.x;
    float knee = max(l - t, 0.0);
    float w = knee / max(l, 0.0001);
    return float4(c * w, 1.0);
}
)";

// Downsample: 13-tap (Jimenez SIGGRAPH 2014 の partial)
const char* kDownsamplePS = R"(
cbuffer Post : register(b0) {
    float4 params0;
    float4 params1;   // y=texel_w, z=texel_h
};
Texture2D    src : register(t0);
SamplerState src_sampler : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 PSMain(VSOut v) : SV_TARGET {
    float2 t = float2(params1.y, params1.z);
    // 13-tap Jimenez (簡易版: 6-tap 中心重み付け)
    float3 a = src.Sample(src_sampler, v.uv + float2(-1, -1) * t).rgb;
    float3 b = src.Sample(src_sampler, v.uv + float2( 1, -1) * t).rgb;
    float3 c = src.Sample(src_sampler, v.uv + float2(-1,  1) * t).rgb;
    float3 d = src.Sample(src_sampler, v.uv + float2( 1,  1) * t).rgb;
    float3 e = src.Sample(src_sampler, v.uv).rgb;
    float3 sum = (a + b + c + d) * 0.25 * 0.5 + e * 0.5;
    return float4(sum, 1.0);
}
)";

// Upsample: tent filter で広域ぼかしを上の段に加算 (additive blend する想定)
const char* kUpsamplePS = R"(
cbuffer Post : register(b0) {
    float4 params0;   // z=radius
    float4 params1;   // y=texel_w, z=texel_h
};
Texture2D    src : register(t0);
SamplerState src_sampler : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 PSMain(VSOut v) : SV_TARGET {
    float r = params0.z;
    float2 t = float2(params1.y, params1.z) * r;
    // 9-tap tent filter
    float3 sum = float3(0, 0, 0);
    sum += src.Sample(src_sampler, v.uv + float2(-1,-1) * t).rgb * (1.0/16.0);
    sum += src.Sample(src_sampler, v.uv + float2( 0,-1) * t).rgb * (2.0/16.0);
    sum += src.Sample(src_sampler, v.uv + float2( 1,-1) * t).rgb * (1.0/16.0);
    sum += src.Sample(src_sampler, v.uv + float2(-1, 0) * t).rgb * (2.0/16.0);
    sum += src.Sample(src_sampler, v.uv + float2( 0, 0) * t).rgb * (4.0/16.0);
    sum += src.Sample(src_sampler, v.uv + float2( 1, 0) * t).rgb * (2.0/16.0);
    sum += src.Sample(src_sampler, v.uv + float2(-1, 1) * t).rgb * (1.0/16.0);
    sum += src.Sample(src_sampler, v.uv + float2( 0, 1) * t).rgb * (2.0/16.0);
    sum += src.Sample(src_sampler, v.uv + float2( 1, 1) * t).rgb * (1.0/16.0);
    return float4(sum, 1.0);
}
)";

// Tonemap + cinematic post-FX: chromatic aberration → HDR mix → tonemap → vignette → grain → gamma
const char* kTonemapPS = R"(
cbuffer Post : register(b0) {
    float4 params0;   // x=threshold, y=intensity, z=radius, w=exposure
    float4 params1;   // x=gamma, y=texel_w, z=texel_h, w=tonemap_kind (0=ACES, 1=AgX, 2=Reinhard ext)
    float4 params2;   // x=vignette_intensity, y=vignette_radius, z=chromatic_aberration, w=grain_intensity
    float4 params3;   // x=grain_time, y=ssr_intensity, zw=pad
    float4 cg0;       // x=saturation, y=contrast, z=temperature, w=tint
    float4 cg_lift;   // xyz=lift (shadow offset)
    float4 cg_gain;   // xyz=gain (highlight multiplier)
};
Texture2D    hdr   : register(t0);
Texture2D    bloom : register(t1);
Texture2D    ssr   : register(t2);
SamplerState hdr_sampler   : register(s0);
SamplerState bloom_sampler : register(s1);
SamplerState ssr_sampler   : register(s2);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float3 ACESFilm(float3 x) {
    // Narkowicz 2016 の近似
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// AgX (Troy Sobotka 2024、Filament の実装に近い形)。
// ACES より highlight 圧縮が緩く、彩度の暴発が少ない (UE5 デフォルトに近い見え方)。
float3 AgxLog(float3 x) {
    return clamp((log2(max(x, 1e-10)) + 12.47393) / (4.026069 + 12.47393), 0.0, 1.0);
}
float3 AgxLook(float3 x) {
    // 6th 次多項式 sigmoid
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return + 15.5 * x4 * x2
           - 40.14 * x4 * x
           + 31.96 * x4
           - 6.868 * x2 * x
           + 0.4298 * x2
           + 0.1191 * x
           - 0.00232;
}
float3 AgxTonemap(float3 x) {
    return AgxLook(AgxLog(x));
}

// Reinhard 拡張 (Lottes/Hable 風)
float3 ReinhardExt(float3 x, float white2) {
    return (x * (1.0 + x / white2)) / (1.0 + x);
}

float3 Tonemap(float3 c, int kind) {
    if (kind == 1) return AgxTonemap(c);
    if (kind == 2) return ReinhardExt(c, 16.0);
    return ACESFilm(c);
}

// Color grading (Phase 34h): tonemap 後 (LDR) に適用する ASC-CDL 風補正。
//   1) lift / gain: shadow offset + highlight multiplier
//   2) contrast: 0.5 を pivot とした curve
//   3) saturation: Rec.709 luminance を基準に grayscale との lerp
//   4) temperature / tint: 色温度シフト (簡易、CIE xy chroma 風)
float3 ColorGrade(float3 c) {
    // lift + gain
    c = c * cg_gain.rgb + cg_lift.rgb;
    // contrast (pivot=0.5)
    c = (c - 0.5) * cg0.y + 0.5;
    c = max(c, 0.0);
    // temperature/tint: 赤↔青 と緑↔マゼンタの加算シフト
    c.r += cg0.z * 0.10;     // temp +1 → +0.1 red
    c.b -= cg0.z * 0.10;     // temp +1 → -0.1 blue
    c.g += cg0.w * 0.10;     // tint +1 → +0.1 green
    c.r -= cg0.w * 0.05;
    c.b -= cg0.w * 0.05;
    // saturation (Rec.709 luminance)
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(float3(lum, lum, lum), c, cg0.x);
    return max(c, 0.0);
}

// procedural noise (Inigo Quilez 風) — film grain 用、低コスト
float HashGrain(float2 p, float t) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32 + t);
    return frac(p.x * p.y);
}

float4 PSMain(VSOut v) : SV_TARGET {
    // 1) Chromatic aberration: 中心から放射方向に R/G/B を分けてサンプル
    float2 center = float2(0.5, 0.5);
    float2 dir = v.uv - center;
    float dist_radial = length(dir);
    float ca = params2.z;
    float3 hdr_col;
    if (ca > 1e-5) {
        float2 ofs = dir * ca;
        hdr_col.r = hdr.Sample(hdr_sampler, v.uv + ofs).r;
        hdr_col.g = hdr.Sample(hdr_sampler, v.uv      ).g;
        hdr_col.b = hdr.Sample(hdr_sampler, v.uv - ofs).b;
    } else {
        hdr_col = hdr.Sample(hdr_sampler, v.uv).rgb;
    }
    hdr_col *= params0.w;       // exposure
    float3 bloom_col = bloom.Sample(bloom_sampler, v.uv).rgb * params0.y;
    float3 ssr_col   = ssr.Sample(ssr_sampler, v.uv).rgb * params3.y;
    float3 mixed = hdr_col + bloom_col + ssr_col;

    // 2) Tonemap
    int kind = (int)params1.w;
    float3 mapped = Tonemap(mixed, kind);

    // 2.5) Color grading (Phase 34h、tonemap 後 LDR で適用)
    mapped = ColorGrade(mapped);

    // 3) Vignette (radial darkening)
    float vig_r = max(params2.y, 1e-4);
    float vig_i = params2.x;
    float vig = smoothstep(1.0, vig_r, dist_radial * 1.414);     // 1.414 ~= sqrt(2)
    mapped *= lerp(1.0 - vig_i, 1.0, vig);

    // 4) Film grain (HDR 後、Gamma 前)
    float g_i = params2.w;
    if (g_i > 1e-5) {
        float n = HashGrain(v.uv * 1024.0, params3.x);
        mapped += (n - 0.5) * g_i;
    }

    // 5) Gamma
    mapped = pow(max(mapped, 0.0), 1.0 / max(params1.x, 0.0001));
    return float4(mapped, 1.0);
}
)";

// 各パスで使う共通の動的 CB レイアウト
struct PostCBLayout {
    Vec4 params0;   // x=threshold, y=intensity, z=radius, w=exposure
    Vec4 params1;   // x=gamma, y=texel_w, z=texel_h, w=tonemap_kind
    Vec4 params2;   // x=vignette_intensity, y=vignette_radius, z=ca, w=grain
    Vec4 params3;   // x=grain_time, y=ssr_intensity
    Vec4 cg0;       // x=saturation, y=contrast, z=temperature, w=tint
    Vec4 cg_lift;   // xyz=lift
    Vec4 cg_gain;   // xyz=gain
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

// 全画面三角形 (頂点バッファ無し) のパイプライン共通設定
void FillFullscreenLayout(PipelineDesc& pd) noexcept {
    pd.topology      = PrimitiveTopology::TriangleList;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    pd.cull_mode     = CullMode::None;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.depth_format  = Format::Unknown;
}

} // namespace

PostProcess::~PostProcess() noexcept {
    Shutdown();
}

Result<void> PostProcess::Init(IRhiDevice& device, u32 width, u32 height,
                                Format color_format) noexcept {
    _device = &device;
    _color_format = color_format;
    _width = width;
    _height = height;

    if (auto r = CreateRenderTargets(device, width, height); r.IsErr()) return r;
    if (auto r = CreatePipelines(device);                   r.IsErr()) return r;

    BufferDesc cbd{};
    cbd.size         = CBSize<PostCBLayout>();
    cbd.usage        = BufferUsage::Uniform;
    cbd.cpu_writable = true;
    auto cbr = CreateRhiBuffer(device, cbd);
    if (cbr.IsErr()) return Err<void>(cbr.Error());
    _cb_post = Move(cbr.Value());

    return Ok();
}

void PostProcess::Shutdown() noexcept {
    _cb_post.Reset();
    _pipe_tonemap.Reset();
    _pipe_upsample.Reset();
    _pipe_downsample.Reset();
    _pipe_extract.Reset();
    _ps_tonemap.Reset();
    _ps_upsample.Reset();
    _ps_downsample.Reset();
    _ps_extract.Reset();
    _vs_fullscreen.Reset();
    for (auto& m : _bloom_mips) m.Reset();
    _hdr_rt.Reset();
    _device = nullptr;
}

Result<void> PostProcess::Resize(u32 width, u32 height) noexcept {
    if (!_device) return ACS_ERR(Render, 300, "PostProcess::Resize before Init");
    if (width == _width && height == _height) return Ok();
    _hdr_rt.Reset();
    for (auto& m : _bloom_mips) m.Reset();
    _width  = width;
    _height = height;
    return CreateRenderTargets(*_device, width, height);
}

Result<void> PostProcess::CreateRenderTargets(IRhiDevice& device, u32 w, u32 h) noexcept {
    // メイン HDR RT
    TextureDesc td{};
    td.width  = w;
    td.height = h;
    td.format = _hdr_format;
    td.is_render_target = true;
    auto hr = CreateRhiTexture(device, td);
    if (hr.IsErr()) return Err<void>(hr.Error());
    _hdr_rt = Move(hr.Value());

    // Bloom mip chain (1/2, 1/4, 1/8, 1/16, 1/32)
    u32 mw = w, mh = h;
    for (u32 i = 0; i < kBloomMips; ++i) {
        mw = mw > 1 ? mw / 2 : 1;
        mh = mh > 1 ? mh / 2 : 1;
        TextureDesc bd{};
        bd.width  = mw;
        bd.height = mh;
        bd.format = _hdr_format;
        bd.is_render_target = true;
        auto br = CreateRhiTexture(device, bd);
        if (br.IsErr()) return Err<void>(br.Error());
        _bloom_mips[i] = Move(br.Value());
    }
    return Ok();
}

Result<void> PostProcess::CreatePipelines(IRhiDevice& device) noexcept {
    // ---- 共通 VS ----
    {
        ShaderDesc sd{};
        sd.stage = ShaderStage::Vertex;
        sd.hlsl_source = kFullscreenVS;
        sd.entry_point = "VSMain";
        sd.debug_name  = "Fullscreen.VS";
        auto r = CreateRhiShader(device, sd);
        if (r.IsErr()) return Err<void>(r.Error());
        _vs_fullscreen = Move(r.Value());
    }

    // ---- 各 PS ----
    auto compile_ps = [&](const char* src, const char* name,
                          UniquePtr<IRhiShader>& out) -> Result<void> {
        ShaderDesc sd{};
        sd.stage = ShaderStage::Pixel;
        sd.hlsl_source = src;
        sd.entry_point = "PSMain";
        sd.debug_name  = name;
        auto r = CreateRhiShader(device, sd);
        if (r.IsErr()) return Err<void>(r.Error());
        out = Move(r.Value());
        return Ok();
    };
    if (auto r = compile_ps(kExtractPS,    "Bloom.Extract",    _ps_extract);    r.IsErr()) return r;
    if (auto r = compile_ps(kDownsamplePS, "Bloom.Downsample", _ps_downsample); r.IsErr()) return r;
    if (auto r = compile_ps(kUpsamplePS,   "Bloom.Upsample",   _ps_upsample);   r.IsErr()) return r;
    if (auto r = compile_ps(kTonemapPS,    "Tonemap",          _ps_tonemap);    r.IsErr()) return r;

    // ---- Pipelines ----
    // Extract: HDR → bloom_mips[0]、Opaque blend
    {
        PipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = _vs_fullscreen.Get();
        pd.ps = _ps_extract.Get();
        pd.rt_format = _hdr_format;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 1;
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "src";
        pd.static_sampler_count = 1;
        pd.static_samplers[0].filter    = SamplerFilter::Linear;
        pd.static_samplers[0].address_u = SamplerAddress::Clamp;
        pd.static_samplers[0].address_v = SamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        _pipe_extract = Move(r.Value());
    }
    // Downsample: bloom_mips[i] → bloom_mips[i+1]、Opaque
    {
        PipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = _vs_fullscreen.Get();
        pd.ps = _ps_downsample.Get();
        pd.rt_format = _hdr_format;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 1;
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "src";
        pd.static_sampler_count = 1;
        pd.static_samplers[0].filter    = SamplerFilter::Linear;
        pd.static_samplers[0].address_u = SamplerAddress::Clamp;
        pd.static_samplers[0].address_v = SamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        _pipe_downsample = Move(r.Value());
    }
    // Upsample: bloom_mips[i+1] → bloom_mips[i]、Additive blend
    {
        PipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = _vs_fullscreen.Get();
        pd.ps = _ps_upsample.Get();
        pd.rt_format = _hdr_format;
        pd.blend_mode = BlendMode::Additive;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 1;
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "src";
        pd.static_sampler_count = 1;
        pd.static_samplers[0].filter    = SamplerFilter::Linear;
        pd.static_samplers[0].address_u = SamplerAddress::Clamp;
        pd.static_samplers[0].address_v = SamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        _pipe_upsample = Move(r.Value());
    }
    // Tonemap: HDR + bloom → backbuffer、Opaque
    {
        PipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = _vs_fullscreen.Get();
        pd.ps = _ps_tonemap.Get();
        pd.rt_format = _color_format;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 3;       // t0=hdr, t1=bloom, t2=ssr (Phase 34e)
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "hdr";
        pd.texture_names[1] = "bloom";
        pd.texture_names[2] = "ssr";
        pd.static_sampler_count = 3;
        pd.static_samplers[0].filter    = SamplerFilter::Linear;
        pd.static_samplers[0].address_u = SamplerAddress::Clamp;
        pd.static_samplers[0].address_v = SamplerAddress::Clamp;
        pd.static_samplers[1].filter    = SamplerFilter::Linear;
        pd.static_samplers[1].address_u = SamplerAddress::Clamp;
        pd.static_samplers[1].address_v = SamplerAddress::Clamp;
        pd.static_samplers[2].filter    = SamplerFilter::Linear;
        pd.static_samplers[2].address_u = SamplerAddress::Clamp;
        pd.static_samplers[2].address_v = SamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        _pipe_tonemap = Move(r.Value());
    }

    return Ok();
}

void PostProcess::Render(IRhiCommandList& cmd, IRhiSwapchain& swapchain, u32 buffer_index,
                          const PostProcessParams& params) noexcept {
    if (!_hdr_rt || !_pipe_extract) return;

    if (params.bloom_enabled) {
        // 1) Extract: HDR → mip[0]
        Pass_Extract(cmd, params);

        // 2) Downsample: mip[i] → mip[i+1]
        for (u32 i = 0; i + 1 < kBloomMips; ++i) {
            Pass_Downsample(cmd, i);
        }

        // 3) Upsample (additive): mip[i+1] → mip[i] に上書き加算
        for (u32 i = kBloomMips - 1; i > 0; --i) {
            Pass_Upsample(cmd, i - 1, params.bloom_radius);
        }
    }

    // 4) Tonemap: HDR + mip[0] → backbuffer
    Pass_Tonemap(cmd, swapchain, buffer_index, params);
}

namespace {
void UpdatePostCB(IRhiBuffer* cb, const PostProcessParams& p,
                  f32 texel_w, f32 texel_h) noexcept {
    if (!cb) return;
    PostCBLayout l{};
    l.params0 = Vec4{ p.bloom_threshold, p.bloom_intensity, p.bloom_radius, p.exposure };
    l.params1 = Vec4{ p.gamma, texel_w, texel_h, static_cast<f32>(p.tonemap_kind) };
    l.params2 = Vec4{ p.vignette_intensity, p.vignette_radius,
                      p.chromatic_aberration, p.grain_intensity };
    l.params3 = Vec4{ p.grain_time, p.ssr_intensity, 0, 0 };
    l.cg0     = Vec4{ p.cg_saturation, p.cg_contrast, p.cg_temperature, p.cg_tint };
    l.cg_lift = Vec4{ p.cg_lift.x, p.cg_lift.y, p.cg_lift.z, 0 };
    l.cg_gain = Vec4{ p.cg_gain.x, p.cg_gain.y, p.cg_gain.z, 0 };
    cb->Update(&l, sizeof(l), 0);
}
} // namespace

void PostProcess::Pass_Extract(IRhiCommandList& cmd, const PostProcessParams& p) noexcept {
    auto* dst = _bloom_mips[0].Get();
    if (!dst || !_hdr_rt) return;
    UpdatePostCB(_cb_post.Get(), p, 1.0f / dst->Width(), 1.0f / dst->Height());

    cmd.BeginRenderToTexture(*dst, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*_pipe_extract);
    cmd.SetConstantBuffer(0, *_cb_post);
    cmd.SetTexture(0, *_hdr_rt);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*dst);
}

void PostProcess::Pass_Downsample(IRhiCommandList& cmd, u32 from_mip) noexcept {
    auto* src = _bloom_mips[from_mip].Get();
    auto* dst = _bloom_mips[from_mip + 1].Get();
    if (!src || !dst) return;
    PostProcessParams p{};   // params 不要だが texel size のみ更新
    UpdatePostCB(_cb_post.Get(), p, 1.0f / src->Width(), 1.0f / src->Height());

    cmd.BeginRenderToTexture(*dst, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*_pipe_downsample);
    cmd.SetConstantBuffer(0, *_cb_post);
    cmd.SetTexture(0, *src);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*dst);
}

void PostProcess::Pass_Upsample(IRhiCommandList& cmd, u32 to_mip, f32 radius) noexcept {
    auto* src = _bloom_mips[to_mip + 1].Get();
    auto* dst = _bloom_mips[to_mip].Get();
    if (!src || !dst) return;
    PostProcessParams p{};
    p.bloom_radius = radius;
    UpdatePostCB(_cb_post.Get(), p, 1.0f / src->Width(), 1.0f / src->Height());

    // additive blend、clear はせず既存内容に加算
    cmd.SetPipeline(*_pipe_upsample);
    cmd.SetConstantBuffer(0, *_cb_post);
    cmd.SetTexture(0, *src);
    cmd.BeginRenderToTexture(*dst, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*dst);
}

void PostProcess::Pass_Tonemap(IRhiCommandList& cmd, IRhiSwapchain& sc, u32 buf_idx,
                                const PostProcessParams& p) noexcept {
    UpdatePostCB(_cb_post.Get(), p, 1.0f / sc.Width(), 1.0f / sc.Height());

    cmd.BeginRenderToSwapchain(sc, buf_idx, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*_pipe_tonemap);
    cmd.SetConstantBuffer(0, *_cb_post);
    if (_hdr_rt) cmd.SetTexture(0, *_hdr_rt);
    if (_bloom_mips[0]) cmd.SetTexture(1, *_bloom_mips[0]);
    // SSR slot: ユーザー指定があれば本物、なければ 0 寄与の bloom mip[最深] を fallback として使う
    // (SSR shader が `* ssr_intensity` で 0 にして無害化)
    if (p.ssr_texture) {
        cmd.SetTexture(2, *p.ssr_texture);
    } else if (_bloom_mips[kBloomMips - 1]) {
        cmd.SetTexture(2, *_bloom_mips[kBloomMips - 1]);  // 1/32 mip、SSR 指定なしなら ssr_intensity=0 で寄与なし
    }
    cmd.Draw(3, 0);
    cmd.EndRenderToSwapchain(sc, buf_idx);
}

} // namespace acs
