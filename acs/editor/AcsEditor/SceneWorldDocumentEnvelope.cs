// SPDX-License-Identifier: Apache-2.0

using System;
using System.Globalization;
using System.IO;

namespace AcsEditor;

/// <summary>
/// Reversible managed envelope for the legacy .acscene and .acs3d source subsystems. Projection, camera,
/// selection mode, and editor layout are intentionally absent. This can be replaced by a canonical
/// native world serializer without changing document identity or transaction semantics.
/// </summary>
internal static class SceneWorldDocumentEnvelope
{
    private const string Magic = "ACS_EDITOR_WORLD 1\n";

    internal static string Pack(string subsystem2D, string subsystem3D)
    {
        subsystem2D ??= "";
        subsystem3D ??= "";
        return Magic +
               subsystem2D.Length.ToString(CultureInfo.InvariantCulture) + "\n" +
               subsystem3D.Length.ToString(CultureInfo.InvariantCulture) + "\n" +
               subsystem2D +
               subsystem3D;
    }

    internal static void Unpack(
        string payload,
        out string subsystem2D,
        out string subsystem3D)
    {
        if (payload == null || !payload.StartsWith(Magic, StringComparison.Ordinal))
            throw new InvalidDataException("Invalid canonical scene transaction header.");
        int firstEnd = payload.IndexOf('\n', Magic.Length);
        int secondEnd = firstEnd < 0 ? -1 : payload.IndexOf('\n', firstEnd + 1);
        if (firstEnd < 0 || secondEnd < 0 ||
            !int.TryParse(
                payload.AsSpan(Magic.Length, firstEnd - Magic.Length),
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out int subsystem2DLength) ||
            !int.TryParse(
                payload.AsSpan(firstEnd + 1, secondEnd - firstEnd - 1),
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out int subsystem3DLength) ||
            subsystem2DLength < 0 ||
            subsystem3DLength < 0 ||
            payload.Length - secondEnd - 1 != subsystem2DLength + subsystem3DLength)
        {
            throw new InvalidDataException("Invalid canonical scene transaction lengths.");
        }

        int body = secondEnd + 1;
        subsystem2D = payload.Substring(body, subsystem2DLength);
        subsystem3D = payload.Substring(body + subsystem2DLength, subsystem3DLength);
    }
}
