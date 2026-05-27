// SPDX-License-Identifier: Apache-2.0
// GameFramework GenreKit — FBeatGrid (rhythm game タイミング判定 + チャート再生)
//
// 1 譜面分の note 配列を保持し、再生時刻 (current_time) を進行させながら
// プレイヤー入力 (Tap) を最近 note と突き合わせて Perfect / Great / Good / Miss
// の 4 段階で判定する。FMusicDirector / FAudioDirector とは独立に動く軽量
// state machine で、譜面と入力の対応関係だけを管理する。
//
// 使い方:
//   class RhythmScene : public Scene {
//       acs::game::FBeatGrid m_Grid;
//
//       void OnEnter() noexcept override {
//           m_Grid.Init();
//           m_Grid.SetTimingWindows(25.0f, 50.0f, 100.0f);
//           m_Grid.SetOnJudgeCallback(&RhythmScene::OnJudge, this);
//           m_Grid.SetOnEndCallback (&RhythmScene::OnEnd,   this);
//
//           acs::game::FBeatNote notes[] = {
//               { 1.000f, acs::game::EBeatLane::Left,  false, 0.0f },
//               { 1.500f, acs::game::EBeatLane::Down,  false, 0.0f },
//               { 2.000f, acs::game::EBeatLane::Up,    true,  0.5f },
//           };
//           m_Grid.LoadChart(notes, 3, 120.0f);
//           m_Grid.Start();
//       }
//       void OnUpdate(f32 dt) noexcept override { m_Grid.Tick(dt); }
//       void OnInput() noexcept {
//           if (key_left_pressed)
//               (void)m_Grid.Tap(acs::game::EBeatLane::Left);
//       }
//
//       static void OnJudge(void* self, acs::game::EBeatLane lane,
//                           acs::game::EJudgement j, u32 combo) noexcept { ... }
//       static void OnEnd  (void* self, u32 hits, u32 misses, f32 acc) noexcept { ... }
//   };
//
// 設計:
//   ・**判定モデル**: 入力 (Tap) があった瞬間 current_time と各 note.time_sec の
//     差分絶対値で最近 note を線形探索 (note 数は通常数百〜数千、譜面長 = 数分
//     なので O(N) で十分。frame ごとに走るのは Tick の miss 検出だけ)。
//     |delta| <= perfect_ms → Perfect
//     |delta| <= great_ms   → Great
//     |delta| <= good_ms    → Good
//     それ以外 → 該当 note なし扱い (戻り値 EJudgement::Miss を返すが内部統計は
//                                     更新しない、note 自体は未消化のまま)。
//   ・**hold note**: is_hold==true は「押し続ける」note だが本 Phase では
//     先頭の Tap を通常 note と同じく判定し、hold_duration_sec は情報として
//     保持するのみ (release 判定は将来拡張)。
//   ・**Miss 検出**: Tick 内で current_time > note.time_sec + good_ms / 1000 を
//     超え、まだ judged されていない note を Miss として callback 発火 + 統計
//     反映。コンボはリセット。
//   ・**combo**: Perfect / Great / Good で +1、Miss で 0 リセット。max_combo は
//     最大値を保持。
//   ・**Accuracy**: Perfect*1.0 + Great*0.8 + Good*0.5 の和を total_notes で
//     除算。LoadChart 直後は 0、再生開始前でも判定済 note があれば値を持つ。
//     total_notes==0 のときは 1.0f (満点扱い、divide-by-zero 回避)。
//   ・**チャート所有**: LoadChart で渡された FBeatNote 配列はコピーして内部
//     `m_Notes` (acs::TArray<FBeatNote>) に格納する。caller 側の寿命に依存しない。
//   ・**callback**: 関数ポインタ (std::function 不使用)。各 1 スロット。
//     JudgeCallback は Tap 起因 / Tick 起因 (Miss) どちらでも発火。
//     EndCallback は最後の note が判定された次の Tick で 1 度だけ発火。
//   ・**非コピー・非ムーブ**、全 noexcept、STL 不使用。
//   ・**STL 依存ゼロ**: container/Array.h と foundation/Types.h のみ。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// 入力レーン。4 ボタン (DDR / 太鼓系) + 拡張 2 ボタン (太鼓のドン / カッ、
// あるいは特殊ボタン)。値は安定 (save / replay 互換のため末尾追加のみ)。
enum class EBeatLane : u8 {
    Left    = 0,
    Down    = 1,
    Up      = 2,
    Right   = 3,
    Custom1 = 4,
    Custom2 = 5,
};
constexpr u32 kBeatLaneCount = 6u;

