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
    float4   cloud_params0;       // x=coverage(0..1), y=density(sharpness), z=time, w=enabled(0/1)
    float4   cloud_params1;       // xyz=cloud_color, w=wind_speed
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

// ---- 雲用の value noise + FBM ----------------------------------------------
// 軽量なハッシュ value noise を 6 octave 重ね、octave ごとに座標を回転させて
// 軸に揃ったタイリングを崩す。gradient/simplex より単純だが、回転 + 多 octave で
// 十分に有機的な雲になる。
float Hash2(float2 p) {
    p = frac(p * float2(127.1, 311.7));
    p += dot(p, p + 34.23);
    return frac(p.x * p.y);
}
float ValueNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);          // smoothstep 補間で格子段差を消す
    float a = Hash2(i + float2(0.0, 0.0));
    float b = Hash2(i + float2(1.0, 0.0));
    float c = Hash2(i + float2(0.0, 1.0));
    float d = Hash2(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}
float Fbm(float2 p) {
    float sum = 0.0;
    float amp = 0.5;
    const float2x2 rot = float2x2(0.80, -0.60, 0.60, 0.80);   // ~37deg 回転
    [unroll]
    for (int i = 0; i < 6; ++i) {
        sum += amp * ValueNoise(p);
        p    = mul(rot, p) * 2.02;               // lacunarity ~2 + 回転
        amp *= 0.5;                              // gain 0.5
    }
    return sum;
}

// ---- volumetric clouds 用 3D value noise + FBM (レイマーチ立体雲) ------------
float Hash3(float3 p) {
    p = frac(p * 0.3183099 + float3(0.1, 0.2, 0.3));
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float ValueNoise3(float3 p) {
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);                          // smoothstep 補間
    float n000 = Hash3(i + float3(0,0,0)), n100 = Hash3(i + float3(1,0,0));
    float n010 = Hash3(i + float3(0,1,0)), n110 = Hash3(i + float3(1,1,0));
    float n001 = Hash3(i + float3(0,0,1)), n101 = Hash3(i + float3(1,0,1));
    float n011 = Hash3(i + float3(0,1,1)), n111 = Hash3(i + float3(1,1,1));
    float nx00 = lerp(n000, n100, f.x), nx10 = lerp(n010, n110, f.x);
    float nx01 = lerp(n001, n101, f.x), nx11 = lerp(n011, n111, f.x);
    return lerp(lerp(nx00, nx10, f.y), lerp(nx01, nx11, f.y), f.z);
}
float Fbm3(float3 p) {
    float sum = 0.0, amp = 0.5;
    [unroll] for (int i = 0; i < 5; ++i) { sum += amp * ValueNoise3(p); p *= 2.03; amp *= 0.5; }
    return sum;
}
// 雲スラブ内の点 p の密度 (0..1)。高周波 base + 高度プロファイル + 多段 detail 侵食で «くっきりした»
// 立体雲に (低周波の大きい塊だとボケて低解像度に見えるため、周波数とコントラストを上げる)。
float CloudDensity3(float3 p, float coverage, float windOff) {
    // 高度プロファイル: スラブ中央で濃く上下で薄い → 丸みのある雲塊 (cumulus)。p.y はスラブ高度。
    float hf = saturate((p.y - 1.0) / 1.6);
    float profile = smoothstep(0.0, 0.22, hf) * smoothstep(1.0, 0.5, hf);
    if (profile <= 0.001) return 0.0;
    float3 q = p * 2.4 + float3(windOff, windOff * 0.2, windOff * 0.6);   // 高周波化 → 雲の数とディテール増
    float base  = Fbm3(q);
    float shape = saturate((base - (1.0 - coverage)) / max(coverage, 0.001)) * profile;
    if (shape <= 0.0) return 0.0;
    // 2 段の高周波 detail で縁を侵食 → もこもこ + wispy なディテール
    float d1 = Fbm3(q * 3.0 + 11.3);
    float d2 = Fbm3(q * 8.0 + 27.1);
    float d  = saturate(shape - (1.0 - shape) * (d1 * 0.45 + d2 * 0.25));
    return d * d * (3.0 - 2.0 * d);                       // smoothstep でコントラスト強調 (くっきり)
}

