// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;

namespace AcsEditor;

internal enum InspectorReflectedPropertyKind
{
    Bool = 0,
    I32 = 1,
    U32 = 2,
    F32 = 3,
    Vec2 = 4,
    Vec3 = 5,
    Vec4 = 6,
    String = 7,
    Enum = 8,
    ObjectRef = 9,
}

[Flags]
internal enum InspectorReflectedPropertyFlags
{
    None = 0,
    ReadOnly = 1 << 0,
    Hidden = 1 << 1,
    Transient = 1 << 2,
    Color = 1 << 3,
}

internal sealed class InspectorReflectedPropertySchema
{
    private readonly ReadOnlyCollection<float> _defaults;

    private InspectorReflectedPropertySchema(
        string name,
        int index,
        InspectorReflectedPropertyKind kind,
        InspectorReflectedPropertyFlags flags,
        string category,
        bool hasDefault,
        IReadOnlyList<float> defaults)
    {
        Name = name;
        Index = index;
        Kind = kind;
        Flags = flags;
        Category = category;
        HasDefault = hasDefault;
        _defaults = Array.AsReadOnly(defaults.ToArray());
    }

    internal string Name { get; }

    internal int Index { get; }

    internal InspectorReflectedPropertyKind Kind { get; }

    internal InspectorReflectedPropertyFlags Flags { get; }

    internal string Category { get; }

    internal bool HasDefault { get; }

    internal IReadOnlyList<float> Defaults => _defaults;

    internal int ComponentCount =>
        InspectorReflectedMultiEditContract.ComponentCount(Kind);

    internal bool IsHidden =>
        Flags.HasFlag(InspectorReflectedPropertyFlags.Hidden);

    internal bool IsReadOnly =>
        Flags.HasFlag(InspectorReflectedPropertyFlags.ReadOnly);

    internal bool IsEditable =>
        !IsHidden &&
        !IsReadOnly &&
        Kind != InspectorReflectedPropertyKind.String &&
        ComponentCount > 0;

    internal bool CanReset => IsEditable && HasDefault;

    internal static bool TryCreate(
        string? name,
        int index,
        int kind,
        int flags,
        string? category,
        bool hasDefault,
        IReadOnlyList<float>? defaults,
        out InspectorReflectedPropertySchema? schema,
        out string failure)
    {
        schema = null;
        failure = "";
        if (string.IsNullOrWhiteSpace(name) ||
            name.Length > 256 ||
            !string.Equals(name, name.Trim(), StringComparison.Ordinal) ||
            name.Any(char.IsControl))
        {
            failure = "Reflected property name is empty, unbounded, or unsafe.";
            return false;
        }
        if (index < 0 || index >= 1024)
        {
            failure = "Reflected property index is outside the supported bound.";
            return false;
        }
        if (!Enum.IsDefined((InspectorReflectedPropertyKind)kind))
        {
            failure = "Reflected property kind is unknown.";
            return false;
        }

        const InspectorReflectedPropertyFlags knownFlags =
            InspectorReflectedPropertyFlags.ReadOnly |
            InspectorReflectedPropertyFlags.Hidden |
            InspectorReflectedPropertyFlags.Transient |
            InspectorReflectedPropertyFlags.Color;
        var typedFlags = (InspectorReflectedPropertyFlags)flags;
        if ((typedFlags & ~knownFlags) != 0)
        {
            failure = "Reflected property contains unknown flag bits.";
            return false;
        }

        string normalizedCategory = category ?? "";
        if (normalizedCategory.Length > 256 ||
            normalizedCategory.Any(char.IsControl))
        {
            failure = "Reflected property category is unbounded or unsafe.";
            return false;
        }

        float[] defaultValues;
        if (defaults is null)
        {
            if (hasDefault)
            {
                failure = "Reflected property default payload is missing.";
                return false;
            }
            defaultValues = new float[4];
        }
        else
        {
            if (defaults.Count != 4 ||
                defaults.Any(static value => !float.IsFinite(value)))
            {
                failure =
                    "Reflected property default payload is not four finite floats.";
                return false;
            }
            defaultValues = defaults.ToArray();
        }

        var typedKind = (InspectorReflectedPropertyKind)kind;
        if (hasDefault &&
            !InspectorReflectedMultiEditContract.TryCanonicalizeActiveValues(
                typedKind,
                defaultValues,
                requireAlreadyCanonical: true,
                out _))
        {
            failure =
                "Reflected property default does not match its declared kind.";
            return false;
        }

        schema = new InspectorReflectedPropertySchema(
            name,
            index,
            typedKind,
            typedFlags,
            normalizedCategory,
            hasDefault,
            defaultValues);
        return true;
    }

