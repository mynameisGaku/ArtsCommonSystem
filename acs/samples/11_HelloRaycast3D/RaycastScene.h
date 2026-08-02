// SPDX-License-Identifier: Apache-2.0
// HelloRaycast3D — シーンのオーケストレーション。
//
// 役割:
//   ・GPU メッシュ (球 / 立方体 / 地面) の所有
//   ・RaycastTargets + RayCaster + HudRenderer の協調
//   ・CStandardShader への描画コマンド発行
//
// CApplication 派生はリソース所有 (CStandardShader / CSpriteBatch / FFont) を担当し、
// 毎フレームの update / render を RaycastScene に委譲する。
#pragma once

#include "Types.h"
#include "RaycastTargets.h"
#include "RayCaster.h"

#include "render/StandardShader.h"
#include "render/RenderAssets.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/IRhiCommandList.h"

namespace helloraycast3d {

class CRaycastScene {
public:
    // プリミティブを GPU にアップロードしてカメラ + オブジェクト配置を構成。
    bool Init(acs::IRhiDevice& dev, acs::f32 aspect) noexcept;

    void Shutdown() noexcept;

    // カメラ入力 + レイキャスト判定。
    void Update(acs::f32 dt) noexcept;

    // 3D + HUD を描画。HUD 用の screen_w / screen_h は CSpriteBatch::Begin に必要。
    void Render(acs::CStandardShader& shader,
                acs::IRhiCommandList& cl,
                acs::CSpriteBatch& batch,
                acs::FFont& font,
                acs::u32 screen_w,
                acs::u32 screen_h) noexcept;

private:
    void m_RenderTargets(acs::CStandardShader& shader,
                         acs::IRhiCommandList& cl) noexcept;

    acs::FGpuMesh    m_GmSphere{};
    acs::FGpuMesh    m_GmCube{};
    acs::FGpuMesh    m_GmPlane{};
    CRaycastTargets  m_Targets;
    CRayCaster       m_Caster;
};

} // namespace helloraycast3d
