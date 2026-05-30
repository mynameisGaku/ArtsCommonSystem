// SPDX-License-Identifier: Apache-2.0
// FTriggerComponent — FNode2D を FTriggerWorld2D に橋渡しし、overlap の
// enter / exit を「コンポーネント単位」で受け取れるようにする Component2D。
//
// FTriggerWorld2D 自体は world 全体で 1 組のコールバックしか持たないため、本
// コンポーネントは各 trigger に自分自身 (this) を user data として紐付け、world の
// global コールバックを静的ディスパッチャに差し替えて id→component を逆引きする。
// これにより player / pickup / hazard などを各ノードのハンドラで個別に処理できる。
//
// 使い方 (サブクラスで override):
//   class FPickup : public FTriggerComponent {
//   public:
//       ACS_GAME_COMPONENT_KIND(FPickup)
//       using FTriggerComponent::FTriggerComponent;
//       void OnTriggerEnter(FTriggerComponent* other) noexcept override { /* 取得処理 */ }
//   };
//   auto& pk = node->AddComponent<FPickup>(Services().Triggers(),
//                                          /*layer=*/kPickupLayer, /*mask=*/0);
//   pk.SetCircle(0.4f);
//
// 使い方 (関数ポインタ):
//   auto& tg = node->AddComponent<FTriggerComponent>(world, kPlayerLayer, kPickupLayer);
//   tg.SetCircle(0.45f);
//   tg.SetOnEnter(+[](FTriggerComponent& self, FTriggerComponent* other, void* user) noexcept {
//       // self が mask に合致する other に触れた
//   }, this);
//
// layer / mask 規約 (CollisionWorld2D と同じ bitmask):
//   ・layer = 自分が属するレイヤ。mask = 自分が反応したいレイヤ。
//   ・this は「other.layer & this.mask != 0」のときだけ自分のハンドラが発火する。
//     (幾何 overlap 判定自体は FTriggerWorld2D が全 pair で行い、フィルタは本層で適用)
#pragma once

#include "gameframework/Component2D.h"
#include "gameframework/TriggerWorld2D.h"
#include "math/Vec.h"

namespace acs::game {

class FTriggerComponent : public FComponent2D {
public:
    ACS_GAME_COMPONENT_KIND(FTriggerComponent)

    static constexpr u32 kAllLayers = 0xFFFFFFFFu;

    // world: 所属するトリガワールド (通常 Services().Triggers())。
    // layer: 自分のレイヤ bit。mask: 反応したい相手のレイヤ bitmask。
    explicit FTriggerComponent(FTriggerWorld2D& world,
                               u32 layer = kAllLayers,
                               u32 mask  = kAllLayers) noexcept
        : m_World(&world), m_Layer(layer), m_Mask(mask) {}

    // 形状設定 (OnAttach までに呼ぶ。後から変えても次の OnUpdate で反映)。
    void SetCircle(f32 radius) noexcept { m_Kind = EKind::Circle; m_Radius = radius; }
    void SetAabb(FVec2 half_size) noexcept { m_Kind = EKind::Aabb; m_Half = half_size; }

    u32 Layer() const noexcept { return m_Layer; }
    u32 Mask()  const noexcept { return m_Mask; }
    void SetLayer(u32 layer) noexcept { m_Layer = layer; }
    void SetMask(u32 mask) noexcept { m_Mask = mask; }

    // コールバック (関数ポインタ + user)。サブクラス override と併用可。
    using TriggerFn = void(*)(FTriggerComponent& self, FTriggerComponent* other, void* user) noexcept;
    void SetOnEnter(TriggerFn fn, void* user) noexcept { m_OnEnter = fn; m_OnEnterUser = user; }
    void SetOnExit (TriggerFn fn, void* user) noexcept { m_OnExit  = fn; m_OnExitUser  = user; }

    // サブクラス用フック (override only what you need)。
    virtual void OnTriggerEnter(FTriggerComponent* /*other*/) noexcept {}
    virtual void OnTriggerExit (FTriggerComponent* /*other*/) noexcept {}

    void OnAttach(FNode2D& owner) noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnDetach() noexcept override;

private:
    enum class EKind : u8 { Circle, Aabb };

    void Register() noexcept;   // 初回 OnUpdate で遅延登録 (SetCircle/SetAabb 後を尊重)
    void SyncShape() noexcept;
    void HandleEnter(FTriggerComponent* other) noexcept;
    void HandleExit (FTriggerComponent* other) noexcept;

    // world の global コールバックに差し込む静的ディスパッチャ (user = &world)。
    static void SDispatchEnter(void* user, FTriggerId self, FTriggerId other) noexcept;
    static void SDispatchExit (void* user, FTriggerId self, FTriggerId other) noexcept;

    FTriggerWorld2D* m_World = nullptr;
    FTriggerId       m_Id;
    EKind            m_Kind   = EKind::Circle;
    f32              m_Radius = 0.5f;
    FVec2            m_Half{0.5f, 0.5f};
    u32              m_Layer  = kAllLayers;
    u32              m_Mask   = kAllLayers;
    bool             m_Registered = false;

    TriggerFn m_OnEnter     = nullptr;
    TriggerFn m_OnExit      = nullptr;
    void*     m_OnEnterUser = nullptr;
    void*     m_OnExitUser  = nullptr;
};

} // namespace acs::game
