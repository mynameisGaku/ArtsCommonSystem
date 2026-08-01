// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework Pillar F — ARigidBody2D (剛体ボディ・コンポーネント)
// -----------------------------------------------------------------------------
// CRigidWorld2D の剛体を ANode に結びつける AComponent。owner ノードに attach
// すると共有ワールドへ円/箱ボディを登録し、物理ステップ後にボディ位置を owner の
// Local 位置へ書き戻す。これで「ノードに付けたら落ちて衝突する」がゲームから書ける。
//
// 使い方:
//   CRigidWorld2D world;
//   world.AddStaticAabb({0, 12}, {20, 0.5f});             // 床
//   auto crate = NewObject<ANode>();
//   crate->SetPosition2D(FVec2{0, 0});
//   auto& rb = crate->AddComponent<ARigidBody2D>(world);   // 共有ワールドを渡す
//   rb.SetBox({0.5f, 0.5f}, /*mass=*/1.0f);
//   root.AddChild(Move(crate));
//   // 毎フレーム:
//   StepRigidBodies(world, root, dt, FVec2{0, 10});        // step + ノード同期
//
// 注意:
//   ・ボディ位置は world 座標。owner の Local へ書き戻すため、物理ノードは identity
//     transform の親 (シーン root 等) 直下に置くこと (親に回転/スケールがあると
//     Local==World が崩れる。world→local 逆変換は今後の拡張点)。
//   ・1 ノードにつき ARigidBody2D は 1 つ (複数付けると同じ Local 位置を奪い合う)。
//   ・SetCircle/SetBox を再呼び出しすると古いボディを置き換える (ghost を残さない)。
//   ・owner ノード破棄時に OnDetach でボディをワールドから除去する (ghost/leak 防止)。
// =============================================================================
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
