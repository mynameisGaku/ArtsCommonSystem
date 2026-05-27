// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — FSequence / FSequenceRunner (Phase 4)
//
// 時間付きアクションの連鎖 (cutscene / 出現ウェーブ / scripted UI 等)。
//   `seq.Wait(0.5f).Call(my_fn, user).Tween(&x, 0, 100, 1.0f).Loop(0)`
// のようなビルダーで定義し、FSequenceRunner::Start(Move(seq)) で実行開始。
//
// 設計選択 (v3 仕様の単純化版):
//   ・パラレル合成 (`Parallel(sub)`) は v1.1 送り。複数 FSequence を Runner に
//     並列で Start すれば事実上のパラレルになるため、Phase 1 では未実装。
//   ・コールバックは `void(*)(void*)` 関数ポインタ。ACS 規約 (std::function 不使用)。
//   ・FTween は FSequence 内蔵 (FTweenManager に委譲しない) — sequence の進行
//     時間と一体化させたいため。完了時は最終値を正確に書く。
//
// 使い方:
//   class TitleScene : public Scene {
//       FSequenceRunner m_Seqs;
//       FVec3 m_LogoColor;
//       void OnEnter() noexcept override {
//           FSequence s;
//           s.Wait(0.3f)
//            .Tween(&m_LogoColor, FVec3{0,0,0}, FVec3{1,1,1}, 0.5f, Easing::OutCubic)
//            .Wait(1.0f)
//            .Call(&TitleScene::FadeOutBegin, this);
//           m_Seqs.Start(Move(s));
//       }
//       void OnUpdate(f32 dt) noexcept override { m_Seqs.Tick(dt); }
//       static void FadeOutBegin(void* self) noexcept {
//           static_cast<TitleScene*>(self)->_ready_to_quit = true;
//       }
//   };
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "gameframework/Easing.h"

namespace acs::game {

// アクション 1 つを表す POD。FSequence 内で TArray に詰めて持つ。
struct SeqAction {
    enum class Kind : u8 { Wait, Call, TweenF, TweenV2, TweenV3 };

    Kind kind     = Kind::Wait;
    f32  duration = 0.0f;          // Wait/FTween 用 (Call は 0)

    // Call
    void(*call_fn)(void* user) noexcept = nullptr;
    void* call_user                     = nullptr;

    // FTween (型ごとに別フィールド、active な kind のみ意味を持つ)
    f32*  tween_f_target  = nullptr;
    f32   tween_f_from    = 0.0f;
    f32   tween_f_to      = 0.0f;
    FVec2* tween_v2_target = nullptr;
    FVec2  tween_v2_from   {};
    FVec2  tween_v2_to     {};
    FVec3* tween_v3_target = nullptr;
    FVec3  tween_v3_from   {};
    FVec3  tween_v3_to     {};

    Easing::EasingFn ease = Easing::Linear;
};

// アクションの連鎖を builder パターンで構築。FSequenceRunner::Start に Move する。
class FSequence {
public:
    FSequence() noexcept = default;
    ~FSequence() noexcept = default;

    FSequence(const FSequence&)            = delete;
    FSequence& operator=(const FSequence&) = delete;
    FSequence(FSequence&&) noexcept            = default;
    FSequence& operator=(FSequence&&) noexcept = default;

    // 各 builder: action を 1 つ追加して self 参照を返す (連鎖記述用)。
    FSequence& Wait(f32 seconds) noexcept;
    FSequence& Call(void(*fn)(void* user) noexcept, void* user = nullptr) noexcept;
    FSequence& FTween(f32* target,  f32  from, f32  to, f32 duration,
                     Easing::EasingFn ease = Easing::Linear) noexcept;
    FSequence& FTween(FVec2* target, FVec2 from, FVec2 to, f32 duration,
                     Easing::EasingFn ease = Easing::Linear) noexcept;
    FSequence& FTween(FVec3* target, FVec3 from, FVec3 to, f32 duration,
                     Easing::EasingFn ease = Easing::Linear) noexcept;

    // ループ回数。0 = 無限。既定 1 (1 回再生で終了)。
    FSequence& Loop(u32 count) noexcept {
        m_LoopCount = count;
        return *this;
    }

    const TArray<SeqAction>& Actions() const noexcept { return m_Actions; }
    u32 LoopCount() const noexcept { return m_LoopCount; }

private:
    TArray<SeqAction> m_Actions;
    u32              m_LoopCount = 1;
};

struct SeqHandle {
    u32  index      = 0xFFFFFFFFu;
    u32  generation = 0;
    bool IsValid() const noexcept { return generation != 0; }
};

class FSequenceRunner {
public:
    FSequenceRunner() noexcept = default;
    ~FSequenceRunner() noexcept = default;

    FSequenceRunner(const FSequenceRunner&)            = delete;
    FSequenceRunner& operator=(const FSequenceRunner&) = delete;

    // FSequence の所有権を奪って実行開始。空 FSequence は invalid handle を返す。
    SeqHandle Start(FSequence seq) noexcept;

    // 進行中の FSequence を中止 (現在の FTween は最後に書いた値で停止)。
    void Cancel(SeqHandle h) noexcept;

    // 全 FSequence を破棄。Scene::OnExit などで使う。
    void CancelAll() noexcept;

    bool IsActive(SeqHandle h) const noexcept;
    u32  ActiveCount() const noexcept { return m_ActiveCount; }

    // 毎フレーム呼ぶ (Scene::OnUpdate から FSceneClock::Dt() を渡す想定)。
    void Tick(f32 dt) noexcept;

private:
    struct Slot {
        bool active         = false;
        bool call_fired     = false;
        u32  generation     = 0;
        FSequence seq;                // owned
        u32  action_idx     = 0;
        f32  action_elapsed = 0.0f;
        u32  loops_done     = 0;
    };

    u32  AcquireSlot() noexcept;
    void AdvanceToNext(Slot& s) noexcept;
    void FinishAction(Slot& s, const SeqAction& act) noexcept;

    TArray<Slot> m_Slots;
    u32         m_ActiveCount = 0;
};

} // namespace acs::game
