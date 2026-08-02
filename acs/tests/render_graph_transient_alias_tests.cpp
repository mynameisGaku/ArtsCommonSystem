// SPDX-License-Identifier: Apache-2.0
// Render Graph transient alias 寿命解析の parity / stress 検証

#include "container/Array.h"
#include "render/RenderGraphTransientAlias.h"
#include "test/Expect.h"
#include "test/Test.h"

using namespace acs;

namespace {

/** 単純な区間比較で alias 可否を求める参照実装。 */
bool ScalarCanAlias(const FRenderGraphResourceLifetime& first, const FRenderGraphResourceLifetime& second) noexcept {
    return first.resource_id != second.resource_id && first.size_bytes != 0 && second.size_bytes != 0 && first.first_pass <= first.last_pass && second.first_pass <= second.last_pass && first.transient && second.transient && first.alias_allowed && second.alias_allowed && first.compatibility_key == second.compatibility_key && (first.last_pass < second.first_pass || second.last_pass < first.first_pass);
}

/** リソース識別子に対応する slot を探す。 */
u32 FindAliasSlot(const CRenderGraphTransientAliasPlanner& planner, u32 resource_id) noexcept {
    for (usize assignment_index = 0; assignment_index < planner.AssignmentCount(); ++assignment_index) {
        if (planner.Assignments()[assignment_index].resource_id == resource_id) {
            return planner.Assignments()[assignment_index].slot_index;
        }
    }
    return ~static_cast<u32>(0);
}

} // namespace

ACS_TEST(RenderGraphTransientAlias, MatchesInclusiveScalarParity)
{
    // 入力順と pass 順を意図的にずらした寿命群。
    const FRenderGraphResourceLifetime lifetimes[] = {{13u, 2u, 200u, 4u, 4u, true, true}, {11u, 1u, 80u, 2u, 3u, true, true}, {14u, 1u, 300u, 4u, 5u, false, true}, {10u, 1u, 100u, 0u, 1u, true, true}, {15u, 1u, 60u, 6u, 7u, true, true}, {12u, 1u, 90u, 1u, 2u, true, true}};
    // 寿命群の要素数。
    constexpr usize lifetime_count = sizeof(lifetimes) / sizeof(lifetimes[0]);

    for (usize first_index = 0; first_index < lifetime_count; ++first_index) {
        for (usize second_index = 0; second_index < lifetime_count; ++second_index) {
            EXPECT_EQ(CRenderGraphTransientAliasPlanner::CanAlias(lifetimes[first_index], lifetimes[second_index]), ScalarCanAlias(lifetimes[first_index], lifetimes[second_index]));
        }
    }

    // 寿命群を解析する候補プランナー。
    CRenderGraphTransientAliasPlanner planner;
    EXPECT_TRUE(planner.Build(lifetimes, lifetime_count));
    EXPECT_EQ(planner.Summary().resource_count, 6u);
    EXPECT_EQ(planner.Summary().slot_count, 4u);
    EXPECT_EQ(planner.Summary().logical_bytes, 830u);
    EXPECT_EQ(planner.Summary().candidate_heap_bytes, 690u);
    EXPECT_EQ(planner.Summary().PotentialSavedBytes(), 140u);
    EXPECT_EQ(FindAliasSlot(planner, 10u), FindAliasSlot(planner, 11u));
    EXPECT_EQ(FindAliasSlot(planner, 10u), FindAliasSlot(planner, 15u));
    EXPECT_FALSE(FindAliasSlot(planner, 10u) == FindAliasSlot(planner, 12u));
    EXPECT_FALSE(FindAliasSlot(planner, 14u) == FindAliasSlot(planner, 15u));

    for (usize first_index = 0; first_index < lifetime_count; ++first_index) {
        for (usize second_index = first_index + 1; second_index < lifetime_count; ++second_index) {
            if (FindAliasSlot(planner, lifetimes[first_index].resource_id) == FindAliasSlot(planner, lifetimes[second_index].resource_id)) {
                EXPECT_TRUE(ScalarCanAlias(lifetimes[first_index], lifetimes[second_index]));
            }
        }
    }
}

ACS_TEST(RenderGraphTransientAlias, StressIsDeterministicAndFailClosed)
{
    // 大規模 graph の論理リソース数。
    constexpr usize resource_count = 4096;
    // 逆順で与える寿命群。
    TArray<FRenderGraphResourceLifetime> lifetimes;
    EXPECT_TRUE(lifetimes.TryResize(resource_count));
    // 同じ graph を正順へ並べた比較入力。
    TArray<FRenderGraphResourceLifetime> ordered;
    EXPECT_TRUE(ordered.TryResize(resource_count));
    for (usize input_index = 0; input_index < resource_count; ++input_index) {
        // 逆順にした論理 pass 番号。
        const u32 logical_index = static_cast<u32>(resource_count - input_index - 1);
        lifetimes[input_index] = FRenderGraphResourceLifetime{logical_index + 1u, static_cast<u64>(logical_index % 4u) + 1u, static_cast<u64>(64u + logical_index % 1024u), logical_index * 2u, logical_index * 2u, true, true};
        ordered[logical_index] = lifetimes[input_index];
    }

    // 逆順入力の候補計画。
    CRenderGraphTransientAliasPlanner first;
    // 正順入力の候補計画。
    CRenderGraphTransientAliasPlanner second;
    EXPECT_TRUE(first.Build(lifetimes.Data(), lifetimes.Size()));
    EXPECT_TRUE(second.Build(ordered.Data(), ordered.Size()));
    EXPECT_EQ(first.Summary().resource_count, 4096u);
    EXPECT_EQ(first.Summary().slot_count, 4u);
    EXPECT_EQ(first.Summary().candidate_heap_bytes, second.Summary().candidate_heap_bytes);
    EXPECT_TRUE(first.Summary().PotentialSavedBytes() > 0u);
    for (u32 resource_id = 1u; resource_id <= static_cast<u32>(resource_count); ++resource_id) {
        EXPECT_EQ(FindAliasSlot(first, resource_id), FindAliasSlot(second, resource_id));
    }

    // 重複識別子は不正入力として計画を空へ戻す。
    FRenderGraphResourceLifetime invalid[] = {{7u, 1u, 64u, 0u, 0u, true, true}, {7u, 1u, 64u, 1u, 1u, true, true}};
    EXPECT_FALSE(first.Build(invalid, 2u));
    EXPECT_EQ(first.AssignmentCount(), 0u);
    EXPECT_EQ(first.Summary().resource_count, 0u);
    EXPECT_EQ(first.Summary().candidate_heap_bytes, 0u);
}
