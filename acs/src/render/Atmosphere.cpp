// SPDX-License-Identifier: Apache-2.0
// Physical atmospheric scattering 実装
#include "render/Atmosphere.h"
#include "math/Math.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <cmath>
#include <cstring>

namespace acs {

namespace {

/** 地表半径 (m、Earth)。 */
constexpr f32 kGroundRadius = kSkyAtmosphereGroundRadiusMeters;

/** 大気上端半径 (m、地表から 100 km)。 */
constexpr f32 kAtmosphereRadius = kSkyAtmosphereTopRadiusMeters;

/** 焼き込み観測者の高度を大気モデルの範囲へ収める。 */
f32 SanitizeBakeAltitude(f32 altitude) noexcept {
    if (!std::isfinite(static_cast<double>(altitude)) || altitude < 0.0f)
        return 0.0f;
    return altitude > kSkyAtmosphereTopAltitudeMeters
        ? kSkyAtmosphereTopAltitudeMeters
        : altitude;
}

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
 * ミー散乱による消散係数（散乱と吸収の合計、m⁻¹）を返す。
 *
 * @return GPU大気表と同じ 4.4 ×10⁻⁶ m⁻¹。散乱係数を追加で足さない。
 */
inline f32 MieExtinction_Internal() noexcept { return 4.4e-6f; }

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

/** ray が球の手前側へ入る距離。地表へ向く環境光線の終端に使う。 */
f32 RaySphereNear(FVec3 ro, FVec3 rd, f32 radius) noexcept {
    const f32 b = Dot(ro, rd);
    const f32 c = Dot(ro, ro) - radius * radius;
    const f32 disc = b*b - c;
    if (disc < 0.0f) return -1.0f;
    const f32 t = -b - Sqrt(disc);
    // 地表上から内向きなら距離0で遮られる。接するだけの地表接線は入口としない。
    return t > 0.0f || (t == 0.0f && b < 0.0f) ? t : -1.0f;
}

/** 指定した大気経路が地表球へ入るなら、太陽光は地面に遮られている。 */
bool IsGroundOccluded(FVec3 ro, FVec3 rd, f32 atmosphere_distance) noexcept {
    const f32 ground_distance = RaySphereNear(ro, rd, kGroundRadius);
    return ground_distance >= 0.0f &&
           ground_distance < atmosphere_distance;
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
 * 高度 h でのオゾン密度比。25km をピークとするテント (10〜40km)。
 * オゾン層は «吸収のみ»。これが «正しい青» と薄明の色 (sunset の青紫帯) を生む。
 * @param h 地表を 0 とした高度 (m)。
 */
ACS_FORCEINLINE f32 DensityOzone(f32 h) noexcept {
    const f32 km = h * 0.001f;
    const f32 dist = (km >= 25.0f) ? (km - 25.0f) : (25.0f - km);
    const f32 d = 1.0f - dist / 15.0f;
    return d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
}

/** オゾン吸収係数 β (RGB、m⁻¹)。(0.650, 1.881, 0.085)×10⁻⁶。散乱はしない (吸収のみ)。 */
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
/** 太陽光路で高度が一方向に変化する区間ごとに、各指数密度を評価する点数。 */
constexpr u32 kSunTransmittanceSteps = 24u;

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
    // 消散は散乱を含むため、吸収だけと取り違えて二重加算しない。
    const f32 beta_m_ext = MieExtinction_Internal();
    const FVec3 beta_o = OzoneAbsorption();
    const FVec3 tau{
        beta_r.x * optical_depth_r + beta_m_ext * optical_depth_m + beta_o.x * optical_depth_o,
        beta_r.y * optical_depth_r + beta_m_ext * optical_depth_m + beta_o.y * optical_depth_o,
        beta_r.z * optical_depth_r + beta_m_ext * optical_depth_m + beta_o.z * optical_depth_o,
    };
    return FVec3{Exp(-tau.x), Exp(-tau.y), Exp(-tau.z)};
}

/** 最低高度から外向きへ進む距離をmで受け取り、半径の増加を差の桁落ちなしで返す。 */
f64 SunRadialRise_Internal(f64 radius, f64 nearest_distance, f64 distance) noexcept {
    // 始点と終点の半径の二乗差。nearest_distanceは球中心への最接近点からの距離。
    const f64 offset = distance * (2.0 * nearest_distance + distance);
    return offset / (::sqrt(radius * radius + offset) + radius);
}

/** 半径の増加をmで受け取り、その高度へ達する外向きの距離を返す。増加0以下は距離0。 */
f64 SunDistanceToRise_Internal(f64 radius, f64 nearest_distance, f64 rise) noexcept {
    if (rise <= 0.0) return 0.0;
    // 指定した高度までの半径の二乗差。
    const f64 offset = rise * (2.0 * radius + rise);
    return offset / (::sqrt(nearest_distance * nearest_distance + offset) + nearest_distance);
}

/** 高度と距離をmで受け取り、指数密度の積分長を返す。stepsは一区間の評価数、距離0以下または0点は積分長0。 */
f64 SunExponentialColumn_Internal(f64 height, f64 nearest_distance, f64 distance, f64 scale_height, u32 steps) noexcept {
    if (distance <= 0.0 || steps == 0u) return 0.0;
    // 積分区間の最低高度における地球中心からの半径。
    const f64 radius = static_cast<f64>(kGroundRadius) + height;
    // 最低高度から終点までの高度差。
    const f64 rise = SunRadialRise_Internal(radius, nearest_distance, distance);
    // 密度が指数的に変化する量。短区間でもexpとの差を直接引かない。
    const f64 density_mass = -::expm1(-rise / scale_height);
    // 密度変化が表現可能な最小値を下回った場合だけ、局所密度一定の極限を使う。
    if (density_mass <= 0.0) return ::exp(-height / scale_height) * distance;
    // 鉛直の光路は密度の原始関数を使い、0/1点指定でも解析的な積分量を保つ。
    if (nearest_distance == radius) return ::exp(-height/scale_height)*scale_height*density_mass;
    // GPUと同じ変換座標uの上端を求める二乗差。
    const f64 offset = 2.0 * radius * scale_height * density_mass;
    // 変換座標uの上端。平方根同士を減算しない。
    const f64 upper = offset / (::sqrt(nearest_distance * nearest_distance + offset) + nearest_distance);
    // 1、2、4、8点Gauss-Legendre則の標本位置。各則の先頭は点数から1を引いた添字。
    constexpr f64 nodes[15] = {0.0, -0.57735026918962576451, 0.57735026918962576451, -0.86113631159405257522, -0.33998104358485626480, 0.33998104358485626480, 0.86113631159405257522, -0.96028985649753623168, -0.79666647741362673959, -0.52553240991632898582, -0.18343464249564980494, 0.18343464249564980494, 0.52553240991632898582, 0.79666647741362673959, 0.96028985649753623168};
    // 各標本の重み。各則で[-1,1]の区間長2へ合計される。
    constexpr f64 weights[15] = {2.0, 1.0, 1.0, 0.34785484513745385737, 0.65214515486254614263, 0.65214515486254614263, 0.34785484513745385737, 0.10122853629037625915, 0.22238103445337447054, 0.31370664587788728734, 0.36268378337836198297, 0.36268378337836198297, 0.31370664587788728734, 0.22238103445337447054, 0.10122853629037625915};
    // 指定された総評価数。区間境界を点数比例で決めるため倍精度へ変換する。
    const f64 sample_count = static_cast<f64>(steps);
    // Jの既知一次成分1+u/rを解析積分する。残差だけを求積し、1点でも鉛直への接続を保つ。
    f64 sum = upper+upper*upper/(2.0*radius);
    // 既に使った評価数。20点なら8、8、4点の順で進む。
    u32 completed = 0u;
    while (completed < steps) {
        // 残りの評価数を超えない最大の求積則を選ぶ。
        const u32 remaining = steps - completed;
        // この部分区間へ割り当てる評価数。
        const u32 order = remaining >= 8u ? 8u : (remaining >= 4u ? 4u : (remaining >= 2u ? 2u : 1u));
        // 選んだ求積則の標本と重みの先頭。
        const u32 table_offset = order - 1u;
        // v区間[0,1]を点数に比例して分割した半幅。
        const f64 half_width = 0.5 * static_cast<f64>(order) / sample_count;
        // この部分区間の中点。累積加算で区間境界をずらさない。
        const f64 middle = (static_cast<f64>(completed) + 0.5 * static_cast<f64>(order)) / sample_count;
        // 選んだ則の標本を順に評価し、指定点数だけ積分する。
        for (u32 index = 0u; index < order; ++index) {
            // u=U*v*(2-v)で高高度端の急変を緩める。評価点数は変えず、微分も重みへ反映する。
            const f64 v = middle + half_width * nodes[table_offset + index];
            const f64 u = upper*v*(2.0-v);
            const f64 upper_remainder = upper*(1.0-v)*(1.0-v);
            // 指数密度の累積量。GPUと同じuへの変数変換を逆にたどる。
            const f64 w = u * (u + 2.0 * nearest_distance) / (2.0 * radius * scale_height);
            // 上端へ寄せた標本でwが1へ丸まらないよう、上端との差から補余量を直接求める。
            const f64 complement = ::exp(-rise/scale_height)+upper_remainder*(upper+u+2.0*nearest_distance)/(2.0*radius*scale_height);
            const f64 radial_rise = -scale_height*(w <= 0.125 ? ::log1p(-w) : ::log(complement));
            // 球中心への最接近点から標本までの距離。
            const f64 ray_distance = ::sqrt(nearest_distance * nearest_distance + radial_rise * (2.0 * radius + radial_rise));
            // 変数変換による長さの倍率。接点の標本が0へ丸まった場合も極限は1。
            const f64 jacobian = ray_distance > 0.0 ? (nearest_distance + u) * (1.0 + radial_rise / radius) / ray_distance : 1.0;
            sum += half_width * weights[table_offset + index] * (jacobian-(1.0+u/radius))*(2.0*upper*(1.0-v));
        }
        completed += order;
    }
    return ::exp(-height / scale_height) * sum;
}

/** 高度と距離をmで受け取り、オゾンの一つの直線的な密度帯を2点則で積分する。重なる高度帯がなければ0。 */
f64 SunOzoneBandColumn_Internal(f64 height, f64 nearest_distance, f64 distance, f64 bottom, f64 top, bool rising) noexcept {
    if (distance <= 0.0 || height >= top) return 0.0;
    // 最低高度における地球中心からの半径。
    const f64 radius = static_cast<f64>(kGroundRadius) + height;
    // 密度帯の下端へ到達する光路上の距離。
    const f64 band_begin = SunDistanceToRise_Internal(radius, nearest_distance, bottom - height);
    // 密度帯の上端へ到達する光路上の距離。
    const f64 band_end = SunDistanceToRise_Internal(radius, nearest_distance, top - height);
    // 高度差が丸まっても元の光路端を保持し、存在しない距離を積分しない。
    const f64 begin = band_begin < distance ? band_begin : distance;
    const f64 end = band_end < distance ? band_end : distance;
    if (end <= begin) return 0.0;
    // 密度帯と重なる区間の半幅。
    const f64 half_width = 0.5 * (end - begin);
    // 密度帯と重なる区間の中点。
    const f64 middle = 0.5 * (end + begin);
    // 2点Gauss-Legendre則の前半標本における高度の増加。
    const f64 rise0 = SunRadialRise_Internal(radius, nearest_distance, middle - half_width * 0.57735026918962576451);
    // 2点Gauss-Legendre則の後半標本における高度の増加。
    const f64 rise1 = SunRadialRise_Internal(radius, nearest_distance, middle + half_width * 0.57735026918962576451);
    // 密度が増える帯と減る帯を、それぞれの一次式で評価した和。
    const f64 sum = rising ? (2.0 * (height - bottom) + rise0 + rise1) / (top - bottom) : (2.0 * (top - height) - rise0 - rise1) / (top - bottom);
    return half_width * sum;
}

/** 高度が一方向に増える光路の光学的厚さをRGBへ加算する。高度と距離はm、距離0以下または0点は加算しない。 */
void SunMonotonicOpticalDepth_Internal(f64 height, f64 nearest_distance, f64 distance, u32 steps, f64 (&optical_depth)[3]) noexcept {
    if (distance <= 0.0 || steps == 0u) return;
    // 分子散乱の指数密度を積んだ長さ。
    const f64 rayleigh = SunExponentialColumn_Internal(height, nearest_distance, distance, static_cast<f64>(kRayleighH), steps);
    // 微粒子による散乱と吸収に共通の指数密度を積んだ長さ。
    const f64 mie = SunExponentialColumn_Internal(height, nearest_distance, distance, static_cast<f64>(kMieH), steps);
    // オゾンは10、25、40kmの折れ目で分け、指数密度の評価数とは独立に積分する。
    const f64 ozone = SunOzoneBandColumn_Internal(height, nearest_distance, distance, 10000.0, 25000.0, true) + SunOzoneBandColumn_Internal(height, nearest_distance, distance, 25000.0, 40000.0, false);
    // 既存CPU経路と同じ、mの逆数で表す分子散乱係数。
    const FVec3 beta_r = RayleighBeta();
    // 散乱を含む微粒子の消散係数。散乱係数を重ねて加えない。
    const f64 beta_m_ext = static_cast<f64>(MieExtinction_Internal());
    // 波長ごとのオゾン吸収係数。
    const FVec3 beta_o = OzoneAbsorption();
    optical_depth[0] += static_cast<f64>(beta_r.x) * rayleigh + beta_m_ext * mie + static_cast<f64>(beta_o.x) * ozone;
    optical_depth[1] += static_cast<f64>(beta_r.y) * rayleigh + beta_m_ext * mie + static_cast<f64>(beta_o.y) * ozone;
    optical_depth[2] += static_cast<f64>(beta_r.z) * rayleigh + beta_m_ext * mie + static_cast<f64>(beta_o.z) * ozone;
}

/** 遮られていない光路の透過率をm単位で積分する。地表遮蔽は呼出し側が判定し、距離0以下・0点・方向長0なら透過率1。 */
FVec3 AtmospherePathTransmittance_Internal(FVec3 origin, FVec3 direction, f32 distance, u32 steps) noexcept {
    if (distance <= 0.0f || steps == 0u) return FVec3{1.0f, 1.0f, 1.0f};
    // 二乗する前に倍精度へ変換し、方向長の丸めを光路長へも反映する。
    const f64 direction_length = ::sqrt(static_cast<f64>(direction.x) * direction.x + static_cast<f64>(direction.y) * direction.y + static_cast<f64>(direction.z) * direction.z);
    if (direction_length <= 0.0) return FVec3{1.0f, 1.0f, 1.0f};
    // 正規化した太陽方向のX成分。
    const f64 direction_x = static_cast<f64>(direction.x) / direction_length;
    // 正規化した太陽方向のY成分。
    const f64 direction_y = static_cast<f64>(direction.y) / direction_length;
    // 正規化した太陽方向のZ成分。
    const f64 direction_z = static_cast<f64>(direction.z) / direction_length;
    // 方向の正規化後も終点が変わらないように補正した経路長。
    const f64 path_length = static_cast<f64>(distance) * direction_length;
    // 地球中心からの位置を単位方向へ射影した距離。
    const f64 projection = static_cast<f64>(origin.x) * direction_x + static_cast<f64>(origin.y) * direction_y + static_cast<f64>(origin.z) * direction_z;
    // 最接近点を有限な光路内へ収めた、始点からの距離。
    const f64 closest = -projection < 0.0 ? 0.0 : (-projection > path_length ? path_length : -projection);
    // 光路の最低高度点のX座標。FVec3へ戻して丸めない。
    const f64 lowest_x = static_cast<f64>(origin.x) + direction_x * closest;
    // 光路の最低高度点のY座標。
    const f64 lowest_y = static_cast<f64>(origin.y) + direction_y * closest;
    // 光路の最低高度点のZ座標。
    const f64 lowest_z = static_cast<f64>(origin.z) + direction_z * closest;
    // 高度の差を計算する地表半径。
    const f64 ground_radius = static_cast<f64>(kGroundRadius);
    // 最低高度点の地球中心からの距離。
    const f64 radial_length = ::sqrt(lowest_x * lowest_x + lowest_y * lowest_y + lowest_z * lowest_z);
    // 半径同士を引かず、二乗差を因数分解して求めた最低高度。
    const f64 raw_height = (lowest_x * lowest_x + lowest_z * lowest_z + (lowest_y - ground_radius) * (lowest_y + ground_radius)) / (radial_length + ground_radius);
    // 既存の密度評価と同じく、地表より低い高度は0へ収める。
    const f64 height = raw_height > 0.0 ? raw_height : 0.0;
    // 球中心への最接近点から、区間の最低高度点までの距離。
    const f64 nearest_distance = ::fabs(projection + closest);
    // 半径が一方向に増える各区間から加算するRGBの光学的厚さ。
    f64 optical_depth[3] = {0.0, 0.0, 0.0};
    SunMonotonicOpticalDepth_Internal(height, nearest_distance, closest, steps, optical_depth);
    if (closest < path_length) {
        SunMonotonicOpticalDepth_Internal(height, nearest_distance, path_length - closest, steps, optical_depth);
    }
    return FVec3{static_cast<f32>(::exp(-optical_depth[0])), static_cast<f32>(::exp(-optical_depth[1])), static_cast<f32>(::exp(-optical_depth[2]))};
}

/** 地表外の有限視線と太陽の平行光から、地球の影の区間をmで求める。影がなければ幅0の分割位置を返す。 */
void ViewShadowInterval_Internal(FVec3 origin, FVec3 direction, FVec3 sun, f64 distance, f64& shadow_begin, f64& shadow_end) noexcept {
    // 太陽方向へ直交する射影を外積で作り、ほぼ平行な方向の1-cos^2の相殺を避ける。
    const f64 vx = static_cast<f64>(direction.y)*sun.z-static_cast<f64>(direction.z)*sun.y;
    const f64 vy = static_cast<f64>(direction.z)*sun.x-static_cast<f64>(direction.x)*sun.z;
    const f64 vz = static_cast<f64>(direction.x)*sun.y-static_cast<f64>(direction.y)*sun.x;
    const f64 ox = static_cast<f64>(origin.y)*sun.z-static_cast<f64>(origin.z)*sun.y;
    const f64 oy = static_cast<f64>(origin.z)*sun.x-static_cast<f64>(origin.x)*sun.z;
    const f64 oz = static_cast<f64>(origin.x)*sun.y-static_cast<f64>(origin.y)*sun.x;
    // 影円柱の内部はa*t^2+2*b*t+c<0。単位長への丸めを前提にしない。
    const f64 a = vx*vx+vy*vy+vz*vz;
    const f64 b = ox*vx+oy*vy+oz*vz;
    const f64 sun_squared = static_cast<f64>(sun.x)*sun.x+static_cast<f64>(sun.y)*sun.y+static_cast<f64>(sun.z)*sun.z;
    const f64 radius = static_cast<f64>(kGroundRadius);
    const f64 c = ox*ox+oy*oy+oz*oz-radius*radius*sun_squared;
    // 平行光で影がない極限では分割を区間端へ収束させる。角度の接近経路に依存する頂点を残さない。
    const f64 vertex = a > 0.0 ? -b/a : 0.0;
    const f64 split = vertex < 0.0 ? 0.0 : (vertex > distance ? distance : vertex);
    shadow_begin = distance;
    shadow_end = distance;
    // 太陽の背面を分ける平面。影なしを返す全分岐で同じ分割規則を使う。
    const f64 plane_origin = static_cast<f64>(origin.x)*sun.x+static_cast<f64>(origin.y)*sun.y+static_cast<f64>(origin.z)*sun.z;
    const f64 plane_direction = static_cast<f64>(direction.x)*sun.x+static_cast<f64>(direction.y)*sun.y+static_cast<f64>(direction.z)*sun.z;
    f64 begin = 0.0;
    f64 end = distance;
    if (a > 0.0) {
        // 判別式は外積の三重積から求め、遠い円柱中心によるb^2-a*cの相殺を避ける。
        const f64 triple = static_cast<f64>(origin.x)*vx+static_cast<f64>(origin.y)*vy+static_cast<f64>(origin.z)*vz;
        const f64 discriminant = sun_squared*(radius*radius*a-triple*triple);
        // 有限区間へ収めた最接近点で、円柱外側と太陽側の隔たりを求める。
        const f64 cylinder_gap = b >= 0.0 ? c : -discriminant/a;
        const f64 plane_value = plane_origin+plane_direction*split;
        const f64 plane_gap = plane_value > 0.0 ? plane_value : 0.0;
        const f64 gap = (cylinder_gap > 0.0 ? cylinder_gap : 0.0)+plane_gap*plane_gap;
        const f64 remaining = distance-split;
        shadow_begin = gap <= 0.0 ? split : (gap >= a*remaining*remaining ? distance : split+::sqrt(gap/a));
        // 最後の加算で終点を1ulp越える場合も、有限光路外を返さない。
        shadow_begin = shadow_begin < distance ? shadow_begin : distance;
        shadow_end = shadow_begin;
        if (discriminant <= 0.0) return;
        const f64 root = ::sqrt(discriminant);
        const f64 q = -b-(b >= 0.0 ? root : -root);
        const f64 first = q/a;
        const f64 second = q != 0.0 ? c/q : 0.0;
        begin = first < second ? first : second;
        end = first < second ? second : first;
    } else if (c >= 0.0) {
        return;
    }
    // 円柱のうち太陽と反対側だけが影。平面上への接触は正の幅を持たない。
    if (plane_direction > 0.0) {
        const f64 crossing = -plane_origin/plane_direction;
        if (crossing < end) end = crossing;
    } else if (plane_direction < 0.0) {
        const f64 crossing = -plane_origin/plane_direction;
        if (crossing > begin) begin = crossing;
    } else if (plane_origin >= 0.0) {
        return;
    }
    begin = begin < 0.0 ? 0.0 : begin;
    end = end > distance ? distance : end;
    if (end <= begin) return;
    shadow_begin = begin;
    shadow_end = end;
}

/** 密度の変化に沿う求積で、同じ散乱点の太陽・視線透過率を積む。距離はm、区間長0以下なら寄与0。 */
FVec3 ViewDensityScattering_Internal(FVec3 origin, FVec3 direction, FVec3 sun_direction, f64 direction_length, f64 closest, f64 nearest_distance, f64 height, f64 distance, f64 traversal_sign, f64 scale_height, u32 steps, u32 sun_steps) noexcept {
    if (distance <= 0.0 || steps == 0u) return FVec3{};
    // 半径が外向きへ増える区間へ変換し、低高度に多い分子・微粒子をそれぞれ積分する。
    const f64 radius = static_cast<f64>(kGroundRadius)+height;
    const f64 rise = SunRadialRise_Internal(radius,nearest_distance,distance);
    const f64 density_mass = -::expm1(-rise/scale_height);
    const f64 offset = 2.0*radius*scale_height*density_mass;
    const f64 upper = density_mass > 0.0 ? offset/(::sqrt(nearest_distance*nearest_distance+offset)+nearest_distance) : distance;
    // 指定点数を1、2、4、8点のGauss-Legendre則へ分ける。物理係数や点数を増やして補正しない。
    constexpr f64 nodes[15] = {0.0,-0.57735026918962576451,0.57735026918962576451,-0.86113631159405257522,-0.33998104358485626480,0.33998104358485626480,0.86113631159405257522,-0.96028985649753623168,-0.79666647741362673959,-0.52553240991632898582,-0.18343464249564980494,0.18343464249564980494,0.52553240991632898582,0.79666647741362673959,0.96028985649753623168};
    // 各則の重みは[-1,1]の幅2へ合計される。
    constexpr f64 weights[15] = {2.0,1.0,1.0,0.34785484513745385737,0.65214515486254614263,0.65214515486254614263,0.34785484513745385737,0.10122853629037625915,0.22238103445337447054,0.31370664587788728734,0.36268378337836198297,0.36268378337836198297,0.31370664587788728734,0.22238103445337447054,0.10122853629037625915};
    // 最低高度の密度を除いた重みで平均を求め、高高度の分母が0へ消えることを避ける。
    f64 sum[3]{};
    f64 weight_sum = 0.0;
    u32 completed = 0u;
    while (completed < steps) {
        // 残り点数に収まる最大の則で、変換後の区間を点数に比例して分ける。
        const u32 remaining = steps-completed;
        const u32 order = remaining >= 8u ? 8u : (remaining >= 4u ? 4u : (remaining >= 2u ? 2u : 1u));
        const u32 table_offset = order-1u;
        const f64 half_width = 0.5*upper*static_cast<f64>(order)/static_cast<f64>(steps);
        const f64 middle = upper*(static_cast<f64>(completed)+0.5*static_cast<f64>(order))/static_cast<f64>(steps);
        for (u32 index = 0u; index < order; ++index) {
            // 密度の累積座標から、実際の光路位置と変数変換の倍率を復元する。
            const f64 u = middle+half_width*nodes[table_offset+index];
            const f64 w = density_mass > 0.0 ? u*(u+2.0*nearest_distance)/(2.0*radius*scale_height) : 0.0;
            const f64 radial_rise = -scale_height*::log1p(-w);
            const f64 radial_distance = ::sqrt(nearest_distance*nearest_distance+radial_rise*(2.0*radius+radial_rise));
            const f64 local_distance = density_mass > 0.0 ? SunDistanceToRise_Internal(radius,nearest_distance,radial_rise) : u;
            const f64 jacobian = density_mass > 0.0 && radial_distance > 0.0 ? (nearest_distance+u)*(1.0+radial_rise/radius)/radial_distance : 1.0;
            // 影判定が標本を0へしても分母から密度を除かない。散乱源と媒質の有無を区別する。
            const f64 weight = half_width*weights[table_offset+index]*jacobian;
            weight_sum += weight;
            // 向きを戻して視点からの距離へ変換する。方向長の丸めも距離と同時に補正する。
            const f32 view_distance = static_cast<f32>((closest+traversal_sign*local_distance)/direction_length);
            const FVec3 sample_position = origin+direction*view_distance;
            const f32 sun_distance = RaySphereOuter(sample_position,sun_direction,kAtmosphereRadius);
            if (sun_distance <= 0.0f || IsGroundOccluded(sample_position,sun_direction,sun_distance)) continue;
            // 視線透過は影区間を含む全経路を評価する。別の標本からの累積状態には依存しない。
            const FVec3 view_t = AtmospherePathTransmittance_Internal(origin,direction,view_distance,sun_steps);
            const FVec3 sun_t = AtmospherePathTransmittance_Internal(sample_position,sun_direction,sun_distance,sun_steps);
            sum[0] += weight*static_cast<f64>(view_t.x)*sun_t.x;
            sum[1] += weight*static_cast<f64>(view_t.y)*sun_t.y;
            sum[2] += weight*static_cast<f64>(view_t.z)*sun_t.z;
        }
        completed += order;
    }
    // 散乱標本の近似で密度の総量を変えない。透過率が一定なら共有の列積分と同じ値を返す。
    const f64 column = SunExponentialColumn_Internal(height,nearest_distance,distance,scale_height,sun_steps);
    const f64 normalization = weight_sum > 0.0 ? column/weight_sum : 0.0;
    return FVec3{static_cast<f32>(sum[0]*normalization),static_cast<f32>(sum[1]*normalization),static_cast<f32>(sum[2]*normalization)};
}

/** 地表外の視線で単散乱を積分する。始点・距離はm、方向は正規化済み。大気と交わらなければRGBを0とする。 */
FVec3 SingleScatter(FVec3 ro, FVec3 rd, FVec3 sun_dir, FVec3 sun_intensity, u32 ray_steps, u32 sun_steps) noexcept {
    // 大気上端までの有限距離。
    f32 t_atm = RaySphereOuter(ro, rd, kAtmosphereRadius);
    if (t_atm <= 0) return FVec3{0, 0, 0};

    const u32 safe_ray_steps = ray_steps > 0u ? ray_steps : 1u;
    const u32 safe_sun_steps = sun_steps > 0u ? sun_steps : 1u;

    const f32 cos_view_sun = Dot(rd, sun_dir);
    const f32 phase_r = PhaseRayleigh(cos_view_sun);
    const f32 phase_m = PhaseHG(cos_view_sun, MieG());

    const FVec3 beta_r = RayleighBeta();
    const f32  beta_m = MieBeta();
    // 方向長を倍精度で補正し、半径が増減する境界で光路を二分する。
    const f64 direction_length = ::sqrt(static_cast<f64>(rd.x)*rd.x+static_cast<f64>(rd.y)*rd.y+static_cast<f64>(rd.z)*rd.z);
    if (direction_length <= 0.0) return FVec3{};
    const f64 path_length = static_cast<f64>(t_atm)*direction_length;
    const f64 projection = (static_cast<f64>(ro.x)*rd.x+static_cast<f64>(ro.y)*rd.y+static_cast<f64>(ro.z)*rd.z)/direction_length;
    const f64 closest = -projection < 0.0 ? 0.0 : (-projection > path_length ? path_length : -projection);
    const f64 lowest_x = ro.x+static_cast<f64>(rd.x)*closest/direction_length;
    const f64 lowest_y = ro.y+static_cast<f64>(rd.y)*closest/direction_length;
    const f64 lowest_z = ro.z+static_cast<f64>(rd.z)*closest/direction_length;
    const f64 radius = ::sqrt(lowest_x*lowest_x+lowest_y*lowest_y+lowest_z*lowest_z);
    const f64 raw_height = radius-static_cast<f64>(kGroundRadius);
    const f64 height = raw_height > 0.0 ? raw_height : 0.0;
    const f64 nearest_distance = ::fabs(projection+closest);
    // 評価点ごとの明暗切替に任せず、有限視線上の影の境界で積分領域を切る。
    f64 shadow_begin = 0.0;
    f64 shadow_end = 0.0;
    ViewShadowInterval_Internal(ro,rd,sun_dir,t_atm,shadow_begin,shadow_end);
    // 総評価数を二つの密度へ分配する。0/1点指定でも各密度は最低1点を保つ。
    const u32 rayleigh_steps = safe_ray_steps/2u+safe_ray_steps%2u;
    const u32 mie_steps = safe_ray_steps/2u > 0u ? safe_ray_steps/2u : 1u;
    FVec3 inscatter_r{};
    FVec3 inscatter_m{};
    for (u32 side = 0u; side < 2u; ++side) {
        // 各区間は最低高度から外向きに求積し、標本の視線上の順序に依存しない。
        const f64 distance = side == 0u ? closest : path_length-closest;
        if (distance <= 0.0) continue;
        const f64 traversal_sign = side == 0u ? -1.0 : 1.0;
        // 影の端を、最低高度から外向きに測る距離へ移す。
        const f64 first_shadow = traversal_sign*(shadow_begin*direction_length-closest);
        const f64 last_shadow = traversal_sign*(shadow_end*direction_length-closest);
        const f64 shadow_low = first_shadow < last_shadow ? first_shadow : last_shadow;
        const f64 shadow_high = first_shadow < last_shadow ? last_shadow : first_shadow;
        // 帯が消えても他の帯の標本数を変えない。高さをまたぐだけで遠方の散乱光が跳ぶ再配分を避ける。
        constexpr f64 ozone_heights[3] = {10000.0,25000.0,40000.0};
        f64 boundaries[5]{};
        for (u32 boundary_index = 0u; boundary_index < 3u; ++boundary_index) {
            const f64 boundary = SunDistanceToRise_Internal(radius,nearest_distance,ozone_heights[boundary_index]-height);
            boundaries[boundary_index+1u] = boundary < distance ? boundary : distance;
        }
        boundaries[4] = distance;
        constexpr u32 band_count = 4u;
        for (u32 band = 0u; band < band_count; ++band) {
            // 各帯の低高度端を原点にし、余った点は低高度側から一つずつ配る。
            const u32 rayleigh_budget = rayleigh_steps/band_count+(band < rayleigh_steps%band_count ? 1u : 0u);
            const u32 mie_budget = mie_steps/band_count+(band < mie_steps%band_count ? 1u : 0u);
            // 影の手前・奥は常に別区間として扱う。影が幅0へ消える場合も点配置を保つ。
            for (u32 piece = 0u; piece < 2u; ++piece) {
                const f64 begin = piece == 0u ? boundaries[band] : (shadow_high > boundaries[band] ? shadow_high : boundaries[band]);
                const f64 end = piece == 0u && shadow_low < boundaries[band+1u] ? shadow_low : boundaries[band+1u];
                const f64 band_distance = end-begin;
                if (band_distance <= 0.0) continue;
                const f64 band_height = height+SunRadialRise_Internal(radius,nearest_distance,begin);
                const f64 band_closest = closest+traversal_sign*begin;
                inscatter_r = inscatter_r+ViewDensityScattering_Internal(ro,rd,sun_dir,direction_length,band_closest,nearest_distance+begin,band_height,band_distance,traversal_sign,kRayleighH,rayleigh_budget > 0u ? rayleigh_budget : 1u,safe_sun_steps);
                inscatter_m = inscatter_m+ViewDensityScattering_Internal(ro,rd,sun_dir,direction_length,band_closest,nearest_distance+begin,band_height,band_distance,traversal_sign,kMieH,mie_budget > 0u ? mie_budget : 1u,safe_sun_steps);
            }
        }
    }

    // CPU代替経路ではGPUの多重散乱表を参照できないため、計算していない
    // 等方光を足さず、Rayleigh/Mieの単散乱と吸収だけを返す。明るさを保つ
    // ための視覚補正を混ぜると、GPU経路と物理的な意味が変わる。
    const FVec3 result{
        sun_intensity.x * (beta_r.x * phase_r * inscatter_r.x + beta_m * phase_m * inscatter_m.x),
        sun_intensity.y * (beta_r.y * phase_r * inscatter_r.y + beta_m * phase_m * inscatter_m.y),
        sun_intensity.z * (beta_r.z * phase_r * inscatter_r.z + beta_m * phase_m * inscatter_m.z),
    };
    return result;
}

/** 指定方向を正規化し、退化していれば天頂方向へ戻す。 */
FVec3 NormalizeAtmosphereDirection(FVec3 value) noexcept {
    const f32 length_squared = Dot(value, value);
    if (!std::isfinite(length_squared) || length_squared <= 1.0e-12f)
        return FVec3{0.0f, 1.0f, 0.0f};
    const f32 inverse_length = 1.0f / Sqrt(length_squared);
    return value * inverse_length;
}

/**
 * 地表球へ当たる下半球 ray を Lambert 地表 + view-path haze として評価する。
 * 真下は地表反射、地平線へ近づくほど長い大気経路の散乱へ連続的に移る。
 */
FVec3 GroundHemisphere(FVec3 viewer, FVec3 view_dir, FVec3 sun_dir,
                       FVec3 sun_intensity, FVec3 ground_albedo, u32 ray_steps,
                       u32 sun_steps) noexcept {
    const f32 t_ground = RaySphereNear(viewer, view_dir, kGroundRadius);
    if (t_ground < 0.0f) {
        return SingleScatter(viewer, view_dir, sun_dir, sun_intensity,
                             ray_steps, sun_steps);
    }

    const FVec3 ground_point = viewer + view_dir * t_ground;
    const f32 ground_len = Sqrt(Dot(ground_point, ground_point));
    const FVec3 ground_normal =
        ground_len > 1.0f ? ground_point * (1.0f / ground_len) : FVec3{0, 1, 0};
    const FVec3 surface_origin = ground_point + ground_normal * 1.0f;
    const FVec3 view_t =
        Transmittance(viewer, view_dir, t_ground, ray_steps);

    FVec3 direct{0, 0, 0};
    const f32 n_dot_l = Dot(ground_normal, sun_dir);
    if (n_dot_l > 0.0f) {
        const f32 t_sun =
            RaySphereOuter(surface_origin, sun_dir, kAtmosphereRadius);
        const FVec3 sun_t =
            AtmospherePathTransmittance_Internal(surface_origin, sun_dir, t_sun, sun_steps);
        const f32 lambert = n_dot_l / kPi;
        direct = FVec3{
            sun_intensity.x * sun_t.x * lambert,
            sun_intensity.y * sun_t.y * lambert,
            sun_intensity.z * sun_t.z * lambert,
        };
    }

    // A near-horizontal sky ray supplies low-frequency skylight and the
    // asymptotic haze colour. It matters most near the geometric horizon;
    // for a downward ray the short view path leaves the ground term dominant.
    const f32 xz_len =
        Sqrt(view_dir.x * view_dir.x + view_dir.z * view_dir.z);
    FVec3 horizon_dir =
        xz_len > 1e-5f
            ? FVec3{view_dir.x / xz_len, 0.002f, view_dir.z / xz_len}
            : FVec3{0.0f, 0.002f, 1.0f};
    const f32 horizon_len = Sqrt(Dot(horizon_dir, horizon_dir));
    horizon_dir = horizon_dir * (1.0f / horizon_len);
    const FVec3 horizon =
        SingleScatter(viewer, horizon_dir, sun_dir, sun_intensity,
                      ray_steps, sun_steps);

    ground_albedo = FVec3{
        ground_albedo.x > 0.0f ? ground_albedo.x : 0.0f,
        ground_albedo.y > 0.0f ? ground_albedo.y : 0.0f,
        ground_albedo.z > 0.0f ? ground_albedo.z : 0.0f,
    };
    const FVec3 ground_radiance{
        ground_albedo.x * (direct.x + horizon.x * 0.35f),
        ground_albedo.y * (direct.y + horizon.y * 0.35f),
        ground_albedo.z * (direct.z + horizon.z * 0.35f),
    };
    return FVec3{
        ground_radiance.x * view_t.x + horizon.x * (1.0f - view_t.x),
        ground_radiance.y * view_t.y + horizon.y * (1.0f - view_t.y),
        ground_radiance.z * view_t.z + horizon.z * (1.0f - view_t.z),
    };
}

} // namespace

FVec3 CAtmosphere::EvaluateSkyRadiance(
    f32 altitude, FVec3 view_dir,
    const FAtmosphereParams& params) noexcept {
    if (!std::isfinite(altitude) || altitude < 0.0f) altitude = 0.0f;
    const FVec3 viewer{0.0f, kGroundRadius + altitude, 0.0f};
    const FVec3 view = NormalizeAtmosphereDirection(view_dir);
    const FVec3 sun = NormalizeAtmosphereDirection(params.sun_dir);
    const u32 ray_steps = params.ray_steps > 0u ? params.ray_steps : 1u;
    const u32 sun_steps = params.sun_steps > 0u ? params.sun_steps : 1u;
    if (RaySphereNear(viewer, view, kGroundRadius) >= 0.0f) {
        return GroundHemisphere(viewer, view, sun, params.sun_intensity,
                                params.ground_albedo, ray_steps, sun_steps);
    }
    return SingleScatter(viewer, view, sun, params.sun_intensity,
                         ray_steps, sun_steps);
}

/**
 * 指定した高度で、太陽方向へ抜ける大気透過率を返す。
 *
 * @details
 * 雲を照らす太陽の «色» を求めるためのもの。太陽光は雲へ届く前に大気を通るので、
 * 低い太陽ほど青が削られて赤くなる。これを掛けないと、夕方でも雲が昼の白さのままになる。
 *
 * 水平方向の位置には依らないものとして扱う (雲の層はごく薄く、太陽の高さが支配的)。
 * @param altitude 地表からの高さ (world 単位)。
 * @param sun_dir 太陽へ向かう単位方向 (上が +Y)。
 * @return RGB 透過率。大気の外や真下向きなら (1,1,1) 側へ寄る。
 */
FVec3 SunTransmittanceAtAltitude(f32 altitude, FVec3 sun_dir) noexcept {
    if (altitude < 0.0f) altitude = 0.0f;

    const FVec3 origin{0.0f, kGroundRadius + altitude, 0.0f};
    const f32 length = Sqrt(Dot(sun_dir, sun_dir));
    if (length <= 0.0f) return FVec3{1.0f, 1.0f, 1.0f};

    const FVec3 dir{sun_dir.x / length, sun_dir.y / length, sun_dir.z / length};

    // 地面に隠れる向きなら光は届かない。
    if (RaySphereNear(origin, dir, kGroundRadius) >= 0.0f) return FVec3{0.0f, 0.0f, 0.0f};

    const f32 distance = RaySphereOuter(origin, dir, kAtmosphereRadius);
    if (distance <= 0.0f) return FVec3{1.0f, 1.0f, 1.0f};

    return AtmospherePathTransmittance_Internal(origin, dir, distance, kSunTransmittanceSteps);
}


/** equirect 画像の各方向で単散乱を評価し RGBA float 配列を焼く。 */
TArray<f32> CAtmosphere::BakeEquirect(u32 width, u32 height,
                                     const FAtmosphereParams& params) noexcept {
    return BakeEquirectAtAltitude(width, height, 2.0f, params);
}

TArray<f32> CAtmosphere::BakeEquirectAtAltitude(
    u32 width, u32 height, f32 altitude,
    const FAtmosphereParams& params) noexcept {
    TArray<f32> out;
    out.SetNum(static_cast<usize>(width) * height * 4u);
    const f32 safe_altitude = SanitizeBakeAltitude(altitude);

    for (u32 y = 0; y < height; ++y) {
        const f32 theta = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height) * kPi;
        const f32 sin_t = Sin(theta);
        const f32 cos_t = Cos(theta);
        for (u32 x = 0; x < width; ++x) {
            const f32 phi_norm = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width);
            const f32 phi = phi_norm * 2.0f * kPi - kPi;
            // equirect 規約: theta=0 が +Y、phi=0 が +Z
            const FVec3 view_dir{sin_t * Sin(phi), cos_t, sin_t * Cos(phi)};

            const FVec3 col = CAtmosphere::EvaluateSkyRadiance(
                safe_altitude, view_dir, params);

            const u32 idx = (y * width + x) * 4u;
            out[idx + 0] = col.x;
            out[idx + 1] = col.y;
            out[idx + 2] = col.z;
            out[idx + 3] = 1.0f;
        }
    }
    return out;
}

