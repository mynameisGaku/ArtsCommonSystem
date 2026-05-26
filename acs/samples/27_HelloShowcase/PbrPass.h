// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — opaque HDR pass。Sky + 床 + 4 体の sphere + 公転する
// emissive オーブを HDR RT に描く。ガラス球 (Clear/Frosted) は描かない
// (RefractionPass がそれらを担当)。
//
// PBR ライティングは PbrShader 経由。SSR (前フレームの反射) と SSAO
// (前フレームの indirect AO) を warm flag で 1 フレーム遅延合成する。
#pragma once

#include "ShowcaseAssets.h"
#include "ShowcaseTypes.h"

#include "math/Camera.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "foundation/Types.h"

namespace acs { class IRhiCommandList; class IRhiTexture; }

namespace helloshowcase {

// 不透明 HDR pass を実行する。
//   ssr_warm  : 前フレームで SSR.Render が走った？ (frame 0 の garbage を回避)
//   ssao_warm : 同上の SSAO 版
// 戻り値: 公転 orb の現フレーム transform 配列 (motion pass で再利用)。
void ExecutePbrPass(Assets& a,
                     acs::IRhiCommandList& cl,
                     acs::IRhiTexture& hdr,
                     acs::IRhiTexture& depth,
                     const acs::Camera& camera,
                     const acs::Mat4& view_proj_jittered,
                     acs::Vec3 cam_pos,
                     acs::f32 orb_phase,
                     bool ssr_warm,
                     bool ssao_warm,
                     acs::Mat4 (&orb_curr_out)[kOrbCount]) noexcept;

} // namespace helloshowcase
