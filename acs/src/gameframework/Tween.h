// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — FTween / FTweenManager (Phase 3)
//
// 値書き戻し型 FTween: 利用者の f32/FVec2/FVec3 変数のポインタを渡し、
// FTweenManager が毎 Tick で補間して書き込む。コールバック不要 (= ACS の
// std::function 非使用方針と整合)。Easing は関数ポインタで指定。
//
// 使い方:
//   class GameplayScene : public Scene {
//       acs::game::FTweenManager m_Tweens;
//       acs::FVec3 m_Color{0, 0, 0};
//
//       void OnEnter() noexcept override {
//           m_Tweens.Tween(&m_Color,
//                          acs::FVec3{0.05f, 0.20f, 0.10f},
//                          acs::FVec3{0.10f, 0.30f, 0.20f},
//                          /*duration=*/2.0f,
//                          Easing::InOutSine);
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           m_Tweens.Tick(dt);
//           GetGame().SetClearColor(m_Color.x, m_Color.y, m_Color.z);
//       }
//   };
//
// 安全性:
//   ・Handle は (index, generation) で stale 参照を検出。Cancel(h) は
//     完了済 or 別 FTween に再利用された slot を弄らない。
//   ・duration <= 0 を渡すと「即時設定」(target に to を書いて Handle=invalid 返す)。
//   ・target が null なら no-op + invalid handle 返却。
//   ・FTween 完了時は target に正確に `to` を書く (浮動小数誤差を 1 frame 残さない)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "gameframework/Easing.h"

namespace acs::game {

struct FTweenHandle {
    u32  index      = 0xFFFFFFFFu;
    u32  generation = 0;
    bool IsValid() const noexcept { return generation != 0; }
};

class FTweenManager {
public:
    FTweenManager() noexcept = default;
    ~FTweenManager() noexcept = default;

    FTweenManager(const FTweenManager&)            = delete;
    FTweenManager& operator=(const FTweenManager&) = delete;

    // 各 FTween は target に毎 Tick `from→to` の補間値を書き込む。
    // duration <= 0 は即時 `*target = to` + invalid handle 返却。
    // target が null は no-op + invalid。ease は null なら Linear 扱い。
    FTweenHandle Tween(f32* target,  f32  from, f32  to, f32 duration,
                       Easing::EasingFn ease = Easing::Linear) noexcept;
    FTweenHandle Tween(FVec2* target, FVec2 from, FVec2 to, f32 duration,
                       Easing::EasingFn ease = Easing::Linear) noexcept;
    FTweenHandle Tween(FVec3* target, FVec3 from, FVec3 to, f32 duration,
                       Easing::EasingFn ease = Easing::Linear) noexcept;

    // 進行中の FTween を中止 (target は最後に書いた値で止まる)。stale handle は無視。
    void Cancel(FTweenHandle h) noexcept;

    // 全 FTween を即座に終了させる (target は完了値 to が書かれる)。
    // Scene::OnExit 等で確実に状態を確定させたいときに。
    void CompleteAll() noexcept;

    // 全 FTween を即座に破棄 (target に最終書き込みなし)。
    void CancelAll() noexcept;

    bool IsActive(FTweenHandle h) const noexcept;
    u32  ActiveCount() const noexcept;

    // 毎フレーム呼ぶ。dt はゲーム時間 (Clock::Dt() か Scene::OnUpdate の dt)。
    void Tick(f32 dt) noexcept;

private:
    enum class Kind : u8 { None = 0, F32, FVec2, FVec3 };

    struct Slot {
        Kind kind        = Kind::None;
        bool active      = false;
        u32  generation  = 0;
        void* target     = nullptr;
        // 型ごとの from/to (アクティブな kind のみが意味を持つ)
        f32  from_f      = 0.0f;
        f32  to_f        = 0.0f;
        FVec2 from_v2     {};
        FVec2 to_v2       {};
        FVec3 from_v3     {};
        FVec3 to_v3       {};
        f32  elapsed     = 0.0f;
        f32  duration    = 1.0f;
        Easing::EasingFn ease = Easing::Linear;
    };

    u32  AcquireSlot() noexcept;
    void FillCommon(Slot& s, void* target, f32 duration,
                    Easing::EasingFn ease) noexcept;

    TArray<Slot> m_Slots;
    u32         m_ActiveCount = 0;
};

} // namespace acs::game
