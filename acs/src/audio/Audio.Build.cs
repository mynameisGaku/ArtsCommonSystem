// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Audio モジュール: XAudio2 バックエンドのサウンド再生。</summary>
public sealed class Audio : AcsModule
{
    public Audio()
    {
        Type = ModuleType.Runtime;
        PublicDeps.AddRange(new[] { "Foundation", "Memory", "Container", "Threading", "Asset" });
        PublicLibs.AddRange(new[] { "xaudio2", "ole32" });
        Feature("XAUDIO2", "AUDIO_XAUDIO2", desc: "Use XAudio2 backend");
    }
}
