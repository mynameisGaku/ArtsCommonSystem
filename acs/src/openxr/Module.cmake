# SPDX-License-Identifier: Apache-2.0
# OpenXr module - real Khronos OpenXR loader backend.

if(NOT ACS_BUILD_OPENXR)
    return()
endif()

include(ACSThirdParty)
acs_third_party_openxr()

acs_module(
    NAME    OpenXr
    TYPE    Runtime
    SOURCES
        KhronosOpenXrBridge.cpp
        OpenXrDefault.cpp
    HEADERS
        KhronosOpenXrBridge.h
    PUBLIC_DEPS
        Foundation
        Math
        GameFramework
    LINK_PUBLIC
        acs_third_party_openxr
)
