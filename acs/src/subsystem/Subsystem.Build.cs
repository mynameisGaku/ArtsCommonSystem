// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Subsystem モジュール: owner 寿命に従う共有サービスの登録・所有・更新。</summary>
public sealed class Subsystem : AcsModule
{
    public Subsystem()
    {
        Type = ModuleType.Runtime;
        PublicDeps.AddRange(new[] { "Foundation", "Memory", "Container" });
    }
}