// 判定結果。値が小さいほど良い (Perfect=0)。
enum class EJudgement : u8 {
    Perfect = 0,
    Great   = 1,
    Good    = 2,
    Miss    = 3,
};

// 譜面上の 1 note。time_sec は楽曲頭からの絶対秒。
struct FBeatNote {
    f32      time_sec          = 0.0f;
    EBeatLane lane              = EBeatLane::Left;
    bool     is_hold           = false;
    f32      hold_duration_sec = 0.0f;
};

// 判定 callback。Tap (即時) / Tick (Miss 検出) のどちらでも発火する。
// user : SetOnJudgeCallback で渡した不透明ポインタ。
// lane : 判定対象 note の lane (Miss 時は元 note の lane)。
// j    : 判定結果。
// combo: 判定後の current_combo (Miss 後は 0)。
using JudgeCallback = void(*)(void* user, EBeatLane lane, EJudgement j, u32 combo) noexcept;

// 譜面終了 callback。全 note 判定完了の次 Tick で 1 度だけ発火。
// user    : SetOnEndCallback で渡した不透明ポインタ。
// hits    : Perfect+Great+Good の合計。
// misses  : Miss の合計。
// accuracy: Accuracy() と同値 ([0, 1])。
// 注意: 同 namespace acs::game に FDialogueScript::EndCallback が居るため
// FBeatGrid 側は `BeatEndCallback` という固有名にしている (rename Phase 19a-fix part 2)。
using BeatEndCallback = void(*)(void* user, u32 hits, u32 misses, f32 accuracy) noexcept;

class FBeatGrid {
public:
    FBeatGrid() noexcept = default;
    ~FBeatGrid() noexcept = default;

    // 非コピー・非ムーブ (callback の self ポインタとの競合を防ぐ)
    FBeatGrid(const FBeatGrid&)            = delete;
    FBeatGrid& operator=(const FBeatGrid&) = delete;
    FBeatGrid(FBeatGrid&&)                 = delete;
    FBeatGrid& operator=(FBeatGrid&&)      = delete;

    // ----- 初期化 -----
    // 統計 / 状態を初期化する。複数回呼出可能 (idempotent)。
    // LoadChart 前後どちらに呼んでも安全。
    void Init() noexcept;

    // ----- チャート読込 -----
    // notes: 譜面 note 配列 (caller 所有、本関数内でコピーする)。
    // count: notes 要素数。0 / nullptr は空チャートとして受理 (即 end)。
    // bpm  : 表示 / 参考用 BPM。負値は 0 に切り上げ。
    // 既存譜面と state は全破棄して上書き。Start() は別途呼ぶ必要あり。
    void LoadChart(const FBeatNote* notes, u32 count, f32 bpm) noexcept;

    // ----- 判定窓設定 -----
    // 単位 ms (ミリ秒)。perfect <= great <= good を期待。
    // 順序が逆転した場合は内部でクランプ (perfect=min(p,g,gd) 等)。
    // 負値は 0 に切り上げ。default = 25 / 50 / 100 ms。
    void SetTimingWindows(f32 perfect_ms, f32 great_ms, f32 good_ms) noexcept;

    // ----- 再生制御 -----
    void Start () noexcept; // current_time=0、再生開始
    void Stop  () noexcept; // 即時停止、current_time=0、終了 callback は呼ばない
    void Pause () noexcept; // 進行停止 (current_time / 判定 flag は保持)
    void Resume() noexcept; // Pause 後の再開
    bool IsPlaying() const noexcept { return m_Playing; }
    bool IsPaused () const noexcept { return m_Paused;  }

