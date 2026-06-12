// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — 2D 操舵ビヘイビア (Steering2D)
// -----------------------------------------------------------------------------
// エージェントの「目標へ向かう/止まる/逃げる/経路を辿る」操舵力を計算する純関数群。
// 利用側が pos/vel を保持し、戻りの操舵力で速度を更新する (本モジュールは状態を持たない)。
// NavGrid/TilemapNav の waypoint 列と組み合わせて敵 AI の移動を駆動する。
//
// 使い方:
//   FSteerParams sp{ .max_speed = 4.0f, .max_force = 8.0f };
//   FVec2 steer = SteerArrive(pos, vel, target, sp, /*slow_radius=*/2.0f);
//   vel += steer * dt; clamp(|vel|, max_speed); pos += vel * dt;
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

/** 操舵パラメータ (最大速度・最大操舵力)。 */
struct FSteerParams {
    f32 max_speed = 4.0f;   // 最大移動速度
    f32 max_force = 8.0f;   // 1 ステップで加えられる操舵力の上限
};

/** target へ最大速度で向かう操舵力 (Seek)。 */
FVec2 SteerSeek(FVec2 pos, FVec2 vel, FVec2 target, FSteerParams p) noexcept;

/** target から最大速度で離れる操舵力 (Flee)。 */
FVec2 SteerFlee(FVec2 pos, FVec2 vel, FVec2 target, FSteerParams p) noexcept;

/** target に近づくと減速して停止する操舵力 (Arrive)。slow_radius 内で速度を線形に落とす。 */
FVec2 SteerArrive(FVec2 pos, FVec2 vel, FVec2 target, FSteerParams p, f32 slow_radius) noexcept;

/**
 * waypoint 列を順に辿る経路追従。現在の目標 waypoint への Arrive 操舵を返し、
 * arrive_radius 内に入ったら次へ進む。最後に到達したら done=true。
 *
 * @details 状態 (現在 index) は m_Index で保持する non-pure。points は外部所有 (寿命に注意)。
 */
class FPathFollower {
public:
    FPathFollower() noexcept = default;

    /**
     * 追従する waypoint 列を設定する (index リセット)。
     * @param points waypoint 配列 (外部所有、寿命は呼び出し側責務)。
     * @param count 要素数。
     * @param arrive_radius この距離内で waypoint 到達とみなす。
     */
    void SetPath(const FVec2* points, u32 count, f32 arrive_radius = 0.5f) noexcept;

    /**
     * 現在位置に応じた操舵力を返し、到達した waypoint を消化する。
     * @param pos エージェント現在位置。
     * @param vel エージェント現在速度。
     * @param p 操舵パラメータ。
     * @param[out] done 全 waypoint 到達なら true。
     * @return 操舵力。done のときは現在速度を打ち消す停止操舵 (= -vel を max_force でクランプ;
     *         速度ゼロなら結果もゼロ)。
     */
    FVec2 Steer(FVec2 pos, FVec2 vel, FSteerParams p, bool& done) noexcept;

    /** 現在の目標 waypoint index。 */
    u32 Index() const noexcept { return m_Index; }

private:
    const FVec2* m_Points       = nullptr;
    u32          m_Count        = 0u;
    u32          m_Index        = 0u;
    f32          m_ArriveRadius = 0.5f;
};

} // namespace acs::game
