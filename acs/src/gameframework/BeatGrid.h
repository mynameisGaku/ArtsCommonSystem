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
//   ・**hold note**: is_hold==true は「押し続ける」note だが現状は
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

/**
 * 入力レーン。
 *
 * @details
 * 4 ボタン (DDR / 太鼓系) + 拡張 2 ボタン (太鼓のドン / カッ、あるいは特殊
 * ボタン)。値は安定 (save / replay 互換のため末尾追加のみ)。
 */
enum class EBeatLane : u8 {
    /** 左 (DDR の ←、または太鼓のドン)。 */
    Left    = 0,

    /** 下 (DDR の ↓)。 */
    Down    = 1,

    /** 上 (DDR の ↑)。 */
    Up      = 2,

    /** 右 (DDR の →、または太鼓のカッ)。 */
    Right   = 3,

    /** 拡張ボタン 1。 */
    Custom1 = 4,

    /** 拡張ボタン 2。 */
    Custom2 = 5,
};

/** 入力レーンの総数。 */
constexpr u32 kBeatLaneCount = 6u;

/**
 * 判定結果。値が小さいほど良い (Perfect=0)。
 */
enum class EJudgement : u8 {
    /** 最良判定 (|delta| <= perfect_window)。 */
    Perfect = 0,

    /** 中判定 (|delta| <= great_window)。 */
    Great   = 1,

    /** 可判定 (|delta| <= good_window)。 */
    Good    = 2,

    /** 失敗 (good_window 外、または該当 note なし)。 */
    Miss    = 3,
};

/**
 * 譜面上の 1 note。
 */
struct FBeatNote {
    /** 楽曲頭からの絶対秒。 */
    f32      time_sec          = 0.0f;

    /** この note を叩くレーン。 */
    EBeatLane lane              = EBeatLane::Left;

    /** 押し続け note なら true (現状は先頭 Tap のみ判定)。 */
    bool     is_hold           = false;

    /** hold note の押下持続秒 (情報として保持。release 判定は将来拡張)。 */
    f32      hold_duration_sec = 0.0f;
};

/**
 * 判定 callback の型。Tap (即時) / Tick (Miss 検出) のどちらでも発火する。
 *
 * @param user SetOnJudgeCallback で渡した不透明ポインタ。
 * @param lane 判定対象 note の lane (Miss 時は元 note の lane)。
 * @param j 判定結果。
 * @param combo 判定後の current_combo (Miss 後は 0)。
 */
using JudgeCallback = void(*)(void* user, EBeatLane lane, EJudgement j, u32 combo) noexcept;

/**
 * 譜面終了 callback の型。全 note 判定完了の次 Tick で 1 度だけ発火する。
 *
 * @details
 * 同 namespace acs::game に FDialogueScript::EndCallback が居るため
 * FBeatGrid 側は BeatEndCallback という固有名にしている。
 * @param user SetOnEndCallback で渡した不透明ポインタ。
 * @param hits Perfect+Great+Good の合計。
 * @param misses Miss の合計。
 * @param accuracy Accuracy() と同値 ([0, 1])。
 */
using BeatEndCallback = void(*)(void* user, u32 hits, u32 misses, f32 accuracy) noexcept;

/**
 * 1 譜面分の note 配列を保持しタイミング判定を行う rhythm game state machine。
 *
 * @details
 * 再生時刻 (current_time) を進行させながらプレイヤー入力 (Tap) を最近 note と
 * 突き合わせて Perfect / Great / Good / Miss の 4 段階で判定する。譜面は内部に
 * コピー所有し、判定 / 終了は関数ポインタ callback で通知する。非コピー・非ムーブ、
 * 全 noexcept、STL 不使用。
 */
class FBeatGrid {
public:
    /** 空の grid を構築する (譜面は LoadChart で読み込む)。 */
    FBeatGrid() noexcept = default;

    /** 破棄する (内部配列は TArray が解放)。 */
    ~FBeatGrid() noexcept = default;

    /** コピー禁止 (callback の self ポインタとの競合を防ぐため)。 */
    FBeatGrid(const FBeatGrid&)            = delete;

    /** コピー代入も禁止。 */
    FBeatGrid& operator=(const FBeatGrid&) = delete;

