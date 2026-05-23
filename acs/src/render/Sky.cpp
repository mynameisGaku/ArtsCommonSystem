// SPDX-License-Identifier: Apache-2.0
// 手続き生成スカイ実装
#include "render/Sky.h"
#include "math/Camera.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <cstring>

namespace acs {

namespace {

// HLSL: フルスクリーン三角形（VB 不要）+ 視線方向ベース sky
const char* kSkyHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer Sky : register(b0) {
    float4x4 inv_view_proj;       // 画面 NDC → ワールドへの逆変換
    float4   camera_pos;          // xyz=eye
    float4   sun_dir;             // xyz=方向 (camera→sun)
    float4   sun_color;           // xyz=色
    float4   sun_params;          // x=radius (1-cos), y=glow, zw=pad
    float4   zenith_color;
    float4   horizon_color;
    float4   ground_color;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 ndc : TEXCOORD0;       // -1..+1 の 2D NDC
};

VSOut VSMain(uint id : SV_VertexID) {
    // 大きな三角形を 1 枚張ってフルスクリーンを覆う
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.ndc = uv * 2.0 - 1.0;
    // D3D の Y は下が +、ndc.y を反転して上から下に統一
    o.pos = float4(o.ndc.x, -o.ndc.y, 1.0, 1.0);    // z=1 (far)
    return o;
}

float4 PSMain(VSOut v) : SV_TARGET {
    // 1) NDC（z=1）から逆 VP でワールド座標を求める
    float4 wp = mul(float4(v.ndc.x, -v.ndc.y, 1.0, 1.0), inv_view_proj);
    wp.xyz /= wp.w;
    float3 dir = normalize(wp.xyz - camera_pos.xyz);

    // 2) 高さ角でグラデ。dir.y = 0 が地平線、+1 が天頂、-1 が真下
    float t = dir.y;
    float3 sky;
    if (t >= 0.0) {
        // 地平線 → 天頂
        sky = lerp(horizon_color.xyz, zenith_color.xyz, pow(saturate(t), 0.6));
    } else {
        // 地平線 → 地面
        sky = lerp(horizon_color.xyz, ground_color.xyz, pow(saturate(-t), 0.6));
    }

    // 3) 太陽: 視線と太陽方向の角度
    float c    = saturate(dot(dir, normalize(sun_dir.xyz)));
    float ang  = 1.0 - c;    // 0 = 太陽の中心
    if (ang < sun_params.x) {
        sky = sun_color.xyz;
    } else if (ang < sun_params.y) {
        float k = 1.0 - smoothstep(sun_params.x, sun_params.y, ang);
        sky = lerp(sky, sun_color.xyz, k);
    }

    return float4(sky, 1.0);
}
)";

struct SkyCB {
    Mat4 inv_view_proj;
    Vec4 camera_pos;
    Vec4 sun_dir;
    Vec4 sun_color;
    Vec4 sun_params;     // (radius, glow, _, _)
    Vec4 zenith;
    Vec4 horizon;
    Vec4 ground;
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

ACS_FORCEINLINE Vec3 NormalizeSafe(Vec3 v) noexcept {
    const f32 len2 = v.x*v.x + v.y*v.y + v.z*v.z;
    if (len2 < 1e-12f) return Vec3{0, 1, 0};
    const f32 inv = 1.0f / Sqrt(len2);
    return { v.x * inv, v.y * inv, v.z * inv };
}

} // namespace

void Sky::SetSunDirection(Vec3 dir) noexcept { _sun_dir = NormalizeSafe(dir); }

Result<void> Sky::Init(IRhiDevice& device, EFormat rt_format, EFormat depth_format) noexcept {
    ShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kSkyHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Sky.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    _vs = Move(vs_r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSkyHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Sky.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    _ps = Move(ps_r.Value());

    BufferDesc cbd{};
    cbd.size = CBSize<SkyCB>();
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    auto cb_r = CreateRhiBuffer(device, cbd);
    if (cb_r.IsErr()) return Err<void>(cb_r.Error());
    _cb = Move(cb_r.Value());

    PipelineDesc pd{};
    pd.vs = _vs.Get();
    pd.ps = _ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = depth_format;
    pd.depth_test    = false;          // sky は最初に塗るだけ。既存深度は維持
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 0;
    pd.cbuffer_names[0] = "Sky";
    pd.layout_count  = 0;
    pd.vertex_stride = 0;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    _pipeline = Move(pl_r.Value());

    return Ok();
}

void Sky::Shutdown() noexcept {
    _pipeline.Reset();
    _cb.Reset();
    _ps.Reset();
    _vs.Reset();
}

void Sky::PresetDay() noexcept {
    _sun_dir     = NormalizeSafe(Vec3{0.4f, 0.7f, 0.4f});
    _sun_color   = Vec3{1.0f, 0.95f, 0.85f};
    _sun_radius  = 0.0006f;
    _sun_glow    = 0.05f;
    _zenith      = Vec3{0.15f, 0.35f, 0.78f};
    _horizon     = Vec3{0.70f, 0.83f, 0.95f};
    _ground      = Vec3{0.18f, 0.20f, 0.20f};
}

void Sky::PresetSunset() noexcept {
    _sun_dir     = NormalizeSafe(Vec3{0.7f, 0.05f, 0.5f});
    _sun_color   = Vec3{1.0f, 0.55f, 0.25f};
    _sun_radius  = 0.001f;
    _sun_glow    = 0.20f;
    _zenith      = Vec3{0.06f, 0.10f, 0.30f};
    _horizon     = Vec3{1.00f, 0.55f, 0.25f};
    _ground      = Vec3{0.10f, 0.06f, 0.08f};
}

void Sky::PresetNight() noexcept {
    _sun_dir     = NormalizeSafe(Vec3{0.3f, 0.6f, 0.2f});
    _sun_color   = Vec3{0.85f, 0.85f, 0.95f};
    _sun_radius  = 0.0008f;
    _sun_glow    = 0.04f;
    _zenith      = Vec3{0.02f, 0.03f, 0.08f};
    _horizon     = Vec3{0.05f, 0.07f, 0.15f};
    _ground      = Vec3{0.02f, 0.03f, 0.05f};
}

void Sky::Render(IRhiCommandList& cl, const Camera& camera) noexcept {
    if (!_pipeline || !_cb) return;
    SkyCB cb{};
    cb.inv_view_proj = Inverse(camera.ViewProjection());
    Vec3 eye = camera.Eye();
    cb.camera_pos = Vec4{eye.x, eye.y, eye.z, 1};
    cb.sun_dir    = Vec4{_sun_dir.x, _sun_dir.y, _sun_dir.z, 0};
    cb.sun_color  = Vec4{_sun_color.x, _sun_color.y, _sun_color.z, 1};
    cb.sun_params = Vec4{_sun_radius, _sun_glow, 0, 0};
    cb.zenith     = Vec4{_zenith.x, _zenith.y, _zenith.z, 1};
    cb.horizon    = Vec4{_horizon.x, _horizon.y, _horizon.z, 1};
    cb.ground     = Vec4{_ground.x, _ground.y, _ground.z, 1};
    _cb->Update(&cb, sizeof(cb));

    cl.SetPipeline(*_pipeline);
    cl.SetConstantBuffer(0, *_cb);
    cl.Draw(3);    // VB 無し、SV_VertexID で 3 頂点
}

} // namespace acs
