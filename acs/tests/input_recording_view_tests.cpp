// SPDX-License-Identifier: Apache-2.0
// immutable input recording view と fail-closed span のテスト。
#include "test/Test.h"
#include "test/Expect.h"

#include "container/Span.h"
#include "gameframework/InputRecorder.h"
#include "gameframework/InputRecordingView.h"

using namespace acs;
using namespace acs::game;

ACS_TEST(InputRecordingView, DecodesValidatedRecordingWithoutOwningBytes)
{
    /** view 用の正常な記録を生成する recorder。 */
    FInputRecorder recorder;
    recorder.StartRecording(120u);
    /** field parity を検査する一件の sample。 */
    FInputSample source;
    source.tick = 7u;
    source.key_codes_changed[0] = 65u;
    source.key_states[0] = 1u;
    source.mouse_pos = FVec2{12.5f, -4.25f};
    source.mouse_button_states = 3u;
    recorder.Capture(source);

    /** `.acsr` を保持する caller-owned buffer。 */
    u8 bytes[128] = {};
    /** serializer が書いた byte 数。 */
    u32 written = 0u;
    EXPECT_TRUE(recorder.SaveToBuffer(bytes, sizeof(bytes), written).IsOk());

    /** immutable zero-copy decode の結果。 */
    TResult<FInputRecordingView> decoded = FInputRecordingView::Decode(TSpan<const u8>(bytes, written));
    EXPECT_TRUE(decoded.IsOk());
    if (decoded.IsErr()) return;
    EXPECT_EQ(decoded.Value().TickRateHz(), 120u);
    EXPECT_EQ(decoded.Value().SampleCount(), 1u);

    /** 復元後の field parity を受ける sample。 */
    FInputSample restored;
    EXPECT_TRUE(decoded.Value().DecodeSample(0u, restored));
    EXPECT_EQ(restored.tick, 7u);
    EXPECT_EQ(restored.key_codes_changed[0], static_cast<u8>(65u));
    EXPECT_EQ(restored.key_states[0], static_cast<u8>(1u));
    EXPECT_EQ(restored.mouse_pos.x, 12.5f);
    EXPECT_EQ(restored.mouse_pos.y, -4.25f);
    EXPECT_EQ(restored.mouse_button_states, static_cast<u8>(3u));
    EXPECT_TRUE(!decoded.Value().DecodeSample(1u, restored));
}

ACS_TEST(InputRecordingView, RejectsTruncationAndCorruptionFailClosed)
{
    /** 最小の正常記録を生成する recorder。 */
    FInputRecorder recorder;
    recorder.StartRecording(60u);
    /** CRC 対象を持たせる sample。 */
    FInputSample sample;
    recorder.Capture(sample);
    /** 破損試験用 buffer。 */
    u8 bytes[128] = {};
    /** serializer が書いた byte 数。 */
    u32 written = 0u;
    EXPECT_TRUE(recorder.SaveToBuffer(bytes, sizeof(bytes), written).IsOk());

    EXPECT_TRUE(FInputRecordingView::Decode(TSpan<const u8>(bytes, written - 1u)).IsErr());
    bytes[16u] ^= 1u;
    /** CRC 不一致の decode 結果。 */
    TResult<FInputRecordingView> corrupt = FInputRecordingView::Decode(TSpan<const u8>(bytes, written));
    EXPECT_TRUE(corrupt.IsErr());
    if (corrupt.IsErr()) EXPECT_EQ(corrupt.Error().subcode, static_cast<u16>(FInputRecorder::kSub_BadCrc));
}

ACS_TEST(InputRecordingView, SpanTrySubSpanPreservesOutputOnInvalidRange)
{
    /** 有効な backing storage。 */
    u8 storage[4] = {1u, 2u, 3u, 4u};
    /** 範囲検査する全体 view。 */
    const TSpan<const u8> source(storage, 4u);
    /** 失敗時に維持される sentinel view。 */
    TSpan<const u8> output(storage + 1u, 1u);
    EXPECT_TRUE(!source.TrySubSpan(3u, 2u, output));
    EXPECT_TRUE(output.Data() == storage + 1u);
    EXPECT_EQ(output.Size(), static_cast<usize>(1u));

    /** null と非 zero size の不正 view。 */
    const TSpan<const u8> invalid(nullptr, 4u);
    EXPECT_TRUE(!invalid.TrySubSpan(0u, 1u, output));
}
