// SPDX-License-Identifier: Apache-2.0
// tick単位のinput sampleを録画・再生し、`.acsr`形式のbufferへ保存・読込する。
//
// StartRecording / StopRecording / StartReplay / StopReplay /
// Capture / ConsumeSample / SampleCount / Clear / SaveToBuffer / LoadFromBuffer
// をすべて本実装する。SaveToBuffer / LoadFromBuffer は `.acsr` の bit-precise
// file layout に対応する実 I/O。
//
// 設計メモ:
//   ・Mode 遷移は明示的: Idle → (StartRecording) → Recording → (StopRecording) → Idle
//                       Idle → (StartReplay)    → Replaying → (StopReplay)    → Idle
//     Recording 中に StartReplay を呼ぶと黙って Replaying に切り替わる (cursor
//     はリセット)。逆も同様。録画と再生が同時並行することはない。
//   ・ConsumeSample は m_Cursor から線形走査。記録順 = tick 昇順を仮定すれば
//     cursor 前進で amortised O(1)。CLockstep::ConsumeInput と同じ pattern。
//   ・key_codes_changed / key_states は struct 定義時にデフォルト初期化済みなので
//     Capture 側は値コピーのみ。InputSample の sizeof は環境依存 (FVec2 = 8 B,
//     padding 込みで 28〜32 B) だが、本クラスは内部で sizeof に依存しないため
//     ABI 変動に耐える。file I/O 時は明示的に field を 1 つずつ
//     little-endian で書き出す (`InputSample` 自体の memcpy には頼らない)。
//   ・SaveToBuffer / LoadFromBuffer の `.acsr` on-disk
//     layout は header 16B + samples (1 sample = 29B、field 単位 LE) + footer 4B。
//     1 sample = tick(4) + key_codes_changed(8) + key_states(8) + mouse_pos.x(4) +
//     mouse_pos.y(4) + mouse_button_states(1) = 29B。CRC32 は samples 部のみを
//     対象に計算し footer に置く (CSaveArchive / assetpack と同一の
//     poly 0xEDB88320 実装)。InputSample 自体の memcpy には頼らず、ABI 非依存。
#include "gameframework/InputRecorder.h"
#include "gameframework/InputRecordingFormat.h"
#include "gameframework/InputRecordingView.h"

#include "foundation/Move.h"

