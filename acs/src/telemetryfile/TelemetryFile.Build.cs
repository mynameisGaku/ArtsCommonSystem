// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>TelemetryFile モジュール: JSON Lines テレメトリ sink (IBackendClient、gated)。</summary>
public sealed class TelemetryFile : AcsModule
{
    public TelemetryFile()
    {
        Type = ModuleType.Runtime;
        Guard = "ACS_BUILD_TELEMETRY_FILE";
        PublicDeps.AddRange(new[] { "Foundation", "GameFramework" });
    }
}
