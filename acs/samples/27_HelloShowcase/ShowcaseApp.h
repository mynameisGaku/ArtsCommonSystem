// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — FApplication 派生のメインクラス。
// PBR / IBL / SSR / SSAO / Refraction / Bloom / ACES + TAA を 1 つのフレームで
// 連携させる cinematic demo。auto-orbit カメラで scene を 1 周。
//
// 実装は pass 系 helper (PbrPass / RefractionPass / MotionPass / SsrPass /
// BloomPass / HudPass) と GPU resource bundle (FAssets) に分割している。
// FShowcaseApp 自身は state (camera, orbit, toggle) と OnStart / OnUpdate /
// OnCustomFrame の orchestration だけを持つ。
//
// キー:
//   P  : auto-orbit を pause / resume
//   R  : SSR (env reflection mix) の toggle
//   X  : refraction glass の toggle
//   Esc: 終了
#pragma once

#include "ShowcaseAssets.h"

#include "app/Application.h"
#include "math/Camera.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "render/PostProcess.h"

namespace helloshowcase {

class FShowcaseApp : public acs::FApplication {
public:
    FShowcaseApp() noexcept;
    ~FShowcaseApp() noexcept override;

    void OnStart() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    bool OnCustomFrame() noexcept override;
    void OnShutdown() noexcept override;

private:
    FAssets                  m_Assets;
    acs::FCamera             m_Camera;
    acs::FPostProcessParams  m_PostParams;
    acs::FVec3               m_CamPos        = acs::FVec3{0, 1.4f, -5.5f};
    acs::f32                m_OrbitAngle    = 0.0f;     // カメラ orbit (rad)
    acs::f32                m_OrbPhase      = 0.0f;     // emissive オーブの位相 (rad)
    acs::f32                m_PrevOrbPhase = 0.0f;
    acs::f32                m_ExposureTarget  = 0.7f;
    acs::f32                m_AdaptedExposure = 0.7f;
    acs::FMat4               m_PrevVpNoJitter{};
    bool                    m_bPrevVpValid   = false;
    acs::u32                m_TaaFrameIndex = 0;
    bool                    m_ShowSsr        = true;
    bool                    m_SsrWarm        = false;
    bool                    m_bSsaoWarm       = false;
    bool                    m_ShowRefraction = true;
    bool                    m_Paused          = false;
};

} // namespace helloshowcase
