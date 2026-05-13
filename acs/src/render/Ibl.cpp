// Image-Based Lighting 実装 (Phase 31)
//
// 現段階の機能: BRDF LUT 生成、Sky → env cubemap キャプチャ、skybox preview 描画。
// irradiance / prefilter は後続ステップで追加する。
#include "render/Ibl.h"
#include "render/Sky.h"
#include "math/Mat.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <cstring>

namespace acs {

namespace {
// IBL の build / draw 系は Diligent backend 専用 (BeginRenderToTextureSlice /
// cubemap / R11G11B10F に依存)。Dx12 raw backend では呼ばれても crash しないよう
// 早期 return できるかチェックする。
bool IsDiligentBackend(IRhiDevice& device) noexcept {
    const char* n = device.BackendName();
    return n && std::strncmp(n, "Diligent", 8) == 0;
}
} // namespace

namespace {

// BRDF LUT 生成シェーダ。
// Epic Games "Real Shading in UE4" の split-sum approximation。
// 出力 RG16F: r=scale (F0 にかける係数), g=bias (F0 と無関係なオフセット)
// 実行時 PBR 反射: F0 * lut.r + lut.g を Fresnel-modulated specular IBL の係数として使う。
//
// **テクスチャ座標規約 (PbrShader IBL 統合時に必ず一致させること)**:
//   ・row 0  (texture top, v=0) = roughness 0  (鏡面)
//   ・row 255 (texture bottom, v=1) = roughness 1 (粗い表面)
//   ・col 0  (texture left,  u=0) = NdotV 0  (grazing 角)
//   ・col 255 (texture right, u=1) = NdotV 1  (正対)
// → PbrShader 側で `brdf_lut.Sample(s, float2(NdotV, roughness))` で sampling 可能。
const char* kBrdfLutHLSL = R"(
#pragma pack_matrix(row_major)

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;     // (0,0)=top-left, (1,1)=bottom-right。LUT のサンプル座標と一致させる
};

VSOut VSMain(uint id : SV_VertexID) {
    // 大三角形でフルスクリーンを覆う。uv は (0,0) / (2,0) / (0,2)、pos は (-1,+1) / (3,+1) / (-1,-3)
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.uv = uv;
    // pos.y を反転して uv.y=0 が「画面上=テクスチャ row 0」と一致するようにする
    o.pos = float4(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0), 0.0, 1.0);
    return o;
}

static const float PI = 3.14159265358979323846;

// Hammersley low-discrepancy 2D point on [0,1)^2
float radicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;     // 1/2^32
}
float2 Hammersley(uint i, uint n) {
    return float2(float(i) / float(n), radicalInverseVdC(i));
}

// GGX (Trowbridge-Reitz) importance-sampled half-vector in tangent space, then world-space N rotation
float3 ImportanceSampleGGX(float2 xi, float roughness) {
    float a = roughness * roughness;
    float phi      = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// Smith joint geometry for IBL: k = a^2 / 2 (direct light は (a+1)^2/8 だが IBL は別物)
float G_SchlickIBL(float NoV, float roughness) {
    float a = roughness;
    float k = (a * a) * 0.5;
    return NoV / (NoV * (1.0 - k) + k);
}
float G_SmithIBL(float NoV, float NoL, float roughness) {
    return G_SchlickIBL(NoV, roughness) * G_SchlickIBL(NoL, roughness);
}

// 1024 サンプルで split-sum BRDF を積分する。N=(0,0,1) 固定で V のみ平面回転で表現。
float2 IntegrateBRDF(float NoV, float roughness) {
    float3 V;
    V.x = sqrt(max(0.0, 1.0 - NoV * NoV));
    V.y = 0.0;
    V.z = NoV;

    float A = 0.0;
    float B = 0.0;
    const uint kSamples = 1024u;
    for (uint i = 0u; i < kSamples; ++i) {
        float2 xi = Hammersley(i, kSamples);
        float3 H  = ImportanceSampleGGX(xi, roughness);    // tangent-space H, N is implicit (0,0,1)
        float3 L  = 2.0 * dot(V, H) * H - V;

        float NoL = saturate(L.z);
        float NoH = saturate(H.z);
        float VoH = saturate(dot(V, H));

        if (NoL > 0.0) {
            float G     = G_SmithIBL(NoV, NoL, roughness);
            float G_vis = (G * VoH) / max(NoH * NoV, 1e-6);
            float Fc    = pow(1.0 - VoH, 5.0);
            A += (1.0 - Fc) * G_vis;
            B +=        Fc  * G_vis;
        }
    }
    return float2(A, B) / float(kSamples);
}

float2 PSMain(VSOut v) : SV_TARGET {
    // u 軸 = NdotV、v 軸 = roughness。両端は数値不安定なので 0 近傍を避ける。
    float NoV       = max(v.uv.x, 1.0 / 256.0);
    float roughness = max(v.uv.y, 1.0 / 256.0);
    return IntegrateBRDF(NoV, roughness);
}
)";

