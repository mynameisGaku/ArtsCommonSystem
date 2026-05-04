acs_module(
    NAME    Threading
    TYPE    Runtime
    SOURCES
        Mutex.cpp
        RwLock.cpp
        ConditionVar.cpp
        Thread.cpp
        ThreadPool.cpp
        JobGraph.cpp
    HEADERS
        Atomic.h
        MemoryOrder.h
        Mutex.h
        RwLock.h
        ConditionVar.h
        ScopedLock.h
        Thread.h
        ThreadId.h
        ThreadPool.h
        JobGraph.h
    PUBLIC_DEPS
        Foundation
)

acs_module_feature(MODULE Threading NAME THREADPOOL
    DEFINE THREADING_THREADPOOL
    DESCRIPTION "Build the work-stealing thread pool"
    DEFAULT ON)

acs_module_feature(MODULE Threading NAME LOCKFREE_MPSC
    DEFINE THREADING_LOCKFREE_MPSC
    DESCRIPTION "Build the lock-free MPSC queue (Phase 2 — currently placeholder)"
    DEFAULT OFF)
