// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "math/Math.h"

namespace acs::game {

/**
 * 2D ノードの位置/回転/スケールを表す 20 byte の値型 transform。
 *
 * @details
 * FMat4 を直接保持するより小さく合成が速く、かつ分解が可逆 (親の rotation/scale を
 * 取り出して使える)。合成は `world = parent.Compose(local)` の規約で行い、scale は
 * component-wise 積、rotation は和、position は親 scale でスケール後に親 rotation で
 * 回して親 position を加算する。
 */
struct FTransform2D {
    /** 位置 (親座標系での平行移動)。 */
    FVec2 position{0.0f, 0.0f};

    /** 回転 (ラジアン、反時計回り)。 */
    f32  rotation = 0.0f;

    /** スケール (X/Y 個別)。 */
    FVec2 scale{1.0f, 1.0f};

    /** 単位 transform を構築する (位置 0、回転 0、スケール 1)。 */
    constexpr FTransform2D() noexcept = default;

    /**
     * 位置・回転・スケールを指定して構築する。
     *
     * @param pos 位置。
     * @param rot 回転 (ラジアン)。
     * @param sca スケール (X/Y)。
     */
    constexpr FTransform2D(FVec2 pos, f32 rot, FVec2 sca) noexcept
        : position(pos), rotation(rot), scale(sca) {}

    /**
     * 親の座標系に local を載せた world transform を合成して返す。
     *
     * @details
     * `world = parent.Compose(local)` の規約。子 position を親 scale でスケールし、
     * 親 rotation で回転して親 position を加算する。rotation は親子の和、scale は
     * component-wise 積になる。
     * @param local 親 (this) の座標系に載せるローカル transform。
     * @return 合成後の world transform。
     */
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

    /**
     * 4x4 行列に変換する (CSpriteBatch::SetView 等で 4x4 が必要なとき用)。
     *
     * @details Z=0 平面で S * R * T を row-major (acs/Math の行ベクトル規約) で展開する。合成内では誤差/コストを避けるため使わない。
     * @return この transform を表す 4x4 行列。
     */
    FMat4 ToMat4() const noexcept {
        const f32 c = Cos(rotation);
        const f32 s = Sin(rotation);
        // S * R * T を row-major で展開 (acs/Math の行ベクトル規約)
        FMat4 m{};
        m.m[0][0] = scale.x * c;   m.m[0][1] = scale.x * s;   m.m[0][2] = 0.0f; m.m[0][3] = 0.0f;
        m.m[1][0] = -scale.y * s;  m.m[1][1] = scale.y * c;   m.m[1][2] = 0.0f; m.m[1][3] = 0.0f;
        m.m[2][0] = 0.0f;          m.m[2][1] = 0.0f;          m.m[2][2] = 1.0f; m.m[2][3] = 0.0f;
        m.m[3][0] = position.x;    m.m[3][1] = position.y;    m.m[3][2] = 0.0f; m.m[3][3] = 1.0f;
        return m;
    }

    /**
     * 単位 transform を返す (位置 0、回転 0、スケール 1)。
     *
     * @return 単位 transform。
     */
    static constexpr FTransform2D Identity() noexcept { return {}; }
};

} // namespace acs::game
