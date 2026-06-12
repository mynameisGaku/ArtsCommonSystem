// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>LocalMatch モジュール: 決定論的ローカル IMatchmaker バックエンド (gated)。</summary>
public sealed class LocalMatch : AcsModule
{
    public LocalMatch()
    {
        Type = ModuleType.Runtime;
        Guard = "ACS_BUILD_LOCAL_MATCHMAKER";
        PublicDeps.AddRange(new[] { "Foundation", "GameFramework" });
    }
}
