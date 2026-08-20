// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * Orbit cameraの入力、状態、固定時間から次状態とview座標を決定する値型controller。
 *
 * @details renderer、device、World、platform入力を参照しない。gameplay、AI、replay、testは
 * 同じFOrbitCameraInput3Dを渡せる。pitchは正方向で見下ろし、yawは正方向で右へ回る。
 */
class COrbitCameraController3D final {
public:
    /** 一回の更新へ渡す正規化済み操作量。各値は処理時に[-1, 1]へ制限される。 */
    struct FOrbitCameraInput3D final {
        /** 水平面上の前後移動。正で前進。 */
        f32 move_forward = 0.0f;

        /** 水平面上の左右移動。正で右移動。 */
        f32 move_right = 0.0f;

        /** world Y軸上の上下移動。正で上昇。 */
        f32 move_up = 0.0f;

        /** 水平回転。正で右を向く。 */
        f32 look_yaw = 0.0f;

        /** 垂直回転。正で見下ろす。 */
        f32 look_pitch = 0.0f;

        /** targetとの距離変更。正で近づき、負で遠ざかる。 */
        f32 zoom = 0.0f;
    };

    /** 呼び出し側が所有し、snapshotやreplayへそのまま保存できるcamera状態。 */
    struct FOrbitCameraState3D final {
        /** cameraが見るworld座標。 */
        FVec3 target{0.0f, 0.0f, 0.0f};

        /** world Y軸周りの水平角度。 */
        f32 yaw_radians = 0.0f;

        /** 水平面からの垂直角度。正で見下ろす。 */
        f32 pitch_radians = 0.22f;

        /** targetからeyeまでの距離。 */
        f32 distance = 8.0f;
    };

    /** 固定tickの補間区間を再現するprevious/current状態のprocess内保存値。 */
    struct FOrbitCameraFixedStepSnapshot3D final {
        /** 一つ前の固定tick完了時に確定した状態。 */
        FOrbitCameraState3D previous{};

        /** 現在の固定tick完了時に確定した状態。 */
        FOrbitCameraState3D current{};
    };

    /** controllerの速度と安全範囲。 */
    struct FOrbitCameraSettings3D final {
        /** yaw入力1.0で一秒間に回る角度。 */
        f32 yaw_radians_per_second = 1.45f;

        /** pitch入力1.0で一秒間に回る角度。 */
        f32 pitch_radians_per_second = 1.0875f;

        /** distanceへ掛ける一秒あたりの移動倍率。 */
        f32 movement_distance_scale_per_second = 0.55f;

        /** 近距離でも移動速度を失わないための基準距離。 */
        f32 minimum_movement_distance = 1.0f;

        /** 上下反転を防ぐpitch絶対値上限。 */
        f32 pitch_limit_radians = 1.49225652f;

        /** 複数移動軸の合成長を1以下へ揃えるか。 */
        bool normalize_movement = false;

        /** zoom入力1.0で一秒間に変更する現在距離の倍率。 */
        f32 zoom_distance_scale_per_second = 1.0f;

        /** targetへ近づける最小距離。 */
        f32 minimum_distance = 0.01f;

        /** targetから離れられる最大距離。 */
        f32 maximum_distance = 1000000.0f;
    };

    /** rendererへ渡せるworld座標系のview情報。 */
    struct FOrbitCameraView3D final {
        /** camera位置。 */
        FVec3 eye{};

        /** cameraが見る位置。 */
        FVec3 look_at{};

        /** cameraの上方向。 */
        FVec3 up{0.0f, 1.0f, 0.0f};
    };

    /** 既定設定で構築する。 */
    COrbitCameraController3D() noexcept = default;

    /**
     * 設定を検証し、成功時だけ現在設定へ反映する。
     * @return 全速度が有限かつ非負で、安全範囲が有効ならtrue。
     */
    bool TryConfigure(const FOrbitCameraSettings3D& settings) noexcept;

    /** 現在の検証済み設定を返す。 */
    const FOrbitCameraSettings3D& Settings() const noexcept
    {
        return m_Settings;
    }

    /**
     * 入力と経過秒から次のorbit状態を計算する。
     * @return 入力、時間、状態が有効ならtrue。失敗時はstateを変更しない。
     */
    bool TryStep(const FOrbitCameraInput3D& input, f32 delta_seconds, FOrbitCameraState3D& state) const noexcept;

    /**
     * 前回と現在の固定tick状態を描画補間率で混ぜる。
     * @param previous 前回の固定tick後状態。
     * @param current 現在の固定tick後状態。
     * @param interpolation_alpha previousを0、currentを1とする[0,1]の描画補間率。
     * @param output 補間した描画用状態。失敗時は変更しない。
     * @return 設定、両状態、補間率が有効ならtrue。
     */
    bool TryInterpolateState(const FOrbitCameraState3D& previous, const FOrbitCameraState3D& current, f64 interpolation_alpha, FOrbitCameraState3D& output) const noexcept;

    /**
     * 固定tick snapshotの両状態が現在設定で復元可能か調べる。
     * @return previous/currentが安全なview範囲ならtrue。
     */
    bool IsSnapshotValid(const FOrbitCameraFixedStepSnapshot3D& snapshot) const noexcept;

    /**
     * targetからcameraまでの障害物距離と余白からpresentation用距離を短縮する。
     * @param state 衝突前のdesired orbit状態。
     * @param obstruction_distance targetから最初の障害物までの距離。
     * @param camera_clearance 障害物の手前へ確保する距離。
     * @param output 短縮したpresentation状態。失敗時は変更しない。
     * @return 入力が有限で障害物距離がdesired距離内かつ余白後も最小距離を保てるならtrue。
     */
    bool TryResolveObstructedState(const FOrbitCameraState3D& state, f32 obstruction_distance, f32 camera_clearance, FOrbitCameraState3D& output) const noexcept;

    /**
     * orbit状態からeye、look-at、upを計算する。
     * @return 状態が有効ならtrue。失敗時はviewを変更しない。
     */
    bool TryBuildView(const FOrbitCameraState3D& state, FOrbitCameraView3D& view) const noexcept;

private:
    /** 検証済みの速度と安全範囲。 */
    FOrbitCameraSettings3D m_Settings{};
};

} // namespace acs::game
