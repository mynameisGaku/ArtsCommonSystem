// SPDX-License-Identifier: Apache-2.0
// Image-Based Lighting 実装 (Phase 31)
//
// 現段階の機能: BRDF LUT 生成、FSky → env cubemap キャプチャ、skybox preview 描画。
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
// **テクスチャ座標規約 (FPbrShader IBL 統合時に必ず一致させること)**:
//   ・row 0  (texture top, v=0) = roughness 0  (鏡面)
//   ・row 255 (texture bottom, v=1) = roughness 1 (粗い表面)
//   ・col 0  (texture left,  u=0) = NdotV 0  (grazing 角)
//   ・col 255 (texture right, u=1) = NdotV 1  (正対)
// → FPbrShader 側で `brdf_lut.Sample(s, float2(NdotV, roughness))` で sampling 可能。
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

// ---- 環境 cubemap キャプチャ (FSky procedural を 6 face に焼く) ----
//
// FSky.cpp の手続き式 (高さ角でグラデ + 太陽 disc) と同じ式を per-face で評価する。
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

// 標準 D3D11/12 cubemap face mapping (TextureCube.Sample で direct に使える形式)。
// uv.y=0 が face row 0 (= 後で TextureCube.Sample が返す top) になる前提。
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
    FVec4 sun_dir;
    FVec4 sun_color;
    FVec4 sun_params;
    FVec4 zenith;
    FVec4 horizon;
    FVec4 ground;
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
    // 遠平面 (z=1) で描画。FSky.cpp と同じく pos.y は -ndc.y で D3D の上下を統一
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
    FMat4 inv_view_proj;
    FVec4 eye;
    FVec4 mip_pad;      // x=mip level
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
// → ここでは (E(N)/π) を cubemap に焼き、FPbrShader 側で `albedo * irradiance.Sample(N)`
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

// ---- Equirectangular HDR → cubemap 変換 ----
//
// 各 cubemap face 各 texel の world direction N を球面座標 (phi, theta) に変換し、
// equirect texture から sample。phi = atan2(N.x, N.z)、theta = acos(N.y)。
//
// equirect 規約: u = phi/(2π) + 0.5 (0=後方, 0.25=右, 0.5=前方, 0.75=左)、v = theta/π
//   (0 = +Y zenith, 1 = -Y nadir)。これは Polyhaven / sIBL Archive 等の HDR と一致。
const char* kEquirectToCubeHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer EqToCube : register(b0) {
    int4 face_pad;
};

Texture2D    equirect          : register(t0);
SamplerState equirect_sampler  : register(s0);

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

static const float PI     = 3.14159265358979;
static const float TWO_PI = 6.28318530717958;

float4 PSMain(VSOut v) : SV_TARGET {
    float3 N = normalize(CubeFaceDir(v.uv, face_pad.x));
    // equirect 座標 (sIBL Archive 規約)
    float phi   = atan2(N.x, N.z);            // [-π, π]
    float theta = acos(clamp(N.y, -1.0, 1.0));// [0, π]
    float2 uv;
    uv.x = phi / TWO_PI + 0.5;                 // [0, 1]
    uv.y = theta / PI;                         // [0, 1], v=0 が +Y
    float3 c = equirect.SampleLevel(equirect_sampler, uv, 0).rgb;
    return float4(c, 1.0);
}
)";

struct EquirectCBLayout {
    i32 face_index;
    i32 pad0;
    i32 pad1;
    i32 pad2;
};

} // namespace

TResult<void> ImageBasedLighting::EnsureBrdfLut(IRhiDevice& device,
                                                IRhiCommandList& cl) noexcept {
    if (m_bBrdfBuilt) return Ok();
    auto r = BuildBrdfLut(device, cl);
    if (r.IsErr()) return r;
    m_bBrdfBuilt = true;
    return Ok();
}

