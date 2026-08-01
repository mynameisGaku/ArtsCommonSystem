// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Span.h"
#include "foundation/Result.h"

namespace acs::game {

struct FInputSample;

/**
 * 検証済み入力記録を所有権なしで参照する immutable decoder。
 *
 * @details Decode は header・上限・厳密サイズ・CRC・有限 float を全件検証する。
 * view の寿命中は呼び出し側が元バッファを生存かつ不変に保つ必要がある。
 */
class FInputRecordingView {
public:
    /**
     * 外部バイト列を全検証して zero-copy view を作る。
     *
     * @param bytes 所有権を借用する `.acsr` 全体。
     * @return 成功時は検証済み view、失敗時は CInputRecorder と同じ subcode。
     */
    static TResult<FInputRecordingView> Decode(TSpan<const u8> bytes) noexcept;

    /**
     * 指定 index の sample を ABI 非依存に復元する。
     *
     * @param index 復元する sample index。
     * @param out 成功時の sample 格納先。
     * @return 範囲内なら true。失敗時は out を変更しない。
     */
    bool DecodeSample(u32 index, FInputSample& out) const noexcept;

    /** 記録時の tick rate を返す。 */
    u32 TickRateHz() const noexcept { return m_TickRateHz; }

    /** 検証済み sample 件数を返す。 */
    u32 SampleCount() const noexcept { return m_SampleCount; }

private:
    /** 検証済み sample 領域と metadata から view を構築する。 */
    FInputRecordingView(TSpan<const u8> sample_bytes, u32 tick_rate_hz, u32 sample_count) noexcept
        : m_SampleBytes(sample_bytes), m_TickRateHz(tick_rate_hz), m_SampleCount(sample_count) {}

    /** 呼び出し側が所有する検証済み sample バイト列。 */
    TSpan<const u8> m_SampleBytes;

    /** 記録時の tick rate。 */
    u32 m_TickRateHz = 0u;

    /** 検証済み sample 件数。 */
    u32 m_SampleCount = 0u;
};

} // namespace acs::game
