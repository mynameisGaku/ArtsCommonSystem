// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "event/Delegate.h"
#include "event/MulticastDelegate.h"
#include "event/SimpleMulticastDelegate.h"

using namespace acs;

namespace {

/** 静的関数へ渡された整数の合計。 */
int G_StaticDelegateTotal = 0;

/** 右辺値参照で受け取った確認用の整数。 */
int G_ForwardedDelegateValue = 0;

/** 引数なしの複数デリゲートが呼ばれた回数。 */
int G_SimpleMulticastHits = 0;

/** コピーできない値の転送を確認する型。 */
struct FMoveOnlyDelegateValue {
    /**
     * 確認する整数を持つ値を作る。
     * @param initial_value 保存する整数。
     */
    explicit FMoveOnlyDelegateValue(int initial_value) noexcept : value(initial_value) {}
    /** コピーを禁止する。 */
    FMoveOnlyDelegateValue(const FMoveOnlyDelegateValue&) = delete;
    /** コピー代入を禁止する。 */
    FMoveOnlyDelegateValue& operator=(const FMoveOnlyDelegateValue&) = delete;
    /**
     * 元の値を空にして移動する。
     * @param other 移動元の値。
     */
    FMoveOnlyDelegateValue(FMoveOnlyDelegateValue&& other) noexcept : value(other.value) { other.value = 0; }
    /** 転送結果を確認する整数。 */
    int value = 0;
};

/**
 * 二つの整数を加算する。
 * @param left 左側の整数。
 * @param right 右側の整数。
 */
int AddStatic(int left, int right) {
    return left + right;
}

/**
 * 入力値を静的な確認用合計へ加える。
 * @param value 合計へ加える整数。
 */
void AccumulateStatic(int value) noexcept {
    G_StaticDelegateTotal += value;
}

/** 引数なしの複数デリゲートから呼ばれた回数を一つ増やす。 */
void IncrementSimpleMulticast() noexcept {
    ++G_SimpleMulticastHits;
}

/**
 * コピーできない値を右辺値参照で受け取る。
 * @param value 転送を確認する値。
 */
void ConsumeMoveOnly(FMoveOnlyDelegateValue&& value) noexcept {
    G_ForwardedDelegateValue = value.value;
    value.value = 0;
}

/** 型付きデリゲートから呼び出す処理と結果を持つ。 */
struct FTypedDelegateReceiver {
    /**
     * 入力値へ設定倍率を掛ける。
     * @param value 倍率を掛ける整数。
     */
    int Multiply(int value) noexcept { return value * factor; }
    /**
     * 入力値へ設定倍率を掛け、対象を変更せず結果を返す。
     * @param value 倍率を掛ける整数。
     */
    int MultiplyConst(int value) const noexcept { return value * factor; }
    /**
     * 入力値を合計へ加える。
     * @param value 合計へ加える整数。
     */
    void Accumulate(int value) noexcept { total += value; }
    /** 乗算に使う倍率。 */
    int factor = 3;
    /** 受け取った整数の合計。 */
    int total = 0;
};

} // namespace

/** 戻り値とメンバー関数を単一デリゲートで扱えることを確認する。 */
ACS_TEST(Delegate, SupportsReturnValuesAndMemberFunctions) {
    /** 二つの整数を加算するデリゲート。 */
    TDelegate<int(int, int)> add = TDelegate<int(int, int)>::CreateStatic<&AddStatic>();
    /** デリゲートの実行結果。 */
    int result = 0;
    EXPECT_TRUE(add.TryExecute(result, 2, 5));
    EXPECT_EQ(result, 7);

    /** メンバー関数を提供する呼出し対象。 */
    FTypedDelegateReceiver receiver;
    /** 対象の倍率を使うデリゲート。 */
    TDelegate<int(int)> multiply = TDelegate<int(int)>::CreateRaw<&FTypedDelegateReceiver::Multiply>(&receiver);
    EXPECT_TRUE(multiply.TryExecute(result, 4));
    EXPECT_EQ(result, 12);

    /** 読み取り専用メンバー関数を提供する呼出し対象。 */
    const FTypedDelegateReceiver const_receiver;
    /** 読み取り専用対象の倍率を使うデリゲート。 */
    TDelegate<int(int)> const_multiply = TDelegate<int(int)>::CreateRaw<&FTypedDelegateReceiver::MultiplyConst>(&const_receiver);
    EXPECT_TRUE(const_multiply.TryExecute(result, 5));
    EXPECT_EQ(result, 15);
}

