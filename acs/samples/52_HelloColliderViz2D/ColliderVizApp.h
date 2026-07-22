// SPDX-License-Identifier: Apache-2.0
// HelloColliderViz2D — スプライトコライダーを目視確認するデモ。
// 不定形スプライトを描き、その上に凸包 (緑) と簡略化輪郭 (シアン) を線で重ねる。
// マウスのプローブ円が凸包コライダーに重なると赤く表示 (FCollisionWorld2D 経由)。
#pragma once

#include "app/Application.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "collision/SpriteCollider.h"
#include "gameframework/CollisionWorld2D.h"
#include "memory/UniquePtr.h"

namespace colliderviz {

class FColliderVizApp : public acs::FApplication {
public:
    void OnStart() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender() noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::FSpriteBatch m_Batch;
    acs::FFont         m_Font;
    bool              m_bFontReady = false;

    acs::TUniquePtr<acs::IRhiTexture> m_SpriteTex;
    acs::FSpriteCollider              m_Collider;
    acs::game::FCollisionWorld2D      m_World;
    acs::game::FShapeId               m_PolyId;

    acs::u32  m_TexW = 0, m_TexH = 0;
    acs::FVec2 m_SpritePos{0, 0};      // 画面上のスプライト左上
    acs::f32  m_Scale = 3.0f;          // 表示倍率
    acs::FVec2 m_Probe{0, 0};          // マウス追従プローブ
    bool      m_bOverlap = false;
};

} // namespace colliderviz