TResult<void> ImageBasedLighting::BuildBrdfLut(IRhiDevice& device,
                                               IRhiCommandList& cl) noexcept {
    // Dx12 raw backend には高度3D (cubemap / per-slice RTV) が無いため本実装不能。
    // fake-success せず正直に capability error を返す (Ibl.h の境界を参照)。
    if (!IsDiligentBackend(device)) {
        static bool s_warned = false;
        if (!s_warned) {
            ACS_LOG_WARN("ImageBasedLighting: BRDF LUT requires the Diligent backend "
                         "(rebuild with -DACS_RENDER_DILIGENT=ON); skipped");
            s_warned = true;
        }
        return ACS_ERR(Render, 88, "BRDF LUT requires the Diligent backend (-DACS_RENDER_DILIGENT=ON)");
    }
    // 前回失敗で残った半端テクスチャは破棄。途中失敗時にも次回呼び出しで
    // 確実に rebuild されるよう、入口で全 reset しておく。
    m_BrdfLut.Reset();

    // 1) RT 用テクスチャ
    FTextureDesc td{};
    td.width            = kBrdfLutSize;
    td.height           = kBrdfLutSize;
    td.format           = EFormat::R16G16_Float;
    td.is_render_target = true;
    auto t_r = CreateRhiTexture(device, td);
    if (t_r.IsErr()) return Err<void>(t_r.Error());
    m_BrdfLut = Move(t_r.Value());

    // 2) 一時 VS/PS/Pipeline (LUT は 1 回描画後は静的データなのでパイプラインは捨てる)
    TUniquePtr<IRhiShader>   vs;
    TUniquePtr<IRhiShader>   ps;
    TUniquePtr<IRhiPipeline> pipeline;

    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kBrdfLutHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "IblBrdfLut.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr())
        return Err<void>(r.Error());
    else vs = Move(r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kBrdfLutHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "IblBrdfLut.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr())
        return Err<void>(r.Error());
    else ps = Move(r.Value());

    FPipelineDesc pd{};
    pd.vs            = vs.Get();
    pd.ps            = ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = EFormat::R16G16_Float;
    pd.depth_format  = EFormat::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 0;
    pd.texture_slots = 0;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr())
        return Err<void>(r.Error());
    else pipeline = Move(r.Value());

    // 3) RT に 1 パス描画
    ClearColor black{0, 0, 0, 1};
    cl.BeginRenderToTexture(*m_BrdfLut, black);
    cl.SetPipeline(*pipeline);
    cl.Draw(3);
    cl.EndRenderToTexture(*m_BrdfLut);

    // pipeline / vs / ps はここで解放されるが、m_BrdfLut は残る。
    return Ok();
}

TResult<void> ImageBasedLighting::EnsureEnvCubemap(IRhiDevice& device,
                                                  IRhiCommandList& cl,
                                                  const FSky& sky) noexcept {
    if (m_bEnvBuilt) return Ok();
    auto r = BuildEnvCubemap(device, cl, sky);
    if (r.IsErr()) return r;
    m_bEnvBuilt = true;
    return Ok();
}