constexpr u32 kBrdfLutSize    = 256;
constexpr u32 kEnvCubeSize    = 256;
constexpr u32 kIrradianceSize = 32;
constexpr u32 kPrefilterSize  = 128;
constexpr u32 kPrefilterMips  = 5;     // 128/64/32/16/8 → roughness 0/0.25/0.5/0.75/1.0

// ---- 環境 cubemap キャプチャ (Sky procedural を 6 face に焼く) ----
//
// Sky.cpp の手続き式 (高さ角でグラデ + 太陽 disc) と同じ式を per-face で評価する。
// face_index で 6 面それぞれの (uv → world dir) 変換を選ぶ。
const char* kEnvCaptureHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer EnvCapture : register(b0) {
    int4   face_pad;          // x=face index 0..5
    float4 sun_dir;           // xyz=方向 (camera→sun)
    float4 sun_color;
    float4 sun_params;        // x=radius (1-cos), y=glow
    float4 zenith_color;
    float4 horizon_color;
    float4 ground_color;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;   // (0,0)=face top-left, (1,1)=face bottom-right
};

VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.uv  = uv;
    // pos.y を反転して uv.y=0 が「face row 0」と一致
    o.pos = float4(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0), 0.0, 1.0);
    return o;
}

// 標準 D3D11/12 cubemap face mapping。
// uv.y=0 が face row 0 (= 後で TextureCube.Sample が返す top) になる前提。
// Filament の cmgen が採用してる formulation と同等。
float3 CubeFaceDir(float2 uv01, int face) {
    float2 m = uv01 * 2.0 - 1.0;
    if (face == 0) return float3( 1.0, -m.y, -m.x);    // +X
    if (face == 1) return float3(-1.0, -m.y,  m.x);    // -X
    if (face == 2) return float3( m.x,  1.0,  m.y);    // +Y
    if (face == 3) return float3( m.x, -1.0, -m.y);    // -Y
    if (face == 4) return float3( m.x, -m.y,  1.0);    // +Z
    return                 float3(-m.x, -m.y, -1.0);   // -Z
}

float3 ProcSky(float3 dir) {
    float t = dir.y;
    float3 sky;
    if (t >= 0.0) {
        sky = lerp(horizon_color.xyz, zenith_color.xyz, pow(saturate(t), 0.6));
    } else {
        sky = lerp(horizon_color.xyz, ground_color.xyz, pow(saturate(-t), 0.6));
    }
    float c   = saturate(dot(dir, normalize(sun_dir.xyz)));
    float ang = 1.0 - c;
    if (ang < sun_params.x) {
        sky = sun_color.xyz;
    } else if (ang < sun_params.y) {
        float k = 1.0 - smoothstep(sun_params.x, sun_params.y, ang);
        sky = lerp(sky, sun_color.xyz, k);
    }
    return sky;
}

float4 PSMain(VSOut v) : SV_TARGET {
    float3 dir = normalize(CubeFaceDir(v.uv, face_pad.x));
    return float4(ProcSky(dir), 1.0);
}
)";

struct EnvCaptureCBLayout {
    i32 face_index;
    i32 pad0;
    i32 pad1;
    i32 pad2;
    Vec4 sun_dir;
    Vec4 sun_color;
    Vec4 sun_params;
    Vec4 zenith;
    Vec4 horizon;
    Vec4 ground;
};

// ---- Skybox preview (env / irradiance / prefilter cubemap を fullscreen 描画) ----
const char* kSkyboxHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer Skybox : register(b0) {
    float4x4 inv_view_proj;
    float4   eye;            // xyz=camera world pos
    float4   mip_pad;        // x=mip level (prefilter で 0..4 を切替、env/irradiance は 0)
};

TextureCube env : register(t0);
SamplerState env_sampler : register(s0);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 ndc : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.ndc = uv * 2.0 - 1.0;
    // 遠平面 (z=1) で描画。Sky.cpp と同じく pos.y は -ndc.y で D3D の上下を統一
    o.pos = float4(o.ndc.x, -o.ndc.y, 1.0, 1.0);
    return o;
}

