// SPDX-License-Identifier: Apache-2.0
// GameFramework meta — CPerfBudget 実装
//
// state holder のみ。計測 (タイマ / FAllocator hook) は呼出し側責務。詳細は
// CPerfBudget.h 参照。
#include "gameframework/PerfBudget.h"

#include <cstring>   // strcmp

namespace acs::game {

usize CPerfBudget::FindCategoryIndex(const char* category) const noexcept {
    if (category == nullptr) return m_Categories.Size();
    // pointer 同一 → strcmp の順。リテラル運用なら pointer 同一の高速 path で抜ける。
    for (usize i = 0; i < m_Categories.Size(); ++i) {
        const char* existing = m_Categories[i].category;
        if (existing == category) return i;
        if (existing != nullptr && ::strcmp(existing, category) == 0) return i;
    }
    return m_Categories.Size();
}

void CPerfBudget::SetFrameBudget(f32 ms) noexcept {
    // 負値 / NaN を 0 (= 判定無効) に正規化。
    // NaN は自分自身と比較できない性質を使って弾く。
    if (!(ms > 0.0f)) {
        m_FrameBudgetMs = 0.0f;
    } else {
        m_FrameBudgetMs = ms;
    }
}

void CPerfBudget::DefineCategory(const char* category, f32 budget_ms, u32 budget_bytes) noexcept {
    if (category == nullptr) return;
    // budget 側は負値を 0 に clamp (上限 0 = 「常に超過扱い」になるが、それも仕様)。
    const f32 safe_budget_ms = (budget_ms > 0.0f) ? budget_ms : 0.0f;

    const usize idx = FindCategoryIndex(category);
    if (idx < m_Categories.Size()) {
        // 既存上書き: budget_* のみ差し替え、spent_* は保持 (計測継続のため)。
        m_Categories[idx].budget_ms    = safe_budget_ms;
        m_Categories[idx].budget_bytes = budget_bytes;
        return;
    }
    FBudgetEntry e;
    e.category     = category;
    e.spent_ms     = 0.0f;
    e.budget_ms    = safe_budget_ms;
    e.spent_bytes  = 0u;
    e.budget_bytes = budget_bytes;
    m_Categories.PushBack(e);
}

void CPerfBudget::RecordTimeMs(const char* category, f32 elapsed_ms) noexcept {
    if (category == nullptr) return;
    // 負値 / NaN は無視 (NaN は自身との比較が false なので !(>0) で弾ける)。
    if (!(elapsed_ms > 0.0f)) return;
    const usize idx = FindCategoryIndex(category);
    if (idx >= m_Categories.Size()) return;  // 未定義 category は no-op
    m_Categories[idx].spent_ms += elapsed_ms;
}

void CPerfBudget::RecordMemoryAlloc(const char* category, u32 bytes) noexcept {
    if (category == nullptr || bytes == 0u) return;
    const usize idx = FindCategoryIndex(category);
    if (idx >= m_Categories.Size()) return;
    // u32 overflow ガード: 加算前に上限 (UINT32_MAX) を超えないかチェック。
    FBudgetEntry& e = m_Categories[idx];
    const u32 remain = static_cast<u32>(0xFFFFFFFFu) - e.spent_bytes;
    if (bytes >= remain) {
        e.spent_bytes = 0xFFFFFFFFu;  // 飽和加算
    } else {
        e.spent_bytes += bytes;
    }
}

void CPerfBudget::RecordMemoryFree(const char* category, u32 bytes) noexcept {
    if (category == nullptr || bytes == 0u) return;
    const usize idx = FindCategoryIndex(category);
    if (idx >= m_Categories.Size()) return;
    FBudgetEntry& e = m_Categories[idx];
    // unsigned underflow を避けるため clamp。alloc/free 不整合時の保護。
    if (bytes >= e.spent_bytes) {
        e.spent_bytes = 0u;
    } else {
        e.spent_bytes -= bytes;
    }
}

void CPerfBudget::BeginFrame() noexcept {
    // spent_ms のみ 0 にリセット。spent_bytes は累積保持 (現在保持中の量)。
    for (usize i = 0; i < m_Categories.Size(); ++i) {
        m_Categories[i].spent_ms = 0.0f;
    }
}

void CPerfBudget::EndFrame() noexcept {
    // 全 category の spent_ms 合計を取り、frame 履歴に push。
    f32 total = 0.0f;
    for (usize i = 0; i < m_Categories.Size(); ++i) {
        total += m_Categories[i].spent_ms;
    }
    m_LastFrameMs = total;

    // frame_budget_ms <= 0 (= 未設定) なら超過判定無効。
    if (m_FrameBudgetMs > 0.0f) {
        m_FrameOverBudget = (total > m_FrameBudgetMs);
    } else {
        m_FrameOverBudget = false;
    }

    // 循環バッファ書き込み (CDebugOverlay と同じパターン)。
    if (m_FrameHistory.Capacity() < kFrameHistoryCap) {
        m_FrameHistory.Reserve(kFrameHistoryCap);
    }
    if (!m_bFrameFilled) {
        m_FrameHistory.PushBack(total);
        ++m_FrameIndex;
        if (m_FrameIndex >= kFrameHistoryCap) {
            m_FrameIndex  = 0u;
            m_bFrameFilled = true;
        }
    } else {
        m_FrameHistory[m_FrameIndex] = total;
        ++m_FrameIndex;
        if (m_FrameIndex >= kFrameHistoryCap) m_FrameIndex = 0u;
    }
}

bool CPerfBudget::IsOverBudget(const char* category) const noexcept {
    if (category == nullptr) return false;
    const usize idx = FindCategoryIndex(category);
    if (idx >= m_Categories.Size()) return false;
    const FBudgetEntry& e = m_Categories[idx];
    // budget_ms > 0 のときのみ ms 超過を判定 (0 = 無効上限)。
    const bool ms_over    = (e.budget_ms    > 0.0f) && (e.spent_ms    > e.budget_ms);
    const bool bytes_over = (e.budget_bytes > 0u  ) && (e.spent_bytes > e.budget_bytes);
    return ms_over || bytes_over;
}

f32 CPerfBudget::AverageFrameMs() const noexcept {
    const usize n = m_FrameHistory.Size();
    if (n == 0u) return 0.0f;
    f32 sum = 0.0f;
    for (usize i = 0; i < n; ++i) sum += m_FrameHistory[i];
    return sum / static_cast<f32>(n);
}

u32 CPerfBudget::CategoryCount() const noexcept {
    return static_cast<u32>(m_Categories.Size());
}

const FBudgetEntry* CPerfBudget::AllCategories(u32& out_count) const noexcept {
    out_count = static_cast<u32>(m_Categories.Size());
    if (m_Categories.Size() == 0u) return nullptr;
    return m_Categories.Data();
}

void CPerfBudget::Reset() noexcept {
    m_Categories.Clear();
    m_FrameHistory.Clear();
    m_FrameIndex        = 0u;
    m_bFrameFilled       = false;
    m_LastFrameMs      = 0.0f;
    m_FrameOverBudget  = false;
    // m_FrameBudgetMs は意図的に保持 (頻繁に再設定する想定がない)。
}

} // namespace acs::game
