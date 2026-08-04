// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

// ゲームパッドのモーションセンサー (ジャイロ / 加速度) の値。
//
// Joy-Con や DualSense のように IMU を積んだパッドは、スティックやボタンとは別に姿勢の情報を
// 出す。軸 (EGamepadAxis) はスティックとトリガーのための -1..+1 / 0..1 の世界なので、単位が
// 違うモーションはそこへ混ぜず、この型で別に受け取る。
//
// センサーを持たないパッド (Xbox 系や DirectInput 経由の汎用パッド) では valid が false になる。

namespace acs {

/**
 * ゲームパッドのモーションセンサーの値。
 *
 * @details
 * 座標系はパッドを正面に構えた状態を基準に、x が右、y が上、z が手前 (自分側) 方向。
 * ジャイロは各軸まわりの角速度、加速度は重力込みの値で、静止していると下向きに約 1G かかる。
 * 出荷時の個体差までは補正していないので、静止時に僅かな値が残る。厳密に使う場合は
 * 静止状態のジャイロ値を平均して差し引く (ドリフト補正) こと。
 */
struct FGamepadMotion {
    /** 角速度 (度/秒)。 */
    FVec3 gyro { 0.0f, 0.0f, 0.0f };

    /** 加速度 (G。静止時は重力ぶんが乗る)。 */
    FVec3 accel { 0.0f, 0.0f, 0.0f };

    /** センサーを積んだパッドから実際に値を取れているか。 */
    bool valid = false;
};

} // namespace acs