namespace acs::game {

/** Recording モードへ切り替え、tick / cursor をリセットする (既存 samples は保持)。 */
void CInputRecorder::StartRecording(u32 tick_rate_hz) noexcept {
    m_Mode = ERecorderMode::Recording;
    // tick_rate_hz == 0 は意味を成さないので最低 1 に丸める。0 除算防止 +
    // CLockstep::Init と同じ規約。
    m_TickRateHz = (tick_rate_hz == 0) ? 1u : tick_rate_hz;
    m_CurrentTick = 0;
    m_Cursor       = 0;
    // 既存 samples は破棄しない (続きから録画したい場合の許容)。
    // 完全リセットしたい場合は呼び出し側で Clear() を併用する。
}

/** Idle へ戻す (samples / tick_rate_hz は保持)。 */
void CInputRecorder::StopRecording() noexcept {
    // Idle に戻すだけで samples / tick_rate_hz は保持。直後に SaveToBuffer を
    // 呼ぶ想定。Capture/ConsumeSample 双方の no-op 条件を兼ねる。
    m_Mode = ERecorderMode::Idle;
}

/** Replaying モードへ切り替え、cursor / current_tick を 0 にする (samples は保持)。 */
void CInputRecorder::StartReplay() noexcept {
    m_Mode         = ERecorderMode::Replaying;
    m_Cursor       = 0;
    m_CurrentTick = 0;
    // m_Samples はそのまま。直前に Capture した内容を頭から再生する想定。
}

/** Idle へ戻す (再度 StartReplay すれば cursor は 0 から再開)。 */
void CInputRecorder::StopReplay() noexcept {
    // Idle に戻すだけ。再度 StartReplay すれば cursor は 0 から再開する。
    m_Mode = ERecorderMode::Idle;
}

/** Recording 中のみ sample を末尾に蓄積し、current_tick を s.tick + 1 へ進める。 */
void CInputRecorder::Capture(const FInputSample& s) noexcept {
    // Recording モード以外では記録しない (Idle / Replaying 中の誤呼び出しを許容)。
    // 黙って no-op にする理由: ゲームループから無条件に Capture を呼べる設計に
    // しておくと、上位層の if 分岐が不要になり録画開始/停止だけで切り替えできる。
    if (m_Mode != ERecorderMode::Recording) {
        return;
    }
    m_Samples.Add(s);
    // m_CurrentTick は「次に書き込む tick」のヒント。連続 tick 想定で
    // sample.tick + 1 に進める。CLockstep::RecordInput と同じ方針。
    m_CurrentTick = s.tick + 1u;
}

/** Replaying 中に cursor から線形走査し、指定 tick の sample を取り出す (amortised O(1))。 */
bool CInputRecorder::ConsumeSample(u32 tick, FInputSample& out) noexcept {
    // Replaying モード以外では取り出しを禁止する (誤用検知)。
    if (m_Mode != ERecorderMode::Replaying) {
        return false;
    }
    const usize n = m_Samples.Num();
    // m_Cursor から線形走査。記録順 = tick 昇順を仮定するため、
    // ヒット後に cursor を前進させて amortised O(1) を狙う。
    for (usize i = m_Cursor; i < n; ++i) {
        const FInputSample& s = m_Samples[i];
        if (s.tick == tick) {
            out = s;
            // 次回検索開始位置を更新。CLockstep と異なり同 tick 内に複数 sample が
            // 入る想定はない (1 tick = 1 sample = 1 raw input snapshot) ため、
            // 単純に i + 1 を採用する。
            m_Cursor       = static_cast<u32>(i + 1u);
            m_CurrentTick = tick + 1u;
            return true;
        }
        // 記録順が tick 昇順を破る場合 (rollback / 未来 sample の混入) は素朴に
        // 走査を続ける。per-tick index を持てば高速化できる。
    }
    return false;
}

/** 蓄積済み sample 数を返す。 */
u32 CInputRecorder::SampleCount() const noexcept {
    return static_cast<u32>(m_Samples.Num());
}

/** 全 sample を破棄し cursor / current_tick を 0 に戻す (Mode / tick_rate は保持)。 */
void CInputRecorder::Clear() noexcept {
    m_Samples.Reset();
    m_CurrentTick = 0;
    m_Cursor       = 0;
    // m_Mode / m_TickRateHz は保持。StartRecording / StartReplay で改めて切り替える設計。
}

/**
 * 現在の samples を `.acsr` layout で buffer に書き出す。
 *
 * @details
 * layout: [magic][version][tick_rate_hz][sample_count][samples...][crc32]。必要量は
 * 16 (header) + sample_count * 29 (samples) + 4 (footer)。header → samples (field 単位
 * LE) → samples 部の crc32 footer の順に書き出す。
 * @param buffer 書き込み先バッファ。
 * @param size buffer の容量 (バイト)。
 * @param out_written 実際に書き込んだバイト数の書き込み先。
 * @return 成功なら Ok。buffer が null なら kSub_NullBuffer、容量不足なら kSub_BufferTooSmall。
 */
TResult<void> CInputRecorder::SaveToBuffer(u8* buffer, u32 size, u32& out_written) noexcept {
    out_written = 0;
    if (buffer == nullptr) {
        return ACS_ERR(IO, kSub_NullBuffer,
                       "CInputRecorder::SaveToBuffer: buffer is null");
    }

    if (m_Samples.Num() > kInputRecorderMaximumSamples ||
        m_TickRateHz == 0 || m_TickRateHz > kInputRecorderMaximumTickRateHz) {
        return ACS_ERR(IO, kSub_LimitExceeded,
                       "CInputRecorder::SaveToBuffer: tick rate or sample count exceeds the limit");
    }
    const u32 sample_count = static_cast<u32>(m_Samples.Num());
    for (u32 i = 0; i < sample_count; ++i) {
        if (!input_recording_detail::HasFiniteMousePosition(m_Samples[i])) {
            return ACS_ERR(IO, kSub_BadValue,
                           "CInputRecorder::SaveToBuffer: non-finite mouse position");
        }
    }
    // 必要バイト数 = header + samples + footer。u64 で計算して overflow を避ける。
    const u64 required64 =
        static_cast<u64>(input_recording_detail::kHeaderBytes) +
        static_cast<u64>(sample_count) * static_cast<u64>(input_recording_detail::kSampleBytes) +
        static_cast<u64>(input_recording_detail::kFooterBytes);
    if (required64 > static_cast<u64>(size)) {
        return ACS_ERR(IO, kSub_BufferTooSmall,
                       "CInputRecorder::SaveToBuffer: buffer too small for samples");
    }
    const u32 required = static_cast<u32>(required64);

    // ---- header (16B) -------------------------------------------------------
    input_recording_detail::WriteU32(buffer + 0, input_recording_detail::kMagic);
    input_recording_detail::WriteU32(buffer + 4, input_recording_detail::kVersion);
    input_recording_detail::WriteU32(buffer + 8, m_TickRateHz);
    input_recording_detail::WriteU32(buffer + 12, sample_count);

    // ---- samples (field 単位 LE。1 sample = kSampleBytes バイト) -------------
    u8* samples_begin = buffer + input_recording_detail::kHeaderBytes;
    for (u32 i = 0; i < sample_count; ++i) {
        input_recording_detail::WriteSample(samples_begin + static_cast<u64>(i) * input_recording_detail::kSampleBytes, m_Samples[i]);
    }

    // ---- crc32 footer (samples 部のみを対象) --------------------------------
    const u64 samples_bytes = static_cast<u64>(sample_count) * input_recording_detail::kSampleBytes;
    const u32 crc = input_recording_detail::ComputeCrc32(TSpan<const u8>(samples_begin, static_cast<usize>(samples_bytes)));
    input_recording_detail::WriteU32(samples_begin + samples_bytes, crc);

    out_written = required;
    return Ok();
}

/**
 * `.acsr` buffer を検証して samples を「置換」復元する。
 *
 * @details
 * layout: [magic][version][tick_rate_hz][sample_count][samples...][crc32]。header の
 * magic / version、sample_count とサイズの整合、samples 部の crc32 を順に検証し、すべて
 * 通れば m_Samples を Clear → Reserve → 復元する。完了後は tick_rate を file の値で更新し
 * current_tick / cursor を 0 にリセット (StartReplay 待ち状態)。
 * @param buffer 読み込み元バッファ。
 * @param size buffer のバイト数。
 * @return 成功なら Ok。null は kSub_NullBuffer、サイズ不整合は kSub_BadSize、magic /
 *         version / crc 不一致はそれぞれ kSub_BadMagic / kSub_BadVersion / kSub_BadCrc。
 */
TResult<void> CInputRecorder::LoadFromBuffer(const u8* buffer, u32 size) noexcept {
    return TryLoadFromBuffer(buffer, size);
}

/** 全検証とstaging成功後にだけsamplesを置換するchecked load。 */
TResult<void> CInputRecorder::TryLoadFromBuffer(const u8* buffer, u32 size) noexcept {
    /** allocation 前に全入力を検証する immutable zero-copy view。 */
    TResult<FInputRecordingView> view_result = FInputRecordingView::Decode(TSpan<const u8>(buffer, size));
    if (view_result.IsErr()) return view_result.Error();
    /** 検証済み入力記録 view。 */
    const FInputRecordingView& view = view_result.Value();

    TArray<FInputSample> staged(*m_Samples.GetAllocator());
    if (!staged.TryReserve(static_cast<usize>(view.SampleCount()))) {
        return ACS_ERR(Memory, kSub_Oom,
                       "CInputRecorder::TryLoadFromBuffer: sample staging allocation failed");
    }
    for (u32 index = 0u; index < view.SampleCount(); ++index) {
        /** view から復元する一件分の sample。 */
        FInputSample sample;
        if (!view.DecodeSample(index, sample) || !staged.TryAdd(sample)) {
            return ACS_ERR(Memory, kSub_Oom,
                           "CInputRecorder::TryLoadFromBuffer: sample staging append failed");
        }
    }

    m_Samples = Move(staged);
    m_TickRateHz  = view.TickRateHz();
    m_CurrentTick = 0;
    m_Cursor      = 0;
    return Ok();
}

/** 指定indexのsampleをcursorやmodeへ影響させず複製する。 */
bool CInputRecorder::TryReadSampleAt_Internal(u32 index, FInputSample& output) const noexcept
{
    if (static_cast<usize>(index) >= m_Samples.Num()) return false;
    output = m_Samples[index];
    return true;
}

/** target allocatorを引き継ぐ空のload stagingへ初期化する。 */
void CInputRecorder::PrepareLoadStaging_Internal(CInputRecorder& staging) const noexcept
{
    staging.m_Mode = ERecorderMode::Idle;
    staging.m_TickRateHz = 60u;
    staging.m_CurrentTick = 0u;
    staging.m_Cursor = 0u;
    staging.m_Samples = TArray<FInputSample>(*m_Samples.GetAllocator());
}

/** loaded persistent stateだけをno-fail swapし、modeは各instanceで維持する。 */
void CInputRecorder::SwapLoadedState_Internal(CInputRecorder& other) noexcept
{
    u32 value = m_TickRateHz;
    m_TickRateHz = other.m_TickRateHz;
    other.m_TickRateHz = value;
    value = m_CurrentTick;
    m_CurrentTick = other.m_CurrentTick;
    other.m_CurrentTick = value;
    value = m_Cursor;
    m_Cursor = other.m_Cursor;
    other.m_Cursor = value;
    TArray<FInputSample> samples = Move(m_Samples);
    m_Samples = Move(other.m_Samples);
    other.m_Samples = Move(samples);
}

} // namespace acs::game
