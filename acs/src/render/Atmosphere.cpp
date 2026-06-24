// SPDX-License-Identifier: Apache-2.0
// Physical atmospheric scattering 実装
#include "render/Atmosphere.h"
#include "math/Math.h"
#include "foundation/Move.h"

namespace acs {

namespace {

/** 地表半径 (m、Earth)。 */
constexpr f32 kGroundRadius     = 6360000.0f;

/** 大気上端半径 (m、地表から 60 km)。 */
constexpr f32 kAtmosphereRadius = 6420000.0f;

/** Rayleigh の scale height (m、8 km)。 */
constexpr f32 kRayleighH        = 8000.0f;

/** Mie の scale height (m、1.2 km)。 */
constexpr f32 kMieH             = 1200.0f;

/**
 * Rayleigh 散乱係数 β (RGB、m⁻¹) を返す。
 *
 * @return 波長別の Rayleigh 散乱係数 (5.802, 13.558, 33.1) ×10⁻⁶ m⁻¹。
 */
inline FVec3 RayleighBeta() noexcept {
    return FVec3{5.802e-6f, 13.558e-6f, 33.1e-6f};
}

/**
 * Mie 散乱係数 β (m⁻¹) を返す。
 *
 * @return Mie 散乱係数 3.996 ×10⁻⁶ m⁻¹。
 */
inline f32  MieBeta() noexcept       { return 3.996e-6f; }

/**
 * Mie 吸収係数 (m⁻¹) を返す。
 *
 * @return Mie 吸収係数 4.4 ×10⁻⁶ m⁻¹。
 */
inline f32  MieAbsorption() noexcept { return 4.4e-6f; }

/**
 * Mie 位相関数の非対称パラメータ g を返す。
 *
 * @return Henyey-Greenstein の g (0.8、前方散乱寄り)。
 */
inline f32  MieG() noexcept          { return 0.8f; }

/**
 * ray-sphere 交差で外側に抜ける距離 t を求める。
 *
 * @details
 * ro は地球中心からのオフセット、rd は単位ベクトル前提。常に大きい方の解
 * (ray が大気圏内にあるとき外側に出る距離) を返す。交差しなければ -1 を返す。
 * @param ro 地球中心を原点とする ray の始点。
 * @param rd 単位長の ray 方向。
 * @param radius 交差を取る球の半径。
 * @return ray 始点から外側交点までの距離 t (交差しなければ -1)。
 */
f32 RaySphereOuter(FVec3 ro, FVec3 rd, f32 radius) noexcept {
    const f32 b = Dot(ro, rd);
    const f32 c = Dot(ro, ro) - radius * radius;
    const f32 disc = b*b - c;
    if (disc < 0) return -1.0f;
    return -b + Sqrt(disc);
}

/**
 * Henyey-Greenstein 位相関数 (Mie 用)。
 *
 * @param cos_theta 入射と散乱方向のなす角のコサイン。
 * @param g 非対称パラメータ (前方/後方散乱の偏り)。
 * @return 与えた角度での散乱位相値。
 */
ACS_FORCEINLINE f32 PhaseHG(f32 cos_theta, f32 g) noexcept {
    const f32 g2 = g * g;
    const f32 d = 1.0f + g2 - 2.0f * g * cos_theta;
    return (1.0f - g2) / (4.0f * kPi * Pow(d, 1.5f));
}

/**
 * Rayleigh 位相関数。
 *
 * @param cos_theta 入射と散乱方向のなす角のコサイン。
 * @return 与えた角度での Rayleigh 位相値。
 */
ACS_FORCEINLINE f32 PhaseRayleigh(f32 cos_theta) noexcept {
    return (3.0f / (16.0f * kPi)) * (1.0f + cos_theta * cos_theta);
}

/**
 * 高度 h での Rayleigh 密度比 (地表で 1)。
 *
 * @param h 地表を 0 とした高度 (m)。
 * @return 高度 h での Rayleigh 密度比。
 */
ACS_FORCEINLINE f32 DensityRayleigh(f32 h) noexcept { return Exp(-h / kRayleighH); }

/**
 * 高度 h での Mie 密度比 (地表で 1)。
 *
 * @param h 地表を 0 とした高度 (m)。
 * @return 高度 h での Mie 密度比。
 */
ACS_FORCEINLINE f32 DensityMie(f32 h)      noexcept { return Exp(-h / kMieH); }

/**
 * 高度 h でのオゾン密度比 (Hillaire/Bruneton)。25km をピークとするテント (10〜40km)。
 * オゾン層は «吸収のみ»。これが «正しい青» と薄明の色 (sunset の青紫帯) を生む。
 * @param h 地表を 0 とした高度 (m)。
 */
ACS_FORCEINLINE f32 DensityOzone(f32 h) noexcept {
    const f32 km = h * 0.001f;
    const f32 dist = (km >= 25.0f) ? (km - 25.0f) : (25.0f - km);
    const f32 d = 1.0f - dist / 15.0f;
    return d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
}

/** オゾン吸収係数 β (RGB、m⁻¹)。Hillaire 標準 (0.650, 1.881, 0.085)×10⁻⁶。散乱はしない (吸収のみ)。 */
inline FVec3 OzoneAbsorption() noexcept {
    return FVec3{0.650e-6f, 1.881e-6f, 0.085e-6f};
}

/**
 * 点 P から方向 dir に距離 t_max 進んだ点までの透過率 T(P, Q) を計算する。
 *
 * @details 累積光学厚さを steps 段で中点積分し、Rayleigh + Mie 消衰から透過率を求める。
 * @param P_earth_centered 地球中心を原点とする始点 P。
 * @param dir 透過率を測る単位方向。
 * @param t_max P から測る距離 (経路長)。
 * @param steps 光学厚さ積分のサンプル段数。
 * @return RGB 透過率 (t_max<=0 または steps==0 なら (1,1,1))。
 */
FVec3 Transmittance(FVec3 P_earth_centered, FVec3 dir, f32 t_max, u32 steps) noexcept {
    if (t_max <= 0.0f || steps == 0) return FVec3{1, 1, 1};
    const f32 step_len = t_max / static_cast<f32>(steps);
    f32 optical_depth_r = 0;
    f32 optical_depth_m = 0;
    f32 optical_depth_o = 0;   // オゾン
    for (u32 i = 0; i < steps; ++i) {
        const FVec3 sample_pos = P_earth_centered + dir * (step_len * (static_cast<f32>(i) + 0.5f));
        f32 alt = Sqrt(Dot(sample_pos, sample_pos)) - kGroundRadius;
        if (alt < 0) alt = 0;
        optical_depth_r += DensityRayleigh(alt) * step_len;
        optical_depth_m += DensityMie(alt)      * step_len;
        optical_depth_o += DensityOzone(alt)    * step_len;
    }
    const FVec3 beta_r = RayleighBeta();
    const f32  beta_m_ext = MieBeta() + MieAbsorption();
    const FVec3 beta_o = OzoneAbsorption();
    const FVec3 tau{
        beta_r.x * optical_depth_r + beta_m_ext * optical_depth_m + beta_o.x * optical_depth_o,
        beta_r.y * optical_depth_r + beta_m_ext * optical_depth_m + beta_o.y * optical_depth_o,
        beta_r.z * optical_depth_r + beta_m_ext * optical_depth_m + beta_o.z * optical_depth_o,
    };
    return FVec3{Exp(-tau.x), Exp(-tau.y), Exp(-tau.z)};
}

/**
 * view ray に沿って太陽からの単散乱を積算し inscatter 輝度を返す。
 *
 * @details
 * view ray r(t) = ro + rd * t に沿って大気上端まで march し、各 sample で太陽方向
 * への透過率と view 側の累積透過率を掛けて Rayleigh / Mie の in-scatter を積算する。
 * @param ro 地球中心を原点とする view ray の始点。
 * @param rd view ray の単位方向。
 * @param sun_dir 太陽方向の単位ベクトル。
 * @param sun_intensity 太陽のピーク輝度 (RGB)。
 * @param ray_steps view ray 沿いのサンプル段数。
 * @param sun_steps 各 sample から太陽への透過率積分の段数。
 * @return view 方向の単散乱輝度 (RGB)。大気と交差しなければ (0,0,0)。
 */
FVec3 SingleScatter(FVec3 ro, FVec3 rd, FVec3 sun_dir, FVec3 sun_intensity,
                  u32 ray_steps, u32 sun_steps) noexcept {
    // 大気圏外との交点まで march
    f32 t_atm = RaySphereOuter(ro, rd, kAtmosphereRadius);
    if (t_atm <= 0) return FVec3{0, 0, 0};

    const f32 cos_view_sun = Dot(rd, sun_dir);
    const f32 phase_r = PhaseRayleigh(cos_view_sun);
    const f32 phase_m = PhaseHG(cos_view_sun, MieG());

    const f32 step_len = t_atm / static_cast<f32>(ray_steps);
    const FVec3 beta_r = RayleighBeta();
    const f32  beta_m = MieBeta();

    FVec3 inscatter_r{0, 0, 0};
    FVec3 inscatter_m{0, 0, 0};
    // view ray 沿いに伝播した累積光学厚さ (T_view)
    f32 view_od_r = 0, view_od_m = 0, view_od_o = 0;
    const FVec3 beta_o = OzoneAbsorption();

    // 注: ray_steps=32 は十分でリング状バンドはほぼ出ず、出力側 TPDF ディザが 8bit バンドを処理する。
    // ここに per-direction ジッタを入れるとベイク (時間平滑化されない) にグレインが残るため midpoint を維持。
    for (u32 i = 0; i < ray_steps; ++i) {
        const FVec3 sample_pos = ro + rd * (step_len * (static_cast<f32>(i) + 0.5f));
        const f32 alt = Sqrt(Dot(sample_pos, sample_pos)) - kGroundRadius;
        if (alt < 0) {
            // 地面に潜った点、寄与なし
            continue;
        }
        const f32 d_r = DensityRayleigh(alt) * step_len;
        const f32 d_m = DensityMie(alt)      * step_len;
        const f32 d_o = DensityOzone(alt)    * step_len;
        view_od_r += d_r;
        view_od_m += d_m;
        view_od_o += d_o;

        // 太陽光線が sample_pos へ届く透過率
        const f32 t_sun = RaySphereOuter(sample_pos, sun_dir, kAtmosphereRadius);
        if (t_sun <= 0) continue;
        const FVec3 T_sun = Transmittance(sample_pos, sun_dir, t_sun, sun_steps);

        // view side 透過率 (現位置までの累積、オゾン消衰込み)
        const f32 beta_m_ext = beta_m + MieAbsorption();
        const FVec3 tau_view{
            beta_r.x * view_od_r + beta_m_ext * view_od_m + beta_o.x * view_od_o,
            beta_r.y * view_od_r + beta_m_ext * view_od_m + beta_o.y * view_od_o,
            beta_r.z * view_od_r + beta_m_ext * view_od_m + beta_o.z * view_od_o,
        };
        const FVec3 T_view{Exp(-tau_view.x), Exp(-tau_view.y), Exp(-tau_view.z)};

        const FVec3 T_combined{T_sun.x * T_view.x, T_sun.y * T_view.y, T_sun.z * T_view.z};
        inscatter_r = inscatter_r + T_combined * (d_r);
        inscatter_m = inscatter_m + T_combined * (d_m);
    }

    // 多重散乱 (Hillaire 近似): 2 次以降の散乱は方向依存が薄れ «等方» に近い。単散乱の蓄積
    // (inscatter_r/m) を等方位相 1/4π で再評価し ms 強度を掛けて加算 → 空が暗くなりすぎず、
    // 地平線/薄明が自然に満ちる (Rayleigh+Mie+ozone の完全 LUT は将来の GPU 化で)。
    const f32 kIso = 1.0f / (4.0f * kPi);
    const f32 kMs  = 0.18f;                  // 多重散乱の強さ (視覚調整、控えめにして白飛び回避)
    const FVec3 result{
        sun_intensity.x * (beta_r.x * (phase_r + kMs * kIso) * inscatter_r.x + beta_m * (phase_m + kMs * kIso) * inscatter_m.x),
        sun_intensity.y * (beta_r.y * (phase_r + kMs * kIso) * inscatter_r.y + beta_m * (phase_m + kMs * kIso) * inscatter_m.y),
        sun_intensity.z * (beta_r.z * (phase_r + kMs * kIso) * inscatter_r.z + beta_m * (phase_m + kMs * kIso) * inscatter_m.z),
    };
    return result;
}

} // namespace

/** equirect 画像の各方向で単散乱を評価し RGBA float 配列を焼く。 */
TArray<f32> FAtmosphere::BakeEquirect(u32 width, u32 height,
                                     const AtmosphereParams& params) noexcept {
    TArray<f32> out;
    out.Resize(static_cast<usize>(width) * height * 4u);

    FVec3 sun_dir;
    {
        f32 l2 = params.sun_dir.x*params.sun_dir.x
               + params.sun_dir.y*params.sun_dir.y
               + params.sun_dir.z*params.sun_dir.z;
        if (l2 < 1e-12f) sun_dir = FVec3{0, 1, 0};
        else {
            f32 inv = 1.0f / Sqrt(l2);
            sun_dir = FVec3{params.sun_dir.x*inv, params.sun_dir.y*inv, params.sun_dir.z*inv};
        }
    }
    // 観察者位置: 地表 + 数 m (camera 高度的なもの)。地球中心が原点なので
    // (0, ground_radius + 2, 0) に置く。
    const FVec3 viewer{0, kGroundRadius + 2.0f, 0};

    for (u32 y = 0; y < height; ++y) {
        const f32 theta = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height) * kPi;
        const f32 sin_t = Sin(theta);
        const f32 cos_t = Cos(theta);
        for (u32 x = 0; x < width; ++x) {
            const f32 phi_norm = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width);
            const f32 phi = phi_norm * 2.0f * kPi - kPi;
            // equirect 規約: theta=0 が +Y、phi=0 が +Z
            const FVec3 view_dir{sin_t * Sin(phi), cos_t, sin_t * Cos(phi)};

            FVec3 col;
            if (view_dir.y < -0.05f) {
                // 地表より下を向く: 簡易 ground (大気の散乱で青みのある暗い色)
                col = FVec3{0.04f, 0.04f, 0.05f};
            } else {
                col = SingleScatter(viewer, view_dir, sun_dir, params.sun_intensity,
                                    params.ray_steps, params.sun_steps);
                // Sun disc: 視線と太陽方向のコサインが小範囲内なら HDR な太陽色を加算。
                // 大気透過 (sun direction での累積) は SingleScatter 内で間接的に考慮されるが、
                // 鋭い disc 自体は別途乗算。Bloom と合わさって光輪 → glare に見える。
                const f32 cos_to_sun = view_dir.x*sun_dir.x + view_dir.y*sun_dir.y + view_dir.z*sun_dir.z;
                // 太陽の見かけ径 cos: 内側 (full) 0.99995、外側 (フェード端) 0.9995
                // → smoothstep で軟らかい disc。bloom firefly 抑制。
                const f32 sun_inner = 0.99995f;
                const f32 sun_outer = 0.9995f;
                if (cos_to_sun > sun_outer) {
                    // 透過率 (太陽光が大気を通って観察者まで届く比率) で disc 輝度を補正
                    f32 t_sun = RaySphereOuter(viewer, sun_dir, kAtmosphereRadius);
                    FVec3 T = (t_sun > 0)
                        ? Transmittance(viewer, sun_dir, t_sun, params.sun_steps)
                        : FVec3{1, 1, 1};
                    // smoothstep(outer, inner, cos): outer で 0、inner で 1
                    const f32 t = Saturate((cos_to_sun - sun_outer) / (sun_inner - sun_outer));
                    const f32 fade = t * t * (3.0f - 2.0f * t);
                    const f32 disc_strength = 30.0f;     // 250→30 (bloom firefly 抑制)
                    col.x += params.sun_intensity.x * T.x * disc_strength * fade;
                    col.y += params.sun_intensity.y * T.y * disc_strength * fade;
                    col.z += params.sun_intensity.z * T.z * disc_strength * fade;
                }
            }

            const u32 idx = (y * width + x) * 4u;
            out[idx + 0] = col.x;
            out[idx + 1] = col.y;
            out[idx + 2] = col.z;
            out[idx + 3] = 1.0f;
        }
    }
    return out;
}

