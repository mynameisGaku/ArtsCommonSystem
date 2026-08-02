// SPDX-License-Identifier: Apache-2.0
// HelloIbl — CApplication 派生クラス。
//
// IBL + HDR / ACES tonemap のメインデモ。BRDF LUT / env cube / irradiance /
// prefilter を初フレームで一括生成し、以降は preset 切替 / displaymode /
// material lobe 行 (sheen / iridescence / subsurface) と post-process (Bloom +
// ACES + CAS sharpening + auto-exposure + grading) を組み合わせて表示する。
// Shadow / SSR / SSAO / SSGI / Motion / TAA / Refraction / Lightmap が全部入りで
// キーで toggle 可能。フル機能 sample。
//
// 実装は機能ごとに分割している:
//   ShadowPass.{h,cpp}            - CSM (3 cascade atlas) caster
//   GBufferPass.{h,cpp}           - motion vector + world normal MRT
//   ScreenSpaceEffects.{h,cpp}    - SSR / SSAO / SSGI dispatch
//   RefractionPass.{h,cpp}        - screen-space 屈折ガラス
//   DynamicOrbs.{h,cpp}           - 公転する発光オーブ
//   SceneDraw.{h,cpp}             - 5x5 sphere grid + floor の PBR draw
//   PbrLightingBindings.{h,cpp}   - CPbrShader の各種テクスチャ/パラメータ bind
//   IblPresetBuilder.{h,cpp}      - preset 切替時の env / irradiance / prefilter 再生成
//   TaaJitter.{h,cpp}             - Halton(2,3) sub-pixel jitter
//   ExposureControl.{h,cpp}       - auto / manual 露出補間
//   HudOverlay.{h,cpp}            - CSpriteBatch HUD + デバッグオーバーレイ
//
// 操作 (主要なもの):
//   1/2/3 空プリセット / 4 スタジオ HDR / 5 大気
//   I 表示モード / S SH9 / C クリアコート / Z 異方性 / L 面光源 / G プローブ / F 霧
//   H 影 / R SSR / X 屈折 / O SSAO / T TAA / J SSGI / K ライトマップ / M モーション
//   B ブルーム / U 自動露出 / Q-E 露出 / WASD 移動 / 矢印 視点 / Esc 終了
#pragma once

#include "app/Application.h"
#include "render/Ibl.h"
#include "render/Sky.h"
#include "render/PbrShader.h"
#include "render/RenderAssets.h"
#include "render/ShadowMap.h"
#include "render/Ssr.h"
#include "render/Ssao.h"
#include "render/Ssgi.h"
#include "render/MotionVector.h"
#include "render/RefractionShader.h"
#include "render/Blit.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/PostProcess.h"

#include "container/Array.h"
#include "math/Camera.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "memory/UniquePtr.h"
#include "foundation/Types.h"

#include "IblTypes.h"

