// SPDX-License-Identifier: Apache-2.0

#include "render/RenderGraphTransientAlias.h"

namespace acs {
namespace {

/** lhs が決定的な解析順で rhs より前か返す。 */
bool IsEarlier(const FRenderGraphResourceLifetime& lhs, const FRenderGraphResourceLifetime& rhs) noexcept {
    if (lhs.first_pass != rhs.first_pass) {
        return lhs.first_pass < rhs.first_pass;
    }
    return lhs.resource_id < rhs.resource_id;
}

} // namespace

bool FRenderGraphTransientAliasPlanner::CanAlias(const FRenderGraphResourceLifetime& first, const FRenderGraphResourceLifetime& second) noexcept {
    if (first.resource_id == second.resource_id || first.size_bytes == 0 || second.size_bytes == 0 || first.first_pass > first.last_pass || second.first_pass > second.last_pass || !first.transient || !second.transient || !first.alias_allowed || !second.alias_allowed || first.compatibility_key != second.compatibility_key) {
        return false;
    }

    // 両端を含むため同じ pass に触れる場合は重複する。
    return first.last_pass < second.first_pass || second.last_pass < first.first_pass;
}

bool FRenderGraphTransientAliasPlanner::Build(const FRenderGraphResourceLifetime* lifetimes, usize count) noexcept {
    Reset();
    if (count == 0) {
        return true;
    }
    if (!lifetimes || count > static_cast<usize>(~static_cast<u32>(0))) {
        return false;
    }

    // 入力全体を先に検証し、部分計画を公開しない。
    for (usize input_index = 0; input_index < count; ++input_index) {
        // 検証中の寿命。
        const FRenderGraphResourceLifetime& lifetime = lifetimes[input_index];
        if (lifetime.size_bytes == 0 || lifetime.first_pass > lifetime.last_pass) {
            return false;
        }
        for (usize previous_index = 0; previous_index < input_index; ++previous_index) {
            if (lifetimes[previous_index].resource_id == lifetime.resource_id) {
                return false;
            }
        }
    }
    if (!m_Assignments.TryResize(count) || !m_Order.TryResize(count) || !m_SlotLastPass.TryResize(count) || !m_SlotCompatibility.TryResize(count) || !m_SlotBytes.TryResize(count) || !m_SlotReusable.TryResize(count)) {
        Reset();
        return false;
    }

    // 入力 index を pass と resource id の順へ安定化する。
    for (usize input_index = 0; input_index < count; ++input_index) {
        m_Order[input_index] = static_cast<u32>(input_index);
    }
    for (usize order_index = 1; order_index < count; ++order_index) {
        // 挿入する入力 index。
        const u32 input_index = m_Order[order_index];
        // 挿入位置を後方から探す。
        usize insert_index = order_index;
        while (insert_index > 0) {
            // 直前に並んでいる入力 index。
            const u32 previous_index = m_Order[insert_index - 1];
            if (!IsEarlier(lifetimes[input_index], lifetimes[previous_index])) {
                break;
            }
            m_Order[insert_index] = previous_index;
            --insert_index;
        }
        m_Order[insert_index] = input_index;
    }

    // 次に未使用となる slot 番号。
    u32 slot_count = 0;
    for (usize rank = 0; rank < count; ++rank) {
        // この順位に対応する入力位置。
        const usize input_index = m_Order[rank];
        // 現在割り当てる寿命。
        const FRenderGraphResourceLifetime& lifetime = lifetimes[input_index];
        // 再利用できなければ新規 slot を選ぶ。
        u32 selected_slot = slot_count;
        if (lifetime.transient && lifetime.alias_allowed) {
            for (u32 slot_index = 0; slot_index < slot_count; ++slot_index) {
                if (m_SlotReusable[slot_index] != 0 && m_SlotCompatibility[slot_index] == lifetime.compatibility_key && m_SlotLastPass[slot_index] < lifetime.first_pass) {
                    selected_slot = slot_index;
                    break;
                }
            }
        }
        if (selected_slot == slot_count) {
            m_SlotCompatibility[selected_slot] = lifetime.compatibility_key;
            m_SlotBytes[selected_slot] = lifetime.size_bytes;
            m_SlotReusable[selected_slot] = lifetime.transient && lifetime.alias_allowed ? 1u : 0u;
            ++slot_count;
        } else if (lifetime.size_bytes > m_SlotBytes[selected_slot]) {
            m_SlotBytes[selected_slot] = lifetime.size_bytes;
        }
        m_SlotLastPass[selected_slot] = lifetime.last_pass;
        m_Assignments[input_index] = FRenderGraphAliasAssignment{lifetime.resource_id, selected_slot};
    }

    // alias 前の論理バイト合計。
    u64 logical_bytes = 0;
    // slot ごとの最大値を合計した候補バイト数。
    u64 candidate_heap_bytes = 0;
    for (usize input_index = 0; input_index < count; ++input_index) {
        if (logical_bytes > ~static_cast<u64>(0) - lifetimes[input_index].size_bytes) {
            Reset();
            return false;
        }
        logical_bytes += lifetimes[input_index].size_bytes;
    }
    for (u32 slot_index = 0; slot_index < slot_count; ++slot_index) {
        if (candidate_heap_bytes > ~static_cast<u64>(0) - m_SlotBytes[slot_index]) {
            Reset();
            return false;
        }
        candidate_heap_bytes += m_SlotBytes[slot_index];
    }

    m_Summary = FRenderGraphAliasPlanSummary{static_cast<u32>(count), slot_count, logical_bytes, candidate_heap_bytes};
    return true;
}

void FRenderGraphTransientAliasPlanner::Reset() noexcept {
    m_Assignments.Clear();
    m_Order.Clear();
    m_SlotLastPass.Clear();
    m_SlotCompatibility.Clear();
    m_SlotBytes.Clear();
    m_SlotReusable.Clear();
    m_Summary = {};
}

} // namespace acs
