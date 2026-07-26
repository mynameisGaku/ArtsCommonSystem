// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — motion + normal G-buffer pass の実装。
#include "MotionPass.h"

#include "math/Math.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"

using namespace acs;

namespace helloshowcase {

FMotionPassOutput ExecuteMotionPass(
    FAssets& a, IRhiCommandList& cl,
    const FMat4& vp_no_jitter,
    const FMat4& prev_vp_no_jitter,
    bool prev_vp_valid,
    f32 prev_orb_phase,
    const FMat4 (&orb_curr)[kOrbCount]) noexcept {
    constexpr u32 kRequiredDraws =
        1u + kSphereCount + kOrbCount;
    const FMat4& motion_prev_vp = prev_vp_valid ? prev_vp_no_jitter : vp_no_jitter;
    if (!a.motion.BeginFrame(kRequiredDraws) ||
        !a.motion.Begin(cl, vp_no_jitter, motion_prev_vp)) {
        return {};
    }

    bool complete = true;

    const FMat4 floor_model = FMat4::Translation(FVec3{0, kFloorY, 0});
    if (!a.motion.DrawMesh(
            cl, a.gm_floor, floor_model, floor_model)) {
        complete = false;
    }

    // 床 + 静的 sphere (ガラス含む — motion 上は静的なので curr == prev)
    for (u32 i = 0; i < kSphereCount; ++i) {
        const FMat4 m = FMat4::Scale(FVec3{kSphereScale, kSphereScale, kSphereScale}) *
                       FMat4::Translation(FVec3{kSphereX[i], kSphereY, kSphereZ});
        if (!a.motion.DrawMesh(cl, a.gm_sphere, m, m)) {
            complete = false;
        }
    }

    // 公転 orb は dynamic — prev pos を計算。motion vector では pulse による
    // スケール変化は無視し、kOrbScale 一定で計算する (元実装と同じ)。
    // 軌道平行移動が motion の主成分なので十分。
    for (u32 i = 0; i < kOrbCount; ++i) {
        const f32 ang_prev = prev_orb_phase + static_cast<f32>(i) * kPi;
        const FVec3 pos_prev{
            kOrbOrbitRadius * Sin(ang_prev),
            kOrbY,
            kOrbOrbitRadius * Cos(ang_prev),
        };
        const FMat4 prev = FMat4::Scale(FVec3{kOrbScale, kOrbScale, kOrbScale}) *
                          FMat4::Translation(pos_prev);
        if (!a.motion.DrawMesh(
                cl, a.gm_sphere, orb_curr[i],
                prev_vp_valid ? prev : orb_curr[i])) {
            complete = false;
        }
    }
    a.motion.End(cl);

    if (!complete ||
        a.motion.ObjectDrawCount() != kRequiredDraws) {
        return {};
    }
    return FMotionPassOutput{
        prev_vp_valid ? a.motion.OutputTexture() : nullptr,
        a.motion.OutputNormalTexture()};
}

} // namespace helloshowcase
