acs_module(
    NAME    Platform
    TYPE    Runtime
    SOURCES
        Window.cpp
        Input.cpp
        Time.cpp
        FileSystem.cpp
        Storage.cpp
        Localization.cpp
    HEADERS
        Window.h
        Input.h
        InputCodes.h
        Time.h
        FileSystem.h
        Storage.h
        Localization.h
        Event.h
    PUBLIC_DEPS
        Foundation
        Memory
        Container
        Math
    LINK_PRIVATE
        Shell32
        Ole32
)

acs_module_feature(MODULE Platform NAME WINDOW
    DEFINE PLATFORM_WINDOW
    DESCRIPTION "Build Win32 window subsystem"
    DEFAULT ON)

acs_module_feature(MODULE Platform NAME INPUT
    DEFINE PLATFORM_INPUT
    DESCRIPTION "Build keyboard / mouse / gamepad input subsystem"
    DEFAULT ON)