// IGN ベースの dither (8-bit 量子化前にバンディングを消す)。
float SkyDither(float2 pix, float t) {
    pix += t * float2(5.588238, 1.715728);
    return frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715))));
}

float4 PSMain(VSOut v) : SV_TARGET {
    // 1) NDC（z=1）から逆 VP でワールド座標を求める
    float4 wp = mul(float4(v.ndc.x, -v.ndc.y, 1.0, 1.0), inv_view_proj);
    wp.xyz /= wp.w;
    float3 dir   = normalize(wp.xyz - camera_pos.xyz);
    float3 sundn = normalize(sun_dir.xyz);

    // 2) 高さ角でグラデ。dir.y = 0 が地平線、+1 が天頂、-1 が真下。
    //    指数を 0.5 に緩めて天頂までの遷移を滑らかにする。
    float t = dir.y;
    float3 sky;
    if (t >= 0.0) {
        sky = lerp(horizon_color.xyz, zenith_color.xyz, pow(saturate(t), 0.5));
    } else {
        sky = lerp(horizon_color.xyz, ground_color.xyz, pow(saturate(-t), 0.5));
    }

    // 2.5) 太陽方向の地平線グロー (Mie 前方散乱の簡易ローブ): 地平線付近 + 太陽方位で
    //      暖色を盛り、のっぺりした 2-stop グラデを大気らしくする。
    float sun_d      = saturate(dot(dir, sundn));
    float horizonBnd = exp(-abs(t) * 6.0);             // 地平線に集中
    float glow       = pow(sun_d, 4.0) * horizonBnd;
    sky += sun_color.xyz * glow * 0.6;

    // 3) 太陽ディスク + ハロー
    float c   = sun_d;
    float ang = 1.0 - c;    // 0 = 太陽の中心
    if (ang < sun_params.x) {
        sky = sun_color.xyz;
    } else if (ang < sun_params.y) {
        float k = 1.0 - smoothstep(sun_params.x, sun_params.y, ang);
        sky = lerp(sky, sun_color.xyz, k);
    }

    // 4) volumetric clouds: 雲スラブ [h0,h1] を視線方向にレイマーチして密度を積分。各サンプルで太陽へ
    //    ライトマーチして自己影 (Beer-Lambert) → 立体的な雲。地平線より上のみ。
    if (cloud_params0.w >= 0.5 && dir.y > 0.02) {
        float coverage  = saturate(cloud_params0.x);
        float density   = max(cloud_params0.y, 0.1);
        float windOff   = cloud_params0.z * cloud_params1.w * 0.03;   // time*wind で流す

        const float h0 = 1.0, h1 = 2.6;                  // 雲スラブの仮想高度 (dome 空間)
        float t0 = h0 / dir.y, t1 = h1 / dir.y;          // ray がスラブに入る/出る距離
        const int  N  = 48;                              // 高周波ノイズを拾うためステップ増 (undersample 回避)
        float dt = (t1 - t0) / float(N);
        float3 litCol   = lerp(cloud_params1.xyz, sun_color.xyz, sun_d * 0.6);
        float3 shadowCol= cloud_params1.xyz * 0.30;

        // per-pixel ジッタ: 全ピクセルで march 位相が揃うと縦縞バンドが出るため、開始位相を画素ごとに散らす
        // (Bayer/blue-noise ディザ相当)。TAA + 出力ディザが残差を均す。
        float jit = SkyDither(v.pos.xy, cloud_params0.z);   // [0,1)
        float  transmit = 1.0;                           // 視線方向の透過率 (1=空, 0=厚い雲)
        float3 scatter  = float3(0,0,0);
        [loop] for (int i = 0; i < N; ++i) {
            float3 p = dir * (t0 + dt * (float(i) + jit));
            float dens = CloudDensity3(p, coverage, windOff) * density;
            if (dens > 0.01) {
                // 太陽方向へ 3 ステップ light march → 上流の雲量で自己影
                float lightDens = 0.0;
                [unroll] for (int l = 1; l <= 3; ++l)
                    lightDens += CloudDensity3(p + sundn * (0.12 * float(l)), coverage, windOff);
                float lightT = exp(-lightDens * 0.9);
                float3 lit   = lerp(shadowCol, litCol, lightT);
                lit += sun_color.xyz * pow(sun_d, 8.0) * (1.0 - lightT) * 0.5;   // silver lining
                float a = 1.0 - exp(-dens * dt * 6.0);    // この区間の不透明度 (消衰を上げ縁をくっきり)
                scatter  += transmit * a * lit;           // front-to-back 合成
                transmit *= (1.0 - a);
                if (transmit < 0.02) break;
            }
        }
        float hFade = smoothstep(0.02, 0.10, dir.y);      // 地平線で雲を消す
        float cloudA = (1.0 - transmit) * hFade;
        sky = sky * (1.0 - cloudA) + scatter * hFade;     // 空の上に雲を合成
    }

    // 5) Dither: 8-bit 出力時のグラデ縞を消す (HDR 出力時は ±1/255 で実質無影響)。
    //    d2 は軸別オフセット + 別の時間位相で d1 と独立化し、足して TPDF にする。
    float d1 = SkyDither(v.pos.xy, cloud_params0.z);
    float d2 = SkyDither(v.pos.xy + float2(113.0, 71.0), cloud_params0.z * 0.37 + 0.5);
    sky += (d1 + d2 - 1.0) * (1.0 / 255.0);

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

    /** 雲パラメータ0 (x=coverage, y=density, z=time, w=enabled)。 */
    FVec4 cloud0;

    /** 雲パラメータ1 (xyz=cloud_color, w=wind_speed)。 */
    FVec4 cloud1;
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
    m_CloudColor    = FVec3{1.0f, 1.0f, 1.0f};
    m_CloudCoverage = 0.60f;
    m_CloudDensity  = 1.1f;
    m_CloudWind     = 1.0f;
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
    m_CloudColor    = FVec3{1.0f, 0.72f, 0.50f};   // 茜色に染まった雲
    m_CloudCoverage = 0.55f;
    m_CloudDensity  = 1.3f;
    m_CloudWind     = 0.8f;
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
    m_CloudColor    = FVec3{0.10f, 0.12f, 0.20f};  // 紺青の薄い雲
    m_CloudCoverage = 0.38f;
    m_CloudDensity  = 1.8f;
    m_CloudWind     = 0.5f;
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
    cb.cloud0     = FVec4{m_CloudCoverage, m_CloudDensity, m_Time,
                          m_bCloudsEnabled ? 1.0f : 0.0f};
    cb.cloud1     = FVec4{m_CloudColor.x, m_CloudColor.y, m_CloudColor.z, m_CloudWind};
    m_Cb->Update(&cb, sizeof(cb));

    cl.SetPipeline(*m_Pipeline);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.Draw(3);    // VB 無し、SV_VertexID で 3 頂点

    // 雲アニメ用の時間を内部で進める。これにより呼び出し側が SetTime を書かなくても
    // 雲が流れる (SetTime を毎フレーム呼べばそちらが優先され、決定論的に制御できる)。
    // 60fps 想定の固定ステップ。雲はゆっくり流れるので実フレームレート差は問題にならない。
    m_Time += 1.0f / 60.0f;
}

