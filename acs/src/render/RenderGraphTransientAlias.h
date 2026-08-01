// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "render/RenderGraphAliasAssignment.h"
#include "render/RenderGraphAliasPlanSummary.h"
#include "render/RenderGraphResourceLifetime.h"

namespace acs {

/**
 * Render Graph 一時リソースの alias 候補を構築する。
 *
 * @details 互換クラスが一致し、両方が transient かつ alias 許可済みで、
 * inclusive な寿命区間が重ならない場合だけ同一 slot を選ぶ。入力順に依存しない
 * pass 順の決定的な greedy 計画を作り、GPU alias barrier の実体化は行わない。
 */
class CRenderGraphTransientAliasPlanner {
public:
    /**
     * 寿命配列から alias 候補計画を構築する。
     *
     * @param lifetimes 解析する寿命配列。
     * @param count 寿命配列の要素数。
     * @return 成功なら true、不正入力または確保失敗なら false。
     */
    bool Build(const FRenderGraphResourceLifetime* lifetimes, usize count) noexcept;

    /** 現在の計画を空へ戻す。 */
    void Reset() noexcept;

    /**
     * 2 つの寿命を安全に alias 候補へできるか返す。
     *
     * @param first 一方の寿命。
     * @param second もう一方の寿命。
     * @return 安全な候補なら true。
     */
    static bool CanAlias(const FRenderGraphResourceLifetime& first, const FRenderGraphResourceLifetime& second) noexcept;

    /** 入力順と同じ割り当て配列を返す。 */
    const FRenderGraphAliasAssignment* Assignments() const noexcept {
        return m_Assignments.Data();
    }

    /** 割り当て要素数を返す。 */
    usize AssignmentCount() const noexcept {
        return m_Assignments.Size();
    }

    /** 現在の計画集計を返す。 */
    const FRenderGraphAliasPlanSummary& Summary() const noexcept {
        return m_Summary;
    }

private:
    /** 入力順を維持した slot 割り当て。 */
    TArray<FRenderGraphAliasAssignment> m_Assignments;

    /** pass 順へ並べた入力 index の作業領域。 */
    TArray<u32> m_Order;

    /** slot が最後に参照される pass。 */
    TArray<u32> m_SlotLastPass;

    /** slot の alias 互換クラス。 */
    TArray<u64> m_SlotCompatibility;

    /** slot に必要な最大配置バイト数。 */
    TArray<u64> m_SlotBytes;

    /** slot が後続 transient に再利用可能か。 */
    TArray<u8> m_SlotReusable;

    /** 現在の計画集計。 */
    FRenderGraphAliasPlanSummary m_Summary{};
};

/** 旧名を使う既存コード向けの互換別名。 */
using FRenderGraphTransientAliasPlanner = CRenderGraphTransientAliasPlanner;


} // namespace acs
