# SPDX-License-Identifier: Apache-2.0
# TelemetryFile module - real JSON Lines telemetry sink for IBackendClient.

if(NOT ACS_BUILD_TELEMETRY_FILE)
    return()
endif()

acs_module(
    NAME    TelemetryFile
    TYPE    Runtime
    SOURCES
        FileTelemetryBackendClient.cpp
        FileTelemetryDefault.cpp
    HEADERS
        FileTelemetryBackendClient.h
    PUBLIC_DEPS
        Foundation
        GameFramework
)