// ===================== GPU 物理大気 (CSkyAtmosphere) =====================
namespace {

/** AtmoCB レイアウト (HLSL の cbuffer AtmoCB と一致)。 */
struct FAtmoCB {
    FVec4 sunDir;
    FVec4 sunInt;
    FVec4 groundAlbedo;
};
static_assert(sizeof(FAtmoCB) == 48u, "大気散乱の定数配置はシェーダー側と一致させる");

/** ApCB レイアウト (HLSL の cbuffer ApCB と一致)。 */
struct FApCB {
    FMat4 cameraRelativeInvViewProj;
    FVec4 camPos;
    FVec4 sunDir;
    FVec4 sunInt;
    FVec4 apParams;
    FVec4 fogColorDensity;
    FVec4 fogParams;
};
static_assert(sizeof(FApCB) == 160u, "空気遠近法の定数配置はシェーダー側と一致させる");

/** Fullscreen AP composite の b0。 */
struct FApCompositeCB {
    FMat4 cameraRelativeInvViewProj;
    FVec4 maxDistance;
    // x=0は物体深度、x=1は深度消去済み背景の最遠層を使う。
    FVec4 compositeParams;
};
static_assert(sizeof(FApCompositeCB) == 96u, "空気遠近法合成の定数配置はシェーダー側と一致させる");

ACS_FORCEINLINE f32 FiniteOr(f32 value, f32 fallback) noexcept {
    if (!std::isfinite(static_cast<double>(value))) return fallback;
    // Canonicalize signed zero so exact cache comparison does not turn a
    // numerically identical camera state into a spurious GPU dispatch.
    return value == 0.0f ? 0.0f : value;
}

FVec3 SanitizeVec3(FVec3 value, FVec3 fallback) noexcept {
    return FVec3{
        FiniteOr(value.x, fallback.x),
        FiniteOr(value.y, fallback.y),
        FiniteOr(value.z, fallback.z),
    };
}

FMat4 SanitizeMatrix(const FMat4& value) noexcept {
    FMat4 result = FMat4::Identity();
    for (u32 row = 0; row < 4u; ++row) {
        for (u32 column = 0; column < 4u; ++column) {
            result.m[row][column] =
                FiniteOr(value.m[row][column], result.m[row][column]);
        }
    }
    return result;
}

/** 旧ワールド逆行列からカメラ相対逆行列を作る互換処理。 */
FMat4 BuildCameraRelativeInverseFromWorld_Internal(const FMat4& inv_view_proj, FVec3 cam_pos) noexcept {
    const FMat4 sanitizedInverse = SanitizeMatrix(inv_view_proj);
    const FVec3 sanitizedCamera = SanitizeVec3(cam_pos, FVec3{});
    return sanitizedInverse * FMat4::Translation(FVec3{-sanitizedCamera.x, -sanitizedCamera.y, -sanitizedCamera.z});
}

/** 空気遠近法の視錐台格子。画面48²、深度96層で250 kmまで解像する。 */
constexpr u32 kApXYRes = kSkyAtmosphereFroxelXyResolution;
constexpr u32 kApZRes  = kSkyAtmosphereFroxelZResolution;

// 共通 HLSL (km 単位、地球規模の大気パラメータ)。各 CS に inline する。
// kMieEは散乱を含む消散係数。吸収だけなら (4.4-3.996)*0.001 km⁻¹ になる。
#define ATMO_COMMON_HLSL \
"static const float PI = 3.14159265;\n" \
"static const float kBottom = 6360.0;\n" \
"static const float kTop    = 6460.0;\n" \
"static const float3 kRayS  = float3(5.802, 13.558, 33.1) * 0.001;\n" \
"static const float  kRayH  = 8.0;\n" \
"static const float  kMieS  = 3.996 * 0.001;\n" \
"static const float  kMieE  = 4.4 * 0.001;\n" \
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
"float RaySphere(float3 ro,float3 rd,float r){ float result=-1.0; float b=dot(ro,rd); float c=dot(ro,ro)-r*r; float disc=b*b-c; if(disc>=0.0){ result=-b+sqrt(disc); } return result; }\n" \
"float RaySphereNear(float3 ro,float3 rd,float r){ float result=-1.0; float b=dot(ro,rd); float c=dot(ro,ro)-r*r; float disc=b*b-c; if(disc>=0.0){ float nearT=-b-sqrt(disc); if(nearT>=0.0) result=nearT; } return result; }\n" \
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
"  float tTop=RaySphere(P,dir,kTop);\n"
"  float tGround=RaySphereNear(P,dir,kBottom);\n"
"  if(tTop<=0 || (tGround>=0.0 && tGround<tTop)){ transOut[id.xy]=float4(0,0,0,1); return; }\n"
"  const int N=40; float dt=tTop/N; float3 tau=0;\n"
"  [loop] for(int i=0;i<N;i++){ float3 sp=P+dir*(dt*(i+0.5)); float alt=length(sp)-kBottom; float3 sR; float sM; float3 ext; SampleMedium(max(alt,0.0),sR,sM,ext); tau+=ext*dt; }\n"
"  transOut[id.xy]=float4(exp(-tau),1.0);\n"
"}\n";

// Multi-scattering LUT (32x32)。方向別の二次散乱と等方伝達を積算する。
// texel=(cosSunZenith=uv.x*2-1, viewHeight=bottom+uv.y*(top-bottom))。64 方向 (8x8) を
// azimuth × uniform-cos-polar で等立体角サンプルし、各方向を march して
// «太陽からの 2 次 in-scatter L» と «等方 transfer multiScatAs1» を蓄積。sphereSolidAngle/64
// × isotropicPhase(1/4π) = 1/64 平均。Fms = (L/64)/(1-(multiScatAs1/64)) で無限多重散乱を幾何級数和。
// 球平均と太陽透過率は各経路で一度だけ適用する。
const char* kMultiCS =
ATMO_COMMON_HLSL
"Texture2D<float4> transLut : register(t0);\n"
"RWTexture2D<float4> msOut : register(u0);\n"
"float3 SampleTrans(float r,float mu){\n"
"  float2 p=saturate(TransParamsToUv(r,mu))*float2(255.0,63.0);\n"
"  int2 p0=int2(floor(p)); int2 p1=min(p0+1,int2(255,63)); float2 f=frac(p);\n"
"  float3 a=lerp(transLut.Load(int3(p0.x,p0.y,0)).rgb,transLut.Load(int3(p1.x,p0.y,0)).rgb,f.x);\n"
"  float3 b=lerp(transLut.Load(int3(p0.x,p1.y,0)).rgb,transLut.Load(int3(p1.x,p1.y,0)).rgb,f.x);\n"
"  return lerp(a,b,f.y);\n"
"}\n"
"[numthreads(8,8,1)]\n"
"void CSMulti(uint3 id : SV_DispatchThreadID){\n"
"  const uint W=32,H=32; if(id.x>=W||id.y>=H) return;\n"
"  float2 uv=(float2(id.xy)+0.5)/float2(W,H);\n"
"  float cosSun=uv.x*2.0-1.0;\n"
"  float r=kBottom + uv.y*(kTop-kBottom);\n"
"  float3 P0=float3(0.0,r,0.0);\n"
"  float3 sun=float3(sqrt(saturate(1.0-cosSun*cosSun)), cosSun, 0.0);\n"
"  float3 Lsum=float3(0,0,0); float3 MSsum=float3(0,0,0);\n"
"  const uint SQ=8u;\n"
"  [loop] for(uint s=0u;s<64u;s++){\n"
  "    float u=(float(s%SQ)+0.5)/float(SQ); float v=(float(s/SQ)+0.5)/float(SQ);\n"
  "    float azimuth=2.0*PI*u; float cosPolar=1.0-2.0*v;\n"
  "    float sinPolar=sqrt(saturate(1.0-cosPolar*cosPolar));\n"
  "    float3 dir=float3(cos(azimuth)*sinPolar, sin(azimuth)*sinPolar, cosPolar);\n"
"    float tTop=RaySphere(P0,dir,kTop); float tGround=RaySphereNear(P0,dir,kBottom);\n"
"    float tMax=tTop; bool hitGround=false; if(tGround>0.0 && tGround<tMax){ tMax=tGround; hitGround=true; }\n"
"    if(tMax<=0.0) continue;\n"
"    const int N=20; float dt=tMax/float(N);\n"
"    float cosVS=dot(dir,sun); float phR=RayleighPhase(cosVS); float phM=HgPhase(cosVS,kMieG);\n"
"    float3 L=float3(0,0,0); float3 ms=float3(0,0,0); float3 Tput=float3(1,1,1);\n"
"    [loop] for(int i=0;i<N;i++){\n"
"      float3 P=P0+dir*(dt*(float(i)+0.5)); float rr=length(P); float alt=rr-kBottom; if(alt<0.0) break;\n"
"      float3 sR; float sM; float3 ext; SampleMedium(alt,sR,sM,ext); float3 scat=sR+sM;\n"
"      float muSun=dot(P/rr,sun); float3 Tsun=SampleTrans(rr,muSun);\n"
"      float3 sampleT=exp(-ext*dt);\n"
"      float3 S=Tsun*(sR*phR+sM*phM);\n"                  // globalL=1, MS=0 (1次のみ)
"      float3 Sint=(S - S*sampleT)/max(ext,1e-7); L+=Tput*Sint;\n"
"      float3 MSc=scat;\n"                                // 等方 transfer (位相/太陽なし)
"      float3 MSint=(MSc - MSc*sampleT)/max(ext,1e-7); ms+=Tput*MSint;\n"
"      Tput*=sampleT;\n"
"    }\n"
"    if(hitGround){ float3 Pg=P0+dir*tMax; float rg=length(Pg); float3 ng=Pg/rg; float muG=dot(ng,sun);\n"
"      if(muG>0.0){ float3 TsunG=SampleTrans(rg,muG); L += Tput * TsunG * float3(0.3,0.3,0.3) * (muG/PI); } }\n"
"    Lsum+=L; MSsum+=ms;\n"
"  }\n"
"  float3 InScat=Lsum/64.0; float3 MultiScatAs1=MSsum/64.0;\n"
"  float3 Fms=InScat/max(float3(1,1,1)-MultiScatAs1, 1e-4);\n"
"  msOut[id.xy]=float4(Fms,1.0);\n"
"}\n";

// Equirect bake。view dir 毎に Rayleigh+Mie+ozone 単散乱 + 多重散乱 LUT を積分。Transmittance LUT で太陽透過率を引く。
const char* kBakeCS =
ATMO_COMMON_HLSL
"cbuffer AtmoCB : register(b0){ float4 sunDir; float4 sunInt; float4 groundAlbedo; };\n"
"Texture2D<float4> transLut : register(t0);\n"
"Texture2D<float4> multiLut : register(t1);\n"
"RWTexture2D<float4> bakeOut : register(u0);\n"
"float3 SampleTrans(float r,float mu){\n"
"  float2 p=saturate(TransParamsToUv(r,mu))*float2(255.0,63.0);\n"
"  int2 p0=int2(floor(p)); int2 p1=min(p0+1,int2(255,63)); float2 f=frac(p);\n"
"  float3 a=lerp(transLut.Load(int3(p0.x,p0.y,0)).rgb,transLut.Load(int3(p1.x,p0.y,0)).rgb,f.x);\n"
"  float3 b=lerp(transLut.Load(int3(p0.x,p1.y,0)).rgb,transLut.Load(int3(p1.x,p1.y,0)).rgb,f.x);\n"
"  return lerp(a,b,f.y);\n"
"}\n"
"float3 SampleMulti(float r,float mu){\n"
"  float2 uv=float2(mu*0.5+0.5,saturate((r-kBottom)/(kTop-kBottom)));\n"
"  float2 p=saturate(uv)*31.0; int2 p0=int2(floor(p)); int2 p1=min(p0+1,int2(31,31)); float2 f=frac(p);\n"
"  float3 a=lerp(multiLut.Load(int3(p0.x,p0.y,0)).rgb,multiLut.Load(int3(p1.x,p0.y,0)).rgb,f.x);\n"
"  float3 b=lerp(multiLut.Load(int3(p0.x,p1.y,0)).rgb,multiLut.Load(int3(p1.x,p1.y,0)).rgb,f.x);\n"
"  return lerp(a,b,f.y);\n"
"}\n"
"[numthreads(8,8,1)]\n"
"void CSBake(uint3 id : SV_DispatchThreadID){\n"
"  uint W,H; bakeOut.GetDimensions(W,H); if(id.x>=W||id.y>=H) return;\n"
"  float2 uv=(float2(id.xy)+0.5)/float2(W,H);\n"
"  float theta=uv.y*PI; float phi=uv.x*2.0*PI-PI; float st=sin(theta),ct=cos(theta);\n"
"  float3 dir=float3(st*sin(phi),ct,st*cos(phi)); float3 sd=normalize(sunDir.xyz);\n"
"  float3 P0=float3(0,kBottom+max(groundAlbedo.w,0.0),0); float tAtm=RaySphere(P0,dir,kTop);\n"
"  float tGround=RaySphereNear(P0,dir,kBottom);\n"
"  bool hitGround=tGround>0.0 && tGround<tAtm; float tMax=hitGround?tGround:tAtm;\n"
"  float3 col=float3(0,0,0);\n"
"  if(tMax>0.0){\n"
"    const int N=32; float dt=tMax/N; float cosVS=dot(dir,sd);\n"
"    float phR=RayleighPhase(cosVS); float phM=HgPhase(cosVS,kMieG);\n"
"    float3 L=0; float3 Tview=float3(1,1,1);\n"
"    [loop] for(int i=0;i<N;i++){\n"
"      float3 P=P0+dir*(dt*(i+0.5)); float r=length(P); float alt=r-kBottom; if(alt<0) break;\n"
"      float3 sR; float sM; float3 ext; SampleMedium(alt,sR,sM,ext);\n"
"      float muSun=dot(P/r,sd); float3 Tsun=SampleTrans(r,muSun);\n"
"      float3 MS=SampleMulti(r,muSun);\n"                 // 多重散乱 LUT (WE GetMultipleScattering)
"      float3 Sdir=sR*phR + sM*phM;\n"
"      float3 sampleScatter=Sdir*Tsun + MS*(sR+sM);\n"    // WE: S=Tsun*phaseScat + MS*scattering
"      float3 sampleT=exp(-ext*dt);\n"
"      float3 Sint=(sampleScatter - sampleScatter*sampleT)/max(ext,1e-7);\n"
"      L+=Tview*Sint; Tview*=sampleT;\n"
"    }\n"
"    if(hitGround){\n"
"      float3 Pg=P0+dir*tGround; float rg=max(length(Pg),kBottom); float3 ng=Pg/rg;\n"
"      float muG=max(dot(ng,sd),0.0); float3 TsunG=muG>0.0?SampleTrans(rg,muG):float3(0,0,0);\n"
"      float3 skyIrradiance=max(SampleMulti(rg,muG),0.0)*0.25;\n"
"      float3 groundUnit=max(groundAlbedo.xyz,0.0)*(TsunG*(muG/PI)+skyIrradiance);\n"
"      L+=Tview*groundUnit;\n"
"    }\n"
"    col=L*sunInt.xyz;\n"
"  }\n"
"  col=max(col,0.0);\n"
"  bakeOut[id.xy]=float4(col,1.0);\n"
"}\n";

// aerial perspective + local height fog の camera-volume LUT。
// 48x48x96 froxel、24-step stratified integration。大気 LUT は bilinear 参照し、最近傍由来の
// バンディングを除去する。物理 AP は premultiplied in-scatter と RGB transmittance を別UAVへ、
// local fog は従来の scalar opacity 付き volume へ書く。
const char* kApCS =
ATMO_COMMON_HLSL
R"(
#pragma pack_matrix(row_major)
cbuffer ApCB : register(b0) {
  float4x4 cameraRelativeInvViewProj;
  float4 camPos;
  float4 sunDir;
  float4 sunInt;
  float4 apParams;          // x=シーン→km係数, y=カメラ高度(km), z=最大距離
  float4 fogColorDensity;   // xyz=散乱アルベド, w=シーン単位あたりの消散係数
  float4 fogParams;         // x=高度減衰, y=基準高度, z=HG g, w=太陽散乱
};
Texture2D<float4> transLut : register(t0);
Texture2D<float4> multiLut : register(t1);
RWTexture3D<float4> apOut : register(u0);
RWTexture3D<float4> apTransOut : register(u1);

float3 LoadTransBilinear(float2 uv) {
  float2 p=saturate(uv)*float2(255.0,63.0);
  int2 p0=int2(floor(p)); int2 p1=min(p0+1,int2(255,63)); float2 f=frac(p);
  float3 a=lerp(transLut.Load(int3(p0.x,p0.y,0)).rgb,
                transLut.Load(int3(p1.x,p0.y,0)).rgb,f.x);
  float3 b=lerp(transLut.Load(int3(p0.x,p1.y,0)).rgb,
                transLut.Load(int3(p1.x,p1.y,0)).rgb,f.x);
  return lerp(a,b,f.y);
}
float3 LoadMultiBilinear(float2 uv) {
  float2 p=saturate(uv)*float2(31.0,31.0);
  int2 p0=int2(floor(p)); int2 p1=min(p0+1,int2(31,31)); float2 f=frac(p);
  float3 a=lerp(multiLut.Load(int3(p0.x,p0.y,0)).rgb,
                multiLut.Load(int3(p1.x,p0.y,0)).rgb,f.x);
  float3 b=lerp(multiLut.Load(int3(p0.x,p1.y,0)).rgb,
                multiLut.Load(int3(p1.x,p1.y,0)).rgb,f.x);
  return lerp(a,b,f.y);
}
float3 SampleTrans(float r,float mu) {
  return LoadTransBilinear(TransParamsToUv(r,mu));
}
float3 SampleMulti(float r,float mu) {
  return LoadMultiBilinear(float2(mu*0.5+0.5,saturate((r-kBottom)/(kTop-kBottom))));
}
float Hash13(uint3 p) {
  return frac(sin(dot(float3(p),float3(12.9898,78.233,37.719)))*43758.5453);
}
float FogPhase(float cosTheta,float g) {
  g=clamp(g,-0.85,0.85); float g2=g*g;
  float d=max(1.0+g2-2.0*g*cosTheta,1e-3);
  return (1.0-g2)/(d*sqrt(d));
}

float3 CameraRelativeViewDirection(float2 ndc) {
  // 透視投影の遠点が無限遠になっても、同次xyzは正しい視線を保持する。
  float4 farHomogeneous=mul(
      float4(ndc,1.0,1.0),cameraRelativeInvViewProj);
  bool perspective=abs(cameraRelativeInvViewProj[2][3])>1.0e-7;
  float3 candidate=perspective
      ?farHomogeneous.xyz
      :mul(float4(0.0,0.0,1.0,0.0),
           cameraRelativeInvViewProj).xyz;
  float lengthSquared=dot(candidate,candidate);
  return lengthSquared>1.0e-12&&lengthSquared<3.0e38
      ?candidate*rsqrt(lengthSquared):float3(0.0,0.0,1.0);
}

void IntegrateAp(uint3 id,uint W,uint H,uint D,
                 out float3 L,out float3 Tview) {
  float2 uv=(float2(id.xy)+0.5)/float2(W,H);
  float sliceN=(float(id.z)+0.5)/float(D);
  float tScene=sliceN*sliceN*apParams.z;                   // 近傍を密にする深度分布
  float2 ndc=float2(uv.x*2.0-1.0,-(uv.y*2.0-1.0));
  float3 dir=CameraRelativeViewDirection(ndc);
  float3 P0=float3(0.0,kBottom+apParams.y,0.0);
  float3 sd=normalize(sunDir.xyz);
  float cosVS=dot(dir,sd);
  bool atmosphereEnabled=apParams.x>0.0;
  bool localFogEnabled=fogColorDensity.w>0.0;
  float phR=0.0, phM=0.0;
  if(atmosphereEnabled) {
    phR=RayleighPhase(cosVS);
    phM=HgPhase(cosVS,kMieG);
  }
  float fogPhase=0.0;
  float3 sunTint=float3(0.0,0.0,0.0);
  if(localFogEnabled) {
    fogPhase=FogPhase(cosVS,fogParams.z);
    float sunLum=max(dot(max(sunInt.xyz,0.0),float3(0.2126,0.7152,0.0722)),1e-4);
    sunTint=max(sunInt.xyz,0.0)/sunLum;
  }

  const int N=24;
  float dtScene=tScene/float(N);
  float jitter=0.15+0.70*Hash13(id);                       // 時間方向にちらつかない安定した層化
  L=float3(0,0,0); Tview=float3(1,1,1);
  [loop] for(int i=0;i<N;i++) {
    float t=dtScene*(float(i)+jitter);
    float3 atmosphereExtinction=float3(0.0,0.0,0.0);
    float3 atmosphereScatter=float3(0.0,0.0,0.0);
    if(atmosphereEnabled) {
      float3 P=P0+dir*(t*apParams.x);
      float r=length(P), alt=max(r-kBottom,0.0);
      float3 sR; float sM; float3 extKm; SampleMedium(alt,sR,sM,extKm);
      float muSun=dot(P/max(r,1e-5),sd);
      float3 Tsun=SampleTrans(r,muSun), MS=SampleMulti(r,muSun);
      atmosphereExtinction=extKm*apParams.x;
      atmosphereScatter=((sR*phR+sM*phM)*Tsun+MS*(sR+sM))*sunInt.xyz*apParams.x;
    }

    float fogD=0.0;
    float3 fogLight=float3(0.0,0.0,0.0);
    if(localFogEnabled) {
      float worldY=camPos.y+dir.y*t;
      fogD=fogColorDensity.w*exp(-min(max(fogParams.x,0.0)*max(worldY-fogParams.y,0.0),80.0));
      fogLight=fogColorDensity.xyz*(1.0+sunTint*(fogPhase*max(fogParams.w,0.0)));
    }

    // scene-unit basis へ揃えて大気と local fog を同一 Beer-Lambert step で積分。
    float3 totalExt=atmosphereExtinction+fogD;
    float3 totalScatter=atmosphereScatter+fogLight*fogD;
    float3 sampleT=exp(-min(totalExt*dtScene,80.0));
    float3 Sint=totalScatter*(1.0-sampleT)/max(totalExt,1e-7);
    L+=Tview*Sint;
    Tview*=sampleT;
  }
}
[numthreads(4,4,4)]
void CSAp(uint3 id : SV_DispatchThreadID) {
  uint W,H,D; apOut.GetDimensions(W,H,D); if(id.x>=W||id.y>=H||id.z>=D) return;
  float3 L,Tview; IntegrateAp(id,W,H,D,L,Tview);
  float meanT=dot(Tview,float3(1.0/3.0,1.0/3.0,1.0/3.0));
  apOut[id]=float4(L,saturate(1.0-meanT));
  apTransOut[id]=float4(saturate(Tview),1.0);
}
[numthreads(4,4,4)]
void CSLocalFog(uint3 id : SV_DispatchThreadID) {
  uint W,H,D; apOut.GetDimensions(W,H,D); if(id.x>=W||id.y>=H||id.z>=D) return;
  float3 L,Tview; IntegrateAp(id,W,H,D,L,Tview);
  float meanT=dot(Tview,float3(1.0/3.0,1.0/3.0,1.0/3.0));
  apOut[id]=float4(L,saturate(1.0-meanT));
}
)";