// ===================== GPU Hillaire 大気 (FSkyAtmosphere) =====================
namespace {

/** AtmoCB レイアウト (HLSL の cbuffer AtmoCB と一致)。 */
struct AtmoCB { FVec4 sunDir; FVec4 sunInt; };

/** ApCB レイアウト (HLSL の cbuffer ApCB と一致)。 */
struct ApCB { FMat4 invViewProj; FVec4 camPos; FVec4 sunDir; FVec4 sunInt; FVec4 apParams; };

/** Aerial perspective froxel volume の解像度 (32^3、WickedEngine の AP LUT 相当)。 */
constexpr u32 kApRes = 32;

// 共通 HLSL (km 単位、Hillaire/Bruneton Earth)。各 CS に inline する。
#define ATMO_COMMON_HLSL \
"static const float PI = 3.14159265;\n" \
"static const float kBottom = 6360.0;\n" \
"static const float kTop    = 6460.0;\n" \
"static const float3 kRayS  = float3(5.802, 13.558, 33.1) * 0.001;\n" \
"static const float  kRayH  = 8.0;\n" \
"static const float  kMieS  = 3.996 * 0.001;\n" \
"static const float  kMieE  = 4.440 * 0.001;\n" \
"static const float  kMieH  = 1.2;\n" \
"static const float  kMieG  = 0.8;\n" \
"static const float3 kOzoneA = float3(0.650, 1.881, 0.085) * 0.001;\n" \
"void SampleMedium(float altKm, out float3 sR, out float sM, out float3 ext){\n" \
"  float dR=exp(-altKm/kRayH); float dM=exp(-altKm/kMieH);\n" \
"  float dO=saturate(1.0-abs(altKm-25.0)/15.0);\n" \
"  sR=kRayS*dR; sM=kMieS*dM; ext=sR + (kMieE*dM) + (kOzoneA*dO);\n" \
"}\n" \
"float RayleighPhase(float c){ return 3.0/(16.0*PI)*(1.0+c*c); }\n" \
"float HgPhase(float c,float g){ float g2=g*g; float d=1.0+g2-2.0*g*c; return (1.0-g2)/(4.0*PI*max(pow(max(d,1e-4),1.5),1e-6)); }\n" \
"float RaySphere(float3 ro,float3 rd,float r){ float b=dot(ro,rd); float c=dot(ro,ro)-r*r; float disc=b*b-c; if(disc<0)return -1.0; return -b+sqrt(disc); }\n" \
"float2 TransParamsToUv(float r,float mu){ float H=sqrt(max(kTop*kTop-kBottom*kBottom,0.0)); float rho=sqrt(max(r*r-kBottom*kBottom,0.0)); float disc=r*r*(mu*mu-1.0)+kTop*kTop; float d=max(0.0,-r*mu+sqrt(max(disc,0.0))); float dMin=kTop-r; float dMax=rho+H; float xMu=(dMax>dMin)?(d-dMin)/(dMax-dMin):0.0; float xR=(H>0.0)?rho/H:0.0; return float2(xMu,xR); }\n" \
"void TransUvToParams(float2 uv,out float r,out float mu){ float H=sqrt(max(kTop*kTop-kBottom*kBottom,0.0)); float rho=H*uv.y; r=sqrt(max(rho*rho+kBottom*kBottom,0.0)); float dMin=kTop-r; float dMax=rho+H; float d=dMin+uv.x*(dMax-dMin); mu=(d<=0.0)?1.0:(H*H-rho*rho-d*d)/(2.0*r*d); mu=clamp(mu,-1.0,1.0); }\n"

// Transmittance LUT (256x64)。各 texel = (viewHeight, cosZenith) → 大気上端への透過率。
const char* kTransCS =
ATMO_COMMON_HLSL
"RWTexture2D<float4> transOut : register(u0);\n"
"[numthreads(8,8,1)]\n"
"void CSTrans(uint3 id : SV_DispatchThreadID){\n"
"  const uint W=256,H=64; if(id.x>=W||id.y>=H) return;\n"
"  float2 uv=(float2(id.xy)+0.5)/float2(W,H);\n"
"  float r,mu; TransUvToParams(uv,r,mu);\n"
"  float3 P=float3(0,r,0); float3 dir=float3(sqrt(saturate(1.0-mu*mu)),mu,0);\n"
"  float tTop=RaySphere(P,dir,kTop); if(tTop<=0){ transOut[id.xy]=float4(1,1,1,1); return; }\n"
"  const int N=40; float dt=tTop/N; float3 tau=0;\n"
"  [loop] for(int i=0;i<N;i++){ float3 sp=P+dir*(dt*(i+0.5)); float alt=length(sp)-kBottom; float3 sR; float sM; float3 ext; SampleMedium(max(alt,0.0),sR,sM,ext); tau+=ext*dt; }\n"
"  transOut[id.xy]=float4(exp(-tau),1.0);\n"
"}\n";

// Equirect bake。view dir 毎に Rayleigh+Mie+ozone 単散乱 + 等方多重散乱を積分。Transmittance LUT で太陽透過率を引く。
const char* kBakeCS =
ATMO_COMMON_HLSL
"cbuffer AtmoCB : register(b0){ float4 sunDir; float4 sunInt; };\n"
"Texture2D<float4> transLut : register(t0);\n"
"RWTexture2D<float4> bakeOut : register(u0);\n"
"float3 SampleTrans(float r,float mu){ float2 uv=saturate(TransParamsToUv(r,mu)); int2 px=int2(uv*float2(255.0,63.0)+0.5); return transLut.Load(int3(px,0)).rgb; }\n"
"[numthreads(8,8,1)]\n"
"void CSBake(uint3 id : SV_DispatchThreadID){\n"
"  uint W,H; bakeOut.GetDimensions(W,H); if(id.x>=W||id.y>=H) return;\n"
"  float2 uv=(float2(id.xy)+0.5)/float2(W,H);\n"
"  float theta=uv.y*PI; float phi=uv.x*2.0*PI-PI; float st=sin(theta),ct=cos(theta);\n"
"  float3 dir=float3(st*sin(phi),ct,st*cos(phi)); float3 sd=normalize(sunDir.xyz);\n"
"  float3 col;\n"
"  if(dir.y<-0.02){ col=float3(0.02,0.02,0.03); }\n"
"  else{\n"
"    float3 P0=float3(0,kBottom+0.005,0); float tAtm=RaySphere(P0,dir,kTop);\n"
"    const int N=32; float dt=tAtm/N; float cosVS=dot(dir,sd);\n"
"    float phR=RayleighPhase(cosVS); float phM=HgPhase(cosVS,kMieG);\n"
"    float3 L=0; float3 Tview=float3(1,1,1);\n"
"    [loop] for(int i=0;i<N;i++){\n"
"      float3 P=P0+dir*(dt*(i+0.5)); float r=length(P); float alt=r-kBottom; if(alt<0) break;\n"
"      float3 sR; float sM; float3 ext; SampleMedium(alt,sR,sM,ext);\n"
"      float muSun=dot(P/r,sd); float3 Tsun=SampleTrans(r,muSun);\n"
"      float3 Sdir=sR*phR + sM*phM;\n"
"      float3 Sms=(sR+sM)*(0.18/(4.0*PI));\n"
"      float3 sampleScatter=(Sdir+Sms)*Tsun;\n"
"      float3 sampleT=exp(-ext*dt);\n"
"      float3 Sint=(sampleScatter - sampleScatter*sampleT)/max(ext,1e-7);\n"
"      L+=Tview*Sint; Tview*=sampleT;\n"
"    }\n"
"    col=L*sunInt.xyz;\n"
"    float cosToSun=dot(dir,sd);\n"
"    if(cosToSun>0.9995){ float3 Td=SampleTrans(kBottom+0.005, sd.y); float t=saturate((cosToSun-0.9995)/(0.99995-0.9995)); float fade=t*t*(3.0-2.0*t); col+=sunInt.xyz*Td*30.0*fade; }\n"
"  }\n"
"  bakeOut[id.xy]=float4(col,1.0);\n"
"}\n";

// Aerial perspective camera-volume LUT (WickedEngine skyAtmosphere_cameraVolumeLutCS 相当)。
// 32^3 froxel: xy=screen uv、z=距離スライス (squared 分布で近傍に精度を寄せる)。各 froxel に
// camera→その距離までの大気 in-scatter (Rayleigh+Mie+ozone, sun透過) と平均透過率(.a=1-T) を積分。
// 3D trilinear サンプルなので滑らか = cubemap テクセル境界由来の «斜めの段» が原理的に出ない。
const char* kApCS =
ATMO_COMMON_HLSL
"cbuffer ApCB : register(b0){ float4x4 invViewProj; float4 camPos; float4 sunDir; float4 sunInt; float4 apParams; };\n"
"Texture2D<float4> transLut : register(t0);\n"
"RWTexture3D<float4> apOut : register(u0);\n"
"float3 SampleTrans(float r,float mu){ float2 uv=saturate(TransParamsToUv(r,mu)); int2 px=int2(uv*float2(255.0,63.0)+0.5); return transLut.Load(int3(px,0)).rgb; }\n"
"[numthreads(4,4,4)]\n"
"void CSAp(uint3 id : SV_DispatchThreadID){\n"
"  uint W,H,D; apOut.GetDimensions(W,H,D); if(id.x>=W||id.y>=H||id.z>=D) return;\n"
"  float2 uv=(float2(id.xy)+0.5)/float2(W,H);\n"
"  float sliceN=(float(id.z)+0.5)/float(D); sliceN*=sliceN;          // squared 分布 (近傍密)\n"
"  float tScene=sliceN*apParams.z;                                   // この froxel までの距離 (scene 単位)\n"
"  float4 clip=float4(uv.x*2.0-1.0, -(uv.y*2.0-1.0), 1.0, 1.0);\n"
"  float4 wp=mul(clip, invViewProj); float3 dir=normalize(wp.xyz/wp.w - camPos.xyz);\n"
"  float kmTotal=max(tScene*apParams.x, 1e-4);                       // scene→km\n"
"  float3 P0=float3(0.0, kBottom+apParams.y, 0.0);                   // 大気空間のカメラ高度 (km)\n"
"  float3 sd=normalize(sunDir.xyz); float cosVS=dot(dir,sd);\n"
"  float phR=RayleighPhase(cosVS); float phM=HgPhase(cosVS,kMieG);\n"
"  const int N=16; float dtKm=kmTotal/float(N);\n"
"  float3 L=float3(0,0,0); float3 Tview=float3(1,1,1);\n"
"  [loop] for(int i=0;i<N;i++){\n"
"    float3 P=P0+dir*(dtKm*(float(i)+0.5)); float r=length(P); float alt=r-kBottom; if(alt<0.0) alt=0.0;\n"
"    float3 sR; float sM; float3 ext; SampleMedium(alt,sR,sM,ext);\n"
"    float muSun=dot(P/r,sd); float3 Tsun=SampleTrans(r,muSun);\n"
"    float3 Sdir=sR*phR + sM*phM; float3 Sms=(sR+sM)*(0.18/(4.0*PI));\n"
"    float3 sampleScatter=(Sdir+Sms)*Tsun; float3 sampleT=exp(-ext*dtKm);\n"
"    float3 Sint=(sampleScatter - sampleScatter*sampleT)/max(ext,1e-7);\n"
"    L+=Tview*Sint; Tview*=sampleT;\n"
"  }\n"
"  float3 inScatter=L*sunInt.xyz;\n"
"  float meanT=dot(Tview, float3(1.0/3.0,1.0/3.0,1.0/3.0));\n"
"  apOut[id]=float4(inScatter, 1.0-meanT);\n"             // .rgb=in-scatter, .a=opacity
"}\n";

} // namespace