// ===================== FVolumetricClouds (GPU レイマーチ) =====================
namespace {

// 雲レイマーチ compute。視線ごとに雲スラブを march、Worley FBM 密度を coverage/height で remap、
// 太陽へ light-march (Beer) + dual-lobe HG + powder でエネルギー保存散乱を積分。出力=非premult色+alpha。
// ---- Phase 4.5: Perlin-Worley 3D noise を init で 1 回焼く (WickedEngine/Nubis 流) ----
// 128^3 RGBA16F: R=Perlin-Worley (dilate), G/B/A=Worley FBM @ 増加周波数。tileable。
// per-voxel に worley 27-cell を計算するのは «高価» だが init 1 回だけなので実用 (per-frame march では
// この焼き上がりテクスチャを 1 fetch する → 速くて crisp)。
const char* kNoiseGenCS = R"(
RWTexture3D<float4> noiseOut : register(u0);
float3 h33(float3 p){
    p = float3(dot(p,float3(127.1,311.7,74.7)), dot(p,float3(269.5,183.3,246.1)), dot(p,float3(113.5,271.9,124.6)));
    return frac(sin(p)*43758.5453123);
}
// tileable gradient (Perlin) noise。freq = 整数セル数。
float gnoise(float3 x, float freq){
    float3 p=floor(x), f=frac(x);
    float3 u=f*f*(3.0-2.0*f);
    float n=0.0;
    [unroll] for(int dz=0;dz<2;dz++) [unroll] for(int dy=0;dy<2;dy++) [unroll] for(int dx=0;dx<2;dx++){
        float3 o=float3(dx,dy,dz);
        float3 g=h33(fmod(p+o,freq))*2.0-1.0;          // gradient [-1,1]
        float w=lerp(1.0-u.x,u.x,o.x)*lerp(1.0-u.y,u.y,o.y)*lerp(1.0-u.z,u.z,o.z);
        n += w*dot(g, f-o);
    }
    return n*0.5+0.5;
}
float perlinFbm(float3 p, float freq){
    return gnoise(p*freq,freq)*0.5 + gnoise(p*freq*2.0,freq*2.0)*0.25 + gnoise(p*freq*4.0,freq*4.0)*0.125
         + gnoise(p*freq*8.0,freq*8.0)*0.0625;
}
// tileable worley (cellular)。1 - 最近接点距離 → セルが明るい «もくもく» 特性。
float worley(float3 x, float freq){
    float3 p=floor(x*freq), f=frac(x*freq);
    float md=1.0;
    [unroll] for(int dz=-1;dz<=1;dz++) [unroll] for(int dy=-1;dy<=1;dy++) [unroll] for(int dx=-1;dx<=1;dx++){
        float3 o=float3(dx,dy,dz);
        float3 fp=h33(fmod(p+o+freq,freq));            // tileable wrap
        float3 d=o+fp-f;
        md=min(md, dot(d,d));
    }
    return 1.0-sqrt(saturate(md));
}
float worleyFbm(float3 p, float freq){
    return worley(p,freq)*0.625 + worley(p,freq*2.0)*0.25 + worley(p,freq*4.0)*0.125;
}
float remap(float v,float a,float b,float c,float d){ return c + (saturate((v-a)/(b-a)))*(d-c); }
[numthreads(4,4,4)]
void CSNoise(uint3 id : SV_DispatchThreadID){
    if(id.x>=128u||id.y>=128u||id.z>=128u) return;
    float3 uvw=(float3(id)+0.5)/128.0;
    float pf = perlinFbm(uvw, 4.0);                    // 低周波 Perlin
    float w0 = worleyFbm(uvw, 6.0);
    float w1 = worleyFbm(uvw, 12.0);
    float w2 = worleyFbm(uvw, 24.0);
    float pw = remap(pf, w0-1.0, 1.0, 0.0, 1.0);       // Perlin を Worley で dilate (Nubis)
    noiseOut[id]=float4(pw, w0, w1, w2);
}
)";

