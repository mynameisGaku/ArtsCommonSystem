// SPDX-License-Identifier: Apache-2.0
// HelloIbl — TAA sub-pixel jitter helper の実装。
//
// Halton(2,3) low-discrepancy sequence で 8 フレーム周期の sub-pixel ぶん xy を
// ずらした projection を作る。rendering 全体 (skybox, PbrShader, SSR, SSAO) に
// 同じ jitter を適用し、複数フレームの累積でエッジが滑らかになる。
//
// 注意: shadow pass はライト視点なので jitter しない (caster と影 map が同じ
// ビューなら遮蔽判定はそのまま正しい)。
#include "HelloIblApp.h"

#include "math/Mat.h"

using namespace acs;

namespace helloibl {

FMat4 BuildJitteredViewProjection(HelloIblApp& app, const FMat4& vp_no_jitter,
                                 u32 hdr_width, u32 hdr_height) noexcept {
    if (!app._use_taa) return vp_no_jitter;

    const u32 idx    = (app._taa_frame_index % 8) + 1;   // +1 で Halton(0)=0 を避ける
    const f32 jx     = Halton(idx, 2) - 0.5f;            // [-0.5, 0.5) sub-pixel
    const f32 jy     = Halton(idx, 3) - 0.5f;
    const f32 jx_ndc = jx * 2.0f / static_cast<f32>(hdr_width);
    const f32 jy_ndc = jy * 2.0f / static_cast<f32>(hdr_height);

    // jitter mat = identity with M[3][0..1] = jx_ndc / jy_ndc
    FMat4 jmat = FMat4::Identity();
    jmat.m[3][0] = jx_ndc;
    jmat.m[3][1] = jy_ndc;

    app._taa_frame_index = (app._taa_frame_index + 1u) % 1024u;
    return vp_no_jitter * jmat;
}

} // namespace helloibl