TResult<void> ImageBasedLighting::BuildEnvCubemap(IRhiDevice& device,
                                                  IRhiCommandList& cl,
                                                  const FSky& sky) noexcept {
    if (!IsDiligentBackend(device)) {
        static bool s_warned = false;
        if (!s_warned) {
            ACS_LOG_WARN("ImageBasedLighting: env cubemap requires the Diligent backend "
                         "(rebuild with -DACS_RENDER_DILIGENT=ON); skipped");
            s_warned = true;
        }
        return ACS_ERR(Render, 88, "env cubemap requires the Diligent backend (-DACS_RENDER_DILIGENT=ON)");
    }
    m_EnvCube.Reset();

    // 1) cubemap (6 face, R11G11B10_Float, per-slice RTV)
    FTextureDesc td{};
    td.width            = kEnvCubeSize;
    td.height           = kEnvCubeSize;
    td.format           = EFormat::R11G11B10_Float;
    td.array_size       = 6;
    td.is_cubemap       = true;
    td.is_render_target = true;
    td.per_slice_rtv    = true;
    auto t_r = CreateRhiTexture(device, td);
    if (t_r.IsErr()) return Err<void>(t_r.Error());
    m_EnvCube = Move(t_r.Value());

    // 2) 一時 VS/PS/Pipeline/CB
    TUniquePtr<IRhiShader>   vs;
    TUniquePtr<IRhiShader>   ps;
    TUniquePtr<IRhiPipeline> pipeline;
    TUniquePtr<IRhiBuffer>   cb;

    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kEnvCaptureHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "IblEnvCapture.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else vs = Move(r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kEnvCaptureHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "IblEnvCapture.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else ps = Move(r.Value());

    FBufferDesc cbd{};
    cbd.size = sizeof(EnvCaptureCBLayout);
    // 256B align (DX12 CB 制約)
    cbd.size = (cbd.size + 255u) & ~static_cast<usize>(255u);
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else cb = Move(r.Value());

    FPipelineDesc pd{};
    pd.vs            = vs.Get();
    pd.ps            = ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = EFormat::R11G11B10_Float;
    pd.depth_format  = EFormat::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 0;
    pd.cbuffer_names[0] = "EnvCapture";
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else pipeline = Move(r.Value());

    // 3) 6 face を順に塗る
    ClearColor black{0, 0, 0, 1};
    const FVec3 sd = sky.SunDirection();
    const FVec3 sc = sky.SunColor();
    const FVec3 zn = sky.ZenithColor();
    const FVec3 hz = sky.HorizonColor();
    const FVec3 gr = sky.GroundColor();

    cl.SetPipeline(*pipeline);
    for (u32 face = 0; face < 6; ++face) {
        EnvCaptureCBLayout data{};
        data.face_index = static_cast<i32>(face);
        data.sun_dir    = FVec4{sd.x, sd.y, sd.z, 0};
        data.sun_color  = FVec4{sc.x, sc.y, sc.z, 1};
        data.sun_params = FVec4{sky.SunRadius(), sky.SunGlow(), 0, 0};
        data.zenith     = FVec4{zn.x, zn.y, zn.z, 1};
        data.horizon    = FVec4{hz.x, hz.y, hz.z, 1};
        data.ground     = FVec4{gr.x, gr.y, gr.z, 1};
        cb->Update(&data, sizeof(data));

        cl.BeginRenderToTextureSlice(*m_EnvCube, face, 0, black);
        cl.SetPipeline(*pipeline);          // BeginRenderToTextureSlice 後の re-bind 保険
        cl.SetConstantBuffer(0, *cb);
        cl.Draw(3);
    }
    // 最後の slice 描画後 main pass RT に戻すため EndRenderToTexture を呼ぶ。
    // BeginRenderToTextureSlice は EndRenderToTexture の RT 復帰ロジックと
    // 共用する設計なので、何かしらの "終わり" 通知が必要。
    cl.EndRenderToTexture(*m_EnvCube);

    return Ok();
}

