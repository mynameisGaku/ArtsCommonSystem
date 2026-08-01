// SPDX-License-Identifier: Apache-2.0
// リズムゲーム譜面の再生、タップ判定、ホールドノートのライフサイクル。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs {
class FAllocator;
}

namespace acs::game {

enum class EBeatLane : u8 {
    Left = 0,
    Down = 1,
    Up = 2,
    Right = 3,
    Custom1 = 4,
    Custom2 = 5,
};

inline constexpr u32 kBeatLaneCount = 6u;
inline constexpr u32 kMaxBeatChartNotes = 65536u;
inline constexpr f32 kMaxBeatChartTimeSec = 86400.0f;
inline constexpr f32 kMaxBeatHoldDurationSec = 3600.0f;
inline constexpr f32 kMaxBeatBpm = 1000.0f;
inline constexpr f32 kMaxBeatTimingWindowMs = 10000.0f;

enum class EJudgement : u8 {
    Perfect = 0,
    Great = 1,
    Good = 2,
    Miss = 3,
};

struct FBeatNote {
    f32 time_sec = 0.0f;
    EBeatLane lane = EBeatLane::Left;
    bool is_hold = false;
    f32 hold_duration_sec = 0.0f;
};

/**
 * 検証付き譜面読み込み API の結果。
 *
 * Success 以外では、現在の譜面・再生状態・統計・コールバックを変更しない。
 */
enum class EBeatChartLoadResult : u8 {
    Success = 0,
    NullNotes,
    TooManyNotes,
    InvalidBpm,
    InvalidNoteTime,
    InvalidLane,
    InvalidHoldDuration,
    OutOfMemory,
};

/** 譜面読み込み結果の安定した診断名を返す。 */
const char* BeatChartLoadResultName(EBeatChartLoadResult result) noexcept;

using FJudgeCallback =
    void(*)(void* user, EBeatLane lane, EJudgement judgement, u32 combo) noexcept;
using FBeatEndCallback =
    void(*)(void* user, u32 hits, u32 misses, f32 accuracy) noexcept;

/** 互換 alias。新規コードでは F 接頭辞付きコールバック型を使う。 */
using JudgeCallback = FJudgeCallback;
using BeatEndCallback = FBeatEndCallback;

/**
 * コピーした譜面を所有し、再生時計に対して各ノートを判定する。
 *
 * 通常ノートは PressLane/Tap で確定する。ホールドノートは PressLane/Tap で開始し、
 * ReleaseLane または Tick のタイムアウトで一度だけ確定する。先頭と末尾の判定は
 * 悪い方を採用し、末尾の Good 範囲より前の解放は即座に Miss とする。
 */
class CBeatGrid {
public:
    CBeatGrid() noexcept = default;

    /** 決定論的な確保失敗テストやサブシステム固有 allocator を利用可能にする。 */
    explicit CBeatGrid(FAllocator& allocator) noexcept;

    ~CBeatGrid() noexcept = default;

    CBeatGrid(const CBeatGrid&) = delete;
    CBeatGrid& operator=(const CBeatGrid&) = delete;
    CBeatGrid(CBeatGrid&&) = delete;
    CBeatGrid& operator=(CBeatGrid&&) = delete;

    /** 譜面とコールバックを保持したまま再生状態とスコアをリセットする。 */
    void Init() noexcept;

    /**
     * 譜面全体を検証・一時構築してからコミットする。
     *
     * ノートは呼び出し側の順序を保つ。同距離の候補は入力 index が小さい方を採用する。
     * 空譜面では nullptr を許可する。
     */
    EBeatChartLoadResult TryLoadChart(
        const FBeatNote* notes, u32 count, f32 bpm) noexcept;

    /**
     * TryLoadChart の互換ラッパー。
     *
     * 不正入力や確保失敗は無視し、部分置換せず読み込み済み譜面を維持する。
     */
    void LoadChart(const FBeatNote* notes, u32 count, f32 bpm) noexcept;

    /**
     * 判定時間幅を検証付きで更新する。値は有限・非負・kMaxBeatTimingWindowMs 以下で、
     * perfect <= great <= good の順序を満たす必要がある。
     */
    bool TrySetTimingWindows(
        f32 perfect_ms, f32 great_ms, f32 good_ms) noexcept;

