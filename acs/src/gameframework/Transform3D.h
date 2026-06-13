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
#include "math/Math.h"

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
        // world 向き: 子ローカル回転を先に、親回転を後に適用 = local * parent
        // (codebase の a*b は «a を先に・b を後に» 適用するため。Rotate(a*b,v)=Rotate(b,Rotate(a,v)))。
        out.rotation = local.rotation * rotation;
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
     * オイラー角 (度、XYZ 順) から rotation を設定する。
     *
     * @details
     * editor_abi の Node3DModel と同じ合成順 `Rx * Ry * Rz` (row-vector で点に X→Y→Z の順で
     * 適用) になるよう quaternion を組む: `rotation = qx * qy * qz` (codebase の `a*b` は a を
     * 先に適用するため、ToMatrix が Rx*Ry*Rz と一致する)。エディタの度数オイラー編集を
     * クォータニオン保持の transform に橋渡しする。
     * @param deg X/Y/Z 各軸のオイラー角 (度)。
     */
    void SetEulerDeg(FVec3 deg) noexcept {
        const FQuat qx = FQuat::AxisAngle(FVec3{1, 0, 0}, deg.x * kDeg2Rad);
        const FQuat qy = FQuat::AxisAngle(FVec3{0, 1, 0}, deg.y * kDeg2Rad);
        const FQuat qz = FQuat::AxisAngle(FVec3{0, 0, 1}, deg.z * kDeg2Rad);
        // a*b は «a 先 / b 後» 適用なので、Rx→Ry→Rz の順に v へ効かせるには qx*qy*qz。
        // → Rotate(rotation, v) == v * (Rx*Ry*Rz) (editor Node3DModel と一致)。
        rotation = Normalize(qx * qy * qz);
    }

    /**
     * 現在の rotation をオイラー角 (度、XYZ 順) に分解して返す。
     *
     * @details
     * `Rx * Ry * Rz` 分解 (SetEulerDeg の逆)。回転行列 (row-major) から
     * β=asin(-m02)、α=atan2(m12,m22)、γ=atan2(m01,m00) を取り出す。Y が ±90° 近傍
     * (ジンバルロック) では γ=0 に縮退させ α に寄せる。|Y|<90° では SetEulerDeg と往復一致。
     * @return X/Y/Z 各軸のオイラー角 (度)。
     */
    FVec3 EulerDeg() const noexcept {
        const FMat4 m = ToMatrix(rotation);          // row-major、Rotate(q,v)=v*m
        f32 sy = -m.m[0][2];                          // -sin(Y)
        sy = (sy < -1.0f) ? -1.0f : (sy > 1.0f ? 1.0f : sy);   // asin 用にクランプ
        const f32 beta = ASin(sy);                   // Y
        const f32 cy   = Sqrt(1.0f - sy * sy);       // cos(Y) >= 0 (|Y|<=90°)
        f32 alpha, gamma;
        if (cy > 1e-4f) {
            alpha = ATan2(m.m[1][2], m.m[2][2]);     // X = atan2(sx*cy, cx*cy)
            gamma = ATan2(m.m[0][1], m.m[0][0]);     // Z = atan2(cy*sz, cy*cz)
        } else {
            // ジンバルロック (cos Y ≈ 0): Z を 0 に固定し X へ寄せる
            alpha = ATan2(-m.m[2][1], m.m[1][1]);
            gamma = 0.0f;
        }
        return FVec3{ alpha * kRad2Deg, beta * kRad2Deg, gamma * kRad2Deg };
    }

    /**
     * 単位 transform を返す (位置 0、回転 恒等、スケール 1)。
     *
     * @return 単位 transform。
     */
    static FTransform3D Identity() noexcept { return {}; }
};

} // namespace acs::game
