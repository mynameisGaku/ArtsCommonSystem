// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar F — APhysicsBody2D
//
// kinematic 物理 body の AComponent。velocity + acceleration + gravity を
// 統合し、CCollisionWorld2D に登録された他の shape と衝突したら **軸独立で**
// blocking する (X 軸試行 → overlap なら velocity.x=0 / x 移動キャンセル、
// 同様に Y 軸)。剛体ソルバではなく「2D プラットフォーマー / トップダウン用の
// swept kinematic」。
//
// 使い方:
//   auto ball = NewObject<ANode>();
//   ball->SetPosition2D(FVec2{0, 5});
//   auto& body = ball->AddComponent<APhysicsBody2D>(Services().Physics());
//   body.SetCircle(0.5f);
//   body.gravity = FVec2{0, 10};   // +Y=画面下: 重力は下向き = 正の Y
//   body.velocity = FVec2{1, 0};
//   root.AddChild(Move(ball));
//
// 設計選択:
//   ・**AComponent 派生**: AddComponent<APhysicsBody2D>(world) で attach。
//     CollisionWorld への参照は constructor で受け取る。
//   ・**axis-separated movement**: X / Y を独立に試して overlap なら止める。
//   ・**自己除外**: OverlapAabb/Circle の `exclude` 引数で m_Handle を渡し、
//     自分との衝突を無視。
//   ・**OnUpdate で統合**: dt は scene の (scaled) dt。fixed timestep が
//     必要なら scene 側で `OnFixedUpdate` から body.Step(fixed_dt) を呼ぶ
//     形式に拡張可。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Collision2D.h"
#include "container/Array.h"
#include "gameframework/AComponent.h"
#include "gameframework/ANode.h"
#include "gameframework/CollisionWorld2D.h"

namespace acs::game {

/**
 * kinematic 物理 body を表す AComponent。
 *
 * @details
 * velocity + acceleration + gravity を毎フレーム統合し、CCollisionWorld2D に登録された
 * 他 shape と衝突したら停止する。既定の collide-and-slide では望む変位だけ動かしてから
 * 貫通を MTV で押し出し、押し出し法線方向の速度成分を消して面に沿って滑る。slide=false の
 * ときは X / Y を独立に試して overlap なら軸ごとに止める旧来の軸独立 block。剛体ソルバでは
 * なく 2D プラットフォーマー / トップダウン用の swept kinematic。
 */
class APhysicsBody2D : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(APhysicsBody2D)

    /**
     * CollisionWorld への参照を受け取って構築する。
     *
     * @param world shape 登録・overlap クエリに使う衝突ワールド (参照を保持)。
     */
    explicit APhysicsBody2D(CCollisionWorld2D& world) noexcept : m_World(&world) {}

    /**
     * 形状を円に設定する (再設定で上書き)。
     *
     * @details 0 以下の半径は 0.001 に丸める。未登録かつ owner があれば即登録、
     * 登録済みなら CollisionWorld 側へ反映する。
     * @param radius 円の半径。
     */
    void SetCircle(f32 radius) noexcept {
        m_Kind = EShapeKind::Circle;
        m_Radius = radius > 0.0f ? radius : 0.001f;
        // 既に登録済なら CollisionWorld 側に反映
        if (HasOwner() && !m_Registered) RegisterShapeAt(Owner().Position2D());
        else SyncShapeIfRegistered();
    }

    /**
     * 形状を軸並行矩形 (AABB) に設定する (再設定で上書き)。
     *
     * @details 未登録かつ owner があれば即登録、登録済みなら CollisionWorld 側へ反映する。
     * @param half_size 矩形の半幅・半高。
     */
    void SetAabb(FVec2 half_size) noexcept {
        m_Kind = EShapeKind::Aabb;
        m_HalfSize = half_size;
        if (HasOwner() && !m_Registered) RegisterShapeAt(Owner().Position2D());
        else SyncShapeIfRegistered();
    }

