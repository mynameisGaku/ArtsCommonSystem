// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FTransform2D (Phase 5)
//
// 2D ノードの位置/回転/スケールを 20 byte の値型で表す。`FMat4` 直接保持より
// 小さく合成が速い、かつ分解が非可逆でない (= 親 transform の rotation/scale を
// 取り出して使える)。
//
// 合成規約:
//   `world = parent.Compose(local)` で「親の座標系に local を載せる」。
//   ・world.scale    = parent.scale * local.scale  (component-wise)
//   ・world.rotation = parent.rotation + local.rotation
//   ・world.position = parent.position + Rotate(parent.scale * local.position, parent.rotation)
//
// ToMat4() は FSpriteBatch::SetView や 4x4 行列が必要な場面でだけ使う (合成内では
// 使わない、誤差/コストを避けるため)。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "math/Math.h"

namespace acs::game {

struct FTransform2D {
    FVec2 position{0.0f, 0.0f};
    f32  rotation = 0.0f;     // radians
    FVec2 scale{1.0f, 1.0f};

    constexpr FTransform2D() noexcept = default;
    constexpr FTransform2D(FVec2 pos, f32 rot, FVec2 sca) noexcept
        : position(pos), rotation(rot), scale(sca) {}

    // 親の座標系に local を載せた world transform を返す (`world = parent.Compose(local)`)。
    FTransform2D Compose(const FTransform2D& local) const noexcept {
        // 親 scale で子 position をスケール → 親 rotation で回転 → 親 position を加算
        const f32 sx = scale.x * local.position.x;
        const f32 sy = scale.y * local.position.y;
        const f32 c  = Cos(rotation);
        const f32 s  = Sin(rotation);
        FTransform2D out;
        out.position.x = position.x + (sx * c - sy * s);
        out.position.y = position.y + (sx * s + sy * c);
        out.rotation   = rotation + local.rotation;
        out.scale.x    = scale.x * local.scale.x;
        out.scale.y    = scale.y * local.scale.y;
        return out;
    }

    // FSpriteBatch 等で 4x4 が必要なときの変換 (Z=0、scale on XY)。
    FMat4 ToMat4() const noexcept {
        const f32 c = Cos(rotation);
        const f32 s = Sin(rotation);
        // T * R * S を row-major で展開 (acs/Math 規約)
        FMat4 m{};
        m.m[0][0] = scale.x * c;   m.m[0][1] = scale.x * s;   m.m[0][2] = 0.0f; m.m[0][3] = 0.0f;
        m.m[1][0] = -scale.y * s;  m.m[1][1] = scale.y * c;   m.m[1][2] = 0.0f; m.m[1][3] = 0.0f;
        m.m[2][0] = 0.0f;          m.m[2][1] = 0.0f;          m.m[2][2] = 1.0f; m.m[2][3] = 0.0f;
        m.m[3][0] = position.x;    m.m[3][1] = position.y;    m.m[3][2] = 0.0f; m.m[3][3] = 1.0f;
        return m;
    }

    static constexpr FTransform2D Identity() noexcept { return {}; }
};

} // namespace acs::game
