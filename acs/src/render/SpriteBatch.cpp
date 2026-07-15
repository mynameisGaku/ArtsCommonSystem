// SPDX-License-Identifier: Apache-2.0
// 2D スプライト描画ヘルパ実装
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "memory/Memory.h"
#include "memory/Allocator.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <cstring>
#include <cmath>

namespace acs {

namespace {

// HLSL: ピクセル座標 → NDC 変換（cb で screen サイズを受け取る）
const char* kSpriteHLSL = R"(
cbuffer Screen : register(b0) {
    float4 inv_screen;   // (1/w, 1/h, w, h)
    float4 view;         // (cam_x, cam_y, zoom, _)
};

struct VSIn {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR;
};
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR;
};

VSOut VSMain(VSIn v) {
    VSOut o;
    // ワールド座標にカメラ（中心 view.xy、ズーム view.z）を適用 → ピクセル → NDC
    float2 screen_half = float2(inv_screen.z, inv_screen.w) * 0.5;
    float2 sp = (v.pos - view.xy) * view.z + screen_half;
    float2 ndc;
    ndc.x = sp.x * inv_screen.x * 2.0 - 1.0;
    ndc.y = 1.0 - sp.y * inv_screen.y * 2.0;
    o.pos = float4(ndc, 0.0, 1.0);
    o.uv  = v.uv;
    o.col = v.col;
    return o;
}

Texture2D    atlas : register(t0);
// 命名規約: <texture>m_Sampler
SamplerState atlas_sampler : register(s0);

float4 PSMain(VSOut v) : SV_TARGET {
    return atlas.Sample(atlas_sampler, v.uv) * v.col;
}
)";

// HLSL: ピクセル効果プリセット (マテリアルアセットの基本シェーダ)。
// VS は通常スプライトと共通 (m_Vs を再利用) なので、ここでは PSEffect だけを使う。
// cbuffer Effect(b1) の effect_id で分岐し、1 つの PSO で全プリセットを賄う。
const char* kSpriteEffectHLSL = R"(
cbuffer Screen : register(b0) {
    float4 inv_screen;
    float4 view;
};
cbuffer Effect : register(b1) {
    float4 fx_a;     // x=effect_id, y=strength, z=p0, w=p1
    float4 fx_b;     // x=p2, y=time, z=_, w=_
    float4 fx_color; // 染め色 / 縁色
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR;
};

Texture2D    atlas : register(t0);
SamplerState atlas_sampler : register(s0);

// 輝度軸まわりの色相回転 (Rodrigues 回転、おおむね輝度を保つ)。
float3 HueShiftRGB(float3 c, float angle) {
    const float3 k = float3(0.57735027, 0.57735027, 0.57735027); // (1,1,1)/sqrt(3)
    float ca = cos(angle);
    float sa = sin(angle);
    return c * ca + cross(k, c) * sa + k * dot(k, c) * (1.0 - ca);
}

float4 PSEffect(VSOut v) : SV_TARGET {
    int   id  = (int)fx_a.x;
    float str = fx_a.y;
    float p0  = fx_a.z;
    float p1  = fx_a.w;
    float t   = fx_b.y;

    // --- UV 歪み系 (サンプル前) ---
    float2 uv = v.uv;
    if (id == 4) {                       // Wave: str=振幅, p0=周波数, p1=速度
        uv.x += sin(uv.y * p0 + t * p1) * str;
        uv.y += cos(uv.x * p0 + t * p1) * str * 0.5;
    } else if (id == 5) {                // Pixelate: p0=セル数X, p1=セル数Y
        float2 cells = float2(max(p0, 1.0), max(p1, 1.0));
        uv = (floor(uv * cells) + 0.5) / cells;
    }

    float4 base;
    if (id == 12) {                      // Chromatic: R/B を逆方向にずらしてサンプル (色収差)
        float o   = p0;
        float4 sr = atlas.Sample(atlas_sampler, uv + float2(o, 0.0));
        float4 cg = atlas.Sample(atlas_sampler, uv);
        float4 sb = atlas.Sample(atlas_sampler, uv - float2(o, 0.0));
        // 透明な隣接タップ(縁)では中心チャンネルへフォールバックし、暗色フリンジを防ぐ。
        float r   = lerp(cg.r, sr.r, sr.a);
        float b   = lerp(cg.b, sb.b, sb.a);
        float4 ab = float4(r, cg.g, b, cg.a) * v.col;
        base = lerp(cg * v.col, ab, saturate(str));
    } else {
        base = atlas.Sample(atlas_sampler, uv) * v.col;
    }

    // --- 色系 (サンプル後) ---
    if (id == 1) {                       // Grayscale
        float g = dot(base.rgb, float3(0.299, 0.587, 0.114));
        base.rgb = lerp(base.rgb, float3(g, g, g), saturate(str));
    } else if (id == 2) {                // Tint
        base.rgb = lerp(base.rgb, base.rgb * fx_color.rgb, saturate(str));
    } else if (id == 3) {                // Vignette: str=強さ, p0=開始半径, p1=終了半径, color=縁色
        float r   = length(v.uv - 0.5) * 1.41421356;       // 0=中心 .. 1=隅
        float vig = smoothstep(p0, max(p1, p0 + 0.001), r); // 0=内側 .. 1=外周
        base.rgb  = lerp(base.rgb, fx_color.rgb, saturate(str) * vig);
    } else if (id == 6) {                // HueShift: str=角度°, p0=回転速度°/s
        base.rgb = HueShiftRGB(base.rgb, radians(str + p0 * t));
    } else if (id == 7) {                // Brightness/Contrast: str=明るさ, p0=コントラスト
        base.rgb = (base.rgb - 0.5) * max(p0, 0.0) + 0.5;
        base.rgb = saturate(base.rgb * str);   // コントラスト>1 の暗ピクセルで負値→背景減算を防ぐ
    } else if (id == 8) {                // Invert: 色反転
        base.rgb = lerp(base.rgb, 1.0 - base.rgb, saturate(str));
    } else if (id == 9) {                // Sepia: セピア調
        float3 sep = float3(dot(base.rgb, float3(0.393, 0.769, 0.189)),
                            dot(base.rgb, float3(0.349, 0.686, 0.168)),
                            dot(base.rgb, float3(0.272, 0.534, 0.131)));
        base.rgb = lerp(base.rgb, sep, saturate(str));
    } else if (id == 10) {               // Posterize: str=度合い, p0=階調数
        float lv = max(p0, 2.0);
        float3 q = floor(base.rgb * lv + 0.5) / lv;
        base.rgb = lerp(base.rgb, q, saturate(str));
    } else if (id == 11) {               // Scanline: str=濃さ, p0=線の本数
        float s = 0.5 + 0.5 * sin(v.uv.y * p0 * 6.2831853);
        base.rgb *= 1.0 - saturate(str) * s;
    }
    return base;
}
)";

// HLSL: lit (PBR metallic-roughness) スプライト。アルベド(t0) + 法線マップ(t1) を
// 2D 点光源 (b2) で Cook-Torrance BRDF 陰影付けする。VS は world px を PS へ渡す。
const char* kLitSpriteHLSL = R"(
cbuffer Screen : register(b0) {
    float4 inv_screen;   // (1/w, 1/h, w, h)
    float4 view;         // (cam_x, cam_y, zoom, _)
};
cbuffer LitMaterial : register(b1) {
    float4 base_color;   // rgb=tint, a=opacity
    float4 mat0;         // x=metallic, y=roughness, z=normalStrength, w=ao
    float4 emissive;     // rgb=emissive*strength, w=自己影オクルーダー番号 (-1=無し)
    float4 toon0;        // x=mode(0=PBR,1=Toon), y=rimPower, z=specThreshold, w=softness
    float4 shadow1;      // rgb=1影色, w=1影しきい値
    float4 shadow2;      // rgb=2影色, w=2影しきい値
    float4 toon_rim;     // rgb=リム色, w=自分より下(先描画)のキャスターのスキップマスク (bit j=occluder j)
    float4 toon_spec;    // rgb=トゥーンスペキュラ色
    float4 adv0;         // x=clearcoat, y=clearcoatRoughness, z=anisotropy, w=specularLevel
    float4 adv1;         // x=specularTint, y=sheen, z=sheenRoughness, w=subsurface
    float4 sheen_col;    // rgb=シーン色
    float4 sss_col;      // rgb=サブサーフェス色
};
cbuffer Lights : register(b2) {
    float4 light_info;        // x=count, y=height, z=occluder count
    float4 ambient;           // rgb=ambient
    float4 light_pos[16];     // xy=pos(px), z=radius, w=intensity
    float4 light_col[16];     // rgb=color, w=type (0=Point,1=Spot,2=Directional)
    float4 light_dir[16];     // xy=dir(正規化), z=coneInner(cos), w=coneOuter(cos)
    float4 occ[16];           // xy=center(px), z=radius(外接), w=shape(0=円,1=箱,2=多角形)
    float4 occ2[16];          // xy=box halfExtents, z=rotation(rad), w=多角形の頂点数
    float4 occ_poly[256];     // 多角形オクルーダーの頂点 (occluder j: occ_poly[j*16..j*15]=32頂点, 1 float4=2頂点)
};

struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR; float2 wpos : TEXCOORD1; };

VSOut VSLit(VSIn v) {
    VSOut o;
    float2 screen_half = float2(inv_screen.z, inv_screen.w) * 0.5;
    float2 sp = (v.pos - view.xy) * view.z + screen_half;
    float2 ndc;
    ndc.x = sp.x * inv_screen.x * 2.0 - 1.0;
    ndc.y = 1.0 - sp.y * inv_screen.y * 2.0;
    o.pos  = float4(ndc, 0.0, 1.0);
    o.uv   = v.uv;
    o.col  = v.col;
    o.wpos = v.pos;          // world px (ライト計算用)
    return o;
}

Texture2D    atlas         : register(t0);   // アルベド
Texture2D    normal_tex    : register(t1);   // 法線マップ (未指定は平面 1x1)
SamplerState atlas_sampler : register(s0);

static const float PI = 3.14159265;

// 箱の符号付き距離 (中心 C, 回転 rot, 半サイズ he のローカル系で評価)。負=内側。
float BoxSdf(float2 p, float2 C, float rot, float2 he) {
    float2 lp = p - C;
    float  cs = cos(-rot), sn = sin(-rot);
    float2 r  = float2(lp.x * cs - lp.y * sn, lp.x * sn + lp.y * cs);
    float2 q  = abs(r) - he;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
}