// Camera-volume を scene depth で終端して HDR scene へ適用する fullscreen pass。
// physical AP は RGB multiply + premultiplied additive、local fog は scalar alpha blend。
const char* kApCompositeVS = R"(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
  float2 uv = float2((id << 1) & 2, id & 2);
  VSOut o;
  o.uv = uv;
  o.pos = float4(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0), 0.0, 1.0);
  return o;
}
)";

const char* kApCompositePS = R"(
#pragma pack_matrix(row_major)
cbuffer ApCompositeCB : register(b0) {
  float4x4 cameraRelativeInvViewProj;
  float4 maxDistance;
  float4 compositeParams;
};
Texture2D sceneDepth : register(t0);
Texture3D apVolume : register(t1);
Texture2D cloudDepth : register(t2);
SamplerState sceneDepth_sampler : register(s0);
SamplerState apVolume_sampler : register(s1);
SamplerState cloudDepth_sampler : register(s2);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float PhysicalSlice(VSOut v) {
  float depth = sceneDepth.SampleLevel(sceneDepth_sampler, v.uv, 0.0).r;
  if (depth >= 1.0) discard;
  float2 ndc = float2(v.uv.x * 2.0 - 1.0, 1.0 - v.uv.y * 2.0);
  float4 cameraRelativePosition = mul(float4(ndc, depth, 1.0), cameraRelativeInvViewProj);
  float safeW = abs(cameraRelativePosition.w) > 1e-6 ? cameraRelativePosition.w : (cameraRelativePosition.w < 0.0 ? -1e-6 : 1e-6);
  float3 cameraRelativeSurface = cameraRelativePosition.xyz / safeW;
  float maxDist = max(maxDistance.x, 1e-3);
  float dist = length(cameraRelativeSurface);
  return sqrt(saturate(dist / maxDist));
}
float4 PSMultiply(VSOut v) : SV_TARGET {
  float slice = PhysicalSlice(v);
  float4 transfer = apVolume.SampleLevel(
      apVolume_sampler, float3(v.uv, slice), 0.0);
  bool valid = transfer.a >= 0.5 && transfer.a <= 1.01
            && all(transfer.rgb == transfer.rgb)
            && all(transfer.rgb >= 0.0) && all(transfer.rgb <= 1.001);
  // A missing/unbound u1 is normally zero-filled. Never multiply the scene by
  // that invalid transfer: fail open to identity until the volume is valid.
  return valid ? float4(saturate(transfer.rgb), 1.0)
               : float4(1.0, 1.0, 1.0, 1.0);
}
float4 PSAddScatter(VSOut v) : SV_TARGET {
  float slice = PhysicalSlice(v);
  float3 inScatter = apVolume.SampleLevel(
      apVolume_sampler, float3(v.uv, slice), 0.0).rgb;
  bool valid = all(inScatter == inScatter)
            && all(abs(inScatter) < 65504.0);
  return float4(valid ? max(inScatter, 0.0) : float3(0,0,0), 1.0);
}
float4 PSMain(VSOut v) : SV_TARGET {
  float depth = sceneDepth.SampleLevel(sceneDepth_sampler, v.uv, 0.0).r;
  float maxDist = max(maxDistance.x, 1e-3);
  float dist;
  if (depth >= 1.0) {
    // 物理大気では、すでに散乱を焼いた空を変更しない。
    // Local-fog mode covers sky/cloud pixels. Clouds use their resolved
    // distance; only a clear sky reaches the local volume's far slice.
    if (compositeParams.x <= 0.5) discard;
    dist = maxDist;
    if (compositeParams.y > 0.5) {
      float resolvedCloudDepth =
          cloudDepth.SampleLevel(cloudDepth_sampler, v.uv, 0.0).r;
      if (resolvedCloudDepth <= 250000.0)
        dist = min(dist, resolvedCloudDepth);
    }
  } else {
    // 物体上の大気は、復元した表面までの距離で必ず終端する。
    float2 ndc = float2(v.uv.x * 2.0 - 1.0, 1.0 - v.uv.y * 2.0);
    float4 cameraRelativePosition = mul(float4(ndc, depth, 1.0), cameraRelativeInvViewProj);
    float safeW = abs(cameraRelativePosition.w) > 1e-6 ? cameraRelativePosition.w : (cameraRelativePosition.w < 0.0 ? -1e-6 : 1e-6);
    float3 cameraRelativeSurface = cameraRelativePosition.xyz / safeW;
    dist = length(cameraRelativeSurface);
  }
  float slice = sqrt(saturate(dist / maxDist));
  float4 ap = apVolume.SampleLevel(apVolume_sampler, float3(v.uv, slice), 0.0);
  float opacity = saturate(ap.a);
  float3 straightScatter = opacity > 1e-5 ? max(ap.rgb, 0.0) / opacity : 0.0;
  return float4(straightScatter, opacity);
}
)";

} // namespace

