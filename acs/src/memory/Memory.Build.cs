// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Memory モジュール: 各種アロケータ + セグメント化 MemorySystem + スナップショット。</summary>
public sealed class Memory : AcsModule
{
    public Memory()
    {
        Type = ModuleType.Runtime;
        Preamble = @"include(ACSThirdParty)
acs_third_party_mimalloc()";
        PublicDeps.AddRange(new[] { "Foundation", "Threading" });
        PrivateLibs.Add("acs_third_party::mimalloc");
        Feature("LINEAR_ALLOCATOR", "MEMORY_LINEAR_ALLOC", desc: "Include LinearAllocator");
        Feature("POOL_ALLOCATOR", "MEMORY_POOL_ALLOC", desc: "Include PoolAllocator");
        Feature("ARENA_ALLOCATOR", "MEMORY_ARENA_ALLOC", desc: "Include ArenaAllocator");
        Feature("VIRTUAL_MEMORY", "MEMORY_VIRTUAL_MEM", desc: "VirtualAlloc reserve+commit layer");
        Feature("TLSF", "MEMORY_TLSF", desc: "TLSF allocator");
        Feature("MIMALLOC", "MEMORY_MIMALLOC", desc: "mimalloc v3 virtual-memory allocator");
        Feature("SEGMENT_SYSTEM", "MEMORY_SEGMENT_SYS", desc: "Segmented MemorySystem");
        Feature("SNAPSHOT", "MEMORY_SNAPSHOT", desc: "Memory usage snapshot output");
    }
}
