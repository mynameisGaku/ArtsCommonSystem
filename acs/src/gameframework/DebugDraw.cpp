// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FDebugDraw 実装
//
// ヘッダの「設計選択」を参照。ここではジオメトリ生成のみで I/O 副作用を持たない。
// 各 Draw* は内部 TArray に Line を PushBack するだけ。
#include "gameframework/DebugDraw.h"

#include "math/Math.h"

namespace acs::game {

void FDebugDraw::DrawLine(FVec2 a, FVec2 b, FVec4 color) noexcept {
    m_Lines.PushBack(Line{a, b, color});
}

void FDebugDraw::DrawAabb(const Aabb2& a, FVec4 color) noexcept {
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
    m_Lines.PushBack(Line{tl, tr, color});  // 上辺
    m_Lines.PushBack(Line{tr, br, color});  // 右辺
    m_Lines.PushBack(Line{br, bl, color});  // 下辺
    m_Lines.PushBack(Line{bl, tl, color});  // 左辺
}

void FDebugDraw::DrawCircle(const Circle& c, FVec4 color, u32 segments) noexcept {
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
        m_Lines.PushBack(Line{prev, curr, color});
        prev = curr;
    }
    // i = segments のとき theta = 2π となり、開始点に戻るため自然に閉じる。
}

void FDebugDraw::DrawCross(FVec2 pos, f32 size, FVec4 color) noexcept {
    // "+" 記号: 横線（左右）+ 縦線（上下）。size は片側長。
    const f32 h = size;
    m_Lines.PushBack(Line{FVec2{pos.x - h, pos.y}, FVec2{pos.x + h, pos.y}, color});
    m_Lines.PushBack(Line{FVec2{pos.x, pos.y - h}, FVec2{pos.x, pos.y + h}, color});
}

} // namespace acs::game
