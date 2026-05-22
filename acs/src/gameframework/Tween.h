// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — Tween / TweenManager (Phase 3)
//
// 値書き戻し型 Tween: 利用者の f32/Vec2/Vec3 変数のポインタを渡し、
// TweenManager が毎 Tick で補間して書き込む。コールバック不要 (= ACS の
// std::function 非使用方針と整合)。Easing は関数ポインタで指定。
//
// 使い方:
//   class GameplayScene : public Scene {
//       acs::game::TweenManager _tweens;
//       acs::Vec3 _color{0, 0, 0};
//
//       void OnEnter() noexcept override {
//           _tweens.Tween(&_color,
//                          acs::Vec3{0.05f, 0.20f, 0.10f},
//                          acs::Vec3{0.10f, 0.30f, 0.20f},
//                          /*duration=*/2.0f,
//                          Easing::InOutSine);
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           _tweens.Tick(dt);
//           GetGame().SetClearColor(_color.x, _color.y, _color.z);
//       }
//   };
//
// 安全性:
//   ・Handle は (index, generation) で stale 参照を検出。Cancel(h) は
//     完了済 or 別 Tween に再利用された slot を弄らない。
//   ・duration <= 0 を渡すと「即時設定」(target に to を書いて Handle=invalid 返す)。
//   ・target が null なら no-op + invalid handle 返却。
//   ・Tween 完了時は target に正確に `to` を書く (浮動小数誤差を 1 frame 残さない)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "gameframework/Easing.h"

namespace acs::game {

struct TweenHandle {
    u32  index      = 0xFFFFFFFFu;
    u32  generation = 0;
    bool IsValid() const noexcept { return generation != 0; }
};

class TweenManager {
public:
    TweenManager() noexcept = default;
    ~TweenManager() noexcept = default;

    TweenManager(const TweenManager&)            = delete;
    TweenManager& operator=(const TweenManager&) = delete;

    // 各 Tween は target に毎 Tick `from→to` の補間値を書き込む。
    // duration <= 0 は即時 `*target = to` + invalid handle 返却。
    // target が null は no-op + invalid。ease は null なら Linear 扱い。
    TweenHandle Tween(f32* target,  f32  from, f32  to, f32 duration,
                       Easing::EasingFn ease = Easing::Linear) noexcept;
    TweenHandle Tween(Vec2* target, Vec2 from, Vec2 to, f32 duration,
                       Easing::EasingFn ease = Easing::Linear) noexcept;
    TweenHandle Tween(Vec3* target, Vec3 from, Vec3 to, f32 duration,
                       Easing::EasingFn ease = Easing::Linear) noexcept;

    // 進行中の Tween を中止 (target は最後に書いた値で止まる)。stale handle は無視。
    void Cancel(TweenHandle h) noexcept;

    // 全 Tween を即座に終了させる (target は完了値 to が書かれる)。
    // Scene::OnExit 等で確実に状態を確定させたいときに。
    void CompleteAll() noexcept;

    // 全 Tween を即座に破棄 (target に最終書き込みなし)。
    void CancelAll() noexcept;

    bool IsActive(TweenHandle h) const noexcept;
    u32  ActiveCount() const noexcept;

    // 毎フレーム呼ぶ。dt はゲーム時間 (Clock::Dt() か Scene::OnUpdate の dt)。
    void Tick(f32 dt) noexcept;

private:
    enum class Kind : u8 { None = 0, F32, Vec2, Vec3 };

    struct Slot {
        Kind kind        = Kind::None;
        bool active      = false;
        u32  generation  = 0;
        void* target     = nullptr;
        // 型ごとの from/to (アクティブな kind のみが意味を持つ)
        f32  from_f      = 0.0f;
        f32  to_f        = 0.0f;
        Vec2 from_v2     {};
        Vec2 to_v2       {};
        Vec3 from_v3     {};
        Vec3 to_v3       {};
        f32  elapsed     = 0.0f;
        f32  duration    = 1.0f;
        Easing::EasingFn ease = Easing::Linear;
    };

    u32  AcquireSlot() noexcept;
    void FillCommon(Slot& s, void* target, f32 duration,
                    Easing::EasingFn ease) noexcept;

    Array<Slot> _slots;
    u32         _active_count = 0;
};

} // namespace acs::game
