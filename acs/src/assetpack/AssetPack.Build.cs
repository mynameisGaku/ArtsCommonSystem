// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>AssetPack モジュール: .acpak アーカイブ (LZ4 + AES-256-GCM) の読み書き。</summary>
public sealed class AssetPack : AcsModule
{
    public AssetPack()
    {
        Type = ModuleType.Runtime;
        PublicDeps.AddRange(new[] { "Foundation", "Container", "Memory", "Platform", "Threading", "GameFramework" });
        // Bcrypt は AES-256-GCM / PBKDF2 / CSPRNG に使う (Windows SDK 同梱)。
        PrivateLibs.Add("Bcrypt");
    }
}