    /**
     * 互換ラッパー。有限値は順序付き時間幅へ clamp し、非有限値なら全時間幅を維持する。
     */
    void SetTimingWindows(f32 perfect_ms, f32 great_ms, f32 good_ms) noexcept;

    void Start() noexcept;
    void Stop() noexcept;
    void Pause() noexcept;
    void Resume() noexcept;

    bool IsPlaying() const noexcept { return m_Playing; }
    bool IsPaused() const noexcept { return m_Paused; }
    f32 CurrentTimeSec() const noexcept { return m_CurrentTime; }
    f32 Bpm() const noexcept { return m_Bpm; }

    /**
     * レーンを押す。通常ノートは即時確定し、ホールドノートは active 状態へ入り、
     * 解放またはタイムアウトまではスコアイベントを発火せず先頭判定だけを返す。
     */
    EJudgement PressLane(EBeatLane lane) noexcept;

    /**
     * 現在時刻に末尾が最も近い active ホールドを解放する。同距離なら譜面入力順を使う。
     * active ホールドが無ければ統計を変えず Miss を返す。
     */
    EJudgement ReleaseLane(EBeatLane lane) noexcept;

    /** PressLane の旧互換 alias。通常ノートの挙動は同じ。 */
    EJudgement Tap(EBeatLane lane) noexcept { return PressLane(lane); }

    bool IsLaneHolding(EBeatLane lane) const noexcept;
    u32 ActiveHoldCount() const noexcept;

    /**
     * 再生を進める。非正値・非有限値・f32 overflow を起こす加算は状態を変えず無視する。
     */
    void Tick(f32 dt) noexcept;

    u32 TotalNotes() const noexcept { return m_TotalNotes; }
    u32 HitNotes() const noexcept {
        return m_PerfectCount + m_GreatCount + m_GoodCount;
    }
    u32 MissedNotes() const noexcept { return m_MissCount; }
    f32 Accuracy() const noexcept;
    u32 MaxCombo() const noexcept { return m_MaxCombo; }
    u32 CurrentCombo() const noexcept { return m_CurrentCombo; }

    void SetOnJudgeCallback(FJudgeCallback cb, void* user) noexcept {
        m_JudgeCb = cb;
        m_JudgeUser = user;
    }

    void SetOnEndCallback(FBeatEndCallback cb, void* user) noexcept {
        m_EndCb = cb;
        m_EndUser = user;
    }

    /** 譜面・状態・統計・判定時間幅・コールバックを全て消去する。 */
    void ClearAll() noexcept;

private:
    struct FHoldState {
        bool active = false;
        EJudgement head_judgement = EJudgement::Miss;
    };

    usize FindNearestNote(EBeatLane lane) const noexcept;
    usize FindNearestActiveHold(EBeatLane lane) const noexcept;
    EJudgement ClassifyDelta(f32 abs_delta_sec) const noexcept;

    /**
     * スコアを更新してコールバックを呼ぶ。コールバックが状態変更を伴う再入操作を
     * 行った場合は false を返す。
     */
    bool ApplyJudgement(EBeatLane lane, EJudgement judgement) noexcept;

    bool FireEndIfComplete() noexcept;
    void ResetRunState() noexcept;
    void ClearActiveHolds() noexcept;
    void BumpRevision() noexcept;

    TArray<FBeatNote> m_Notes;
    TArray<bool> m_Judged;
    TArray<FHoldState> m_Holds;

    f32 m_Bpm = 0.0f;
    f32 m_PerfectWindowSec = 0.025f;
    f32 m_GreatWindowSec = 0.050f;
    f32 m_GoodWindowSec = 0.100f;
    f32 m_CurrentTime = 0.0f;

    bool m_Playing = false;
    bool m_Paused = false;
    bool m_bEndedFired = false;

    u32 m_TotalNotes = 0u;
    u32 m_PerfectCount = 0u;
    u32 m_GreatCount = 0u;
    u32 m_GoodCount = 0u;
    u32 m_MissCount = 0u;
    u32 m_CurrentCombo = 0u;
    u32 m_MaxCombo = 0u;

    FJudgeCallback m_JudgeCb = nullptr;
    void* m_JudgeUser = nullptr;
    FBeatEndCallback m_EndCb = nullptr;
    void* m_EndUser = nullptr;

    /** Tick ループ中にコールバック起点の置換・リセットを検出する。 */
    u64 m_StateRevision = 1u;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FBeatGrid = CBeatGrid;

} // namespace acs::game
