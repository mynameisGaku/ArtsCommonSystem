# SPDX-License-Identifier: Apache-2.0
# CrashWin module - real Windows DbgHelp minidump backend for ICrashReporterBackend.

if(NOT ACS_BUILD_CRASH_REPORTER)
    return()
endif()

acs_module(
    NAME    CrashWin
    TYPE    Runtime
    SOURCES
        WindowsCrashReporter.cpp
    HEADERS
        WindowsCrashReporter.h
    PUBLIC_DEPS
        Foundation
        GameFramework
    LINK_PUBLIC
        Dbghelp.lib
)

