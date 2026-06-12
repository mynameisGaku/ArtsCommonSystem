// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Steamworks モジュール: ISteamworksBridge の real 実装 (gated、SDK 必須)。</summary>
public sealed class Steamworks : AcsModule
{
    public Steamworks()
    {
        Type = ModuleType.Runtime;
        Guard = "ACS_BUILD_STEAMWORKS";
        Preamble = @"include(ACSThirdParty)
acs_third_party_steamworks()";
        PublicDeps.AddRange(new[] { "Foundation", "GameFramework" });
        PublicLibs.Add("acs_third_party_steamworks");
    }
}
