// SPDX-License-Identifier: Apache-2.0
// Screen-Space Ambient Occlusion 実装 (Phase 34j、HBAO-lite)
#include "render/Ssao.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs {

namespace {

const char* kSsaoHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer SsaoCB : register(b0) {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4   eye;              // xyz = world pos
    float4   params;           // x=intensity, y=radius, z=texel_w, w=texel_h
    float4x4 view;             // Phase 34j-6 GTAO: world → view 変換
    float4   light_dir;        // Phase 34q: dir light 0 への方向 (xyz, world、surface→light)
};

Texture2D    scene_depth    : register(t0);
Texture2D    normal_gbuffer : register(t1);   // world-space normal (Phase 34m/34o)
SamplerState scene_depth_sampler    : register(s0);
SamplerState normal_gbuffer_sampler : register(s1);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.uv  = uv;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.pos.y = -o.pos.y;
    return o;
}

// uv + depth → world pos
float3 ReconstructWorldPos(float2 uv, float depth) {
    float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    clip.y = -clip.y;
    float4 wp = mul(clip, inv_view_proj);
    return wp.xyz / max(wp.w, 1e-6);
}

// uv + depth → view-space pos (world を経由して view 変換)
float3 ReconstructViewPos(float2 uv, float depth) {
    return mul(float4(ReconstructWorldPos(uv, depth), 1.0), view).xyz;
}