bool CSkyAtmosphere::SameVolumeCacheKey(
    const FVolumeCacheKey& lhs,
    const FVolumeCacheKey& rhs) noexcept {
    return std::memcmp(&lhs, &rhs, sizeof(FVolumeCacheKey)) == 0;
}

TResult<void> CSkyAtmosphere::Init(IRhiDevice& device, EFormat hdr_format) noexcept {
    m_Ready = false;
    m_LutsReady = false;
    m_LocalFogVolumeValid = false;
    m_LocalFogMaxDistance = kLocalVolumetricFogMaxDistance;
    m_PhysicalApCacheValid = false;
    m_LocalFogCacheValid = false;
    m_PhysicalApDispatchCount = 0;
    m_LocalFogDispatchCount = 0;
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Compute; sd.hlsl_source = kTransCS;
        sd.entry_point = "CSTrans"; sd.debug_name = "Atmo.TransCS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_TransCs = Move(r.Value()); }
    {   FComputePipelineDesc pd{}; pd.cs = m_TransCs.Get(); pd.uav_slots = 1; pd.uav_names[0] = "transOut";
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_TransPipe = Move(r.Value()); }
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Compute; sd.hlsl_source = kBakeCS;
        sd.entry_point = "CSBake"; sd.debug_name = "Atmo.BakeCS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_BakeCs = Move(r.Value()); }
    {   FComputePipelineDesc pd{}; pd.cs = m_BakeCs.Get(); pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "AtmoCB";
        pd.srv_slots = 2; pd.srv_names[0] = "transLut"; pd.srv_names[1] = "multiLut"; pd.uav_slots = 1; pd.uav_names[0] = "bakeOut";
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_BakePipe = Move(r.Value()); }
    {   FTextureDesc td{}; td.width = 256; td.height = 64; td.format = EFormat::R16G16B16A16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td); if (r.IsErr()) return Err<void>(r.Error()); m_TransLut = Move(r.Value()); }
    // Multi-scattering LUT (32x32 RGBA16F UAV/SRV) + compute pipeline (WE multiScatteredLuminanceLut)。
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Compute; sd.hlsl_source = kMultiCS;
        sd.entry_point = "CSMulti"; sd.debug_name = "Atmo.MultiCS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_MultiCs = Move(r.Value()); }
    {   FComputePipelineDesc pd{}; pd.cs = m_MultiCs.Get(); pd.srv_slots = 1; pd.srv_names[0] = "transLut";
        pd.uav_slots = 1; pd.uav_names[0] = "msOut";
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_MultiPipe = Move(r.Value()); }
    {   FTextureDesc td{}; td.width = 32; td.height = 32; td.format = EFormat::R16G16B16A16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td); if (r.IsErr()) return Err<void>(r.Error()); m_MultiLut = Move(r.Value()); }
    {   FBufferDesc bd{}; bd.size = 256; bd.usage = EBufferUsage::Uniform; bd.cpu_writable = true;
        auto r = CreateRhiBuffer(device, bd); if (r.IsErr()) return Err<void>(r.Error()); m_Cb = Move(r.Value()); }
    // aerial perspective + local fog 用に 48x48x96 RGBA16F froxel volume、
    // compute pipeline、CB を構築する。
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Compute; sd.hlsl_source = kApCS;
        sd.entry_point = "CSAp"; sd.debug_name = "Atmo.ApCS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_ApCs = Move(r.Value()); }
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Compute; sd.hlsl_source = kApCS;
        sd.entry_point = "CSLocalFog"; sd.debug_name = "Atmo.LocalFogCS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_LocalFogCs = Move(r.Value()); }
    {   FComputePipelineDesc pd{}; pd.cs = m_ApCs.Get(); pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "ApCB";
        pd.srv_slots = 2; pd.srv_names[0] = "transLut"; pd.srv_names[1] = "multiLut";
        pd.uav_slots = 2; pd.uav_names[0] = "apOut"; pd.uav_names[1] = "apTransOut";
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_ApPipe = Move(r.Value()); }
    {   FComputePipelineDesc pd{}; pd.cs = m_LocalFogCs.Get(); pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "ApCB";
        pd.srv_slots = 2; pd.srv_names[0] = "transLut"; pd.srv_names[1] = "multiLut";
        pd.uav_slots = 1; pd.uav_names[0] = "apOut";
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_LocalFogPipe = Move(r.Value()); }
    {   FTextureDesc td{}; td.width = kApXYRes; td.height = kApXYRes; td.depth = kApZRes;
        td.format = EFormat::R16G16B16A16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td); if (r.IsErr()) return Err<void>(r.Error()); m_ApVol = Move(r.Value()); }
    {   FTextureDesc td{}; td.width = kApXYRes; td.height = kApXYRes; td.depth = kApZRes;
        td.format = EFormat::R16G16B16A16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td); if (r.IsErr()) return Err<void>(r.Error()); m_ApTransVol = Move(r.Value()); }
    {   FBufferDesc bd{}; bd.size = sizeof(FApCB); bd.usage = EBufferUsage::Uniform; bd.cpu_writable = true;
        auto r = CreateRhiBuffer(device, bd); if (r.IsErr()) return Err<void>(r.Error()); m_ApCb = Move(r.Value()); }
    {   FTextureDesc td{}; td.width = kApXYRes; td.height = kApXYRes; td.depth = kApZRes;
        td.format = EFormat::R16G16B16A16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td); if (r.IsErr()) return Err<void>(r.Error()); m_LocalFogVol = Move(r.Value()); }
    {   FBufferDesc bd{}; bd.size = sizeof(FApCB); bd.usage = EBufferUsage::Uniform; bd.cpu_writable = true;
        auto r = CreateRhiBuffer(device, bd); if (r.IsErr()) return Err<void>(r.Error()); m_LocalFogCb = Move(r.Value()); }
    // fullscreen depth-aware AP composite。この pass では depth を SRV として sample し、
    // DSV には bind しない。
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Vertex; sd.hlsl_source = kApCompositeVS;
        sd.entry_point = "VSMain"; sd.debug_name = "Atmo.ApCompositeVS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_ApCompositeVs = Move(r.Value()); }
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Pixel; sd.hlsl_source = kApCompositePS;
        sd.entry_point = "PSMain"; sd.debug_name = "Atmo.ApCompositePS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_ApCompositePs = Move(r.Value()); }
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Pixel; sd.hlsl_source = kApCompositePS;
        sd.entry_point = "PSMultiply"; sd.debug_name = "Atmo.ApMultiplyPS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_ApMultiplyPs = Move(r.Value()); }
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Pixel; sd.hlsl_source = kApCompositePS;
        sd.entry_point = "PSAddScatter"; sd.debug_name = "Atmo.ApAddScatterPS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_ApAddPs = Move(r.Value()); }
    {   FPipelineDesc pd{};
        pd.vs = m_ApCompositeVs.Get(); pd.ps = m_ApCompositePs.Get();
        pd.topology = EPrimitiveTopology::TriangleList;
        pd.rt_format = hdr_format; pd.depth_format = EFormat::Unknown;
        pd.depth_test = false; pd.depth_write = false;
        pd.cull_mode = ECullMode::None; pd.blend_mode = EBlendMode::AlphaBlend;
        pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "ApCompositeCB";
        pd.texture_slots = 3;
        pd.texture_names[0] = "sceneDepth";
        pd.texture_names[1] = "apVolume";
        pd.texture_names[2] = "cloudDepth";
        pd.static_sampler_count = 3;
        pd.static_samplers[0].filter = ESamplerFilter::Point;
        pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[1].filter = ESamplerFilter::Linear;
        pd.static_samplers[1].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[1].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[1].address_w = ESamplerAddress::Clamp;
        pd.static_samplers[2].filter = ESamplerFilter::Point;
        pd.static_samplers[2].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[2].address_v = ESamplerAddress::Clamp;
        pd.layout_count = 0; pd.vertex_stride = 0;
        auto r = CreateRhiPipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_ApCompositePipe = Move(r.Value()); }
    {   FPipelineDesc pd{};
        pd.vs = m_ApCompositeVs.Get(); pd.ps = m_ApMultiplyPs.Get();
        pd.topology = EPrimitiveTopology::TriangleList;
        pd.rt_format = hdr_format; pd.depth_format = EFormat::Unknown;
        pd.depth_test = false; pd.depth_write = false;
        pd.cull_mode = ECullMode::None; pd.blend_mode = EBlendMode::Multiply;
        pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "ApCompositeCB";
        pd.texture_slots = 2;
        pd.texture_names[0] = "sceneDepth";
        pd.texture_names[1] = "apVolume";
        pd.static_sampler_count = 2;
        pd.static_samplers[0].filter = ESamplerFilter::Point;
        pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[1].filter = ESamplerFilter::Linear;
        pd.static_samplers[1].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[1].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[1].address_w = ESamplerAddress::Clamp;
        pd.layout_count = 0; pd.vertex_stride = 0;
        auto r = CreateRhiPipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_ApMultiplyPipe = Move(r.Value()); }
    {   FPipelineDesc pd{};
        pd.vs = m_ApCompositeVs.Get(); pd.ps = m_ApAddPs.Get();
        pd.topology = EPrimitiveTopology::TriangleList;
        pd.rt_format = hdr_format; pd.depth_format = EFormat::Unknown;
        pd.depth_test = false; pd.depth_write = false;
        pd.cull_mode = ECullMode::None; pd.blend_mode = EBlendMode::AdditivePreserveAlpha;
        pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "ApCompositeCB";
        pd.texture_slots = 2;
        pd.texture_names[0] = "sceneDepth";
        pd.texture_names[1] = "apVolume";
        pd.static_sampler_count = 2;
        pd.static_samplers[0].filter = ESamplerFilter::Point;
        pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[1].filter = ESamplerFilter::Linear;
        pd.static_samplers[1].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[1].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[1].address_w = ESamplerAddress::Clamp;
        pd.layout_count = 0; pd.vertex_stride = 0;
        auto r = CreateRhiPipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_ApAddPipe = Move(r.Value()); }
    {   FBufferDesc bd{}; bd.size = 256; bd.usage = EBufferUsage::Uniform; bd.cpu_writable = true;
        auto r = CreateRhiBuffer(device, bd); if (r.IsErr()) return Err<void>(r.Error()); m_ApCompositeCb = Move(r.Value()); }
    {   FBufferDesc bd{}; bd.size = 256; bd.usage = EBufferUsage::Uniform; bd.cpu_writable = true;
        auto r = CreateRhiBuffer(device, bd); if (r.IsErr()) return Err<void>(r.Error()); m_LocalFogCompositeCb = Move(r.Value()); }
    m_Ready = true;
    ACS_LOG_INFO("CSkyAtmosphere: scattering LUTs and aerial-perspective volume initialized");
    return Ok();
}

