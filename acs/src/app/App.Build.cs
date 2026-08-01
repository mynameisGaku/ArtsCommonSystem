// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>App モジュール: acs::Application エントリ + サンプル雛形。</summary>
public sealed class App : AcsModule
{
    public App()
    {
        Type = ModuleType.Runtime;
        PublicDeps.AddRange(new[]
        {
            "Foundation", "Memory", "Container", "Subsystem", "Threading", "Math",
            "Platform", "Asset", "Ecs", "Event", "Render",
        });
    }
}