const char* kCloudCS = R"(
#pragma pack_matrix(row_major)
cbuffer CloudCB : register(b0) {
    float4x4 invViewProj;
    float4 camPos;     // xyz
    float4 sunDir;     // xyz (world)
    float4 sunCol;     // rgb (color*intensity)
    float4 skyCol;     // rgb ambient
    float4 params;     // x=coverage, y=density, z=wind*time
    float4 dims;       // xy = half-res dims
};
RWTexture2D<float4> cloudOut : register(u0);
Texture3D    shapeNoise         : register(t0);   // Phase 4.5: 焼いた Perlin-Worley 3D noise
SamplerState shapeNoise_sampler : register(s0);   // wrap (tileable)

float remapc(float v,float a,float b,float c,float d){ return c + saturate((v-a)/max(b-a,1e-5))*(d-c); }
float hash13(float3 p){ p=frac(p*0.1031); p+=dot(p,p.zyx+31.32); return frac((p.x+p.y)*p.z); }
// 8-tap trilinear value noise (worley 27-cell より遥かに安い → per-sample march で実用速度)。
float vnoise(float3 p){
    float3 i=floor(p), f=frac(p); f=f*f*(3.0-2.0*f);
    float n000=hash13(i+float3(0,0,0)), n100=hash13(i+float3(1,0,0));
    float n010=hash13(i+float3(0,1,0)), n110=hash13(i+float3(1,1,0));
    float n001=hash13(i+float3(0,0,1)), n101=hash13(i+float3(1,0,1));
    float n011=hash13(i+float3(0,1,1)), n111=hash13(i+float3(1,1,1));
    float nx00=lerp(n000,n100,f.x), nx10=lerp(n010,n110,f.x);
    float nx01=lerp(n001,n101,f.x), nx11=lerp(n011,n111,f.x);
    return lerp(lerp(nx00,nx10,f.y), lerp(nx01,nx11,f.y), f.z);
}
float fbm3(float3 p){ return vnoise(p)*0.5 + vnoise(p*2.03)*0.27 + vnoise(p*4.01)*0.13; }       // shape (3 oct)
float fbm2(float3 p){ return vnoise(p)*0.6 + vnoise(p*2.07)*0.3; }                                 // light march 用 (安い 2 oct)
float hg(float c,float g){ float g2=g*g; return (1.0-g2)/(12.566370*pow(max(1.0+g2-2.0*g*c,1e-3),1.5)); }

