// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Container モジュール: Array / String / HashMap / Json などのコンテナ。</summary>
public sealed class Container : AcsModule
{
    public Container()
    {
        Type = ModuleType.Runtime;
        PublicDeps.AddRange(new[] { "Foundation", "Memory" });
        Feature("HASHMAP", "CONTAINER_HASHMAP", desc: "Include Robin-Hood THashMap");
        Feature("STRING_SSO", "CONTAINER_STRING_SSO",
            desc: "Use small-string optimization for FString (22 inline bytes)");
    }
}
