// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;

namespace AcsEditor;

/// <summary>3D Prefab root component selective Applyの純粋計算契約を検証する。</summary>
internal static class PrefabRootComponentPropertyApply3DSelfTest
{
    /// <summary>型解決、選択値更新、入力保持、fail-closedを検証して失敗数を返す。</summary>
    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int passed = 0;
        int failed = 0;

        void Check(bool condition, string label, string detail = "")
        {
            if (condition)
            {
                passed++;
                output.WriteLine("PASS  " + label);
                return;
            }
            failed++;
            output.WriteLine("FAIL  " + label);
            if (detail.Length > 0) output.WriteLine("      " + detail);
        }

        const string source =
            "ACS3D v2\r\n" +
            "N3D 10 -1 0 0 0 0 0 0 0 1 1 1 0.100 0.200 0.300 1.000 Root\r\n" +
            "CMP3D 10 AMeshComponent3D\r\n" +
            "CPROP3D 10 0 1 0.2000 0.0000 0.0000 0.0000\r\n" +
            "CMP3D 10 AWaterSurface3DComponent\r\n" +
            "CPROP3D 10 1 4 0.1000 0.2000 0.3000 0.4000\r\n" +
            "N3D 11 10 0 1 2 3 0 0 0 1 1 1 0.400 0.500 0.600 1.000 Child\r\n" +
            "CMP3D 11 AWaterSurface3DComponent\r\n" +
            "CPROP3D 11 0 4 0.5000 0.6000 0.7000 0.8000\r\n";
        var value = new PrefabRootComponentPropertyValue3D(0.9f, 0.8f, 0.7f, 0.6f);
        bool changed = PrefabRootComponentPropertyApply3D.TryBuildSource(
            source,
            "AWaterSurface3DComponent",
            4,
            value,
            out string changedSource,
            out string changedError);
        const string expected =
            "ACS3D v2\r\n" +
            "N3D 10 -1 0 0 0 0 0 0 0 1 1 1 0.100 0.200 0.300 1.000 Root\r\n" +
            "CMP3D 10 AMeshComponent3D\r\n" +
            "CPROP3D 10 0 1 0.2000 0.0000 0.0000 0.0000\r\n" +
            "CMP3D 10 AWaterSurface3DComponent\r\n" +
            "CPROP3D 10 1 4 0.9000 0.8000 0.7000 0.6000\r\n" +
            "N3D 11 10 0 1 2 3 0 0 0 1 1 1 0.400 0.500 0.600 1.000 Child\r\n" +
            "CMP3D 11 AWaterSurface3DComponent\r\n" +
            "CPROP3D 11 0 4 0.5000 0.6000 0.7000 0.8000\r\n";
        Check(
            changed && changedSource == expected,
            "type identity updates only the selected root component property and preserves CRLF",
            changedError);

        const string missingProperty =
            "ACS3D v2\n" +
            "N3D 4 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n" +
            "CMP3D 4 AWaterSurface3DComponent\n" +
            "FLG3D 4 0 1\n";
        bool inserted = PrefabRootComponentPropertyApply3D.TryBuildSource(
            missingProperty,
            "AWaterSurface3DComponent",
            4,
            value,
            out string insertedSource,
            out string insertedError);
        const string expectedInserted =
            "ACS3D v2\n" +
            "N3D 4 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n" +
            "CMP3D 4 AWaterSurface3DComponent\n" +
            "CPROP3D 4 0 4 0.9000 0.8000 0.7000 0.6000\n" +
            "FLG3D 4 0 1\n";
        Check(
            inserted && insertedSource == expectedInserted,
            "missing target CPROP3D is inserted after its component without changing other directives",
            insertedError);