IRhiTexture* CSkyAtmosphere::BuildAerialPerspective(IRhiDevice& device, IRhiCommandList& cl,
                                                    const FMat4& inv_view_proj, FVec3 cam_pos,
                                                    FVec3 sun_dir, FVec3 sun_intensity,
                                                    f32 max_dist_scene, f32 scene_to_km,
                                                    f32 cam_alt_km) noexcept {
    return BuildAerialPerspective(device, cl, inv_view_proj, cam_pos, sun_dir, sun_intensity, max_dist_scene, scene_to_km, cam_alt_km, FVolumetricFogParams{});
}

IRhiTexture* CSkyAtmosphere::BuildAerialPerspective(IRhiDevice& device, IRhiCommandList& cl, const FMat4& inv_view_proj, FVec3 cam_pos, FVec3 sun_dir, FVec3 sun_intensity, f32 max_dist_scene, f32 scene_to_km, f32 cam_alt_km, const FVolumetricFogParams& fog) noexcept {
    const FVec3 sanitizedCamera = SanitizeVec3(cam_pos, FVec3{});
    const FMat4 cameraRelativeInverse = BuildCameraRelativeInverseFromWorld_Internal(inv_view_proj, sanitizedCamera);
    return BuildAerialPerspectiveCameraRelative(device, cl, cameraRelativeInverse, sanitizedCamera, sun_dir, sun_intensity, max_dist_scene, scene_to_km, cam_alt_km, fog);
}

