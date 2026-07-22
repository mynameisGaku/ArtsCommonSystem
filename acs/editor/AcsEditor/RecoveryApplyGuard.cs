// SPDX-License-Identifier: Apache-2.0

using System;

namespace AcsEditor;

internal static class RecoveryApplyGuard
{
    /// <summary>
    /// Recovery may replace native state only when a clean baseline exists and the latest
    /// dispatcher-thread serialization still matches it exactly.
    /// </summary>
    internal static bool CanReplace(
        string? cleanBaseline,
        string latestNativeSnapshot) =>
        cleanBaseline != null &&
        string.Equals(
            cleanBaseline,
            latestNativeSnapshot,
            StringComparison.Ordinal);
}
