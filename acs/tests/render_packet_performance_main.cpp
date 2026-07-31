// SPDX-License-Identifier: Apache-2.0
#include "render/SpriteSortList.h"

#include <chrono>
#include <cstdio>

namespace {

using namespace acs;

/** 整列結果がlayer、depth、提出順の契約を満たすか調べる。 */
bool ValidateOrder(const FSpriteSortList& list) noexcept
{
    for (u32 index = 1u; index < list.Count(); ++index) {
        /** 一つ前の整列済みcommand。 */
        const FSpriteCmd& previous = list.Ordered(index - 1u);
        /** 現在の整列済みcommand。 */
        const FSpriteCmd& current = list.Ordered(index);
        if (previous.layer > current.layer) return false;
        if (previous.layer == current.layer && previous.depth > current.depth) return false;
        if (previous.layer == current.layer && previous.depth == current.depth && previous.color.x >= current.color.x) return false;
    }
    return true;
}

} // namespace

/** 描画packet整列の決定的な作業量と補助時間をJSONで報告する。 */
int main()
{
    using namespace acs;
    /** 大量描画を再現するcommand数。 */
    constexpr u32 kCommandCount = 16'384u;
    /** 従来の逆順挿入sortが行う比較回数。 */
    constexpr u64 kBaselineComparisons = static_cast<u64>(kCommandCount) * static_cast<u64>(kCommandCount - 1u) / 2u;

    FSpriteSortList list;
    list.Reserve(kCommandCount);
    for (u32 index = 0u; index < kCommandCount; ++index) {
        /** 従来挿入sortの最大比較数を正確に再現する逆順layer。 */
        const i32 layer = static_cast<i32>(kCommandCount - index);
        /** layer比較だけを分離する固定depth。 */
        constexpr f32 depth = 0.0f;
        list.SubmitRect(0.0f, 0.0f, 1.0f, 1.0f, FVec4{static_cast<f32>(index), 0.0f, 0.0f, 1.0f}, layer, depth);
    }

    /** radix sort計測の開始時刻。 */
    const auto begin = std::chrono::steady_clock::now();
    list.Sort();
    /** radix sort計測の終了時刻。 */
    const auto end = std::chrono::steady_clock::now();
    /** 最適化後の決定的なitem走査数。 */
    const u64 optimized_visits = list.LastSortItemVisits();
    /** 実時間の補助診断値。 */
    const u64 elapsed_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
    /** 走査削減率。 */
    const double reduction_percent = 100.0 * (1.0 - static_cast<double>(optimized_visits) / static_cast<double>(kBaselineComparisons));
    /** 順序と線形上限が成立した場合はtrue。 */
    const bool ok = list.Count() == kCommandCount && ValidateOrder(list) && list.LastSortPassCount() > 0u && list.LastSortPassCount() <= 8u && optimized_visits <= static_cast<u64>(kCommandCount) * 18u && optimized_visits < kBaselineComparisons;

    std::printf("{\"schema\":1,\"status\":\"%s\",\"commands\":%u,\"baseline_insertion_comparisons\":%llu,\"optimized_item_visits\":%llu,\"radix_passes\":%u,\"work_reduction_percent\":%.3f,\"timing_diagnostic_ns\":%llu}\n", ok ? "pass" : "fail", kCommandCount, static_cast<unsigned long long>(kBaselineComparisons), static_cast<unsigned long long>(optimized_visits), list.LastSortPassCount(), reduction_percent, static_cast<unsigned long long>(elapsed_ns));
    return ok ? 0 : 1;
}
