// SPDX-License-Identifier: Apache-2.0
#include "HudRenderer.h"

#include "RaycastTargets.h"

#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/IRhiCommandList.h"

#include <cstdio>

using namespace acs;

namespace helloraycast3d {

void HudRenderer::Draw(SpriteBatch& batch,
                       Font& font,
                       IRhiCommandList& cl,
                       u32 screen_w,
                       u32 screen_h,
                       const RaycastTargets& targets) noexcept {
    // Font 未ロードならスキップ (Sample::TryLoadDefaultUIFont が失敗する環境を許容)
    if (!font.AtlasTexture()) return;

    batch.Begin(cl, screen_w, screen_h);

    // クロスヘア: 中心に水平 16x2 + 垂直 2x16 の白十字
    const f32 cx = screen_w * 0.5f;
    const f32 cy = screen_h * 0.5f;
    batch.DrawRect(cx - 8, cy - 1, 16, 2, Vec4{1, 1, 1, 0.8f});
    batch.DrawRect(cx - 1, cy - 8, 2, 16, Vec4{1, 1, 1, 0.8f});

    // 当たり情報: ヒット時はインデックスとワールド座標、未ヒットは "(no hit)"
    char buf[160];
    if (targets.HasHit()) {
        const Vec3 p = targets.HitPoint();
        std::snprintf(buf, sizeof(buf),
                      "HIT: object[%d] at (%.2f, %.2f, %.2f)",
                      targets.HitIndex(), p.x, p.y, p.z);
        batch.DrawString(font, buf, 20, 20, Vec4{1.0f, 0.9f, 0.4f, 1});
    } else {
        batch.DrawString(font, "(no hit)", 20, 20, Vec4{0.7f, 0.7f, 0.7f, 1});
    }
    batch.DrawString(font,
                     "WASD: 移動  矢印: 視点  Esc: 終了",
                     20, 44, Vec4{0.7f, 0.8f, 0.9f, 1});
    batch.End();
}

} // namespace helloraycast3d
