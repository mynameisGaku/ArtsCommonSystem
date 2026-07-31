// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Scripting モジュール: Lua 5.4 バックエンド (acs::game::IScriptVm、gated)。</summary>
public sealed class Scripting : AcsModule
{
    public Scripting()
    {
        Type = ModuleType.Runtime;
        Guard = "ACS_BUILD_SCRIPTING";
        Preamble = @"include(ACSThirdParty)
acs_third_party_lua()";
        PublicDeps.AddRange(new[] { "Foundation", "Container", "GameFramework", "Memory" });
        PublicLibs.Add("acs_third_party_lua");
        // Lua C APIを含む内部ヘッダーは配布用の公開ヘッダー一覧から除外する。
        ExcludeFiles.AddRange(new[] { "LuaVmImpl.h", "LuaVmValueConversion.h" });
    }
}
