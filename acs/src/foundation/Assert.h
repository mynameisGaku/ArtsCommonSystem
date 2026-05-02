// ACS Foundation — Assertion macros.
//
//   ACS_ASSERT(expr)            — abort if expr is false (compiled out when
//                                 ACS_ASSERTS_ENABLED == 0).
//   ACS_ASSERTF(expr, fmt, ...) — assert with printf-style message.
//   ACS_VERIFY(expr)            — like ACS_ASSERT but expr is always evaluated.
//   ACS_UNREACHABLE()           — mark a path as unreachable; panics in debug.
//   ACS_NOT_IMPLEMENTED()       — explicit "todo" panic.
//
// Every failed assert routes through Panic() which captures a full stack trace,
// so the user sees: file:line, function, expression, message, and call stack.
#pragma once

#include "foundation/Compiler.h"
#include "foundation/SourceLoc.h"
#include "foundation/Panic.h"

#ifndef ACS_ASSERTS_ENABLED
    #define ACS_ASSERTS_ENABLED 1
#endif

#if ACS_ASSERTS_ENABLED

    #define ACS_ASSERT(expr)                                                    \
        do {                                                                    \
            if (ACS_UNLIKELY(!(expr))) {                                        \
                ::acs::Panic(::acs::SourceLoc::Current(),                       \
                             ACS_STRINGIFY(expr), "assertion failed");          \
            }                                                                   \
        } while (0)

    #define ACS_ASSERTF(expr, fmt, ...)                                         \
        do {                                                                    \
            if (ACS_UNLIKELY(!(expr))) {                                        \
                ::acs::Panic(::acs::SourceLoc::Current(),                       \
                             ACS_STRINGIFY(expr), fmt, ##__VA_ARGS__);          \
            }                                                                   \
        } while (0)

    #define ACS_UNREACHABLE()                                                   \
        ::acs::Panic(::acs::SourceLoc::Current(), "<unreachable>",              \
                     "unreachable code reached")

    #define ACS_NOT_IMPLEMENTED()                                               \
        ::acs::Panic(::acs::SourceLoc::Current(), "<not implemented>",          \
                     "feature not implemented")

#else // ACS_ASSERTS_ENABLED

    #define ACS_ASSERT(expr)                ((void)0)
    #define ACS_ASSERTF(expr, fmt, ...)     ((void)0)
    #if ACS_COMPILER_MSVC
        #define ACS_UNREACHABLE() __assume(0)
    #else
        #define ACS_UNREACHABLE() __builtin_unreachable()
    #endif
    #define ACS_NOT_IMPLEMENTED()           ::acs::Panic(::acs::SourceLoc::Current(), "<not implemented>", "feature not implemented")

#endif

// VERIFY always evaluates the expression; only the panic is gated.
#if ACS_ASSERTS_ENABLED
    #define ACS_VERIFY(expr) ACS_ASSERT(expr)
#else
    #define ACS_VERIFY(expr) do { (void)(expr); } while (0)
#endif
