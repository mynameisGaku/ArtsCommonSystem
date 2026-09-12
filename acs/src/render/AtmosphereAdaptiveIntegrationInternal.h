// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_RENDER_ATMOSPHERE_ADAPTIVE_INTEGRATION_INTERNAL_H
#define ACS_RENDER_ATMOSPHERE_ADAPTIVE_INTEGRATION_INTERNAL_H

#include "foundation/Types.h"

namespace acs::render_internal {

/**
 * 非負のRGB総量・推定誤差と区間配列から、未達の色の相対誤差が最大の区間を選ぶ。
 * 分割位置は区間内部、分割不能なら始点を渡す。配列はleaf_count個以上必要。
 * 区間数0、上限到達、全色許容内、候補なしではleaf_countを返す。同率は先頭を保つ。
 */
inline u32 SelectAtmosphereAdaptiveInterval_Internal(const f64 total[3], const f64 total_error[3], const f64 intervals[][2], const f64 errors[][3], const f64 split_positions[], u32 leaf_count, u32 maximum_intervals, f64 integration_tolerance) noexcept {
    if (leaf_count == 0u || leaf_count >= maximum_intervals) return leaf_count;
    // 全色の推定誤差が許容内なら、残り予算があっても分割しない。
    const bool converged = total_error[0] <= integration_tolerance*total[0] && total_error[1] <= integration_tolerance*total[1] && total_error[2] <= integration_tolerance*total[2];
    if (converged) return leaf_count;
    // 候補なしの印と、それまでに見つけた最大の相対誤差。
    u32 selected = leaf_count;
    f64 largest_error = 0.0;
    // 配列順に調べ、同率では先に見つけた区間を残す。
    for (u32 index = 0u; index < leaf_count; ++index) {
        if (split_positions[index] <= intervals[index][0]) continue;
        // 色別の総誤差が許容を超える成分だけへ、残り分割予算を配る。
        for (u32 channel = 0u; channel < 3u; ++channel) {
            if (total_error[channel] <= integration_tolerance*total[channel]) continue;
            // 総量0の色に正の誤差がある場合も、0除算せず候補に残す。
            const f64 priority = total[channel] > 0.0 ? errors[index][channel]/total[channel] : (errors[index][channel] > 0.0 ? 1.0 : 0.0);
            if (priority > largest_error) {
                largest_error = priority;
                selected = index;
            }
        }
    }
    return selected;
}

} // namespace acs::render_internal

#endif
