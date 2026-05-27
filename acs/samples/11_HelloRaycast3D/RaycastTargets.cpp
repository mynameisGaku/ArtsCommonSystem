// SPDX-License-Identifier: Apache-2.0
#include "RaycastTargets.h"

#include "math/Math.h"

using namespace acs;

namespace helloraycast3d {

void RaycastTargets::Init() noexcept {
    for (u32 i = 0; i < kNumObjects; ++i) {
        const f32 a = (kPi * 2.0f) * static_cast<f32>(i) / kNumObjects;
        Object& o = m_Objects[i];
        // 球と立方体を交互に / 上下に段差を付けて見栄えに変化を出す
        o.kind     = (i & 1) ? ShapeKind::FSphere : ShapeKind::Cube;
        o.position = { Sin(a) * kObjectArenaR,
                       1.0f + ((i & 2) ? 1.5f : 0.0f),
                       Cos(a) * kObjectArenaR };
        o.radius_or_half = kObjectRadius;
        // 色相を等分割し、それぞれ別の色合いに
        const f32 hue = static_cast<f32>(i) / kNumObjects;
        o.base_color  = m_HsvToRgb(hue, 0.6f, 0.95f);
    }
    m_HitIndex = -1;
}

void RaycastTargets::SetHit(i32 index, FVec3 point) noexcept {
    m_HitIndex = index;
    m_HitPoint = point;
}

void RaycastTargets::ClearHit() noexcept {
    m_HitIndex = -1;
}

FVec3 RaycastTargets::m_HsvToRgb(f32 h, f32 s, f32 v) noexcept {
    // h を [0,1) に折り返す (負値も拾えるように 2 段階で)
    h = h - static_cast<i32>(h);
    if (h < 0) h += 1.0f;

    const f32 c  = v * s;
    const f32 hp = h * 6.0f;
    // hp を 2 で割った余り → 0..2 に折り返したもの。中間色 x の計算に使う。
    const f32 x  = c * (1.0f - Abs(hp - 2.0f * static_cast<i32>(hp / 2.0f) - 1.0f));

    f32 r = 0, g = 0, b = 0;
    const i32 i = static_cast<i32>(hp);
    switch (i % 6) {
        case 0: r = c; g = x; b = 0; break;
        case 1: r = x; g = c; b = 0; break;
        case 2: r = 0; g = c; b = x; break;
        case 3: r = 0; g = x; b = c; break;
        case 4: r = x; g = 0; b = c; break;
        case 5: r = c; g = 0; b = x; break;
    }
    // 明度オフセット (v - c) を全成分に足して輝度を合わせる
    const f32 m = v - c;
    return { r + m, g + m, b + m };
}

} // namespace helloraycast3d