    /** ムーブ禁止。 */
    FBeatGrid(FBeatGrid&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FBeatGrid& operator=(FBeatGrid&&)      = delete;

    /**
     * 再生状態と統計を初期化する (譜面 / callback は保持)。
     *
     * @details
     * 複数回呼出可能 (idempotent)。LoadChart 前後どちらに呼んでも安全。
     * 既存譜面の judged フラグは全 false に戻す (= 再判定可能状態)。
     */
    void Init() noexcept;

    /**
     * 譜面 note 配列を読み込んで内部にコピー所有する。
     *
     * @details
     * 既存譜面と state は全破棄して上書きする。Start() は別途呼ぶ必要がある。
     * @param notes 譜面 note 配列 (caller 所有、本関数内でコピーする)。
     * @param count notes の要素数 (0 / nullptr は空チャートとして受理し即 end)。
     * @param bpm 表示 / 参考用 BPM (負値は 0 に切り上げ)。
     */
    void LoadChart(const FBeatNote* notes, u32 count, f32 bpm) noexcept;

    /**
     * 判定窓を設定する (単位 ms)。
     *
     * @details
     * perfect <= great <= good を期待。順序が逆転した場合は内部でクランプして
     * 単調増加を保証する。default = 25 / 50 / 100 ms。
     * @param perfect_ms Perfect 判定窓のミリ秒 (負値は 0 に切り上げ)。
     * @param great_ms Great 判定窓のミリ秒 (perfect_ms 未満なら perfect_ms に補正)。
     * @param good_ms Good 判定窓のミリ秒 (great_ms 未満なら great_ms に補正)。
     */
    void SetTimingWindows(f32 perfect_ms, f32 great_ms, f32 good_ms) noexcept;

    /** 再生を開始する (current_time=0、judged フラグと統計をリセット)。 */
    void Start () noexcept;

    /** 即時停止する (current_time=0、終了 callback は呼ばない、統計は維持)。 */
    void Stop  () noexcept;

    /** 進行を一時停止する (current_time / 判定フラグは保持)。 */
    void Pause () noexcept;

    /** Pause 後に再開する。 */
    void Resume() noexcept;

    /**
     * 再生中かを返す。
     *
     * @return Start 済みで Stop されていなければ true。
     */
    bool IsPlaying() const noexcept { return m_Playing; }

    /**
     * 一時停止中かを返す。
     *
     * @return Pause 中なら true。
     */
    bool IsPaused () const noexcept { return m_Paused;  }

    /**
     * 現在の再生時刻を返す。
     *
     * @return 楽曲頭からの経過秒。
     */
    f32 CurrentTimeSec() const noexcept { return m_CurrentTime; }

    /**
     * 譜面の BPM を返す。
     *
     * @return LoadChart で渡された表示 / 参考用 BPM。
     */
    f32 Bpm           () const noexcept { return m_Bpm;          }

    /**
     * lane への入力を最近 note と突き合わせて判定する。
     *
     * @details
     * lane の最近 note (judged=false) と current_time の差分から判定する。
     * 該当 note (= |delta| <= good_window) が無ければ EJudgement::Miss を返すが
     * 統計 / コンボには影響を与えない (= caller への通知のみ)。該当 note が
     * あれば judged=true を立て、統計 / コンボを更新し判定 callback を発火する。
     * @param lane 入力されたレーン。
     * @return 判定結果 (該当 note なし / 停止中は Miss)。
     */
    EJudgement Tap(EBeatLane lane) noexcept;

    /**
     * 毎フレーム呼んで再生時刻を進める。
     *
     * @details
     * current_time を進め、good_window を過ぎて未判定の note を Miss として
     * callback 発火 + 統計反映する。全 note が判定済になった次 Tick で
     * EndCallback を 1 度だけ発火する。
     * @param dt 前フレームからの経過秒 (0 以下は無視)。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 譜面の総 note 数を返す。
     *
     * @return LoadChart で読み込んだ note 数。
     */
    u32  TotalNotes () const noexcept { return m_TotalNotes; }

    /**
     * Hit した note 数 (Perfect+Great+Good) を返す。
     *
     * @return 命中 note の合計数。
     */
    u32  HitNotes   () const noexcept { return m_PerfectCount + m_GreatCount + m_GoodCount; }

    /**
     * Miss した note 数を返す。
     *
     * @return Miss 判定の合計数。
     */
    u32  MissedNotes() const noexcept { return m_MissCount; }

    /**
     * 正確度を返す。
     *
     * @return (Perfect*1.0 + Great*0.8 + Good*0.5) / total ([0, 1])。total==0 は 1.0f (満点扱い)。
     */
    f32  Accuracy   () const noexcept;

    /**
     * 最大コンボ数を返す。
     *
     * @return 再生中に到達した最大連続 Hit 数。
     */
    u32  MaxCombo   () const noexcept { return m_MaxCombo;     }

    /**
     * 現在のコンボ数を返す。
     *
     * @return 現在の連続 Hit 数 (Miss で 0 にリセット)。
     */
    u32  CurrentCombo() const noexcept { return m_CurrentCombo; }

    /**
     * 判定 callback を設定する。
     *
     * @param cb 判定時に呼ばれる関数ポインタ (nullptr で解除)。
     * @param user callback に渡す不透明ポインタ。
     */
    void SetOnJudgeCallback(JudgeCallback cb, void* user) noexcept {
        m_JudgeCb = cb; m_JudgeUser = user;
    }

    /**
     * 譜面終了 callback を設定する。
     *
     * @param cb 全 note 判定完了時に呼ばれる関数ポインタ (nullptr で解除)。
     * @param user callback に渡す不透明ポインタ。
     */
    void SetOnEndCallback(BeatEndCallback cb, void* user) noexcept {
        m_EndCb = cb; m_EndUser = user;
    }

    /** 譜面 / 統計 / 状態 / callback を全リセットする (Init と同等 + 譜面破棄)。 */
    void ClearAll() noexcept;

private:
    /**
     * lane の最近 note を線形探索する。
     *
     * @param lane 探索対象のレーン。
     * @return 同一 lane / judged=false の中で |time-current| が最小の index (該当なしは m_Notes.Size())。
     */
    usize FindNearestNote(EBeatLane lane) const noexcept;

    /**
     * ms 値を sec に変換する。
     *
     * @param ms ミリ秒値。
     * @return ms * 0.001 (負値は 0 にクランプ)。
     */
    static f32 MsToSec(f32 ms) noexcept;

    /**
     * |delta| から判定結果を決める。
     *
     * @param abs_delta_sec note 時刻と current_time の差分絶対値 (秒)。
     * @return 対応する EJudgement (good_window より外なら Miss)。
     */
    EJudgement ClassifyDelta(f32 abs_delta_sec) const noexcept;

    /**
     * 判定を確定して統計 / コンボ更新 + callback 発火を一括で行う。
     *
     * @param lane 判定対象 note の lane。
     * @param j 確定した判定結果。
     */
    void ApplyJudgement(EBeatLane lane, EJudgement j) noexcept;

    /** 譜面 note 配列 (LoadChart でコピー所有)。 */
    TArray<FBeatNote> m_Notes;

    /** m_Notes と同 size の判定済フラグ (true = Hit or Miss)。 */
    TArray<bool>    m_Judged;

    /** 譜面の表示 / 参考用 BPM。 */
    f32             m_Bpm = 0.0f;

    /** Perfect 判定窓 (秒、既定 25 ms)。 */
    f32 m_PerfectWindowSec = 0.025f;

    /** Great 判定窓 (秒、既定 50 ms)。 */
    f32 m_GreatWindowSec   = 0.050f;

    /** Good 判定窓 (秒、既定 100 ms)。 */
    f32 m_GoodWindowSec    = 0.100f;

    /** 現在の再生時刻 (楽曲頭からの経過秒)。 */
    f32  m_CurrentTime = 0.0f;

    /** 再生中フラグ。 */
    bool m_Playing      = false;

    /** 一時停止中フラグ。 */
    bool m_Paused       = false;

    /** EndCallback の二重発火防止フラグ。 */
    bool m_bEndedFired  = false;

    /** 譜面の総 note 数。 */
    u32 m_TotalNotes    = 0u;

    /** Perfect 判定数。 */
    u32 m_PerfectCount  = 0u;

    /** Great 判定数。 */
    u32 m_GreatCount    = 0u;

    /** Good 判定数。 */
    u32 m_GoodCount     = 0u;

    /** Miss 判定数。 */
    u32 m_MissCount     = 0u;

    /** 現在のコンボ数 (Miss で 0 リセット)。 */
    u32 m_CurrentCombo  = 0u;

    /** 到達した最大コンボ数。 */
    u32 m_MaxCombo      = 0u;

    /** 判定 callback (未設定なら nullptr)。 */
    JudgeCallback m_JudgeCb   = nullptr;

    /** 判定 callback に渡す不透明ポインタ。 */
    void*         m_JudgeUser = nullptr;

    /** 譜面終了 callback (未設定なら nullptr)。 */
    BeatEndCallback m_EndCb   = nullptr;

    /** 譜面終了 callback に渡す不透明ポインタ。 */
    void*         m_EndUser   = nullptr;
};

} // namespace acs::game