    internal bool IsCompatibleWith(
        InspectorReflectedPropertySchema other)
    {
        ArgumentNullException.ThrowIfNull(other);
        if (!string.Equals(Name, other.Name, StringComparison.Ordinal) ||
            Kind != other.Kind ||
            Flags != other.Flags ||
            !string.Equals(
                Category,
                other.Category,
                StringComparison.Ordinal) ||
            HasDefault != other.HasDefault)
        {
            return false;
        }
        for (int index = 0; index < 4; ++index)
        {
            if (BitConverter.SingleToInt32Bits(Defaults[index]) !=
                BitConverter.SingleToInt32Bits(other.Defaults[index]))
            {
                return false;
            }
        }
        return true;
    }
}

internal sealed record InspectorReflectedComponentSnapshot(
    int NodeId,
    int Slot,
    string TypeName,
    IReadOnlyList<InspectorReflectedPropertySchema> Properties);

internal sealed record InspectorReflectedNodeSnapshot(
    int NodeId,
    IReadOnlyList<InspectorReflectedComponentSnapshot> Components);

internal readonly record struct InspectorReflectedComponentTarget(
    int NodeId,
    int Slot);

internal readonly record struct InspectorReflectedPropertyTarget(
    int NodeId,
    int Slot,
    int PropertyIndex);

internal sealed record InspectorReflectedCommonProperty(
    InspectorReflectedPropertySchema Schema,
    IReadOnlyList<InspectorReflectedPropertyTarget> Targets);

internal sealed record InspectorReflectedCommonComponent(
    string TypeName,
    IReadOnlyList<InspectorReflectedComponentTarget> Targets,
    IReadOnlyList<InspectorReflectedCommonProperty> Properties);

internal static class InspectorReflectedMultiEditContract
{
    private const float MaximumExactInteger = 16_777_216.0f;

    internal static int ComponentCount(
        InspectorReflectedPropertyKind kind) => kind switch
    {
        InspectorReflectedPropertyKind.Bool or
        InspectorReflectedPropertyKind.I32 or
        InspectorReflectedPropertyKind.U32 or
        InspectorReflectedPropertyKind.F32 or
        InspectorReflectedPropertyKind.Enum or
        InspectorReflectedPropertyKind.ObjectRef => 1,
        InspectorReflectedPropertyKind.Vec2 => 2,
        InspectorReflectedPropertyKind.Vec3 => 3,
        InspectorReflectedPropertyKind.Vec4 => 4,
        InspectorReflectedPropertyKind.String => 0,
        _ => -1,
    };

