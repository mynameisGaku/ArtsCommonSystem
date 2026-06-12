// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Easy モジュール: 初学者向け「簡単モード」ファサード (手続き的 2D API)。</summary>
public sealed class Easy : AcsModule
{
    public Easy()
    {
        Type = ModuleType.Runtime;
        PublicDeps.AddRange(new[]
        {
            "Foundation", "Threading", "Memory", "Container",
            "Math", "Platform", "Asset", "Render", "Audio",
        });
    }
}