        const string reordered =
            "ACS3D v2\n" +
            "N3D 8 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n" +
            "CMP3D 8 AWaterSurface3DComponent\n" +
            "CPROP3D 8 0 4 0.1000 0 0 0\n" +
            "CMP3D 8 AMeshComponent3D\n" +
            "CPROP3D 8 1 1 0.3000 0 0 0\n";
        bool reorderedChanged = PrefabRootComponentPropertyApply3D.TryBuildSource(
            reordered,
            "AWaterSurface3DComponent",
            4,
            value,
            out string reorderedSource,
            out string reorderedError);
        Check(
            reorderedChanged &&
            reorderedSource.Contains("CPROP3D 8 0 4 0.9000 0.8000 0.7000 0.6000\n", StringComparison.Ordinal) &&
            reorderedSource.Contains("CPROP3D 8 1 1 0.3000 0 0 0\n", StringComparison.Ordinal),
            "source component reorder is resolved by type instead of the current instance slot",
            reorderedError);

        const string blueprint =
            "ACSBP 1\n" +
            "VAR Speed 3\n" +
            "CMP 4\n" +
            missingProperty +
            "C 1 Gameplay\n";
        string blueprintComponents = AcsbpFormat.ExtractCmp(blueprint);
        bool blueprintChanged = PrefabRootComponentPropertyApply3D.TryBuildSource(
            blueprintComponents,
            "AWaterSurface3DComponent",
            4,
            value,
            out string updatedBlueprintComponents,
            out string blueprintError);
        string updatedBlueprint = AcsbpFormat.ReplaceCmp(blueprint, updatedBlueprintComponents);
        Check(
            blueprintChanged &&
            updatedBlueprint.Contains("VAR Speed 3\n", StringComparison.Ordinal) &&
            updatedBlueprint.Contains("C 1 Gameplay\n", StringComparison.Ordinal) &&
            AcsbpFormat.ExtractCmp(updatedBlueprint) == updatedBlueprintComponents,
            "Blueprint selective component Apply preserves VAR and graph sections",
            blueprintError);

        string duplicateType = source.Replace(
            "CMP3D 10 AWaterSurface3DComponent\r\n",
            "CMP3D 10 AWaterSurface3DComponent\r\nCMP3D 10 AWaterSurface3DComponent\r\n",
            StringComparison.Ordinal);
        bool duplicateRejected = !PrefabRootComponentPropertyApply3D.TryBuildSource(
            duplicateType,
            "AWaterSurface3DComponent",
            4,
            value,
            out string duplicateSource,
            out _);
        Check(
            duplicateRejected && duplicateSource == duplicateType,
            "duplicate root component type fails without changing source text");

        bool missingTypeRejected = !PrefabRootComponentPropertyApply3D.TryBuildSource(
            source,
            "ADirectionalLight3DComponent",
            4,
            value,
            out string missingTypeSource,
            out _);
        Check(
            missingTypeRejected && missingTypeSource == source,
            "missing root component type fails without changing source text");

        string multipleRoots = source.Replace(
            "N3D 11 10",
            "N3D 11 -1",
            StringComparison.Ordinal);
        bool rootsRejected = !PrefabRootComponentPropertyApply3D.TryBuildSource(
            multipleRoots,
            "AWaterSurface3DComponent",
            4,
            value,
            out string rootsSource,
            out _);
        Check(
            rootsRejected && rootsSource == multipleRoots,
            "multiple Prefab roots fail closed");

        bool propertyRejected = !PrefabRootComponentPropertyApply3D.TryBuildSource(
            source,
            "AWaterSurface3DComponent",
            32,
            value,
            out string propertySource,
            out _);
        Check(
            propertyRejected && propertySource == source,
            "out-of-range property fails without changing source text");

        bool valueRejected = !PrefabRootComponentPropertyApply3D.TryBuildSource(
            source,
            "AWaterSurface3DComponent",
            4,
            value with { X = float.NaN },
            out string valueSource,
            out _);
        Check(
            valueRejected && valueSource == source,
            "non-finite component value fails without changing source text");

        output.WriteLine(
            $"Prefab root component property Apply 3D self-test: passed={passed} failed={failed}");
        return failed;
    }
}
