// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Test — 軽量テストフレームワーク（GoogleTest 不使用）
// -----------------------------------------------------------------------------
// マクロ ACS_TEST(Suite, Name) でテストケースを宣言すると、グローバルな
// 単方向リストにリンクされ、RunAll() で全ケースを順に実行する。
//
// 使い方:
//     ACS_TEST(MathVec3, Add) {
//         EXPECT_EQ(FVec3(1,2,3) + FVec3(4,5,6), FVec3(5,7,9));
//     }
//
//     int main() { return ::acs::test::RunAll(); }
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/SourceLoc.h"
#include "container/StringView.h"

namespace acs::test {

using TestFn = void (*)();

// 1 テストケースの記述子（リンクトリスト用）
struct TestCase {
    const char* suite;     // スイート名
    const char* name;      // テスト名
    const char* file;      // ソースファイル
    u32         line;      // 行番号
    TestFn      fn;        // テスト本体
    TestCase*   next;      // 次のケース（リンクリスト）
};

// テスト登録（ACS_TEST マクロが起動時に呼ぶ）
void Register(TestCase* tc) noexcept;

// 全テスト実行。失敗があれば 1 を返す（main の戻り値に使う）。
int  RunAll() noexcept;

// テスト本体内から呼ぶ失敗報告 / 補助情報出力
void RecordFailure(SourceLoc loc, const char* expr, const char* fmt, ...) noexcept;
void RecordInfo   (SourceLoc loc, const char* fmt, ...) noexcept;

} // namespace acs::test

// =============================================================================
// ACS_TEST マクロ
// -----------------------------------------------------------------------------
// 静的なグローバルオブジェクトのコンストラクタで Register を呼ぶことで、
// main 開始前にテスト一覧が構築される。
// =============================================================================
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
