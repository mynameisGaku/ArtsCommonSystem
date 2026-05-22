// SPDX-License-Identifier: Apache-2.0
// GameFramework meta — PerfBudget (CPU/メモリ予算追跡)
//
// フレーム時間 (ms) とメモリ確保量 (bytes) の **予算超過追跡**を行う state holder。
// カテゴリ単位で予算を定義し、毎フレーム実測値を記録 → 集計し、各カテゴリ・フレーム
// 全体の超過状態を問い合わせ可能にする。実 UI 描画 (DebugOverlay / ImGui /
// `acs::easy::DrawString` 等) は呼出し側責務。
//
// 使い方:
//   acs::game::PerfBudget _budget;
//   void OnInit() noexcept {
//       _budget.SetFrameBudget(16.67f);                // 60fps
//       _budget.DefineCategory("Render", 8.0f, 64u*1024u*1024u);
//       _budget.DefineCategory("AI",     2.0f, 16u*1024u*1024u);
//       _budget.DefineCategory("Audio",  1.0f,  8u*1024u*1024u);
//   }
//   void OnFrame(f32 dt) noexcept {
//       _budget.BeginFrame();
//       // ... 各システムが計測スコープ末尾で RecordTimeMs / RecordMemoryAlloc を呼ぶ
//       _budget.RecordTimeMs("Render", render_elapsed_ms);
//       _budget.RecordMemoryAlloc("Render", bytes_allocated_this_frame);
//       _budget.EndFrame();
//       if (_budget.IsOverBudget("Render")) { /* 警告表示 */ }
//   }
//
// 設計選択 (GameFramework meta Phase 1):
//   ・**state holder のみ**: 計測 (QueryPerformanceCounter / Allocator hook 等) は
//     呼出し側責務。本クラスは値の保持・集計・予算判定だけを行う。
//   ・**category は line-key 検索**: `const char*` を pointer 同一 → strcmp の順で
//     線形走査。DebugOverlay watches と同じ規約。category 数は通常 10〜50 程度を想定
//     し、O(N) 走査で十分高速。
//   ・**frame 履歴**: 直近 60 frame の合計 ms 値を循環バッファで保持。AverageFrameMs
//     は履歴の算術平均、LastFrameMs は最新値。DebugOverlay と同じパターン。
//   ・**spent_ms は BeginFrame でリセット、spent_bytes は累積**:
//     - spent_ms は per-frame 時間なので 1 frame 単位で 0 リセット。
//     - spent_bytes は (alloc / free) の差分累積 (現在保持中の合計)。EndFrame でも
//       リセットしない。Free を呼ばないと永久に増え続ける = 想定通り (リーク検出に
//       使う想定)。
//   ・**フレーム合計超過判定**: EndFrame で 全 category の spent_ms 合計を取り、
//     frame_budget_ms と比較。IsFrameOverBudget() で取得。frame_budget_ms <= 0 の
//     場合 (未設定) は常に false。
//   ・**Reset**: 全 category を削除し、frame 履歴もクリア。SetFrameBudget で設定
//     した frame_budget_ms はリセットされない (頻繁に再定義する想定がない)。
//   ・**非コピー・非ムーブ**: 履歴 / category 配列の所有権を曖昧にしないため。
//   ・**STL 不使用**: `acs::Array<BudgetEntry>` / `acs::Array<f32>` を使用。
//     `<string>` 禁止。実装側で `<cstring>` (strcmp) のみ許可。
//
// 範囲外 (将来 phase で):
//   ・スレッドセーフ (Record 系は 1 thread 前提。MT 計測は外側で集計してから注入)
//   ・スコープガード `BudgetScope` (RAII で auto record)
//   ・自動メモリトラッキング (Pillar A `Allocator` hook 経由で alloc/free を自動記録)
//   ・カテゴリ階層 ("Render/Shadow" "Render/Post" 等)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// 1 カテゴリ分の予算 + 実測。
// category は caller 所有 (本クラスは複製しない、リテラル想定)。
struct BudgetEntry {
    const char* category    = nullptr;
    f32         spent_ms    = 0.0f;   // 現フレームの累積時間 (BeginFrame でリセット)
    f32         budget_ms   = 0.0f;   // 上限 (超過判定用)
    u32         spent_bytes = 0u;     // 現在保持中のメモリ (alloc / free 差分累積)
    u32         budget_bytes = 0u;    // 上限 (超過判定用)
};

class PerfBudget {
public:
    PerfBudget() noexcept = default;
    ~PerfBudget() noexcept = default;

