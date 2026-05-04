acs_module(
    NAME    Ui
    TYPE    Runtime
    SOURCES
        UiRenderer.cpp
    HEADERS
        Widget.h
        Widgets.h
        UiRenderer.h
    PUBLIC_DEPS
        Foundation
        Memory
        Container
        Math
        Platform
        Render
        Mvvm
)
