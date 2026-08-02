// SPDX-License-Identifier: Apache-2.0
// ACSBT グラフ永続化の敵対入力・トランザクション性テスト。

#include "test/Test.h"
#include "test/Expect.h"

#if WITH_RENDER_DX12_RAW

#include "gameframework/tools/btedit/BehaviorTreeEditorPanel.h"

#include <cstdio>
#include <cstring>
#include <limits>

using namespace acs;
using namespace acs::game;
using namespace acs::game::btedit;

namespace {

EBtStatus BtPersistenceSuccess(void*, f32) noexcept {
    return EBtStatus::Success;
}

constexpr const char kCanonicalGraph[] =
    "ACSBT 4\n"
    "2\n"
    "0 -1 1 0 0 0 0 10 20 - Root Sequence\n"
    "1 0 2 0 0 0 0 30 40 - Win Action\n"
    "BB 3\n"
    "hp 2 42.5\n"
    "ammo 1 -7\n"
    "alive 0 1\n";

} // namespace

ACS_TEST(BtPersistenceSafety, CanonicalParseAndRuntimeBake)
{
    ABehaviorTreeEditorPanel panel;
    panel.Init();
    FBtBlackboard blackboard;
    blackboard.Add("sentinel", EBtVarType::I32);
    blackboard.SetI32("sentinel", 91);
    panel.SetDynamicBlackboard(&blackboard);

    const FBtGraphPersistenceResult result =
        panel.TryParseGraphText(kCanonicalGraph, sizeof(kCanonicalGraph) - 1u);
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(panel.NodeCount(), 2u);
    EXPECT_EQ(blackboard.Count(), 3u);
    EXPECT_NEAR(blackboard.GetF32("hp"), 42.5f, 0.0001f);
    EXPECT_EQ(blackboard.GetI32("ammo"), -7);
    EXPECT_TRUE(blackboard.GetBool("alive"));
    EXPECT_FALSE(blackboard.Has("sentinel"));

    CBtActionRegistry registry;
    registry.Register("Win Action", &BtPersistenceSuccess);
    panel.SetActionRegistry(&registry);
    CBehaviorTree tree;
    tree.SetRoot(panel.BuildRuntimeTree());
    EXPECT_TRUE(tree.HasRoot());
    EXPECT_TRUE(tree.Tick(&blackboard, 0.016f) == EBtStatus::Success);
}

ACS_TEST(BtPersistenceSafety, FailureLeavesGraphAndBlackboardUnchanged)
{
    ABehaviorTreeEditorPanel panel;
    panel.Init();
    panel.AddNode(
        EBtKind::Selector, "existing",
        ABehaviorTreeEditorPanel::kInvalidId);
    FBtBlackboard blackboard;
    blackboard.Add("sentinel", EBtVarType::I32);
    blackboard.SetI32("sentinel", 91);
    panel.SetDynamicBlackboard(&blackboard);

    constexpr char duplicate_id[] =
        "ACSBT 4\n"
        "2\n"
        "0 -1 1 0 0 0 0 0 0 - root\n"
        "0 0 2 0 0 0 0 0 0 - child\n"
        "BB 0\n";
    FBtGraphPersistenceResult result = panel.TryParseGraphText(
        duplicate_id, sizeof(duplicate_id) - 1u);
    EXPECT_TRUE(
        result.error == EBtGraphPersistenceError::DuplicateNodeId);
    EXPECT_EQ(panel.NodeCount(), 1u);
    EXPECT_EQ(blackboard.Count(), 1u);
    EXPECT_EQ(blackboard.GetI32("sentinel"), 91);

    constexpr char truncated[] =
        "ACSBT 4\n"
        "1\n"
        "0 -1 2 0 0 0 0 0 0 - action\n";
    result =
        panel.TryParseGraphText(truncated, sizeof(truncated) - 1u);
    EXPECT_TRUE(
        result.error ==
        EBtGraphPersistenceError::InvalidBlackboardRecord);
    EXPECT_EQ(panel.NodeCount(), 1u);
    EXPECT_EQ(blackboard.GetI32("sentinel"), 91);

    const char embedded_nul[] = {
        'A', 'C', 'S', 'B', 'T', ' ', '4', '\n', '0', '\n',
        'B', 'B', ' ', '0', '\0', '\n'
    };
    result = panel.TryParseGraphText(
        embedded_nul, sizeof(embedded_nul));
    EXPECT_TRUE(result.error == EBtGraphPersistenceError::EmbeddedNul);
    EXPECT_EQ(panel.NodeCount(), 1u);
    EXPECT_EQ(blackboard.GetI32("sentinel"), 91);
}

