// SPDX-License-Identifier: Apache-2.0
// HelloLightmap — multi-bounce path-traced lightmap baker の実装。
//
// PathTrace + BakeLightmaps の実体。設計上の解説は LightmapBaker.h と
// LightmapTypes.h を参照。
#include "LightmapBaker.h"

#include "foundation/Log.h"

using namespace acs;

namespace hellolightmap {

FVec3 TexelToWorld(const Quad& q, f32 tu, f32 tv) noexcept {
    const f32 lx = (tu - 0.5f) * q.plane_w;
    const f32 lz = (0.5f - tv) * q.plane_h;
    return TransformPoint(FVec3{lx, 0.0f, lz}, q.model);
}

FVec3 PathTrace(FVec3 origin, FVec3 normal,
               const Quad (&quads)[kQuadCount], Rng& rng) noexcept {
    FVec3 throughput{1.0f, 1.0f, 1.0f};
    FVec3 o = origin;
    FVec3 N = normal;
    for (u32 b = 0; b < kBounceDepth; ++b) {
        FVec3 T, B;
        MakeTBN(N, T, B);
        const FVec3 dir = CosineSampleHemisphere(N, T, B, rng);

        // Cornell box の全 5 面と交差、最近を採用。
        // 平面 quad + hemisphere 方向なので自己交差は幾何的に起きない。
        f32 best_t = 1e9f;
        i32 best_q = -1;
        for (u32 hj = 0; hj < kQuadCount; ++hj) {
            const f32 t = RayQuad(o, dir, quads[hj]);
            if (t > 0.0f && t < best_t) { best_t = t; best_q = static_cast<i32>(hj); }
        }
        if (best_q < 0) return FVec3{0, 0, 0};              // 開口へ脱出
        const Quad& hq = quads[static_cast<u32>(best_q)];
        if (hq.emissive) {
            return Hadamard(throughput, kLightRadiance);   // 光源に到達
        }
        // 非発光面で拡散反射: hit albedo を throughput に畳んで path 継続
        throughput = Hadamard(throughput, hq.albedo);
        o = o + dir * best_t + hq.normal * 1e-3f;          // 次の origin
        N = hq.normal;
    }
    return FVec3{0, 0, 0};       // kBounceDepth 以内に光源へ届かず
}

void BakeLightmaps(IRhiDevice& dev, Quad (&quads)[kQuadCount]) noexcept {
    TArray<FVec3> raw;      raw.Resize(static_cast<usize>(kLmSize) * kLmSize);
    TArray<FVec3> blurred;  blurred.Resize(static_cast<usize>(kLmSize) * kLmSize);

    for (u32 qi = 0; qi < kQuadCount; ++qi) {
        Quad& q = quads[qi];
        q.lm_data.Resize(static_cast<usize>(kLmSize) * kLmSize);   // 1 FVec4 / texel

        if (q.emissive) {
            // 天井 = 光源。受光ではなく放射輝度そのものを焼く
            // (描画時に albedo が掛かり、box 内で最も明るい面になる)。
            for (usize i = 0; i < raw.Size(); ++i) raw[i] = kLightRadiance;
        } else {
            const FVec3 N = q.normal;
            for (u32 ty = 0; ty < kLmSize; ++ty) {
                for (u32 tx = 0; tx < kLmSize; ++tx) {
                    const f32 tu = (static_cast<f32>(tx) + 0.5f) / kLmSize;
                    const f32 tv = (static_cast<f32>(ty) + 0.5f) / kLmSize;
                    const FVec3 wp     = TexelToWorld(q, tu, tv);
                    const FVec3 origin = wp + N * 0.01f;     // self-hit 回避

                    // texel ごとに decorrelate した seed (再現性あり)
                    Rng rng{ (qi * 2654435761u) ^ (ty * 40503u)
                             ^ (tx * 73856093u) ^ 0x9E3779B9u };
                    if (rng.state == 0u) rng.state = 1u;

                    FVec3 e{0, 0, 0};
                    for (u32 r = 0; r < kBakeRays; ++r) {
                        e += PathTrace(origin, N, quads, rng);
                    }
                    raw[static_cast<usize>(ty) * kLmSize + tx] =
                        e * (1.0f / static_cast<f32>(kBakeRays));
                }
            }
        }

        // 3x3 box blur で MC ノイズを均す (indirect は低周波なので質感は保たれる)。
        for (i32 ty = 0; ty < static_cast<i32>(kLmSize); ++ty) {
            for (i32 tx = 0; tx < static_cast<i32>(kLmSize); ++tx) {
                FVec3 sum{0, 0, 0};
                u32  n = 0;
                for (i32 dy = -1; dy <= 1; ++dy) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        const i32 sx = tx + dx, sy = ty + dy;
                        if (sx < 0 || sx >= static_cast<i32>(kLmSize) ||
                            sy < 0 || sy >= static_cast<i32>(kLmSize)) continue;
                        sum += raw[static_cast<usize>(sy) * kLmSize + sx];
                        ++n;
                    }
                }
                blurred[static_cast<usize>(ty) * kLmSize + tx] =
                    sum * (1.0f / static_cast<f32>(n));
            }
        }

        // float irradiance をそのまま HDR lightmap へ (量子化・clamp なし)
        for (u32 i = 0; i < kLmSize * kLmSize; ++i) {
            const FVec3 c = blurred[i];
            q.lm_data[i] = FVec4{c.x, c.y, c.z, 1.0f};
        }

        TextureDesc td{};
        td.width  = kLmSize;
        td.height = kLmSize;
        td.format = EFormat::R32G32B32A32_Float;
        td.initial_data      = q.lm_data.Data();
        td.initial_data_size = q.lm_data.Size() * sizeof(FVec4);
        auto r = CreateRhiTexture(dev, td);
        if (r.IsErr()) {
            ACS_LOG_ERROR("HelloLightmap: lightmap texture 生成失敗 (quad %u)", qi);
        } else {
            q.lightmap = Move(r.Value());
        }
    }
}

} // namespace hellolightmap
