// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "math/Mat.h"
#include "math/Quat.h"
#include "platform/GamepadMotion.h"

// ジャイロと加速度から、パッドの姿勢 (向き) を積み上げる。
//
// ジャイロが返すのは角速度なので、そのままでは「いまどちらを向いているか」は分からない。
// 毎フレーム積分して回転として持ち、クォータニオンでも回転行列でも取り出せるようにする。
//
// 積分だけだと誤差が溜まって少しずつ傾いていく (ドリフト) ため、パッドがほぼ静止している
// ときだけ加速度から重力の向きを見て、傾きをゆっくり引き戻す。振り回している間は加速度に
// 重力以外の成分が混ざるので補正しない。
//
// 向きの基準は「最後に Reset した時点の姿勢」。絶対方位 (北がどちらか) は分からないので、
// 方位を合わせたい場合は任意のタイミングで Reset を呼んで基準を取り直す。

namespace acs {

/**
 * ゲームパッドの姿勢。
 *
 * @details 1 台につき 1 つ持ち、毎フレーム Integrate を呼んで進める。
 */
class CGamepadOrientation {
public:
    /** 無回転の状態で構築する。 */
    CGamepadOrientation() noexcept = default;

    /**
     * モーションの値で姿勢を 1 フレーム進める。
     *
     * @details valid でないモーションを渡した場合は何もしない (姿勢は保たれる)。
     * @param motion このフレームのモーション。
     * @param delta_seconds 前フレームからの経過秒。
     */
    void Integrate(const FGamepadMotion& motion, f32 delta_seconds) noexcept;

    /**
     * 姿勢をクォータニオンで返す。
     *
     * @return 単位クォータニオン。
     */
    const FQuat& Rotation() const noexcept { return m_Rotation; }

    /**
     * 姿勢を回転行列で返す。
     *
     * @details 呼ぶたびにクォータニオンから作るので、繰り返し使う場合は控えておくこと。
     * @return 回転行列 (平行移動なし)。
     */
    FMat4 RotationMatrix() const noexcept { return ToMatrix(m_Rotation); }

    /** 現在の向きを基準 (無回転) に取り直す。 */
    void Reset() noexcept { m_Rotation = FQuat::Identity(); }

private:
    /** 積み上げた姿勢。 */
    FQuat m_Rotation = FQuat::Identity();
};

} // namespace acs