// 高度プロファイル + tile 座標 (アニメ)。
float cloudProfile(float3 p){ float hf=saturate((p.y-1.0)/1.6); return smoothstep(0.0,0.2,hf)*smoothstep(1.0,0.5,hf); }
float3 cloudUVW(float3 p){ return p*0.62 + float3(params.z*0.05, 0.0, params.z*0.035); }   // 周波数↑で雲形を細かく(crisp)
// coverage→閾値。base(Perlin-Worley) の平均は高め(~0.6)なので閾値を高域に寄せて «散らばった» 雲に。
float cloudThr(float coverage){ return 1.0 - coverage*0.55; }   // cov0.5→0.725, cov1→0.45
// shape のみの密度 (light march 用、detail 無しで安い)。Perlin-Worley を 1 fetch。
float cloudShape(float3 p, float coverage){
    float profile=cloudProfile(p); if(profile<=0.001) return 0.0;
    float4 ns = shapeNoise.SampleLevel(shapeNoise_sampler, cloudUVW(p), 0);
    float wfbm = ns.g*0.625 + ns.b*0.25 + ns.a*0.125;
    float base = remapc(ns.r, wfbm-1.0, 1.0, 0.0, 1.0);   // Perlin-Worley (Nubis dilate)
    float thr = cloudThr(coverage);
    return saturate((base - thr)/max(1.0-thr, 0.05) * 1.6)*profile;   // 境界鋭く (cloudDensity と整合)
}
// view march 用の密度 (shape + 高周波 worley detail 侵食 → crisp billowy)。
float cloudDensity(float3 p, float coverage){
    float profile=cloudProfile(p); if(profile<=0.001) return 0.0;
    float3 uvw=cloudUVW(p);
    float4 ns = shapeNoise.SampleLevel(shapeNoise_sampler, uvw, 0);
    float wfbm = ns.g*0.625 + ns.b*0.25 + ns.a*0.125;
    float base = remapc(ns.r, wfbm-1.0, 1.0, 0.0, 1.0);
    float thr = cloudThr(coverage);
    float shape = saturate((base - thr)/max(1.0-thr, 0.05) * 1.6)*profile;       // ×1.6 で境界を鋭く (crisp)
    if(shape<=0.0) return 0.0;
    float4 nd = shapeNoise.SampleLevel(shapeNoise_sampler, uvw*4.0 + 9.7, 0);    // 高周波 worley で細かい縁を削る
    float detail = nd.b*0.5 + nd.a*0.5;                                          // 高域 worley = fine cauliflower edge
    return saturate(remapc(shape, detail*0.85, 1.0, 0.0, 1.0));                  // 縁を強めに削り出す → crisp billowy
}

