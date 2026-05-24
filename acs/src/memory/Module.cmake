acs_module(
    NAME    Memory
    TYPE    Runtime
    SOURCES
        Memory.cpp
        SystemAllocator.cpp
        LinearAllocator.cpp
        PoolAllocator.cpp
        ArenaAllocator.cpp
        VirtualMemory.cpp
        Tlsf.cpp
        MemorySystem.cpp
        MemorySnapshot.cpp
    HEADERS
        Allocator.h
        Memory.h
        New.h
        SystemAllocator.h
        LinearAllocator.h
        PoolAllocator.h
        ArenaAllocator.h
        UniquePtr.h
        Rc.h
        VirtualMemory.h
        Tlsf.h
        Segment.h
        MemorySystem.h
        MemorySnapshot.h
    PUBLIC_DEPS
        Foundation
        Threading
)

acs_module_feature(MODULE Memory NAME LINEAR_ALLOCATOR
    DEFINE MEMORY_LINEAR_ALLOC
    DESCRIPTION "Include LinearAllocator"
    DEFAULT ON)

acs_module_feature(MODULE Memory NAME POOL_ALLOCATOR
    DEFINE MEMORY_POOL_ALLOC
    DESCRIPTION "Include PoolAllocator"
    DEFAULT ON)

acs_module_feature(MODULE Memory NAME ARENA_ALLOCATOR
    DEFINE MEMORY_ARENA_ALLOC
    DESCRIPTION "Include ArenaAllocator"
    DEFAULT ON)

acs_module_feature(MODULE Memory NAME VIRTUAL_MEMORY
    DEFINE MEMORY_VIRTUAL_MEM
    DESCRIPTION "VirtualAlloc reserve+commit layer"
    DEFAULT ON)

acs_module_feature(MODULE Memory NAME TLSF
    DEFINE MEMORY_TLSF
    DESCRIPTION "TLSF allocator"
    DEFAULT ON)

acs_module_feature(MODULE Memory NAME SEGMENT_SYSTEM
    DEFINE MEMORY_SEGMENT_SYS
    DESCRIPTION "Segmented MemorySystem"
    DEFAULT ON)

acs_module_feature(MODULE Memory NAME SNAPSHOT
    DEFINE MEMORY_SNAPSHOT
    DESCRIPTION "Memory usage snapshot output"
    DEFAULT ON)