namespace helloibl {

// 機能別 helper の前方宣言 (friend にして private メンバへアクセスさせる)。
class CHelloIblApp;
void ApplyPresetRebuilds(CHelloIblApp& app) noexcept;
acs::FMat4 BuildJitteredViewProjection(CHelloIblApp& app, const acs::FMat4& vp_no_jitter,
                                      acs::u32 hdr_width, acs::u32 hdr_height) noexcept;
void RenderShadowPass(CHelloIblApp& app, const acs::FVec3& sun_dir) noexcept;
acs::FVec3 ResolveSunDirection(const CHelloIblApp& app) noexcept;
void BindPbrLighting(CHelloIblApp& app, const acs::FMat4& vp_for_render,
                     const acs::FDirLight& sun) noexcept;
void DrawFloor(CHelloIblApp& app) noexcept;
void DrawSphereGrid(CHelloIblApp& app) noexcept;
void UpdateDynamicOrbs(CHelloIblApp& app) noexcept;
void DrawDynamicOrbs(CHelloIblApp& app) noexcept;
void RenderRefractionPass(CHelloIblApp& app, const acs::FMat4& vp_for_render,
                          const acs::FViewport& vp, const acs::FScissorRect& svr) noexcept;
void RenderMotionAndNormalGBuffer(CHelloIblApp& app,
                                  const acs::FMat4& vp_no_jitter) noexcept;
void RenderSsrPass(CHelloIblApp& app, const acs::FMat4& vp_for_render,
                   const acs::FMat4& inv_vp, const acs::FMat4& vp_no_jitter) noexcept;
void RenderSsaoPass(CHelloIblApp& app, const acs::FMat4& vp_for_render,
                    const acs::FMat4& inv_vp, const acs::FVec3& sun_dir) noexcept;
void RenderSsgiPass(CHelloIblApp& app, const acs::FMat4& vp_for_render,
                    const acs::FMat4& inv_vp, const acs::FMat4& vp_no_jitter) noexcept;
void UpdateExposureControls(CHelloIblApp& app, acs::f32 dt) noexcept;
void DrawBrdfLutOverlay(CHelloIblApp& app, acs::IRhiTexture* lut, acs::u32 sw) noexcept;
void DrawSsrDebugOverlay(CHelloIblApp& app, acs::u32 sh) noexcept;
void DrawSsaoDebugOverlay(CHelloIblApp& app, acs::u32 sw, acs::u32 sh) noexcept;
void DrawStatusText(CHelloIblApp& app, acs::IRhiTexture* lut, acs::u32 sw) noexcept;
void DrawHud(CHelloIblApp& app, acs::u32 sw, acs::u32 sh) noexcept;

class CHelloIblApp : public acs::CApplication {
public:
    void OnStart() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    bool OnCustomFrame() noexcept override;
    void OnShutdown() noexcept override;

private:
    // 動的球 i の時刻 t における transform。中心 (0, 3, 1) のまわりを XY 平面で
    // 公転させる (画面内を大きく掃くので、motion vector 無しだと TAA で trail が出る)。
    acs::FMat4 ComputeDynTransform(acs::u32 i, acs::f32 t) const noexcept;

    // helper モジュール群は private メンバを直接触る (friend)。
    friend void ApplyPresetRebuilds(CHelloIblApp&) noexcept;
    friend acs::FMat4 BuildJitteredViewProjection(CHelloIblApp&, const acs::FMat4&,
                                                 acs::u32, acs::u32) noexcept;
    friend void RenderShadowPass(CHelloIblApp&, const acs::FVec3&) noexcept;
    friend acs::FVec3 ResolveSunDirection(const CHelloIblApp&) noexcept;
    friend void BindPbrLighting(CHelloIblApp&, const acs::FMat4&, const acs::FDirLight&) noexcept;
    // BindPbrLighting の各サブ bind helper (PbrLightingBindings.cpp、private へ直接 bind)。
    friend void BindIbl(CHelloIblApp&) noexcept;
    friend void BindSun(CHelloIblApp&, const acs::FMat4&, const acs::FDirLight&) noexcept;
    friend void BindSsao(CHelloIblApp&) noexcept;
    friend void BindSsgi(CHelloIblApp&) noexcept;
    friend void BindSsr(CHelloIblApp&) noexcept;
    friend void BindShadow(CHelloIblApp&) noexcept;
    friend void BindFog(CHelloIblApp&) noexcept;
    friend void BindProbeGrid(CHelloIblApp&) noexcept;
    friend void BindAreaLight(CHelloIblApp&) noexcept;
    friend void DrawFloor(CHelloIblApp&) noexcept;
    friend void DrawSphereGrid(CHelloIblApp&) noexcept;
    friend void UpdateDynamicOrbs(CHelloIblApp&) noexcept;
    friend void DrawDynamicOrbs(CHelloIblApp&) noexcept;
    friend void RenderRefractionPass(CHelloIblApp&, const acs::FMat4&,
                                     const acs::FViewport&, const acs::FScissorRect&) noexcept;
    friend void RenderMotionAndNormalGBuffer(CHelloIblApp&, const acs::FMat4&) noexcept;
    friend void RenderSsrPass(CHelloIblApp&, const acs::FMat4&, const acs::FMat4&,
                              const acs::FMat4&) noexcept;
    friend void RenderSsaoPass(CHelloIblApp&, const acs::FMat4&, const acs::FMat4&,
                               const acs::FVec3&) noexcept;
    friend void RenderSsgiPass(CHelloIblApp&, const acs::FMat4&, const acs::FMat4&,
                               const acs::FMat4&) noexcept;
    friend void UpdateExposureControls(CHelloIblApp&, acs::f32) noexcept;
    friend void DrawBrdfLutOverlay(CHelloIblApp&, acs::IRhiTexture*, acs::u32) noexcept;
    friend void DrawSsrDebugOverlay(CHelloIblApp&, acs::u32) noexcept;
    friend void DrawSsaoDebugOverlay(CHelloIblApp&, acs::u32, acs::u32) noexcept;
    friend void DrawStatusText(CHelloIblApp&, acs::IRhiTexture*, acs::u32) noexcept;
    friend void DrawHud(CHelloIblApp&, acs::u32, acs::u32) noexcept;