    // 非コピー・非ムーブ (履歴 / category の所有権を曖昧にしないため)
    PerfBudget(const PerfBudget&)            = delete;
    PerfBudget& operator=(const PerfBudget&) = delete;
    PerfBudget(PerfBudget&&)                 = delete;
    PerfBudget& operator=(PerfBudget&&)      = delete;

    // ----- セットアップ -----

    // 目標フレーム時間 (ms)。例 60fps → 16.67、120fps → 8.33。
    // <= 0 を渡すと「フレーム超過判定無効」扱い (IsFrameOverBudget が常に false)。
    void SetFrameBudget(f32 ms) noexcept;

    // category を新規定義 / 既存上書き。category 名は caller 所有 (リテラル想定)。
    // 同名 (pointer 同一 or strcmp 一致) があれば budget_* を差し替え、spent_* は保持。
    // nullptr の category は no-op。
    void DefineCategory(const char* category, f32 budget_ms, u32 budget_bytes) noexcept;

    // ----- 記録 (各システムが計測末尾で呼ぶ) -----

    // 経過時間 (ms) をカテゴリに加算。category 未定義 / nullptr / elapsed_ms <= 0
    // は no-op。複数回呼出しは累積。
    void RecordTimeMs(const char* category, f32 elapsed_ms) noexcept;

    // メモリ確保 (bytes) をカテゴリに加算。category 未定義 / nullptr / bytes == 0
    // は no-op。
    void RecordMemoryAlloc(const char* category, u32 bytes) noexcept;

    // メモリ解放 (bytes) をカテゴリから減算。spent_bytes を下回る場合は 0 に clamp。
    // category 未定義 / nullptr / bytes == 0 は no-op。
    void RecordMemoryFree(const char* category, u32 bytes) noexcept;

    // ----- フレーム境界 -----

    // 全 category の spent_ms を 0 にリセット。spent_bytes は累積保持。
    void BeginFrame() noexcept;

    // 全 category の spent_ms 合計を frame 履歴 (60 frame 循環バッファ) に push。
    // _last_frame_ms / _frame_over_budget を更新。
    void EndFrame() noexcept;

    // ----- 問い合わせ -----

    // category が ms か bytes のいずれかで予算超過しているか。
    // category 未定義 / nullptr は false。
    bool IsOverBudget(const char* category) const noexcept;

    // 直近 EndFrame で集計した frame 合計 ms が frame_budget_ms を超過したか。
    // SetFrameBudget 未呼出 (= 0) なら常に false。
    bool IsFrameOverBudget() const noexcept { return _frame_over_budget; }

    // 直近 EndFrame で記録した frame 合計 ms。EndFrame 未呼出時は 0。
    f32 LastFrameMs() const noexcept { return _last_frame_ms; }

    // 直近 60 frame の合計 ms 算術平均。履歴空時は 0。
    f32 AverageFrameMs() const noexcept;

    // ----- 列挙 / 管理 -----

    // 登録済み category 数。
    u32 CategoryCount() const noexcept;

    // 全 category を読み取り用に列挙。out_count に件数を書き込み、生ポインタを返す。
    // 戻り値はクラス所有の内部バッファ (`Array<BudgetEntry>::Data`)。DefineCategory /
    // Reset 呼出しまで有効。空のときは nullptr を返し out_count = 0。
    const BudgetEntry* AllCategories(u32& out_count) const noexcept;

    // 全 category を削除し、frame 履歴もクリア。frame_budget_ms は保持。
    void Reset() noexcept;

private:
    // 履歴は固定容量の循環バッファ。サンプル数 = 60 (DebugOverlay と整合)。
    static constexpr u32 kFrameHistoryCap = 60u;

    // category 配列内で一致する entry の index を返す。見つからなければ
    // _categories.Size() を返す (= 範囲外)。pointer 同一 → strcmp の順。
    usize FindCategoryIndex(const char* category) const noexcept;

    Array<BudgetEntry> _categories;

    f32  _frame_budget_ms     = 0.0f;   // 0 のとき = フレーム超過判定無効

    Array<f32> _frame_history;          // size <= kFrameHistoryCap、要素は合計 ms
    u32        _frame_index   = 0u;     // 次に書き込むスロット (mod kFrameHistoryCap)
    bool       _frame_filled  = false;  // 履歴が一周したか (size == cap の意味)

    f32  _last_frame_ms       = 0.0f;
    bool _frame_over_budget   = false;
};

} // namespace acs::game