float4 PSMain(VSOut v) : SV_TARGET {
    // 遠平面の NDC からワールド位置を逆変換し、カメラからの方向を取る
    float4 wp = mul(float4(v.ndc.x, -v.ndc.y, 1.0, 1.0), inv_view_proj);
    wp.xyz /= wp.w;
    float3 dir = normalize(wp.xyz - eye.xyz);
    return float4(env.SampleLevel(env_sampler, dir, mip_pad.x).rgb, 1.0);
}
)";

struct SkyboxCBLayout {
    Mat4 inv_view_proj;
    Vec4 eye;
    Vec4 mip_pad;      // x=mip level
};

// ---- Diffuse irradiance 生成 (env cubemap の半球積分) ----
//
// 各 face 各 texel の方向 N について:
//   E(N) = ∫_Ω L_env(ω) (N·ω) dω
//   sphere coords: dω = sinθ dθ dφ
//   E(N) ≈ ΔθΔφ Σ L(θᵢ,φⱼ) cosθᵢ sinθᵢ
//
// Lambert diffuse の ambient 反射光 (radiance):
//   L_diffuse = (albedo/π) E(N)
// → ここでは (E(N)/π) を cubemap に焼き、PbrShader 側で `albedo * irradiance.Sample(N)`
//   と素直に乗算できる形にする。
//
// kNumPhi × kNumTheta = 64 × 16 = 1024 サンプル / texel。32x32x6 = 6144 texel × 1024 ≈ 6.3M
// TextureCube サンプルだが、初期化時 1 回切りなので問題ない。
const char* kIrradianceHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer Irradiance : register(b0) {
    int4 face_pad;       // x=face index 0..5
};

TextureCube env : register(t0);
SamplerState env_sampler : register(s0);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.uv  = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0), 0.0, 1.0);
    return o;
}

float3 CubeFaceDir(float2 uv01, int face) {
    float2 m = uv01 * 2.0 - 1.0;
    if (face == 0) return float3( 1.0, -m.y, -m.x);
    if (face == 1) return float3(-1.0, -m.y,  m.x);
    if (face == 2) return float3( m.x,  1.0,  m.y);
    if (face == 3) return float3( m.x, -1.0, -m.y);
    if (face == 4) return float3( m.x, -m.y,  1.0);
    return                 float3(-m.x, -m.y, -1.0);
}

static const float PI       = 3.14159265358979;
static const float TWO_PI   = 6.28318530717958;
static const float HALF_PI  = 1.57079632679489;
static const uint  kNumPhi   = 64u;
static const uint  kNumTheta = 16u;

float3 IntegrateDiffuse(float3 N) {
    // tangent frame (N が world up に近いときは右 seed を変える)
    float3 up_seed = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 right   = normalize(cross(up_seed, N));
    float3 up      = cross(N, right);

    float3 irr   = float3(0.0, 0.0, 0.0);
    float  dPhi   = TWO_PI / float(kNumPhi);
    float  dTheta = HALF_PI / float(kNumTheta);
    [loop]
    for (uint p = 0u; p < kNumPhi; ++p) {
        float phi  = float(p) * dPhi;
        float cphi = cos(phi);
        float sphi = sin(phi);
        [loop]
        for (uint t = 0u; t < kNumTheta; ++t) {
            // pixel-center sampling で θ=0 退化と θ=π/2 端点バイアスを避ける
            float theta = (float(t) + 0.5) * dTheta;
            float sinT  = sin(theta);
            float cosT  = cos(theta);
            float3 ts   = float3(sinT * cphi, sinT * sphi, cosT);
            float3 ws   = ts.x * right + ts.y * up + ts.z * N;
            irr += env.SampleLevel(env_sampler, ws, 0).rgb * cosT * sinT;
        }
    }
    // (1/π) E(N) = (dPhi dTheta / π) Σ L cosT sinT
    return (dPhi * dTheta / PI) * irr;
}

float4 PSMain(VSOut v) : SV_TARGET {
    float3 N = normalize(CubeFaceDir(v.uv, face_pad.x));
    return float4(IntegrateDiffuse(N), 1.0);
}
)";

struct IrradianceCBLayout {
    i32 face_index;
    i32 pad0;
    i32 pad1;
    i32 pad2;
};

