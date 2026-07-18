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
            "Foundation", "Memory", "Container", "Threading", "Math", "Platform", "Render", "App",
            // RollbackSession (World スナップショット履歴 + 入力台帳の rollback netcode 統合層) が
            // acs::World / FRollbackBuffer を使う。
            "Ecs",
        });

        // tools/** は ImGui ベースのエディタ群なので、raw DX12 と一緒にだけ組み込む。
        // Diligent 構成ではランタイム基盤を残し、DX12 専用 Imgui への依存を持ち込まない。
        When("ACS_RENDER_DX12_RAW")
            .SubdirSrc("tools")
            .Dep("Imgui");
        // XAudio2Backend.cpp (xaudio2/ole32) と NetSnapshot.cpp の Winsock2 (ws2_32)。
        PublicLibs.AddRange(new[] { "xaudio2", "ole32", "ws2_32" });
    }
}