/** 未設定や空の対象を実行せず、呼出し側の結果を保つことを確認する。 */
ACS_TEST(Delegate, UnboundExecutionPreservesCallerState) {
    /** 未設定の整数デリゲート。 */
    TDelegate<int(int)> unbound;
    /** 未設定実行で変更されない確認用結果。 */
    int result = 41;
    EXPECT_FALSE(unbound.TryExecute(result, 9));
    EXPECT_EQ(result, 41);

    /** 空の対象から作った未設定デリゲート。 */
    const TDelegate<int(int)> null_member = TDelegate<int(int)>::CreateRaw<&FTypedDelegateReceiver::Multiply, FTypedDelegateReceiver>(nullptr);
    EXPECT_FALSE(null_member.IsBound());
    EXPECT_FALSE(null_member.TryExecute(result, 3));
    EXPECT_EQ(result, 41);
}

/** 引数の参照種別を保ったまま呼出し先へ転送することを確認する。 */
ACS_TEST(Delegate, PreservesArgumentValueCategory) {
    G_ForwardedDelegateValue = 0;
    /** 右辺値参照を受け取る型付きデリゲート。 */
    TDelegate<void(FMoveOnlyDelegateValue&&)> consume = TDelegate<void(FMoveOnlyDelegateValue&&)>::CreateStatic<&ConsumeMoveOnly>();
    /** 呼出し先へ転送するコピー不可の値。 */
    FMoveOnlyDelegateValue value{17};
    EXPECT_TRUE(consume.ExecuteIfBound(Move(value)));
    EXPECT_EQ(G_ForwardedDelegateValue, 17);
    EXPECT_EQ(value.value, 0);
}

/** 複数デリゲートが既存イベントの登録と解除を使うことを確認する。 */
ACS_TEST(Delegate, MulticastUsesTypedEventStorage) {
    /** 呼出し結果を記録する対象。 */
    FTypedDelegateReceiver receiver;
    /** 整数を通知する複数デリゲート。 */
    TMulticastDelegate<void(int)> changed;
    /** 登録した処理を識別する値。 */
    const FTypedEventHandle handle = changed.AddRaw<&FTypedDelegateReceiver::Accumulate>(&receiver);
    EXPECT_TRUE(handle.IsValid());
    changed.Broadcast(6);
    EXPECT_EQ(receiver.total, 6);
    EXPECT_TRUE(changed.Remove(handle));
    changed.Broadcast(10);
    EXPECT_EQ(receiver.total, 6);
}

/** 静的関数と一回購読を簡潔に登録できることを確認する。 */
ACS_TEST(Delegate, MulticastSupportsConvenienceBindings) {
    G_StaticDelegateTotal = 0;
    /** 整数を静的関数へ通知する複数デリゲート。 */
    TMulticastDelegate<void(int)> changed;
    /** 一度だけ呼ぶ静的関数の識別値。 */
    const FTypedEventHandle once_handle = changed.AddOnceStatic<&AccumulateStatic>();
    EXPECT_TRUE(once_handle.IsValid());
    EXPECT_TRUE(changed.IsBound());
    changed.Broadcast(4);
    changed.Broadcast(8);
    EXPECT_EQ(G_StaticDelegateTotal, 4);
    EXPECT_FALSE(changed.IsBound());
}

/** 引数なしの複数デリゲートを型指定なしで使えることを確認する。 */
ACS_TEST(Delegate, SimpleMulticastSupportsOneShotBinding) {
    G_SimpleMulticastHits = 0;
    /** 引数なしの処理を複数登録できるデリゲート。 */
    FSimpleMulticastDelegate changed;
    /** 一度だけ呼ぶ静的関数の識別値。 */
    const FTypedEventHandle handle = changed.AddOnceStatic<&IncrementSimpleMulticast>();
    EXPECT_TRUE(handle.IsValid());
    changed.Broadcast();
    changed.Broadcast();
    EXPECT_EQ(G_SimpleMulticastHits, 1);
    EXPECT_FALSE(changed.IsBound());
}
