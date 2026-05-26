// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — 屈折ガラス球 pass。opaque HDR pass の直後に、HDR を bg_rt
// へ複製し、その bg_rt を背景として ClearGlass / FrostedGlass を HDR の
// 上に load (clear なし) で書き加える。ClearGlass には diamond/prism 風の
// 色分離 (dispersion) も入れる。
//
// 呼び出し側 (ShowcaseApp) で _show_refraction == false のときは丸ごとスキップ。
#pragma once

#include "ShowcaseAssets.h"

#include "math/Mat.h"
#include "math/Vec.h"

namespace acs { class IRhiCommandList; class IRhiTexture; }

namespace helloshowcase {

void ExecuteRefractionPass(Assets& a,
                            acs::IRhiCommandList& cl,
                            acs::IRhiTexture& hdr,
                            acs::IRhiTexture& depth,
                            const acs::FMat4& view_proj,
                            acs::FVec3 cam_pos) noexcept;

} // namespace helloshowcase