[numthreads(8,8,1)]
void CSCloud(uint3 tid : SV_DispatchThreadID){
    uint W=(uint)dims.x, H=(uint)dims.y;
    if(tid.x>=W || tid.y>=H) return;
    float2 uv=(float2(tid.xy)+0.5)/dims.xy;
    float4 clip=float4(uv.x*2-1, -(uv.y*2-1), 1, 1);
    float4 wp=mul(clip, invViewProj); wp/=wp.w;
    float3 dir=normalize(wp.xyz - camPos.xyz);
    if(dir.y < 0.03){ cloudOut[tid.xy]=float4(0,0,0,0); return; }
    float coverage=params.x, density=params.y;
    float t0=1.0/dir.y, t1=2.6/dir.y;     // 雲スラブ [1, 2.6] (dome space)
    const int N=28; float dt=(t1-t0)/float(N);
    float jit=hash13(float3(uv*dims.xy, 0.0));
    float3 sun=normalize(sunDir.xyz);
    float cosA=dot(dir,sun);
    float phase=max(hg(cosA,0.75), hg(cosA,-0.25)*0.6);
    float hFade=smoothstep(0.03,0.12,dir.y);
    float transmit=1.0; float3 scatter=float3(0,0,0);
    [loop] for(int i=0;i<N;i++){
        float3 p=dir*(t0+dt*(float(i)+jit));
        float dens=cloudDensity(p,coverage)*density;
        if(dens>0.01){
            float lt=0.0;
            [unroll] for(int l=1;l<=4;l++) lt += cloudShape(p+sun*(0.13*float(l)), coverage);   // shape のみで安く
            float lightT=exp(-lt*density*1.4);                       // 自己影を強め深部を暗く → 立体コントラスト
            float powder=1.0 - exp(-dens*3.0);                       // powder 効果を強め縁を締める
            float3 sunL=sunCol.rgb * lightT * phase * (powder*0.9+0.1);
            float3 ambL=skyCol.rgb * (0.22 + 0.45*saturate(p.y/2.6));// 環境光フィルを抑え影を残す
            float a=1.0 - exp(-dens*dt*6.0);
            scatter += transmit * a * (sunL+ambL);
            transmit *= (1.0-a);
            if(transmit<0.02) break;
        }
    }
    float baseA = 1.0 - transmit;
    float3 col = baseA>1e-4 ? scatter/baseA : float3(0,0,0);   // 非premult (AlphaBlend 用)
    cloudOut[tid.xy]=float4(col, baseA*hFade);
}
)";

// 雲合成: 全画面三角形で cloudTex を AlphaBlend 合成 (sky の上に)。
const char* kCloudCompVS = R"(
struct VSOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; };
VSOut VSMain(uint id:SV_VertexID){
    float2 uv=float2((id<<1)&2, id&2);
    VSOut o; o.uv=uv; o.pos=float4(uv.x*2.0-1.0, -(uv.y*2.0-1.0), 0.0, 1.0); return o;
}
)";
const char* kCloudCompPS = R"(
Texture2D cloudTex : register(t0);
SamplerState cloudTex_sampler : register(s0);
struct VSOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; };
float4 PSMain(VSOut v):SV_TARGET { return cloudTex.Sample(cloudTex_sampler, v.uv); }
)";