void ImageBasedLighting::ComputeSh9FromEquirect(const f32* rgba_float,
                                                  u32 width, u32 height,
                                                  FVec4 out_sh_rgb[9]) noexcept {
    // 初期化
    for (u32 i = 0; i < 9; ++i) out_sh_rgb[i] = FVec4{0, 0, 0, 0};
    if (!rgba_float || width == 0 || height == 0) return;

    // SH basis 定数 (Ramamoorthi-Hanrahan 2001、Stupid SH Tricks の表記)
    // c0 = 1/2 * sqrt(1/π)        (l=0)
    // c1 = sqrt(3/(4π)) * (x,y,z)  (l=1)
    // c2 (l=2): xy / xz / yz / (3z²-1) / (x²-y²) 系
    const f32 inv_2sqrtPi   = 0.282094791773878f;     // 1/(2√π)
    const f32 sqrt3_2sqrtPi = 0.488602511902920f;     // √3/(2√π)
    const f32 sqrt15_2sqrtPi = 1.092548430592079f;    // √15/(2√π)
    const f32 sqrt5_4sqrtPi  = 0.315391565252520f;    // √5/(4√π)
    const f32 sqrt15_4sqrtPi = 0.546274215296039f;    // √15/(4√π)

    const f32 dPhi   = 2.0f * kPi / static_cast<f32>(width);
    const f32 dTheta = kPi / static_cast<f32>(height);

    f64 sh_r[9] = {0,0,0,0,0,0,0,0,0};
    f64 sh_g[9] = {0,0,0,0,0,0,0,0,0};
    f64 sh_b[9] = {0,0,0,0,0,0,0,0,0};

    for (u32 y = 0; y < height; ++y) {
        const f32 theta = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height) * kPi;
        const f32 sinT  = Sin(theta);
        const f32 cosT  = Cos(theta);
        const f32 weight = sinT * dPhi * dTheta;   // 球面要素 dω = sinθ dθ dφ
        for (u32 x = 0; x < width; ++x) {
            const f32 phi = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width)
                            * 2.0f * kPi - kPi;
            const f32 sinP = Sin(phi);
            const f32 cosP = Cos(phi);
            // direction (右手座標、equirect 規約 phi=0 が +Z、theta=0 が +Y)
            const f32 nx = sinT * sinP;
            const f32 ny = cosT;
            const f32 nz = sinT * cosP;

            const u32 idx = (y * width + x) * 4u;
            const f32 r = rgba_float[idx + 0];
            const f32 g = rgba_float[idx + 1];
            const f32 b = rgba_float[idx + 2];

            // SH basis values (cartesian form)
            f32 Y[9];
            Y[0] = inv_2sqrtPi;                                       // l=0
            Y[1] = sqrt3_2sqrtPi * ny;                                 // l=1, m=-1 (y)
            Y[2] = sqrt3_2sqrtPi * nz;                                 // l=1, m=0  (z)
            Y[3] = sqrt3_2sqrtPi * nx;                                 // l=1, m=1  (x)
            Y[4] = sqrt15_2sqrtPi * nx * ny;                           // l=2, m=-2 (xy)
            Y[5] = sqrt15_2sqrtPi * ny * nz;                           // l=2, m=-1 (yz)
            Y[6] = sqrt5_4sqrtPi  * (3.0f * nz * nz - 1.0f);           // l=2, m=0
            Y[7] = sqrt15_2sqrtPi * nx * nz;                           // l=2, m=1  (xz)
            Y[8] = sqrt15_4sqrtPi * (nx * nx - ny * ny);               // l=2, m=2

            for (u32 i = 0; i < 9; ++i) {
                sh_r[i] += static_cast<f64>(r) * Y[i] * weight;
                sh_g[i] += static_cast<f64>(g) * Y[i] * weight;
                sh_b[i] += static_cast<f64>(b) * Y[i] * weight;
            }
        }
    }
    for (u32 i = 0; i < 9; ++i) {
        out_sh_rgb[i] = FVec4{
            static_cast<f32>(sh_r[i]),
            static_cast<f32>(sh_g[i]),
            static_cast<f32>(sh_b[i]),
            0.0f
        };
    }
}

