# SPDX-License-Identifier: Apache-2.0
# LocalMatch module - deterministic local IMatchmaker backend.

if(NOT ACS_BUILD_LOCAL_MATCHMAKER)
    return()
endif()

acs_module(
    NAME    LocalMatch
    TYPE    Runtime
    SOURCES
        LocalMatchmaker.cpp
    HEADERS
        LocalMatchmaker.h
    PUBLIC_DEPS
        Foundation
        GameFramework
)
