// SPDX-License-Identifier: Apache-2.0
// 2D 動的ライティング + ソフト影 / ブロブ影 実装。
#include "render/Light2D.h"
#include "render/SpriteBatch.h"
#include "foundation/Move.h"

namespace acs {

namespace {

/**
 * scene × (ambient + Σ light·影) を合成する fullscreen シェーダの HLSL ソース。
 *
 * @details
 * occluder mask を linear sample しつつ、面光源近似で複数レイを撃って penumbra を
 * 出す。座標は全てピクセル空間 (occluder/scene と同じ)。
 */
const char* kLight2DHLSL = R"(
cbuffer Light2DCB : register(b0) {
    float4 ambient;     // rgb = ambient, w = light count
    float4 screen;      // x = 1/w, y = 1/h, z = march steps, w = ray count
    float4 lpos[16];    // xy = pos(px), z = radius(px), w = intensity
    float4 lcol[16];    // rgb = color, w = softness(0..1)
};

Texture2D    scene_tex : register(t0);
Texture2D    occ_tex   : register(t1);
SamplerState lin_samp  : register(s0);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.uv  = uv;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.pos.y = -o.pos.y;
    return o;
}

// Interleaved Gradient Noise (banding 抑制の per-pixel jitter)
float IGN(float2 p) {
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// P から光源へのソフト影 visibility (1=照らされる, 0=完全に影)。
// 面光源近似: 光源を softness に比例した幅に広げ、複数レイの遮蔽率で penumbra。
float Visibility(float2 P, float2 Lp, float softness, float jitter) {
    int steps = (int)screen.z;
    int rays  = (int)screen.w;
    float2 toL  = Lp - P;
    float  dlen = length(toL);
    if (dlen < 1e-3) return 1.0;
    float2 dir  = toL / dlen;
    float2 perp = float2(-dir.y, dir.x);
    float  spread = softness * 28.0;          // 面光源の半径 (px)

    float vis = 0.0;
    [loop]
    for (int r = 0; r < rays; ++r) {
        // 光源側のターゲットを perp 方向にずらす (面光源サンプル)
        float fr = ((float(r) + jitter) / float(rays)) - 0.5;   // -0.5 .. 0.5
        float2 target = Lp + perp * (fr * 2.0 * spread);
        float2 rstep  = (target - P) / float(steps);
        float  hit = 0.0;
        [loop]
        for (int s = 1; s < steps; ++s) {     // 端点 (受光点/光源) はスキップ
            float2 sp = P + rstep * float(s);
            float  o  = occ_tex.SampleLevel(lin_samp, sp * screen.xy, 0).r;
            if (o > 0.5) { hit = 1.0; break; }
        }
        vis += 1.0 - hit;
    }
    return vis / float(rays);
}

float4 PSMain(VSOut v) : SV_TARGET {
    float2 px  = v.uv / screen.xy;            // ピクセル座標
    float3 col = scene_tex.SampleLevel(lin_samp, v.uv, 0).rgb;

    float3 light = ambient.rgb;
    int n = (int)ambient.w;
    float jitter = IGN(v.pos.xy);

    [loop]
    for (int i = 0; i < n; ++i) {
        float2 Lp   = lpos[i].xy;
        float  R    = lpos[i].z;
        float2 d    = Lp - px;
        float  dist = length(d);
        if (dist < R) {
            float fall = saturate(1.0 - dist / R);
            fall = fall * fall;                // 滑らかな二次 falloff
            float vis = Visibility(px, Lp, lcol[i].w, jitter);
            light += lcol[i].rgb * lpos[i].w * fall * vis;
        }
    }
    return float4(col * light, 1.0);
}
)";

/** Light2DCB 定数バッファの CPU 側レイアウト (HLSL の cbuffer と一致)。 */
struct FLight2DcbLayout {
    /** rgb = ambient、w = light count。 */
    FVec4 ambient;

    /** x = 1/w、y = 1/h、z = march steps、w = ray count。 */
    FVec4 screen;

    /** 各光源の xy = pos(px)、z = radius(px)、w = intensity。 */
    FVec4 lpos[CLighting2D::kMaxLights];

