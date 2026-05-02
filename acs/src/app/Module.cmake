acs_module(
    NAME    App
    TYPE    Runtime
    SOURCES
        Application.cpp
    HEADERS
        AppConfig.h
        Application.h
        EntryPoint.h
    PUBLIC_DEPS
        Foundation
        Memory
        Container
        Threading
        Math
        Platform
        Asset
        Ecs
        Render
)
