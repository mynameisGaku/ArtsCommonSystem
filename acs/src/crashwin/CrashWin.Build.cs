// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>CrashWin モジュール: Windows DbgHelp minidump バックエンド (gated)。</summary>
public sealed class CrashWin : AcsModule
{
    public CrashWin()
    {
        Type = ModuleType.Runtime;
        Guard = "ACS_BUILD_CRASH_REPORTER";
        PublicDeps.AddRange(new[] { "Foundation", "GameFramework" });
        PublicLibs.Add("Dbghelp.lib");
    }
}
