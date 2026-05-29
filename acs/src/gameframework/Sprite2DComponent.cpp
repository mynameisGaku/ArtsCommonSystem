// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Sprite2DComponent.h"

#include "gameframework/Node2D.h"
#include "gameframework/RenderContext.h"
#include "render/SpriteBatch.h"
#include "render/IRhiTexture.h"

namespace acs::game {

void FSprite2DComponent::OnDraw(RenderContext& rc) noexcept {
    if (!rc.HasSprites()) return;

    const FTransform2D wt = Owner().World();
    const f32 w = m_Size.x * wt.scale.x;
    const f32 h = m_Size.y * wt.scale.y;
    const f32 cx = wt.position.x + (0.5f - m_Pivot.x) * w;
    const f32 cy = wt.position.y + (0.5f - m_Pivot.y) * h;

    FSpriteBatch& sb = rc.Sprites();
    if (m_Texture) {
        sb.DrawRotated(*m_Texture, cx, cy, w, h, wt.rotation, 0.0f, 0.0f, 1.0f, 1.0f, m_Tint);
    } else {
        sb.DrawRectRotated(cx, cy, w, h, wt.rotation, m_Tint);
    }
}

} // namespace acs::game
