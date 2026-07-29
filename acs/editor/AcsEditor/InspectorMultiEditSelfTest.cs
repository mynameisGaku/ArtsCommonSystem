// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace AcsEditor;

internal static class InspectorMultiEditSelfTest
{
    private sealed record TransformMutationFixture(
        float[] Before,
        float[] After,
        uint ComponentMask);

    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int passed = 0;
        int failed = 0;

        void Check(bool condition, string label)
        {
            if (condition)
            {
                passed++;
                output.WriteLine("PASS: " + label);
            }
            else
            {
                failed++;
                output.WriteLine("FAIL: " + label);
            }
        }

        try
        {
            InspectorMixedFloat common =
                InspectorMultiEditContract.ResolveFloat(
                    new[] { 3.0f, 3.000001f, 3.0f });
            InspectorMixedFloat mixed =
                InspectorMultiEditContract.ResolveFloat(
                    new[] { 3.0f, 4.0f, 3.0f });
            Check(
                common is { HasValue: true, IsMixed: false } &&
                common.Value == 3.0f &&
                mixed is { HasValue: true, IsMixed: true },
                "mixed resolver distinguishes a shared authored float from multiple values");
            Check(
                InspectorMultiEditContract.DisplayText(common) == "3" &&
                InspectorMultiEditContract.DisplayText(mixed) ==
                    InspectorMultiEditContract.MixedPlaceholder,
                "Details presentation uses an explicit mixed-value marker");
            float?[] sparse =
                InspectorMultiEditContract.BuildSparsePatch(
                    9,
                    3,
                    new float?[] { null, 45.0f, null });
            Check(
                sparse.Length == 9 &&
                sparse.Count(static value => value.HasValue) == 1 &&
                sparse[4] == 45.0f,
                "per-axis Details edit leaves every untouched component unassigned");

            float negativeZero =
                BitConverter.Int32BitsToSingle(unchecked((int)0x80000000));
            float[] legacyTransform =
            {
                1.0f, 2.0f, 3.0f,
                10.0f, 20.0f, 30.0f,
                0.0f, negativeZero, 5.0e-5f,
            };
            float?[] locationPatch =
                InspectorMultiEditContract.BuildSparsePatch(
                    9,
                    0,
                    new float?[] { 9.0f, null, null });
            bool builtLocation =
                InspectorMultiEditContract.TryBuildTransformMutation(
                    legacyTransform,
                    locationPatch,
                    out float[] locationAfter,
                    out uint locationMask);
            Check(
                builtLocation &&
                locationMask == 1u &&
                locationAfter[0] == 9.0f &&
                BitConverter.SingleToInt32Bits(locationAfter[6]) ==
                    BitConverter.SingleToInt32Bits(legacyTransform[6]) &&
                BitConverter.SingleToInt32Bits(locationAfter[7]) ==
                    BitConverter.SingleToInt32Bits(legacyTransform[7]) &&
                BitConverter.SingleToInt32Bits(locationAfter[8]) ==
                    BitConverter.SingleToInt32Bits(legacyTransform[8]),
                "location sparse edit preserves every untouched legacy scale bit");

            float?[] scalePatch =
                InspectorMultiEditContract.BuildSparsePatch(
                    9,
                    6,
                    new float?[] { null, null, 0.0f });
            float[] canonicalTransform =
                (float[])legacyTransform.Clone();
            canonicalTransform[6] = 1.0f;
            canonicalTransform[7] = -1.0f;
            canonicalTransform[8] = 2.0f;
            bool builtScale =
                InspectorMultiEditContract.TryBuildTransformMutation(
                    canonicalTransform,
                    scalePatch,
                    out float[] scaleAfter,
                    out uint scaleMask);
            Check(
                builtScale &&
                scaleMask == (1u << 8) &&
                scaleAfter[8] == 1.0e-4f &&
                BitConverter.SingleToInt32Bits(scaleAfter[6]) ==
                    BitConverter.SingleToInt32Bits(canonicalTransform[6]) &&
                BitConverter.SingleToInt32Bits(scaleAfter[7]) ==
                    BitConverter.SingleToInt32Bits(canonicalTransform[7]),
                "only an edited scale axis receives the invertibility clamp");

            float?[] scaleXPatch =
                InspectorMultiEditContract.BuildSparsePatch(
                    9,
                    6,
                    new float?[] { 2.0f, null, null });
            Check(
                InspectorMultiEditContract.ContainsNonRestorablePatchedScale(
                    legacyTransform,
                    scaleXPatch) &&
                !InspectorMultiEditContract.TryBuildTransformMutation(
                    legacyTransform,
                    scaleXPatch,
                    out _,
                    out _),
                "selected legacy near-zero scale is rejected before an atomic batch can write");

            var emptyTransformPatch = new float?[9];
            var invalidTransformPatch = new float?[9];
            invalidTransformPatch[0] = float.NaN;
            Check(
                !InspectorMultiEditContract.TryBuildTransformMutation(
                    legacyTransform,
                    emptyTransformPatch,
                    out _,
                    out _) &&
                !InspectorMultiEditContract.TryBuildTransformMutation(
                    legacyTransform,
                    invalidTransformPatch,
                    out _,
                    out _),
                "empty and non-finite native transform masks fail closed");

            InspectorMixedBool commonBool =
                InspectorMultiEditContract.ResolveBool(
                    new[] { true, true, true });
            InspectorMixedBool mixedBool =
                InspectorMultiEditContract.ResolveBool(
                    new[] { true, false, true });
            Check(
                commonBool is
                    { HasValue: true, IsMixed: false, Value: true } &&
                mixedBool is { HasValue: true, IsMixed: true },
                "tri-state Enabled presentation preserves common and mixed values");

            int[] normalized =
                InspectorMultiEditContract.NormalizeSelection(
                    new[] { 7, -1, 4, 7, 12 },
                    primaryNodeId: 4);
            Check(
                normalized.SequenceEqual(new[] { 4, 7, 12 }),
                "selection normalization is deduplicated and primary-first");
            Check(
                InspectorMultiEditContract.SameSelection(
                    normalized,
                    new[] { 12, 4, 7 }) &&
                !InspectorMultiEditContract.SameSelection(
                    normalized,
                    new[] { 12, 4, 9 }),
                "stale UI batches are rejected with order-independent selection comparison");
            Check(
                InspectorMultiEditContract.SelectionIdentity(
                    normalized) ==
                InspectorMultiEditContract.SelectionIdentity(
                    new[] { 12, 7, 4 }),
                "batch merge identity is stable across native enumeration order");

            var world = new Dictionary<int, int>
            {
                [4] = 10,
                [7] = 20,
                [12] = 30,
            };
            InspectorAtomicBatchResult success =
                InspectorMultiEditContract.ApplyAtomically(
                    normalized,
                    (int nodeId, out int before) =>
                    {
                        before = world[nodeId];
                        return true;
                    },
                    (nodeId, _) =>
                    {
                        world[nodeId] = 99;
                        return true;
                    },
                    (nodeId, before) =>
                    {
                        world[nodeId] = before;
                        return true;
                    });
            Check(
                success.Succeeded &&
                normalized.All(nodeId => world[nodeId] == 99),
                "successful atomic batch applies every selected node");

            world[4] = 10;
            world[7] = 20;
            world[12] = 30;
            var rollbackOrder = new List<int>();
            InspectorAtomicBatchResult rolledBack =
                InspectorMultiEditContract.ApplyAtomically(
                    normalized,
                    (int nodeId, out int before) =>
                    {
                        before = world[nodeId];
                        return true;
                    },
                    (nodeId, _) =>
                    {
                        world[nodeId] = 77;
                        return nodeId != 7;
                    },
                    (nodeId, before) =>
                    {
                        rollbackOrder.Add(nodeId);
                        world[nodeId] = before;
                        return true;
                    });
            Check(
                !rolledBack.Succeeded &&
                rolledBack.FailureStage ==
                    InspectorAtomicFailureStage.Apply &&
                rolledBack.FailedNodeId == 7 &&
                rolledBack.RollbackSucceeded &&
                world[4] == 10 &&
                world[7] == 20 &&
                world[12] == 30 &&
                rollbackOrder.SequenceEqual(new[] { 7, 4 }),
                "failed target and every prior write roll back in reverse order");

            int writes = 0;
            InspectorAtomicBatchResult captureFailure =
                InspectorMultiEditContract.ApplyAtomically(
                    normalized,
                    (int nodeId, out int before) =>
                    {
                        before = world[nodeId];
                        return nodeId != 7;
                    },
                    (nodeId, _) =>
                    {
                        writes++;
                        return true;
                    },
                    (_, _) => true);
            Check(
                !captureFailure.Succeeded &&
                captureFailure.FailureStage ==
                    InspectorAtomicFailureStage.Capture &&
                writes == 0,
                "all nodes are captured before the first native mutation");

            var transformWorld = new Dictionary<int, float[]>
            {
                [4] = (float[])canonicalTransform.Clone(),
                [7] = (float[])legacyTransform.Clone(),
                [12] = (float[])canonicalTransform.Clone(),
            };
            int[] legacyBits = transformWorld[7]
                .Select(BitConverter.SingleToInt32Bits)
                .ToArray();
            int transformWrites = 0;
            InspectorAtomicBatchResult nonRestorableCapture =
                InspectorMultiEditContract.ApplyAtomically(
                    normalized,
                    (int nodeId, out TransformMutationFixture mutation) =>
                    {
                        float[] before =
                            (float[])transformWorld[nodeId].Clone();
                        if (!InspectorMultiEditContract
                                .TryBuildTransformMutation(
                                    before,
                                    scaleXPatch,
                                    out float[] after,
                                    out uint mask))
                        {
                            mutation = null!;
                            return false;
                        }

                        mutation = new TransformMutationFixture(
                            before,
                            after,
                            mask);
                        return true;
                    },
                    (nodeId, mutation) =>
                    {
                        transformWrites++;
                        transformWorld[nodeId] =
                            (float[])mutation.After.Clone();
                        return true;
                    },
                    (nodeId, mutation) =>
                    {
                        transformWorld[nodeId] =
                            (float[])mutation.Before.Clone();
                        return true;
                    });
            Check(
                !nonRestorableCapture.Succeeded &&
                nonRestorableCapture.FailureStage ==
                    InspectorAtomicFailureStage.Capture &&
                nonRestorableCapture.FailedNodeId == 7 &&
                transformWrites == 0 &&
                transformWorld[7]
                    .Select(BitConverter.SingleToInt32Bits)
                    .SequenceEqual(legacyBits),
                "non-restorable legacy scale aborts with zero writes and bit-exact state");

            bool rollbackFailureObserved = false;
            InspectorAtomicBatchResult rollbackFailure =
                InspectorMultiEditContract.ApplyAtomically(
                    new[] { 4, 7 },
                    (int nodeId, out int before) =>
                    {
                        before = world[nodeId];
                        return true;
                    },
                    (nodeId, _) => nodeId != 7,
                    (nodeId, _) =>
                    {
                        rollbackFailureObserved = true;
                        return nodeId != 4;
                    });
            Check(
                !rollbackFailure.Succeeded &&
                !rollbackFailure.RollbackSucceeded &&
                rollbackFailureObserved,
                "rollback inconsistency is reported instead of hidden");

            var historyWorld = new Dictionary<int, int>
            {
                [4] = 1,
                [7] = 2,
                [12] = 3,
            };
            string HistorySnapshot() =>
                string.Join(
                    "|",
                    historyWorld.OrderBy(static pair => pair.Key)
                        .Select(static pair => $"{pair.Key}:{pair.Value}"));
            string hostedState = HistorySnapshot();
            void RestoreHistory(EditorDocumentState state)
            {
                hostedState = state.Payload;
                foreach (string pair in state.Payload.Split('|'))
                {
                    string[] fields = pair.Split(':');
                    historyWorld[int.Parse(fields[0])] =
                        int.Parse(fields[1]);
                }
            }
            var historyDocument = new EditorDocument(
                new EditorDocumentId("scene", "inspector-multi-edit"),
                "Inspector multi-edit",
                null,
                () => EditorDocumentState.Text(HistorySnapshot()),
                RestoreHistory,
                initialState: EditorDocumentState.Text(hostedState));
            using (historyDocument.BeginTransaction(
                       "Edit Location",
                       "selection-a",
                       TimeSpan.Zero))
            {
                InspectorAtomicBatchResult historyBatch =
                    InspectorMultiEditContract.ApplyAtomically(
                        normalized,
                        (int nodeId, out int before) =>
                        {
                            before = historyWorld[nodeId];
                            return true;
                        },
                        (nodeId, _) =>
                        {
                            historyWorld[nodeId] = 42;
                            hostedState = HistorySnapshot();
                            return true;
                        },
                        (nodeId, before) =>
                        {
                            historyWorld[nodeId] = before;
                            hostedState = HistorySnapshot();
                            return true;
                        });
                Check(
                    historyBatch.Succeeded,
                    "document-host integration batch succeeds");
            }
            string appliedHostedState = hostedState;
            bool undoSucceeded =
                historyDocument.Undo(out _) &&
                hostedState == "4:1|7:2|12:3";
            bool redoSucceeded =
                historyDocument.Redo(out _) &&
                hostedState == appliedHostedState;
            Check(
                historyDocument.UndoCount == 1 &&
                undoSucceeded &&
                redoSucceeded,
                "one multi-node batch is one Scene document Undo/Redo unit");
        }
        catch (Exception error)
        {
            failed++;
            output.WriteLine("FAIL: unexpected exception");
            output.WriteLine(error);
        }

        output.WriteLine(
            $"Inspector multi-edit self-test: {passed} passed, {failed} failed.");
        return failed;
    }
}