IRhiTexture* CSkyAtmosphere::BuildAerialPerspectiveCameraRelative(IRhiDevice& device, IRhiCommandList& cl, const FMat4& camera_relative_inv_view_proj, FVec3 cam_pos, FVec3 sun_dir, FVec3 sun_intensity, f32 max_dist_scene, f32 scene_to_km, f32 cam_alt_km) noexcept {
    return BuildAerialPerspectiveCameraRelative(device, cl, camera_relative_inv_view_proj, cam_pos, sun_dir, sun_intensity, max_dist_scene, scene_to_km, cam_alt_km, FVolumetricFogParams{});
}

IRhiTexture* CSkyAtmosphere::BuildAerialPerspectiveCameraRelative(IRhiDevice& /*device*/, IRhiCommandList& cl, const FMat4& camera_relative_inv_view_proj, FVec3 cam_pos, FVec3 sun_dir, FVec3 sun_intensity, f32 max_dist_scene, f32 scene_to_km, f32 cam_alt_km, const FVolumetricFogParams& fog) noexcept {
    m_LocalFogVolumeValid = false;
    m_LocalFogMaxDistance = kLocalVolumetricFogMaxDistance;
    if (!m_Ready || !m_ApVol || !m_ApTransVol || !m_TransLut ||
        !m_ApPipe || !m_LocalFogPipe) {
        return nullptr;
    }
    const FMat4 sanitizedInvViewProj = SanitizeMatrix(camera_relative_inv_view_proj);
    cam_pos = SanitizeVec3(cam_pos, FVec3{0, 0, 0});
    FVec3 sd = SanitizeVec3(sun_dir, FVec3{0, 1, 0});
    {   f32 l2 = sd.x*sd.x + sd.y*sd.y + sd.z*sd.z;
        if (l2 < 1e-12f) sd = FVec3{0, 1, 0};
        else { f32 inv = 1.0f / Sqrt(l2); sd = FVec3{sd.x*inv, sd.y*inv, sd.z*inv}; } }
    sun_intensity = SanitizeVec3(sun_intensity, FVec3{0, 0, 0});
    sun_intensity = FVec3{
        sun_intensity.x > 0.0f ? sun_intensity.x : 0.0f,
        sun_intensity.y > 0.0f ? sun_intensity.y : 0.0f,
        sun_intensity.z > 0.0f ? sun_intensity.z : 0.0f,
    };
    max_dist_scene = FiniteOr(max_dist_scene, 0.01f);
    scene_to_km = FiniteOr(scene_to_km, 0.0f);
    cam_alt_km = FiniteOr(cam_alt_km, 0.0f);
    if (max_dist_scene < 0.01f) max_dist_scene = 0.01f;
    if (scene_to_km < 0.0f) scene_to_km = 0.0f;
    if (cam_alt_km < 0.0f) cam_alt_km = 0.0f;

    FVec3 fogColor =
        SanitizeVec3(fog.color, FVolumetricFogParams{}.color);
    fogColor = FVec3{
        fogColor.x > 0.0f ? fogColor.x : 0.0f,
        fogColor.y > 0.0f ? fogColor.y : 0.0f,
        fogColor.z > 0.0f ? fogColor.z : 0.0f,
    };
    f32 fogDensity = FiniteOr(fog.density, 0.0f);
    f32 fogFalloff = FiniteOr(fog.height_falloff, 0.0f);
    f32 fogBase = FiniteOr(fog.height_base, 0.0f);
    f32 fogG = FiniteOr(fog.anisotropy, 0.0f);
    f32 fogSun = FiniteOr(fog.sun_scatter, 0.0f);
    if (fogDensity < 0.0f) fogDensity = 0.0f;
    if (fogFalloff < 0.0f) fogFalloff = 0.0f;
    if (fogG < -0.85f) fogG = -0.85f;
    if (fogG >  0.85f) fogG =  0.85f;
    if (fogSun < 0.0f) fogSun = 0.0f;

    FApCB cb{};
    cb.cameraRelativeInvViewProj = sanitizedInvViewProj;
    cb.camPos = FVec4{cam_pos.x, cam_pos.y, cam_pos.z, 0.0f};
    cb.sunDir = FVec4{sd.x, sd.y, sd.z, 0.0f};
    cb.sunInt = FVec4{sun_intensity.x, sun_intensity.y, sun_intensity.z, 0.0f};
    cb.apParams = FVec4{scene_to_km, cam_alt_km, max_dist_scene, static_cast<f32>(kApZRes)};
    cb.fogColorDensity = FVec4{fogColor.x, fogColor.y, fogColor.z, fogDensity};
    cb.fogParams = FVec4{fogFalloff, fogBase, fogG, fogSun};
    // 物理大気は遠距離用とし、近距離の局所霧は別の2.5 km体積表へ積分する。
    FApCB atmosphereCb = cb;
    atmosphereCb.camPos = FVec4{};
    atmosphereCb.fogColorDensity.w = 0.0f;

    FVolumeCacheKey physicalKey{};
    physicalKey.invViewProj = atmosphereCb.cameraRelativeInvViewProj;
    physicalKey.camPos = atmosphereCb.camPos;
    physicalKey.sunDir = atmosphereCb.sunDir;
    physicalKey.sunInt = atmosphereCb.sunInt;
    physicalKey.apParams = atmosphereCb.apParams;
    const bool physicalEnabled = scene_to_km > 0.0f;
    const bool physicalDirty =
        physicalEnabled &&
        (!m_PhysicalApCacheValid ||
         !SameVolumeCacheKey(m_PhysicalApCacheKey, physicalKey));
    // Transmittance / multi-scattering LUT は Earth 定数だけで決まり、camera/sun には非依存。
    // 初回だけ焼き、毎フレームは camera-volume 本体の更新に GPU 時間を集中する。
    if (!m_LutsReady && physicalEnabled) {
        cl.SetComputePipeline(*m_TransPipe);
        cl.BindUav(0, *m_TransLut);
        cl.Dispatch(32, 8, 1);
        cl.SetComputePipeline(*m_MultiPipe);
        cl.SetTexture(0, *m_TransLut);
        cl.BindUav(0, *m_MultiLut);
        cl.Dispatch(4, 4, 1);
        m_LutsReady = true;
    }
    // Rebuild only when one of the exact sanitized physical inputs changed.
    // A static editor camera therefore pays the 48x48x96x24 integration once.
    if (physicalDirty) {
        m_ApCb->Update(&atmosphereCb, sizeof(atmosphereCb));
        cl.SetComputePipeline(*m_ApPipe);
        cl.SetConstantBuffer(0, *m_ApCb);
        cl.SetTexture(0, *m_TransLut);
        cl.SetTexture(1, *m_MultiLut);
        cl.BindUav(0, *m_ApVol);
        cl.BindUav(1, *m_ApTransVol);
        cl.Dispatch(kApXYRes / 4, kApXYRes / 4, kApZRes / 4);
        m_PhysicalApCacheKey = physicalKey;
        m_PhysicalApCacheValid = true;
        ++m_PhysicalApDispatchCount;
    }

    // 物理空は大気散乱を含むため、局所霧だけの伝達体積表を分離する。
    // これにより、深度消去済み背景へ最遠層を適用しても大気散乱を二重に足さない。
    // 二つの処理はCPU更新後に実行され得るため、定数バッファも分ける。
    m_LocalFogVolumeValid = fogDensity > 1e-7f && m_LocalFogVol && m_LocalFogCb;
    if (m_LocalFogVolumeValid) {
        FApCB fogOnlyCb = cb;
        // 物理大気の消散と散乱を止め、局所霧だけを積分する。
        fogOnlyCb.apParams.x = 0.0f;
        fogOnlyCb.apParams.y = 0.0f;
        fogOnlyCb.camPos = FVec4{0.0f, cb.camPos.y, 0.0f, 0.0f};
        m_LocalFogMaxDistance =
            max_dist_scene < kLocalVolumetricFogMaxDistance
                ? max_dist_scene
                : kLocalVolumetricFogMaxDistance;
        fogOnlyCb.apParams.z = m_LocalFogMaxDistance;

        FVolumeCacheKey localFogKey{};
        localFogKey.invViewProj = fogOnlyCb.cameraRelativeInvViewProj;
        localFogKey.camPos = fogOnlyCb.camPos;
        localFogKey.sunDir = fogOnlyCb.sunDir;
        localFogKey.sunInt = fogOnlyCb.sunInt;
        localFogKey.apParams = fogOnlyCb.apParams;
        localFogKey.fogColorDensity = fogOnlyCb.fogColorDensity;
        localFogKey.fogParams = fogOnlyCb.fogParams;
        const bool localFogDirty =
            !m_LocalFogCacheValid ||
            !SameVolumeCacheKey(m_LocalFogCacheKey, localFogKey);
        if (localFogDirty) {
            m_LocalFogCb->Update(&fogOnlyCb, sizeof(fogOnlyCb));
            cl.SetComputePipeline(*m_LocalFogPipe);
            cl.SetConstantBuffer(0, *m_LocalFogCb);
            cl.SetTexture(0, *m_TransLut);
            cl.SetTexture(1, *m_MultiLut);
            cl.BindUav(0, *m_LocalFogVol);
            cl.Dispatch(kApXYRes / 4, kApXYRes / 4, kApZRes / 4);
            m_LocalFogCacheKey = localFogKey;
            m_LocalFogCacheValid = true;
            ++m_LocalFogDispatchCount;
        }
    }
    return physicalEnabled ? m_ApVol.Get() : nullptr;
}

