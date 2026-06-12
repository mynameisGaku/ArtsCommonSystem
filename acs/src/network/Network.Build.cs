// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Network モジュール: Winsock2 ベースの TCP / UDP ソケット。</summary>
public sealed class Network : AcsModule
{
    public Network()
    {
        Type = ModuleType.Runtime;
        PublicDeps.AddRange(new[] { "Foundation", "Memory", "Container" });
        PublicLibs.Add("ws2_32");
    }
}
