// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — motion + normal G-buffer pass。床 + opaque sphere + glass +
// emissive orb を全部描く (motion vector は scene 全体で authoritative にする
// と TAA の reproject が安定する)。
//
// 出力:
//   - motion (RG16F)  : prev_uv - curr_uv (TAA / SSR temporal が読む)
//   - normal (RGBA16F): world-space normal (SSR / SSAO が読む)
//
// 戻り値: 前フレームに 1 度でも本 pass を走らせていれば motion 出力テクスチャ、
//        そうでなければ nullptr (FPostProcess の TAA に渡しても無視されるため)。
#pragma once

#include "ShowcaseAssets.h"
#include "ShowcaseTypes.h"

#include "math/Mat.h"

namespace acs { class IRhiCommandList; class IRhiTexture; }

namespace helloshowcase {

acs::IRhiTexture* ExecuteMotionPass(Assets& a,
                                     acs::IRhiCommandList& cl,
                                     const acs::FMat4& vp_no_jitter,
                                     const acs::FMat4& prev_vp_no_jitter,
                                     bool prev_vp_valid,
                                     acs::f32 prev_orb_phase,
                                     const acs::FMat4 (&orb_curr)[kOrbCount]) noexcept;

} // namespace helloshowcase
