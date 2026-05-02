acs_module(
    NAME    Platform
    TYPE    Runtime
    SOURCES
        Window.cpp
        Input.cpp
        Time.cpp
        FileSystem.cpp
    HEADERS
        Window.h
        Input.h
        InputCodes.h
        Time.h
        FileSystem.h
        Event.h
    PUBLIC_DEPS
        Foundation
        Memory
        Container
        Math
)

acs_module_feature(MODULE Platform NAME WINDOW
    DEFINE PLATFORM_WINDOW
    DESCRIPTION "Build Win32 window subsystem"
    DEFAULT ON)

acs_module_feature(MODULE Platform NAME INPUT
    DEFINE PLATFORM_INPUT
    DESCRIPTION "Build keyboard / mouse / gamepad input subsystem"
    DEFAULT ON)
