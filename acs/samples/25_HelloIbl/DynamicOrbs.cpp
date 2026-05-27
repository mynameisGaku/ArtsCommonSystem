// SPDX-License-Identifier: Apache-2.0
// HelloIbl — グリッド前方を公転する発光オーブ 3 個。
//
// emissive + bloom で光り、motion vector を G-buffer に焼くので TAA の trail も
// 出ない。glossy な床にも映り込む (SSR が scene color = 発光込みを反射するため)。
//
// 各フレームで:
//   1) m_DynPrev ← 前フレームの m_DynCurr へ swap
//   2) m_DynCurr を m_AnimTime から再計算
// この prev/curr ペアを color pass と motion pass の両方が読む。
#include "HelloIblApp.h"

using namespace acs;

namespace helloibl {

namespace {

// オーブの発光色。emissive ベース + base_color にも同じ色を与えると、
// FPbrShader の emissive 寄与と reflection の見た目が一致する。
constexpr FVec3 kOrbGlow[kDynCount] = {
    FVec3{1.00f, 0.45f, 0.15f},   // 暖色オレンジ
    FVec3{0.25f, 1.00f, 0.40f},   // 緑
    FVec3{0.30f, 0.55f, 1.00f},   // 青
};

} // anonymous namespace

void UpdateDynamicOrbs(HelloIblApp& app) noexcept {
    // prev ← 前フレームの curr。OnCustomFrame の冒頭で呼ぶこと
    // (FPbrShader が curr を読み、FMotionVector が prev/curr を比べる)。
    for (u32 i = 0; i < kDynCount; ++i) {
        app.m_DynPrev[i] = app.m_DynCurr[i];
        app.m_DynCurr[i] = app.ComputeDynTransform(i, app.m_AnimTime);
    }
}

void DrawDynamicOrbs(HelloIblApp& app) noexcept {
    IRhiCommandList* cl = app.GetRenderer().CommandList();
    if (!cl) return;

    app.m_Pbr.SetExtParams(0.0f, 0.5f, 0.0f, FVec3{1, 0, 0});
    for (u32 i = 0; i < kDynCount; ++i) {
        app.m_Pbr.SetEmissive(kOrbGlow[i], /*strength=*/3.0f);
        app.m_Pbr.DrawMesh(*cl, app.m_GmSphere, app.m_DynCurr[i],
                          kOrbGlow[i], 0.0f, 0.35f, 1.0f);
    }
    app.m_Pbr.SetEmissive(FVec3{0, 0, 0}, 0.0f);   // 後続描画のため reset
}

} // namespace helloibl
