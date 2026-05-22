// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar F Phase 2 — PhysicsBody2D (Phase 11)
//
// kinematic 物理 body の Component2D。velocity + acceleration + gravity を
// 統合し、CollisionWorld2D に登録された他の shape と衝突したら **軸独立で**
// blocking する (X 軸試行 → overlap なら velocity.x=0 / x 移動キャンセル、
// 同様に Y 軸)。剛体ソルバではなく「2D プラットフォーマー / トップダウン用の
// swept kinematic」(v3 spec §7.2 PhysicsBody2D)。
//
// 使い方:
//   auto ball = MakeUnique<Node2D>();
//   ball->Local().position = Vec2{0, 5};
//   auto& body = ball->AddComponent<PhysicsBody2D>(Services().Physics());
//   body.SetCircle(0.5f);
//   body.gravity = Vec2{0, -10};
//   body.velocity = Vec2{1, 0};
//   root.AddChild(Move(ball));
//
// 設計選択 (Phase 11):
//   ・**Component2D 派生**: AddComponent<PhysicsBody2D>(world) で attach。
//     CollisionWorld への参照は constructor で受け取る。
//   ・**axis-separated movement**: X / Y を独立に試して overlap なら止める。
//     真の collide-and-slide は Phase 3。
//   ・**自己除外**: OverlapAabb/Circle の `exclude` 引数で _handle を渡し、
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

class PhysicsBody2D : public Component2D {
public:
    ACS_GAME_COMPONENT_KIND(PhysicsBody2D)

    // CollisionWorld への参照は constructor で必須受取
    explicit PhysicsBody2D(CollisionWorld2D& world) noexcept : _world(&world) {}

    // ----- 形状設定 (どちらか一方を設定。再設定で上書き) -----
    void SetCircle(f32 radius) noexcept {
        _kind = ShapeKind::Circle;
        _radius = radius > 0.0f ? radius : 0.001f;
        // 既に登録済なら CollisionWorld 側に反映
        SyncShapeIfRegistered();
    }
    void SetAabb(Vec2 half_size) noexcept {
        _kind = ShapeKind::Aabb;
        _half_size = half_size;
        SyncShapeIfRegistered();
    }

    // ----- 動力学 -----
    Vec2 velocity     {0.0f, 0.0f};
    Vec2 acceleration {0.0f, 0.0f};
    Vec2 gravity      {0.0f, 0.0f};

    // 現在の collision handle (deregister 時に invalidated)
    ShapeId Handle() const noexcept { return _handle; }

    // Component2D hooks
    void OnAttach(Node2D& owner) noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnDetach() noexcept override;

private:
    enum class ShapeKind : u8 { None = 0, Circle, Aabb };

    bool WouldBlockAt(Vec2 pos) noexcept;
    void RegisterShapeAt(Vec2 pos) noexcept;
    void SyncShapeIfRegistered() noexcept;

    CollisionWorld2D* _world  = nullptr;
    ShapeKind         _kind   = ShapeKind::None;
    f32               _radius = 0.5f;
    Vec2              _half_size{0.5f, 0.5f};
    ShapeId           _handle;
    bool              _registered = false;
};

} // namespace acs::game