void CSkyAtmosphere::CompositeAerialPerspective(IRhiCommandList& cl,
                                                IRhiTexture& depth,
                                                IRhiTexture& ap_volume,
                                                IRhiTexture& transmittance_volume,
                                                const FMat4& inv_view_proj,
                                                FVec3 cam_pos,
                                                f32 max_dist_scene,
                                                u32 screen_width,
                                                u32 screen_height) noexcept {
    const FMat4 cameraRelativeInverse = BuildCameraRelativeInverseFromWorld_Internal(inv_view_proj, cam_pos);
    CompositeAerialPerspectiveCameraRelative(cl, depth, ap_volume, transmittance_volume, cameraRelativeInverse, max_dist_scene, screen_width, screen_height);
}

void CSkyAtmosphere::CompositeAerialPerspectiveCameraRelative(IRhiCommandList& cl, IRhiTexture& depth, IRhiTexture& ap_volume, IRhiTexture& transmittance_volume, const FMat4& camera_relative_inv_view_proj, f32 max_dist_scene, u32 screen_width, u32 screen_height) noexcept {
    if (!m_Ready || !m_ApMultiplyPipe || !m_ApAddPipe ||
        !m_ApCompositeCb ||
        screen_width == 0 || screen_height == 0) {
        return;
    }
    FApCompositeCB cb{};
    f32 safeMaxDistance = FiniteOr(max_dist_scene, 0.001f);
    if (safeMaxDistance < 0.001f) safeMaxDistance = 0.001f;
    cb.cameraRelativeInvViewProj = SanitizeMatrix(camera_relative_inv_view_proj);
    cb.maxDistance = FVec4{safeMaxDistance, 0.0f, 0.0f, 0.0f};
    cb.compositeParams = FVec4{0.0f, 0.0f, 0.0f, 0.0f};
    m_ApCompositeCb->Update(&cb, sizeof(cb));

    FViewport vp{};
    vp.width = static_cast<f32>(screen_width);
    vp.height = static_cast<f32>(screen_height);
    cl.SetViewport(vp);
    FScissorRect sr{};
    sr.right = static_cast<i32>(screen_width);
    sr.bottom = static_cast<i32>(screen_height);
    cl.SetScissor(sr);

    // Exact wavelength-dependent transfer:
    //   scene.rgb = scene.rgb * T.rgb + L.rgb
    // A pipeline switch invalidates root/resource bindings on both RHIs, so
    // every pass deliberately rebinds its complete resource set.
    cl.SetPipeline(*m_ApMultiplyPipe);
    cl.SetConstantBuffer(0, *m_ApCompositeCb);
    cl.SetTexture(0, depth);
    cl.SetTexture(1, transmittance_volume);
    cl.Draw(3, 0);

    cl.SetPipeline(*m_ApAddPipe);
    cl.SetConstantBuffer(0, *m_ApCompositeCb);
    cl.SetTexture(0, depth);
    cl.SetTexture(1, ap_volume);
    cl.Draw(3, 0);
}

