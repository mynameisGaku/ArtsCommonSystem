// SPDX-License-Identifier: Apache-2.0
// immutable `.acsr` zero-copy decoder 実装。
#include "gameframework/InputRecordingView.h"

#include "gameframework/InputRecordingFormat.h"
#include "gameframework/InputRecorder.h"

namespace acs::game {

/** `.acsr` 全体を検証し、sample 領域だけを借用する view を返す。 */
TResult<FInputRecordingView> FInputRecordingView::Decode(TSpan<const u8> bytes) noexcept
{
    if (bytes.Data() == nullptr) {
        return ACS_ERR(IO, CInputRecorder::kSub_NullBuffer, "FInputRecordingView::Decode: buffer is null");
    }
    if (bytes.Size() < input_recording_detail::kHeaderBytes + input_recording_detail::kFooterBytes) {
        return ACS_ERR(IO, CInputRecorder::kSub_BadSize, "FInputRecordingView::Decode: buffer is smaller than header and footer");
    }

    /** 検証対象の固定長 header。 */
    TSpan<const u8> header;
    if (!bytes.TrySubSpan(0u, input_recording_detail::kHeaderBytes, header)) {
        return ACS_ERR(IO, CInputRecorder::kSub_BadSize, "FInputRecordingView::Decode: header range is invalid");
    }
    if (input_recording_detail::ReadU32(header.Data()) != input_recording_detail::kMagic) {
        return ACS_ERR(IO, CInputRecorder::kSub_BadMagic, "FInputRecordingView::Decode: magic mismatch");
    }
    if (input_recording_detail::ReadU32(header.Data() + 4u) != input_recording_detail::kVersion) {
        return ACS_ERR(IO, CInputRecorder::kSub_BadVersion, "FInputRecordingView::Decode: version mismatch");
    }

    /** header に記録された tick rate。 */
    const u32 tick_rate_hz = input_recording_detail::ReadU32(header.Data() + 8u);
    /** header に記録された sample 件数。 */
    const u32 sample_count = input_recording_detail::ReadU32(header.Data() + 12u);
    if (tick_rate_hz == 0u || tick_rate_hz > kInputRecorderMaximumTickRateHz || sample_count > kInputRecorderMaximumSamples) {
        return ACS_ERR(IO, CInputRecorder::kSub_LimitExceeded, "FInputRecordingView::Decode: metadata exceeds product limits");
    }

    /** overflow しない 64bit で計算した sample byte 数。 */
    const u64 sample_bytes_u64 = static_cast<u64>(sample_count) * static_cast<u64>(input_recording_detail::kSampleBytes);
    /** header/footer 込みの厳密な期待 byte 数。 */
    const u64 expected_bytes = static_cast<u64>(input_recording_detail::kHeaderBytes) + sample_bytes_u64 + static_cast<u64>(input_recording_detail::kFooterBytes);
    if (expected_bytes != static_cast<u64>(bytes.Size())) {
        return ACS_ERR(IO, CInputRecorder::kSub_BadSize, "FInputRecordingView::Decode: sample count and size mismatch");
    }

    /** zero-copy で借用する sample 領域。 */
    TSpan<const u8> samples;
    if (!bytes.TrySubSpan(input_recording_detail::kHeaderBytes, static_cast<usize>(sample_bytes_u64), samples)) {
        return ACS_ERR(IO, CInputRecorder::kSub_BadSize, "FInputRecordingView::Decode: sample range is invalid");
    }
    /** CRC footer 領域。 */
    TSpan<const u8> footer;
    if (!bytes.TrySubSpan(input_recording_detail::kHeaderBytes + samples.Size(), input_recording_detail::kFooterBytes, footer)) {
        return ACS_ERR(IO, CInputRecorder::kSub_BadSize, "FInputRecordingView::Decode: footer range is invalid");
    }
    if (input_recording_detail::ComputeCrc32(samples) != input_recording_detail::ReadU32(footer.Data())) {
        return ACS_ERR(IO, CInputRecorder::kSub_BadCrc, "FInputRecordingView::Decode: CRC32 mismatch");
    }

    for (u32 index = 0u; index < sample_count; ++index) {
        /** 現在検査する sample の先頭。 */
        const u8* sample = samples.Data() + static_cast<usize>(index) * input_recording_detail::kSampleBytes;
        if (!input_recording_detail::HasFiniteMousePosition(sample)) {
            return ACS_ERR(IO, CInputRecorder::kSub_BadValue, "FInputRecordingView::Decode: non-finite mouse position");
        }
    }
    return FInputRecordingView(samples, tick_rate_hz, sample_count);
}

/** 範囲内 sample を field 単位で復元する。 */
bool FInputRecordingView::DecodeSample(u32 index, FInputSample& out) const noexcept
{
    if (index >= m_SampleCount) return false;
    /** 指定 sample の検証済み byte view。 */
    TSpan<const u8> sample;
    if (!m_SampleBytes.TrySubSpan(static_cast<usize>(index) * input_recording_detail::kSampleBytes, input_recording_detail::kSampleBytes, sample)) return false;

    /** 全 field の復元完了まで out を変更しない staging sample。 */
    FInputSample decoded;
    input_recording_detail::ReadSample(sample.Data(), decoded);
    out = decoded;
    return true;
}

} // namespace acs::game
