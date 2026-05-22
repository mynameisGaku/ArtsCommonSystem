// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — SceneServices (Phase 8、v3 spec §3.1)
//
// シーンが必要なサービス (SceneClock / TweenManager / SequenceRunner / InputMap)
// を bit flag (`Svc`) で宣言、SceneServices が遅延 alloc して保持する取り付けハブ。
// Game/SceneManager が自動で tick + scene 切替に追従。
//
// 使い方:
//   class GameplayScene : public Scene {
//   public:
//       Svc WantedServices() const noexcept override {
//           return Svc::Default2D;  // Clock | Tweens | Sequences | Input
//       }
//       void OnEnter() noexcept override {
//           Services().Input().BindKey(ActionId("Jump"), Key::Space);
//           Services().Tweens().Tween(&_color, c1, c2, 2.0f, Easing::InOutSine);
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
//   ・**遅延 alloc**: constructor 内で wanted bit を見て UniquePtr<T> を作る。
//     未要求のサービスは UniquePtr が null、accessor 呼出は assert で検出。
//   ・**2 phase tick**: PreUpdate (Clock 進行) → scene.OnUpdate → PostUpdate
//     (Tweens/Sequences tick)。新規スケジュールは次フレーム頭から進行 (predictable)。
//   ・**自動 pause**: シーンが下位に追いやられた間は OnUpdate が呼ばれず、
//     Clock も tick されないので tween/seq は自然に止まる。明示的 Pause 不要。
//
// 範囲外 (Phase 8 では):
//   ・Camera2D / Physics2D / Audio / Events / Debug / Timers / Ui の各サービス
//     (該当 Pillar 実装時に Svc enum と SceneServices に追加)。
#pragma once

#include "foundation/Types.h"
#include "memory/UniquePtr.h"
#include "gameframework/Clock.h"
#include "gameframework/Tween.h"
#include "gameframework/Sequence.h"
#include "gameframework/InputMap.h"
#include "gameframework/Camera2D.h"
#include "gameframework/CollisionWorld2D.h"

namespace acs::game {

enum class Svc : u32 {
    None       = 0,
    Clock      = 1u << 0,
    Tweens     = 1u << 1,
    Sequences  = 1u << 2,
    Input      = 1u << 3,
    Camera2D   = 1u << 4,
    Physics2D  = 1u << 5,
    // Future: Audio, Events, Debug, Timers, Ui
    Default2D  = Clock | Tweens | Sequences | Input,
};

constexpr Svc operator|(Svc a, Svc b) noexcept {
    return static_cast<Svc>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr Svc operator&(Svc a, Svc b) noexcept {
    return static_cast<Svc>(static_cast<u32>(a) & static_cast<u32>(b));
}
constexpr bool SvcHas(Svc mask, Svc flag) noexcept {
    return (static_cast<u32>(mask) & static_cast<u32>(flag)) != 0u;
}

class SceneServices {
public:
    // Wanted bit を見て該当サービスを alloc。未要求は null のまま。
    explicit SceneServices(Svc wanted) noexcept;
    ~SceneServices() noexcept = default;

    SceneServices(const SceneServices&)            = delete;
    SceneServices& operator=(const SceneServices&) = delete;

    Svc  Wanted() const noexcept { return _wanted; }
    bool Has(Svc s) const noexcept { return SvcHas(_wanted, s); }

    // Accessors. 該当サービスが要求されていなければ ACS_ASSERT で停止。
    SceneClock&          Clock()     noexcept;
    TweenManager&        Tweens()    noexcept;
    SequenceRunner&      Sequences() noexcept;
    InputMap&            Input()     noexcept;
    acs::game::Camera2D& Camera()    noexcept;
    CollisionWorld2D&    Physics()   noexcept;

    // Game/SceneManager driver (利用者は触らない)。
    //   PreUpdate: Clock.Tick(raw_dt) で時間を進める (= scaled dt が確定)
    //   PostUpdate: Tweens/Sequences.Tick(scaled_dt) で更新適用
    void _PreUpdate(f32 raw_dt) noexcept;
    void _PostUpdate(f32 scaled_dt) noexcept;

    // PreUpdate 後に scene の OnUpdate に渡す dt。Clock 未要求なら raw_dt をそのまま返す。
    f32  _ScaledDt(f32 raw_dt) const noexcept;

private:
    Svc                       _wanted = Svc::None;
    UniquePtr<SceneClock>     _clock;
    UniquePtr<TweenManager>   _tweens;
    UniquePtr<SequenceRunner> _sequences;
    UniquePtr<InputMap>       _input;
    UniquePtr<acs::game::Camera2D> _camera;
    UniquePtr<CollisionWorld2D>    _physics;
};

} // namespace acs::game
