// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Threading モジュール: Mutex / Thread / ThreadPool / JobGraph + atomics。</summary>
public sealed class Threading : AcsModule
{
    public Threading()
    {
        Type = ModuleType.Runtime;
        PublicDeps.Add("Foundation");
        Feature("THREADPOOL", "THREADING_THREADPOOL", desc: "Build the work-stealing thread pool");
        Feature("LOCKFREE_MPSC", "THREADING_LOCKFREE_MPSC", def: false,
            desc: "Reserved ACS build switch that controls WITH_THREADING_LOCKFREE_MPSC; currently unused by Threading sources");
    }
}
