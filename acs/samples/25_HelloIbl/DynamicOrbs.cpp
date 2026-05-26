// SPDX-License-Identifier: Apache-2.0
// HelloIbl — グリッド前方を公転する発光オーブ 3 個。
//
// emissive + bloom で光り、motion vector を G-buffer に焼くので TAA の trail も
// 出ない。glossy な床にも映り込む (SSR が scene color = 発光込みを反射するため)。
//
// 各フレームで:
//   1) _dyn_prev ← 前フレームの _dyn_curr へ swap
//   2) _dyn_curr を _anim_time から再計算
// この prev/curr ペアを color pass と motion pass の両方が読む。
#include "HelloIblApp.h"

using namespace acs;

namespace helloibl {

namespace {

// オーブの発光色。emissive ベース + base_color にも同じ色を与えると、
// PbrShader の emissive 寄与と reflection の見た目が一致する。
constexpr Vec3 kOrbGlow[kDynCount] = {
    Vec3{1.00f, 0.45f, 0.15f},   // 暖色オレンジ
    Vec3{0.25f, 1.00f, 0.40f},   // 緑
    Vec3{0.30f, 0.55f, 1.00f},   // 青
};

} // anonymous namespace

void UpdateDynamicOrbs(HelloIblApp& app) noexcept {
    // prev ← 前フレームの curr。OnCustomFrame の冒頭で呼ぶこと
    // (PbrShader が curr を読み、MotionVector が prev/curr を比べる)。
    for (u32 i = 0; i < kDynCount; ++i) {
        app._dyn_prev[i] = app._dyn_curr[i];
        app._dyn_curr[i] = app.ComputeDynTransform(i, app._anim_time);
    }
}

void DrawDynamicOrbs(HelloIblApp& app) noexcept {
    IRhiCommandList* cl = app.GetRenderer().CommandList();
    if (!cl) return;

    app._pbr.SetExtParams(0.0f, 0.5f, 0.0f, Vec3{1, 0, 0});
    for (u32 i = 0; i < kDynCount; ++i) {
        app._pbr.SetEmissive(kOrbGlow[i], /*strength=*/3.0f);
        app._pbr.DrawMesh(*cl, app._gm_sphere, app._dyn_curr[i],
                          kOrbGlow[i], 0.0f, 0.35f, 1.0f);
    }
    app._pbr.SetEmissive(Vec3{0, 0, 0}, 0.0f);   // 後続描画のため reset
}

} // namespace helloibl
