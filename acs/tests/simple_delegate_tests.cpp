// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "event/SimpleDelegate.h"

using namespace acs;

namespace {

/**
 * 指定カウンターを一つ増やす。
 * @param user 更新する整数へのポインター。
 */
void IncrementStatic(void* user) {
    ++*static_cast<int*>(user);
}

/** 引数なしデリゲートから呼び出す処理を持つ。 */
struct FDelegateReceiver {
    /** 呼出し回数を一つ増やす。 */
    void Increment() noexcept { ++hits; }
    /** 処理が呼ばれた回数。 */
    int hits = 0;
};

} // namespace

/** 静的関数とメンバー関数を引数なしデリゲートへ設定できることを確認する。 */
ACS_TEST(SimpleDelegate, SupportsStaticAndMemberFunctions) {
    /** 静的関数が呼ばれた回数。 */
    int static_hits = 0;
    /** 静的関数を呼び出すデリゲート。 */
    FSimpleDelegate static_delegate = FSimpleDelegate::CreateStatic(&IncrementStatic, &static_hits);
    EXPECT_TRUE(static_delegate.ExecuteIfBound());
    EXPECT_EQ(static_hits, 1);

    /** メンバー関数を提供する呼出し対象。 */
    FDelegateReceiver receiver;
    /** 対象のメンバー関数を呼び出すデリゲート。 */
    FSimpleDelegate member_delegate = FSimpleDelegate::CreateRaw<&FDelegateReceiver::Increment>(&receiver);
    EXPECT_TRUE(member_delegate.ExecuteIfBound());
    EXPECT_EQ(receiver.hits, 1);

    member_delegate.Unbind();
    EXPECT_FALSE(member_delegate.ExecuteIfBound());
}