// GTAO (Jimenez et al. 2016 "Practical Realtime Strategies for Accurate
// Indirect Occlusion")。各 slice で view ベクトルに対する両側の horizon angle
// を screen-march で求め、normal を slice 平面に射影した角度 n を使って
// cosine-weighted visibility を analytical に積分する。HBAO の「最大遮蔽」より
// 物理的に正確 (法線半球の被覆率をちゃんと積分する)。
float4 PSMain(VSOut v) : SV_TARGET {
    float depth = scene_depth.SampleLevel(scene_depth_sampler, v.uv, 0).r;
    if (depth >= 0.9999) return float4(1, 1, 1, 1);   // sky → no AO

    // view-space position
    float3 P = ReconstructViewPos(v.uv, depth);
    // normal G-buffer の world normal を view 空間へ変換 (Phase 34o)。
    // 旧 cross(ddx,ddy) は 2x2 quad 単位で faceted になり、GTAO の slice 平面
    // 射影が段差状にずれて AO がブロック状になっていた。
    float3 Nw = normalize(normal_gbuffer.SampleLevel(normal_gbuffer_sampler, v.uv, 0).xyz);
    float3 N  = normalize(mul(float4(Nw, 0.0), view).xyz);
    float3 V = normalize(-P);             // view 空間: eye = 原点
    if (dot(N, V) < 0.0) N = -N;

    const int   kSlices = 3;
    const int   kSteps  = 6;
    const float kRadius = max(params.y, 0.05);
    const float kIntensity = params.x;
    const float PI = 3.14159265;
    const float HALF_PI = 1.57079632;

    // Interleaved Gradient Noise jitter (slice 角 / step 距離をずらして banding 抑制)
    float jitter = frac(52.9829189 * frac(dot(v.pos.xy, float2(0.06711056, 0.00583715))));

    float visibility = 0.0;
    [unroll]
    for (int s = 0; s < kSlices; ++s) {
        float phi = (float(s) + jitter) * (PI / float(kSlices));
        float2 dir = float2(cos(phi), sin(phi));        // screen-space slice 方向

        // slice 平面: P, V, と screen 方向。view 空間では screen x/y ≈ view x/y。
        float3 omega  = float3(dir.x, dir.y, 0.0);
        float3 sliceN = normalize(cross(omega, V));     // slice 平面の法線
        // N を slice 平面に射影
        float3 projN  = N - sliceN * dot(N, sliceN);
        float  projNLen = length(projN);
        if (projNLen < 1e-4) continue;
        float3 projNn = projN / projNLen;
        // projN の V に対する角度 n (slice 平面内、符号付き)
        float n = acos(clamp(dot(projNn, V), -1.0, 1.0));
        float3 omegaIn = normalize(omega - sliceN * dot(omega, sliceN));
        if (dot(projNn, omegaIn) < 0.0) n = -n;

        // 両側の horizon cos を screen-march で探す
        float cH1 = -1.0;   // -omega 側 (init = 最も低い地平)
        float cH2 = -1.0;   // +omega 側
        // world radius → screen UV。非線形 depth ではなく view-space linear Z
        // (P.z) で割る。透視投影では screen size ∝ world size / view_z なので
        // これが次元的に正しい。0.5 は fov/aspect 由来の proj scale の粗い近似。
        float screen_r = kRadius * 0.5 / max(P.z, 0.1);
        [unroll]
        for (int t = 1; t <= kSteps; ++t) {
            float tt = (float(t) - 0.5 + jitter * 0.5) / float(kSteps);
            float2 off = dir * screen_r * tt;

            float2 uvA = v.uv + off;     // +omega 側
            if (uvA.x >= 0.0 && uvA.x <= 1.0 && uvA.y >= 0.0 && uvA.y <= 1.0) {
                float dA = scene_depth.SampleLevel(scene_depth_sampler, uvA, 0).r;
                if (dA < 0.9999) {
                    float3 D = ReconstructViewPos(uvA, dA) - P;
                    float  dl = length(D);
                    if (dl > 1e-4 && dl < kRadius) {
                        float c  = dot(D / dl, V);
                        float fo = saturate(1.0 - dl / kRadius);   // 距離 falloff
                        cH2 = max(cH2, lerp(-1.0, c, fo));
                    }
                }
            }
            float2 uvB = v.uv - off;     // -omega 側
            if (uvB.x >= 0.0 && uvB.x <= 1.0 && uvB.y >= 0.0 && uvB.y <= 1.0) {
                float dB = scene_depth.SampleLevel(scene_depth_sampler, uvB, 0).r;
                if (dB < 0.9999) {
                    float3 D = ReconstructViewPos(uvB, dB) - P;
                    float  dl = length(D);
                    if (dl > 1e-4 && dl < kRadius) {
                        float c  = dot(D / dl, V);
                        float fo = saturate(1.0 - dl / kRadius);
                        cH1 = max(cH1, lerp(-1.0, c, fo));
                    }
                }
            }
        }

        // horizon cos → angle。-omega 側は負角、+omega 側は正角。
        float h1 = -acos(clamp(cH1, -1.0, 1.0));
        float h2 =  acos(clamp(cH2, -1.0, 1.0));
        // normal 半球にクランプ (h は n ± HALF_PI の範囲に収める)
        h1 = n + max(h1 - n, -HALF_PI);
        h2 = n + min(h2 - n,  HALF_PI);
        // GTAO analytical integral (Jimenez 2016 Listing)
        float a1 = -cos(2.0 * h1 - n) + cos(n) + 2.0 * h1 * sin(n);
        float a2 = -cos(2.0 * h2 - n) + cos(n) + 2.0 * h2 * sin(n);
        visibility += projNLen * 0.25 * (a1 + a2);
    }
    visibility = saturate(visibility / float(kSlices));

    // visibility = 見える割合 (1=全開放、0=全遮蔽)。intensity で AO 強度調整。
    float ao = saturate(1.0 - (1.0 - visibility) * kIntensity);

    // ===== Contact shadow (Phase 34q) =====
    // dir light 0 へ向かう短距離 screen-space ray march。surface と光源の間に
    // geometry があれば直接光が遮られている (= 接地影)。shadow map が拾えない
    // 細かい接触遮蔽 (球と床の接地際など) を補う。結果は .g に焼き、blur で軟化、
    // PbrShader が direct light に乗算する。
    float contact = 1.0;
    float3 Pw = ReconstructWorldPos(v.uv, depth);
    float3 L  = normalize(light_dir.xyz);             // 光源へ向かう方向 (surface→light)
    if (dot(Nw, L) > 0.02) {                          // 光に面した pixel のみ march
        const int   kCSteps = 16;
        const float kCDist  = 0.45;                   // contact 距離 (world units)
        float3 cs0   = Pw + Nw * 0.012;               // self-occlusion バイアス
        float  cstep = kCDist / float(kCSteps);
        [loop]
        for (int cs = 1; cs <= kCSteps; ++cs) {
            float3 sp = cs0 + L * (cstep * (float(cs) - 0.5 + jitter));
            float4 cp = mul(float4(sp, 1.0), view_proj);
            if (cp.w < 1e-4) break;
            float3 cn = cp.xyz / cp.w;
            if (cn.x < -1.0 || cn.x > 1.0 || cn.y < -1.0 || cn.y > 1.0) break;
            float2 cuv = float2(cn.x * 0.5 + 0.5, -cn.y * 0.5 + 0.5);
            float  sd  = scene_depth.SampleLevel(scene_depth_sampler, cuv, 0).r;
            if (sd >= 0.9999) continue;               // sky は貫通
            // ray が scene surface の奥へ入った → 光路に geometry の可能性。
            // 厚みは NDC depth 差ではなく world 距離で判定する (NDC は非線形で
            // 距離次元にならない)。ray 点 sp と scene surface 点の world 距離が
            // 近ければ「光路の遮蔽体」、遠ければ「無関係な前景の裏」。
            if (cn.z > sd + 0.0008) {
                float3 scene_wp = ReconstructWorldPos(cuv, sd);
                if (distance(sp, scene_wp) < 0.10) { contact = 0.0; break; }
            }
        }
    }
    return float4(ao, contact, 0.0, 1.0);
}
)";

