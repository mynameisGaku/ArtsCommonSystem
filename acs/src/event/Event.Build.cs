// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Event モジュール: Timer / MessageBroker / MessagePipe (イベント配送)。</summary>
public sealed class Event : AcsModule
{
    public Event()
    {
        Type = ModuleType.Runtime;
        // ソース/ヘッダは acsbuild が src/event を走査して自動収集する。
        PublicDeps.AddRange(new[] { "Foundation", "Container", "Threading" });
    }
}