TResult<void> ImageBasedLighting::LoadEquirectHdrFromMemory(
        IRhiDevice& device, IRhiCommandList& cl,
        const f32* rgba_float, u32 width, u32 height) noexcept {
    if (!IsDiligentBackend(device)) {
        static bool s_warned = false;
        if (!s_warned) {
            ACS_LOG_WARN("ImageBasedLighting: equirect HDR requires the Diligent backend "
                         "(rebuild with -DACS_RENDER_DILIGENT=ON); skipped");
            s_warned = true;
        }
        return ACS_ERR(Render, 88, "equirect HDR requires the Diligent backend (-DACS_RENDER_DILIGENT=ON)");
    }
    if (!rgba_float || width == 0 || height == 0) {
        return ACS_ERR(Render, 165, "LoadEquirectHdrFromMemory: invalid input");
    }

    // env / irradiance / prefilter を全て無効化 → caller が Ensure* を呼び直す
    ResetEnvCubemap();

    // 1) equirect Texture2D (R32G32B32A32_Float、CPU 提供データを直接 upload)
    TUniquePtr<IRhiTexture> equirect;
    {
        FTextureDesc td{};
        td.width  = width;
        td.height = height;
        td.format = EFormat::R32G32B32A32_Float;
        td.initial_data      = rgba_float;
        td.initial_data_size = static_cast<usize>(width) * height * 4u * sizeof(f32);
        auto r = CreateRhiTexture(device, td);
        if (r.IsErr()) return Err<void>(r.Error());
        equirect = Move(r.Value());
    }

    // 2) 出力 env cubemap (FSky 由来と同サイズ、R11G11B10_Float、per-slice RTV)
    {
        FTextureDesc td{};
        td.width            = kEnvCubeSize;
        td.height           = kEnvCubeSize;
        td.format           = EFormat::R11G11B10_Float;
        td.array_size       = 6;
        td.is_cubemap       = true;
        td.is_render_target = true;
        td.per_slice_rtv    = true;
        auto r = CreateRhiTexture(device, td);
        if (r.IsErr()) return Err<void>(r.Error());
        m_EnvCube = Move(r.Value());
    }

    // 3) 一時 VS/PS/Pipeline/CB
    TUniquePtr<IRhiShader>   vs;
    TUniquePtr<IRhiShader>   ps;
    TUniquePtr<IRhiPipeline> pipeline;
    TUniquePtr<IRhiBuffer>   cb;

    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kEquirectToCubeHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "IblEqToCube.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else vs = Move(r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kEquirectToCubeHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "IblEqToCube.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else ps = Move(r.Value());

    FBufferDesc cbd{};
    cbd.size = (sizeof(EquirectCBLayout) + 255u) & ~static_cast<usize>(255u);
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else cb = Move(r.Value());

    FPipelineDesc pd{};
    pd.vs            = vs.Get();
    pd.ps            = ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = EFormat::R11G11B10_Float;
    pd.depth_format  = EFormat::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 1;
    pd.cbuffer_names[0] = "EqToCube";
    pd.texture_names[0] = "equirect";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Wrap;     // 経度方向は wrap
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;    // 緯度方向は clamp
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else pipeline = Move(r.Value());

    // 4) 6 face を描く
    ClearColor black{0, 0, 0, 1};
    cl.SetPipeline(*pipeline);
    cl.SetConstantBuffer(0, *cb);
    cl.SetTexture(0, *equirect);
    for (u32 face = 0; face < 6; ++face) {
        EquirectCBLayout data{};
        data.face_index = static_cast<i32>(face);
        cb->Update(&data, sizeof(data));

        cl.BeginRenderToTextureSlice(*m_EnvCube, face, 0, black);
        cl.Draw(3);
    }
    cl.EndRenderToTexture(*m_EnvCube);

    m_bEnvBuilt = true;
    return Ok();
}

TResult<void> ImageBasedLighting::EnsureIrradiance(IRhiDevice& device,
                                                   IRhiCommandList& cl) noexcept {
    if (m_bIrradianceBuilt) return Ok();
    // Diligent でなければ高度3D 不能 → fake-success せず capability error を返す。
    if (!IsDiligentBackend(device)) {
        static bool s_warned = false;
        if (!s_warned) {
            ACS_LOG_WARN("ImageBasedLighting: irradiance requires the Diligent backend "
                         "(rebuild with -DACS_RENDER_DILIGENT=ON); skipped");
            s_warned = true;
        }
        return ACS_ERR(Render, 88, "irradiance requires the Diligent backend (-DACS_RENDER_DILIGENT=ON)");
    }
    if (!m_EnvCube) {
        return ACS_ERR(Render, 160,
            "ImageBasedLighting::EnsureIrradiance: env cubemap not built yet");
    }
    auto r = BuildIrradiance(device, cl);
    if (r.IsErr()) return r;
    m_bIrradianceBuilt = true;
    return Ok();
}

