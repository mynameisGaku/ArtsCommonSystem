// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Test モジュール: 軽量テストフレームワーク (ACS_TEST / EXPECT_*)。</summary>
public sealed class Test : AcsModule
{
    public Test()
    {
        Type = ModuleType.Test;
        PublicDeps.AddRange(new[] { "Foundation", "Threading", "Container" });
    }
}