// ---- Specular prefilter 生成 (GGX importance sampling, mip = roughness) ----
//
// 各 mip 各 face 各 texel の方向 N について:
//   prefilter(N, r) = Σ L_env(L_i) * (N·L_i) / Σ (N·L_i)
// L_i は GGX importance-sampled half-vector H から reflect(-V, H) として得る。
// Karis の "V = N" 近似で V 依存を取り除いている (split-sum の片側、もう片方が BRDF LUT)。
//
// mip 0 = roughness 0 (鏡面、env をそのままコピー)
// mip 4 = roughness 1 (極限ぼかし)
const char* kPrefilterHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer Prefilter : register(b0) {
    int4   face_pad;          // x = face 0..5
    float4 rough_pad;         // x = roughness 0..1
};

TextureCube env : register(t0);
SamplerState env_sampler : register(s0);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.uv  = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0), 0.0, 1.0);
    return o;
}

float3 CubeFaceDir(float2 uv01, int face) {
    float2 m = uv01 * 2.0 - 1.0;
    if (face == 0) return float3( 1.0, -m.y, -m.x);
    if (face == 1) return float3(-1.0, -m.y,  m.x);
    if (face == 2) return float3( m.x,  1.0,  m.y);
    if (face == 3) return float3( m.x, -1.0, -m.y);
    if (face == 4) return float3( m.x, -m.y,  1.0);
    return                 float3(-m.x, -m.y, -1.0);
}

static const float PI = 3.14159265358979;

float radicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
float2 Hammersley(uint i, uint n) {
    return float2(float(i) / float(n), radicalInverseVdC(i));
}

float3 ImportanceSampleGGX(float2 xi, float3 N, float roughness) {
    float a = roughness * roughness;
    float phi      = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float3 H_t = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    // tangent → world
    float3 up_seed = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 right = normalize(cross(up_seed, N));
    float3 up    = cross(N, right);
    return normalize(H_t.x * right + H_t.y * up + H_t.z * N);
}

float3 PrefilterEnvMap(float3 N, float roughness) {
    float3 V = N;        // Karis 近似 (split-sum)
    float3 sum = float3(0.0, 0.0, 0.0);
    float  total_weight = 0.0;
    const uint kSamples = 1024u;
    [loop]
    for (uint i = 0u; i < kSamples; ++i) {
        float2 xi = Hammersley(i, kSamples);
        float3 H  = ImportanceSampleGGX(xi, N, roughness);
        float3 L  = 2.0 * dot(V, H) * H - V;
        float  NoL = saturate(dot(N, L));
        if (NoL > 0.0) {
            sum += env.SampleLevel(env_sampler, L, 0).rgb * NoL;
            total_weight += NoL;
        }
    }
    return sum / max(total_weight, 1e-6);
}

float4 PSMain(VSOut v) : SV_TARGET {
    float3 N = normalize(CubeFaceDir(v.uv, face_pad.x));
    float  r = saturate(rough_pad.x);
    if (r < 1.0e-3) {
        // mip 0 = 鏡面: env をそのままコピー (importance sampling は退化する)
        return float4(env.SampleLevel(env_sampler, N, 0).rgb, 1.0);
    }
    return float4(PrefilterEnvMap(N, r), 1.0);
}
)";

struct PrefilterCBLayout {
    i32 face_index;
    i32 pad0;
    i32 pad1;
    i32 pad2;
    f32 roughness;
    f32 pad3;
    f32 pad4;
    f32 pad5;
};

} // namespace

Result<void> ImageBasedLighting::EnsureBrdfLut(IRhiDevice& device,
                                                IRhiCommandList& cl) noexcept {
    if (_brdf_built) return Ok();
    auto r = BuildBrdfLut(device, cl);
    if (r.IsErr()) return r;
    _brdf_built = true;
    return Ok();
}

