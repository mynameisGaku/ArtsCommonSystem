// SPDX-License-Identifier: Apache-2.0
// GameFramework GenreKit — FBeatGrid 実装
//
// 設計メモ:
//  ・探索は線形 O(N) (FindNearestNote)。譜面 1 曲分の note 数は通常数百〜数千、
//    Tap は 1 フレーム最大数回なので十分速い。将来 lane 別の sorted index を
//    張れば O(log N) 可能だが本 Phase では単純化を優先。
//  ・hold note は本 Phase で「先頭タップだけを通常 note と同じく判定する」
//    扱い (release 判定 / 持続中スコアは未実装)。FBeatNote::hold_duration_sec
//    は情報として保持するのみ。
//  ・EndCallback は「全 note が judged=true になった次 Tick」で 1 度だけ発火
//    する。Tap 内で最後の 1 つを判定した瞬間に直接呼ぶと、callback 内で
//    Tap が再帰される懸念があるため Tick 起因に統一。
//  ・Tick での Miss 検出は good_window を 1 frame でも過ぎたら確定。dt が
//    大きく複数 note を一括で Miss する場合も同一 Tick 内で全て発火する。
//  ・Pause 中の Tap は受理する (current_time は止まっているので判定結果は
//    pause 直前と同じ)。Stop 中の Tap は再生していないため EJudgement::Miss
//    (該当 note 無し扱い) を返すのみ。
#include "gameframework/BeatGrid.h"

