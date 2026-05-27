// SPDX-License-Identifier: Apache-2.0
// HelloLightmap — Cornell box / lightmap baker の共通型と定数。
//
// 内容:
//   ・Cornell box の寸法と baker パラメータ (constexpr)
//   ・天井光源の放射輝度 (kLightRadiance)
//   ・Quad: Cornell box の 1 面 (mesh + 軸並行平面パラメータ + lightmap)
//   ・Rng: xorshift32 軽量 RNG (texel ごとに seed して再現性)
//   ・MakeTBN / CosineSampleHemisphere / Hadamard: baker から使う数学ヘルパ
//
// 「軸並行平面」表現を取っているのは Cornell box の壁が全部軸並行で、
// 解析的 ray cast (RayQuad) がそのまま使えるため。これにより baker は
// BVH や mesh トライアングル交差を持たずに済む。
#pragma once

#include "asset/MeshAsset.h"
#include "render/RenderAssets.h"
#include "container/Array.h"
#include "memory/UniquePtr.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "math/Math.h"
#include "foundation/Types.h"

namespace hellolightmap {

// Cornell box の寸法: x∈[-1,1], y∈[0,2], z∈[-1,2] (手前 z=-1 は開口)
inline constexpr acs::f32 kBoxMinX = -1.0f;
inline constexpr acs::f32 kBoxMaxX =  1.0f;
inline constexpr acs::f32 kBoxMinY =  0.0f;
inline constexpr acs::f32 kBoxMaxY =  2.0f;
inline constexpr acs::f32 kBoxMinZ = -1.0f;
inline constexpr acs::f32 kBoxMaxZ =  2.0f;

inline constexpr acs::u32 kLmSize      = 64;   // lightmap 解像度 (1 面あたり)
inline constexpr acs::u32 kBakeRays    = 64;   // texel あたりの path 数
inline constexpr acs::u32 kBounceDepth = 5;    // 1 path の最大バウンス数 (multi-bounce GI)
inline constexpr acs::u32 kQuadCount   = 5;    // Cornell box の面数 (床/天井/奥/左/右)

// 天井光源の放射輝度。lightmap は HDR (R32G32B32A32F) なので飽和の心配がなく、
// 物理的に明るい値を入れられる。tonemap (ACES) 後に程よい明るさになるよう調整
// した値 (絵作りで自由に変えてよい)。
inline const acs::FVec3 kLightRadiance{4.5f, 4.2f, 3.8f};

// Cornell box の 1 面。MakePlane (XZ 平面、法線 +Y) を model で配置する。
//   axis / axis_value / u_min.. : ray cast 用の world-space 軸並行平面パラメータ
//   plane_w / plane_h           : MakePlane に渡したローカルサイズ。baker が
//                                 texel uv → ローカル座標 → model → world と
//                                 変換するのに使う (回転を model に委ねる)。
struct Quad {
    acs::GpuMesh                mesh;
    acs::FMat4                   model;
    acs::FVec3                   albedo;
    acs::FVec3                   normal;        // world-space 法線 (model から導出)
    acs::f32                    plane_w = 0.0f;
    acs::f32                    plane_h = 0.0f;
    acs::TUniquePtr<acs::IRhiTexture> lightmap;
    acs::TArray<acs::FVec4>       lm_data;        // bake 結果 (HDR、RGBA float)
    // 面が乗る軸 (0=x, 1=y, 2=z) と、その軸の値。残り 2 軸が矩形範囲。
    acs::i32                    axis        = 0;
    acs::f32                    axis_value  = 0.0f;
    acs::f32                    u_min = 0.0f, u_max = 0.0f;   // 矩形範囲 (axis 以外の 1 軸目)
    acs::f32                    v_min = 0.0f, v_max = 0.0f;   // 矩形範囲 (axis 以外の 2 軸目)
    bool                        emissive    = false;          // 天井 = 光源
};

// 軸並行平面 axis(=value) 上の矩形 [u_min,u_max]x[v_min,v_max] と
// ray (o,d) の交差。hit したら t を返す (なければ -1)。
inline acs::f32 RayQuad(acs::FVec3 o, acs::FVec3 d, const Quad& q) noexcept {
    acs::f32 od, dd;
    if (q.axis == 0)      { od = o.x; dd = d.x; }
    else if (q.axis == 1) { od = o.y; dd = d.y; }
    else                  { od = o.z; dd = d.z; }
    if (dd > -1e-6f && dd < 1e-6f) return -1.0f;     // 平面と平行
    const acs::f32 t = (q.axis_value - od) / dd;
    if (t < 1e-3f) return -1.0f;                     // 後方 or 自己交差
    const acs::FVec3 p{o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
    // axis 以外の 2 軸を取り出す
    acs::f32 pu, pv;
    if (q.axis == 0)      { pu = p.y; pv = p.z; }
    else if (q.axis == 1) { pu = p.x; pv = p.z; }
    else                  { pu = p.x; pv = p.y; }
    if (pu < q.u_min || pu > q.u_max || pv < q.v_min || pv > q.v_max) return -1.0f;
    return t;
}

// componentwise 乗算 (FVec3 には Hadamard 積の operator が無いので自前で持つ)。
inline acs::FVec3 Hadamard(acs::FVec3 a, acs::FVec3 b) noexcept {
    return acs::FVec3{a.x * b.x, a.y * b.y, a.z * b.z};
}

// 軽量 RNG (xorshift32)。texel ごとに seed して bake の再現性を持たせる。
struct Rng {
    acs::u32 state;
    acs::f32 NextF() noexcept {                       // [0, 1)
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<acs::f32>(state & 0x00FFFFFFu) / 16777216.0f;
    }
};

// 法線 N に直交する tangent / bitangent を作る。
inline void MakeTBN(acs::FVec3 N, acs::FVec3& T, acs::FVec3& B) noexcept {
    T = (acs::Abs(N.y) > 0.9f) ? acs::FVec3{1, 0, 0} : acs::FVec3{0, 1, 0};
    T = acs::Normalize(T - N * acs::Dot(T, N));
    B = acs::Cross(N, T);
}

// cosine-weighted hemisphere sampling。pdf = cosθ/π なので、この方向で
// 入射放射輝度を平均すると cosine 重みと pdf の π が相殺し、「入射 irradiance の
// 平均」がそのまま得られる (FPbrShader が描画時に albedo を掛ける)。
inline acs::FVec3 CosineSampleHemisphere(acs::FVec3 N, acs::FVec3 T, acs::FVec3 B, Rng& rng) noexcept {
    const acs::f32 u1  = rng.NextF();
    const acs::f32 u2  = rng.NextF();
    const acs::f32 r   = acs::Sqrt(u1);
    const acs::f32 phi = 6.2831853f * u2;
    const acs::f32 x = r * acs::Cos(phi);
    const acs::f32 y = r * acs::Sin(phi);
    const acs::f32 z = acs::Sqrt(1.0f - u1);               // u1∈[0,1) なので 1-u1 > 0
    return acs::Normalize(T * x + B * y + N * z);
}

} // namespace hellolightmap