ACS_TEST(BtPersistenceSafety, RejectsReferencesCyclesAndNonFiniteValues)
{
    ABehaviorTreeEditorPanel panel;
    panel.Init();

    constexpr char bad_parent[] =
        "ACSBT 4\n"
        "1\n"
        "0 1 2 0 0 0 0 0 0 - action\n"
        "BB 0\n";
    FBtGraphPersistenceResult result =
        panel.TryParseGraphText(bad_parent, sizeof(bad_parent) - 1u);
    EXPECT_TRUE(
        result.error ==
        EBtGraphPersistenceError::InvalidParentReference);

    constexpr char cycle[] =
        "ACSBT 4\n"
        "2\n"
        "0 1 1 0 0 0 0 0 0 - a\n"
        "1 0 1 0 0 0 0 0 0 - b\n"
        "BB 0\n";
    result = panel.TryParseGraphText(cycle, sizeof(cycle) - 1u);
    EXPECT_TRUE(result.error == EBtGraphPersistenceError::CycleDetected);

    constexpr char non_finite[] =
        "ACSBT 4\n"
        "1\n"
        "0 -1 2 0 0 0 nan 0 0 - action\n"
        "BB 0\n";
    result =
        panel.TryParseGraphText(non_finite, sizeof(non_finite) - 1u);
    EXPECT_TRUE(
        result.error == EBtGraphPersistenceError::NonFiniteNumber ||
        result.error == EBtGraphPersistenceError::InvalidNumber);

    constexpr char duplicate_blackboard[] =
        "ACSBT 4\n"
        "0\n"
        "BB 2\n"
        "hp 2 1\n"
        "hp 2 2\n";
    result = panel.TryParseGraphText(
        duplicate_blackboard, sizeof(duplicate_blackboard) - 1u);
    EXPECT_TRUE(
        result.error ==
        EBtGraphPersistenceError::DuplicateBlackboardName);
}

ACS_TEST(BtPersistenceSafety, EnforcesDocumentAndLineBounds)
{
    ABehaviorTreeEditorPanel panel;
    panel.Init();
    const FBtGraphPersistenceResult oversized = panel.TryParseGraphText(
        "x", ABehaviorTreeEditorPanel::kMaxGraphTextBytes + 1u);
    EXPECT_TRUE(
        oversized.error == EBtGraphPersistenceError::InputTooLarge);

    char long_line[ABehaviorTreeEditorPanel::kMaxGraphLineBytes + 2u]{};
    std::memset(long_line, 'x', sizeof(long_line));
    const FBtGraphPersistenceResult line_result =
        panel.TryParseGraphText(long_line, sizeof(long_line));
    EXPECT_TRUE(
        line_result.error == EBtGraphPersistenceError::LineTooLong);
}

ACS_TEST(BtPersistenceSafety, RejectsExcessiveParentDepth)
{
    ABehaviorTreeEditorPanel panel;
    panel.Init();
    char graph[32768]{};
    usize offset = 0u;
    int written = std::snprintf(
        graph, sizeof(graph), "ACSBT 4\n65\n");
    EXPECT_TRUE(written > 0);
    offset = static_cast<usize>(written);
    for (u32 id = 0u; id < 65u; ++id) {
        const i32 parent = id == 0u ? -1 : static_cast<i32>(id - 1u);
        const u32 kind = id == 64u
            ? static_cast<u32>(EBtKind::Action)
            : static_cast<u32>(EBtKind::Sequence);
        written = std::snprintf(
            graph + offset, sizeof(graph) - offset,
            "%u %d %u 0 0 0 0 0 0 - n%u\n",
            id, parent, kind, id);
        EXPECT_TRUE(written > 0);
        offset += static_cast<usize>(written);
    }
    written = std::snprintf(
        graph + offset, sizeof(graph) - offset, "BB 0\n");
    EXPECT_TRUE(written > 0);
    offset += static_cast<usize>(written);

    const FBtGraphPersistenceResult result =
        panel.TryParseGraphText(graph, offset);
    EXPECT_TRUE(
        result.error == EBtGraphPersistenceError::DepthLimitExceeded);
}

