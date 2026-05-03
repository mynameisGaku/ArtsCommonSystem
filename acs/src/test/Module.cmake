acs_module(
    NAME    Test
    TYPE    Test
    SOURCES
        Test.cpp
    HEADERS
        Test.h
        Expect.h
    PUBLIC_DEPS
        Foundation
        Threading
        Container
)