    /**
     * 形状を凸多角形に設定する (再設定で上書き)。
     *
     * @details local_poly はボディ原点基準のローカル頂点で、world 形状は body 位置 +
     * local 頂点。未登録かつ owner があれば即登録、登録済みなら CollisionWorld 側へ反映する。
     * @param local_poly ボディ原点基準のローカル凸多角形。
     */
    void SetPolygon(const FConvexPoly2& local_poly) noexcept {
        m_Kind = EShapeKind::Poly;
        m_LocalPoly = local_poly;
        if (HasOwner() && !m_Registered) RegisterShapeAt(Owner().Position2D());
        else SyncShapeIfRegistered();
    }

    /**
     * 形状を OBB (回転矩形) に設定する。
     *
     * @details 回転を中心原点の local poly に焼き込んで SetPolygon に渡す (body 位置で平行移動)。
     * @param half_size 矩形の半幅・半高。
     * @param rotation 回転角 (ラジアン)。
     */
    void SetObb(FVec2 half_size, f32 rotation) noexcept {
        SetPolygon(ToPoly(FObb2{FVec2{0.0f, 0.0f}, half_size, rotation}));
    }

    /** collide-and-slide を使うか (既定 true)。false で旧来の軸独立 block。 */
    bool slide = true;

    /** 現在の速度 (world unit / sec)。 */
    FVec2 velocity     {0.0f, 0.0f};

    /** 一定加速度 (world unit / sec^2)。毎フレーム velocity に積分される。 */
    FVec2 acceleration {0.0f, 0.0f};

    /** 重力加速度 (world unit / sec^2)。+Y=画面下なので下向きは正の Y。 */
    FVec2 gravity      {0.0f, 0.0f};

    /**
     * 現在の collision handle を返す。
     *
     * @return CollisionWorld 上の FShapeId (未登録 / deregister 後は invalid)。
     */
    FShapeId Handle() const noexcept { return m_Handle; }

    /**
     * owner に attach された時に shape を CollisionWorld に登録する。
     *
     * @param owner この body を所有するノード。
     */
    void OnAttach(ANode& owner) noexcept override;

    /**
     * 毎フレーム速度を統合して owner の位置を更新する。
     *
     * @param dt 経過秒 (0 以下なら何もしない)。
     */
    void OnUpdate(f32 dt) noexcept override;

    /** detach 時に shape を CollisionWorld から除去し handle を無効化する。 */
    void OnDetach() noexcept override;

private:
    /** body の形状種別。 */
    enum class EShapeKind : u8 { None = 0, Circle, Aabb, Poly };

    /**
     * 指定位置に shape を置いたとき他 shape と overlap するかを返す。
     *
     * @param pos 試行位置。
     * @return 自分以外の shape と overlap するなら true。
     */
    bool WouldBlockAt(FVec2 pos) noexcept;

    /**
     * 指定位置に現在の形状を CollisionWorld へ登録する (未登録時のみ)。
     *
     * @param pos 登録位置。
     */
    void RegisterShapeAt(FVec2 pos) noexcept;

    /** 登録済みなら owner の現在位置で CollisionWorld の shape を更新する。 */
    void SyncShapeIfRegistered() noexcept;

    /**
     * ローカル多角形を指定位置へ平行移動した world 多角形を返す。
     *
     * @param pos 平行移動先の body 位置。
     * @return world 座標の凸多角形。
     */
    FConvexPoly2 WorldPoly(FVec2 pos) const noexcept;

    /** shape 登録・overlap クエリに使う衝突ワールド (非所有)。 */
    CCollisionWorld2D* m_World  = nullptr;

    /** 現在の形状種別。 */
    EShapeKind         m_Kind   = EShapeKind::None;

    /** 円形状の半径。 */
    f32               m_Radius = 0.5f;

    /** AABB 形状の半幅・半高。 */
    FVec2              m_HalfSize{0.5f, 0.5f};

    /** 多角形形状のローカル頂点 (body 原点基準)。 */
    FConvexPoly2       m_LocalPoly{};

    /** CollisionWorld 上の shape handle。 */
    FShapeId           m_Handle;

    /** CollisionWorld に登録済みかのフラグ。 */
    bool              m_Registered = false;
};

} // namespace acs::game