// Phase 34j-4: depth-aware bilateral blur。SSAO raw のノイズを、depth 不連続
// (シルエットエッジ) を跨がないように平滑化する。5x5 kernel、spatial gaussian ×
// depth similarity weight。
const char* kSsaoBlurHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer SsaoCB : register(b0) {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4   eye;
    float4   params;     // z=texel_w, w=texel_h
    float4x4 view;       // blur では未使用、CB レイアウト整合のため宣言
    float4   light_dir;  // blur では未使用、CB レイアウト整合のため宣言
};

Texture2D    ssao_raw    : register(t0);
Texture2D    scene_depth : register(t1);
SamplerState ssao_raw_sampler    : register(s0);
SamplerState scene_depth_sampler : register(s1);

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
    float2 tx = float2(params.z, params.w);
    float center_d = scene_depth.SampleLevel(scene_depth_sampler, v.uv, 0).r;

    // 5x5 depth-aware bilateral blur (.r=AO, .g=contact shadow を一括平滑化)
    float2 sum = float2(0, 0);
    float  wsum = 0.0;
    const int kR = 2;
    [unroll]
    for (int dy = -kR; dy <= kR; ++dy) {
        [unroll]
        for (int dx = -kR; dx <= kR; ++dx) {
            float2 uv = v.uv + float2(dx, dy) * tx;
            float2 oc = ssao_raw.SampleLevel(ssao_raw_sampler, uv, 0).rg;
            float d  = scene_depth.SampleLevel(scene_depth_sampler, uv, 0).r;
            // spatial gaussian (sigma ~= 2 px)
            float sw = exp(-float(dx*dx + dy*dy) / 8.0);
            // depth similarity (edge stopping): depth 差が ~0.005 で weight 半減
            float dd = d - center_d;
            float dw = exp(-dd * dd * 25000.0);
            float w  = sw * dw;
            sum  += oc * w;
            wsum += w;
        }
    }
    float2 result = (wsum > 1e-5)
                    ? sum / wsum
                    : ssao_raw.SampleLevel(ssao_raw_sampler, v.uv, 0).rg;
    return float4(result.x, result.y, 0.0, 1.0);
}
)";

struct SsaoCBLayout {
    FMat4 view_proj;
    FMat4 inv_view_proj;
    FVec4 eye;
    FVec4 params;
    FMat4 view;             // Phase 34j-6 GTAO
    FVec4 light_dir;        // Phase 34q: contact shadow 用 dir light 0 方向
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

TResult<void> Ssao::Init(IRhiDevice& device, u32 width, u32 height) noexcept {
    _device = &device;
    _width = width;
    _height = height;

    if (auto r = CreateOutputRT(device, width, height); r.IsErr()) return r;
    if (auto r = CreatePipeline(device); r.IsErr()) return r;

    FBufferDesc cbd{};
    cbd.size = CBSize<SsaoCBLayout>();
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else _cb = Move(r.Value());

    return Ok();
}

TResult<void> Ssao::CreateOutputRT(IRhiDevice& device, u32 width, u32 height) noexcept {
    _output.Reset();
    _blur_output.Reset();
    FTextureDesc td{};
    td.width  = width;
    td.height = height;
    // RGBA8 だが .r のみ使用 (R8_UNorm を EFormat に追加するより既存型流用が小コスト)
    td.format = EFormat::R8G8B8A8_UNorm;
    td.is_render_target = true;
    auto r = CreateRhiTexture(device, td);
    if (r.IsErr()) return Err<void>(r.Error());
    _output = Move(r.Value());

    // Phase 34j-4: blur 後の RT (同フォーマット / 同サイズ)
    auto br = CreateRhiTexture(device, td);
    if (br.IsErr()) return Err<void>(br.Error());
    _blur_output = Move(br.Value());
    return Ok();
}

TResult<void> Ssao::CreatePipeline(IRhiDevice& device) noexcept {
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kSsaoHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Ssao.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else _vs = Move(r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSsaoHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Ssao.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else _ps = Move(r.Value());

    FPipelineDesc pd{};
    pd.vs            = _vs.Get();
    pd.ps            = _ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = EFormat::R8G8B8A8_UNorm;
    pd.depth_format  = EFormat::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 2;
    pd.cbuffer_names[0] = "SsaoCB";
    pd.texture_names[0] = "scene_depth";
    pd.texture_names[1] = "normal_gbuffer";
    pd.static_sampler_count = 2;
    pd.static_samplers[0].filter    = ESamplerFilter::Point;       // depth は離散値 → Point
    pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[1].filter    = ESamplerFilter::Point;       // 法線は per-pixel 厳密に
    pd.static_samplers[1].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[1].address_v = ESamplerAddress::Clamp;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else _pipeline = Move(r.Value());

    // Phase 34j-4: blur pipeline (ssao_raw + scene_depth → blurred)。
    // VS は SSAO 本体と同じ fullscreen-triangle なので _vs を再利用する
    // (kSsaoHLSL と kSsaoBlurHLSL の VSMain は同一構造)。
    FShaderDesc bps_d{};
    bps_d.stage = EShaderStage::Pixel;
    bps_d.hlsl_source = kSsaoBlurHLSL;
    bps_d.entry_point = "PSMain";
    bps_d.debug_name  = "SsaoBlur.PS";
    if (auto r = CreateRhiShader(device, bps_d); r.IsErr()) return Err<void>(r.Error());
    else _blur_ps = Move(r.Value());

    FPipelineDesc bpd{};
    bpd.vs            = _vs.Get();
    bpd.ps            = _blur_ps.Get();
    bpd.topology      = EPrimitiveTopology::TriangleList;
    bpd.rt_format     = EFormat::R8G8B8A8_UNorm;
    bpd.depth_format  = EFormat::Unknown;
    bpd.depth_test    = false;
    bpd.depth_write   = false;
    bpd.cull_mode     = ECullMode::None;
    bpd.blend_mode    = EBlendMode::Opaque;
    bpd.cbuffer_slots = 1;
    bpd.texture_slots = 2;
    bpd.cbuffer_names[0] = "SsaoCB";
    bpd.texture_names[0] = "ssao_raw";
    bpd.texture_names[1] = "scene_depth";
    bpd.static_sampler_count = 2;
    bpd.static_samplers[0].filter    = ESamplerFilter::Linear;     // AO は linear で補間
    bpd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    bpd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    bpd.static_samplers[1].filter    = ESamplerFilter::Point;      // depth は離散値
    bpd.static_samplers[1].address_u = ESamplerAddress::Clamp;
    bpd.static_samplers[1].address_v = ESamplerAddress::Clamp;
    bpd.vertex_stride = 0;
    bpd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, bpd); r.IsErr()) return Err<void>(r.Error());
    else _blur_pipeline = Move(r.Value());
    return Ok();
}

