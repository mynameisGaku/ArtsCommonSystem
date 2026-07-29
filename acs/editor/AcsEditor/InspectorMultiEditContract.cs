// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Globalization;

namespace AcsEditor;

internal readonly record struct InspectorMixedFloat(
    bool HasValue,
    bool IsMixed,
    float Value);

internal readonly record struct InspectorMixedBool(
    bool HasValue,
    bool IsMixed,
    bool Value);

internal enum InspectorAtomicFailureStage
{
    None,
    Validation,
    Capture,
    Apply,
}

internal readonly record struct InspectorAtomicBatchResult(
    bool Succeeded,
    InspectorAtomicFailureStage FailureStage,
    int FailedNodeId,
    bool RollbackSucceeded)
{
    internal static InspectorAtomicBatchResult Success =>
        new(true, InspectorAtomicFailureStage.None, -1, true);
}

internal delegate bool InspectorStateCapture<TState>(
    int nodeId,
    out TState state);

/// <summary>
/// Pure contracts shared by the generated Details UI and its headless tests.
/// Native scene state remains authoritative; this type only resolves presentation
/// values and coordinates all-or-nothing mutation callbacks.
/// </summary>
internal static class InspectorMultiEditContract
{
    internal const string MixedPlaceholder = "—";
    internal const int TransformComponentCount = 9;

    internal static InspectorMixedFloat ResolveFloat(
        IReadOnlyList<float> values,
        float relativeTolerance = 1.0e-5f)
    {
        ArgumentNullException.ThrowIfNull(values);
        if (values.Count == 0 || relativeTolerance < 0.0f)
            return default;

        float first = values[0];
        if (!float.IsFinite(first))
            return default;
        for (int index = 1; index < values.Count; ++index)
        {
            float candidate = values[index];
            if (!float.IsFinite(candidate) ||
                !NearlyEqual(first, candidate, relativeTolerance))
            {
                return new InspectorMixedFloat(true, true, first);
            }
        }

        return new InspectorMixedFloat(true, false, first);
    }

    internal static InspectorMixedBool ResolveBool(
        IReadOnlyList<bool> values)
    {
        ArgumentNullException.ThrowIfNull(values);
        if (values.Count == 0)
            return default;

        bool first = values[0];
        for (int index = 1; index < values.Count; ++index)
        {
            if (values[index] != first)
                return new InspectorMixedBool(true, true, first);
        }

        return new InspectorMixedBool(true, false, first);
    }

    internal static string DisplayText(InspectorMixedFloat value) =>
        !value.HasValue || value.IsMixed
            ? MixedPlaceholder
            : value.Value.ToString("0.###", CultureInfo.InvariantCulture);

    internal static float?[] BuildSparsePatch(
        int totalComponentCount,
        int offset,
        IReadOnlyList<float?> editedComponents)
    {
        ArgumentNullException.ThrowIfNull(editedComponents);
        if (totalComponentCount <= 0 ||
            offset < 0 ||
            offset > totalComponentCount - editedComponents.Count)
        {
            throw new ArgumentOutOfRangeException(nameof(offset));
        }

        var result = new float?[totalComponentCount];
        for (int index = 0; index < editedComponents.Count; ++index)
            result[offset + index] = editedComponents[index];
        return result;
    }

    /// <summary>
    /// Builds the exact native masked-transform request and its expected
    /// readback. Only selected scale axes receive the native invertibility
    /// clamp; untouched legacy values retain their original float bits.
    /// </summary>
    internal static bool TryBuildTransformMutation(
        IReadOnlyList<float> before,
        IReadOnlyList<float?> patch,
        out float[] after,
        out uint componentMask)
    {
        ArgumentNullException.ThrowIfNull(before);
        ArgumentNullException.ThrowIfNull(patch);
        after = Array.Empty<float>();
        componentMask = 0u;
        if (before.Count != TransformComponentCount ||
            patch.Count != TransformComponentCount)
        {
            return false;
        }

        after = new float[TransformComponentCount];
        for (int index = 0; index < TransformComponentCount; ++index)
        {
            float current = before[index];
            if (!float.IsFinite(current))
            {
                after = Array.Empty<float>();
                return false;
            }

            after[index] = current;
        }

        const float minimumScaleMagnitude = 1.0e-4f;
        for (int index = 6; index < TransformComponentCount; ++index)
        {
            if (patch[index].HasValue &&
                MathF.Abs(before[index]) < minimumScaleMagnitude)
            {
                // A failed later target could not restore this selected axis:
                // the native safety clamp would canonicalize it again. Reject
                // the entire capture before the first batch write instead.
                after = Array.Empty<float>();
                return false;
            }
        }

        for (int index = 0; index < TransformComponentCount; ++index)
        {
            if (!patch[index].HasValue)
                continue;

            float value = patch[index]!.Value;
            if (!float.IsFinite(value))
            {
                after = Array.Empty<float>();
                componentMask = 0u;
                return false;
            }

            if (index >= 6)
            {
                if (value >= 0.0f &&
                    value < minimumScaleMagnitude)
                {
                    value = minimumScaleMagnitude;
                }
                else if (value < 0.0f &&
                         value > -minimumScaleMagnitude)
                {
                    value = -minimumScaleMagnitude;
                }
            }

            after[index] = value;
            componentMask |= 1u << index;
        }

        if (componentMask != 0u)
            return true;
        after = Array.Empty<float>();
        return false;
    }

