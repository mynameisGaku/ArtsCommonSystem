// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace AcsEditor;

internal static class InspectorReflectedMultiEditSelfTest
{
    private sealed record PropertyMutation(float[] Before, float[] After);

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

        InspectorReflectedPropertySchema Schema(
            string name,
            int index,
            InspectorReflectedPropertyKind kind,
            InspectorReflectedPropertyFlags flags =
                InspectorReflectedPropertyFlags.None,
            string category = "",
            bool hasDefault = true,
            float x = 0.0f,
            float y = 0.0f,
            float z = 0.0f,
            float w = 0.0f)
        {
            bool created =
                InspectorReflectedPropertySchema.TryCreate(
                    name,
                    index,
                    (int)kind,
                    (int)flags,
                    category,
                    hasDefault,
                    new[] { x, y, z, w },
                    out InspectorReflectedPropertySchema? schema,
                    out string failure);
            if (!created || schema is null)
                throw new InvalidDataException(failure);
            return schema;
        }

        try
        {
            InspectorReflectedPropertySchema roughness =
                Schema(
                    "roughness",
                    4,
                    InspectorReflectedPropertyKind.F32,
                    category: "Surface Detail",
                    x: 0.105f);
            InspectorReflectedPropertySchema flow =
                Schema(
                    "flowDirection",
                    7,
                    InspectorReflectedPropertyKind.Vec2,
                    category: "Waves",
                    x: 0.92f,
                    y: 0.38f);
            InspectorReflectedPropertySchema mode =
                Schema(
                    "mode",
                    8,
                    InspectorReflectedPropertyKind.Enum,
                    x: 1.0f);
            InspectorReflectedPropertySchema hidden =
                Schema(
                    "internalState",
                    9,
                    InspectorReflectedPropertyKind.F32,
                    InspectorReflectedPropertyFlags.Hidden);
            InspectorReflectedPropertySchema readOnly =
                Schema(
                    "runtimeValue",
                    10,
                    InspectorReflectedPropertyKind.F32,
                    InspectorReflectedPropertyFlags.ReadOnly,
                    hasDefault: false);
            InspectorReflectedPropertySchema boolSchema =
                Schema(
                    "enabled",
                    11,
                    InspectorReflectedPropertyKind.Bool,
                    x: 1.0f);

            InspectorReflectedComponentSnapshot Water(
                int nodeId,
                int slot,
                bool mismatchedMode = false)
            {
                InspectorReflectedPropertySchema targetMode =
                    mismatchedMode
                        ? Schema(
                            "mode",
                            80,
                            InspectorReflectedPropertyKind.F32,
                            x: 1.0f)
                        : Schema(
                            "mode",
                            80,
                            InspectorReflectedPropertyKind.Enum,
                            x: 1.0f);
                return new(
                    nodeId,
                    slot,
                    "AWaterSurface3DComponent",
                    new[]
                    {
                        Schema(
                            "roughness",
                            40,
                            InspectorReflectedPropertyKind.F32,
                            category: "Surface Detail",
                            x: 0.105f),
                        Schema(
                            "flowDirection",
                            70,
                            InspectorReflectedPropertyKind.Vec2,
                            category: "Waves",
                            x: 0.92f,
                            y: 0.38f),
                        targetMode,
                        Schema(
                            "internalState",
                            90,
                            InspectorReflectedPropertyKind.F32,
                            InspectorReflectedPropertyFlags.Hidden),
                        Schema(
                            "runtimeValue",
                            100,
                            InspectorReflectedPropertyKind.F32,
                            InspectorReflectedPropertyFlags.ReadOnly,
                            hasDefault: false),
                        Schema(
                            "enabled",
                            110,
                            InspectorReflectedPropertyKind.Bool,
                            x: 1.0f),
                    });
            }

            var nodes = new[]
            {
                new InspectorReflectedNodeSnapshot(
                    4,
                    new[]
                    {
                        new InspectorReflectedComponentSnapshot(
                            4,
                            3,
                            "AUniqueComponent",
                            Array.Empty<InspectorReflectedPropertySchema>()),
                        new InspectorReflectedComponentSnapshot(
                            4,
                            0,
                            "AWaterSurface3DComponent",
                            new[]
                            {
                                roughness,
                                flow,
                                mode,
                                hidden,
                                readOnly,
                                boolSchema,
                            }),
                    }),
                new InspectorReflectedNodeSnapshot(
                    7,
                    new[] { Water(7, 2) }),
                new InspectorReflectedNodeSnapshot(
                    12,
                    new[] { Water(12, 1, mismatchedMode: true) }),
            };
            bool intersected =
                InspectorReflectedMultiEditContract.TryIntersect(
                    nodes,
                    out IReadOnlyList<InspectorReflectedCommonComponent> common,
                    out string intersectionFailure);
            InspectorReflectedCommonComponent? water =
                common.SingleOrDefault();
            Check(
                intersected &&
                intersectionFailure.Length == 0 &&
                water is not null &&
                water.TypeName == "AWaterSurface3DComponent" &&
                water.Targets.Select(static target =>
                        (target.NodeId, target.Slot))
                    .SequenceEqual(
                        new[] { (4, 0), (7, 2), (12, 1) }),
                "component intersection is exact-type and resolves each target slot");
            Check(
                water is not null &&
                water.Properties.Select(static property =>
                        property.Schema.Name)
                    .SequenceEqual(
                        new[]
                        {
                            "roughness",
                            "flowDirection",
                            "internalState",
                            "runtimeValue",
                            "enabled",
                        }) &&
                water.Properties.All(property =>
                    property.Targets.Select(static target =>
                            target.PropertyIndex)
                        .SequenceEqual(
                            property.Schema.Name switch
                            {
                                "roughness" => new[] { 4, 40, 40 },
                                "flowDirection" => new[] { 7, 70, 70 },
                                "internalState" => new[] { 9, 90, 90 },
                                "runtimeValue" => new[] { 10, 100, 100 },
                                "enabled" => new[] { 11, 110, 110 },
                                _ => Array.Empty<int>(),
                            })),
                "property intersection rejects a kind mismatch and retains per-target indices");

            var duplicateTypeNode = new InspectorReflectedNodeSnapshot(
                20,
                new[] { Water(20, 0), Water(20, 1) });
            Check(
                !InspectorReflectedMultiEditContract.TryIntersect(
                    new[] { nodes[0], duplicateTypeNode },
                    out _,
                    out _),
                "duplicate component types fail closed instead of binding an ambiguous slot");

            InspectorMixedFloat[] mixedFlow =
                InspectorReflectedMultiEditContract.ResolveMixedComponents(
                    flow,
                    new IReadOnlyList<float>[]
                    {
                        new[] { 0.92f, 0.38f, 0.0f, 0.0f },
                        new[] { 0.92f, 0.45f, 0.0f, 0.0f },
                        new[] { 0.92f, 0.50f, 0.0f, 0.0f },
                    });
            Check(
                mixedFlow.Length == 2 &&
                mixedFlow[0] is
                    { HasValue: true, IsMixed: false, Value: 0.92f } &&
                mixedFlow[1] is { HasValue: true, IsMixed: true } &&
                InspectorMultiEditContract.DisplayText(mixedFlow[1]) ==
                    InspectorMultiEditContract.MixedPlaceholder,
                "reflected vector components expose explicit per-axis mixed values");

            InspectorMixedBool mixedBool =
                InspectorReflectedMultiEditContract.ResolveMixedBool(
                    new IReadOnlyList<float>[]
                    {
                        new[] { 1.0f, 0.0f, 0.0f, 0.0f },
                        new[] { 0.0f, 0.0f, 0.0f, 0.0f },
                    });
            Check(
                mixedBool is { HasValue: true, IsMixed: true },
                "reflected bool presentation preserves a tri-state mixed value");

            float negativeZero =
                BitConverter.Int32BitsToSingle(unchecked((int)0x80000000));
            float[] beforeFlow =
                { 0.92f, 0.38f, negativeZero, 5.0f };
            bool builtSparse =
                InspectorReflectedMultiEditContract.TryBuildMutation(
                    flow,
                    beforeFlow,
                    new float?[] { null, 0.75f, null, null },
                    out float[] afterFlow);
            Check(
                builtSparse &&
                afterFlow[0] == 0.92f &&
                afterFlow[1] == 0.75f &&
                BitConverter.SingleToInt32Bits(afterFlow[2]) ==
                    BitConverter.SingleToInt32Bits(negativeZero) &&
                afterFlow[3] == 5.0f,
                "sparse reflected vector edits preserve every untouched float bit");

            bool enumBuilt =
                InspectorReflectedMultiEditContract.TryBuildMutation(
                    mode,
                    new[] { 1.0f, 8.0f, 9.0f, 10.0f },
                    new float?[] { 2.6f, null, null, null },
                    out float[] enumAfter);
            Check(
                enumBuilt &&
                enumAfter.SequenceEqual(
                    new[] { 3.0f, 8.0f, 9.0f, 10.0f }) &&
                !InspectorReflectedMultiEditContract.TryBuildMutation(
                    mode,
                    new[] { 1.0f, 0.0f, 0.0f, 0.0f },
                    new float?[]
                    {
                        16_777_218.0f,
                        null,
                        null,
                        null,
                    },
                    out _),
                "integer-like reflected edits canonicalize safely and reject inexact overflow");

            bool resetAvailable =
                InspectorReflectedMultiEditContract.TryBuildDefaultPatch(
                    flow,
                    out float?[] resetPatch) &&
                resetPatch.SequenceEqual(
                    new float?[] { 0.92f, 0.38f, null, null });
            Check(
                resetAvailable &&
                !InspectorReflectedMultiEditContract.TryBuildDefaultPatch(
                    readOnly,
                    out _) &&
                !InspectorReflectedMultiEditContract.TryBuildMutation(
                    readOnly,
                    new[] { 5.0f, 0.0f, 0.0f, 0.0f },
                    new float?[] { 2.0f, null, null, null },
                    out _),
                "Reset uses schema defaults and remains unavailable for non-editable fields");

            Check(
                InspectorMultiEditContract.SameSelection(
                    new[] { 4, 7, 12 },
                    new[] { 12, 4, 7 }) &&
                !InspectorMultiEditContract.SameSelection(
                    new[] { 4, 7, 12 },
                    new[] { 4, 7, 20 }),
                "reflected batches reject stale selection with the shared selection contract");

            var world = new Dictionary<int, float[]>
            {
                [4] = new[] { 0.1f, 1.0f, 2.0f, 3.0f },
                [7] = new[] { 0.2f, 4.0f, 5.0f, 6.0f },
                [12] = new[] { 0.3f, 7.0f, 8.0f, 9.0f },
            };
            int captures = 0;
            int writesBeforeCaptureComplete = 0;
            var rollbackOrder = new List<int>();
            InspectorAtomicBatchResult rolledBack =
                InspectorMultiEditContract.ApplyAtomically(
                    new[] { 4, 7, 12 },
                    (int nodeId, out PropertyMutation mutation) =>
                    {
                        captures++;
                        float[] before = (float[])world[nodeId].Clone();
                        bool built =
                            InspectorReflectedMultiEditContract
                                .TryBuildMutation(
                                    roughness,
                                    before,
                                    new float?[]
                                    {
                                        0.8f,
                                        null,
                                        null,
                                        null,
                                    },
                                    out float[] after);
                        mutation = new(before, after);
                        return built;
                    },
                    (nodeId, mutation) =>
                    {
                        if (captures != 3)
                            writesBeforeCaptureComplete++;
                        world[nodeId] =
                            (float[])mutation.After.Clone();
                        return nodeId != 7;
                    },
                    (nodeId, mutation) =>
                    {
                        rollbackOrder.Add(nodeId);
                        world[nodeId] =
                            (float[])mutation.Before.Clone();
                        return true;
                    });
            Check(
                !rolledBack.Succeeded &&
                rolledBack.FailedNodeId == 7 &&
                rolledBack.RollbackSucceeded &&
                writesBeforeCaptureComplete == 0 &&
                rollbackOrder.SequenceEqual(new[] { 7, 4 }) &&
                world[4][0] == 0.1f &&
                world[7][0] == 0.2f &&
                world[12][0] == 0.3f,
                "all reflected targets preflight before writes and rollback includes the failing target");

            int mutationWrites = 0;
            bool schemaStillMatches = false;
            InspectorAtomicBatchResult schemaDrift =
                InspectorMultiEditContract.ApplyAtomically(
                    new[] { 4, 7, 12 },
                    (int nodeId, out PropertyMutation mutation) =>
                    {
                        mutation = new(
                            (float[])world[nodeId].Clone(),
                            new[] { 0.9f, 0.0f, 0.0f, 0.0f });
                        return nodeId != 7 || schemaStillMatches;
                    },
                    (_, _) =>
                    {
                        mutationWrites++;
                        return true;
                    },
                    (_, _) => true);
            Check(
                !schemaDrift.Succeeded &&
                schemaDrift.FailureStage ==
                    InspectorAtomicFailureStage.Capture &&
                mutationWrites == 0,
                "component/schema drift aborts during capture with zero mutations");

            string Snapshot() =>
                string.Join(
                    "|",
                    world.OrderBy(static pair => pair.Key)
                        .Select(static pair =>
                            $"{pair.Key}:{pair.Value[0]:R}"));
            string hostedState = Snapshot();
            void Restore(EditorDocumentState state)
            {
                hostedState = state.Payload;
                foreach (string item in state.Payload.Split('|'))
                {
                    string[] fields = item.Split(':');
                    world[int.Parse(fields[0])][0] =
                        float.Parse(
                            fields[1],
                            System.Globalization.CultureInfo.InvariantCulture);
                }
            }
            var document = new EditorDocument(
                new EditorDocumentId(
                    "scene",
                    "reflected-component-multi-edit"),
                "Reflected component multi-edit",
                null,
                () => EditorDocumentState.Text(Snapshot()),
                Restore,
                initialState: EditorDocumentState.Text(hostedState));
            using (document.BeginTransaction(
                       "Edit AWaterSurface3DComponent.roughness",
                       "component-selection",
                       TimeSpan.Zero))
            {
                InspectorAtomicBatchResult historyBatch =
                    InspectorMultiEditContract.ApplyAtomically(
                        new[] { 4, 7, 12 },
                        (int nodeId, out float[] before) =>
                        {
                            before = (float[])world[nodeId].Clone();
                            return true;
                        },
                        (nodeId, _) =>
                        {
                            world[nodeId][0] = 0.42f;
                            hostedState = Snapshot();
                            return true;
                        },
                        (nodeId, before) =>
                        {
                            world[nodeId] = before;
                            hostedState = Snapshot();
                            return true;
                        });
                Check(
                    historyBatch.Succeeded,
                    "reflected component document-host batch succeeds");
            }
            string applied = hostedState;
            bool undo =
                document.Undo(out _) &&
                hostedState == "4:0.1|7:0.2|12:0.3";
            bool redo =
                document.Redo(out _) &&
                hostedState == applied;
            Check(
                document.UndoCount == 1 &&
                undo &&
                redo,
                "one reflected multi-property batch is one Scene Undo/Redo unit");
        }
        catch (Exception error)
        {
            failed++;
            output.WriteLine("FAIL: unexpected exception");
            output.WriteLine(error);
        }

        output.WriteLine(
            "Inspector reflected multi-edit self-test: " +
            $"{passed} passed, {failed} failed.");
        return failed;
    }
}