void Ssao::Shutdown() noexcept {
    _blur_pipeline.Reset();
    _pipeline.Reset();
    _cb.Reset();
    _blur_ps.Reset();
    _ps.Reset();
    _vs.Reset();
    _blur_output.Reset();
    _output.Reset();
    _device = nullptr;
}

TResult<void> Ssao::Resize(u32 width, u32 height) noexcept {
    if (!_device) return ACS_ERR(Render, 330, "Ssao::Resize before Init");
    if (width == _width && height == _height) return Ok();
    _width = width;
    _height = height;
    return CreateOutputRT(*_device, width, height);
}

void Ssao::Render(IRhiDevice& /*device*/, IRhiCommandList& cl,
                  IRhiTexture& scene_depth,
                  IRhiTexture& normal_gbuffer,
                  const FMat4& view_proj, const FMat4& inv_view_proj,
                  const FMat4& view,
                  FVec3 eye, FVec3 light_dir,
                  f32 intensity, f32 radius) noexcept {
    if (!_output || !_blur_output || !_pipeline || !_blur_pipeline || !_cb) return;
    SsaoCBLayout data{};
    data.view_proj     = view_proj;
    data.inv_view_proj = inv_view_proj;
    data.eye           = FVec4{eye.x, eye.y, eye.z, 1};
    data.params        = FVec4{intensity, radius,
                               1.0f / static_cast<f32>(_width),
                               1.0f / static_cast<f32>(_height)};
    data.view          = view;       // Phase 34j-6 GTAO: world → view
    data.light_dir     = FVec4{light_dir.x, light_dir.y, light_dir.z, 0};
    _cb->Update(&data, sizeof(data));

    // Pass 1: SSAO raw → _output
    cl.BeginRenderToTexture(*_output, ClearColor{1, 1, 1, 1}, nullptr, 1.0f);
    cl.SetPipeline(*_pipeline);
    cl.SetConstantBuffer(0, *_cb);
    cl.SetTexture(0, scene_depth);
    cl.SetTexture(1, normal_gbuffer);
    cl.Draw(3);
    cl.EndRenderToTexture(*_output);

    // Pass 2 (Phase 34j-4): depth-aware bilateral blur → _blur_output
    cl.BeginRenderToTexture(*_blur_output, ClearColor{1, 1, 1, 1}, nullptr, 1.0f);
    cl.SetPipeline(*_blur_pipeline);
    cl.SetConstantBuffer(0, *_cb);
    cl.SetTexture(0, *_output);       // SSAO raw
    cl.SetTexture(1, scene_depth);
    cl.Draw(3);
    cl.EndRenderToTexture(*_blur_output);
}

} // namespace acs