    internal static bool TryIntersect(
        IReadOnlyList<InspectorReflectedNodeSnapshot> nodes,
        out IReadOnlyList<InspectorReflectedCommonComponent> common,
        out string failure)
    {
        ArgumentNullException.ThrowIfNull(nodes);
        common = Array.Empty<InspectorReflectedCommonComponent>();
        failure = "";
        if (nodes.Count < 2)
        {
            failure =
                "Reflected multi-edit requires at least two selected nodes.";
            return false;
        }

        var nodeIds = new HashSet<int>();
        var componentsByNode =
            new List<Dictionary<string, InspectorReflectedComponentSnapshot>>(
                nodes.Count);
        foreach (InspectorReflectedNodeSnapshot node in nodes)
        {
            if (node.NodeId < 0 || !nodeIds.Add(node.NodeId))
            {
                failure =
                    "Reflected multi-edit node identities are invalid or duplicated.";
                return false;
            }

            var types = new Dictionary<
                string,
                InspectorReflectedComponentSnapshot>(
                StringComparer.Ordinal);
            var slots = new HashSet<int>();
            foreach (InspectorReflectedComponentSnapshot component in
                     node.Components)
            {
                if (component.NodeId != node.NodeId ||
                    component.Slot < 0 ||
                    component.Slot >= 1024 ||
                    !slots.Add(component.Slot) ||
                    string.IsNullOrWhiteSpace(component.TypeName) ||
                    component.TypeName.Length > 256 ||
                    !string.Equals(
                        component.TypeName,
                        component.TypeName.Trim(),
                        StringComparison.Ordinal) ||
                    component.TypeName.Any(char.IsControl) ||
                    !types.TryAdd(component.TypeName, component) ||
                    !ValidatePropertySet(component.Properties))
                {
                    failure =
                        "Reflected component topology or schema is invalid.";
                    return false;
                }
            }
            componentsByNode.Add(types);
        }

        var result = new List<InspectorReflectedCommonComponent>();
        foreach (InspectorReflectedComponentSnapshot firstComponent in
                 nodes[0].Components)
        {
            var matched =
                new InspectorReflectedComponentSnapshot[nodes.Count];
            matched[0] = firstComponent;
            bool presentOnEveryNode = true;
            for (int nodeIndex = 1;
                 nodeIndex < nodes.Count;
                 ++nodeIndex)
            {
                if (!componentsByNode[nodeIndex].TryGetValue(
                        firstComponent.TypeName,
                        out InspectorReflectedComponentSnapshot? component))
                {
                    presentOnEveryNode = false;
                    break;
                }
                matched[nodeIndex] = component;
            }
            if (!presentOnEveryNode)
                continue;

            var propertyMaps = new Dictionary<
                string,
                InspectorReflectedPropertySchema>[matched.Length];
            for (int nodeIndex = 0;
                 nodeIndex < matched.Length;
                 ++nodeIndex)
            {
                propertyMaps[nodeIndex] =
                    matched[nodeIndex].Properties.ToDictionary(
                        static property => property.Name,
                        StringComparer.Ordinal);
            }

            var commonProperties =
                new List<InspectorReflectedCommonProperty>();
            foreach (InspectorReflectedPropertySchema firstProperty in
                     firstComponent.Properties)
            {
                var targets =
                    new InspectorReflectedPropertyTarget[nodes.Count];
                targets[0] = new(
                    nodes[0].NodeId,
                    firstComponent.Slot,
                    firstProperty.Index);
                bool compatible = true;
                for (int nodeIndex = 1;
                     nodeIndex < nodes.Count;
                     ++nodeIndex)
                {
                    if (!propertyMaps[nodeIndex].TryGetValue(
                            firstProperty.Name,
                            out InspectorReflectedPropertySchema? property) ||
                        !firstProperty.IsCompatibleWith(property))
                    {
                        compatible = false;
                        break;
                    }
                    targets[nodeIndex] = new(
                        nodes[nodeIndex].NodeId,
                        matched[nodeIndex].Slot,
                        property.Index);
                }
                if (compatible)
                {
                    commonProperties.Add(new(
                        firstProperty,
                        Array.AsReadOnly(targets)));
                }
            }

            InspectorReflectedComponentTarget[] componentTargets =
                matched.Select(static component =>
                        new InspectorReflectedComponentTarget(
                            component.NodeId,
                            component.Slot))
                    .ToArray();
            result.Add(new(
                firstComponent.TypeName,
                Array.AsReadOnly(componentTargets),
                commonProperties.AsReadOnly()));
        }

        common = result.AsReadOnly();
        return true;
    }

    internal static InspectorMixedFloat[] ResolveMixedComponents(
        InspectorReflectedPropertySchema schema,
        IReadOnlyList<IReadOnlyList<float>> values)
    {
        ArgumentNullException.ThrowIfNull(schema);
        ArgumentNullException.ThrowIfNull(values);
        if (values.Count == 0 ||
            values.Any(static value => value.Count != 4) ||
            values.SelectMany(static value => value)
                .Any(static value => !float.IsFinite(value)))
        {
            throw new ArgumentException(
                "Reflected mixed-value input must contain finite float4 values.",
                nameof(values));
        }

        var result = new InspectorMixedFloat[schema.ComponentCount];
        var componentValues = new float[values.Count];
        for (int component = 0;
             component < result.Length;
             ++component)
        {
            for (int valueIndex = 0;
                 valueIndex < values.Count;
                 ++valueIndex)
            {
                componentValues[valueIndex] =
                    values[valueIndex][component];
            }
            result[component] =
                InspectorMultiEditContract.ResolveFloat(componentValues);
        }
        return result;
    }

    internal static InspectorMixedBool ResolveMixedBool(
        IReadOnlyList<IReadOnlyList<float>> values)
    {
        ArgumentNullException.ThrowIfNull(values);
        if (values.Count == 0 ||
            values.Any(static value =>
                value.Count != 4 ||
                !float.IsFinite(value[0])))
        {
            throw new ArgumentException(
                "Reflected bool input must contain finite float4 values.",
                nameof(values));
        }
        return InspectorMultiEditContract.ResolveBool(
            values.Select(static value => value[0] != 0.0f).ToArray());
    }

