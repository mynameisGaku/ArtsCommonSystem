// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar D — InputRecorder 実装 (Phase D-1 スケルトン)
//
// 現フェーズ:
//   ・StartRecording / StopRecording / StartReplay / StopReplay /
//     Capture / ConsumeSample / SampleCount / Clear はすべて本実装。
//   ・SaveToBuffer / LoadFromBuffer は ACS_ERR(IO, kSub_NotImplemented) を返す
//     stub (Lockstep Phase M-1 / SaveSlot Phase 1 と同じ pattern)。Phase D-2 で
//     `.acsr` の bit-precise file layout に対応する実 I/O を接続予定。
//
// 設計メモ:
//   ・Mode 遷移は明示的: Idle → (StartRecording) → Recording → (StopRecording) → Idle
//                       Idle → (StartReplay)    → Replaying → (StopReplay)    → Idle
//     Recording 中に StartReplay を呼ぶと黙って Replaying に切り替わる (cursor
//     はリセット)。逆も同様。録画と再生が同時並行することはない。
//   ・ConsumeSample は _cursor から線形走査。記録順 = tick 昇順を仮定すれば
//     cursor 前進で amortised O(1)。Lockstep::ConsumeInput と同じ pattern。
//   ・key_codes_changed / key_states は struct 定義時にデフォルト初期化済みなので
//     Capture 側は値コピーのみ。InputSample の sizeof は環境依存 (FVec2 = 8 B,
//     padding 込みで 28〜32 B) だが、本クラスは内部で sizeof に依存しないため
//     ABI 変動に耐える。Phase D-2 の file I/O 時は明示的に field を 1 つずつ
//     little-endian で書き出す予定 (`InputSample` 自体の memcpy には頼らない)。
#include "gameframework/InputRecorder.h"

