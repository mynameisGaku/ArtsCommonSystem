// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;

namespace AcsEditor;

/// <summary>3D Prefab child node selective Applyの純粋計算契約を検証する。</summary>
internal static class PrefabNodePropertyApply3DSelfTest
{
    /// <summary>source ID解決、選択値更新、入力保持、fail-closedを検証して失敗数を返す。</summary>
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

        const string targetId = "fedcba9876543210fedcba9876543210";
        const string source =
            "ACS3D v2\r\n" +
            "N3D 10 -1 0 0 0 0 0 0 0 1 1 1 0.100 0.200 0.300 1.000 Root\r\n" +
            "PSID3D 10 0123456789abcdef0123456789abcdef\r\n" +
            "N3D 11 10 0 1 2 3 0 0 0 1 1 1 0.400 0.500 0.600 1.000 Wheel\r\n" +
            "PSID3D 11 fedcba9876543210fedcba9876543210\r\n" +
            "N3D 12 10 0 4 5 6 0 0 0 1 1 1 0.700 0.800 0.900 1.000 Door\r\n" +
            "PSID3D 12 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n" +
            "FLG3D 12 0 1\r\n";
        var values = new PrefabNodePropertyValues3D(
            Visible: false,
            Enabled: false,
            Red: 0.9f,
            Green: 0.8f,
            Blue: 0.7f,
            Alpha: 0.6f);

        bool flagsOk = PrefabNodePropertyApply3D.TryBuildSource(
            source,
            targetId,
            PrefabNodeProperty3D.Visible | PrefabNodeProperty3D.Enabled,
            values,
            out string flagsSource,
            out string flagsError);
        Check(
            flagsOk &&
            flagsSource.Contains("FLG3D 11 0 0\r\n", StringComparison.Ordinal) &&
            flagsSource.Contains("FLG3D 12 0 1\r\n", StringComparison.Ordinal),
            "source ID selects only the matching child flags and preserves CRLF",
            flagsError);

        bool colorOk = PrefabNodePropertyApply3D.TryBuildSource(
            flagsSource,
            targetId,
            PrefabNodeProperty3D.Color,
            values,
            out string colorSource,
            out string colorError);
        Check(
            colorOk &&
            colorSource.Contains("N3D 11 10 0 1 2 3 0 0 0 1 1 1 0.900 0.800 0.700 0.600 Wheel", StringComparison.Ordinal) &&
            colorSource.Contains("N3D 12 10 0 4 5 6 0 0 0 1 1 1 0.700 0.800 0.900 1.000 Door", StringComparison.Ordinal),
            "Color Apply changes only the source-identified child",
            colorError);

        string renumbered = source
            .Replace("N3D 11 10", "N3D 201 10", StringComparison.Ordinal)
            .Replace("PSID3D 11 ", "PSID3D 201 ", StringComparison.Ordinal);
        bool renumberedOk = PrefabNodePropertyApply3D.TryBuildSource(
            renumbered,
            targetId,
            PrefabNodeProperty3D.Visible,
            values,
            out string renumberedSource,
            out string renumberedError);
        Check(
            renumberedOk && renumberedSource.Contains("FLG3D 201 0 1", StringComparison.Ordinal),
            "source ID remains valid after numeric node ID changes",
            renumberedError);

        bool invalidIdRejected = !PrefabNodePropertyApply3D.TryBuildSource(
            source,
            targetId.ToUpperInvariant(),
            PrefabNodeProperty3D.Visible,
            values,
            out string invalidIdSource,
            out _);
        Check(invalidIdRejected && invalidIdSource == source, "non-canonical source ID fails without changing input");

        string duplicateIdentity = source.Replace(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            targetId,
            StringComparison.Ordinal);
        bool duplicateRejected = !PrefabNodePropertyApply3D.TryBuildSource(
            duplicateIdentity,
            targetId,
            PrefabNodeProperty3D.Visible,
            values,
            out string duplicateSource,
            out _);
        Check(duplicateRejected && duplicateSource == duplicateIdentity, "ambiguous source ID fails without changing input");

        bool unknownMaskRejected = !PrefabNodePropertyApply3D.TryBuildSource(
            source,
            targetId,
            (PrefabNodeProperty3D)(1u << 8),
            values,
            out string unknownMaskSource,
            out _);
        Check(unknownMaskRejected && unknownMaskSource == source, "unknown property bit fails without changing input");

        output.WriteLine($"Prefab node property Apply 3D self-test: passed={passed} failed={failed}");
        return failed;
    }
}
