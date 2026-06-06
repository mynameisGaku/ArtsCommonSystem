// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Container — FJson DOM パーサ (型/数値/エスケープ/エラー/chain 安全性)
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "container/Json.h"

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
