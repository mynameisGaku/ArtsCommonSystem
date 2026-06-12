// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Asset モジュール: 画像/メッシュ/音声/テキストのローダ (stb/cgltf/ufbx/dr_libs)。</summary>
public sealed class Asset : AcsModule
{
    public Asset()
    {
        Type = ModuleType.Runtime;
        // サードパーティは FetchContent (cmake/ACSThirdParty.cmake) 経由で取得。
        Preamble = @"acs_third_party_stb()
acs_third_party_cgltf()
acs_third_party_ufbx()
acs_third_party_drlibs()";
        PublicDeps.AddRange(new[] { "Foundation", "Memory", "Container", "Threading", "Math", "Platform" });
        PrivateLibs.AddRange(new[]
        {
            "acs_third_party::stb", "acs_third_party::cgltf",
            "acs_third_party::ufbx", "acs_third_party::drlibs",
        });
    }
}
