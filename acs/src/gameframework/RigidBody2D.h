// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "gameframework/AComponent.h"
#include "gameframework/RigidWorld2D.h"

namespace acs::game {

class ANode;

/**
 * CRigidWorld2D の剛体を owner ノードに結びつける AComponent。
 *
 * @details
 * ctor で共有ワールドを受け取り、SetCircle/SetBox で owner の現在位置にボディを登録する。
 * PullFromWorld でボディ位置を owner の Local 位置へ反映する (通常は StepRigidBodies が呼ぶ)。
 */
class ARigidBody2D : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(ARigidBody2D)

    /**
     * 共有ワールドを指定して構築する (AddComponent<ARigidBody2D>(world))。
     * @param world このボディが属する剛体ワールド。
     */
    explicit ARigidBody2D(CRigidWorld2D& world) noexcept : m_World(&world) {}

    /**
     * 円ボディとしてワールドへ登録する (owner の現在 Local 位置で)。
     * @param radius 半径。
     * @param mass 質量 (<=0 で静的)。
     * @param restitution 反発係数 [0,1]。
     * @param friction 摩擦係数。
     */
    void SetCircle(f32 radius, f32 mass, f32 restitution = 0.0f, f32 friction = 0.3f) noexcept;

    /**
     * 箱 (AABB) ボディとしてワールドへ登録する。
     * @param half 半サイズ。
     * @param mass 質量 (<=0 で静的)。
     * @param restitution 反発係数 [0,1]。
     * @param friction 摩擦係数。
     */
    void SetBox(FVec2 half, f32 mass, f32 restitution = 0.0f, f32 friction = 0.3f) noexcept;

    /** ワールドのボディ位置を owner の Local 位置へ反映する。 */
    void PullFromWorld() noexcept;

    /** owner ノード破棄時にボディをワールドから除去する (ghost/leak 防止)。 */
    void OnDetach() noexcept override;

    /** ボディが登録済みか。 */
    bool IsRegistered() const noexcept { return m_Registered; }

    /** ワールド内のボディ index (未登録時は不定)。 */
    u32 BodyIndex() const noexcept { return m_BodyIndex; }

    /** ボディの速度。 */
    FVec2 Velocity() const noexcept {
        return (m_Registered && m_World != nullptr) ? m_World->Velocity(m_BodyIndex) : FVec2{0.0f, 0.0f};
    }

    /** ボディの速度を設定する (発射・ジャンプ等)。 */
    void SetVelocity(FVec2 v) noexcept {
        if (m_Registered && m_World != nullptr) m_World->SetVelocity(m_BodyIndex, v);
    }

    /** 連続衝突判定 (CCD) を切り替える。弾丸・高速ボール等が薄い壁をすり抜けるのを防ぐ。 */
    void SetCcd(bool on) noexcept {
        if (m_Registered && m_World != nullptr) m_World->SetCcd(m_BodyIndex, on);
    }

    /** このボディの CCD が有効か。 */
    bool IsCcd() const noexcept {
        return m_Registered && m_World != nullptr && m_World->IsCcd(m_BodyIndex);
    }

private:
    CRigidWorld2D* m_World      = nullptr;
    u32            m_BodyIndex  = 0u;
    bool           m_Registered = false;
};

/**
 * ワールドを 1 ステップ進め、root 配下の全 ARigidBody2D を owner ノードへ同期する。
 *
 * @details world.Step → ツリー走査で各 ARigidBody2D.PullFromWorld()。ゲームループから毎フレーム呼ぶ。
 * @param world 進める剛体ワールド。
 * @param root 同期対象ツリーの根。
 * @param dt 時間刻み (秒)。
 * @param gravity 重力加速度。
 */
void StepRigidBodies(CRigidWorld2D& world, ANode& root, f32 dt, FVec2 gravity) noexcept;

} // namespace acs::game