// occluder j の頂点 k (0..31) を取り出す。occ_poly は occluder ごとに 16 float4 (32頂点)、1 float4=2頂点。
float2 OccVert(int j, int k) {
    float4 f = occ_poly[j * 16 + (k >> 1)];
    return (k & 1) ? f.zw : f.xy;
}
// 点 p から線分 [a,b] への距離。
float DistPtSeg(float2 p, float2 a, float2 b) {
    float2 ab = b - a, ap = p - a;
    float  t  = clamp(dot(ap, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
    return length(ap - ab * t);
}
float Cross2(float2 a, float2 b) { return a.x * b.y - a.y * b.x; }
// 2線分 [p1,p2],[p3,p4] が «真に» 交差するか (向き付き面積の符号)。
bool SegSegCross(float2 p1, float2 p2, float2 p3, float2 p4) {
    float d1 = Cross2(p4 - p3, p1 - p3);
    float d2 = Cross2(p4 - p3, p2 - p3);
    float d3 = Cross2(p2 - p1, p3 - p1);
    float d4 = Cross2(p2 - p1, p4 - p1);
    return (d1 * d2 < 0.0) && (d3 * d4 < 0.0);
}
// 多角形オクルーダー j の «遮蔽» 評価 (輪郭エッジ交差 + 非ゼロ巻き数則)。
// umbra: 線分 [P,L] が輪郭のどれかの辺を横切る (外側 w=0 から横切れば必ず w≠0 の充填領域へ
//        入る) か、P 自身の巻き数が非ゼロ (P がポリゴン内部)。三角形ファンと違い凹み (星の
//        腕の間・王冠の谷間) を埋めず、自己交差 (五芒星) も nonzero 充填で中央まで遮蔽する。
// penumbra: «影境界レイ» (光源→頂点の延長線、頂点より先 s>0) までの距離。頂点までの距離だと
//        光線が頂点の近くを通過しただけで減光し、光が当たる面の縁にスジ状の偽影が張り付く。
//        帯幅は頂点からの距離 s に «純粋比例» で広げる (= 境界レイまわりの一定角度。面光源の
//        penumbra は距離に比例して広がる)。上限を付けると頭打ち地点で見かけの境界が折れ曲がる
//        ため付けない。返す距離は d*r/s に正規化する (r=オクルーダー外接半径)。
// inside_umbra: P がオクルーダー内部のときの扱い。false (既定) = «影を落とさない» (1e9)。
//        重なったスプライトは互いの内部になるため、内部=umbra だと面同士が塗り潰される。
//        true は自己影 (m_SelfShadow) 用で、内部 = umbra (自分の形に包まれて暗くなる)。
float SegPolyDist(float2 P, float2 L, int j, int n, float r, bool inside_umbra) {
    if (n < 3) return 1e9;
    bool crossed = false;
    int  wind = 0;
    int  pv   = n - 1;
    [loop] for (int i = 0; i < n; ++i) {
        float2 a = OccVert(j, pv), b = OccVert(j, i);
        crossed = crossed || SegSegCross(P, L, a, b);  // 境界横断 = umbra (内部判定の後に適用)
        if ((a.y <= P.y) != (b.y <= P.y)) {            // P からの水平レイで巻き数を数える
            float x = a.x + (P.y - a.y) * (b.x - a.x) / (b.y - a.y);
            if (x > P.x) wind += (b.y > a.y) ? 1 : -1;
        }
        pv = i;
    }
    if (wind != 0) return inside_umbra ? 0.0 : 1e9;  // P が内部 = 重なり (影なし) or 自己影 (umbra)
    if (crossed) return 0.0;                         // 境界横断 = umbra
    float best = 1e9;
    [loop] for (int k = 0; k < n; ++k) {
        float2 q  = OccVert(j, k);
        float2 u  = q - L;
        float  ul = max(length(u), 1e-3);
        float2 ud = u / ul;
        float2 w  = P - q;
        float  s  = dot(w, ud);                      // 境界レイに沿った頂点からの距離
        if (s <= 0.0) continue;                      // 頂点より光源側に影境界は無い
        float  d  = length(w - ud * s);              // 境界レイへの垂直距離
        best = min(best, d * max(r, 1.0) / s);
    }
    return best;
}

// オクルーダー (円/箱/多角形) によるソフト影。影は «オクルーダーが画素 P と光源 Lp の間にある»
// 場合のみ落ちる (片側)。多角形=線分貫通(umbra)+影境界レイ距離(penumbra)、円=光源視点レイへの垂直
// 距離+奥行ゲート、箱=最近点 SDF+向きゲート。いずれも «画素の光源側» (オクルーダーが画素の後方)
// では影ゼロ → 輪郭まわりの誤った影 (光源側ハロー) が出ない。selfOcc は «自分自身» のオクルーダー
// 番号で、自スプライトを自己影しないようスキップする (距離ベース自己フェードは «球の外周=影要る»
// と «球自身=影要らない» が同距離域で矛盾し明るいリング/直線エッジを生むため、番号スキップが正)。
// 画素がオクルーダーの «内部» にある場合、そのオクルーダーは影を落とさない (重なったスプライトが
// 互いの面を塗り潰さないように)。例外は selfShadowJ (m_SelfShadow=1 の自分自身): 内部 = umbra。
// skipMask は «自分より下 (先描画) のキャスター» のビット集合: 影の上下関係は描画順で決まり、
// 影は上の物体から下の物体・地面にのみ落ちる。下から上の面に影が乗る物理は無く、凹形状の相手と
// 重なった際のまだら影も原理的に出ない。1=遮蔽なし。
float ShadowVisibility(float2 P, float2 Lp, int occ_count, int selfOcc, int selfShadowJ, int skipMask) {
    float vis = 1.0;
    [loop] for (int j = 0; j < occ_count; ++j) {
        if (j == selfOcc) continue;                      // 自分自身は影を落とさない
        if (((skipMask >> j) & 1) != 0) continue;        // 自分より下のキャスターは影を落とさない
        bool   inside_umbra = (j == selfShadowJ);        // 自己影 ON の自分だけ «内部=umbra»
        float2 C     = occ[j].xy;
        float  d_len = max(length(Lp - P), 1e-3);        // ガード: dir は常に有限
        float2 dir   = (Lp - P) / d_len;
        float  t     = dot(C - P, dir);
        float2 cp    = P + dir * clamp(t, 0.0, d_len);   // 線分上の最近点 (光源側へはみ出さない)
        float  occv;
        int    shape = (int)occ[j].w;
        if (shape == 1) {                                // 箱 (通常 CPU は4頂点ポリゴンで出すため後方互換用)
            float2 he = occ2[j].xy;
            if (he.x <= 0.0 && he.y <= 0.0) continue;    // ゼロ面積の箱は遮蔽なし
            float  rot = occ2[j].z;
            if (!inside_umbra && BoxSdf(P, C, rot, he) <= 0.0) continue;   // P が箱内部 = 重なり → 影なし
            float  pen = max((he.x + he.y) * 0.25, 5.0);
            float  gate= smoothstep(-pen, pen, t);       // 中心が画素の後方(光源側)なら影を消す
            occv = (1.0 - smoothstep(-pen, pen, BoxSdf(cp, C, rot, he))) * gate;
        } else if (shape == 2) {                         // 多角形 (三角形/ポリゴン)
            int   nv  = min((int)occ2[j].w, 32);         // occ_poly のバジェット (32頂点) を超えない
            if (nv < 3) continue;                        // 退化ポリゴンは遮蔽なし
            float pen = max(occ[j].z * 0.22, 4.0);
            // 線分 [P, 光源] が多角形を貫けば 0 → 完全遮蔽 (umbra)。外れれば «影境界レイ»
            // までの距離で penumbra。光源側の画素は影ゼロ (輪郭ハロー無し)。
            occv = 1.0 - smoothstep(0.0, pen, SegPolyDist(P, Lp, j, nv, occ[j].z, inside_umbra));
        } else {                                         // 円: 光源→画素レイへの垂直距離 + 奥行ゲート
            float r = occ[j].z;
            if (r <= 0.0) continue;                      // ゼロ半径の円は遮蔽なし
            if (!inside_umbra && length(P - C) <= r) continue;   // P が円内部 = 重なり → 影なし
            float  pen   = max(r * 0.35, 6.0);
            float2 toP   = P - Lp;                        // 光源→画素
            float  dP    = max(length(toP), 1e-3);
            float2 dirP  = toP / dP;
            float  along = dot(C - Lp, dirP);            // 円中心の «光源からの» 奥行
            float  perp  = length((C - Lp) - dirP * along); // 中心からレイへの垂直距離
            // tangent 側 penumbra (perp≈r で減衰) × 奥行ゲート (画素が円より手前=光源側なら影なし)
            // × 前方ゲート (along>0: 円が光源より後ろにある画素には影を落とさない。along は無限
            //   投影なので奥行ゲートだけだと光源の反対側に偽影が出る)。
            occv = (1.0 - smoothstep(r - pen, r + pen, perp))
                 * smoothstep(along - r, along, dP) * smoothstep(-r, r, along);
        }
        vis *= 1.0 - saturate(occv);                     // saturate で vis を [0,1] に保つ
    }
    return vis;
}

// 等方 GGX NDF (alpha = roughness^2 を渡す)。クリアコート用。
float D_GGX(float ndh, float alpha) {
    float a2  = alpha * alpha;
    float den = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(PI * den * den, 1e-6);
}
// 異方 GGX NDF。ax=ay のとき D_GGX と一致 (T=接線, B=従法線方向の半角射影)。
float D_GGX_aniso(float ndh, float hdt, float hdb, float ax, float ay) {
    float d = hdt * hdt / (ax * ax) + hdb * hdb / (ay * ay) + ndh * ndh;
    return 1.0 / max(PI * ax * ay * d * d, 1e-6);
}
// Smith-Schlick 幾何項 (roughness を直接)。
float G_Smith(float ndv, float ndl, float roughness) {
    float k  = (roughness + 1.0); k = k * k / 8.0;
    float gv = ndv / (ndv * (1.0 - k) + k);
    float gl = ndl / (ndl * (1.0 - k) + k);
    return gv * gl;
}
// Charlie 分布 (布の逆反射シーン)。
float D_Charlie(float ndh, float r) {
    float inv = 1.0 / max(r * r, 0.0025);
    float s   = max(1.0 - ndh * ndh, 0.0);      // sin^2
    return (2.0 + inv) * pow(s, inv * 0.5) / (2.0 * PI);
}

// Substrate 風マルチロブ BRDF: metallic-roughness ベース + 異方性 + 鏡面レベル/tint +
// サブサーフェス wrap + シーン + クリアコート上層。拡張パラメータは cbuffer から直接読む。
float3 BRDF(float3 N, float3 V, float3 L, float3 albedo, float metallic, float roughness) {
    float  ndl = max(dot(N, L), 0.0);
    if (ndl <= 0.0 && adv1.w <= 0.0) return float3(0, 0, 0);  // SSS が無ければ裏面は寄与なし
    float3 H   = normalize(V + L);
    float  ndv = max(dot(N, V), 1e-3);
    float  ndh = max(dot(N, H), 0.0);
    float  vdh = max(dot(V, H), 0.0);

    float clearcoat = saturate(adv0.x);
    float ccRough   = clamp(adv0.y, 0.04, 1.0);
    float aniso     = clamp(adv0.z, -0.95, 0.95);
    float specLevel = saturate(adv0.w);
    float specTint  = saturate(adv1.x);
    float sheenW    = saturate(adv1.y);
    float sheenR    = clamp(adv1.z, 0.05, 1.0);
    float subsurf   = saturate(adv1.w);

    // 反射 F0: 誘電体は specLevel/tint で制御、金属は albedo。
    float3 dielF0 = (0.08 * specLevel) * lerp(float3(1, 1, 1), albedo, specTint);
    float3 F0 = lerp(dielF0, albedo, metallic);
    float3 F  = F0 + (1.0 - F0) * pow(1.0 - vdh, 5.0);

    // 異方 GGX。接線フレームは per-pixel N から直交化する (法線マップで N が傾いても
    // T,B が N と直交を保ち、D_GGX_aniso の前提が崩れて暴発するのを防ぐ)。平面 N=(0,0,1)
    // では T=(1,0,0)/B=(0,1,0) に一致 (= 従来の画面XY 方向)。
    float3 T = float3(1, 0, 0) - N * N.x;                 // 画面X を N へ Gram-Schmidt 直交化
    T = (dot(T, T) > 1e-6) ? normalize(T) : float3(0, 1, 0);
    float3 B = cross(N, T);
    float  alpha = roughness * roughness;
    float  ax = max(alpha * (1.0 + aniso), 1e-3);
    float  ay = max(alpha * (1.0 - aniso), 1e-3);
    float  D  = D_GGX_aniso(ndh, dot(H, T), dot(H, B), ax, ay);
    float  G  = G_Smith(ndv, ndl, roughness);
    float3 spec = D * G * F / max(4.0 * ndv * ndl, 1e-4);

    // 拡散 (サブサーフェスで wrap + 影を SSS 色へ)。
    float3 kd     = (1.0 - F) * (1.0 - metallic);
    float  wrap   = subsurf * 0.5;
    float  ndlW   = saturate((dot(N, L) + wrap) / (1.0 + wrap));
    float  diffNdl = lerp(ndl, ndlW, subsurf);
    float3 diffAlb = lerp(albedo, sss_col.rgb * albedo, subsurf * (1.0 - ndl));
    float3 diff    = kd * diffAlb / PI;

    // シーン (Charlie)。
    float3 sheen = float3(0, 0, 0);
    if (sheenW > 0.0) {
        float Dc = D_Charlie(ndh, sheenR);
        float Vs = 1.0 / (4.0 * (ndl + ndv - ndl * ndv) + 1e-4);
        sheen = sheen_col.rgb * sheenW * Dc * Vs;
    }

    float3 baseLayer = diff * diffNdl + (spec + sheen) * ndl;

    // クリアコート上層 (固定 F0=0.04 の GGX)。下層を (1 - cc*Fcc) で減衰。
    if (clearcoat > 0.0) {
        float Dcc = D_GGX(ndh, ccRough * ccRough);
        float Gcc = G_Smith(ndv, ndl, ccRough);
        float Fcc = 0.04 + 0.96 * pow(1.0 - vdh, 5.0);
        float specCC = Dcc * Gcc * Fcc / max(4.0 * ndv * ndl, 1e-4);
        baseLayer = baseLayer * (1.0 - clearcoat * Fcc) + clearcoat * specCC * ndl;
    }
    return baseLayer;
}

// per-channel ソフトニー: knee 以下は無変更、超過分のみ滑らかに 1.0 へ漸近 (決して飽和しない)。
// C1 連続 (knee で傾き 1)。BGRA8 RT のハードな 1.0 クリップ (光源近傍の白円) を柔らかいハイライトへ。
float3 SoftKnee(float3 c) {
    const float knee  = 0.85;
    const float range = 1.0 - knee;                       // knee 超のヘッドルーム (0.15)
    float3 over   = max(c - knee, 0.0);
    float3 rolled = range * (over / (over + range));      // [0,∞) → [0,range)
    return min(c, knee) + rolled;
}

float4 PSLit(VSOut v) : SV_TARGET {
    float4 alb = atlas.Sample(atlas_sampler, v.uv) * base_color;
    alb.rgb *= v.col.rgb;
    float metallic  = saturate(mat0.x);
    float roughness = clamp(mat0.y, 0.04, 1.0);
    float nstr      = mat0.z;
    float ao        = mat0.w;

    // 法線マップ規約: DirectX (+Y-down) を前提。world/screen が Y-down なので nt.y は無反転。
    // アセットが OpenGL (+Y-up, green-up) で書き出されている場合は次行で nt.y = -nt.y; する。
    float3 nt = normal_tex.Sample(atlas_sampler, v.uv).xyz * 2.0 - 1.0;
    nt.xy *= nstr;
    float3 N = normalize(float3(nt.x, nt.y, max(nt.z, 0.05)));
    float3 P = float3(v.wpos, 0.0);
    float3 V = float3(0.0, 0.0, 1.0);

    int   count     = (int)light_info.x;
    float height    = light_info.y;
    int   occ_count = (int)light_info.z;
    int   mode      = (int)toon0.x;       // 0=PBR, 1=Toon
    int   selfOcc   = (int)sss_col.w;     // 自分自身のオクルーダー番号 (-1=無し)。自己影スキップ用
    int   selfShJ   = (int)emissive.w;    // 自己影 (m_SelfShadow=1) の自分の番号 (-1=無し)。内部=umbra
    int   skipMask  = (int)toon_rim.w;    // 自分より下 (先描画) のキャスターのスキップマスク
    float3 Lo  = float3(0, 0, 0);         // PBR 直接光
    float  lum = 0.0;                     // トゥーン: 受けた光量 (cel ランプ用)
    float  tspec = 0.0;                   // トゥーン: スペキュラ最大
    [loop] for (int i = 0; i < count; ++i) {
        int    type = (int)light_col[i].w;
        float3 L; float at; float2 shadowTarget;
        if (type == 2) {                                   // Directional (平行光): 方向のみ・減衰なし
            float2 d = normalize(light_dir[i].xy + float2(1e-5, 0));   // 進行方向
            L = normalize(float3(-d, 0.7));                            // 画素→光源 (上方 + 逆方向)
            at = 1.0;
            shadowTarget = v.wpos - d * 5000.0;                       // 影は -方向へ平行に伸びる
        } else {                                            // Point / Spot
            float3 Ld   = float3(light_pos[i].xy, height) - P;
            float  dist = length(Ld);
            L    = Ld / max(dist, 1e-3);
            float rad = max(light_pos[i].z, 1.0);
            float dr  = dist / rad;
            at = saturate(1.0 - dr * dr);    // 緩やかな減衰 (中域を明るく保ち縁で落とす。旧 (1-d/r)^2 は急峻すぎた)
            shadowTarget = light_pos[i].xy;
            if (type == 1) {                                // Spot: 円錐減衰
                float2 axis = normalize(light_dir[i].xy + float2(1e-5, 0));   // スポット軸 (外向き)
                float2 toP  = normalize(P.xy - light_pos[i].xy);
                float  cosA = dot(toP, axis);
                at *= smoothstep(light_dir[i].w, light_dir[i].z, cosA);       // outer→inner
            }
        }
        float vis = ShadowVisibility(v.wpos, shadowTarget, occ_count, selfOcc, selfShJ, skipMask);  // オクルーダーのソフト影
        if (mode == 0) {                                    // PBR: Cook-Torrance
            Lo += BRDF(N, V, L, alb.rgb, metallic, roughness) * light_col[i].rgb * light_pos[i].w * at * vis;
        } else {                                            // Toon: 光量 + ステップスペキュラ
            float ndl = max(dot(N, L), 0.0);
            lum  += ndl * at * vis * light_pos[i].w;
            float3 H = normalize(V + L);
            tspec = max(tspec, pow(max(dot(N, H), 0.0), 48.0) * at * vis);
        }
    }

    float3 col;
    if (mode == 0) {                                        // ===== PBR =====
        // アンビエント: 拡散は金属で減衰、鏡面 (環境反射の簡易版) は F0 で。金属が真っ黒に
        // ならず «照らされた金属が明るく見える»。env 倍率で薄い環境光の反射を表現。
        float3 F0a     = lerp(float3(0.04, 0.04, 0.04), alb.rgb, metallic);
        float3 ambDiff = alb.rgb * (1.0 - metallic) * ambient.rgb * ao;
        float3 ambSpec = F0a * ambient.rgb * 5.0 * ao;      // 環境鏡面 (金属ほど効く)
        col = Lo + ambDiff + ambSpec + emissive.rgb;
        col = SoftKnee(col);                                // 明部の白飛びを per-channel で滑らかに圧縮
    } else {                                                // ===== Toon (UTS 風 cel) =====
        float soft = max(toon0.w, 0.001);
        float3 baseLit = alb.rgb;                           // 明部 = テクスチャ×ベース色
        float3 s1 = shadow1.rgb * alb.rgb;                  // 1影 (テクスチャで色付け)
        float3 s2 = shadow2.rgb * alb.rgb;                  // 2影
        float3 surf = lerp(s1, baseLit, smoothstep(shadow1.w - soft, shadow1.w + soft, lum));
        surf = lerp(s2, surf, smoothstep(shadow2.w - soft, shadow2.w + soft, lum));
        float rim = pow(1.0 - max(dot(N, V), 0.0), max(toon0.y, 0.1));   // リムライト
        surf += toon_rim.rgb * rim;
        surf += toon_spec.rgb * step(toon0.z, tspec);       // ステップ状トゥーンスペキュラ
        surf += emissive.rgb;
        col = surf;
    }
    return float4(col, alb.a);
}
)";

} // namespace