TResult<void> FSkyAtmosphere::Init(IRhiDevice& device) noexcept {
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Compute; sd.hlsl_source = kTransCS;
        sd.entry_point = "CSTrans"; sd.debug_name = "Atmo.TransCS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_TransCs = Move(r.Value()); }
    {   FComputePipelineDesc pd{}; pd.cs = m_TransCs.Get(); pd.uav_slots = 1; pd.uav_names[0] = "transOut";
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_TransPipe = Move(r.Value()); }
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Compute; sd.hlsl_source = kBakeCS;
        sd.entry_point = "CSBake"; sd.debug_name = "Atmo.BakeCS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_BakeCs = Move(r.Value()); }
    {   FComputePipelineDesc pd{}; pd.cs = m_BakeCs.Get(); pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "AtmoCB";
        pd.srv_slots = 1; pd.srv_names[0] = "transLut"; pd.uav_slots = 1; pd.uav_names[0] = "bakeOut";
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_BakePipe = Move(r.Value()); }
    {   FTextureDesc td{}; td.width = 256; td.height = 64; td.format = EFormat::R16G16B16A16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td); if (r.IsErr()) return Err<void>(r.Error()); m_TransLut = Move(r.Value()); }
    {   FBufferDesc bd{}; bd.size = 256; bd.usage = EBufferUsage::Uniform; bd.cpu_writable = true;
        auto r = CreateRhiBuffer(device, bd); if (r.IsErr()) return Err<void>(r.Error()); m_Cb = Move(r.Value()); }
    // Aerial perspective: froxel volume (32^3 RGBA16F UAV+SRV) + compute pipeline + CB。
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Compute; sd.hlsl_source = kApCS;
        sd.entry_point = "CSAp"; sd.debug_name = "Atmo.ApCS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_ApCs = Move(r.Value()); }
    {   FComputePipelineDesc pd{}; pd.cs = m_ApCs.Get(); pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "ApCB";
        pd.srv_slots = 1; pd.srv_names[0] = "transLut"; pd.uav_slots = 1; pd.uav_names[0] = "apOut";
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_ApPipe = Move(r.Value()); }
    {   FTextureDesc td{}; td.width = kApRes; td.height = kApRes; td.depth = kApRes;
        td.format = EFormat::R16G16B16A16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td); if (r.IsErr()) return Err<void>(r.Error()); m_ApVol = Move(r.Value()); }
    {   FBufferDesc bd{}; bd.size = sizeof(ApCB); bd.usage = EBufferUsage::Uniform; bd.cpu_writable = true;
        auto r = CreateRhiBuffer(device, bd); if (r.IsErr()) return Err<void>(r.Error()); m_ApCb = Move(r.Value()); }
    m_Ready = true;
    return Ok();
}