    internal static bool ContainsNonRestorablePatchedScale(
        IReadOnlyList<float> before,
        IReadOnlyList<float?> patch)
    {
        ArgumentNullException.ThrowIfNull(before);
        ArgumentNullException.ThrowIfNull(patch);
        if (before.Count != TransformComponentCount ||
            patch.Count != TransformComponentCount)
        {
            return true;
        }

        const float minimumScaleMagnitude = 1.0e-4f;
        for (int index = 6; index < TransformComponentCount; ++index)
        {
            if (patch[index].HasValue &&
                (!float.IsFinite(before[index]) ||
                 MathF.Abs(before[index]) < minimumScaleMagnitude))
            {
                return true;
            }
        }

        return false;
    }

    internal static int[] NormalizeSelection(
        IEnumerable<int> nodeIds,
        int primaryNodeId)
    {
        ArgumentNullException.ThrowIfNull(nodeIds);
        var result = new List<int>();
        var seen = new HashSet<int>();
        foreach (int nodeId in nodeIds)
        {
            if (nodeId >= 0 && seen.Add(nodeId))
                result.Add(nodeId);
        }

        int primaryIndex = result.IndexOf(primaryNodeId);
        if (primaryIndex > 0)
        {
            result.RemoveAt(primaryIndex);
            result.Insert(0, primaryNodeId);
        }

        return result.ToArray();
    }

    internal static bool SameSelection(
        IReadOnlyList<int> left,
        IReadOnlyList<int> right)
    {
        ArgumentNullException.ThrowIfNull(left);
        ArgumentNullException.ThrowIfNull(right);
        if (left.Count != right.Count)
            return false;

        var remaining = new HashSet<int>(left);
        if (remaining.Count != left.Count)
            return false;
        foreach (int nodeId in right)
        {
            if (!remaining.Remove(nodeId))
                return false;
        }

        return remaining.Count == 0;
    }

    internal static string SelectionIdentity(
        IReadOnlyList<int> nodeIds)
    {
        ArgumentNullException.ThrowIfNull(nodeIds);
        var ordered = new List<int>(nodeIds);
        ordered.Sort();
        ulong hash = 14695981039346656037UL;
        foreach (int nodeId in ordered)
        {
            unchecked
            {
                hash ^= (uint)nodeId;
                hash *= 1099511628211UL;
            }
        }

        return hash.ToString("x16", CultureInfo.InvariantCulture);
    }

    /// <summary>
    /// Captures every target before the first write. If a write fails or throws,
    /// the failing target and every earlier target are restored in reverse order.
    /// Rollback continues after an individual restore failure so callers receive
    /// an honest consistency result instead of a partially hidden failure.
    /// </summary>
    internal static InspectorAtomicBatchResult ApplyAtomically<TState>(
        IReadOnlyList<int> nodeIds,
        InspectorStateCapture<TState> capture,
        Func<int, TState, bool> apply,
        Func<int, TState, bool> rollback)
    {
        ArgumentNullException.ThrowIfNull(nodeIds);
        ArgumentNullException.ThrowIfNull(capture);
        ArgumentNullException.ThrowIfNull(apply);
        ArgumentNullException.ThrowIfNull(rollback);

        if (nodeIds.Count == 0)
        {
            return new InspectorAtomicBatchResult(
                false,
                InspectorAtomicFailureStage.Validation,
                -1,
                true);
        }

        var seen = new HashSet<int>();
        var states = new List<TState>(nodeIds.Count);
        for (int index = 0; index < nodeIds.Count; ++index)
        {
            int nodeId = nodeIds[index];
            if (nodeId < 0 || !seen.Add(nodeId))
            {
                return new InspectorAtomicBatchResult(
                    false,
                    InspectorAtomicFailureStage.Validation,
                    nodeId,
                    true);
            }

            try
            {
                if (!capture(nodeId, out TState state))
                {
                    return new InspectorAtomicBatchResult(
                        false,
                        InspectorAtomicFailureStage.Capture,
                        nodeId,
                        true);
                }

                states.Add(state);
            }
            catch
            {
                return new InspectorAtomicBatchResult(
                    false,
                    InspectorAtomicFailureStage.Capture,
                    nodeId,
                    true);
            }
        }

        for (int index = 0; index < nodeIds.Count; ++index)
        {
            bool applied;
            try
            {
                applied = apply(nodeIds[index], states[index]);
            }
            catch
            {
                applied = false;
            }

            if (applied)
                continue;

            bool rollbackSucceeded = true;
            for (int rollbackIndex = index;
                 rollbackIndex >= 0;
                 --rollbackIndex)
            {
                try
                {
                    rollbackSucceeded &=
                        rollback(
                            nodeIds[rollbackIndex],
                            states[rollbackIndex]);
                }
                catch
                {
                    rollbackSucceeded = false;
                }
            }

            return new InspectorAtomicBatchResult(
                false,
                InspectorAtomicFailureStage.Apply,
                nodeIds[index],
                rollbackSucceeded);
        }

        return InspectorAtomicBatchResult.Success;
    }

    private static bool NearlyEqual(
        float left,
        float right,
        float relativeTolerance)
    {
        if (left.Equals(right))
            return true;
        float scale = MathF.Max(
            1.0f,
            MathF.Max(MathF.Abs(left), MathF.Abs(right)));
        return MathF.Abs(left - right) <= relativeTolerance * scale;
    }
}
