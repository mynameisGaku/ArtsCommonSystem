// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;

namespace AcsEditor;

/// <summary>3D Prefab source node identity migrationの純粋計算契約を検証する。</summary>
internal static class PrefabNodeIdentity3DSelfTest
{
    /// <summary>補完、既存値保持、決定性、fail-closedを検証して失敗数を返す。</summary>
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

        const string sourceIdentity = "C:/Project/Assets/Vehicle.acsprefab";
        const string legacy =
            "ACS3D v2\r\n" +
            "N3D 10 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\r\n" +
            "N3D 11 10 0 1 2 3 0 0 0 1 1 1 1 1 1 1 Child\r\n";
        bool migrated = PrefabNodeIdentity3D.TryEnsureSource(sourceIdentity, legacy, out string identified, out int added, out string migrationError);
        const string expected =
            "ACS3D v2\r\n" +
            "N3D 10 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\r\n" +
            "PSID3D 10 323d728eb27c59fbc55510dcc9caf6c0\r\n" +
            "N3D 11 10 0 1 2 3 0 0 0 1 1 1 1 1 1 1 Child\r\n" +
            "PSID3D 11 9242c6865c469d6a654fbce52000b351\r\n";
        Check(migrated && added == 2 && identified == expected, "legacy source receives deterministic identities and preserves CRLF", migrationError);

        bool unchanged = PrefabNodeIdentity3D.TryEnsureSource(sourceIdentity, identified, out string unchangedText, out int unchangedAdded, out string unchangedError);
        Check(unchanged && unchangedAdded == 0 && unchangedText == identified, "repeated migration is idempotent", unchangedError);

        Check(
            PrefabNodeIdentity3D.BuildSourceNodeId(sourceIdentity, 10) == "323d728eb27c59fbc55510dcc9caf6c0" &&
            PrefabNodeIdentity3D.BuildSourceNodeId(sourceIdentity, 11) == "9242c6865c469d6a654fbce52000b351" &&
            PrefabNodeIdentity3D.BuildSourceNodeId("Assets/Vehicle.acsprefab", 1) == "bc552ad841b671fae2c7b3e0dacc6803",
            "identity calculation is deterministic and matches the native legacy fallback");

        string duplicateIdentity = expected.Replace(
            "9242c6865c469d6a654fbce52000b351",
            "323d728eb27c59fbc55510dcc9caf6c0",
            StringComparison.Ordinal);
        bool duplicateRejected = !PrefabNodeIdentity3D.TryEnsureSource(sourceIdentity, duplicateIdentity, out string duplicateResult, out int duplicateAdded, out _);
        Check(duplicateRejected && duplicateAdded == 0 && duplicateResult == duplicateIdentity, "duplicate source node identity fails without changing input");

        string malformed = expected.Replace(
            "323d728eb27c59fbc55510dcc9caf6c0",
            "323D728EB27C59FBC55510DCC9CAF6C0",
            StringComparison.Ordinal);
        bool malformedRejected = !PrefabNodeIdentity3D.TryEnsureSource(sourceIdentity, malformed, out string malformedResult, out _, out _);
        Check(malformedRejected && malformedResult == malformed, "non-canonical source node identity fails closed");

        output.WriteLine($"Prefab node identity 3D self-test: passed={passed} failed={failed}");
        return failed;
    }
}
