// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>OpenXr モジュール: Khronos OpenXR ローダバックエンド (gated)。</summary>
public sealed class OpenXr : AcsModule
{
    public OpenXr()
    {
        Type = ModuleType.Runtime;
        Guard = "ACS_BUILD_OPENXR";
        Preamble = @"include(ACSThirdParty)
acs_third_party_openxr()";
        PublicDeps.AddRange(new[] { "Foundation", "Math", "GameFramework" });
        PublicLibs.Add("acs_third_party_openxr");
    }
}