    f32 CurrentTimeSec() const noexcept { return m_CurrentTime; }
    f32 Bpm           () const noexcept { return m_Bpm;          }

    // ----- 入力判定 -----
    // lane の最近 note (judged=false) と current_time の差分から判定。
    // 該当 note (= |delta| <= good_window) が無ければ EJudgement::Miss を返すが
    // 統計 / コンボには影響を与えない (= caller への通知のみ)。
    // 該当 note があれば judged=true を立て、統計 / コンボを更新、判定 callback
    // を発火する。
    EJudgement Tap(EBeatLane lane) noexcept;

    // 毎フレーム呼ぶ。dt <= 0 は無視。
    // current_time を進め、good_window を過ぎて未判定の note を Miss として
    // callback 発火 + 統計反映。全 note が判定済になった次 Tick で EndCallback。
    void Tick(f32 dt) noexcept;

    // ----- 統計 -----
    u32  TotalNotes () const noexcept { return m_TotalNotes; }
    u32  HitNotes   () const noexcept { return m_PerfectCount + m_GreatCount + m_GoodCount; }
    u32  MissedNotes() const noexcept { return m_MissCount; }
    // (Perfect*1.0 + Great*0.8 + Good*0.5) / total。total==0 → 1.0f (満点扱い)。
    f32  Accuracy   () const noexcept;
    u32  MaxCombo   () const noexcept { return m_MaxCombo;     }
    u32  CurrentCombo() const noexcept { return m_CurrentCombo; }

    // ----- callback 設定 -----
    void SetOnJudgeCallback(JudgeCallback cb, void* user) noexcept {
        m_JudgeCb = cb; m_JudgeUser = user;
    }
    void SetOnEndCallback(BeatEndCallback cb, void* user) noexcept {
        m_EndCb = cb; m_EndUser = user;
    }

    // 譜面 / 統計 / 状態 / callback を全リセット (Init と同等 + 譜面破棄)。
    void ClearAll() noexcept;

private:
    // 最近 note 探索: 同一 lane / judged=false の中で |time-current| が最小の
    // index を返す。該当なしなら m_Notes.Size() を返す (= npos)。
    usize FindNearestNote(EBeatLane lane) const noexcept;
    // ms 値を sec に変換 (= ms * 0.001)。負値は 0 にクランプ。
    static f32 MsToSec(f32 ms) noexcept;
    // |delta_sec| から EJudgement を決める。good_window より外なら Miss。
    EJudgement ClassifyDelta(f32 abs_delta_sec) const noexcept;
    // 判定確定処理: stats / combo / callback 発火を一括で行う。
    void ApplyJudgement(EBeatLane lane, EJudgement j) noexcept;

    // ----- 譜面 -----
    TArray<FBeatNote> m_Notes;
    TArray<bool>    m_Judged;  // m_Notes と同 size、true = 判定済 (Hit or Miss)
    f32             m_Bpm = 0.0f;

    // ----- 判定窓 (sec) -----
    f32 m_PerfectWindowSec = 0.025f; // 25 ms
    f32 m_GreatWindowSec   = 0.050f; // 50 ms
    f32 m_GoodWindowSec    = 0.100f; // 100 ms

    // ----- 再生状態 -----
    f32  m_CurrentTime = 0.0f;
    bool m_Playing      = false;
    bool m_Paused       = false;
    bool m_bEndedFired  = false; // EndCallback の二重発火防止

    // ----- 統計 -----
    u32 m_TotalNotes    = 0u;
    u32 m_PerfectCount  = 0u;
    u32 m_GreatCount    = 0u;
    u32 m_GoodCount     = 0u;
    u32 m_MissCount     = 0u;
    u32 m_CurrentCombo  = 0u;
    u32 m_MaxCombo      = 0u;

    // ----- callback -----
    JudgeCallback m_JudgeCb   = nullptr;
    void*         m_JudgeUser = nullptr;
    BeatEndCallback m_EndCb   = nullptr;
    void*         m_EndUser   = nullptr;
};

} // namespace acs::game
