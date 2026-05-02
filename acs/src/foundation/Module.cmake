acs_module(
    NAME    Foundation
    TYPE    Runtime
    SOURCES
        Assert.cpp
        Log.cpp
        Panic.cpp
        StackTrace.cpp
    HEADERS
        Types.h
        Compiler.h
        Platform.h
        SourceLoc.h
        TypeTraits.h
        Move.h
        Limits.h
        Result.h
        Error.h
        Assert.h
        Panic.h
        StackTrace.h
        Log.h
    LINK_PUBLIC
        dbghelp
)

acs_module_feature(MODULE Foundation NAME STACKTRACE
    DEFINE FOUNDATION_STACKTRACE
    DESCRIPTION "Capture stack traces via DbgHelp on panic / assert"
    DEFAULT ON)

acs_module_feature(MODULE Foundation NAME LOG_FILE_SINK
    DEFINE FOUNDATION_LOG_FILE
    DESCRIPTION "Enable file output sink in Logger"
    DEFAULT ON)

acs_module_feature(MODULE Foundation NAME LOG_DEBUG_OUTPUT
    DEFINE FOUNDATION_LOG_DBGOUT
    DESCRIPTION "Enable OutputDebugString sink in Logger"
    DEFAULT ON)