FSpriteBatch::~FSpriteBatch() noexcept
{
    // device が先に破棄された場合もあるため、デストラクタでは非所有ポインタを参照しない。
    ReleaseResources();
}

TResult<void> FSpriteBatch::Init(IRhiDevice& device, EFormat rt_format, u32 max_sprites,
                                 u32 msaa_samples) noexcept {
    // 再初期化時も、以前の成功状態や部分初期化状態を残さない。
    Shutdown();

    const auto fail = [this](const FErrorCode& error) noexcept -> TResult<void> {
        Shutdown();
        return Err<void>(error);
    };

    if (max_sprites == 0) max_sprites = 4096;
    // u16 インデックスは頂点番号 [0, 65536) しか指せない (1 sprite = 4 頂点)。これを超えると
    // base=u16(i*4) が wrap して不正インデックスになり、idx_count=u32(max_sprites*6) も
    // overflow して過小確保 → 索引バッファ書き込みで heap overflow し得る。上限で拒否する。
    constexpr u32 kMaxSpritesPerBatch = 65536u / 4u;  // = 16384
    if (max_sprites > kMaxSpritesPerBatch) {
        return fail(ACS_ERR(Render, 252, "FSpriteBatch::Init: max_sprites exceeds u16 index limit (16384)"));
    }
    m_MaxSprites  = max_sprites;
    m_Device      = &device;     // ステンシル PSO の遅延生成で再利用
    m_RtFormat    = rt_format;
    m_MsaaSamples = (msaa_samples > 1) ? msaa_samples : 1;   // 全 PSO に焼き込む

    // === シェーダ ===
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kSpriteHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Sprite.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return fail(vs_r.Error());
    m_Vs = Move(vs_r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSpriteHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Sprite.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return fail(ps_r.Error());
    m_Ps = Move(ps_r.Value());

    // === 動的頂点バッファ（4 頂点 / sprite）===
    const usize vb_size = sizeof(Vertex) * 4 * max_sprites;
    FBufferDesc vbd{};
    vbd.size = vb_size;
    vbd.usage = EBufferUsage::Vertex;
    vbd.cpu_writable = true;       // 自動で frame-cycled になる
    auto vb_r = CreateRhiBuffer(device, vbd);
    if (vb_r.IsErr()) return fail(vb_r.Error());
    m_Vb = Move(vb_r.Value());

    // CPU 側のステージング配列（各フレームここに頂点を積んでから VB へコピー）
    m_VertexAllocator = &DefaultAllocator();
    m_VertexCpu = static_cast<Vertex*>(m_VertexAllocator->Alloc(vb_size));
    if (!m_VertexCpu) return fail(ACS_ERR(Memory, 250, "FSpriteBatch: vertex stage alloc"));

    // === インデックスバッファ（quad ごとに 6 indices、固定）===
    const u32 idx_count = max_sprites * 6;
    FAllocator& allocator = DefaultAllocator();
    u16* idx_ptr = static_cast<u16*>(allocator.Alloc(sizeof(u16) * idx_count));
    if (!idx_ptr) return fail(ACS_ERR(Memory, 251, "FSpriteBatch: index alloc"));
    for (u32 i = 0; i < max_sprites; ++i) {
        const u16 base = static_cast<u16>(i * 4);
        idx_ptr[i*6 + 0] = base + 0;
        idx_ptr[i*6 + 1] = base + 1;
        idx_ptr[i*6 + 2] = base + 2;
        idx_ptr[i*6 + 3] = base + 0;
        idx_ptr[i*6 + 4] = base + 2;
        idx_ptr[i*6 + 5] = base + 3;
    }
    FBufferDesc ibd{};
    ibd.size = sizeof(u16) * idx_count;
    ibd.usage = EBufferUsage::Index16;
    ibd.cpu_writable = true;
    ibd.initial_data = idx_ptr;
    auto ib_r = CreateRhiBuffer(device, ibd);
    allocator.Free(idx_ptr);
    if (ib_r.IsErr()) return fail(ib_r.Error());
    m_Ib = Move(ib_r.Value());

    // === 定数バッファ（screen size + view）のリング ===
    // 1 個だとフレーム内の複数 view (world/HUD/反射) が同一アドレスを上書きし合い、
    // 先に積んだ draw が後の view を読んでしまう。view 切替ごとに別スロットを使う。
    for (u32 i = 0; i < kViewRing; ++i) {
        FBufferDesc cbd{};
        cbd.size = 256;
        cbd.usage = EBufferUsage::Uniform;
        cbd.cpu_writable = true;
        auto cb_r = CreateRhiBuffer(device, cbd);
        if (cb_r.IsErr()) return fail(cb_r.Error());
        m_Cb[i] = Move(cb_r.Value());
    }
    m_CbCur = 0;

    // === 1×1 白テクスチャ（DrawRect 用、矩形には常にこれを bind）===
    const u8 white_pixel[4] = { 255, 255, 255, 255 };
    FTextureDesc td{};
    td.width = 1; td.height = 1;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = white_pixel;
    td.initial_data_size = 4;
    auto wt_r = CreateRhiTexture(device, td);
    if (wt_r.IsErr()) return fail(wt_r.Error());
    m_White = Move(wt_r.Value());

    // === パイプライン（α ブレンド有効、深度無し、カリング無し）===
    FPipelineDesc pd{};
    FillCommonPipelineDesc(pd);
    pd.depth_format  = EFormat::Unknown;   // 2D は深度無し
    pd.depth_test    = false;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return fail(pl_r.Error());
    m_Pipeline = Move(pl_r.Value());

    return Ok();
}

// vs/ps/layout/blend/sampler/slot など、全 SpriteBatch PSO 共通の部分を埋める。
// depth_format / depth_test / stencil は呼び出し側がパス種別に応じて設定する。
void FSpriteBatch::FillCommonPipelineDesc(FPipelineDesc& pd) const noexcept {
    pd.vs = m_Vs.Get();
    pd.ps = m_Ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = m_RtFormat;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::AlphaBlend;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 1;
    pd.cbuffer_names[0] = "Screen";
    pd.texture_names[0] = "atlas";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    pd.vertex_stride = sizeof(Vertex);
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32_Float,    0  };
    pd.layout[1] = { "TEXCOORD", 0, EFormat::R32G32_Float,    8  };
    pd.layout[2] = { "COLOR",    0, EFormat::R32G32B32A32_Float, 16 };
    pd.layout_count = 3;
    pd.sample_count = m_MsaaSamples;   // MSAA RT へ描く場合は全 PSO のサンプル数を合わせる
}

