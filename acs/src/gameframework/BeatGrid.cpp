// SPDX-License-Identifier: Apache-2.0
// GameFramework GenreKit — BeatGrid 実装
//
// 設計メモ:
//  ・探索は線形 O(N) (FindNearestNote)。譜面 1 曲分の note 数は通常数百〜数千、
//    Tap は 1 フレーム最大数回なので十分速い。将来 lane 別の sorted index を
//    張れば O(log N) 可能だが本 Phase では単純化を優先。
//  ・hold note は本 Phase で「先頭タップだけを通常 note と同じく判定する」
//    扱い (release 判定 / 持続中スコアは未実装)。BeatNote::hold_duration_sec
//    は情報として保持するのみ。
//  ・EndCallback は「全 note が judged=true になった次 Tick」で 1 度だけ発火
//    する。Tap 内で最後の 1 つを判定した瞬間に直接呼ぶと、callback 内で
//    Tap が再帰される懸念があるため Tick 起因に統一。
//  ・Tick での Miss 検出は good_window を 1 frame でも過ぎたら確定。dt が
//    大きく複数 note を一括で Miss する場合も同一 Tick 内で全て発火する。
//  ・Pause 中の Tap は受理する (current_time は止まっているので判定結果は
//    pause 直前と同じ)。Stop 中の Tap は再生していないため Judgement::Miss
//    (該当 note 無し扱い) を返すのみ。
#include "gameframework/BeatGrid.h"