Result<void> ImageBasedLighting::BuildBrdfLut(IRhiDevice& device,
                                               IRhiCommandList& cl) noexcept {
    // Dx12 raw backend では何もしない (BeginRenderToTexture が空、Pipeline cast 不能)。
    // Ibl.h で「Diligent 専用」と謳っているが運用上の事故防止のため early-return。
    if (!IsDiligentBackend(device)) {
        ACS_LOG_WARN("ImageBasedLighting: BRDF LUT skipped (backend != Diligent)");
        return Ok();
    }

    // 1) RT 用テクスチャ
    TextureDesc td{};
    td.width            = kBrdfLutSize;
    td.height           = kBrdfLutSize;
    td.format           = Format::R16G16_Float;
    td.is_render_target = true;
    auto t_r = CreateRhiTexture(device, td);
    if (t_r.IsErr()) return Err<void>(t_r.Error());
    _brdf_lut = Move(t_r.Value());

    // 2) 一時 VS/PS/Pipeline (LUT は 1 回描画後は静的データなのでパイプラインは捨てる)
    UniquePtr<IRhiShader>   vs;
    UniquePtr<IRhiShader>   ps;
    UniquePtr<IRhiPipeline> pipeline;

    ShaderDesc vs_d{};
    vs_d.stage = ShaderStage::Vertex;
    vs_d.hlsl_source = kBrdfLutHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "IblBrdfLut.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr())
        return Err<void>(r.Error());
    else vs = Move(r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = ShaderStage::Pixel;
    ps_d.hlsl_source = kBrdfLutHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "IblBrdfLut.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr())
        return Err<void>(r.Error());
    else ps = Move(r.Value());

    PipelineDesc pd{};
    pd.vs            = vs.Get();
    pd.ps            = ps.Get();
    pd.topology      = PrimitiveTopology::TriangleList;
    pd.rt_format     = Format::R16G16_Float;
    pd.depth_format  = Format::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = CullMode::None;
    pd.blend_mode    = BlendMode::Opaque;
    pd.cbuffer_slots = 0;
    pd.texture_slots = 0;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr())
        return Err<void>(r.Error());
    else pipeline = Move(r.Value());

    // 3) RT に 1 パス描画
    ClearColor black{0, 0, 0, 1};
    cl.BeginRenderToTexture(*_brdf_lut, black);
    cl.SetPipeline(*pipeline);
    cl.Draw(3);
    cl.EndRenderToTexture(*_brdf_lut);

    // pipeline / vs / ps はここで解放されるが、_brdf_lut は残る。
    return Ok();
}

Result<void> ImageBasedLighting::EnsureEnvCubemap(IRhiDevice& device,
                                                  IRhiCommandList& cl,
                                                  const Sky& sky) noexcept {
    if (_env_built) return Ok();
    auto r = BuildEnvCubemap(device, cl, sky);
    if (r.IsErr()) return r;
    _env_built = true;
    return Ok();
}

Result<void> ImageBasedLighting::BuildEnvCubemap(IRhiDevice& device,
                                                  IRhiCommandList& cl,
                                                  const Sky& sky) noexcept {
    if (!IsDiligentBackend(device)) {
        ACS_LOG_WARN("ImageBasedLighting: env cubemap skipped (backend != Diligent)");
        return Ok();
    }

    // 1) cubemap (6 face, R11G11B10_Float, per-slice RTV)
    TextureDesc td{};
    td.width            = kEnvCubeSize;
    td.height           = kEnvCubeSize;
    td.format           = Format::R11G11B10_Float;
    td.array_size       = 6;
    td.is_cubemap       = true;
    td.is_render_target = true;
    td.per_slice_rtv    = true;
    auto t_r = CreateRhiTexture(device, td);
    if (t_r.IsErr()) return Err<void>(t_r.Error());
    _env_cube = Move(t_r.Value());

    // 2) 一時 VS/PS/Pipeline/CB
    UniquePtr<IRhiShader>   vs;
    UniquePtr<IRhiShader>   ps;
    UniquePtr<IRhiPipeline> pipeline;
    UniquePtr<IRhiBuffer>   cb;

    ShaderDesc vs_d{};
    vs_d.stage = ShaderStage::Vertex;
    vs_d.hlsl_source = kEnvCaptureHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "IblEnvCapture.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else vs = Move(r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = ShaderStage::Pixel;
    ps_d.hlsl_source = kEnvCaptureHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "IblEnvCapture.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else ps = Move(r.Value());

    BufferDesc cbd{};
    cbd.size = sizeof(EnvCaptureCBLayout);
    // 256B align (DX12 CB 制約)
    cbd.size = (cbd.size + 255u) & ~static_cast<usize>(255u);
    cbd.usage = BufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else cb = Move(r.Value());

    PipelineDesc pd{};
    pd.vs            = vs.Get();
    pd.ps            = ps.Get();
    pd.topology      = PrimitiveTopology::TriangleList;
    pd.rt_format     = Format::R11G11B10_Float;
    pd.depth_format  = Format::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = CullMode::None;
    pd.blend_mode    = BlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 0;
    pd.cbuffer_names[0] = "EnvCapture";
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else pipeline = Move(r.Value());

    // 3) 6 face を順に塗る
    ClearColor black{0, 0, 0, 1};
    const Vec3 sd = sky.SunDirection();
    const Vec3 sc = sky.SunColor();
    const Vec3 zn = sky.ZenithColor();
    const Vec3 hz = sky.HorizonColor();
    const Vec3 gr = sky.GroundColor();

    cl.SetPipeline(*pipeline);
    for (u32 face = 0; face < 6; ++face) {
        EnvCaptureCBLayout data{};
        data.face_index = static_cast<i32>(face);
        data.sun_dir    = Vec4{sd.x, sd.y, sd.z, 0};
        data.sun_color  = Vec4{sc.x, sc.y, sc.z, 1};
        data.sun_params = Vec4{sky.SunRadius(), sky.SunGlow(), 0, 0};
        data.zenith     = Vec4{zn.x, zn.y, zn.z, 1};
        data.horizon    = Vec4{hz.x, hz.y, hz.z, 1};
        data.ground     = Vec4{gr.x, gr.y, gr.z, 1};
        cb->Update(&data, sizeof(data));

        cl.BeginRenderToTextureSlice(*_env_cube, face, 0, black);
        cl.SetPipeline(*pipeline);          // BeginRenderToTextureSlice 後の re-bind 保険
        cl.SetConstantBuffer(0, *cb);
        cl.Draw(3);
    }
    // 最後の slice 描画後 main pass RT に戻すため EndRenderToTexture を呼ぶ。
    // BeginRenderToTextureSlice は EndRenderToTexture の RT 復帰ロジックと
    // 共用する設計なので、何かしらの "終わり" 通知が必要。
    cl.EndRenderToTexture(*_env_cube);

    return Ok();
}

