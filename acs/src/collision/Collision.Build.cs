// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Collision モジュール: アセットから衝突形状を生成する純 CPU ジオメトリ処理。</summary>
public sealed class Collision : AcsModule
{
    public Collision()
    {
        Type = ModuleType.Runtime;
        PublicDeps.AddRange(new[] { "Foundation", "Container", "Math", "Asset" });
    }
}