    internal static bool TryBuildMutation(
        InspectorReflectedPropertySchema schema,
        IReadOnlyList<float> before,
        IReadOnlyList<float?> patch,
        out float[] after)
    {
        ArgumentNullException.ThrowIfNull(schema);
        ArgumentNullException.ThrowIfNull(before);
        ArgumentNullException.ThrowIfNull(patch);
        after = Array.Empty<float>();
        if (!schema.IsEditable ||
            before.Count != 4 ||
            patch.Count != 4 ||
            before.Any(static value => !float.IsFinite(value)))
        {
            return false;
        }

        int componentCount = schema.ComponentCount;
        bool hasValue = false;
        var candidate = before.ToArray();
        for (int component = 0; component < 4; ++component)
        {
            if (!patch[component].HasValue)
                continue;
            if (component >= componentCount ||
                !float.IsFinite(patch[component]!.Value))
            {
                return false;
            }
            candidate[component] = patch[component]!.Value;
            hasValue = true;
        }
        if (!hasValue ||
            !TryCanonicalizeActiveValues(
                schema.Kind,
                candidate,
                requireAlreadyCanonical: false,
                out float[] canonical))
        {
            return false;
        }

        // Canonicalization applies only to edited axes. Unedited authored
        // values, including legacy bool/integer payloads, remain bit-exact so
        // rollback can restore them without hidden mutation.
        for (int component = 0; component < componentCount; ++component)
        {
            if (patch[component].HasValue)
                candidate[component] = canonical[component];
        }
        after = candidate;
        return true;
    }

    internal static bool TryBuildDefaultPatch(
        InspectorReflectedPropertySchema schema,
        out float?[] patch)
    {
        ArgumentNullException.ThrowIfNull(schema);
        patch = new float?[4];
        if (!schema.CanReset)
            return false;
        for (int component = 0;
             component < schema.ComponentCount;
             ++component)
        {
            patch[component] = schema.Defaults[component];
        }
        return true;
    }

    internal static bool ValuesEqual(
        IReadOnlyList<float> left,
        IReadOnlyList<float> right)
    {
        ArgumentNullException.ThrowIfNull(left);
        ArgumentNullException.ThrowIfNull(right);
        if (left.Count != 4 || right.Count != 4)
            return false;
        for (int index = 0; index < 4; ++index)
        {
            if (!float.IsFinite(left[index]) ||
                !float.IsFinite(right[index]) ||
                BitConverter.SingleToInt32Bits(left[index]) !=
                    BitConverter.SingleToInt32Bits(right[index]))
            {
                return false;
            }
        }
        return true;
    }

    internal static bool TryCanonicalizeActiveValues(
        InspectorReflectedPropertyKind kind,
        IReadOnlyList<float> values,
        bool requireAlreadyCanonical,
        out float[] canonical)
    {
        canonical = Array.Empty<float>();
        int componentCount = ComponentCount(kind);
        if (values.Count != 4 ||
            componentCount < 0 ||
            values.Any(static value => !float.IsFinite(value)))
        {
            return false;
        }

        canonical = values.ToArray();
        switch (kind)
        {
            case InspectorReflectedPropertyKind.Bool:
                canonical[0] = values[0] == 0.0f ? 0.0f : 1.0f;
                break;
            case InspectorReflectedPropertyKind.I32:
            case InspectorReflectedPropertyKind.Enum:
            case InspectorReflectedPropertyKind.ObjectRef:
                if (MathF.Abs(values[0]) > MaximumExactInteger)
                    return false;
                canonical[0] = MathF.Round(values[0]);
                break;
            case InspectorReflectedPropertyKind.U32:
                if (values[0] < 0.0f ||
                    values[0] > MaximumExactInteger)
                {
                    return false;
                }
                canonical[0] = MathF.Round(values[0]);
                break;
            case InspectorReflectedPropertyKind.F32:
            case InspectorReflectedPropertyKind.Vec2:
            case InspectorReflectedPropertyKind.Vec3:
            case InspectorReflectedPropertyKind.Vec4:
            case InspectorReflectedPropertyKind.String:
                break;
            default:
                return false;
        }

        if (requireAlreadyCanonical)
        {
            for (int component = 0;
                 component < componentCount;
                 ++component)
            {
                if (BitConverter.SingleToInt32Bits(values[component]) !=
                    BitConverter.SingleToInt32Bits(canonical[component]))
                {
                    canonical = Array.Empty<float>();
                    return false;
                }
            }
        }
        return true;
    }

    private static bool ValidatePropertySet(
        IReadOnlyList<InspectorReflectedPropertySchema> properties)
    {
        if (properties.Count > 1024)
            return false;
        var names = new HashSet<string>(StringComparer.Ordinal);
        var indices = new HashSet<int>();
        foreach (InspectorReflectedPropertySchema property in properties)
        {
            if (property is null ||
                !names.Add(property.Name) ||
                !indices.Add(property.Index))
            {
                return false;
            }
        }
        return true;
    }
}
