// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar D — FInputRecorder 実装 (Phase D-1 スケルトン)
//
// 現フェーズ:
//   ・StartRecording / StopRecording / StartReplay / StopReplay /
//     Capture / ConsumeSample / SampleCount / Clear はすべて本実装。
//   ・SaveToBuffer / LoadFromBuffer は ACS_ERR(IO, kSub_NotImplemented) を返す
//     stub (FLockstep Phase M-1 / FSaveSlot Phase 1 と同じ pattern)。Phase D-2 で
//     `.acsr` の bit-precise file layout に対応する実 I/O を接続予定。
//
// 設計メモ:
//   ・Mode 遷移は明示的: Idle → (StartRecording) → Recording → (StopRecording) → Idle
//                       Idle → (StartReplay)    → Replaying → (StopReplay)    → Idle
//     Recording 中に StartReplay を呼ぶと黙って Replaying に切り替わる (cursor
//     はリセット)。逆も同様。録画と再生が同時並行することはない。
//   ・ConsumeSample は m_Cursor から線形走査。記録順 = tick 昇順を仮定すれば
//     cursor 前進で amortised O(1)。FLockstep::ConsumeInput と同じ pattern。
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

void FInputRecorder::StartRecording(u32 tick_rate_hz) noexcept {
    m_Mode = ERecorderMode::Recording;
    // tick_rate_hz == 0 は意味を成さないので最低 1 に丸める。0 除算防止 +
    // FLockstep::Init と同じ規約。
    m_TickRateHz = (tick_rate_hz == 0) ? 1u : tick_rate_hz;
    m_CurrentTick = 0;
    m_Cursor       = 0;
    // 既存 samples は破棄しない (続きから録画したい場合の許容)。
    // 完全リセットしたい場合は呼び出し側で Clear() を併用する。
}

void FInputRecorder::StopRecording() noexcept {
    // Idle に戻すだけで samples / tick_rate_hz は保持。直後に SaveToBuffer を
    // 呼ぶ想定。Capture/ConsumeSample 双方の no-op 条件を兼ねる。
    m_Mode = ERecorderMode::Idle;
}

// -----------------------------------------------------------------------------
// 再生開始 / 停止
// -----------------------------------------------------------------------------

void FInputRecorder::StartReplay() noexcept {
    m_Mode         = ERecorderMode::Replaying;
    m_Cursor       = 0;
    m_CurrentTick = 0;
    // m_Samples はそのまま。直前に Capture した内容を頭から再生する想定。
}

void FInputRecorder::StopReplay() noexcept {
    // Idle に戻すだけ。再度 StartReplay すれば cursor は 0 から再開する。
    m_Mode = ERecorderMode::Idle;
}

// -----------------------------------------------------------------------------
// Capture (録画中の入力受け取り)
// -----------------------------------------------------------------------------

void FInputRecorder::Capture(const InputSample& s) noexcept {
    // Recording モード以外では記録しない (Idle / Replaying 中の誤呼び出しを許容)。
    // 黙って no-op にする理由: ゲームループから無条件に Capture を呼べる設計に
    // しておくと、上位層の if 分岐が不要になり録画開始/停止だけで切り替えできる。
    if (m_Mode != ERecorderMode::Recording) {
        return;
    }
    m_Samples.PushBack(s);
    // m_CurrentTick は「次に書き込む tick」のヒント。連続 tick 想定で
    // sample.tick + 1 に進める。FLockstep::RecordInput と同じ方針。
    m_CurrentTick = s.tick + 1u;
}

// -----------------------------------------------------------------------------
// ConsumeSample (再生中の入力取り出し)
// -----------------------------------------------------------------------------

bool FInputRecorder::ConsumeSample(u32 tick, InputSample& out) noexcept {
    // Replaying モード以外では取り出しを禁止する (誤用検知)。
    if (m_Mode != ERecorderMode::Replaying) {
        return false;
    }
    const usize n = m_Samples.Size();
    // m_Cursor から線形走査。記録順 = tick 昇順を仮定するため、
    // ヒット後に cursor を前進させて amortised O(1) を狙う。
    for (usize i = m_Cursor; i < n; ++i) {
        const InputSample& s = m_Samples[i];
        if (s.tick == tick) {
            out = s;
            // 次回検索開始位置を更新。FLockstep と異なり同 tick 内に複数 sample が
            // 入る想定はない (1 tick = 1 sample = 1 raw input snapshot) ため、
            // 単純に i + 1 を採用する。
            m_Cursor       = static_cast<u32>(i + 1u);
            m_CurrentTick = tick + 1u;
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

u32 FInputRecorder::SampleCount() const noexcept {
    return static_cast<u32>(m_Samples.Size());
}

void FInputRecorder::Clear() noexcept {
    m_Samples.Clear();
    m_CurrentTick = 0;
    m_Cursor       = 0;
    // m_Mode / m_TickRateHz は保持。StartRecording / StartReplay で改めて切り替える設計。
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
TResult<void> FInputRecorder::SaveToBuffer(u8* buffer, u32 size, u32& out_written) noexcept {
    out_written = 0;
    if (buffer == nullptr) {
        return ACS_ERR(IO, kSub_NullBuffer,
                       "FInputRecorder::SaveToBuffer: buffer is null");
    }
    (void)size;
    // Phase 1: 実 I/O は未接続。"動くが必ず失敗する" stub にしておき、
    // 呼び出し側 (replay 保存 UI / テスト) が TResult を握りつぶさない設計を強制する。
    return ACS_ERR(IO, kSub_NotImplemented,
                   "FInputRecorder::SaveToBuffer is not yet implemented (Phase D-1 stub)");
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
//   6. m_Samples.Clear(); m_Samples.Reserve(sample_count); 順次 PushBack
//   7. m_TickRateHz / m_CurrentTick を 0 にリセット (StartReplay 待ち状態)
TResult<void> FInputRecorder::LoadFromBuffer(const u8* buffer, u32 size) noexcept {
    if (buffer == nullptr) {
        return ACS_ERR(IO, kSub_NullBuffer,
                       "FInputRecorder::LoadFromBuffer: buffer is null");
    }
    (void)size;
    return ACS_ERR(IO, kSub_NotImplemented,
                   "FInputRecorder::LoadFromBuffer is not yet implemented (Phase D-1 stub)");
}

} // namespace acs::game