    acs::CPostProcess        m_Post;
    acs::CImageBasedLighting m_Ibl;
    acs::CSky                m_Sky;
    acs::CPbrShader          m_Pbr;
    acs::FGpuMesh            m_GmSphere;
    acs::FGpuMesh            m_GmPlane;
    acs::CSpriteBatch        m_Batch;
    acs::FFont               m_Font;
    acs::CCamera             m_Camera;
    acs::FPostProcessParams  m_PostParams;
    acs::TArray<acs::f32>    m_EquirectRgba;          // 4 ch float
    acs::FVec4               m_Sh9[9]   = {};          // 計算済 SH 9 係数 (xyz=RGB)
    acs::FVec3               m_CamPos  = acs::FVec3{0, 1.0f, -5.0f};
    acs::f32                m_CamYaw   = 0.0f;
    acs::f32                m_CamPitch = 0.0f;
    acs::i32                m_CurrentPreset = 0;
    bool                    m_bNeedRecapture   = false;
    bool                    m_bNeedStudioHdr  = false;
    bool                    m_bUseSh9          = false;
    bool                    m_bNeedSh9Rebuild = false;
    bool                    m_bUseClearcoat    = false;
    bool                    m_bUseAnisotropy   = false;
    bool                    m_bUseAreaLight   = false;
    bool                    m_bUseProbeGrid   = false;
    bool                    m_bUseFog          = false;
    bool                    m_bNeedAtmosphere  = false;
    bool                    m_bUseShadows      = false;
    bool                    m_ShowSsr         = false;
    bool                    m_SsrWarm         = false; // m_Ssr.Render が 1 度以上走った？
    bool                    m_bUseSsao         = true;  // CPbrShader 側で composite (1-frame latency)
    bool                    m_bSsaoWarm        = false; // m_Ssao.Render が 1 度以上走った？ (frame 0 garbage 回避)
    bool                    m_bUseTaa          = true;
    acs::u32                m_TaaFrameIndex  = 0;     // Halton(2,3) 用カウンタ
    acs::FMat4               m_PrevVpNoJitter{};      // 前フレームの jitter なし VP
    bool                    m_TaaPrevVpValid = false;// 上が本物の VP (default identity 以外) か
    acs::f32                m_ExposureTarget  = 0.7f;  // 露出目標 (preset / Q-E で動く)
    acs::f32                m_AdaptedExposure = 0.7f;  // 実露出 (target へ dt 補間)
    bool                    m_bUseAutoExposure = true; // GPU auto-exposure ('U' で手動切替)
    acs::f32                m_AutoKey          = 0.5f; // 自動露出の目標平均輝度 (Q/E で調整)
    bool                    m_bUseSsgi         = true;
    bool                    m_bSsgiWarm        = false; // m_Ssgi.Render が 1 度以上走った？
    bool                    m_bUseLightmap     = true;
    acs::TUniquePtr<acs::IRhiTexture> m_Lightmap;        // 床用 baked lightmap (256x256 RGBA8)
    acs::CShadowMap          m_Shadow;
    acs::CSsr                m_Ssr;
    acs::CSsao               m_Ssao;
    acs::CSsgi               m_Ssgi;
    acs::CMotionVector       m_Motion;
    bool                    m_MotionGBufferValid = false;
    bool                    m_bUseMotionVec   = true;
    acs::CRefractionShader   m_Refr;                     // screen-space 屈折
    acs::CBlit               m_Blit;                     // HDR -> m_BgRt コピー
    acs::TUniquePtr<acs::IRhiTexture> m_BgRt;           // 屈折用 background キャプチャ
    bool                    m_ShowRefraction  = true;
    acs::FMat4               m_DynCurr[kDynCount] = {}; // 動的球の現フレーム transform
    acs::FMat4               m_DynPrev[kDynCount] = {}; // 動的球の前フレーム transform
    acs::f32                m_AnimTime        = 0.0f;  // 動的球公転の時刻アキュムレータ
    acs::u32                m_DisplayMode     = 0;
};

} // namespace helloibl