IRhiTexture* FSkyAtmosphere::BuildAerialPerspective(IRhiDevice& /*device*/, IRhiCommandList& cl,
                                                    const FMat4& inv_view_proj, FVec3 cam_pos,
                                                    FVec3 sun_dir, FVec3 sun_intensity,
                                                    f32 max_dist_scene, f32 scene_to_km,
                                                    f32 cam_alt_km) noexcept {
    if (!m_Ready || !m_ApVol || !m_TransLut) return nullptr;
    FVec3 sd = sun_dir;
    {   f32 l2 = sd.x*sd.x + sd.y*sd.y + sd.z*sd.z;
        if (l2 < 1e-12f) sd = FVec3{0, 1, 0};
        else { f32 inv = 1.0f / Sqrt(l2); sd = FVec3{sd.x*inv, sd.y*inv, sd.z*inv}; } }
    ApCB cb{};
    cb.invViewProj = inv_view_proj;
    cb.camPos = FVec4{cam_pos.x, cam_pos.y, cam_pos.z, 0.0f};
    cb.sunDir = FVec4{sd.x, sd.y, sd.z, 0.0f};
    cb.sunInt = FVec4{sun_intensity.x, sun_intensity.y, sun_intensity.z, 0.0f};
    cb.apParams = FVec4{scene_to_km, cam_alt_km, max_dist_scene, static_cast<f32>(kApRes)};
    m_ApCb->Update(&cb, sizeof(cb));
    // 1) Transmittance LUT を (再)焼く (sun 非依存・定数だが安価)。
    cl.SetComputePipeline(*m_TransPipe);
    cl.BindUav(0, *m_TransLut);
    cl.Dispatch(32, 8, 1);
    // 2) AP froxel volume を焼く。
    cl.SetComputePipeline(*m_ApPipe);
    cl.SetConstantBuffer(0, *m_ApCb);
    cl.SetTexture(0, *m_TransLut);
    cl.BindUav(0, *m_ApVol);
    cl.Dispatch(kApRes / 4, kApRes / 4, kApRes / 4);
    return m_ApVol.Get();
}

