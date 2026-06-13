// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FTransform3D
//
// 3D ノードの位置/回転/スケールを値型で表す (FTransform2D の 3D 版)。`FMat4` を
// 直接保持するより小さく合成が速く、分解が可逆 (親の rotation/scale を取り出せる)。
// 回転は FQuat (16B 整列) を使う。
//
// 合成規約 (FTransform2D と同じ「親の座標系に local を載せる」):
//   `world = parent.Compose(local)`
//   ・world.scale    = parent.scale * local.scale  (component-wise)
//   ・world.rotation = parent.rotation * local.rotation  (quat 合成: 先に local、次に parent)
//   ・world.position = parent.position + Rotate(parent.rotation, parent.scale * local.position)
//
// ToMat4() は描画/カメラ等で 4x4 が必要なときだけ使う (合成内では使わない)。
// row-major (acs/Math 規約)、v * M の行ベクトル順 = S * R * T。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "math/Quat.h"

namespace acs::game {

/**
 * 3D ノードの位置/回転/スケールを表す値型 transform (FTransform2D の 3D 版)。
 *
 * @details
 * FMat4 を直接保持するより小さく合成が速く、分解が可逆 (親の rotation/scale を取り
 * 出せる)。回転は FQuat。合成は `world = parent.Compose(local)` の規約で、scale は
 * component-wise 積、rotation は quat 合成 (先に local・次に parent)、position は親
 * scale でスケール後に親 rotation で回して親 position を加算する。
 */
struct FTransform3D {
    /** 位置 (親座標系での平行移動)。 */
    FVec3 position{0.0f, 0.0f, 0.0f};

    /** 回転 (クォータニオン、既定は恒等)。 */
    FQuat rotation{};

    /** スケール (X/Y/Z 個別)。 */
    FVec3 scale{1.0f, 1.0f, 1.0f};

    /** 単位 transform を構築する (位置 0、回転 恒等、スケール 1)。 */
    FTransform3D() noexcept = default;

    /**
     * 位置・回転・スケールを指定して構築する。
     *
     * @param pos 位置。
     * @param rot 回転 (クォータニオン)。
     * @param sca スケール (X/Y/Z)。
     */
    FTransform3D(FVec3 pos, FQuat rot, FVec3 sca) noexcept
        : position(pos), rotation(rot), scale(sca) {}

    /**
     * 親の座標系に local を載せた world transform を合成して返す。
     *
     * @details
     * `world = parent.Compose(local)` の規約。子 position を親 scale で
     * component-wise スケール → 親 rotation で回転 → 親 position を加算する。rotation は
     * `parent.rotation * local.rotation` (先に local・次に parent)、scale は component-wise 積。
     * @param local 親 (this) の座標系に載せるローカル transform。
     * @return 合成後の world transform。
     */
    FTransform3D Compose(const FTransform3D& local) const noexcept {
        FTransform3D out;
        // 親 scale で子 position を component-wise スケール → 親 rotation で回転 → 親 position 加算
        const FVec3 scaled{ scale.x * local.position.x,
                            scale.y * local.position.y,
                            scale.z * local.position.z };
        out.position = position + Rotate(rotation, scaled);
        out.rotation = rotation * local.rotation;
        out.scale    = FVec3{ scale.x * local.scale.x,
                              scale.y * local.scale.y,
                              scale.z * local.scale.z };
        return out;
    }

    /**
     * 4x4 行列に変換する (描画/カメラ等で 4x4 が必要なとき用)。
     *
     * @details
     * row-major (acs/Math 規約)、行ベクトル v*M 順で S * R * T を展開する。基底ベクトル
     * (rows 0-2) を scale で行ごとにスケールし、row 3 に position を置く。合成内では誤差/
     * コストを避けるため使わない。
     * @return この transform を表す 4x4 行列。
     */
    FMat4 ToMat4() const noexcept {
        const FMat4 r = ToMatrix(rotation);   // row-major 回転 (rows 0-2 = 回転基底, row3/col3 = identity)
        FMat4 m{};
        m.m[0][0] = scale.x * r.m[0][0]; m.m[0][1] = scale.x * r.m[0][1]; m.m[0][2] = scale.x * r.m[0][2]; m.m[0][3] = 0.0f;
        m.m[1][0] = scale.y * r.m[1][0]; m.m[1][1] = scale.y * r.m[1][1]; m.m[1][2] = scale.y * r.m[1][2]; m.m[1][3] = 0.0f;
        m.m[2][0] = scale.z * r.m[2][0]; m.m[2][1] = scale.z * r.m[2][1]; m.m[2][2] = scale.z * r.m[2][2]; m.m[2][3] = 0.0f;
        m.m[3][0] = position.x;          m.m[3][1] = position.y;          m.m[3][2] = position.z;          m.m[3][3] = 1.0f;
        return m;
    }

    /**
     * 単位 transform を返す (位置 0、回転 恒等、スケール 1)。
     *
     * @return 単位 transform。
     */
    static FTransform3D Identity() noexcept { return {}; }
};

} // namespace acs::game
