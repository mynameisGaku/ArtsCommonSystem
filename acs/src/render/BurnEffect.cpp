// SPDX-License-Identifier: Apache-2.0
// FBurnEffect 実装 (per-pixel 燃えディゾルブ)
#include "render/BurnEffect.h"
#include "foundation/Move.h"

namespace acs {

namespace {

// VS: rect (px) から 2 三角形のクアッドを生成 (VB 不要、SV_VertexID)。
// PS: uv の手続き FBM ノイズ + 対角グラデで燃え際を計算し、燃え尽きた画素は discard。
const char* kBurnHLSL = R"(
cbuffer Burn : register(b0) {
    float4 rect;     // x,y=左上(px), z=w, w=h
    float4 screen;   // x=1/screen_w, y=1/screen_h, z=progress, w=edge
    float4 c_ember;  // xyz=ember 色, w=time
    float4 c_paper;  // xyz=paper 色, w=freq
    float4 misc;     // x=cells (0=なめらか最高解像度, N=N分割のドット調)
};

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    // (0,0)(1,0)(0,1) / (0,1)(1,0)(1,1) の 2 三角形
    float2 quad[6] = {
        float2(0,0), float2(1,0), float2(0,1),
        float2(0,1), float2(1,0), float2(1,1)
    };
    float2 c  = quad[id];
    float2 px = rect.xy + c * rect.zw;                 // スクリーン px
    float2 nd = px * float2(screen.x, screen.y) * 2.0 - 1.0;
    VSOut o;
    o.uv  = c;
    o.pos = float4(nd.x, -nd.y, 0.0, 1.0);             // 左上原点に合わせて y 反転
    return o;
}

float Hash(float2 p) {
    // 整数ビットミックス (lowbias32 系)。frac(p.x*p.y) 系は軸/格子状の模様が出るため使わない。
    // VNoise から整数グリッド座標で呼ばれる前提。
    uint h = (uint)(int)p.x * 0x9E3779B1u + (uint)(int)p.y * 0x85EBCA77u;
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return float(h & 0xFFFFu) / 65535.0;
}
float VNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = Hash(i);
    float b = Hash(i + float2(1, 0));
    float c = Hash(i + float2(0, 1));
    float d = Hash(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}
float Fbm(float2 p) {
    float s = 0.0, a = 0.5;
    const float2x2 m = float2x2(0.80, -0.60, 0.60, 0.80);
    [unroll]
    for (int i = 0; i < 5; ++i) { s += a * VNoise(p); p = mul(m, p) * 2.0; a *= 0.5; }
    return s;
}

float4 PSMain(VSOut v) : SV_TARGET {
    float  freq  = c_paper.w;
    float  t0    = c_ember.w;
    float  cells = misc.x;
    // cells>=1 なら uv をセル中心へ量子化してドット調に。0 ならピクセル単位 (最高解像度)。
    float2 uv    = (cells >= 1.0) ? (floor(v.uv * cells) + 0.5) / cells : v.uv;
    // ドメインワープ: 座標自体をノイズで歪ませてから FBM を引く。
    float2 warp  = float2(Fbm(uv * 2.5 + 19.7), Fbm(uv * 2.5 + 4.3)) - 0.5;
    float2 wuv   = uv + warp * 0.5;
    // 時間で僅かに揺らして炎をちらつかせる
    float2 q     = wuv * freq + float2(t0 * 0.15, -t0 * 0.1);
    // 2 スケールのノイズを混ぜて縁をフラクタルにし、単一の前線(=境界線)を作らない
    float  n     = Fbm(q) * 0.7 + Fbm(q * 2.7 + 8.3) * 0.3;
    // 延焼方向はごく弱く (0.12) だけ残す。強い対角グラデは「一本の境界線」に見えるため
    // ほぼノイズ駆動 (あちこちに穴が空いて広がる dissolve) にする。
    float  grad  = (wuv.x + wuv.y) * 0.5;
    float  t     = 0.88 * n + 0.12 * grad;

    float p       = screen.z;
    float emberW  = max(screen.w, 1e-3);               // 炎(白熱)の帯
    float charW   = emberW * 0.55;                     // 炎の内側の黒コゲ (透明へ抜ける)
    float scorchW = emberW * 1.10;                     // 炎の外側の焦げ茶 (延焼前の紙)

    // t が小さい = もう燃えた。t<p-charW は完全な穴 (下が見える)。
    if (t < p - charW) discard;

    const float3 hot   = float3(1.6, 1.25, 0.6);       // 白熱 (1 超で Diligent では bloom)
    const float3 orange= c_ember.xyz;                  // 橙 (既定の ember)
    const float3 soot  = float3(0.05, 0.03, 0.03);     // 黒コゲ
    const float3 brown = float3(0.30, 0.15, 0.07);     // 焦げ茶

    float3 col; float a = 1.0;
    if (t < p) {
        // 炎の内側の黒コゲ。穴へ向かって alpha を 0 に落とし、煤を残しつつ透明に抜ける。
        float k = (t - (p - charW)) / charW;           // 0=穴際 .. 1=炎際
        col = lerp(soot, orange * 0.5, k * 0.4);
        a   = k;
    } else if (t < p + emberW) {
        // 炎の縁: 白熱 → 橙 → 黒コゲ
        float k = (t - p) / emberW;
        col = (k < 0.4) ? lerp(hot, orange, k / 0.4)
                        : lerp(orange, soot, (k - 0.4) / 0.6);
    } else if (t < p + emberW + scorchW) {
        // 延焼前の焦げ茶 → 紙 (紙が燃える直前に茶色く焦げる)
        float k = (t - p - emberW) / scorchW;
        col = lerp(brown, c_paper.xyz, k * k);
    } else {
        col = c_paper.xyz;                             // 無傷の紙
    }
    return float4(col, a);
}
)";

