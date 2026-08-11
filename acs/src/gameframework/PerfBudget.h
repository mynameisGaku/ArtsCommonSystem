// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * 1 カテゴリ分の予算 + 実測値。
 *
 * @details category 名は caller 所有 (本クラスは複製せず、リテラル運用を想定)。
 */
struct FBudgetEntry {
    /** カテゴリ名 (caller 所有、リテラル想定)。 */
    const char* category    = nullptr;

    /** 現フレームの累積時間 ms (BeginFrame でリセット)。 */
    f32         spent_ms    = 0.0f;

    /** 時間予算の上限 ms (超過判定用)。 */
    f32         budget_ms   = 0.0f;

    /** 現在保持中のメモリ bytes (alloc / free の差分累積)。 */
    u32         spent_bytes = 0u;

    /** メモリ予算の上限 bytes (超過判定用)。 */
    u32         budget_bytes = 0u;
};

/**
 * フレーム時間とメモリ確保量の予算超過をカテゴリ単位で追跡する state holder。
 *
 * @details
 * カテゴリごとに予算を定義し、毎フレーム実測値を記録 → 集計して各カテゴリ・フレーム全体の
 * 超過状態を問い合わせ可能にする。計測 (タイマ / IAllocator hook) と UI 描画は呼出し側責務。
 * category は line-key (pointer 同一 → strcmp) で線形検索する。frame 履歴は直近 60 frame の
 * 合計 ms を循環バッファで保持する。所有権を曖昧にしないため非コピー・非ムーブ。
 */
class CPerfBudget {
public:
    /** 空状態 (category なし、frame 予算なし) で構築する。 */
    CPerfBudget() noexcept = default;

    /** 破棄する。 */
    ~CPerfBudget() noexcept = default;

    /** コピー禁止 (履歴 / category の所有権を曖昧にしないため)。 */
    CPerfBudget(const CPerfBudget&)            = delete;

    /** コピー代入も禁止。 */
    CPerfBudget& operator=(const CPerfBudget&) = delete;

