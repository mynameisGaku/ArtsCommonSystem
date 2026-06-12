// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Ui モジュール: リテインモード Widget ツリー + レンダラ。</summary>
public sealed class Ui : AcsModule
{
    public Ui()
    {
        Type = ModuleType.Runtime;
        PublicDeps.AddRange(new[] { "Foundation", "Memory", "Container", "Math", "Platform", "Render", "Mvvm" });
    }
}
