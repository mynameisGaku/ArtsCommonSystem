// ACS Test — Tiny test framework (no GoogleTest dependency).
//
// Usage:
//     ACS_TEST(MathVec3, Add) {
//         EXPECT_EQ(Vec3(1,2,3) + Vec3(4,5,6), Vec3(5,7,9));
//     }
//
//     int main() { return ::acs::test::RunAll(); }
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/SourceLoc.h"
#include "container/StringView.h"

namespace acs::test {

using TestFn = void (*)();

struct TestCase {
    const char* suite;
    const char* name;
    const char* file;
    u32         line;
    TestFn      fn;
    TestCase*   next;
};

void Register(TestCase* tc) noexcept;
int  RunAll() noexcept;

// Per-test failure reporting.
void RecordFailure(SourceLoc loc, const char* expr, const char* fmt, ...) noexcept;
void RecordInfo   (SourceLoc loc, const char* fmt, ...) noexcept;

} // namespace acs::test

#define ACS_TEST(suite, name)                                                  \
    static void ACS_CONCAT(_acs_test_fn_, __LINE__)();                         \
    namespace {                                                                \
        struct ACS_CONCAT(_acs_test_reg_, __LINE__) {                          \
            ACS_CONCAT(_acs_test_reg_, __LINE__)() noexcept {                  \
                static ::acs::test::TestCase tc {                              \
                    #suite, #name, __FILE__, __LINE__,                         \
                    &ACS_CONCAT(_acs_test_fn_, __LINE__),                      \
                    nullptr                                                    \
                };                                                             \
                ::acs::test::Register(&tc);                                    \
            }                                                                  \
        };                                                                     \
        static ACS_CONCAT(_acs_test_reg_, __LINE__) ACS_CONCAT(_acs_test_inst_, __LINE__);\
    }                                                                          \
    static void ACS_CONCAT(_acs_test_fn_, __LINE__)()
