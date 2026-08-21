// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;

namespace AcsEditor;

/// <summary>3D Prefab root selective Applyの純粋計算契約を検証する。</summary>
internal static class PrefabRootPropertyApply3DSelfTest
{
    /// <summary>選択値だけの更新、入力保持、fail-closedを検証して失敗数を返す。</summary>
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
            "N3D 10 -1 0 0 0 0 0 0 0 1 1 1 0.100 0.200 0.300 1.000 Root With Space\r\n" +
            "N3D 11 10 0 1 2 3 0 0 0 1 1 1 0.400 0.500 0.600 1.000 Child\r\n" +
            "FLG3D 11 0 1\r\n";
        var values = new PrefabRootPropertyValues3D(
            Visible: false,
            Enabled: false,
            Red: 0.9f,
            Green: 0.8f,
            Blue: 0.7f,
            Alpha: 0.6f);

        bool visibleOk = PrefabRootPropertyApply3D.TryBuildSource(
            source,
            PrefabRootProperty3D.Visible,
            values,
            out string visibleSource,
            out string visibleError);
        const string expectedVisible =
            "ACS3D v2\r\n" +
            "N3D 10 -1 0 0 0 0 0 0 0 1 1 1 0.100 0.200 0.300 1.000 Root With Space\r\n" +
            "FLG3D 10 0 1\r\n" +
            "N3D 11 10 0 1 2 3 0 0 0 1 1 1 0.400 0.500 0.600 1.000 Child\r\n" +
            "FLG3D 11 0 1\r\n";
        Check(
            visibleOk && visibleSource == expectedVisible,
            "Visible Apply adds only the missing root flag and preserves CRLF",
            visibleError);

        bool visibleDefaultOk = PrefabRootPropertyApply3D.TryBuildSource(
            source,
            PrefabRootProperty3D.Visible,
            values with { Visible = true },
            out string visibleDefaultSource,
            out string visibleDefaultError);
        Check(
            visibleDefaultOk && visibleDefaultSource == source,
            "default Visible Apply does not add an unnecessary root flag",
            visibleDefaultError);

        bool enabledOk = PrefabRootPropertyApply3D.TryBuildSource(
            expectedVisible,
            PrefabRootProperty3D.Enabled,
            values,
            out string enabledSource,
            out string enabledError);
        const string expectedEnabled =
            "ACS3D v2\r\n" +
            "N3D 10 -1 0 0 0 0 0 0 0 1 1 1 0.100 0.200 0.300 1.000 Root With Space\r\n" +
            "FLG3D 10 0 0\r\n" +
            "N3D 11 10 0 1 2 3 0 0 0 1 1 1 0.400 0.500 0.600 1.000 Child\r\n" +
            "FLG3D 11 0 1\r\n";
        Check(
            enabledOk && enabledSource == expectedEnabled,
            "Enabled Apply preserves the unselected Visible source value",
            enabledError);

        bool colorOk = PrefabRootPropertyApply3D.TryBuildSource(
            expectedEnabled,
            PrefabRootProperty3D.Color,
            values,
            out string colorSource,
            out string colorError);
        const string expectedColor =
            "ACS3D v2\r\n" +
            "N3D 10 -1 0 0 0 0 0 0 0 1 1 1 0.900 0.800 0.700 0.600 Root With Space\r\n" +
            "FLG3D 10 0 0\r\n" +
            "N3D 11 10 0 1 2 3 0 0 0 1 1 1 0.400 0.500 0.600 1.000 Child\r\n" +
            "FLG3D 11 0 1\r\n";
        Check(
            colorOk && colorSource == expectedColor,
            "Color Apply changes only root RGBA and leaves child data untouched",
            colorError);

        const string blueprint =
            "ACSBP 1\n" +
            "VAR Speed 3\n" +
            "CMP 2\n" +
            "ACS3D v2\n" +
            "N3D 10 -1 0 0 0 0 0 0 0 1 1 1 0.100 0.200 0.300 1.000 Root\n" +
            "C 1 Gameplay\n";
        string blueprintComponents = AcsbpFormat.ExtractCmp(blueprint);
        bool blueprintOk = PrefabRootPropertyApply3D.TryBuildSource(
            blueprintComponents,
            PrefabRootProperty3D.Color,
            values,
            out string updatedBlueprintComponents,
            out string blueprintError);
        string updatedBlueprint = AcsbpFormat.ReplaceCmp(
            blueprint,
            updatedBlueprintComponents);
        Check(
            blueprintOk &&
            updatedBlueprint.Contains("VAR Speed 3\n", StringComparison.Ordinal) &&
            updatedBlueprint.Contains("C 1 Gameplay\n", StringComparison.Ordinal) &&
            AcsbpFormat.ExtractCmp(updatedBlueprint) == updatedBlueprintComponents,
            "Blueprint selective Apply preserves VAR and graph sections",
            blueprintError);

        bool noneRejected = !PrefabRootPropertyApply3D.TryBuildSource(
            source,
            PrefabRootProperty3D.None,
            values,
            out string noneSource,
            out _);
        Check(
            noneRejected && noneSource == source,
            "zero Apply mask fails without changing source text");

        bool unknownRejected = !PrefabRootPropertyApply3D.TryBuildSource(
            source,
            (PrefabRootProperty3D)(1u << 8),
            values,
            out string unknownSource,
            out _);
        Check(
            unknownRejected && unknownSource == source,
            "unknown Apply mask fails without changing source text");

        string multipleRoots = source.Replace(
            "N3D 11 10",
            "N3D 11 -1",
            StringComparison.Ordinal);
        bool rootsRejected = !PrefabRootPropertyApply3D.TryBuildSource(
            multipleRoots,
            PrefabRootProperty3D.Visible,
            values,
            out string rootsSource,
            out _);
        Check(
            rootsRejected && rootsSource == multipleRoots,
            "multiple Prefab roots fail closed");

        string duplicateFlags = source + "FLG3D 10 0 1\r\nFLG3D 10 1 0\r\n";
        bool flagsRejected = !PrefabRootPropertyApply3D.TryBuildSource(
            duplicateFlags,
            PrefabRootProperty3D.Color,
            values,
            out string flagsSource,
            out _);
        Check(
            flagsRejected && flagsSource == duplicateFlags,
            "duplicate root flags fail closed");

        var invalidColor = values with { Red = float.NaN };
        bool colorRejected = !PrefabRootPropertyApply3D.TryBuildSource(
            source,
            PrefabRootProperty3D.Color,
            invalidColor,
            out string invalidColorSource,
            out _);
        Check(
            colorRejected && invalidColorSource == source,
            "non-finite Color fails without changing source text");

        output.WriteLine(
            $"Prefab root property Apply 3D self-test: passed={passed} failed={failed}");
        return failed;
    }
}