namespace acs::game {

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------

f32 FBeatGrid::MsToSec(f32 ms) noexcept {
    if (ms < 0.0f) return 0.0f;
    return ms * 0.001f;
}

EJudgement FBeatGrid::ClassifyDelta(f32 abs_delta_sec) const noexcept {
    if (abs_delta_sec <= m_PerfectWindowSec) return EJudgement::Perfect;
    if (abs_delta_sec <= m_GreatWindowSec)   return EJudgement::Great;
    if (abs_delta_sec <= m_GoodWindowSec)    return EJudgement::Good;
    return EJudgement::Miss;
}

usize FBeatGrid::FindNearestNote(EBeatLane lane) const noexcept {
    const usize n = m_Notes.Size();
    usize best_idx = n;            // npos
    f32   best_abs = m_GoodWindowSec; // good_window を超える note は対象外

    for (usize i = 0; i < n; ++i) {
        if (m_Judged[i]) continue;
        const FBeatNote& note = m_Notes[i];
        if (note.lane != lane) continue;
        f32 delta = note.time_sec - m_CurrentTime;
        if (delta < 0.0f) delta = -delta;
        if (delta <= best_abs) {
            best_abs = delta;
            best_idx = i;
        }
    }
    return best_idx;
}

void FBeatGrid::ApplyJudgement(EBeatLane lane, EJudgement j) noexcept {
    switch (j) {
        case EJudgement::Perfect: ++m_PerfectCount; ++m_CurrentCombo; break;
        case EJudgement::Great:   ++m_GreatCount;   ++m_CurrentCombo; break;
        case EJudgement::Good:    ++m_GoodCount;    ++m_CurrentCombo; break;
        case EJudgement::Miss:    ++m_MissCount;    m_CurrentCombo = 0u; break;
    }
    if (m_CurrentCombo > m_MaxCombo) m_MaxCombo = m_CurrentCombo;

    if (m_JudgeCb != nullptr) {
        m_JudgeCb(m_JudgeUser, lane, j, m_CurrentCombo);
    }
}

// ----------------------------------------------------------------------------
// 初期化 / 読込
// ----------------------------------------------------------------------------

void FBeatGrid::Init() noexcept {
    // 譜面 / callback は保持し、再生状態 / 統計のみリセット。
    // (callback は呼出側が SetOn* で別途設定済みの想定なので維持する。
    //  譜面は LoadChart で都度上書きするため Init 単体ではクリアしない方が
    //  「Init → LoadChart → Start」の素直なフローを邪魔しない。)
    m_CurrentTime   = 0.0f;
    m_Playing        = false;
    m_Paused         = false;
    m_bEndedFired    = false;
    m_PerfectCount  = 0u;
    m_GreatCount    = 0u;
    m_GoodCount     = 0u;
    m_MissCount     = 0u;
    m_CurrentCombo  = 0u;
    m_MaxCombo      = 0u;

    // judged flag は既存譜面に対して全 false 化する (= 再判定可能状態)。
    const usize n = m_Judged.Size();
    for (usize i = 0; i < n; ++i) m_Judged[i] = false;
}

void FBeatGrid::LoadChart(const FBeatNote* notes, u32 count, f32 bpm) noexcept {
    // 既存譜面を破棄
    m_Notes.Clear();
    m_Judged.Clear();

    m_Bpm = (bpm < 0.0f) ? 0.0f : bpm;

    if (notes != nullptr && count > 0u) {
        m_Notes.Reserve(count);
        m_Judged.Reserve(count);
        for (u32 i = 0; i < count; ++i) {
            FBeatNote n = notes[i];
            if (n.hold_duration_sec < 0.0f) n.hold_duration_sec = 0.0f;
            m_Notes.PushBack(n);
            m_Judged.PushBack(false);
        }
    }
    m_TotalNotes = static_cast<u32>(m_Notes.Size());

    // 統計 / 状態リセット
    m_CurrentTime   = 0.0f;
    m_Playing        = false;
    m_Paused         = false;
    m_bEndedFired    = false;
    m_PerfectCount  = 0u;
    m_GreatCount    = 0u;
    m_GoodCount     = 0u;
    m_MissCount     = 0u;
    m_CurrentCombo  = 0u;
    m_MaxCombo      = 0u;
}

void FBeatGrid::SetTimingWindows(f32 perfect_ms, f32 great_ms, f32 good_ms) noexcept {
    f32 p = MsToSec(perfect_ms);
    f32 g = MsToSec(great_ms);
    f32 d = MsToSec(good_ms);
    // 順序逆転は内部で整形 (= perfect <= great <= good を保証)
    if (g < p) g = p;
    if (d < g) d = g;
    m_PerfectWindowSec = p;
    m_GreatWindowSec   = g;
    m_GoodWindowSec    = d;
}

// ----------------------------------------------------------------------------
// 再生制御
// ----------------------------------------------------------------------------

void FBeatGrid::Start() noexcept {
    m_CurrentTime = 0.0f;
    m_Playing      = true;
    m_Paused       = false;
    m_bEndedFired  = false;
    // judged flag を再判定可能状態に戻す + 統計クリア (= 再挑戦)
    const usize n = m_Judged.Size();
    for (usize i = 0; i < n; ++i) m_Judged[i] = false;
    m_PerfectCount = 0u;
    m_GreatCount   = 0u;
    m_GoodCount    = 0u;
    m_MissCount    = 0u;
    m_CurrentCombo = 0u;
    m_MaxCombo     = 0u;
}

void FBeatGrid::Stop() noexcept {
    m_Playing      = false;
    m_Paused       = false;
    m_CurrentTime = 0.0f;
    // 統計 / judged は呼出側がスコア表示等に使う可能性があるため維持する。
    // ClearAll で完全クリア。
}

void FBeatGrid::Pause() noexcept {
    if (m_Playing) m_Paused = true;
}

void FBeatGrid::Resume() noexcept {
    if (m_Playing) m_Paused = false;
}

// ----------------------------------------------------------------------------
// 入力 / 駆動
// ----------------------------------------------------------------------------

EJudgement FBeatGrid::Tap(EBeatLane lane) noexcept {
    // 停止中は判定対象なし
    if (!m_Playing) return EJudgement::Miss;

    const usize idx = FindNearestNote(lane);
    if (idx >= m_Notes.Size()) {
        // good_window 内に該当 note なし: caller への通知は Miss だが
        // 統計には影響させない (空打ち)。
        return EJudgement::Miss;
    }

    const FBeatNote& note = m_Notes[idx];
    f32 delta = note.time_sec - m_CurrentTime;
    if (delta < 0.0f) delta = -delta;
    const EJudgement j = ClassifyDelta(delta);

    // ClassifyDelta は FindNearestNote の best_abs <= good_window 制約により
    // ここで Miss にはならないはずだが、defense-in-depth で再確認。
    if (j == EJudgement::Miss) return EJudgement::Miss;

    m_Judged[idx] = true;
    ApplyJudgement(lane, j);
    return j;
}

void FBeatGrid::Tick(f32 dt) noexcept {
    if (!m_Playing) return;
    if (m_Paused)   return;
    if (dt <= 0.0f) return;

    m_CurrentTime += dt;

    // good_window を 1 frame でも過ぎた note を Miss として確定。
    // 一括処理: 大 dt や譜面後半に未判定が残ったまま停止 → 再生再開の場合も
    // 同一 Tick で全部捌く。
    const usize n = m_Notes.Size();
    const f32 miss_threshold = m_CurrentTime - m_GoodWindowSec;
    for (usize i = 0; i < n; ++i) {
        if (m_Judged[i]) continue;
        if (m_Notes[i].time_sec < miss_threshold) {
            m_Judged[i] = true;
            ApplyJudgement(m_Notes[i].lane, EJudgement::Miss);
        }
    }

    // 全 note 判定完了 → 一度だけ EndCallback。
    if (!m_bEndedFired) {
        u32 judged_count = 0u;
        for (usize i = 0; i < n; ++i) {
            if (m_Judged[i]) ++judged_count;
        }
        if (judged_count == m_TotalNotes) {
            m_bEndedFired = true;
            if (m_EndCb != nullptr) {
                m_EndCb(m_EndUser, HitNotes(), m_MissCount, Accuracy());
            }
        }
    }
}

// ----------------------------------------------------------------------------
// 統計
// ----------------------------------------------------------------------------

f32 FBeatGrid::Accuracy() const noexcept {
    if (m_TotalNotes == 0u) return 1.0f; // 空譜面 = 満点扱い (divide-by-zero 回避)
    const f32 weighted =
        static_cast<f32>(m_PerfectCount) * 1.0f +
        static_cast<f32>(m_GreatCount)   * 0.8f +
        static_cast<f32>(m_GoodCount)    * 0.5f;
    return weighted / static_cast<f32>(m_TotalNotes);
}

// ----------------------------------------------------------------------------
// 全リセット
// ----------------------------------------------------------------------------

void FBeatGrid::ClearAll() noexcept {
    m_Notes.Clear();
    m_Judged.Clear();
    m_Bpm                = 0.0f;
    m_PerfectWindowSec = 0.025f;
    m_GreatWindowSec   = 0.050f;
    m_GoodWindowSec    = 0.100f;
    m_CurrentTime       = 0.0f;
    m_Playing            = false;
    m_Paused             = false;
    m_bEndedFired        = false;
    m_TotalNotes        = 0u;
    m_PerfectCount      = 0u;
    m_GreatCount        = 0u;
    m_GoodCount         = 0u;
    m_MissCount         = 0u;
    m_CurrentCombo      = 0u;
    m_MaxCombo          = 0u;
    m_JudgeCb           = nullptr;
    m_JudgeUser         = nullptr;
    m_EndCb             = nullptr;
    m_EndUser           = nullptr;
}

} // namespace acs::game
