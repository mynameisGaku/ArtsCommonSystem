// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Foundation — FPanic ハンドラ
// -----------------------------------------------------------------------------
// プロセスを致命的に終了させる「最終エラー経路」。
//
// 動作シーケンス:
//   1. SRWLOCK で排他取得（並行 panic の出力混在を防ぐ）
//   2. ヘッダ (場所/失敗式/メッセージ) を stderr + OutputDebugString に出力
//   3. シンボル化済みスタックトレースを出力
//   4. ユーザー登録の PanicHook を呼び出す（ロガーフラッシュ等）
//   5. デバッガ接続中なら __debugbreak、そうでなければ TerminateProcess
//
// 直接呼ばずに、ACS_ASSERT / ACS_ASSERTF を経由するのが一般的な使い方。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/SourceLoc.h"
#include "foundation/Compiler.h"

namespace acs {

/**
 * パニック直前に呼ばれる任意のフック関数の型。
 *
 * @details
 * ロガーのフラッシュやクラッシュレポート保存などに使う。第 1 引数 user には
 * SetPanicHook 時に渡したポインタがそのまま渡される。
 */
using PanicHook = void (*)(void* user, const char* msg, usize len);

/**
 * パニックフックを登録 / 解除する (スレッドセーフ)。
 *
 * @param hook 登録するフック (nullptr で解除)。
 * @param user フック呼び出し時にそのまま渡されるコンテキストポインタ。
 */
void SetPanicHook(PanicHook hook, void* user) noexcept;

/**
 * パニックを発生させてプロセスを終了させる ([[noreturn]]、戻らない)。
 *
 * @details
 * 場所・失敗式・メッセージ・スタックトレースを stderr とデバッガに出力し、
 * 登録済み PanicHook を呼んだ後、デバッガ接続中なら __debugbreak、そうでなければ
 * TerminateProcess する。通常は ACS_ASSERT 等のマクロ経由で呼ばれる。
 * @param loc 発生位置 (通常は ACS_ASSERT が FSourceLoc::Current() を渡す)。
 * @param expr 失敗した式の文字列 ("x < size" など)。
 * @param fmt printf 形式のメッセージ書式。
 * @param ... fmt に対応する可変長引数。
 */
ACS_NORETURN void FPanic(FSourceLoc loc, const char* expr, const char* fmt, ...) noexcept;

} // namespace acs
