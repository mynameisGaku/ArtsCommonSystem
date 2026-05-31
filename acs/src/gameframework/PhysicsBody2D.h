// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar F Phase 2 — FPhysicsBody2D (Phase 11)
//
// kinematic 物理 body の FComponent2D。velocity + acceleration + gravity を
// 統合し、FCollisionWorld2D に登録された他の shape と衝突したら **軸独立で**
// blocking する (X 軸試行 → overlap なら velocity.x=0 / x 移動キャンセル、
// 同様に Y 軸)。剛体ソルバではなく「2D プラットフォーマー / トップダウン用の
// swept kinematic」(v3 spec §7.2 FPhysicsBody2D)。
//
// 使い方:
//   auto ball = MakeUnique<FNode2D>();
//   ball->Local().position = FVec2{0, 5};
//   auto& body = ball->AddComponent<FPhysicsBody2D>(Services().Physics());
//   body.SetCircle(0.5f);
//   body.gravity = FVec2{0, 10};   // +Y=画面下: 重力は下向き = 正の Y
//   body.velocity = FVec2{1, 0};
//   root.AddChild(Move(ball));
//
// 設計選択 (Phase 11):
//   ・**FComponent2D 派生**: AddComponent<FPhysicsBody2D>(world) で attach。
//     CollisionWorld への参照は constructor で受け取る。
//   ・**axis-separated movement**: X / Y を独立に試して overlap なら止める。
//     真の collide-and-slide は Phase 3。
//   ・**自己除外**: OverlapAabb/Circle の `exclude` 引数で m_Handle を渡し、
//     自分との衝突を無視。
//   ・**OnUpdate で統合**: dt は scene の (scaled) dt。fixed timestep が
//     必要なら scene 側で `OnFixedUpdate` から body.Step(fixed_dt) を呼ぶ
//     形式に拡張可 (Phase 3 候補)。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Collision2D.h"
#include "container/Array.h"
#include "gameframework/Component2D.h"
#include "gameframework/Node2D.h"
#include "gameframework/CollisionWorld2D.h"

namespace acs::game {

class FPhysicsBody2D : public FComponent2D {
public:
    ACS_GAME_COMPONENT_KIND(FPhysicsBody2D)

    // CollisionWorld への参照は constructor で必須受取
    explicit FPhysicsBody2D(FCollisionWorld2D& world) noexcept : m_World(&world) {}

    // ----- 形状設定 (どちらか一方を設定。再設定で上書き) -----
    void SetCircle(f32 radius) noexcept {
        m_Kind = ShapeKind::Circle;
        m_Radius = radius > 0.0f ? radius : 0.001f;
        // 既に登録済なら CollisionWorld 側に反映
        if (HasOwner() && !m_Registered) RegisterShapeAt(Owner().Local().position);
        else SyncShapeIfRegistered();
    }
    void SetAabb(FVec2 half_size) noexcept {
        m_Kind = ShapeKind::FAabb;
        m_HalfSize = half_size;
        if (HasOwner() && !m_Registered) RegisterShapeAt(Owner().Local().position);
        else SyncShapeIfRegistered();
    }
    // local_poly はボディ原点基準のローカル頂点 (例: スプライト凸包を中心原点に
    // ずらしたもの)。world での形状は body 位置 + local 頂点。
    void SetPolygon(const ConvexPoly2& local_poly) noexcept {
        m_Kind = ShapeKind::Poly;
        m_LocalPoly = local_poly;
        if (HasOwner() && !m_Registered) RegisterShapeAt(Owner().Local().position);
        else SyncShapeIfRegistered();
    }
    // OBB ボディ (回転矩形)。回転は中心原点の local poly に焼き込み (body 位置で平行移動)。
    void SetObb(FVec2 half_size, f32 rotation) noexcept {
        SetPolygon(ToPoly(Obb2{FVec2{0.0f, 0.0f}, half_size, rotation}));
    }

    // collide-and-slide を使うか (既定 true)。false で旧来の軸独立 block。
    bool slide = true;

    // ----- 動力学 -----
    FVec2 velocity     {0.0f, 0.0f};
    FVec2 acceleration {0.0f, 0.0f};
    FVec2 gravity      {0.0f, 0.0f};

    // 現在の collision handle (deregister 時に invalidated)
    FShapeId Handle() const noexcept { return m_Handle; }

    // FComponent2D hooks
    void OnAttach(FNode2D& owner) noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnDetach() noexcept override;

private:
    enum class ShapeKind : u8 { None = 0, Circle, FAabb, Poly };

    bool WouldBlockAt(FVec2 pos) noexcept;
    void RegisterShapeAt(FVec2 pos) noexcept;
    void SyncShapeIfRegistered() noexcept;
    ConvexPoly2 WorldPoly(FVec2 pos) const noexcept;   // local poly を pos へ平行移動

    FCollisionWorld2D* m_World  = nullptr;
    ShapeKind         m_Kind   = ShapeKind::None;
    f32               m_Radius = 0.5f;
    FVec2              m_HalfSize{0.5f, 0.5f};
    ConvexPoly2       m_LocalPoly{};
    FShapeId           m_Handle;
    bool              m_Registered = false;
};

} // namespace acs::game