Result<void> ImageBasedLighting::EnsureIrradiance(IRhiDevice& device,
                                                   IRhiCommandList& cl) noexcept {
    if (_irradiance_built) return Ok();
    if (!_env_cube) {
        return ACS_ERR(Render, 160,
            "ImageBasedLighting::EnsureIrradiance: env cubemap not built yet");
    }
    auto r = BuildIrradiance(device, cl);
    if (r.IsErr()) return r;
    _irradiance_built = true;
    return Ok();
}

Result<void> ImageBasedLighting::BuildIrradiance(IRhiDevice& device,
                                                  IRhiCommandList& cl) noexcept {
    if (!IsDiligentBackend(device)) {
        ACS_LOG_WARN("ImageBasedLighting: irradiance skipped (backend != Diligent)");
        return Ok();
    }

    // 1) irradiance cubemap (6 face, 32x32, R11G11B10_Float, per-slice RTV)
    TextureDesc td{};
    td.width            = kIrradianceSize;
    td.height           = kIrradianceSize;
    td.format           = Format::R11G11B10_Float;
    td.array_size       = 6;
    td.is_cubemap       = true;
    td.is_render_target = true;
    td.per_slice_rtv    = true;
    auto t_r = CreateRhiTexture(device, td);
    if (t_r.IsErr()) return Err<void>(t_r.Error());
    _irradiance_cube = Move(t_r.Value());

    // 2) 一時 VS/PS/Pipeline/CB
    UniquePtr<IRhiShader>   vs;
    UniquePtr<IRhiShader>   ps;
    UniquePtr<IRhiPipeline> pipeline;
    UniquePtr<IRhiBuffer>   cb;

    ShaderDesc vs_d{};
    vs_d.stage = ShaderStage::Vertex;
    vs_d.hlsl_source = kIrradianceHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "IblIrradiance.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else vs = Move(r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = ShaderStage::Pixel;
    ps_d.hlsl_source = kIrradianceHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "IblIrradiance.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else ps = Move(r.Value());

    BufferDesc cbd{};
    cbd.size = (sizeof(IrradianceCBLayout) + 255u) & ~static_cast<usize>(255u);
    cbd.usage = BufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else cb = Move(r.Value());

    PipelineDesc pd{};
    pd.vs            = vs.Get();
    pd.ps            = ps.Get();
    pd.topology      = PrimitiveTopology::TriangleList;
    pd.rt_format     = Format::R11G11B10_Float;
    pd.depth_format  = Format::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = CullMode::None;
    pd.blend_mode    = BlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 1;
    pd.cbuffer_names[0] = "Irradiance";
    pd.texture_names[0] = "env";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = SamplerFilter::Linear;
    pd.static_samplers[0].address_u = SamplerAddress::Clamp;
    pd.static_samplers[0].address_v = SamplerAddress::Clamp;
    pd.static_samplers[0].address_w = SamplerAddress::Clamp;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else pipeline = Move(r.Value());

    // 3) 6 face を順に積分
    // SetConstantBuffer / SetTexture は Diligent SRB 上に永続 bind されるので
    // ループ外で 1 回呼ぶだけで OK (Update は GPU command stream に sequential
    // に挿入され、各 Draw は直前の Update 内容を見る)。
    ClearColor black{0, 0, 0, 1};
    cl.SetPipeline(*pipeline);
    cl.SetConstantBuffer(0, *cb);
    cl.SetTexture(0, *_env_cube);
    for (u32 face = 0; face < 6; ++face) {
        IrradianceCBLayout data{};
        data.face_index = static_cast<i32>(face);
        cb->Update(&data, sizeof(data));

        cl.BeginRenderToTextureSlice(*_irradiance_cube, face, 0, black);
        cl.Draw(3);
    }
    cl.EndRenderToTexture(*_irradiance_cube);
    return Ok();
}