struct CloudCB {
    FMat4 invViewProj;
    FVec4 camPos;
    FVec4 sunDir;
    FVec4 sunCol;
    FVec4 skyCol;
    FVec4 params;
    FVec4 dims;
};

} // namespace

TResult<void> FVolumetricClouds::Init(IRhiDevice& device, EFormat hdr_format) noexcept {
    m_HdrFormat = hdr_format;
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Compute; sd.hlsl_source = kCloudCS;
        sd.entry_point = "CSCloud"; sd.debug_name = "Clouds.CS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_CloudCs = Move(r.Value()); }
    {   FComputePipelineDesc pd{}; pd.cs = m_CloudCs.Get();
        pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "CloudCB";
        pd.uav_slots = 1; pd.uav_names[0] = "cloudOut";
        pd.srv_slots = 1; pd.srv_names[0] = "shapeNoise";   // Phase 4.5: 焼いた 3D noise を sample
        pd.static_sampler_count = 1;
        pd.static_samplers[0].filter = ESamplerFilter::Linear;
        pd.static_samplers[0].address_u = ESamplerAddress::Wrap;   // tileable noise
        pd.static_samplers[0].address_v = ESamplerAddress::Wrap;
        pd.static_samplers[0].address_w = ESamplerAddress::Wrap;
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_CloudPipe = Move(r.Value()); }
    // Phase 4.5: Perlin-Worley 生成 compute + 128^3 shape テクスチャ。
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Compute; sd.hlsl_source = kNoiseGenCS;
        sd.entry_point = "CSNoise"; sd.debug_name = "Clouds.NoiseCS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_NoiseCs = Move(r.Value()); }
    {   FComputePipelineDesc pd{}; pd.cs = m_NoiseCs.Get();
        pd.uav_slots = 1; pd.uav_names[0] = "noiseOut";
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_NoisePipe = Move(r.Value()); }
    {   FTextureDesc td{}; td.width = 128; td.height = 128; td.depth = 128;
        td.format = EFormat::R16G16B16A16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td); if (r.IsErr()) return Err<void>(r.Error()); m_ShapeTex = Move(r.Value()); }
    {   FShaderDesc vd{}; vd.stage = EShaderStage::Vertex; vd.hlsl_source = kCloudCompVS;
        vd.entry_point = "VSMain"; vd.debug_name = "Clouds.CompVS";
        auto r = CreateRhiShader(device, vd); if (r.IsErr()) return Err<void>(r.Error()); m_CompVs = Move(r.Value()); }
    {   FShaderDesc pd{}; pd.stage = EShaderStage::Pixel; pd.hlsl_source = kCloudCompPS;
        pd.entry_point = "PSMain"; pd.debug_name = "Clouds.CompPS";
        auto r = CreateRhiShader(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_CompPs = Move(r.Value()); }
    {   FPipelineDesc pd{};
        pd.vs = m_CompVs.Get(); pd.ps = m_CompPs.Get();
        pd.topology = EPrimitiveTopology::TriangleList;
        pd.rt_format = hdr_format; pd.depth_format = EFormat::Unknown;
        pd.depth_test = false; pd.depth_write = false;
        pd.cull_mode = ECullMode::None; pd.blend_mode = EBlendMode::AlphaBlend;
        pd.cbuffer_slots = 0; pd.texture_slots = 1; pd.texture_names[0] = "cloudTex";
        pd.static_sampler_count = 1;
        pd.static_samplers[0].filter = ESamplerFilter::Linear;
        pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
        pd.layout_count = 0; pd.vertex_stride = 0;
        auto r = CreateRhiPipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_CompPipe = Move(r.Value()); }
    {   FBufferDesc bd{}; bd.size = 256; bd.usage = EBufferUsage::Uniform; bd.cpu_writable = true;
        auto r = CreateRhiBuffer(device, bd); if (r.IsErr()) return Err<void>(r.Error()); m_Cb = Move(r.Value()); }
    m_Ready = true;
    return Ok();
}

