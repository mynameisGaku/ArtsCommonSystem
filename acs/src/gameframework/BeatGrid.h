// SPDX-License-Identifier: Apache-2.0
// GameFramework GenreKit — BeatGrid (rhythm game タイミング判定 + チャート再生)
//
// 1 譜面分の note 配列を保持し、再生時刻 (current_time) を進行させながら
// プレイヤー入力 (Tap) を最近 note と突き合わせて Perfect / Great / Good / Miss
// の 4 段階で判定する。MusicDirector / AudioDirector とは独立に動く軽量
// state machine で、譜面と入力の対応関係だけを管理する。
//
// 使い方:
//   class RhythmScene : public Scene {
//       acs::game::BeatGrid _grid;
//
//       void OnEnter() noexcept override {
//           _grid.Init();
//           _grid.SetTimingWindows(25.0f, 50.0f, 100.0f);
//           _grid.SetOnJudgeCallback(&RhythmScene::OnJudge, this);
//           _grid.SetOnEndCallback (&RhythmScene::OnEnd,   this);
//
//           acs::game::BeatNote notes[] = {
//               { 1.000f, acs::game::EBeatLane::Left,  false, 0.0f },
//               { 1.500f, acs::game::EBeatLane::Down,  false, 0.0f },
//               { 2.000f, acs::game::EBeatLane::Up,    true,  0.5f },
//           };
//           _grid.LoadChart(notes, 3, 120.0f);
//           _grid.Start();
//       }
//       void OnUpdate(f32 dt) noexcept override { _grid.Tick(dt); }
//       void OnInput() noexcept {
//           if (key_left_pressed)
//               (void)_grid.Tap(acs::game::EBeatLane::Left);
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
//   ・**チャート所有**: LoadChart で渡された BeatNote 配列はコピーして内部
//     `_notes` (acs::TArray<BeatNote>) に格納する。caller 側の寿命に依存しない。
//   ・**callback**: 関数ポインタ (std::function 不使用)。各 1 スロット。
//     JudgeCallback は Tap 起因 / Tick 起因 (Miss) どちらでも発火。
//     EndCallback は最後の note が判定された次の Tick で 1 度だけ発火。
//   ・**非コピー・非ムーブ**、全 noexcept、STL 不使用。
//   ・**STL 依存ゼロ**: container/TArray.h と foundation/Types.h のみ。
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
struct BeatNote {
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
// 注意: 同 namespace acs::game に DialogueScript::EndCallback が居るため
// BeatGrid 側は `BeatEndCallback` という固有名にしている (rename Phase 19a-fix part 2)。
using BeatEndCallback = void(*)(void* user, u32 hits, u32 misses, f32 accuracy) noexcept;

class BeatGrid {
public:
    BeatGrid() noexcept = default;
    ~BeatGrid() noexcept = default;

    // 非コピー・非ムーブ (callback の self ポインタとの競合を防ぐ)
    BeatGrid(const BeatGrid&)            = delete;
    BeatGrid& operator=(const BeatGrid&) = delete;
    BeatGrid(BeatGrid&&)                 = delete;
    BeatGrid& operator=(BeatGrid&&)      = delete;

    // ----- 初期化 -----
    // 統計 / 状態を初期化する。複数回呼出可能 (idempotent)。
    // LoadChart 前後どちらに呼んでも安全。
    void Init() noexcept;

    // ----- チャート読込 -----
    // notes: 譜面 note 配列 (caller 所有、本関数内でコピーする)。
    // count: notes 要素数。0 / nullptr は空チャートとして受理 (即 end)。
    // bpm  : 表示 / 参考用 BPM。負値は 0 に切り上げ。
    // 既存譜面と state は全破棄して上書き。Start() は別途呼ぶ必要あり。
    void LoadChart(const BeatNote* notes, u32 count, f32 bpm) noexcept;

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
    bool IsPlaying() const noexcept { return _playing; }
    bool IsPaused () const noexcept { return _paused;  }

    f32 CurrentTimeSec() const noexcept { return _current_time; }
    f32 Bpm           () const noexcept { return _bpm;          }

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
    u32  TotalNotes () const noexcept { return _total_notes; }
    u32  HitNotes   () const noexcept { return _perfect_count + _great_count + _good_count; }
    u32  MissedNotes() const noexcept { return _miss_count; }
    // (Perfect*1.0 + Great*0.8 + Good*0.5) / total。total==0 → 1.0f (満点扱い)。
    f32  Accuracy   () const noexcept;
    u32  MaxCombo   () const noexcept { return _max_combo;     }
    u32  CurrentCombo() const noexcept { return _current_combo; }

    // ----- callback 設定 -----
    void SetOnJudgeCallback(JudgeCallback cb, void* user) noexcept {
        _judge_cb = cb; _judge_user = user;
    }
    void SetOnEndCallback(BeatEndCallback cb, void* user) noexcept {
        _end_cb = cb; _end_user = user;
    }

    // 譜面 / 統計 / 状態 / callback を全リセット (Init と同等 + 譜面破棄)。
    void ClearAll() noexcept;

private:
    // 最近 note 探索: 同一 lane / judged=false の中で |time-current| が最小の
    // index を返す。該当なしなら _notes.Size() を返す (= npos)。
    usize FindNearestNote(EBeatLane lane) const noexcept;
    // ms 値を sec に変換 (= ms * 0.001)。負値は 0 にクランプ。
    static f32 MsToSec(f32 ms) noexcept;
    // |delta_sec| から EJudgement を決める。good_window より外なら Miss。
    EJudgement ClassifyDelta(f32 abs_delta_sec) const noexcept;
    // 判定確定処理: stats / combo / callback 発火を一括で行う。
    void ApplyJudgement(EBeatLane lane, EJudgement j) noexcept;

    // ----- 譜面 -----
    TArray<BeatNote> _notes;
    TArray<bool>    _judged;  // _notes と同 size、true = 判定済 (Hit or Miss)
    f32             _bpm = 0.0f;

    // ----- 判定窓 (sec) -----
    f32 _perfect_window_sec = 0.025f; // 25 ms
    f32 _great_window_sec   = 0.050f; // 50 ms
    f32 _good_window_sec    = 0.100f; // 100 ms

    // ----- 再生状態 -----
    f32  _current_time = 0.0f;
    bool _playing      = false;
    bool _paused       = false;
    bool _ended_fired  = false; // EndCallback の二重発火防止

    // ----- 統計 -----
    u32 _total_notes    = 0u;
    u32 _perfect_count  = 0u;
    u32 _great_count    = 0u;
    u32 _good_count     = 0u;
    u32 _miss_count     = 0u;
    u32 _current_combo  = 0u;
    u32 _max_combo      = 0u;

    // ----- callback -----
    JudgeCallback _judge_cb   = nullptr;
    void*         _judge_user = nullptr;
    BeatEndCallback _end_cb   = nullptr;
    void*         _end_user   = nullptr;
};

} // namespace acs::game