bool FSkyAtmosphere::BakeEquirect(IRhiDevice& device, IRhiCommandList& cl,
                                  const AtmosphereParams& params,
                                  u32 width, u32 height, TArray<f32>& out) noexcept {
    if (!m_Ready) return false;
    FVec3 sd = params.sun_dir;
    {   f32 l2 = sd.x*sd.x + sd.y*sd.y + sd.z*sd.z;
        if (l2 < 1e-12f) sd = FVec3{0, 1, 0};
        else { f32 inv = 1.0f / Sqrt(l2); sd = FVec3{sd.x*inv, sd.y*inv, sd.z*inv}; } }
    AtmoCB cb{}; cb.sunDir = FVec4{sd.x, sd.y, sd.z, 0.0f};
    cb.sunInt = FVec4{params.sun_intensity.x, params.sun_intensity.y, params.sun_intensity.z, 0.0f};
    m_Cb->Update(&cb, sizeof(cb));

    // 1) Transmittance LUT を焼く (256x64 → 32x8 グループ)。
    cl.SetComputePipeline(*m_TransPipe);
    cl.BindUav(0, *m_TransLut);
    cl.Dispatch(32, 8, 1);

    // 2) equirect texture を (再) 確保 (RGBA32F、readback 用)。
    if (m_EqW != width || m_EqH != height || !m_Equirect) {
        FTextureDesc td{}; td.width = width; td.height = height;
        td.format = EFormat::R32G32B32A32_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td); if (r.IsErr()) return false;
        m_Equirect = Move(r.Value()); m_EqW = width; m_EqH = height;
    }
    // 3) equirect bake (transLut SRV を読みつつ)。
    cl.SetComputePipeline(*m_BakePipe);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.SetTexture(0, *m_TransLut);
    cl.BindUav(0, *m_Equirect);
    cl.Dispatch((width + 7) / 8, (height + 7) / 8, 1);

    // 4) CPU へ読み戻す (ReadTexture が Flush+WaitIdle → 上の dispatch を実行してから copy)。
    out.Resize(static_cast<usize>(width) * height * 4u);
    return device.ReadTexture(*m_Equirect, out.Data(),
                              static_cast<u32>(out.Size() * sizeof(f32)));
}

void FSkyAtmosphere::Shutdown() noexcept {
    m_TransPipe.Reset(); m_BakePipe.Reset();
    m_TransCs.Reset();   m_BakeCs.Reset();
    m_TransLut.Reset();  m_Equirect.Reset(); m_Cb.Reset();
    m_ApPipe.Reset();    m_ApCs.Reset();     m_ApVol.Reset(); m_ApCb.Reset();   // aerial perspective (UAF 防止)
    m_EqW = 0; m_EqH = 0; m_Ready = false;
}

} // namespace acs