TResult<void> ImageBasedLighting::BuildIrradiance(IRhiDevice& device,
                                                  IRhiCommandList& cl) noexcept {
    if (!IsDiligentBackend(device)) {
        static bool s_warned = false;
        if (!s_warned) {
            ACS_LOG_WARN("ImageBasedLighting: irradiance requires the Diligent backend "
                         "(rebuild with -DACS_RENDER_DILIGENT=ON); skipped");
            s_warned = true;
        }
        return ACS_ERR(Render, 88, "irradiance requires the Diligent backend (-DACS_RENDER_DILIGENT=ON)");
    }
    m_IrradianceCube.Reset();

    // 1) irradiance cubemap (6 face, 32x32, R11G11B10_Float, per-slice RTV)
    FTextureDesc td{};
    td.width            = kIrradianceSize;
    td.height           = kIrradianceSize;
    td.format           = EFormat::R11G11B10_Float;
    td.array_size       = 6;
    td.is_cubemap       = true;
    td.is_render_target = true;
    td.per_slice_rtv    = true;
    auto t_r = CreateRhiTexture(device, td);
    if (t_r.IsErr()) return Err<void>(t_r.Error());
    m_IrradianceCube = Move(t_r.Value());

    // 2) 一時 VS/PS/Pipeline/CB
    TUniquePtr<IRhiShader>   vs;
    TUniquePtr<IRhiShader>   ps;
    TUniquePtr<IRhiPipeline> pipeline;
    TUniquePtr<IRhiBuffer>   cb;

    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kIrradianceHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "IblIrradiance.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else vs = Move(r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kIrradianceHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "IblIrradiance.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else ps = Move(r.Value());

    FBufferDesc cbd{};
    cbd.size = (sizeof(IrradianceCBLayout) + 255u) & ~static_cast<usize>(255u);
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else cb = Move(r.Value());

    FPipelineDesc pd{};
    pd.vs            = vs.Get();
    pd.ps            = ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = EFormat::R11G11B10_Float;
    pd.depth_format  = EFormat::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 1;
    pd.cbuffer_names[0] = "Irradiance";
    pd.texture_names[0] = "env";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_w = ESamplerAddress::Clamp;
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
    cl.SetTexture(0, *m_EnvCube);
    for (u32 face = 0; face < 6; ++face) {
        IrradianceCBLayout data{};
        data.face_index = static_cast<i32>(face);
        cb->Update(&data, sizeof(data));

        cl.BeginRenderToTextureSlice(*m_IrradianceCube, face, 0, black);
        cl.Draw(3);
    }
    cl.EndRenderToTexture(*m_IrradianceCube);
    return Ok();
}

TResult<void> ImageBasedLighting::EnsurePrefilter(IRhiDevice& device,
                                                  IRhiCommandList& cl) noexcept {
    if (m_bPrefilterBuilt) return Ok();
    // Diligent でなければ高度3D 不能 → fake-success せず capability error を返す。
    if (!IsDiligentBackend(device)) {
        static bool s_warned = false;
        if (!s_warned) {
            ACS_LOG_WARN("ImageBasedLighting: prefilter requires the Diligent backend "
                         "(rebuild with -DACS_RENDER_DILIGENT=ON); skipped");
            s_warned = true;
        }
        return ACS_ERR(Render, 88, "prefilter requires the Diligent backend (-DACS_RENDER_DILIGENT=ON)");
    }
    if (!m_EnvCube) {
        return ACS_ERR(Render, 161,
            "ImageBasedLighting::EnsurePrefilter: env cubemap not built yet");
    }
    auto r = BuildPrefilter(device, cl);
    if (r.IsErr()) return r;
    m_bPrefilterBuilt = true;
    return Ok();
}