/** CB レイアウト (HLSL の cbuffer Burn と一致)。 */
struct BurnCB {
    FVec4 rect;
    FVec4 screen;
    FVec4 ember;
    FVec4 paper;
    FVec4 misc;     // x=cells
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

ACS_FORCEINLINE f32 Saturate01(f32 v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

} // namespace

/** VS/PS/CB/PSO を生成する。 */
TResult<void> FBurnEffect::Init(IRhiDevice& device, EFormat rt_format) noexcept {
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kBurnHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "FBurnEffect.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    m_Vs = Move(vs_r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kBurnHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "FBurnEffect.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    m_Ps = Move(ps_r.Value());

    for (u32 i = 0; i < kRing; ++i) {
        FBufferDesc cbd{};
        cbd.size = CBSize<BurnCB>();
        cbd.usage = EBufferUsage::Uniform;
        cbd.cpu_writable = true;
        auto cb_r = CreateRhiBuffer(device, cbd);
        if (cb_r.IsErr()) return Err<void>(cb_r.Error());
        m_Cb[i] = Move(cb_r.Value());
    }
    m_CbIdx = 0;

    FPipelineDesc pd{};
    pd.vs = m_Vs.Get();
    pd.ps = m_Ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = EFormat::Unknown;     // easy は深度なし
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::AlphaBlend;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 0;
    pd.cbuffer_names[0] = "Burn";
    pd.layout_count  = 0;
    pd.vertex_stride = 0;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    m_Pipeline = Move(pl_r.Value());

    return Ok();
}

/** GPU リソースを解放する。 */
void FBurnEffect::Shutdown() noexcept {
    m_Pipeline.Reset();
    for (u32 i = 0; i < kRing; ++i) m_Cb[i].Reset();
    m_Ps.Reset();
    m_Vs.Reset();
}

/** 定数バッファを更新し、クアッドを 1 回描く。 */
void FBurnEffect::Draw(IRhiCommandList& cl, f32 x, f32 y, f32 w, f32 h,
                       f32 screen_w, f32 screen_h, const BurnParams& p) noexcept {
    if (!m_Pipeline || !m_Cb[0]) return;
    const f32 sw = screen_w > 0.0f ? screen_w : 1.0f;
    const f32 sh = screen_h > 0.0f ? screen_h : 1.0f;
    BurnCB cb{};
    cb.rect   = FVec4{ x, y, w, h };
    cb.screen = FVec4{ 1.0f / sw, 1.0f / sh, Saturate01(p.progress), p.edge };
    cb.ember  = FVec4{ p.ember.x, p.ember.y, p.ember.z, p.time };
    cb.paper  = FVec4{ p.paper.x, p.paper.y, p.paper.z, p.freq };
    cb.misc   = FVec4{ p.cells, 0.0f, 0.0f, 0.0f };

    // 1 フレームで複数回描いても前の draw が読む CB を上書きしないよう、リングを進める。
    m_CbIdx = (m_CbIdx + 1u) % kRing;
    IRhiBuffer* cb_buf = m_Cb[m_CbIdx].Get();
    cb_buf->Update(&cb, sizeof(cb));

    cl.SetPipeline(*m_Pipeline);
    cl.SetConstantBuffer(0, *cb_buf);
    cl.Draw(6);    // VB 無し、SV_VertexID で 6 頂点 (2 三角形)
}

} // namespace acs
