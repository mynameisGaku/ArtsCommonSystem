// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — motion + normal G-buffer pass。床 + opaque sphere + glass +
// emissive orb を全部描く (motion vector は scene 全体で authoritative にする
// と TAA の reproject が安定する)。
//
// 出力:
//   - motion (RG16F)  : prev_uv - curr_uv (TAA / SSR temporal が読む)
//   - normal (RGBA16F): world-space normal (SSR / SSAO が読む)
//
// 戻り値: 全対象 draw が記録できたときだけ normal を公開する。motion はさらに
//        前フレーム VP が有効な場合だけ公開し、cold start と失敗出力を後段から隔離する。
#pragma once

#include "ShowcaseAssets.h"
#include "ShowcaseTypes.h"

#include "math/Mat.h"

namespace acs { class IRhiCommandList; class IRhiTexture; }

namespace helloshowcase {

struct FMotionPassOutput {
    acs::IRhiTexture* motion = nullptr;
    acs::IRhiTexture* normal = nullptr;
};

FMotionPassOutput ExecuteMotionPass(
    FAssets& a,
    acs::IRhiCommandList& cl,
    const acs::FMat4& vp_no_jitter,
    const acs::FMat4& prev_vp_no_jitter,
    bool prev_vp_valid,
    acs::f32 prev_orb_phase,
    const acs::FMat4 (&orb_curr)[kOrbCount]) noexcept;

} // namespace helloshowcase