// ステンシル 4 モードの PSO を遅延生成する。すべて DSVFormat=D24S8 で、深度テスト/
// 書込みは無効 (2D)。StencilEnable と比較/操作だけがモードごとに異なる。
bool FSpriteBatch::EnsureStencilPipelines() noexcept {
    if (m_StencilReady) return true;
    if (!m_Device) return false;

    struct ModeCfg { bool enable; ECompareFunc func; EStencilOp pass; };
    const ModeCfg cfg[4] = {
        { false, ECompareFunc::Always,   EStencilOp::Keep    },  // Off
        { true,  ECompareFunc::Always,   EStencilOp::Replace },  // WriteMask
        { true,  ECompareFunc::Equal,    EStencilOp::Keep    },  // KeepInside
        { true,  ECompareFunc::NotEqual, EStencilOp::Keep    },  // KeepOutside
    };
    for (u32 i = 0; i < 4; ++i) {
        FPipelineDesc pd{};
        FillCommonPipelineDesc(pd);
        pd.depth_format = EFormat::D24_UNorm_S8_UInt;  // stencil 付き DSV と整合させる
        pd.depth_test   = false;
        pd.depth_write  = false;
        pd.stencil.enable     = cfg[i].enable;
        pd.stencil.func       = cfg[i].func;
        pd.stencil.pass_op    = cfg[i].pass;
        pd.stencil.fail_op    = EStencilOp::Keep;
        pd.stencil.depth_fail_op = EStencilOp::Keep;
        auto r = CreateRhiPipeline(*m_Device, pd);
        if (r.IsErr()) {
            ACS_LOG_ERROR("FSpriteBatch: ステンシル PSO %u の生成に失敗", i);
            return false;
        }
        m_StencilPipe[i] = Move(r.Value());
    }
    m_StencilReady = true;
    return true;
}

bool FSpriteBatch::EnsureAdditivePipeline() noexcept {
    if (m_AdditiveReady) return true;
    if (!m_Device) return false;
    FPipelineDesc pd{};
    FillCommonPipelineDesc(pd);
    pd.blend_mode   = EBlendMode::Additive;   // src*srcA + dst (背景を明るくする)
    pd.depth_format = EFormat::Unknown;
    pd.depth_test   = false;
    auto r = CreateRhiPipeline(*m_Device, pd);
    if (r.IsErr()) { ACS_LOG_ERROR("FSpriteBatch: 加算 PSO の生成に失敗"); return false; }
    m_AdditivePipe = Move(r.Value());
    m_AdditiveReady = true;
    return true;
}

// ピクセル効果 PS + PSO + 効果 CB リングを遅延生成する。VS は通常スプライトと共通
// (m_Vs)。PSO は通常パイプラインと同じ DSV 無し・AlphaBlend で、b1 に cbuffer Effect
// を 1 本追加するだけ。効果を使わないシーンでは一切作らない。
bool FSpriteBatch::EnsureEffectPipeline() noexcept {
    if (m_EffectReady) return true;
    if (!m_Device) return false;

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSpriteEffectHLSL;
    ps_d.entry_point = "PSEffect";
    ps_d.debug_name  = "Sprite.Effect.PS";
    auto ps_r = CreateRhiShader(*m_Device, ps_d);
    if (ps_r.IsErr()) { ACS_LOG_ERROR("FSpriteBatch: 効果 PS の生成に失敗"); return false; }
    m_EffectPs = Move(ps_r.Value());

    for (u32 i = 0; i < kEffectRing; ++i) {
        FBufferDesc cbd{};
        cbd.size = 256;
        cbd.usage = EBufferUsage::Uniform;
        cbd.cpu_writable = true;
        auto cb_r = CreateRhiBuffer(*m_Device, cbd);
        if (cb_r.IsErr()) { ACS_LOG_ERROR("FSpriteBatch: 効果 CB の生成に失敗"); return false; }
        m_EffectCb[i] = Move(cb_r.Value());
    }
    m_EffectCbCur = 0;

    FPipelineDesc pd{};
    FillCommonPipelineDesc(pd);          // vs/layout/sampler/atlas/Screen(b0) を共通設定
    pd.ps            = m_EffectPs.Get();  // PS だけ効果版へ差し替え
    pd.depth_format  = EFormat::Unknown;
    pd.depth_test    = false;
    pd.cbuffer_slots = 2;                 // b0=Screen, b1=Effect
    pd.cbuffer_names[1] = "Effect";
    auto pl_r = CreateRhiPipeline(*m_Device, pd);
    if (pl_r.IsErr()) { ACS_LOG_ERROR("FSpriteBatch: 効果 PSO の生成に失敗"); return false; }
    m_EffectPipe = Move(pl_r.Value());

    m_EffectReady = true;
    return true;
}

