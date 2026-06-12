// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Platform モジュール: Win32 ウィンドウ / 入力 / 時刻 / ファイル / ローカライズ。</summary>
public sealed class Platform : AcsModule
{
    public Platform()
    {
        Type = ModuleType.Runtime;
        PublicDeps.AddRange(new[] { "Foundation", "Memory", "Container", "Math" });
        PrivateLibs.AddRange(new[] { "Shell32", "Ole32" });
        Feature("WINDOW", "PLATFORM_WINDOW", desc: "Build Win32 window subsystem");
        Feature("INPUT", "PLATFORM_INPUT", desc: "Build keyboard / mouse / gamepad input subsystem");
    }
}
