// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Sprite2DComponent.h"

#include "gameframework/ANode.h"
#include "gameframework/RenderContext.h"
#include "render/SpriteBatch.h"
#include "render/IRhiTexture.h"

namespace acs::game {

/** owner の world transform から矩形を計算し、テクスチャ有無で描き分けて SpriteBatch へ積む。 */
void ASprite2DComponent::OnDraw(FRenderContext& rc) noexcept {
    if (!rc.HasSprites()) return;

    const FTransform2D wt = Owner().World2D();
    const f32 w = m_Size.x * wt.scale.x;
    const f32 h = m_Size.y * wt.scale.y;
    const f32 cx = wt.position.x + (0.5f - m_Pivot.x) * w;
    const f32 cy = wt.position.y + (0.5f - m_Pivot.y) * h;

    CSpriteBatch& sb = rc.Sprites();
    if (m_Texture) {
        sb.DrawRotated(*m_Texture, cx, cy, w, h, wt.rotation,
                       m_UvMin.x, m_UvMin.y, m_UvMax.x, m_UvMax.y, m_Tint);
    } else {
        sb.DrawRectRotated(cx, cy, w, h, wt.rotation, m_Tint);
    }
}

} // namespace acs::game
