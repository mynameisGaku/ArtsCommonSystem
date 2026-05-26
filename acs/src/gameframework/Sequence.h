// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — Sequence / SequenceRunner (Phase 4)
//
// 時間付きアクションの連鎖 (cutscene / 出現ウェーブ / scripted UI 等)。
//   `seq.Wait(0.5f).Call(my_fn, user).Tween(&x, 0, 100, 1.0f).Loop(0)`
// のようなビルダーで定義し、SequenceRunner::Start(Move(seq)) で実行開始。
//
// 設計選択 (v3 仕様の単純化版):
//   ・パラレル合成 (`Parallel(sub)`) は v1.1 送り。複数 Sequence を Runner に
//     並列で Start すれば事実上のパラレルになるため、Phase 1 では未実装。
//   ・コールバックは `void(*)(void*)` 関数ポインタ。ACS 規約 (std::function 不使用)。
//   ・Tween は Sequence 内蔵 (TweenManager に委譲しない) — sequence の進行
//     時間と一体化させたいため。完了時は最終値を正確に書く。
//
// 使い方:
//   class TitleScene : public Scene {
//       SequenceRunner _seqs;
//       FVec3 _logo_color;
//       void OnEnter() noexcept override {
//           Sequence s;
//           s.Wait(0.3f)
//            .Tween(&_logo_color, FVec3{0,0,0}, FVec3{1,1,1}, 0.5f, Easing::OutCubic)
//            .Wait(1.0f)
//            .Call(&TitleScene::FadeOutBegin, this);
//           _seqs.Start(Move(s));
//       }
//       void OnUpdate(f32 dt) noexcept override { _seqs.Tick(dt); }
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

// アクション 1 つを表す POD。Sequence 内で TArray に詰めて持つ。
struct SeqAction {
    enum class Kind : u8 { Wait, Call, TweenF, TweenV2, TweenV3 };

    Kind kind     = Kind::Wait;
    f32  duration = 0.0f;          // Wait/Tween 用 (Call は 0)

    // Call
    void(*call_fn)(void* user) noexcept = nullptr;
    void* call_user                     = nullptr;

    // Tween (型ごとに別フィールド、active な kind のみ意味を持つ)
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

// アクションの連鎖を builder パターンで構築。SequenceRunner::Start に Move する。
class Sequence {
public:
    Sequence() noexcept = default;
    ~Sequence() noexcept = default;

    Sequence(const Sequence&)            = delete;
    Sequence& operator=(const Sequence&) = delete;
    Sequence(Sequence&&) noexcept            = default;
    Sequence& operator=(Sequence&&) noexcept = default;

    // 各 builder: action を 1 つ追加して self 参照を返す (連鎖記述用)。
    Sequence& Wait(f32 seconds) noexcept;
    Sequence& Call(void(*fn)(void* user) noexcept, void* user = nullptr) noexcept;
    Sequence& Tween(f32* target,  f32  from, f32  to, f32 duration,
                     Easing::EasingFn ease = Easing::Linear) noexcept;
    Sequence& Tween(FVec2* target, FVec2 from, FVec2 to, f32 duration,
                     Easing::EasingFn ease = Easing::Linear) noexcept;
    Sequence& Tween(FVec3* target, FVec3 from, FVec3 to, f32 duration,
                     Easing::EasingFn ease = Easing::Linear) noexcept;

    // ループ回数。0 = 無限。既定 1 (1 回再生で終了)。
    Sequence& Loop(u32 count) noexcept {
        _loop_count = count;
        return *this;
    }

    const TArray<SeqAction>& Actions() const noexcept { return _actions; }
    u32 LoopCount() const noexcept { return _loop_count; }

private:
    TArray<SeqAction> _actions;
    u32              _loop_count = 1;
};

struct SeqHandle {
    u32  index      = 0xFFFFFFFFu;
    u32  generation = 0;
    bool IsValid() const noexcept { return generation != 0; }
};

class SequenceRunner {
public:
    SequenceRunner() noexcept = default;
    ~SequenceRunner() noexcept = default;

    SequenceRunner(const SequenceRunner&)            = delete;
    SequenceRunner& operator=(const SequenceRunner&) = delete;

    // Sequence の所有権を奪って実行開始。空 Sequence は invalid handle を返す。
    SeqHandle Start(Sequence seq) noexcept;

    // 進行中の Sequence を中止 (現在の Tween は最後に書いた値で停止)。
    void Cancel(SeqHandle h) noexcept;

    // 全 Sequence を破棄。Scene::OnExit などで使う。
    void CancelAll() noexcept;

    bool IsActive(SeqHandle h) const noexcept;
    u32  ActiveCount() const noexcept { return _active_count; }

    // 毎フレーム呼ぶ (Scene::OnUpdate から SceneClock::Dt() を渡す想定)。
    void Tick(f32 dt) noexcept;

private:
    struct Slot {
        bool active         = false;
        bool call_fired     = false;
        u32  generation     = 0;
        Sequence seq;                // owned
        u32  action_idx     = 0;
        f32  action_elapsed = 0.0f;
        u32  loops_done     = 0;
    };

    u32  AcquireSlot() noexcept;
    void AdvanceToNext(Slot& s) noexcept;
    void FinishAction(Slot& s, const SeqAction& act) noexcept;

    TArray<Slot> _slots;
    u32         _active_count = 0;
};

} // namespace acs::game
