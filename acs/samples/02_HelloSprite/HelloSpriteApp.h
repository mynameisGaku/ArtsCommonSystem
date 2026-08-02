// SPDX-License-Identifier: Apache-2.0
// HelloSprite — 100 個のスプライトが画面内をバウンドするサンプル。
//
// 学習ポイント:
//   ・CSpriteBatch::Begin/Draw/End の使い方
//   ・ピクセル座標で 2D を描く（左上原点）
//   ・α ブレンド (EBlendMode::AlphaBlend)
//   ・テクスチャ無しの色矩形は DrawRect
#pragma once

#include "app/Application.h"
#include "render/SpriteBatch.h"
#include "render/IRhiTexture.h"
#include "memory/UniquePtr.h"

#include "Types.h"

namespace hellosprite {

class CHelloSpriteApp : public acs::CApplication {
public:
    void OnStart()            noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()           noexcept override;
    void OnShutdown()         noexcept override;

private:
    void SpawnSprites(acs::u32 n) noexcept;

    acs::CSpriteBatch                 m_Batch;
    acs::TUniquePtr<acs::IRhiTexture> m_Tex;
    FSprite                           m_Sprites[kMaxSprites] {};
    acs::u32                         m_SpriteCount = 0;
};

} // namespace hellosprite
