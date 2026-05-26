// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "container/Array.h"
#include "container/String.h"
#include "container/StringView.h"
#include "container/HashMap.h"
#include "container/Hash.h"

using namespace acs;

ACS_TEST(Container, ArrayPushAndIndex) {
    TArray<int> a;
    for (int i = 0; i < 100; ++i) a.PushBack(i);
    EXPECT_EQ(a.Size(), (usize)100);
    EXPECT_EQ(a[0], 0);
    EXPECT_EQ(a[99], 99);
    EXPECT_EQ(a.Back(), 99);
}

ACS_TEST(Container, ArrayResize) {
    TArray<int> a;
    a.Resize(50);
    EXPECT_EQ(a.Size(), (usize)50);
    a.Resize(10);
    EXPECT_EQ(a.Size(), (usize)10);
}

ACS_TEST(Container, StringInlineAndHeap) {
    FString s("hello");
    EXPECT_EQ(s.Size(), (usize)5);
    EXPECT_TRUE(s.View() == FStringView("hello"));

    FString big;
    for (int i = 0; i < 10; ++i) big.Append("0123456789");
    EXPECT_EQ(big.Size(), (usize)100);
}

ACS_TEST(Container, StringFormat) {
    FString s;
    s.AppendFormat("%d-%s", 42, "abc");
    EXPECT_TRUE(s.View() == FStringView("42-abc"));
}

ACS_TEST(Container, HashMapInsertFindRemove) {
    THashMap<u32, u32> m;
    for (u32 i = 0; i < 1000; ++i) m.Insert(i, i * 2);
    EXPECT_EQ(m.Size(), (usize)1000);
    for (u32 i = 0; i < 1000; ++i) {
        u32* v = m.Find(i);
        EXPECT_TRUE(v != nullptr);
        if (v) EXPECT_EQ(*v, i * 2);
    }
    EXPECT_TRUE(m.Remove(500));
    EXPECT_TRUE(m.Find(500) == nullptr);
    EXPECT_EQ(m.Size(), (usize)999);
}

ACS_TEST(Container, HashBytesDeterministic) {
    const char* a = "the quick brown fox";
    u64 h1 = HashBytes(a, 19);
    u64 h2 = HashBytes(a, 19);
    EXPECT_EQ(h1, h2);
}
