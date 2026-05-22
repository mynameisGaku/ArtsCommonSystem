// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — DebugDraw 実装
//
// ヘッダの「設計選択」を参照。ここではジオメトリ生成のみで I/O 副作用を持たない。
// 各 Draw* は内部 Array に Line を PushBack するだけ。
#include "gameframework/DebugDraw.h"

#include "math/Math.h"

namespace acs::game {

void DebugDraw::DrawLine(Vec2 a, Vec2 b, Vec4 color) noexcept {
    _lines.PushBack(Line{a, b, color});
}

void DebugDraw::DrawAabb(const Aabb2& a, Vec4 color) noexcept {
    // 4 隅 → 4 辺。
    //   tl --- tr
    //    |     |
    //   bl --- br
    const Vec2 mn = a.Min();
    const Vec2 mx = a.Max();
    const Vec2 tl{mn.x, mn.y};
    const Vec2 tr{mx.x, mn.y};
    const Vec2 br{mx.x, mx.y};
    const Vec2 bl{mn.x, mx.y};
    _lines.PushBack(Line{tl, tr, color});  // 上辺
    _lines.PushBack(Line{tr, br, color});  // 右辺
    _lines.PushBack(Line{br, bl, color});  // 下辺
    _lines.PushBack(Line{bl, tl, color});  // 左辺
}

void DebugDraw::DrawCircle(const Circle& c, Vec4 color, u32 segments) noexcept {
    // 縮退ガード: 3 角形未満は形にならない。
    if (segments < 3u) segments = 3u;

    const f32 inv_seg = 1.0f / static_cast<f32>(segments);
    const f32 step    = kTwoPi * inv_seg;

    // 最初の点（angle = 0）を計算しておき、ループで前点 → 現点を線で繋ぐ。
    Vec2 prev{c.center.x + c.radius, c.center.y};
    for (u32 i = 1; i <= segments; ++i) {
        const f32 theta = step * static_cast<f32>(i);
        const Vec2 curr{c.center.x + c.radius * Cos(theta),
                        c.center.y + c.radius * Sin(theta)};
        _lines.PushBack(Line{prev, curr, color});
        prev = curr;
    }
    // i = segments のとき theta = 2π となり、開始点に戻るため自然に閉じる。
}

void DebugDraw::DrawCross(Vec2 pos, f32 size, Vec4 color) noexcept {
    // "+" 記号: 横線（左右）+ 縦線（上下）。size は片側長。
    const f32 h = size;
    _lines.PushBack(Line{Vec2{pos.x - h, pos.y}, Vec2{pos.x + h, pos.y}, color});
    _lines.PushBack(Line{Vec2{pos.x, pos.y - h}, Vec2{pos.x, pos.y + h}, color});
}

} // namespace acs::game