// lit (PBR) スプライトの VS/PS + PSO + マテリアル/ライト CB + 平面法線テクスチャを遅延生成する。
bool FSpriteBatch::EnsureLitPipeline() noexcept {
    if (m_LitReady) return true;
    if (!m_Device) return false;

    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kLitSpriteHLSL;
    vs_d.entry_point = "VSLit";
    vs_d.debug_name  = "Sprite.Lit.VS";
    auto vs_r = CreateRhiShader(*m_Device, vs_d);
    if (vs_r.IsErr()) { ACS_LOG_ERROR("FSpriteBatch: lit VS の生成に失敗"); return false; }
    m_LitVs = Move(vs_r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kLitSpriteHLSL;
    ps_d.entry_point = "PSLit";
    ps_d.debug_name  = "Sprite.Lit.PS";
    auto ps_r = CreateRhiShader(*m_Device, ps_d);
    if (ps_r.IsErr()) { ACS_LOG_ERROR("FSpriteBatch: lit PS の生成に失敗"); return false; }
    m_LitPs = Move(ps_r.Value());

    for (u32 i = 0; i < kLitMatRing; ++i) {
        FBufferDesc cbd{};
        cbd.size = 256;   // 1 マテリアル CB のバイト数 (LitMaterial は 48B、256 境界に切り上げ)
        cbd.usage = EBufferUsage::Uniform;
        cbd.cpu_writable = true;
        auto cb_r = CreateRhiBuffer(*m_Device, cbd);
        if (cb_r.IsErr()) { ACS_LOG_ERROR("FSpriteBatch: lit マテリアル CB の生成に失敗"); return false; }
        m_LitMatCb[i] = Move(cb_r.Value());
    }
    m_LitMatCbCur = 0;

    {
        FBufferDesc cbd{};
        cbd.size = 5632;   // light_info+ambient+pos/col/dir/occ/occ2[16]+occ_poly[256] = 5408B
        cbd.usage = EBufferUsage::Uniform;
        cbd.cpu_writable = true;
        auto cb_r = CreateRhiBuffer(*m_Device, cbd);
        if (cb_r.IsErr()) { ACS_LOG_ERROR("FSpriteBatch: ライト CB の生成に失敗"); return false; }
        m_LightsCb = Move(cb_r.Value());
    }

    // 平面法線 1x1 ((128,128,255) = (0,0,1) を [0,1] エンコード)。
    const u8 flat_n[4] = { 128, 128, 255, 255 };
    FTextureDesc td{};
    td.width = 1; td.height = 1;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = flat_n;
    td.initial_data_size = 4;
    auto nt_r = CreateRhiTexture(*m_Device, td);
    if (nt_r.IsErr()) { ACS_LOG_ERROR("FSpriteBatch: 平面法線テクスチャの生成に失敗"); return false; }
    m_FlatNormal = Move(nt_r.Value());

    FPipelineDesc pd{};
    FillCommonPipelineDesc(pd);          // vs/layout/sampler/atlas/Screen(b0) を共通設定
    pd.vs            = m_LitVs.Get();     // VS だけ lit 版へ差し替え (world pos を出す)
    pd.ps            = m_LitPs.Get();
    pd.depth_format  = EFormat::Unknown;
    pd.depth_test    = false;
    pd.cbuffer_slots = 3;                 // b0=Screen, b1=LitMaterial, b2=Lights
    pd.cbuffer_names[1] = "LitMaterial";
    pd.cbuffer_names[2] = "Lights";
    pd.texture_slots = 2;                 // t0=atlas, t1=normal_tex
    pd.texture_names[1] = "normal_tex";
    auto pl_r = CreateRhiPipeline(*m_Device, pd);
    if (pl_r.IsErr()) { ACS_LOG_ERROR("FSpriteBatch: lit PSO の生成に失敗"); return false; }
    m_LitPipe = Move(pl_r.Value());

    m_LitReady = true;
    return true;
}

void FSpriteBatch::SetBlendMode(EBlendMode mode) noexcept {
    if (!m_Cl || mode == m_BlendMode) return;
    Flush();                                  // 直前のバッチを現ブレンドで確定
    IRhiPipeline* pl = nullptr;
    if (mode == EBlendMode::Additive) {
        if (!EnsureAdditivePipeline()) return;
        pl = m_AdditivePipe.Get();
    } else {
        pl = m_Pipeline.Get();                // 既定 = AlphaBlend
    }
    if (!pl) return;
    m_Cl->SetPipeline(*pl);
    BindViewBuffer();                          // PSO 切替で root 引数が無効化される
    m_Cl->SetVertexBuffer(*m_Vb, sizeof(Vertex));
    m_Cl->SetIndexBuffer(*m_Ib);
    m_BlendMode = mode;
}

void FSpriteBatch::SetStencilMode(EStencilMode mode, u8 ref) noexcept {
    if (!m_Cl) return;
    Flush();                                  // 直前のバッチを現モードで確定
    if (!EnsureStencilPipelines()) return;
    IRhiPipeline* pl = m_StencilPipe[static_cast<u32>(mode)].Get();
    if (!pl) return;
    m_Cl->SetPipeline(*pl);
    m_Cl->SetStencilRef(ref);
    // PSO (= root signature) 切替で root 引数が無効化されるので CBV を貼り直す
    // (現 view バッファを bind = view は変えない)。IA (VB/IB) は PSO 切替で無効化
    // されないが、念のため再 bind してパリティを保つ。
    BindViewBuffer();
    m_Cl->SetVertexBuffer(*m_Vb, sizeof(Vertex));
    m_Cl->SetIndexBuffer(*m_Ib);
    m_StencilMode = mode;
}

void FSpriteBatch::SetEffect(ESpriteEffect effect, const FEffectParams& params) noexcept {
    if (!m_Cl) return;
    if (effect == ESpriteEffect::None) { ClearEffect(); return; }
    Flush();                                   // 直前のバッチを現パイプラインで確定
    if (!EnsureEffectPipeline()) return;

    // 効果 CB を次スロットへ書く (フレーム内で複数効果が衝突しないように)。
    m_EffectCbCur = (m_EffectCbCur + 1u) % kEffectRing;
    const f32 cb[12] = {
        static_cast<f32>(static_cast<i32>(effect)), params.strength, params.p0, params.p1,
        params.p2, params.time, 0.0f, 0.0f,
        params.color.x, params.color.y, params.color.z, params.color.w,
    };
    m_EffectCb[m_EffectCbCur]->Update(cb, sizeof(cb));

    m_Cl->SetPipeline(*m_EffectPipe);
    BindViewBuffer();                                       // b0 (PSO 切替で root 引数が無効化)
    m_Cl->SetConstantBuffer(1, *m_EffectCb[m_EffectCbCur]); // b1
    m_Cl->SetVertexBuffer(*m_Vb, sizeof(Vertex));
    m_Cl->SetIndexBuffer(*m_Ib);
    m_EffectActive = true;
}

void FSpriteBatch::ClearEffect() noexcept {
    if (!m_Cl || !m_EffectActive) return;
    Flush();                                   // 効果バッチを確定してから通常へ戻す
    m_Cl->SetPipeline(*m_Pipeline);
    BindViewBuffer();
    m_Cl->SetVertexBuffer(*m_Vb, sizeof(Vertex));
    m_Cl->SetIndexBuffer(*m_Ib);
    m_EffectActive = false;
}

void FSpriteBatch::SetLights(const FSpriteLight* lights, u32 count, FVec3 ambient,
                             f32 light_height,
                             const FSpriteOccluder* occluders, u32 occ_count) noexcept {
    if (!m_Cl) return;
    if (!EnsureLitPipeline()) return;
    if (count > kMaxLitLights) count = kMaxLitLights;
    if (occ_count > kMaxLitLights) occ_count = kMaxLitLights;
    m_SceneLightCount = count;
    // cbuffer Lights: light_info, ambient, pos[16], col[16], dir[16], occ[16], occ2[16], occ_poly[256] = 1352 floats。
    f32 cb[4 + 4 + 16 * 4 * 5 + 256 * 4] = {};
    cb[0] = static_cast<f32>(count); cb[1] = light_height; cb[2] = static_cast<f32>(occ_count);
    cb[4] = ambient.x; cb[5] = ambient.y; cb[6] = ambient.z;
    f32* pos  = cb + 8;            // light_pos[16] (xy, radius, intensity)
    f32* col  = cb + 8 + 64;       // light_col[16] (rgb, type)
    f32* dir  = cb + 8 + 128;      // light_dir[16] (dir.xy, coneInner, coneOuter)
    f32* ocp  = cb + 8 + 192;      // occ[16] (xy=center, z=radius, w=shape)
    f32* ocp2 = cb + 8 + 256;      // occ2[16] (xy=halfExtents, z=rotation, w=多角形頂点数)
    f32* opl  = cb + 8 + 320;      // occ_poly[256] (occluder j: opl[j*64 .. +63] = 32頂点)
    for (u32 i = 0; i < count; ++i) {
        pos[i * 4 + 0] = lights[i].pos.x;
        pos[i * 4 + 1] = lights[i].pos.y;
        pos[i * 4 + 2] = lights[i].radius;
        pos[i * 4 + 3] = lights[i].intensity;
        col[i * 4 + 0] = lights[i].color.x;
        col[i * 4 + 1] = lights[i].color.y;
        col[i * 4 + 2] = lights[i].color.z;
        col[i * 4 + 3] = static_cast<f32>(lights[i].type);
        dir[i * 4 + 0] = lights[i].dir.x;
        dir[i * 4 + 1] = lights[i].dir.y;
        dir[i * 4 + 2] = lights[i].coneInner;
        dir[i * 4 + 3] = lights[i].coneOuter;
    }
    for (u32 i = 0; i < occ_count; ++i) {
        ocp[i * 4 + 0]  = occluders[i].center.x;
        ocp[i * 4 + 1]  = occluders[i].center.y;
        ocp[i * 4 + 2]  = occluders[i].radius;
        ocp[i * 4 + 3]  = static_cast<f32>(occluders[i].shape);
        ocp2[i * 4 + 0] = occluders[i].halfExtents.x;
        ocp2[i * 4 + 1] = occluders[i].halfExtents.y;
        ocp2[i * 4 + 2] = occluders[i].rotation;
        if (occluders[i].shape == 2) {                       // 多角形の頂点 (最大 32) を詰める
            u32 nv = occluders[i].polyCount;
            if (nv > kMaxOccPolyVerts) nv = kMaxOccPolyVerts;
            ocp2[i * 4 + 3] = static_cast<f32>(nv);
            for (u32 k = 0; k < nv; ++k) {
                opl[i * 64 + k * 2 + 0] = occluders[i].polyVerts[k].x;
                opl[i * 64 + k * 2 + 1] = occluders[i].polyVerts[k].y;
            }
        }
    }
    m_LightsCb->Update(cb, sizeof(cb));
}

void FSpriteBatch::SetLitMaterial(const FLitMaterialParams& mat, IRhiTexture* normal_tex) noexcept {
    if (!m_Cl) return;
    Flush();                                   // 直前のバッチを現パイプラインで確定
    if (!EnsureLitPipeline()) return;

    m_LitMatCbCur = (m_LitMatCbCur + 1u) % kLitMatRing;
    const f32 cb[48] = {
        mat.baseColor.x, mat.baseColor.y, mat.baseColor.z, mat.baseColor.w,            // base_color
        mat.metallic, mat.roughness, mat.normalStrength, mat.ao,                       // mat0
        mat.emissive.x * mat.emissiveStrength, mat.emissive.y * mat.emissiveStrength,  // emissive
        mat.emissive.z * mat.emissiveStrength, static_cast<f32>(mat.selfShadowOccluder),   // w=自己影番号
        static_cast<f32>(mat.shadingMode), mat.rimPower, mat.specThreshold, mat.toonSoftness,  // toon0
        mat.shadow1Color.x, mat.shadow1Color.y, mat.shadow1Color.z, mat.shadow1Threshold,     // shadow1
        mat.shadow2Color.x, mat.shadow2Color.y, mat.shadow2Color.z, mat.shadow2Threshold,     // shadow2
        mat.rimColor.x, mat.rimColor.y, mat.rimColor.z,
        static_cast<f32>(mat.occluderSkipMask),                                        // toon_rim (w=スキップマスク)
        mat.specColor.x, mat.specColor.y, mat.specColor.z, 0.0f,                       // toon_spec
        mat.clearcoat, mat.clearcoatRoughness, mat.anisotropy, mat.specularLevel,      // adv0
        mat.specularTint, mat.sheen, mat.sheenRoughness, mat.subsurface,               // adv1
        mat.sheenColor.x, mat.sheenColor.y, mat.sheenColor.z, 0.0f,                    // sheen_col
        mat.subsurfaceColor.x, mat.subsurfaceColor.y, mat.subsurfaceColor.z,           // sss_col.rgb
        static_cast<f32>(mat.selfOccluder),                                            // sss_col.w = selfOcc
    };
    m_LitMatCb[m_LitMatCbCur]->Update(cb, sizeof(cb));

    m_Cl->SetPipeline(*m_LitPipe);
    BindViewBuffer();                                       // b0
    m_Cl->SetConstantBuffer(1, *m_LitMatCb[m_LitMatCbCur]); // b1
    m_Cl->SetConstantBuffer(2, *m_LightsCb);               // b2
    m_Cl->SetTexture(1, normal_tex ? *normal_tex : *m_FlatNormal);   // t1
    m_Cl->SetVertexBuffer(*m_Vb, sizeof(Vertex));
    m_Cl->SetIndexBuffer(*m_Ib);
    m_LitActive = true;
}

void FSpriteBatch::ClearLit() noexcept {
    if (!m_Cl || !m_LitActive) return;
    Flush();
    m_Cl->SetPipeline(*m_Pipeline);
    BindViewBuffer();
    m_Cl->SetVertexBuffer(*m_Vb, sizeof(Vertex));
    m_Cl->SetIndexBuffer(*m_Ib);
    m_LitActive = false;
}

void FSpriteBatch::Shutdown() noexcept {
    // 再初期化や単独 Shutdown でも、GPU が参照中のリソースを先に破棄しない。
    // m_Device は本バッチより長く生存するという既存の所有契約に従う。
    if (m_Device) m_Device->WaitIdle();

    ReleaseResources();
}

void FSpriteBatch::ReleaseResources() noexcept
{
    // 描画中を指す非所有ポインタは、所有リソースより先に無効化する。
    m_Cl = nullptr;
    m_CurrentTex = nullptr;

    m_Pipeline.Reset();
    for (u32 i = 0; i < 4; ++i) m_StencilPipe[i].Reset();
    m_StencilReady = false;
    m_AdditivePipe.Reset();
    m_AdditiveReady = false;
    m_EffectPipe.Reset();
    m_EffectPs.Reset();
    for (u32 i = 0; i < kEffectRing; ++i) m_EffectCb[i].Reset();
    m_EffectReady = false;
    m_EffectActive = false;
    m_LitPipe.Reset();
    m_LitVs.Reset();
    m_LitPs.Reset();
    for (u32 i = 0; i < kLitMatRing; ++i) m_LitMatCb[i].Reset();
    m_LightsCb.Reset();
    m_FlatNormal.Reset();
    m_LitReady = false;
    m_LitActive = false;
    m_White.Reset();
    for (u32 i = 0; i < kViewRing; ++i) m_Cb[i].Reset();
    m_Ib.Reset();
    m_Vb.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
    if (m_VertexCpu) {
        // 確保時と同じアロケータへ返す。DefaultAllocator の差し替えを跨いでも誤解放しない。
        if (m_VertexAllocator) m_VertexAllocator->Free(m_VertexCpu);
        m_VertexCpu = nullptr;
    }
    m_VertexAllocator = nullptr;
    m_Device = nullptr;
    m_RtFormat = EFormat::B8G8R8A8_UNorm;
    m_MsaaSamples = 1;
    m_MaxSprites = 0;
    m_SpriteCount = 0;
    m_FlushedCount = 0;
    m_CbCur = 0;
    m_EffectCbCur = 0;
    m_LitMatCbCur = 0;
    m_SceneLightCount = 0;
    m_StencilMode = EStencilMode::Off;
    m_BlendMode = EBlendMode::AlphaBlend;
    m_ScreenW = 1;
    m_ScreenH = 1;
    m_ViewX = 0.0f;
    m_ViewY = 0.0f;
    m_ViewZoom = 1.0f;
}

void FSpriteBatch::Begin(IRhiCommandList& cl, u32 screen_w, u32 screen_h) noexcept {
    m_Cl = &cl;
    m_ScreenW = screen_w == 0 ? 1 : screen_w;
    m_ScreenH = screen_h == 0 ? 1 : screen_h;
    m_SpriteCount = 0;
    m_FlushedCount = 0;
    m_CurrentTex = nullptr;
    m_StencilMode = EStencilMode::Off;   // 既定パイプライン (m_Pipeline) で開始
    m_BlendMode   = EBlendMode::AlphaBlend;
    m_EffectActive = false;              // 効果は Begin でリセット (既定 = 通常 PSO)
    m_LitActive    = false;              // lit も Begin でリセット

    // ビューを恒等（カメラ無し）に戻し、フレッシュな view バッファへ書く。
    // (Begin は同一フレーム内で複数回呼ばれ得る — 反射の 2 パス等 — ので、
    //  リングを reset せず常に次スロットへ進めて前パスの draw と干渉させない。)
    m_ViewX    = static_cast<f32>(m_ScreenW) * 0.5f;
    m_ViewY    = static_cast<f32>(m_ScreenH) * 0.5f;
    m_ViewZoom = 1.0f;
    AdvanceViewBuffer();
    WriteScreenCBuffer();

    // パイプラインと共通リソースを bind
    cl.SetPipeline(*m_Pipeline);
    BindViewBuffer();
    cl.SetVertexBuffer(*m_Vb, sizeof(Vertex));
    cl.SetIndexBuffer(*m_Ib);

    // SRV descriptor table (root param 1) を Begin 時点で既定の白テクスチャに確定させる。
    // これをしないと「フレーム最初の content draw」が「パイプライン新規 bind 後の最初の
    // descriptor-table bind」を兼ねてしまい、シーン遷移直後 (バッチ再 Init で descriptor
    // slot が LIFO 再利用 + frames-in-flight が 1 フレーム重なる) に、その最初の draw が
    // GPU 実行時に stale を読んで落ちることがある (world 空で HUD パスだけのシーンで顕在化
    // = sample 63 の GameOver が真っ暗になる事象)。白テクスチャを先に bind しておくことで、
    // 最初の content Flush は「2 回目以降の table bind」となり保護される。m_CurrentTex も
    // m_White にしておくと、最初の DrawSub(別tex) は no-op Flush 経由で正しく切り替わる。
    cl.SetTexture(0, *m_White);
    m_CurrentTex = m_White.Get();
}

void FSpriteBatch::Draw(IRhiTexture& tex, f32 x, f32 y, f32 w, f32 h, FVec4 tint) noexcept {
    DrawSub(tex, x, y, w, h, 0, 0, 1, 1, tint);
}

void FSpriteBatch::DrawSub(IRhiTexture& tex,
                          f32 x, f32 y, f32 w, f32 h,
                          f32 u0, f32 v0, f32 u1, f32 v1,
                          FVec4 tint) noexcept {
    if (!m_Cl) return;
    // テクスチャ切替でフラッシュ（同じテクスチャは累積）
    if (m_CurrentTex && m_CurrentTex != &tex) Flush();
    // 容量上限に達したら以降は無視（max_sprites を Init 時に増やす）
    if (m_SpriteCount >= m_MaxSprites) return;
    m_CurrentTex = &tex;

    Vertex* v = m_VertexCpu + m_SpriteCount * 4;
    // 4 頂点を時計回りに積む: (x,y), (x+w,y), (x+w,y+h), (x,y+h)
    v[0] = { x,     y,     u0, v0, tint.x, tint.y, tint.z, tint.w };
    v[1] = { x+w,   y,     u1, v0, tint.x, tint.y, tint.z, tint.w };
    v[2] = { x+w,   y+h,   u1, v1, tint.x, tint.y, tint.z, tint.w };
    v[3] = { x,     y+h,   u0, v1, tint.x, tint.y, tint.z, tint.w };
    ++m_SpriteCount;
}

void FSpriteBatch::DrawRect(f32 x, f32 y, f32 w, f32 h, FVec4 color) noexcept {
    DrawSub(*m_White, x, y, w, h, 0, 0, 1, 1, color);
}

void FSpriteBatch::DrawRotated(IRhiTexture& tex,
                              f32 cx, f32 cy, f32 w, f32 h, f32 radians,
                              f32 u0, f32 v0, f32 u1, f32 v1,
                              FVec4 tint) noexcept {
    if (!m_Cl) return;
    if (m_CurrentTex && m_CurrentTex != &tex) Flush();
    if (m_SpriteCount >= m_MaxSprites) return;
    m_CurrentTex = &tex;

    const f32 s  = ::sinf(radians);
    const f32 c  = ::cosf(radians);
    const f32 hw = w * 0.5f;
    const f32 hh = h * 0.5f;
    // ローカル 4 隅 (TL, TR, BR, BL) を中心まわりに回転。DrawSub と同じ頂点順。
    const f32 lx[4] = { -hw,  hw,  hw, -hw };
    const f32 ly[4] = { -hh, -hh,  hh,  hh };
    const f32 uu[4] = {  u0,  u1,  u1,  u0 };
    const f32 vv[4] = {  v0,  v0,  v1,  v1 };

    Vertex* vtx = m_VertexCpu + m_SpriteCount * 4;
    for (int i = 0; i < 4; ++i) {
        const f32 px = cx + lx[i] * c - ly[i] * s;
        const f32 py = cy + lx[i] * s + ly[i] * c;
        vtx[i] = { px, py, uu[i], vv[i], tint.x, tint.y, tint.z, tint.w };
    }
    ++m_SpriteCount;
}

void FSpriteBatch::DrawRectRotated(f32 cx, f32 cy, f32 w, f32 h, f32 radians,
                                  FVec4 color) noexcept {
    DrawRotated(*m_White, cx, cy, w, h, radians, 0, 0, 1, 1, color);
}

void FSpriteBatch::WriteScreenCBuffer() noexcept {
    const f32 sw = static_cast<f32>(m_ScreenW);
    const f32 sh = static_cast<f32>(m_ScreenH);
    f32 cb[8] = {
        1.0f / sw, 1.0f / sh, sw, sh,
        m_ViewX, m_ViewY, m_ViewZoom, 0.0f,
    };
    m_Cb[m_CbCur]->Update(cb, sizeof(cb));
}

void FSpriteBatch::AdvanceViewBuffer() noexcept {
    m_CbCur = (m_CbCur + 1u) % kViewRing;
}

void FSpriteBatch::BindViewBuffer() noexcept {
    if (m_Cl) m_Cl->SetConstantBuffer(0, *m_Cb[m_CbCur]);
}

void FSpriteBatch::SetView(f32 cam_x, f32 cam_y, f32 zoom) noexcept {
    if (!m_Cl) return;
    Flush();   // 既存バッチを「現在の view バッファ」で確定してから切り替える
    // 別スロットへ進めて新 view を書き、root CBV を貼り直す。これで Flush 済みの
    // draw は旧スロット (旧 view) を、以降の draw は新スロット (新 view) を読む。
    AdvanceViewBuffer();
    m_ViewX    = cam_x;
    m_ViewY    = cam_y;
    m_ViewZoom = (zoom > 0.0001f) ? zoom : 1.0f;
    WriteScreenCBuffer();
    BindViewBuffer();
}

void FSpriteBatch::SetClipRect(i32 x, i32 y, i32 w, i32 h) noexcept {
    if (!m_Cl) return;
    Flush();   // クリップ変更前のバッチを確定
    FScissorRect sr{ x, y, x + w, y + h };
    m_Cl->SetScissor(sr);
}

void FSpriteBatch::ClearClipRect() noexcept {
    if (!m_Cl) return;
    Flush();
    FScissorRect sr{ 0, 0, static_cast<i32>(m_ScreenW), static_cast<i32>(m_ScreenH) };
    m_Cl->SetScissor(sr);
}

void FSpriteBatch::DrawTriangleVC(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                                 FVec4 c0, FVec4 c1, FVec4 c2) noexcept {
    if (!m_Cl) return;
    if (m_CurrentTex && m_CurrentTex != m_White.Get()) Flush();
    if (m_SpriteCount >= m_MaxSprites) return;
    m_CurrentTex = m_White.Get();
    // 4 頂点目に 3 頂点目を重ねる。インデックス (0,2,3) の三角形は面積 0 に
    // 退化して描画されず、(0,1,2) の三角形だけが塗られる。各頂点に別の色を
    // 持たせると、シェーダが COLOR を補間してグラデーション三角形になる。
    Vertex* v = m_VertexCpu + m_SpriteCount * 4;
    v[0] = { x0, y0, 0, 0, c0.x, c0.y, c0.z, c0.w };
    v[1] = { x1, y1, 0, 0, c1.x, c1.y, c1.z, c1.w };
    v[2] = { x2, y2, 0, 0, c2.x, c2.y, c2.z, c2.w };
    v[3] = { x2, y2, 0, 0, c2.x, c2.y, c2.z, c2.w };
    ++m_SpriteCount;
}

void FSpriteBatch::DrawTriangle(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                               FVec4 color) noexcept {
    DrawTriangleVC(x0, y0, x1, y1, x2, y2, color, color, color);
}

void FSpriteBatch::DrawTriangleSub(IRhiTexture& tex,
                                   f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                                   f32 u0, f32 v0, f32 u1, f32 v1, f32 u2, f32 v2,
                                   FVec4 tint) noexcept {
    if (!m_Cl) return;
    if (m_CurrentTex && m_CurrentTex != &tex) Flush();
    if (m_SpriteCount >= m_MaxSprites) return;
    m_CurrentTex = &tex;
    // 任意テクスチャを per-vertex UV で貼る三角形 (水の反射でシーン RT をサンプル)。
    Vertex* v = m_VertexCpu + m_SpriteCount * 4;
    v[0] = { x0, y0, u0, v0, tint.x, tint.y, tint.z, tint.w };
    v[1] = { x1, y1, u1, v1, tint.x, tint.y, tint.z, tint.w };
    v[2] = { x2, y2, u2, v2, tint.x, tint.y, tint.z, tint.w };
    v[3] = { x2, y2, u2, v2, tint.x, tint.y, tint.z, tint.w };
    ++m_SpriteCount;
}

void FSpriteBatch::DrawString(const Font& font, const char* utf8_text,
                           f32 x, f32 y, FVec4 color) noexcept {
    if (!utf8_text || !font.AtlasTexture()) return;
    IRhiTexture* atlas = font.AtlasTexture();

    // (x, y) は行の左上。ベースラインは y + ascent。
    f32 pen_x    = x;
    f32 baseline = y + font.Ascent();

    const char* p = utf8_text;
    while (true) {
        const u32 cp = DecodeUtf8(&p);
        if (cp == 0) break;
        if (cp == '\n') {
            pen_x    = x;
            baseline += font.LineHeight();
            continue;
        }
        GlyphInfo g{};
        if (!font.GetGlyph(cp, g)) continue;
        // packedchar の x_offset/y_offset はベースライン基準
        const f32 qx = pen_x    + g.x_offset;
        const f32 qy = baseline + g.y_offset;
        DrawSub(*atlas, qx, qy, g.width, g.height,
                g.u0, g.v0, g.u1, g.v1, color);
        pen_x += g.x_advance;
    }
}

void FSpriteBatch::End() noexcept {
    Flush();
    m_Cl = nullptr;
}

void FSpriteBatch::FlushPending() noexcept {
    Flush();   // カウントは維持されるので以降のスプライトは VB の続きへ追記される
}

void FSpriteBatch::Rebind() noexcept {
    if (!m_Cl) return;
    // Begin と同じ bind を貼り直す (カウントやリングは進めない)。割り込み描画で
    // 変わった pipeline / vertex・index buffer / view cbuffer を元に戻す。
    m_Cl->SetPipeline(*m_Pipeline);
    BindViewBuffer();
    m_Cl->SetVertexBuffer(*m_Vb, sizeof(Vertex));
    m_Cl->SetIndexBuffer(*m_Ib);
}

void FSpriteBatch::Flush() noexcept {
    if (!m_Cl || !m_CurrentTex) return;
    if (m_SpriteCount <= m_FlushedCount) return;

    // 既にフラッシュ済みのスプライトの後ろに、新しい範囲だけを VB に部分書き込みする。
    // こうすることで、先行投入済みの DrawIndexed が参照する範囲を上書きしない。
    const u32   first_sprite = m_FlushedCount;
    const u32   count        = m_SpriteCount - m_FlushedCount;
    const usize byte_offset  = static_cast<usize>(first_sprite) * 4 * sizeof(Vertex);
    const usize byte_size    = static_cast<usize>(count) * 4 * sizeof(Vertex);
    m_Vb->Update(m_VertexCpu + first_sprite * 4, byte_size, byte_offset);

    m_Cl->SetTexture(0, *m_CurrentTex);
    m_Cl->DrawIndexed(count * 6, first_sprite * 6, 0);

    m_FlushedCount = m_SpriteCount;
    m_CurrentTex   = nullptr;
}

} // namespace acs