TResult<void> ImageBasedLighting::BuildPrefilter(IRhiDevice& device,
                                                 IRhiCommandList& cl) noexcept {
    if (!IsDiligentBackend(device)) {
        static bool s_warned = false;
        if (!s_warned) {
            ACS_LOG_WARN("ImageBasedLighting: prefilter requires the Diligent backend "
                         "(rebuild with -DACS_RENDER_DILIGENT=ON); skipped");
            s_warned = true;
        }
        return ACS_ERR(Render, 88, "prefilter requires the Diligent backend (-DACS_RENDER_DILIGENT=ON)");
    }
    m_PrefilterCube.Reset();
    m_PrefilterMips = 0;

    // 1) prefilter cubemap (6 face, 128x128, 5 mips, R11G11B10_Float, per-slice RTV)
    FTextureDesc td{};
    td.width            = kPrefilterSize;
    td.height           = kPrefilterSize;
    td.format           = EFormat::R11G11B10_Float;
    td.array_size       = 6;
    td.is_cubemap       = true;
    td.mip_levels       = kPrefilterMips;
    td.is_render_target = true;
    td.per_slice_rtv    = true;
    auto t_r = CreateRhiTexture(device, td);
    if (t_r.IsErr()) return Err<void>(t_r.Error());
    m_PrefilterCube  = Move(t_r.Value());
    m_PrefilterMips  = kPrefilterMips;

    // 2) 一時 VS/PS/Pipeline/CB
    TUniquePtr<IRhiShader>   vs;
    TUniquePtr<IRhiShader>   ps;
    TUniquePtr<IRhiPipeline> pipeline;
    TUniquePtr<IRhiBuffer>   cb;

    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kPrefilterHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "IblPrefilter.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else vs = Move(r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kPrefilterHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "IblPrefilter.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else ps = Move(r.Value());

    FBufferDesc cbd{};
    cbd.size = (sizeof(PrefilterCBLayout) + 255u) & ~static_cast<usize>(255u);
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else cb = Move(r.Value());

    FPipelineDesc pd{};
    pd.vs            = vs.Get();
    pd.ps            = ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = EFormat::R11G11B10_Float;
    pd.depth_format  = EFormat::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 1;
    pd.cbuffer_names[0] = "Prefilter";
    pd.texture_names[0] = "env";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_w = ESamplerAddress::Clamp;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else pipeline = Move(r.Value());

    // 3) 5 mip × 6 face = 30 render
    ClearColor black{0, 0, 0, 1};
    cl.SetPipeline(*pipeline);
    cl.SetConstantBuffer(0, *cb);
    cl.SetTexture(0, *m_EnvCube);
    for (u32 mip = 0; mip < kPrefilterMips; ++mip) {
        const f32 roughness = static_cast<f32>(mip) / static_cast<f32>(kPrefilterMips - 1);
        for (u32 face = 0; face < 6; ++face) {
            PrefilterCBLayout data{};
            data.face_index = static_cast<i32>(face);
            data.roughness  = roughness;
            cb->Update(&data, sizeof(data));

            cl.BeginRenderToTextureSlice(*m_PrefilterCube, face, mip, black);
            cl.Draw(3);
        }
    }
    cl.EndRenderToTexture(*m_PrefilterCube);
    return Ok();
}

