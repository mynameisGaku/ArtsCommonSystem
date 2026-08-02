// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Container — FJson DOM パーサ (型/数値/エスケープ/エラー/chain 安全性)
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "container/Json.h"
#include "memory/SystemAllocator.h"

#include <limits>

using namespace acs;

// ---- オブジェクト DOM: 各型のアクセサ -------------------------------------
ACS_TEST(Json, ParseObjectDom) {
    auto r = ParseJson(FStringView{
        "{ \"name\": \"hero\", \"hp\": 42, \"alive\": true, \"nick\": null,"
        "  \"pos\": [10, 20, 30], \"meta\": { \"lvl\": 7 } }"});
    EXPECT_TRUE(r.IsOk());
    const FJsonValue& root = r.Value();
    EXPECT_TRUE(root.IsObject());
    EXPECT_EQ(root.MemberCount(), u32(6));
    EXPECT_TRUE(root.Get("name").AsString() == FStringView("hero"));
    EXPECT_EQ(root.Get("hp").AsInt(), i64(42));
    EXPECT_TRUE(root.Get("alive").AsBool());
    EXPECT_TRUE(root.Get("nick").IsNull());
    EXPECT_TRUE(root.Get("pos").IsArray());
    EXPECT_EQ(root.Get("pos").Size(), u32(3));
    EXPECT_EQ(root.Get("pos").At(1).AsInt(), i64(20));
    EXPECT_EQ(root.Get("meta").Get("lvl").AsInt(), i64(7));
    EXPECT_TRUE(root.Has("name"));
    EXPECT_FALSE(root.Has("absent"));
}

// ---- 欠損キー / 範囲外を跨ぐ chain は静的 Null で安全 ----------------------
ACS_TEST(Json, ChainSafeOnMissing) {
    auto r = ParseJson(FStringView{"{ \"a\": { \"b\": 1 } }"});
    EXPECT_TRUE(r.IsOk());
    const FJsonValue& root = r.Value();
    EXPECT_TRUE(root.Get("x").IsNull());
    EXPECT_EQ(root.Get("x").Get("y").AsNumber(7.0), 7.0);  // 欠損 chain → default
    EXPECT_EQ(root.At(99).AsInt(-1), i64(-1));             // 範囲外 → default
}

// ---- 数値: int / 負 / 小数 / 指数 -----------------------------------------
ACS_TEST(Json, Numbers) {
    auto r = ParseJson(FStringView{"[0, -5, 3.5, 1e3, -2.5e-1]"});
    EXPECT_TRUE(r.IsOk());
    const FJsonValue& a = r.Value();
    EXPECT_TRUE(a.IsArray());
    EXPECT_EQ(a.Size(), u32(5));
    EXPECT_EQ(a.At(0).AsInt(), i64(0));
    EXPECT_EQ(a.At(1).AsInt(), i64(-5));
    EXPECT_NEAR(a.At(2).AsNumber(), 3.5, 1e-9);
    EXPECT_NEAR(a.At(3).AsNumber(), 1000.0, 1e-9);
    EXPECT_NEAR(a.At(4).AsNumber(), -0.25, 1e-9);
}

// ---- 文字列エスケープ: \uXXXX / \t / \n のデコード ------------------------
ACS_TEST(Json, StringEscapes) {
    auto r = ParseJson(FStringView{"\"\\u0041\\tB\\n\""});  // JSON: "A\tB\n"
    EXPECT_TRUE(r.IsOk());
    EXPECT_TRUE(r.Value().AsString() == FStringView("A\tB\n"));
}

// ---- 不正入力は crash せずエラーで返す ------------------------------------
ACS_TEST(Json, RejectsMalformed) {
    EXPECT_TRUE(ParseJson(FStringView{"{ \"a\": }"}).IsErr());          // 値欠落
    EXPECT_TRUE(ParseJson(FStringView{"[1, 2"}).IsErr());               // 閉じない
    EXPECT_TRUE(ParseJson(FStringView{"{ \"a\": 1 } junk"}).IsErr());   // 末尾ゴミ
    EXPECT_TRUE(ParseJson(FStringView{""}).IsErr());                    // 空
    EXPECT_TRUE(ParseJson(FStringView{"\"unterminated"}).IsErr());      // 閉じない文字列
    EXPECT_TRUE(ParseJson(FStringView{"[1, 2,]"}).IsErr());             // 末尾カンマ
}

