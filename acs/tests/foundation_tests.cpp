// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "foundation/Result.h"
#include "foundation/Error.h"
#include "foundation/SourceLoc.h"
#include "foundation/Log.h"

using namespace acs;

ACS_TEST(Foundation, ResultOk) {
    TResult<int> r = Ok(42);
    EXPECT_TRUE(r.IsOk());
    EXPECT_FALSE(r.IsErr());
    EXPECT_EQ(r.Value(), 42);
}

ACS_TEST(Foundation, ResultErr) {
    TResult<int> r = Err<int>(ACS_ERR(Generic, 7, "boom"));
    EXPECT_FALSE(r.IsOk());
    EXPECT_TRUE(r.IsErr());
    EXPECT_EQ(r.Error().subcode, (u16)7);
}

ACS_TEST(Foundation, ResultVoid) {
    TResult<void> r = Ok();
    EXPECT_TRUE(r.IsOk());
    TResult<void> e = Err(ACS_ERR(IO, 3, "io"));
    EXPECT_TRUE(e.IsErr());
}

ACS_TEST(Foundation, SourceLocCaptures) {
    SourceLoc s = SourceLoc::Current();
    EXPECT_TRUE(s.Line() > 0);
    EXPECT_TRUE(s.File() != nullptr);
}

ACS_TEST(Foundation, LoggerEmits) {
    ACS_LOG_INFO("foundation log smoke test value=%d", 7);
    Logger::Flush();
    EXPECT_TRUE(true);
}
