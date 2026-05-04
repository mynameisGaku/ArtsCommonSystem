acs_module(
    NAME    App
    TYPE    Runtime
    SOURCES
        Application.cpp
        Sample.cpp
    HEADERS
        AppConfig.h
        Application.h
        EntryPoint.h
        Sample.h
    PUBLIC_DEPS
        Foundation
        Memory
        Container
        Threading
        Math
        Platform
        Asset
        Ecs
        Event
        Render
)