/** 入力上限、深さ上限、allocator 伝播と数値構文を検証する。 */
ACS_TEST(Json, ParserLimitsAllocatorAndNonFiniteContracts)
{
    // サイズ上限判定だけで参照される最小入力。
    const char Dummy = '0';
    // 上限超過入力の失敗結果。
    auto Oversized = ParseJson(&Dummy, kMaxJsonInputBytes + 1u);
    EXPECT_TRUE(Oversized.IsErr());
    EXPECT_EQ(Oversized.Error().subcode, kSubJsonSize);

    EXPECT_TRUE(ParseJson(FStringView("1e+")).IsErr());

    // 最大深さを超える JSON 配列文字列。
    FString Deep;
    for (u32 Index = 0u; Index < 258u; ++Index) {
        Deep.Append('[');
    }
    Deep.Append('0');
    for (u32 Index = 0u; Index < 258u; ++Index) {
        Deep.Append(']');
    }
    // 深さ上限を超えたパース結果。
    auto TooDeep = ParseJson(Deep.View());
    EXPECT_TRUE(TooDeep.IsErr());
    EXPECT_EQ(TooDeep.Error().subcode, kSubJsonDepth);

    // DOM の全確保を観測する明示 allocator。
    CSystemAllocator Allocator;
    {
        // SSO を超える key と value を持つパース結果。
        auto Parsed = ParseJson(FStringView{"{\"long-key-for-explicit-allocator\":\"long-value-for-explicit-allocator\",\"array\":[1,2,3,4,5,6]}"}, Allocator);
        EXPECT_TRUE(Parsed.IsOk());
        EXPECT_TRUE(Allocator.AllocationCount() > 0u);
        EXPECT_TRUE(Parsed.Value().Get("long-key-for-explicit-allocator").AsString() == FStringView("long-value-for-explicit-allocator"));
    }
    EXPECT_EQ(Allocator.AllocationCount(), 0ull);
}

/** writer の escape roundtrip と失敗時の transactional 動作を検証する。 */
ACS_TEST(Json, WriterRoundTripsEscapesAndRollsBackOnFailure)
{
    // 埋め込み NUL と複数種別を含む入力のパース結果。
    auto Parsed = ParseJson(FStringView{"{\"text\":\"A\\u0000B\\n\",\"items\":[true,false,null,-12.5e2]}"});
    EXPECT_TRUE(Parsed.IsOk());
    EXPECT_EQ(Parsed.Value().Get("text").AsString().Size(), static_cast<usize>(4));

    // writer が生成する JSON 文字列。
    FString Written;
    EXPECT_TRUE(TryWriteJson(Parsed.Value(), Written));
    // writer 出力を再パースした結果。
    auto RoundTrip = ParseJson(Written.View());
    EXPECT_TRUE(RoundTrip.IsOk());
    EXPECT_TRUE(RoundTrip.Value().Get("text").AsString() == Parsed.Value().Get("text").AsString());
    EXPECT_NEAR(RoundTrip.Value().Get("items").At(3u).AsNumber(), -1250.0, 1e-9);

    // 失敗時に保持される既存出力。
    FString Output("unchanged");
    EXPECT_FALSE(TryWriteJson(Parsed.Value(), Output, 256u, 4u));
    EXPECT_TRUE(Output == FString("unchanged"));

    // JSON では表現できない非有限数。
    FJsonValue NonFinite;
    NonFinite._SetNumber(std::numeric_limits<f64>::infinity());
    EXPECT_FALSE(TryWriteJson(NonFinite, Output));
    EXPECT_TRUE(Output == FString("unchanged"));

    // writer 深さ上限を超える入れ子値。
    FJsonValue Nested;
    Nested._MakeArray();
    Nested._PushArrayElem()._MakeArray();
    EXPECT_FALSE(TryWriteJson(Nested, Output, 0u, 1024u));
    EXPECT_TRUE(Output == FString("unchanged"));
}
