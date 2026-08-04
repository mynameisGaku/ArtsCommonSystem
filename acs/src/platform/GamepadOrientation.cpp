// SPDX-License-Identifier: Apache-2.0
// ゲームパッドの姿勢推定 (ジャイロ積分 + 加速度による傾き補正)

#include "platform/GamepadOrientation.h"

#include <cmath>

namespace acs {

namespace {

/** 度をラジアンへ直す係数。 */
constexpr f32 kDegToRad = 3.14159265358979323846f / 180.0f;

/** これ以下の角速度は雑音とみなして積分しない (度/秒)。 */
constexpr f32 kGyroNoiseFloor = 0.5f;

/** 重力だけが掛かっているとみなす加速度の大きさの許容幅 (G)。 */
constexpr f32 kGravityTolerance = 0.15f;

/** 1 秒あたりに引き戻す傾きの割合 (0..1)。大きいほど速く直るが、加速度の揺れを拾いやすい。 */
constexpr f32 kTiltCorrectionRate = 0.4f;

/**
 * ベクトルの長さを返す。
 *
 * @param v 対象のベクトル。
 * @return 長さ。
 */
f32 LengthOf(FVec3 v) noexcept {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

/**
 * ベクトルの外積を返す。
 *
 * @param a 左のベクトル。
 * @param b 右のベクトル。
 * @return 外積。
 */
FVec3 CrossOf(FVec3 a, FVec3 b) noexcept {
    return FVec3{ a.y * b.z - a.z * b.y,
                  a.z * b.x - a.x * b.z,
                  a.x * b.y - a.y * b.x };
}

/**
 * ベクトルの内積を返す。
 *
 * @param a 左のベクトル。
 * @param b 右のベクトル。
 * @return 内積。
 */
f32 DotOf(FVec3 a, FVec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

} // 無名名前空間


void CGamepadOrientation::Integrate(const FGamepadMotion& motion, f32 delta_seconds) noexcept {
    if (!motion.valid || delta_seconds <= 0.0f) return;

    // --- ジャイロを積分して回転を進める ---
    /** 角速度の大きさ (度/秒)。 */
    const f32 speed_deg = LengthOf(motion.gyro);
    if (speed_deg > kGyroNoiseFloor) {
        /** このフレームで回る角度 (ラジアン)。 */
        const f32 angle = speed_deg * kDegToRad * delta_seconds;
        /** 回転軸 (角速度ベクトルの向き)。 */
        const FVec3 axis { motion.gyro.x / speed_deg, motion.gyro.y / speed_deg, motion.gyro.z / speed_deg };

        // パッドから見た回転なので、現在の姿勢の後ろから掛ける。
        m_Rotation = Normalize(m_Rotation * FQuat::AxisAngle(axis, angle));
    }

    // --- 加速度で傾きを引き戻す (静止しているときだけ) ---
    /** 加速度の大きさ (G)。 */
    const f32 accel_magnitude = LengthOf(motion.accel);
    if (accel_magnitude < acs::kEpsilon) return;
    if (accel_magnitude < 1.0f - kGravityTolerance || accel_magnitude > 1.0f + kGravityTolerance) return;

    // 重力が指す向き (パッドから見た下方向)。
    const FVec3 measured_down { -motion.accel.x / accel_magnitude,
                                -motion.accel.y / accel_magnitude,
                                -motion.accel.z / accel_magnitude };

    // いまの姿勢が示す下方向を、パッドの座標系へ引き戻したもの。
    const FVec3 expected_down = Rotate(Inverse(m_Rotation), FVec3{ 0.0f, -1.0f, 0.0f });

    // 2 つの下方向のズレを、少しずつ詰める。
    /** ズレを回す軸 (長さがズレの大きさ)。 */
    const FVec3 axis = CrossOf(expected_down, measured_down);
    const f32 axis_length = LengthOf(axis);
    if (axis_length < acs::kEpsilon) return;

    /** ズレの角度 (ラジアン)。 */
    f32 error = std::atan2(axis_length, DotOf(expected_down, measured_down));

    /** このフレームで詰める角度。 */
    f32 correction = error * kTiltCorrectionRate * delta_seconds;
    if (correction > error) correction = error;
    if (correction < acs::kEpsilon) return;

    const FVec3 normalized_axis { axis.x / axis_length, axis.y / axis_length, axis.z / axis_length };
    m_Rotation = Normalize(m_Rotation * FQuat::AxisAngle(normalized_axis, correction));
}

} // namespace acs