Result<void> ImageBasedLighting::EnsurePrefilter(IRhiDevice& device,
                                                  IRhiCommandList& cl) noexcept {
    if (_prefilter_built) return Ok();
    if (!_env_cube) {
        return ACS_ERR(Render, 161,
            "ImageBasedLighting::EnsurePrefilter: env cubemap not built yet");
    }
    auto r = BuildPrefilter(device, cl);
    if (r.IsErr()) return r;
    _prefilter_built = true;
    return Ok();
}

Result<void> ImageBasedLighting::BuildPrefilter(IRhiDevice& device,
                                                 IRhiCommandList& cl) noexcept {
    if (!IsDiligentBackend(device)) {
        ACS_LOG_WARN("ImageBasedLighting: prefilter skipped (backend != Diligent)");
        return Ok();
    }

    // 1) prefilter cubemap (6 face, 128x128, 5 mips, R11G11B10_Float, per-slice RTV)
    TextureDesc td{};
    td.width            = kPrefilterSize;
    td.height           = kPrefilterSize;
    td.format           = Format::R11G11B10_Float;
    td.array_size       = 6;
    td.is_cubemap       = true;
    td.mip_levels       = kPrefilterMips;
    td.is_render_target = true;
    td.per_slice_rtv    = true;
    auto t_r = CreateRhiTexture(device, td);
    if (t_r.IsErr()) return Err<void>(t_r.Error());
    _prefilter_cube  = Move(t_r.Value());
    _prefilter_mips  = kPrefilterMips;

    // 2) 一時 VS/PS/Pipeline/CB
    UniquePtr<IRhiShader>   vs;
    UniquePtr<IRhiShader>   ps;
    UniquePtr<IRhiPipeline> pipeline;
    UniquePtr<IRhiBuffer>   cb;

    ShaderDesc vs_d{};
    vs_d.stage = ShaderStage::Vertex;
    vs_d.hlsl_source = kPrefilterHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "IblPrefilter.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else vs = Move(r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = ShaderStage::Pixel;
    ps_d.hlsl_source = kPrefilterHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "IblPrefilter.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else ps = Move(r.Value());

    BufferDesc cbd{};
    cbd.size = (sizeof(PrefilterCBLayout) + 255u) & ~static_cast<usize>(255u);
    cbd.usage = BufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else cb = Move(r.Value());

    PipelineDesc pd{};
    pd.vs            = vs.Get();
    pd.ps            = ps.Get();
    pd.topology      = PrimitiveTopology::TriangleList;
    pd.rt_format     = Format::R11G11B10_Float;
    pd.depth_format  = Format::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = CullMode::None;
    pd.blend_mode    = BlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 1;
    pd.cbuffer_names[0] = "Prefilter";
    pd.texture_names[0] = "env";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = SamplerFilter::Linear;
    pd.static_samplers[0].address_u = SamplerAddress::Clamp;
    pd.static_samplers[0].address_v = SamplerAddress::Clamp;
    pd.static_samplers[0].address_w = SamplerAddress::Clamp;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else pipeline = Move(r.Value());

    // 3) 5 mip × 6 face = 30 render
    ClearColor black{0, 0, 0, 1};
    cl.SetPipeline(*pipeline);
    cl.SetConstantBuffer(0, *cb);
    cl.SetTexture(0, *_env_cube);
    for (u32 mip = 0; mip < kPrefilterMips; ++mip) {
        const f32 roughness = static_cast<f32>(mip) / static_cast<f32>(kPrefilterMips - 1);
        for (u32 face = 0; face < 6; ++face) {
            PrefilterCBLayout data{};
            data.face_index = static_cast<i32>(face);
            data.roughness  = roughness;
            cb->Update(&data, sizeof(data));

            cl.BeginRenderToTextureSlice(*_prefilter_cube, face, mip, black);
            cl.Draw(3);
        }
    }
    cl.EndRenderToTexture(*_prefilter_cube);
    return Ok();
}

