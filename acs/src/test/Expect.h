// ACS Test — Assertion macros.
#pragma once

#include "test/Test.h"
#include "foundation/SourceLoc.h"
#include "foundation/Compiler.h"

#define EXPECT_TRUE(expr)                                                     \
    do {                                                                      \
        if (!(expr)) {                                                        \
            ::acs::test::RecordFailure(::acs::SourceLoc::Current(),           \
                                       ACS_STRINGIFY(expr), "expected true"); \
        }                                                                     \
    } while (0)

#define EXPECT_FALSE(expr)                                                    \
    do {                                                                      \
        if ((expr)) {                                                         \
            ::acs::test::RecordFailure(::acs::SourceLoc::Current(),           \
                                       ACS_STRINGIFY(expr), "expected false");\
        }                                                                     \
    } while (0)

#define EXPECT_EQ(a, b)                                                       \
    do {                                                                      \
        auto _av = (a);                                                       \
        auto _bv = (b);                                                       \
        if (!(_av == _bv)) {                                                  \
            ::acs::test::RecordFailure(::acs::SourceLoc::Current(),           \
                ACS_STRINGIFY(a) " == " ACS_STRINGIFY(b),                     \
                "values differ");                                             \
        }                                                                     \
    } while (0)

#define EXPECT_NE(a, b)                                                       \
    do {                                                                      \
        auto _av = (a);                                                       \
        auto _bv = (b);                                                       \
        if (_av == _bv) {                                                     \
            ::acs::test::RecordFailure(::acs::SourceLoc::Current(),           \
                ACS_STRINGIFY(a) " != " ACS_STRINGIFY(b),                     \
                "values equal");                                              \
        }                                                                     \
    } while (0)

#define EXPECT_NEAR(a, b, eps)                                                \
    do {                                                                      \
        f32 _diff = (f32)((a) - (b));                                         \
        if (_diff < 0) _diff = -_diff;                                        \
        if (_diff > (eps)) {                                                  \
            ::acs::test::RecordFailure(::acs::SourceLoc::Current(),           \
                ACS_STRINGIFY(a) " ~= " ACS_STRINGIFY(b),                     \
                "diff %.6f > %.6f", _diff, (f32)(eps));                       \
        }                                                                     \
    } while (0)
