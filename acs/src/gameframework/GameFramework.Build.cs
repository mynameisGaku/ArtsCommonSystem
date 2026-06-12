// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>
/// GameFramework モジュール: Scene/Node/Component、各種 Director/System、in-game エディタ
/// (tools/**)、リフレクション (Reflect/ReflectCatalog) 等。ソースは tools/ や audio_backend/
/// を含め再帰走査で自動収集する。
/// </summary>
public sealed class GameFramework : AcsModule
{
    public GameFramework()
    {
        Type = ModuleType.Runtime;
        PublicDeps.AddRange(new[]
        {
            "Foundation", "Memory", "Container", "Math", "Platform", "Render", "App",
            "Imgui",   // editor panel が .cpp で ImGui を使うため
        });
        // XAudio2Backend.cpp (xaudio2/ole32) と NetSnapshot.cpp の Winsock2 (ws2_32)。
        PublicLibs.AddRange(new[] { "xaudio2", "ole32", "ws2_32" });
    }
}
