// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Timing モジュール: 利用側が値所有する決定論的な時間変換。</summary>
public sealed class Timing : AcsModule
{
    public Timing()
    {
        Type = ModuleType.Runtime;
        PublicDeps.Add("Foundation");
    }
}