namespace acs::game {

// -----------------------------------------------------------------------------
// 録画開始 / 停止
// -----------------------------------------------------------------------------

void InputRecorder::StartRecording(u32 tick_rate_hz) noexcept {
    _mode = ERecorderMode::Recording;
    // tick_rate_hz == 0 は意味を成さないので最低 1 に丸める。0 除算防止 +
    // Lockstep::Init と同じ規約。
    _tick_rate_hz = (tick_rate_hz == 0) ? 1u : tick_rate_hz;
    _current_tick = 0;
    _cursor       = 0;
    // 既存 samples は破棄しない (続きから録画したい場合の許容)。
    // 完全リセットしたい場合は呼び出し側で Clear() を併用する。
}

void InputRecorder::StopRecording() noexcept {
    // Idle に戻すだけで samples / tick_rate_hz は保持。直後に SaveToBuffer を
    // 呼ぶ想定。Capture/ConsumeSample 双方の no-op 条件を兼ねる。
    _mode = ERecorderMode::Idle;
}

// -----------------------------------------------------------------------------
// 再生開始 / 停止
// -----------------------------------------------------------------------------

void InputRecorder::StartReplay() noexcept {
    _mode         = ERecorderMode::Replaying;
    _cursor       = 0;
    _current_tick = 0;
    // _samples はそのまま。直前に Capture した内容を頭から再生する想定。
}

void InputRecorder::StopReplay() noexcept {
    // Idle に戻すだけ。再度 StartReplay すれば cursor は 0 から再開する。
    _mode = ERecorderMode::Idle;
}

// -----------------------------------------------------------------------------
// Capture (録画中の入力受け取り)
// -----------------------------------------------------------------------------

void InputRecorder::Capture(const InputSample& s) noexcept {
    // Recording モード以外では記録しない (Idle / Replaying 中の誤呼び出しを許容)。
    // 黙って no-op にする理由: ゲームループから無条件に Capture を呼べる設計に
    // しておくと、上位層の if 分岐が不要になり録画開始/停止だけで切り替えできる。
    if (_mode != ERecorderMode::Recording) {
        return;
    }
    _samples.PushBack(s);
    // _current_tick は「次に書き込む tick」のヒント。連続 tick 想定で
    // sample.tick + 1 に進める。Lockstep::RecordInput と同じ方針。
    _current_tick = s.tick + 1u;
}

// -----------------------------------------------------------------------------
// ConsumeSample (再生中の入力取り出し)
// -----------------------------------------------------------------------------

bool InputRecorder::ConsumeSample(u32 tick, InputSample& out) noexcept {
    // Replaying モード以外では取り出しを禁止する (誤用検知)。
    if (_mode != ERecorderMode::Replaying) {
        return false;
    }
    const usize n = _samples.Size();
    // _cursor から線形走査。記録順 = tick 昇順を仮定するため、
    // ヒット後に cursor を前進させて amortised O(1) を狙う。
    for (usize i = _cursor; i < n; ++i) {
        const InputSample& s = _samples[i];
        if (s.tick == tick) {
            out = s;
            // 次回検索開始位置を更新。Lockstep と異なり同 tick 内に複数 sample が
            // 入る想定はない (1 tick = 1 sample = 1 raw input snapshot) ため、
            // 単純に i + 1 を採用する。
            _cursor       = static_cast<u32>(i + 1u);
            _current_tick = tick + 1u;
            return true;
        }
        // 記録順が tick 昇順を破る場合 (rollback / 未来 sample の混入) は素朴に
        // 走査を続ける。Phase D-2 で per-tick index を持つことで高速化予定。
    }
    return false;
}

// -----------------------------------------------------------------------------
// 状態 query / リセット
// -----------------------------------------------------------------------------

u32 InputRecorder::SampleCount() const noexcept {
    return static_cast<u32>(_samples.Size());
}

void InputRecorder::Clear() noexcept {
    _samples.Clear();
    _current_tick = 0;
    _cursor       = 0;
    // _mode / _tick_rate_hz は保持。StartRecording / StartReplay で改めて切り替える設計。
}

// -----------------------------------------------------------------------------
// SaveToBuffer — Phase D-1: stub (NotImplemented を返す)
// -----------------------------------------------------------------------------
// Phase D-2 の擬似コード:
//   1. required = 16 (header) + sample_count * sizeof_on_disk(InputSample) + 4 (footer)
//      ※ sizeof_on_disk は memcpy ではなく field を 1 つずつ little-endian で
//      書き出す前提で 4 + 8 + 8 + 8 + 1 + padding = 32 B 程度を想定。
//   2. size < required なら kSub_BufferTooSmall
//   3. write [magic='ACSR'][version=1][tick_rate_hz][sample_count]
//   4. write samples (field 単位で LE エンコード)
//   5. write crc32(samples) footer
//   6. out_written = required
TResult<void> InputRecorder::SaveToBuffer(u8* buffer, u32 size, u32& out_written) noexcept {
    out_written = 0;
    if (buffer == nullptr) {
        return ACS_ERR(IO, kSub_NullBuffer,
                       "InputRecorder::SaveToBuffer: buffer is null");
    }
    (void)size;
    // Phase 1: 実 I/O は未接続。"動くが必ず失敗する" stub にしておき、
    // 呼び出し側 (replay 保存 UI / テスト) が TResult を握りつぶさない設計を強制する。
    return ACS_ERR(IO, kSub_NotImplemented,
                   "InputRecorder::SaveToBuffer is not yet implemented (Phase D-1 stub)");
}

// -----------------------------------------------------------------------------
// LoadFromBuffer — Phase D-1: stub (NotImplemented を返す)
// -----------------------------------------------------------------------------
// Phase D-2 の擬似コード:
//   1. size < 20 (header + footer 最小) なら kSub_BadSize
//   2. magic 検証 → 'ACSR' と不一致なら kSub_BadMagic
//   3. version 検証 → kVersion と不一致なら kSub_BadVersion
//   4. sample_count * sizeof_on_disk(InputSample) + 20 が size と一致しなければ kSub_BadSize
//   5. crc32 計算 → footer と不一致なら kSub_BadCrc
//   6. _samples.Clear(); _samples.Reserve(sample_count); 順次 PushBack
//   7. _tick_rate_hz / _current_tick を 0 にリセット (StartReplay 待ち状態)
TResult<void> InputRecorder::LoadFromBuffer(const u8* buffer, u32 size) noexcept {
    if (buffer == nullptr) {
        return ACS_ERR(IO, kSub_NullBuffer,
                       "InputRecorder::LoadFromBuffer: buffer is null");
    }
    (void)size;
    return ACS_ERR(IO, kSub_NotImplemented,
                   "InputRecorder::LoadFromBuffer is not yet implemented (Phase D-1 stub)");
}

} // namespace acs::game