namespace acs::game {

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------

f32 BeatGrid::MsToSec(f32 ms) noexcept {
    if (ms < 0.0f) return 0.0f;
    return ms * 0.001f;
}

Judgement BeatGrid::ClassifyDelta(f32 abs_delta_sec) const noexcept {
    if (abs_delta_sec <= _perfect_window_sec) return Judgement::Perfect;
    if (abs_delta_sec <= _great_window_sec)   return Judgement::Great;
    if (abs_delta_sec <= _good_window_sec)    return Judgement::Good;
    return Judgement::Miss;
}

usize BeatGrid::FindNearestNote(BeatLane lane) const noexcept {
    const usize n = _notes.Size();
    usize best_idx = n;            // npos
    f32   best_abs = _good_window_sec; // good_window を超える note は対象外

    for (usize i = 0; i < n; ++i) {
        if (_judged[i]) continue;
        const BeatNote& note = _notes[i];
        if (note.lane != lane) continue;
        f32 delta = note.time_sec - _current_time;
        if (delta < 0.0f) delta = -delta;
        if (delta <= best_abs) {
            best_abs = delta;
            best_idx = i;
        }
    }
    return best_idx;
}

void BeatGrid::ApplyJudgement(BeatLane lane, Judgement j) noexcept {
    switch (j) {
        case Judgement::Perfect: ++_perfect_count; ++_current_combo; break;
        case Judgement::Great:   ++_great_count;   ++_current_combo; break;
        case Judgement::Good:    ++_good_count;    ++_current_combo; break;
        case Judgement::Miss:    ++_miss_count;    _current_combo = 0u; break;
    }
    if (_current_combo > _max_combo) _max_combo = _current_combo;

    if (_judge_cb != nullptr) {
        _judge_cb(_judge_user, lane, j, _current_combo);
    }
}

// ----------------------------------------------------------------------------
// 初期化 / 読込
// ----------------------------------------------------------------------------

void BeatGrid::Init() noexcept {
    // 譜面 / callback は保持し、再生状態 / 統計のみリセット。
    // (callback は呼出側が SetOn* で別途設定済みの想定なので維持する。
    //  譜面は LoadChart で都度上書きするため Init 単体ではクリアしない方が
    //  「Init → LoadChart → Start」の素直なフローを邪魔しない。)
    _current_time   = 0.0f;
    _playing        = false;
    _paused         = false;
    _ended_fired    = false;
    _perfect_count  = 0u;
    _great_count    = 0u;
    _good_count     = 0u;
    _miss_count     = 0u;
    _current_combo  = 0u;
    _max_combo      = 0u;

    // judged flag は既存譜面に対して全 false 化する (= 再判定可能状態)。
    const usize n = _judged.Size();
    for (usize i = 0; i < n; ++i) _judged[i] = false;
}

void BeatGrid::LoadChart(const BeatNote* notes, u32 count, f32 bpm) noexcept {
    // 既存譜面を破棄
    _notes.Clear();
    _judged.Clear();

    _bpm = (bpm < 0.0f) ? 0.0f : bpm;

    if (notes != nullptr && count > 0u) {
        _notes.Reserve(count);
        _judged.Reserve(count);
        for (u32 i = 0; i < count; ++i) {
            BeatNote n = notes[i];
            if (n.hold_duration_sec < 0.0f) n.hold_duration_sec = 0.0f;
            _notes.PushBack(n);
            _judged.PushBack(false);
        }
    }
    _total_notes = static_cast<u32>(_notes.Size());

    // 統計 / 状態リセット
    _current_time   = 0.0f;
    _playing        = false;
    _paused         = false;
    _ended_fired    = false;
    _perfect_count  = 0u;
    _great_count    = 0u;
    _good_count     = 0u;
    _miss_count     = 0u;
    _current_combo  = 0u;
    _max_combo      = 0u;
}

void BeatGrid::SetTimingWindows(f32 perfect_ms, f32 great_ms, f32 good_ms) noexcept {
    f32 p = MsToSec(perfect_ms);
    f32 g = MsToSec(great_ms);
    f32 d = MsToSec(good_ms);
    // 順序逆転は内部で整形 (= perfect <= great <= good を保証)
    if (g < p) g = p;
    if (d < g) d = g;
    _perfect_window_sec = p;
    _great_window_sec   = g;
    _good_window_sec    = d;
}

// ----------------------------------------------------------------------------
// 再生制御
// ----------------------------------------------------------------------------

void BeatGrid::Start() noexcept {
    _current_time = 0.0f;
    _playing      = true;
    _paused       = false;
    _ended_fired  = false;
    // judged flag を再判定可能状態に戻す + 統計クリア (= 再挑戦)
    const usize n = _judged.Size();
    for (usize i = 0; i < n; ++i) _judged[i] = false;
    _perfect_count = 0u;
    _great_count   = 0u;
    _good_count    = 0u;
    _miss_count    = 0u;
    _current_combo = 0u;
    _max_combo     = 0u;
}

void BeatGrid::Stop() noexcept {
    _playing      = false;
    _paused       = false;
    _current_time = 0.0f;
    // 統計 / judged は呼出側がスコア表示等に使う可能性があるため維持する。
    // ClearAll で完全クリア。
}

void BeatGrid::Pause() noexcept {
    if (_playing) _paused = true;
}

void BeatGrid::Resume() noexcept {
    if (_playing) _paused = false;
}

// ----------------------------------------------------------------------------
// 入力 / 駆動
// ----------------------------------------------------------------------------

Judgement BeatGrid::Tap(BeatLane lane) noexcept {
    // 停止中は判定対象なし
    if (!_playing) return Judgement::Miss;

    const usize idx = FindNearestNote(lane);
    if (idx >= _notes.Size()) {
        // good_window 内に該当 note なし: caller への通知は Miss だが
        // 統計には影響させない (空打ち)。
        return Judgement::Miss;
    }

    const BeatNote& note = _notes[idx];
    f32 delta = note.time_sec - _current_time;
    if (delta < 0.0f) delta = -delta;
    const Judgement j = ClassifyDelta(delta);

    // ClassifyDelta は FindNearestNote の best_abs <= good_window 制約により
    // ここで Miss にはならないはずだが、defense-in-depth で再確認。
    if (j == Judgement::Miss) return Judgement::Miss;

    _judged[idx] = true;
    ApplyJudgement(lane, j);
    return j;
}

void BeatGrid::Tick(f32 dt) noexcept {
    if (!_playing) return;
    if (_paused)   return;
    if (dt <= 0.0f) return;

    _current_time += dt;

    // good_window を 1 frame でも過ぎた note を Miss として確定。
    // 一括処理: 大 dt や譜面後半に未判定が残ったまま停止 → 再生再開の場合も
    // 同一 Tick で全部捌く。
    const usize n = _notes.Size();
    const f32 miss_threshold = _current_time - _good_window_sec;
    for (usize i = 0; i < n; ++i) {
        if (_judged[i]) continue;
        if (_notes[i].time_sec < miss_threshold) {
            _judged[i] = true;
            ApplyJudgement(_notes[i].lane, Judgement::Miss);
        }
    }

    // 全 note 判定完了 → 一度だけ EndCallback。
    if (!_ended_fired) {
        u32 judged_count = 0u;
        for (usize i = 0; i < n; ++i) {
            if (_judged[i]) ++judged_count;
        }
        if (judged_count == _total_notes) {
            _ended_fired = true;
            if (_end_cb != nullptr) {
                _end_cb(_end_user, HitNotes(), _miss_count, Accuracy());
            }
        }
    }
}

// ----------------------------------------------------------------------------
// 統計
// ----------------------------------------------------------------------------

f32 BeatGrid::Accuracy() const noexcept {
    if (_total_notes == 0u) return 1.0f; // 空譜面 = 満点扱い (divide-by-zero 回避)
    const f32 weighted =
        static_cast<f32>(_perfect_count) * 1.0f +
        static_cast<f32>(_great_count)   * 0.8f +
        static_cast<f32>(_good_count)    * 0.5f;
    return weighted / static_cast<f32>(_total_notes);
}

// ----------------------------------------------------------------------------
// 全リセット
// ----------------------------------------------------------------------------

void BeatGrid::ClearAll() noexcept {
    _notes.Clear();
    _judged.Clear();
    _bpm                = 0.0f;
    _perfect_window_sec = 0.025f;
    _great_window_sec   = 0.050f;
    _good_window_sec    = 0.100f;
    _current_time       = 0.0f;
    _playing            = false;
    _paused             = false;
    _ended_fired        = false;
    _total_notes        = 0u;
    _perfect_count      = 0u;
    _great_count        = 0u;
    _good_count         = 0u;
    _miss_count         = 0u;
    _current_combo      = 0u;
    _max_combo          = 0u;
    _judge_cb           = nullptr;
    _judge_user         = nullptr;
    _end_cb             = nullptr;
    _end_user           = nullptr;
}

} // namespace acs::game