ACS_TEST(BtPersistenceSafety, LegacyV1RemainsReadable)
{
    ABehaviorTreeEditorPanel panel;
    panel.Init();
    FBtBlackboard blackboard;
    blackboard.Add("keep", EBtVarType::I32);
    blackboard.SetI32("keep", 17);
    panel.SetDynamicBlackboard(&blackboard);

    constexpr char legacy[] =
        "ACSBT 1\n"
        "1\n"
        "0 -1 2 1.5 -2.25 Legacy Action\n";
    const FBtGraphPersistenceResult result =
        panel.TryParseGraphText(legacy, sizeof(legacy) - 1u);
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(panel.NodeCount(), 1u);
    EXPECT_EQ(blackboard.Count(), 1u);
    EXPECT_EQ(blackboard.GetI32("keep"), 17);
}

ACS_TEST(BtPersistenceSafety, AtomicSaveAndFileRoundTrip)
{
    constexpr const char* path = "bt_persistence_safety_roundtrip.btg";
    std::remove(path);

    ABehaviorTreeEditorPanel source;
    source.Init();
    const u32 root = source.AddNode(
        EBtKind::Sequence, "Root Sequence",
        ABehaviorTreeEditorPanel::kInvalidId);
    source.AddNode(EBtKind::Action, "Win Action", root);
    FBtBlackboard source_blackboard;
    source_blackboard.Add("hp", EBtVarType::F32);
    source_blackboard.SetF32("hp", 42.5f);
    source.SetDynamicBlackboard(&source_blackboard);

    FBtGraphPersistenceResult result = source.TrySaveGraph(path);
    EXPECT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.bytes > 0u);

    ABehaviorTreeEditorPanel loaded;
    loaded.Init();
    FBtBlackboard loaded_blackboard;
    loaded.SetDynamicBlackboard(&loaded_blackboard);
    result = loaded.TryLoadGraph(path);
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(loaded.NodeCount(), 2u);
    EXPECT_EQ(loaded_blackboard.Count(), 1u);
    EXPECT_NEAR(loaded_blackboard.GetF32("hp"), 42.5f, 0.0001f);

    source_blackboard.SetF32(
        "hp", std::numeric_limits<f32>::infinity());
    result = source.TrySaveGraph(path);
    EXPECT_TRUE(
        result.error == EBtGraphPersistenceError::NonFiniteNumber);

    ABehaviorTreeEditorPanel still_valid;
    still_valid.Init();
    FBtBlackboard still_valid_blackboard;
    still_valid.SetDynamicBlackboard(&still_valid_blackboard);
    EXPECT_TRUE(still_valid.TryLoadGraph(path).Succeeded());
    EXPECT_NEAR(
        still_valid_blackboard.GetF32("hp"), 42.5f, 0.0001f);
    std::remove(path);
}

ACS_TEST(BtPersistenceSafety, StableErrorNames)
{
    EXPECT_TRUE(std::strcmp(
        FBtGraphPersistenceResult::ErrorName(
            EBtGraphPersistenceError::CycleDetected),
        "CycleDetected") == 0);
    EXPECT_TRUE(std::strcmp(
        FBtGraphPersistenceResult::ErrorName(
            EBtGraphPersistenceError::AtomicReplaceFailed),
        "AtomicReplaceFailed") == 0);
}

#endif // WITH_RENDER_DX12_RAW
