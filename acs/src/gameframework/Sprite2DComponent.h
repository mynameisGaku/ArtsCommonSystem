// SPDX-License-Identifier: Apache-2.0
// FSprite2DComponent - minimal render component for FNode2D.
//
// It draws a colored rectangle or a texture through the FSpriteBatch installed
// in RenderContext by FScene2D. Size is in world units; the owner transform
// supplies position/rotation/scale.
#pragma once

#include "gameframework/Component2D.h"
#include "math/Vec.h"

namespace acs {
class IRhiTexture;
}

namespace acs::game {

class FSprite2DComponent : public FComponent2D {
public:
    ACS_GAME_COMPONENT_KIND(FSprite2DComponent)

    FSprite2DComponent() noexcept = default;
    explicit FSprite2DComponent(FVec2 size, FVec4 tint = FVec4{1, 1, 1, 1}) noexcept
        : m_Size(size), m_Tint(tint) {}

    void SetTexture(IRhiTexture* tex) noexcept { m_Texture = tex; }
    IRhiTexture* Texture() const noexcept { return m_Texture; }

    void SetSize(FVec2 s) noexcept { m_Size = s; }
    FVec2 Size() const noexcept { return m_Size; }

    void SetTint(FVec4 c) noexcept { m_Tint = c; }
    FVec4 Tint() const noexcept { return m_Tint; }

    void SetPivot(FVec2 p) noexcept { m_Pivot = p; }
    FVec2 Pivot() const noexcept { return m_Pivot; }

    // UV サブ矩形 (スプライトシートの 1 フレーム等)。既定 {0,0,1,1} = テクスチャ全体。
    // FSpriteAnimComponent がフレーム毎にここを書き換えてアニメさせる。
    void SetUvRect(f32 u0, f32 v0, f32 u1, f32 v1) noexcept { m_UvMin = FVec2{u0, v0}; m_UvMax = FVec2{u1, v1}; }
    void SetUvRect(FVec4 uv) noexcept { m_UvMin = FVec2{uv.x, uv.y}; m_UvMax = FVec2{uv.z, uv.w}; }
    FVec2 UvMin() const noexcept { return m_UvMin; }
    FVec2 UvMax() const noexcept { return m_UvMax; }

    void OnDraw(RenderContext& rc) noexcept override;

private:
    IRhiTexture* m_Texture = nullptr;      // non-owning
    FVec2        m_Size{1.0f, 1.0f};
    FVec4        m_Tint{1.0f, 1.0f, 1.0f, 1.0f};
    FVec2        m_Pivot{0.5f, 0.5f};      // 0..1 inside size
    FVec2        m_UvMin{0.0f, 0.0f};      // UV 左上 (既定 = テクスチャ全体)
    FVec2        m_UvMax{1.0f, 1.0f};      // UV 右下
};

} // namespace acs::game
