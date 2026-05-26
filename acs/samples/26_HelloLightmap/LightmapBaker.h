// SPDX-License-Identifier: Apache-2.0
// HelloLightmap — multi-bounce path-traced lightmap baker。
//
// Cornell box の各面 (Quad) について、CPU で 1 texel ごとに hemisphere へ
// path tracing を行い、入射 irradiance の平均を HDR R32G32B32A32F の
// lightmap texture に焼く。3x3 box blur で MC ノイズを均してから upload する。
//
// 戻り値:
//   - 各 Quad の lightmap (TUniquePtr<IRhiTexture>) が中で set される。
//   - 失敗時は ACS_LOG_ERROR を出してそのまま継続 (該当面だけ lightmap が null)。
//
// 設計上の特徴:
//   - cosine-weighted sampling: throughput に albedo を畳むだけで自然に多重反射が出る
//   - texel ごとの decorrelate seed: bake 結果が完全に再現可能
//   - 平面 quad + hemisphere 方向: 自己交差が幾何的に発生しない
#pragma once

#include "LightmapTypes.h"

namespace acs { class IRhiDevice; }

namespace hellolightmap {

// 面 q のテクセル (tu,tv)∈[0,1] を world 座標へ変換する。
// MakePlane の uv 規約 (u→ローカル +X, v→ローカル -Z) でローカル平面座標に
// 戻し、quad の model 行列で world へ変換する。これで描画時に FPbrShader が
// mesh の uv で lightmap を引く位置と、baker が焼いた位置が厳密に一致する
// (回転・平行移動は model に委ねるので軸の取り違えが起きない)。
acs::FVec3 TexelToWorld(const Quad& q, acs::f32 tu, acs::f32 tv) noexcept;

// 1 本の path をトレースし、texel への入射放射輝度を返す。
// origin から cosine-weighted hemisphere へ飛ばし、拡散面で反射を繰り返す。
// 光源 (天井) に届いたら、それまでの throughput を掛けた放射輝度を返す。
// throughput は receiver の albedo を畳まない (FPbrShader が描画時に lm * albedo
// するため)。cosine-weighted なので複数 path の平均 = 入射 irradiance の平均。
acs::FVec3 PathTrace(acs::FVec3 origin, acs::FVec3 normal,
                    const Quad (&quads)[kQuadCount], Rng& rng) noexcept;

// すべての Quad について lightmap を baking + GPU upload する。
// 各 texel から kBakeRays 本の path を hemisphere へ飛ばし、最大
// kBounceDepth 回まで拡散反射を追跡。固定係数の擬似 1-bounce ではなく、
// 赤/緑壁の多重反射 color bleeding が物理的に出る。結果は入射 irradiance の
// 平均で、描画時に FPbrShader が albedo を掛ける。
// 最後に 3x3 box blur で MC ノイズを均してから HDR texture を生成する。
void BakeLightmaps(acs::IRhiDevice& dev, Quad (&quads)[kQuadCount]) noexcept;

} // namespace hellolightmap