void CSkyAtmosphere::CompositeLocalFog(
    IRhiCommandList& cl, IRhiTexture& depth, IRhiTexture& local_fog_volume,
    IRhiTexture* cloud_depth, const FMat4& inv_view_proj,
    FVec3 cam_pos, f32 max_dist_scene,
    u32 screen_width, u32 screen_height) noexcept {
    const FMat4 cameraRelativeInverse = BuildCameraRelativeInverseFromWorld_Internal(inv_view_proj, cam_pos);
    CompositeLocalFogCameraRelative(cl, depth, local_fog_volume, cloud_depth, cameraRelativeInverse, max_dist_scene, screen_width, screen_height);
}

void CSkyAtmosphere::CompositeLocalFogCameraRelative(IRhiCommandList& cl, IRhiTexture& depth, IRhiTexture& local_fog_volume, IRhiTexture* cloud_depth, const FMat4& camera_relative_inv_view_proj, f32 max_dist_scene, u32 screen_width, u32 screen_height) noexcept {
    if (!m_Ready || !m_ApCompositePipe || !m_LocalFogCompositeCb ||
        screen_width == 0 || screen_height == 0) {
        return;
    }

    FApCompositeCB cb{};
    f32 safeMaxDistance = FiniteOr(max_dist_scene, 0.001f);
    if (safeMaxDistance < 0.001f) safeMaxDistance = 0.001f;
    cb.cameraRelativeInvViewProj = SanitizeMatrix(camera_relative_inv_view_proj);
    cb.maxDistance = FVec4{safeMaxDistance, 0.0f, 0.0f, 0.0f};
    cb.compositeParams = FVec4{1.0f, cloud_depth != nullptr ? 1.0f : 0.0f, 0.0f, 0.0f};
    m_LocalFogCompositeCb->Update(&cb, sizeof(cb));

    FViewport vp{};
    vp.width = static_cast<f32>(screen_width);
    vp.height = static_cast<f32>(screen_height);
    cl.SetViewport(vp);
    FScissorRect sr{};
    sr.right = static_cast<i32>(screen_width);
    sr.bottom = static_cast<i32>(screen_height);
    cl.SetScissor(sr);
    cl.SetPipeline(*m_ApCompositePipe);
    cl.SetConstantBuffer(0, *m_LocalFogCompositeCb);
    cl.SetTexture(0, depth);
    cl.SetTexture(1, local_fog_volume);
    cl.SetTexture(2, cloud_depth != nullptr ? *cloud_depth : depth);
    cl.Draw(3, 0);
}

bool CSkyAtmosphere::BakeEquirect(IRhiDevice& device, IRhiCommandList& cl,
                                  const FAtmosphereParams& params,
                                  u32 width, u32 height, TArray<f32>& out) noexcept {
    return BakeEquirectAtAltitude(
        device, cl, params, width, height, 2.0f, out);
}

bool CSkyAtmosphere::BakeEquirectAtAltitude(
    IRhiDevice& device, IRhiCommandList& cl,
    const FAtmosphereParams& params, u32 width, u32 height,
    f32 altitude, TArray<f32>& out) noexcept {
    if (!m_Ready) return false;
    FVec3 sd = params.sun_dir;
    {   f32 l2 = sd.x*sd.x + sd.y*sd.y + sd.z*sd.z;
        if (l2 < 1e-12f) sd = FVec3{0, 1, 0};
        else { f32 inv = 1.0f / Sqrt(l2); sd = FVec3{sd.x*inv, sd.y*inv, sd.z*inv}; } }
    FAtmoCB cb{}; cb.sunDir = FVec4{sd.x, sd.y, sd.z, 0.0f};
    cb.sunInt = FVec4{params.sun_intensity.x, params.sun_intensity.y, params.sun_intensity.z, 0.0f};
    cb.groundAlbedo = FVec4{
        params.ground_albedo.x > 0.0f ? params.ground_albedo.x : 0.0f,
        params.ground_albedo.y > 0.0f ? params.ground_albedo.y : 0.0f,
        params.ground_albedo.z > 0.0f ? params.ground_albedo.z : 0.0f,
        SanitizeBakeAltitude(altitude) * 0.001f};
    m_Cb->Update(&cb, sizeof(cb));

    // 1) 大気 LUT は定数なので初回だけ焼く。AP と equirect bake のどちらが先でも共有する。
    if (!m_LutsReady) {
        cl.SetComputePipeline(*m_TransPipe);
        cl.BindUav(0, *m_TransLut);
        cl.Dispatch(32, 8, 1);
        cl.SetComputePipeline(*m_MultiPipe);
        cl.SetTexture(0, *m_TransLut);
        cl.BindUav(0, *m_MultiLut);
        cl.Dispatch(4, 4, 1);
        m_LutsReady = true;
    }

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
    cl.SetTexture(1, *m_MultiLut);
    cl.BindUav(0, *m_Equirect);
    cl.Dispatch((width + 7) / 8, (height + 7) / 8, 1);

    // 4) CPU へ読み戻す (ReadTexture が Flush+WaitIdle → 上の dispatch を実行してから copy)。
    out.SetNum(static_cast<usize>(width) * height * 4u);
    return device.ReadTexture(*m_Equirect, out.GetData(),
                              static_cast<u32>(out.Num() * sizeof(f32)));
}

void CSkyAtmosphere::Shutdown() noexcept {
    m_TransPipe.Reset(); m_BakePipe.Reset();
    m_TransCs.Reset();   m_BakeCs.Reset();
    m_MultiPipe.Reset(); m_MultiCs.Reset(); m_MultiLut.Reset();   // 多重散乱 LUT (UAF 防止)
    m_TransLut.Reset();  m_Equirect.Reset(); m_Cb.Reset();
    m_ApMultiplyPipe.Reset(); m_ApMultiplyPs.Reset();
    m_ApAddPipe.Reset(); m_ApAddPs.Reset();
    m_ApCompositePipe.Reset(); m_ApCompositePs.Reset(); m_ApCompositeVs.Reset();
    m_ApCompositeCb.Reset(); m_LocalFogCompositeCb.Reset();
    m_ApPipe.Reset(); m_ApCs.Reset(); m_ApVol.Reset(); m_ApTransVol.Reset();
    m_ApCb.Reset(); // aerial perspective (UAF 防止)
    m_LocalFogPipe.Reset(); m_LocalFogCs.Reset();
    m_LocalFogVol.Reset(); m_LocalFogCb.Reset(); m_LocalFogVolumeValid = false;
    m_LocalFogMaxDistance = kLocalVolumetricFogMaxDistance;
    m_PhysicalApCacheValid = false; m_LocalFogCacheValid = false;
    m_PhysicalApDispatchCount = 0; m_LocalFogDispatchCount = 0;
    m_EqW = 0; m_EqH = 0; m_LutsReady = false; m_Ready = false;
}

} // namespace acs
