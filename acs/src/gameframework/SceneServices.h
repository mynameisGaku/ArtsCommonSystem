// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — FSceneServices (Phase 8、v3 spec §3.1)
//
// シーンが必要なサービス (FSceneClock / FTweenManager / FSequenceRunner / FInputMap)
// を bit flag (`ESvc`) で宣言、FSceneServices が遅延 alloc して保持する取り付けハブ。
// FGame/FSceneManager が自動で tick + scene 切替に追従。
//
// 使い方:
//   class GameplayScene : public Scene {
//   public:
//       ESvc WantedServices() const noexcept override {
//           return ESvc::Default2D;  // Clock | Tweens | Sequences | Input
//       }
//       void OnEnter() noexcept override {
//           Services().Input().BindKey(ActionId("Jump"), EKey::Space);
//           Services().Tweens().Tween(&m_Color, c1, c2, 2.0f, Easing::InOutSine);
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           // dt は Clock 経由 scaled (services 有効時)。Tweens/Sequences は
//           // この OnUpdate の **後** に自動 tick されるので、ここで新規スケジュール
//           // した tween は次フレームから進行する。
//           if (Services().Input().IsPressed(ActionId("Jump"))) DoJump();
//       }
//   };
//
// 設計選択:
//   ・**bit flag 宣言**: `WantedServices()` で宣言したサービスだけ alloc。
//     使わないシーン (例: メニュー) は Physics/Tweens のコストを払わない。
//   ・**遅延 alloc**: constructor 内で wanted bit を見て TUniquePtr<T> を作る。
//     未要求のサービスは TUniquePtr が null、accessor 呼出は assert で検出。
//   ・**2 phase tick**: PreUpdate (Clock 進行) → scene.OnUpdate → PostUpdate
//     (Tweens/Sequences tick)。新規スケジュールは次フレーム頭から進行 (predictable)。
//   ・**自動 pause**: シーンが下位に追いやられた間は OnUpdate が呼ばれず、
//     Clock も tick されないので tween/seq は自然に止まる。明示的 Pause 不要。
//
// 範囲外 (Phase 8 では):
//   ・FCamera2D / Physics2D / Audio / Events / Debug / Timers / Ui の各サービス
//     (該当 Pillar 実装時に ESvc enum と FSceneServices に追加)。
#pragma once

#include "foundation/Types.h"
#include "memory/UniquePtr.h"
#include "gameframework/Clock.h"
#include "gameframework/Tween.h"
#include "gameframework/Sequence.h"
#include "gameframework/InputMap.h"
#include "gameframework/Camera2D.h"
#include "gameframework/CollisionWorld2D.h"
#include "gameframework/TriggerWorld2D.h"

namespace acs::game {

enum class ESvc : u32 {
    None       = 0,
    Clock      = 1u << 0,
    Tweens     = 1u << 1,
    Sequences  = 1u << 2,
    Input      = 1u << 3,
    Camera2D    = 1u << 4,
    Physics2D  = 1u << 5,
    Triggers   = 1u << 6,   // FTriggerWorld2D (overlap enter/stay/exit イベント)
    // Future: Audio, Events, Debug, Timers, Ui
    Default2D  = Clock | Tweens | Sequences | Input,
};

constexpr ESvc operator|(ESvc a, ESvc b) noexcept {
    return static_cast<ESvc>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr ESvc operator&(ESvc a, ESvc b) noexcept {
    return static_cast<ESvc>(static_cast<u32>(a) & static_cast<u32>(b));
}
constexpr bool SvcHas(ESvc mask, ESvc flag) noexcept {
    return (static_cast<u32>(mask) & static_cast<u32>(flag)) != 0u;
}

class FSceneServices {
public:
    // Wanted bit を見て該当サービスを alloc。未要求は null のまま。
    explicit FSceneServices(ESvc wanted) noexcept;
    ~FSceneServices() noexcept = default;

    FSceneServices(const FSceneServices&)            = delete;
    FSceneServices& operator=(const FSceneServices&) = delete;

    ESvc  Wanted() const noexcept { return m_Wanted; }
    bool Has(ESvc s) const noexcept { return SvcHas(m_Wanted, s); }

    // Accessors. 該当サービスが要求されていなければ ACS_ASSERT で停止。
    FSceneClock&          Clock()     noexcept;
    FTweenManager&        Tweens()    noexcept;
    FSequenceRunner&      Sequences() noexcept;
    FInputMap&            Input()     noexcept;
    acs::game::FCamera2D& Camera()    noexcept;
    FCollisionWorld2D&    Physics()   noexcept;
    FTriggerWorld2D&      Triggers()  noexcept;

    // FGame/FSceneManager driver (利用者は触らない)。
    //   PreUpdate: Clock.Tick(raw_dt) で時間を進める (= scaled dt が確定)
    //   PostUpdate: Tweens/Sequences.Tick(scaled_dt) で更新適用
    void _PreUpdate(f32 raw_dt) noexcept;
    void _PostUpdate(f32 scaled_dt) noexcept;

    // PreUpdate 後に scene の OnUpdate に渡す dt。Clock 未要求なら raw_dt をそのまま返す。
    f32  _ScaledDt(f32 raw_dt) const noexcept;

private:
    ESvc                       m_Wanted = ESvc::None;
    TUniquePtr<FSceneClock>     m_Clock;
    TUniquePtr<FTweenManager>   m_Tweens;
    TUniquePtr<FSequenceRunner> m_Sequences;
    TUniquePtr<FInputMap>       m_Input;
    TUniquePtr<acs::game::FCamera2D> m_Camera;
    TUniquePtr<FCollisionWorld2D>    m_Physics;
    TUniquePtr<FTriggerWorld2D>      m_Triggers;
};

} // namespace acs::game