Result<void> ImageBasedLighting::EnsureSkyboxPipeline(IRhiDevice& device,
                                                       Format rt_format,
                                                       Format depth_format) noexcept {
    if (_sky_pipeline && _sky_rt_format == rt_format && _sky_depth_format == depth_format) {
        return Ok();
    }
    // RT/depth format が変わったら再構築
    _sky_pipeline.Reset();
    _sky_cb.Reset();
    _sky_ps.Reset();
    _sky_vs.Reset();

    ShaderDesc vs_d{};
    vs_d.stage = ShaderStage::Vertex;
    vs_d.hlsl_source = kSkyboxHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "IblSkybox.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else _sky_vs = Move(r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = ShaderStage::Pixel;
    ps_d.hlsl_source = kSkyboxHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "IblSkybox.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else _sky_ps = Move(r.Value());

    BufferDesc cbd{};
    cbd.size = (sizeof(SkyboxCBLayout) + 255u) & ~static_cast<usize>(255u);
    cbd.usage = BufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else _sky_cb = Move(r.Value());

    PipelineDesc pd{};
    pd.vs            = _sky_vs.Get();
    pd.ps            = _sky_ps.Get();
    pd.topology      = PrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = depth_format;
    pd.depth_test    = false;     // 背景は最遠なので深度テスト不要
    pd.depth_write   = false;
    pd.cull_mode     = CullMode::None;
    pd.blend_mode    = BlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 1;
    pd.cbuffer_names[0] = "Skybox";
    pd.texture_names[0] = "env";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = SamplerFilter::Linear;
    pd.static_samplers[0].address_u = SamplerAddress::Clamp;
    pd.static_samplers[0].address_v = SamplerAddress::Clamp;
    pd.static_samplers[0].address_w = SamplerAddress::Clamp;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else _sky_pipeline = Move(r.Value());

    _sky_rt_format    = rt_format;
    _sky_depth_format = depth_format;
    return Ok();
}

void ImageBasedLighting::DrawSkybox(IRhiDevice& device, IRhiCommandList& cl,
                                     IRhiTexture& cube,
                                     const Mat4& view_proj, Vec3 eye,
                                     Format rt_format, Format depth_format,
                                     f32 mip_level) noexcept {
    if (!IsDiligentBackend(device)) return;
    if (auto r = EnsureSkyboxPipeline(device, rt_format, depth_format); r.IsErr()) return;

    SkyboxCBLayout cb{};
    cb.inv_view_proj = Inverse(view_proj);
    cb.eye           = Vec4{eye.x, eye.y, eye.z, 1};
    cb.mip_pad       = Vec4{mip_level, 0, 0, 0};
    _sky_cb->Update(&cb, sizeof(cb));

    cl.SetPipeline(*_sky_pipeline);
    cl.SetConstantBuffer(0, *_sky_cb);
    cl.SetTexture(0, cube);
    cl.Draw(3);
}

void ImageBasedLighting::DrawEnvSkybox(IRhiDevice& device, IRhiCommandList& cl,
                                        const Mat4& view_proj, Vec3 eye,
                                        Format rt_format, Format depth_format) noexcept {
    if (!_env_cube) return;
    DrawSkybox(device, cl, *_env_cube, view_proj, eye, rt_format, depth_format);
}

void ImageBasedLighting::ResetEnvCubemap() noexcept {
    // env が無効になれば irradiance / prefilter も無効。
    _prefilter_cube.Reset();
    _prefilter_mips   = 0;
    _prefilter_built  = false;
    _irradiance_cube.Reset();
    _irradiance_built = false;
    _env_cube.Reset();
    _env_built        = false;
}

void ImageBasedLighting::Shutdown() noexcept {
    _sky_pipeline.Reset();
    _sky_cb.Reset();
    _sky_ps.Reset();
    _sky_vs.Reset();
    _sky_rt_format    = Format::Unknown;
    _sky_depth_format = Format::Unknown;
    _prefilter_cube.Reset();
    _prefilter_mips   = 0;
    _irradiance_cube.Reset();
    _env_cube.Reset();
    _brdf_lut.Reset();
    _prefilter_built  = false;
    _irradiance_built = false;
    _env_built        = false;
    _brdf_built       = false;
}

} // namespace acs
