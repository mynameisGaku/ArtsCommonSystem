acs_module(
    NAME    Memory
    TYPE    Runtime
    SOURCES
        Memory.cpp
        SystemAllocator.cpp
        LinearAllocator.cpp
        PoolAllocator.cpp
        ArenaAllocator.cpp
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
    DESCRIPTION "Include PoolAllocator (lock-free Treiber stack)"
    DEFAULT ON)

acs_module_feature(MODULE Memory NAME ARENA_ALLOCATOR
    DEFINE MEMORY_ARENA_ALLOC
    DESCRIPTION "Include ArenaAllocator (page-backed bump)"
    DEFAULT ON)
