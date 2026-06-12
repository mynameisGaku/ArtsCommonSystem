// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Ecs モジュール: World / Entity / Component / Query / System。</summary>
public sealed class Ecs : AcsModule
{
    public Ecs()
    {
        Type = ModuleType.Runtime;
        PublicDeps.AddRange(new[] { "Foundation", "Memory", "Container", "Threading" });
    }
}
