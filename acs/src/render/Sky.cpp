// SPDX-License-Identifier: Apache-2.0
// 手続き生成スカイ実装
#include "render/Sky.h"
#include "math/Camera.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <cstring>

namespace acs {

namespace {

/** スカイの HLSL ソース (フルスクリーン三角形 + 視線方向ベースの空と太陽。VB 不要)。 */
const char* kSkyHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer FSky : register(b0) {
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

/**
 * スカイ定数バッファのレイアウト (HLSL の cbuffer FSky と一致)。
 */
struct SkyCB {
    /** 画面 NDC からワールドへの逆 view-projection。 */
    FMat4 inv_view_proj;

    /** 視点ワールド座標 (xyz=eye)。 */
    FVec4 camera_pos;

    /** 太陽方向 (xyz、camera→sun)。 */
    FVec4 sun_dir;

    /** 太陽の色 (xyz)。 */
    FVec4 sun_color;

    /** 太陽パラメータ (x=radius(1-cos), y=glow, zw=pad)。 */
    FVec4 sun_params;

    /** 天頂の色。 */
    FVec4 zenith;

    /** 地平線の色。 */
    FVec4 horizon;

    /** 地面方向の色。 */
    FVec4 ground;
};

/**
 * 定数バッファサイズを 256 バイト境界に切り上げる (DX12 制約)。
 *
 * @tparam T 定数バッファのレイアウト型。
 * @return sizeof(T) を 256 の倍数に切り上げたバイト数。
 */
template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

/**
 * ゼロ長を避けてベクトルを正規化する。
 *
 * @param v 正規化するベクトル。
 * @return 正規化したベクトル。長さがほぼ 0 なら (0,1,0) を返す。
 */
ACS_FORCEINLINE FVec3 NormalizeSafe(FVec3 v) noexcept {
    const f32 len2 = v.x*v.x + v.y*v.y + v.z*v.z;
    if (len2 < 1e-12f) return FVec3{0, 1, 0};
    const f32 inv = 1.0f / Sqrt(len2);
    return { v.x * inv, v.y * inv, v.z * inv };
}

} // namespace

/** 太陽方向を正規化して保持する。 */
void FSky::SetSunDirection(FVec3 dir) noexcept { m_SunDir = NormalizeSafe(dir); }

/** VS/PS/定数バッファ/パイプラインを生成する。 */
TResult<void> FSky::Init(IRhiDevice& device, EFormat rt_format, EFormat depth_format) noexcept {
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kSkyHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "FSky.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    m_Vs = Move(vs_r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSkyHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "FSky.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    m_Ps = Move(ps_r.Value());

    FBufferDesc cbd{};
    cbd.size = CBSize<SkyCB>();
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    auto cb_r = CreateRhiBuffer(device, cbd);
    if (cb_r.IsErr()) return Err<void>(cb_r.Error());
    m_Cb = Move(cb_r.Value());

    FPipelineDesc pd{};
    pd.vs = m_Vs.Get();
    pd.ps = m_Ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = depth_format;
    pd.depth_test    = false;          // sky は最初に塗るだけ。既存深度は維持
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 0;
    pd.cbuffer_names[0] = "FSky";
    pd.layout_count  = 0;
    pd.vertex_stride = 0;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    m_Pipeline = Move(pl_r.Value());

    return Ok();
}

/** GPU リソースを解放する。 */
void FSky::Shutdown() noexcept {
    m_Pipeline.Reset();
    m_Cb.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
}

/** 昼空プリセット (青空 + 白い太陽) を適用する。 */
void FSky::PresetDay() noexcept {
    m_SunDir     = NormalizeSafe(FVec3{0.4f, 0.7f, 0.4f});
    m_SunColor   = FVec3{1.0f, 0.95f, 0.85f};
    m_SunRadius  = 0.0006f;
    m_SunGlow    = 0.05f;
    m_Zenith      = FVec3{0.15f, 0.35f, 0.78f};
    m_Horizon     = FVec3{0.70f, 0.83f, 0.95f};
    m_Ground      = FVec3{0.18f, 0.20f, 0.20f};
}

/** 夕焼けプリセット (茜色 + 暖色太陽) を適用する。 */
void FSky::PresetSunset() noexcept {
    m_SunDir     = NormalizeSafe(FVec3{0.7f, 0.05f, 0.5f});
    m_SunColor   = FVec3{1.0f, 0.55f, 0.25f};
    m_SunRadius  = 0.001f;
    m_SunGlow    = 0.20f;
    m_Zenith      = FVec3{0.06f, 0.10f, 0.30f};
    m_Horizon     = FVec3{1.00f, 0.55f, 0.25f};
    m_Ground      = FVec3{0.10f, 0.06f, 0.08f};
}

/** 夜空プリセット (紺青 + 弱い月光) を適用する。 */
void FSky::PresetNight() noexcept {
    m_SunDir     = NormalizeSafe(FVec3{0.3f, 0.6f, 0.2f});
    m_SunColor   = FVec3{0.85f, 0.85f, 0.95f};
    m_SunRadius  = 0.0008f;
    m_SunGlow    = 0.04f;
    m_Zenith      = FVec3{0.02f, 0.03f, 0.08f};
    m_Horizon     = FVec3{0.05f, 0.07f, 0.15f};
    m_Ground      = FVec3{0.02f, 0.03f, 0.05f};
}

/** 定数バッファを更新し、フルスクリーン三角形でスカイを描画する。 */
void FSky::Render(IRhiCommandList& cl, const FCamera& camera) noexcept {
    if (!m_Pipeline || !m_Cb) return;
    SkyCB cb{};
    cb.inv_view_proj = Inverse(camera.ViewProjection());
    const FVec3 eye = camera.Eye();
    cb.camera_pos = FVec4{eye.x, eye.y, eye.z, 1};
    cb.sun_dir    = FVec4{m_SunDir.x, m_SunDir.y, m_SunDir.z, 0};
    cb.sun_color  = FVec4{m_SunColor.x, m_SunColor.y, m_SunColor.z, 1};
    cb.sun_params = FVec4{m_SunRadius, m_SunGlow, 0, 0};
    cb.zenith     = FVec4{m_Zenith.x, m_Zenith.y, m_Zenith.z, 1};
    cb.horizon    = FVec4{m_Horizon.x, m_Horizon.y, m_Horizon.z, 1};
    cb.ground     = FVec4{m_Ground.x, m_Ground.y, m_Ground.z, 1};
    m_Cb->Update(&cb, sizeof(cb));

    cl.SetPipeline(*m_Pipeline);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.Draw(3);    // VB 無し、SV_VertexID で 3 頂点
}

} // namespace acs