bool FVolumetricClouds::EnsureSize(IRhiDevice& device, u32 scW, u32 scH) noexcept {
    if (!m_Ready) return false;
    const u32 hw = scW > 1 ? scW : 1;   // Phase 4.5: full-res で crisp に (half-res は upscale で柔らかい)
    const u32 hh = scH > 1 ? scH : 1;
    if (m_CloudTex && m_W == hw && m_H == hh) return true;
    FTextureDesc td{}; td.width = hw; td.height = hh;
    td.format = EFormat::R16G16B16A16_Float; td.is_uav = true;
    auto r = CreateRhiTexture(device, td); if (r.IsErr()) { m_CloudTex.Reset(); m_W = 0; return false; }
    m_CloudTex = Move(r.Value()); m_W = hw; m_H = hh;
    return true;
}

void FVolumetricClouds::RenderCompute(IRhiCommandList& cl, const FMat4& inv_view_proj, FVec3 cam_pos,
                                      FVec3 sun_dir, FVec3 sun_color, FVec3 sky_color,
                                      f32 coverage, f32 density, f32 wind, f32 time) noexcept {
    if (!m_Ready || !m_CloudTex || !m_Cb) return;
    CloudCB cb{};
    cb.invViewProj = inv_view_proj;
    cb.camPos = FVec4{ cam_pos.x, cam_pos.y, cam_pos.z, 0.0f };
    cb.sunDir = FVec4{ sun_dir.x, sun_dir.y, sun_dir.z, 0.0f };
    cb.sunCol = FVec4{ sun_color.x, sun_color.y, sun_color.z, 0.0f };
    cb.skyCol = FVec4{ sky_color.x, sky_color.y, sky_color.z, 0.0f };
    cb.params = FVec4{ coverage, density, time * wind * 0.03f, 0.0f };
    cb.dims   = FVec4{ static_cast<f32>(m_W), static_cast<f32>(m_H), 0.0f, 0.0f };
    m_Cb->Update(&cb, sizeof(cb));
    // Phase 4.5: 初回に Perlin-Worley shape noise (128^3) を焼く (1 回のみ、以降 SRV で sample)。
    if (!m_NoiseBaked && m_NoisePipe && m_ShapeTex) {
        cl.SetComputePipeline(*m_NoisePipe);
        cl.BindUav(0, *m_ShapeTex);
        cl.Dispatch(32, 32, 32);   // 128/4
        m_NoiseBaked = true;
    }
    cl.SetComputePipeline(*m_CloudPipe);
    cl.SetConstantBuffer(0, *m_Cb);
    if (m_ShapeTex) cl.SetTexture(0, *m_ShapeTex);   // shape noise SRV (UAV→SRV は Dispatch の TRANSITION commit)
    cl.BindUav(0, *m_CloudTex);
    cl.Dispatch((m_W + 7) / 8, (m_H + 7) / 8, 1);
}

void FVolumetricClouds::Composite(IRhiCommandList& cl, u32 scW, u32 scH) noexcept {
    if (!m_Ready || !m_CloudTex || !m_CompPipe) return;
    FViewport vp{}; vp.width = static_cast<f32>(scW); vp.height = static_cast<f32>(scH); cl.SetViewport(vp);
    FScissorRect sr{}; sr.right = static_cast<i32>(scW); sr.bottom = static_cast<i32>(scH); cl.SetScissor(sr);
    cl.SetPipeline(*m_CompPipe);
    cl.SetTexture(0, *m_CloudTex);   // UAV→SRV 遷移は CommitShaderResources(TRANSITION) が処理
    cl.Draw(3, 0);
}

void FVolumetricClouds::Shutdown() noexcept {
    m_ShapeTex.Reset(); m_NoisePipe.Reset(); m_NoiseCs.Reset(); m_NoiseBaked = false;
    m_CloudTex.Reset(); m_Cb.Reset(); m_CompPipe.Reset(); m_CompPs.Reset(); m_CompVs.Reset();
    m_CloudPipe.Reset(); m_CloudCs.Reset(); m_Ready = false; m_W = 0; m_H = 0;
}

} // namespace acs
