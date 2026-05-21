// SPDX-License-Identifier: Apache-2.0
// テスト内で条件を検証するアサートマクロ群（失敗してもテストは続行する）
#pragma once

#include "test/Test.h"
#include "foundation/SourceLoc.h"
#include "foundation/Compiler.h"

// expr が true でなければ失敗を記録
#define EXPECT_TRUE(expr)                                                     \
    do {                                                                      \
        if (!(expr)) {                                                        \
            ::acs::test::RecordFailure(::acs::SourceLoc::Current(),           \
                                       ACS_STRINGIFY(expr), "expected true"); \
        }                                                                     \
    } while (0)

// expr が false でなければ失敗を記録
#define EXPECT_FALSE(expr)                                                    \
    do {                                                                      \
        if ((expr)) {                                                         \
            ::acs::test::RecordFailure(::acs::SourceLoc::Current(),           \
                                       ACS_STRINGIFY(expr), "expected false");\
        }                                                                     \
    } while (0)

// a == b でなければ失敗を記録
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

// a != b でなければ失敗を記録
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

// |a - b| が eps 以下でなければ失敗を記録（浮動小数比較）
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
