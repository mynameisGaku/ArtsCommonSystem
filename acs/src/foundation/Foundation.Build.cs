// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Foundation モジュール: 型 / Result / Assert / Log / Panic などの最下層土台。</summary>
public sealed class Foundation : AcsModule
{
    public Foundation()
    {
        Type = ModuleType.Runtime;
        // 依存なし (最下層)。DbgHelp は StackTrace / Panic が使う。
        PublicLibs.Add("dbghelp");
        Feature("STACKTRACE", "FOUNDATION_STACKTRACE",
            desc: "Capture stack traces via DbgHelp on panic / assert");
        Feature("LOG_FILE_SINK", "FOUNDATION_LOG_FILE", desc: "Enable file output sink in Logger");
        Feature("LOG_DEBUG_OUTPUT", "FOUNDATION_LOG_DBGOUT", desc: "Enable OutputDebugString sink in Logger");
    }
}