TResult<void> ImageBasedLighting::EnsureSkyboxPipeline(IRhiDevice& device,
                                                       EFormat rt_format,
                                                       EFormat depth_format) noexcept {
    if (m_SkyPipeline && m_SkyRtFormat == rt_format && m_SkyDepthFormat == depth_format) {
        return Ok();
    }
    // RT/depth format が変わったら再構築
    m_SkyPipeline.Reset();
    m_SkyCb.Reset();
    m_SkyPs.Reset();
    m_SkyVs.Reset();

    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kSkyboxHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "IblSkybox.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else m_SkyVs = Move(r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSkyboxHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "IblSkybox.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else m_SkyPs = Move(r.Value());

    FBufferDesc cbd{};
    cbd.size = (sizeof(SkyboxCBLayout) + 255u) & ~static_cast<usize>(255u);
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else m_SkyCb = Move(r.Value());

    FPipelineDesc pd{};
    pd.vs            = m_SkyVs.Get();
    pd.ps            = m_SkyPs.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = depth_format;
    pd.depth_test    = false;     // 背景は最遠なので深度テスト不要
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 1;
    pd.cbuffer_names[0] = "Skybox";
    pd.texture_names[0] = "env";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_w = ESamplerAddress::Clamp;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else m_SkyPipeline = Move(r.Value());

    m_SkyRtFormat    = rt_format;
    m_SkyDepthFormat = depth_format;
    return Ok();
}

void ImageBasedLighting::DrawSkybox(IRhiDevice& device, IRhiCommandList& cl,
                                     IRhiTexture& cube,
                                     const FMat4& view_proj, FVec3 eye,
                                     EFormat rt_format, EFormat depth_format,
                                     f32 mip_level) noexcept {
    // void なので error を返せない。fake-success と同義の no-op だが、最低限
    // 一度だけ warn して capability 境界を明示する (raw-DX12 は高度3D 非対応)。
    if (!IsDiligentBackend(device)) {
        static bool s_warned = false;
        if (!s_warned) {
            ACS_LOG_WARN("ImageBasedLighting: skybox preview requires the Diligent backend "
                         "(rebuild with -DACS_RENDER_DILIGENT=ON); skipped");
            s_warned = true;
        }
        return;
    }
    if (auto r = EnsureSkyboxPipeline(device, rt_format, depth_format); r.IsErr()) return;

    SkyboxCBLayout cb{};
    cb.inv_view_proj = Inverse(view_proj);
    cb.eye           = FVec4{eye.x, eye.y, eye.z, 1};
    cb.mip_pad       = FVec4{mip_level, 0, 0, 0};
    m_SkyCb->Update(&cb, sizeof(cb));

    cl.SetPipeline(*m_SkyPipeline);
    cl.SetConstantBuffer(0, *m_SkyCb);
    cl.SetTexture(0, cube);
    cl.Draw(3);
}

void ImageBasedLighting::DrawEnvSkybox(IRhiDevice& device, IRhiCommandList& cl,
                                        const FMat4& view_proj, FVec3 eye,
                                        EFormat rt_format, EFormat depth_format) noexcept {
    if (!m_EnvCube) return;
    DrawSkybox(device, cl, *m_EnvCube, view_proj, eye, rt_format, depth_format);
}

void ImageBasedLighting::ResetEnvCubemap() noexcept {
    // env が無効になれば irradiance / prefilter も無効。
    m_PrefilterCube.Reset();
    m_PrefilterMips   = 0;
    m_bPrefilterBuilt  = false;
    m_IrradianceCube.Reset();
    m_bIrradianceBuilt = false;
    m_EnvCube.Reset();
    m_bEnvBuilt        = false;
}

void ImageBasedLighting::Shutdown() noexcept {
    m_SkyPipeline.Reset();
    m_SkyCb.Reset();
    m_SkyPs.Reset();
    m_SkyVs.Reset();
    m_SkyRtFormat    = EFormat::Unknown;
    m_SkyDepthFormat = EFormat::Unknown;
    m_PrefilterCube.Reset();
    m_PrefilterMips   = 0;
    m_IrradianceCube.Reset();
    m_EnvCube.Reset();
    m_BrdfLut.Reset();
    m_bPrefilterBuilt  = false;
    m_bIrradianceBuilt = false;
    m_bEnvBuilt        = false;
    m_bBrdfBuilt       = false;
}

} // namespace acs
