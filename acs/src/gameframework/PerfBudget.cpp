// SPDX-License-Identifier: Apache-2.0
// GameFramework meta — FPerfBudget 実装
//
// state holder のみ。計測 (タイマ / FAllocator hook) は呼出し側責務。詳細は
// FPerfBudget.h 参照。
#include "gameframework/PerfBudget.h"

#include <cstring>   // strcmp

namespace acs::game {

// ============================================================================
// 内部 — category 検索
// ============================================================================

usize FPerfBudget::FindCategoryIndex(const char* category) const noexcept {
    if (category == nullptr) return _categories.Size();
    // pointer 同一 → strcmp の順。リテラル運用なら pointer 同一の高速 path で抜ける。
    for (usize i = 0; i < _categories.Size(); ++i) {
        const char* existing = _categories[i].category;
        if (existing == category) return i;
        if (existing != nullptr && ::strcmp(existing, category) == 0) return i;
    }
    return _categories.Size();
}

// ============================================================================
// セットアップ
// ============================================================================

void FPerfBudget::SetFrameBudget(f32 ms) noexcept {
    // 負値 / NaN を 0 (= 判定無効) に正規化。
    // NaN は自分自身と比較できない性質を使って弾く。
    if (!(ms > 0.0f)) {
        _frame_budget_ms = 0.0f;
    } else {
        _frame_budget_ms = ms;
    }
}

void FPerfBudget::DefineCategory(const char* category, f32 budget_ms, u32 budget_bytes) noexcept {
    if (category == nullptr) return;
    // budget 側は負値を 0 に clamp (上限 0 = 「常に超過扱い」になるが、それも仕様)。
    const f32 safe_budget_ms = (budget_ms > 0.0f) ? budget_ms : 0.0f;

    const usize idx = FindCategoryIndex(category);
    if (idx < _categories.Size()) {
        // 既存上書き: budget_* のみ差し替え、spent_* は保持 (計測継続のため)。
        _categories[idx].budget_ms    = safe_budget_ms;
        _categories[idx].budget_bytes = budget_bytes;
        return;
    }
    BudgetEntry e;
    e.category     = category;
    e.spent_ms     = 0.0f;
    e.budget_ms    = safe_budget_ms;
    e.spent_bytes  = 0u;
    e.budget_bytes = budget_bytes;
    _categories.PushBack(e);
}

// ============================================================================
// 記録
// ============================================================================

void FPerfBudget::RecordTimeMs(const char* category, f32 elapsed_ms) noexcept {
    if (category == nullptr) return;
    // 負値 / NaN は無視 (NaN は自身との比較が false なので !(>0) で弾ける)。
    if (!(elapsed_ms > 0.0f)) return;
    const usize idx = FindCategoryIndex(category);
    if (idx >= _categories.Size()) return;  // 未定義 category は no-op
    _categories[idx].spent_ms += elapsed_ms;
}

void FPerfBudget::RecordMemoryAlloc(const char* category, u32 bytes) noexcept {
    if (category == nullptr || bytes == 0u) return;
    const usize idx = FindCategoryIndex(category);
    if (idx >= _categories.Size()) return;
    // u32 overflow ガード: 加算前に上限 (UINT32_MAX) を超えないかチェック。
    BudgetEntry& e = _categories[idx];
    const u32 remain = static_cast<u32>(0xFFFFFFFFu) - e.spent_bytes;
    if (bytes >= remain) {
        e.spent_bytes = 0xFFFFFFFFu;  // 飽和加算
    } else {
        e.spent_bytes += bytes;
    }
}

void FPerfBudget::RecordMemoryFree(const char* category, u32 bytes) noexcept {
    if (category == nullptr || bytes == 0u) return;
    const usize idx = FindCategoryIndex(category);
    if (idx >= _categories.Size()) return;
    BudgetEntry& e = _categories[idx];
    // unsigned underflow を避けるため clamp。alloc/free 不整合時の保護。
    if (bytes >= e.spent_bytes) {
        e.spent_bytes = 0u;
    } else {
        e.spent_bytes -= bytes;
    }
}

// ============================================================================
// フレーム境界
// ============================================================================

void FPerfBudget::BeginFrame() noexcept {
    // spent_ms のみ 0 にリセット。spent_bytes は累積保持 (現在保持中の量)。
    for (usize i = 0; i < _categories.Size(); ++i) {
        _categories[i].spent_ms = 0.0f;
    }
}

void FPerfBudget::EndFrame() noexcept {
    // 全 category の spent_ms 合計を取り、frame 履歴に push。
    f32 total = 0.0f;
    for (usize i = 0; i < _categories.Size(); ++i) {
        total += _categories[i].spent_ms;
    }
    _last_frame_ms = total;

    // frame_budget_ms <= 0 (= 未設定) なら超過判定無効。
    if (_frame_budget_ms > 0.0f) {
        _frame_over_budget = (total > _frame_budget_ms);
    } else {
        _frame_over_budget = false;
    }

    // 循環バッファ書き込み (FDebugOverlay と同じパターン)。
    if (_frame_history.Capacity() < kFrameHistoryCap) {
        _frame_history.Reserve(kFrameHistoryCap);
    }
    if (!_frame_filled) {
        _frame_history.PushBack(total);
        ++_frame_index;
        if (_frame_index >= kFrameHistoryCap) {
            _frame_index  = 0u;
            _frame_filled = true;
        }
    } else {
        _frame_history[_frame_index] = total;
        ++_frame_index;
        if (_frame_index >= kFrameHistoryCap) _frame_index = 0u;
    }
}

// ============================================================================
// 問い合わせ
// ============================================================================

bool FPerfBudget::IsOverBudget(const char* category) const noexcept {
    if (category == nullptr) return false;
    const usize idx = FindCategoryIndex(category);
    if (idx >= _categories.Size()) return false;
    const BudgetEntry& e = _categories[idx];
    // budget_ms > 0 のときのみ ms 超過を判定 (0 = 無効上限)。
    const bool ms_over    = (e.budget_ms    > 0.0f) && (e.spent_ms    > e.budget_ms);
    const bool bytes_over = (e.budget_bytes > 0u  ) && (e.spent_bytes > e.budget_bytes);
    return ms_over || bytes_over;
}

f32 FPerfBudget::AverageFrameMs() const noexcept {
    const usize n = _frame_history.Size();
    if (n == 0u) return 0.0f;
    f32 sum = 0.0f;
    for (usize i = 0; i < n; ++i) sum += _frame_history[i];
    return sum / static_cast<f32>(n);
}

// ============================================================================
// 列挙 / 管理
// ============================================================================

u32 FPerfBudget::CategoryCount() const noexcept {
    return static_cast<u32>(_categories.Size());
}

const BudgetEntry* FPerfBudget::AllCategories(u32& out_count) const noexcept {
    out_count = static_cast<u32>(_categories.Size());
    if (_categories.Size() == 0u) return nullptr;
    return _categories.Data();
}

void FPerfBudget::Reset() noexcept {
    _categories.Clear();
    _frame_history.Clear();
    _frame_index        = 0u;
    _frame_filled       = false;
    _last_frame_ms      = 0.0f;
    _frame_over_budget  = false;
    // _frame_budget_ms は意図的に保持 (頻繁に再設定する想定がない)。
}

} // namespace acs::game
