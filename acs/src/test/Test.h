// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/SourceLoc.h"
#include "container/StringView.h"

namespace acs::test {

using TestFn = void (*)();

// 1 テストケースの記述子（リンクトリスト用）
struct FTestCase {
    const char* suite;     // スイート名
    const char* name;      // テスト名
    const char* file;      // ソースファイル
    u32         line;      // 行番号
    TestFn      fn;        // テスト本体
    FTestCase*   next;      // 次のケース（リンクリスト）
};

// テスト登録（ACS_TEST マクロが起動時に呼ぶ）
void Register(FTestCase* tc) noexcept;

// 全テスト実行。失敗があれば 1 を返す（main の戻り値に使う）。
int  RunAll() noexcept;

// テスト本体内から呼ぶ失敗報告 / 補助情報出力
void RecordFailure(FSourceLoc loc, const char* expr, const char* fmt, ...) noexcept;
void RecordInfo   (FSourceLoc loc, const char* fmt, ...) noexcept;

} // namespace acs::test

// =============================================================================
// ACS_TEST マクロ
// -----------------------------------------------------------------------------
// 静的なグローバルオブジェクトのコンストラクタで Register を呼ぶことで、
// main 開始前にテスト一覧が構築される。
// =============================================================================
#define ACS_TEST(suite, name)                                                  \
    static void ACS_CONCAT(m_AcsTestFn, __LINE__)();                         \
    namespace {                                                                \
        struct ACS_CONCAT(FAcsTestRegistration, __LINE__) {                   \
            ACS_CONCAT(FAcsTestRegistration, __LINE__)() noexcept {           \
                static ::acs::test::FTestCase tc {                              \
                    #suite, #name, __FILE__, __LINE__,                         \
                    &ACS_CONCAT(m_AcsTestFn, __LINE__),                      \
                    nullptr                                                    \
                };                                                             \
                ::acs::test::Register(&tc);                                    \
            }                                                                  \
        };                                                                     \
        static ACS_CONCAT(FAcsTestRegistration, __LINE__) ACS_CONCAT(m_AcsTestInst, __LINE__);\
    }                                                                          \
    static void ACS_CONCAT(m_AcsTestFn, __LINE__)()
