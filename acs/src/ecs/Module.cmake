acs_module(
    NAME    Ecs
    TYPE    Runtime
    SOURCES
        World.cpp
        ComponentRegistry.cpp
    HEADERS
        Entity.h
        ComponentId.h
        ComponentRegistry.h
        SparseSet.h
        World.h
        Query.h
        System.h
    PUBLIC_DEPS
        Foundation
        Memory
        Container
        Threading
)
