// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FDebugDraw 実装
//
// ヘッダの「設計選択」を参照。ここではジオメトリ生成のみで I/O 副作用を持たない。
// 各 Draw* は内部 TArray に Line を PushBack するだけ。
#include "gameframework/DebugDraw.h"

#include "math/Math.h"

#include <cmath>   // std::sqrt

namespace acs::game {

/** 任意 2 点間の線分を 1 本バッファへ積む。 */
void FDebugDraw::DrawLine(FVec2 a, FVec2 b, FVec4 color) noexcept {
    m_Lines.PushBack(FLine{a, b, color});
}

/** AABB の 4 隅を 4 辺の線分に分解して積む。 */
void FDebugDraw::DrawAabb(const FAabb2& a, FVec4 color) noexcept {
    // 4 隅 → 4 辺。
    //   tl --- tr
    //    |     |
    //   bl --- br
    const FVec2 mn = a.Min();
    const FVec2 mx = a.Max();
    const FVec2 tl{mn.x, mn.y};
    const FVec2 tr{mx.x, mn.y};
    const FVec2 br{mx.x, mx.y};
    const FVec2 bl{mn.x, mx.y};
    m_Lines.PushBack(FLine{tl, tr, color});  // 上辺
    m_Lines.PushBack(FLine{tr, br, color});  // 右辺
    m_Lines.PushBack(FLine{br, bl, color});  // 下辺
    m_Lines.PushBack(FLine{bl, tl, color});  // 左辺
}

/** 円を segments 本の線分に分解した近似輪郭を積む (segments<3 は 3 に丸める)。 */
void FDebugDraw::DrawCircle(const FCircle& c, FVec4 color, u32 segments) noexcept {
    // 縮退ガード: 3 角形未満は形にならない。
    if (segments < 3u) segments = 3u;

    const f32 inv_seg = 1.0f / static_cast<f32>(segments);
    const f32 step    = kTwoPi * inv_seg;

    // 最初の点（angle = 0）を計算しておき、ループで前点 → 現点を線で繋ぐ。
    FVec2 prev{c.center.x + c.radius, c.center.y};
    for (u32 i = 1; i <= segments; ++i) {
        const f32 theta = step * static_cast<f32>(i);
        const FVec2 curr{c.center.x + c.radius * Cos(theta),
                        c.center.y + c.radius * Sin(theta)};
        m_Lines.PushBack(FLine{prev, curr, color});
        prev = curr;
    }
    // i = segments のとき theta = 2π となり、開始点に戻るため自然に閉じる。
}

/** 中心 pos の "+" 記号を横線・縦線の 2 本として積む (size は片側長)。 */
void FDebugDraw::DrawCross(FVec2 pos, f32 size, FVec4 color) noexcept {
    // "+" 記号: 横線（左右）+ 縦線（上下）。size は片側長。
    const f32 h = size;
    m_Lines.PushBack(FLine{FVec2{pos.x - h, pos.y}, FVec2{pos.x + h, pos.y}, color});
    m_Lines.PushBack(FLine{FVec2{pos.x, pos.y - h}, FVec2{pos.x, pos.y + h}, color});
}

/** a→b の矢印: 軸 1 本 + 矢じり 2 本 (b から後方へ ±約23°)。 */
void FDebugDraw::DrawArrow(FVec2 a, FVec2 b, FVec4 color, f32 head_len) noexcept {
    m_Lines.PushBack(FLine{a, b, color});   // 軸
    const FVec2 dv{ b.x - a.x, b.y - a.y };
    const f32   len = std::sqrt(dv.x * dv.x + dv.y * dv.y);
    if (len < 1e-6f) return;               // 退化: 矢じり無し
    const FVec2 dir{ dv.x / len, dv.y / len };
    const f32   hl  = (head_len > 0.0f) ? head_len : len * 0.2f;

    // 後方ベクトル (-dir) を ±ang 回転して矢じり 2 本。
    const FVec2 back{ -dir.x, -dir.y };
    const f32   c = Cos(0.4f), s = Sin(0.4f);
    const FVec2 l{ back.x * c - back.y * s, back.x * s + back.y * c };   // +ang
    const FVec2 r{ back.x * c + back.y * s, -back.x * s + back.y * c };  // -ang
    m_Lines.PushBack(FLine{b, FVec2{b.x + l.x * hl, b.y + l.y * hl}, color});
    m_Lines.PushBack(FLine{b, FVec2{b.x + r.x * hl, b.y + r.y * hl}, color});
}

} // namespace acs::game