    /** ムーブ禁止。 */
    CPerfBudget(CPerfBudget&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CPerfBudget& operator=(CPerfBudget&&)      = delete;

    /**
     * 目標フレーム時間 (ms) を設定する。
     *
     * @details 例 60fps → 16.67、120fps → 8.33。負値 / NaN は 0 (= 判定無効) に正規化する。
     * @param ms 目標フレーム時間 ms (<= 0 でフレーム超過判定を無効化)。
     */
    void SetFrameBudget(f32 ms) noexcept;

    /**
     * category を新規定義、または既存を上書きする。
     *
     * @details
     * 同名 (pointer 同一 or strcmp 一致) があれば budget_* を差し替え、spent_* は保持する。
     * budget_ms の負値は 0 に clamp する。
     * @param category カテゴリ名 (caller 所有、nullptr は no-op)。
     * @param budget_ms 時間予算の上限 ms。
     * @param budget_bytes メモリ予算の上限 bytes。
     */
    void DefineCategory(const char* category, f32 budget_ms, u32 budget_bytes) noexcept;

    /**
     * 経過時間 (ms) をカテゴリに加算する。
     *
     * @details 複数回呼出しは累積する。
     * @param category 加算先カテゴリ名 (未定義 / nullptr は no-op)。
     * @param elapsed_ms 加算する経過時間 ms (<= 0 / NaN は no-op)。
     */
    void RecordTimeMs(const char* category, f32 elapsed_ms) noexcept;

    /**
     * メモリ確保 (bytes) をカテゴリに加算する。
     *
     * @details u32 の飽和加算で overflow を防ぐ。
     * @param category 加算先カテゴリ名 (未定義 / nullptr は no-op)。
     * @param bytes 加算するメモリ量 bytes (0 は no-op)。
     */
    void RecordMemoryAlloc(const char* category, u32 bytes) noexcept;

    /**
     * メモリ解放 (bytes) をカテゴリから減算する。
     *
     * @details spent_bytes を下回る場合は 0 に clamp する (alloc/free 不整合時の保護)。
     * @param category 減算先カテゴリ名 (未定義 / nullptr は no-op)。
     * @param bytes 減算するメモリ量 bytes (0 は no-op)。
     */
    void RecordMemoryFree(const char* category, u32 bytes) noexcept;

    /**
     * フレーム開始時に全 category の spent_ms を 0 にリセットする。
     *
     * @details spent_bytes は累積保持する (現在保持中の量を表すため)。
     */
    void BeginFrame() noexcept;

    /**
     * フレーム終了時に全 category の spent_ms 合計を集計して履歴に push する。
     *
     * @details 合計 ms を 60 frame 循環バッファに push し、LastFrameMs / FrameOverBudget を更新する。
     */
    void EndFrame() noexcept;

    /**
     * category が ms か bytes のいずれかで予算超過しているかを返す。
     *
     * @param category 問い合わせるカテゴリ名 (未定義 / nullptr は false)。
     * @return ms または bytes が予算を超過していれば true。
     */
    bool IsOverBudget(const char* category) const noexcept;

    /**
     * 直近 EndFrame の frame 合計 ms が frame_budget_ms を超過したかを返す。
     *
     * @return 超過していれば true。SetFrameBudget 未呼出 (= 0) なら常に false。
     */
    bool IsFrameOverBudget() const noexcept { return m_FrameOverBudget; }

    /**
     * 直近 EndFrame で記録した frame 合計 ms を返す。
     *
     * @return 直近フレームの合計 ms。EndFrame 未呼出時は 0。
     */
    f32 LastFrameMs() const noexcept { return m_LastFrameMs; }

    /**
     * 直近 60 frame の合計 ms 算術平均を返す。
     *
     * @return 履歴の算術平均 ms。履歴が空なら 0。
     */
    f32 AverageFrameMs() const noexcept;

    /**
     * 登録済み category 数を返す。
     *
     * @return category 件数。
     */
    u32 CategoryCount() const noexcept;

    /**
     * 全 category を読み取り用に列挙する。
     *
     * @details
     * 戻り値はクラス所有の内部バッファ (`TArray<FBudgetEntry>::Data`) で、DefineCategory /
     * Reset 呼出しまで有効。
     * @param out_count category 件数を書き込む出力先。
     * @return category 配列の先頭ポインタ。空のときは nullptr (out_count = 0)。
     */
    const FBudgetEntry* AllCategories(u32& out_count) const noexcept;

    /**
     * 全 category を削除し、frame 履歴もクリアする。
     *
     * @details frame_budget_ms は意図的に保持する (頻繁に再設定する想定がないため)。
     */
    void Reset() noexcept;

private:
    /** frame 履歴循環バッファのサンプル数 (CDebugOverlay と整合)。 */
    static constexpr u32 kFrameHistoryCap = 60u;

    /**
     * category 配列内で一致する entry の index を返す。
     *
     * @details pointer 同一 → strcmp の順で線形走査する。
     * @param category 探すカテゴリ名。
     * @return 一致する index。見つからなければ m_Categories.Size() (範囲外)。
     */
    usize FindCategoryIndex(const char* category) const noexcept;

    /** 登録済み category の配列。 */
    TArray<FBudgetEntry> m_Categories;

    /** 目標フレーム時間 ms (0 = フレーム超過判定無効)。 */
    f32  m_FrameBudgetMs     = 0.0f;

    /** frame 合計 ms の履歴 (size <= kFrameHistoryCap)。 */
    TArray<f32> m_FrameHistory;

    /** 次に書き込む履歴スロット (mod kFrameHistoryCap)。 */
    u32        m_FrameIndex   = 0u;

    /** 履歴が一周したか (size == cap を意味する)。 */
    bool       m_bFrameFilled  = false;

    /** 直近 EndFrame で記録した frame 合計 ms。 */
    f32  m_LastFrameMs       = 0.0f;

    /** 直近 EndFrame で frame 合計 ms が予算超過していたか。 */
    bool m_FrameOverBudget   = false;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FPerfBudget = CPerfBudget;

} // namespace acs::game
