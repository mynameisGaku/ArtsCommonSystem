acs_module(
    NAME    Event
    TYPE    Runtime
    SOURCES
        Timer.cpp
        MessageBroker.cpp
    HEADERS
        Timer.h
        MessageBroker.h
        MessagePipe.h
    PUBLIC_DEPS
        Foundation
        Container
        Threading
)