    /** 各光源の rgb = color、w = softness(0..1)。 */
    FVec4 lcol[CLighting2D::kMaxLights];
};

/**
 * 定数バッファサイズを 256 バイト境界に切り上げて返す。
 *
 * @tparam T サイズを求める定数バッファのレイアウト型。
 * @return 256 の倍数に切り上げた sizeof(T)。
 */
template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

/** GPU リソース (scene/occluder RT・パイプライン・定数バッファ) を確保する。 */
TResult<void> CLighting2D::Init(IRhiDevice& device, EFormat color_format,
                                u32 width, u32 height) noexcept {
    m_Device      = &device;
    m_ColorFormat = color_format;
    m_Width       = width;
    m_Height      = height;

    // OnStart 時点では swapchain サイズが未確定 (0) のことがある (DX12 raw は
    // window resize イベントで初めてサイズが入る)。0 のときは RT 作成を遅延し、
    // 最初の有効サイズで Resize() が作る。
    if (width > 0 && height > 0) {
        if (auto r = CreateTargets(device, width, height); r.IsErr()) return r;
    }
    if (auto r = CreatePipeline(device); r.IsErr()) return r;

    FBufferDesc cbd{};
    cbd.size         = CBSize<FLight2DcbLayout>();
    cbd.usage        = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    auto r = CreateRhiBuffer(device, cbd);
    if (r.IsErr()) return Err<void>(r.Error());
    m_Cb = Move(r.Value());
    return Ok();
}

/** scene RT と occluder RT を生成する (既存があれば作り直す)。 */
TResult<void> CLighting2D::CreateTargets(IRhiDevice& device, u32 w, u32 h) noexcept {
    m_SceneRt.Reset();
    m_OccluderRt.Reset();

    FTextureDesc sd{};
    sd.width            = w;
    sd.height           = h;
    sd.format           = m_ColorFormat;
    sd.is_render_target = true;
    auto sr = CreateRhiTexture(device, sd);
    if (sr.IsErr()) return Err<void>(sr.Error());
    m_SceneRt = Move(sr.Value());

    FTextureDesc od{};
    od.width            = w;
    od.height           = h;
    od.format           = m_ColorFormat;     // SpriteBatch と同フォーマットで描けるように
    od.is_render_target = true;
    auto orr = CreateRhiTexture(device, od);
    if (orr.IsErr()) return Err<void>(orr.Error());
    m_OccluderRt = Move(orr.Value());
    return Ok();
}

/** 合成シェーダ (VS/PS) とパイプラインを生成する。 */
TResult<void> CLighting2D::CreatePipeline(IRhiDevice& device) noexcept {
    FShaderDesc vs_d{};
    vs_d.stage       = EShaderStage::Vertex;
    vs_d.hlsl_source = kLight2DHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "CLighting2D.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else m_Vs = Move(r.Value());

    FShaderDesc ps_d{};
    ps_d.stage       = EShaderStage::Pixel;
    ps_d.hlsl_source = kLight2DHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "CLighting2D.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else m_Ps = Move(r.Value());

    FPipelineDesc pd{};
    pd.vs            = m_Vs.Get();
    pd.ps            = m_Ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = m_ColorFormat;
    pd.depth_format  = EFormat::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 2;
    pd.cbuffer_names[0] = "Light2DCB";
    pd.texture_names[0] = "scene_tex";
    pd.texture_names[1] = "occ_tex";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else m_Pipeline = Move(r.Value());
    return Ok();
}

/** 確保した全 GPU リソースを解放し、状態をリセットする。 */
void CLighting2D::Shutdown() noexcept {
    m_Pipeline.Reset();
    m_Cb.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
    m_OccluderRt.Reset();
    m_SceneRt.Reset();
    m_Device      = nullptr;
    m_LightCount  = 0;
}

/** 解像度変更時に内部 RT を作り直す (無効/同サイズは no-op)。 */
TResult<void> CLighting2D::Resize(u32 width, u32 height) noexcept {
    if (!m_Device) return ACS_ERR(Render, 360, "CLighting2D::Resize before Init");
    if (width == 0 || height == 0) return Ok();                 // 無効サイズは無視
    if (width == m_Width && height == m_Height && m_SceneRt) return Ok();
    m_Width  = width;
    m_Height = height;
    return CreateTargets(*m_Device, width, height);
}

/** 点光源を 1 個追加する (上限到達なら false)。 */
bool CLighting2D::AddLight(const FLight2D& light) noexcept {
    if (m_LightCount >= kMaxLights) return false;
    m_Lights[m_LightCount++] = light;
    return true;
}

/** 影の品質を設定する (march_steps を 2..64、ray_count を 1..16 に clamp)。 */
void CLighting2D::SetShadowQuality(u32 march_steps, u32 ray_count) noexcept {
    m_MarchSteps = (march_steps < 2u) ? 2u : (march_steps > 64u ? 64u : march_steps);
    m_RayCount   = (ray_count   < 1u) ? 1u : (ray_count   > 16u ? 16u : ray_count);
}

/** scene RT を bind + clear して世界の albedo 描画 bracket を開始する。 */
void CLighting2D::BeginScene(IRhiCommandList& cl, FVec4 clear) noexcept {
    if (!m_SceneRt) return;
    cl.BeginRenderToTexture(*m_SceneRt, FClearColor{clear.x, clear.y, clear.z, clear.w},
                            nullptr, 1.0f);
}

/** scene 描画 bracket を終了する。 */
void CLighting2D::EndScene(IRhiCommandList& cl) noexcept {
    if (m_SceneRt) cl.EndRenderToTexture(*m_SceneRt);
}

/** occluder RT を黒 clear して影スプライト描画 bracket を開始する。 */
void CLighting2D::BeginOccluders(IRhiCommandList& cl) noexcept {
    if (!m_OccluderRt) return;
    cl.BeginRenderToTexture(*m_OccluderRt, FClearColor{0, 0, 0, 1}, nullptr, 1.0f);
}

/** occluder 描画 bracket を終了する。 */
void CLighting2D::EndOccluders(IRhiCommandList& cl) noexcept {
    if (m_OccluderRt) cl.EndRenderToTexture(*m_OccluderRt);
}

/** 定数バッファを更新し、現在 bind 中のターゲットへ scene × light を合成描画する。 */
void CLighting2D::Composite(IRhiCommandList& cl, u32 screen_w, u32 screen_h) noexcept {
    if (!m_Pipeline || !m_Cb || !m_SceneRt || !m_OccluderRt) return;

    FLight2DcbLayout data{};
    data.ambient = FVec4{m_Ambient.x, m_Ambient.y, m_Ambient.z,
                         static_cast<f32>(m_LightCount)};
    data.screen  = FVec4{1.0f / static_cast<f32>(screen_w),
                         1.0f / static_cast<f32>(screen_h),
                         static_cast<f32>(m_MarchSteps),
                         static_cast<f32>(m_RayCount)};
    for (u32 i = 0; i < m_LightCount; ++i) {
        const FLight2D& L = m_Lights[i];
        data.lpos[i] = FVec4{L.pos.x, L.pos.y, L.radius, L.intensity};
        data.lcol[i] = FVec4{L.color.x, L.color.y, L.color.z, L.softness};
    }
    m_Cb->Update(&data, sizeof(data));

    cl.SetPipeline(*m_Pipeline);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.SetTexture(0, *m_SceneRt);
    cl.SetTexture(1, *m_OccluderRt);
    cl.Draw(3);
}

/** 柔らかい放射状グラデーションの影テクスチャを CPU で生成して GPU へ上げる。 */
TResult<void> CBlobShadow::Init(IRhiDevice& device, u32 resolution) noexcept {
    u32 res = resolution;
    if (res < 16u) res = 16u;
    if (res > 64u) res = 64u;       // 64² の固定スタックバッファに収める

    u8 pixels[64 * 64 * 4] = {};
    const f32 c = (static_cast<f32>(res) - 1.0f) * 0.5f;
    const f32 inv_r = 1.0f / (static_cast<f32>(res) * 0.5f);
    for (u32 y = 0; y < res; ++y) {
        for (u32 x = 0; x < res; ++x) {
            const f32 dx = (static_cast<f32>(x) - c) * inv_r;
            const f32 dy = (static_cast<f32>(y) - c) * inv_r;
            f32 d = dx * dx + dy * dy;          // 半径² (中心 0、端 ~1)
            if (d > 1.0f) d = 1.0f;
            f32 a = 1.0f - d;                   // 中心 1 → 端 0
            a = a * a;                          // 柔らかく
            const u32 idx = (y * res + x) * 4u;
            pixels[idx + 0] = 0;
            pixels[idx + 1] = 0;
            pixels[idx + 2] = 0;
            pixels[idx + 3] = static_cast<u8>(a * 255.0f + 0.5f);
        }
    }

    FTextureDesc td{};
    td.width             = res;
    td.height            = res;
    td.format            = EFormat::R8G8B8A8_UNorm;
    td.initial_data      = pixels;
    td.initial_data_size = static_cast<usize>(res) * res * 4u;
    auto r = CreateRhiTexture(device, td);
    if (r.IsErr()) return Err<void>(r.Error());
    m_Tex = Move(r.Value());
    return Ok();
}

/** 影テクスチャを解放する。 */
void CBlobShadow::Shutdown() noexcept {
    m_Tex.Reset();
}

/** (cx,cy) 中心に w×h の影テクスチャを SpriteBatch で描く。 */
void CBlobShadow::Draw(CSpriteBatch& sb, f32 cx, f32 cy, f32 w, f32 h,
                       f32 alpha, FVec3 color) noexcept {
    if (!m_Tex) return;
    sb.Draw(*m_Tex, cx - w * 0.5f, cy - h * 0.5f, w, h,
            FVec4{color.x, color.y, color.z, alpha});
}

} // namespace acs
