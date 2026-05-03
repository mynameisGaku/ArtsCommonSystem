// =============================================================================
// ACS Foundation — Panic ハンドラ
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

// パニック直前に呼ばれる任意のフック関数の型。
// user は SetPanicHook 時に渡したポインタがそのまま渡される。
// ロガーのフラッシュやクラッシュレポート保存などに使う。
using PanicHook = void (*)(void* user, const char* msg, usize len);

// パニックフックを登録 / 解除する。スレッドセーフ。
void SetPanicHook(PanicHook hook, void* user) noexcept;

// パニックを発生させる。戻ることはない（[[noreturn]]）。
//   loc  — 発生位置（通常は ACS_ASSERT が SourceLoc::Current() を渡す）
//   expr — 失敗した式の文字列（"x < size" など）
//   fmt  — printf 形式メッセージ
ACS_NORETURN void Panic(SourceLoc loc, const char* expr, const char* fmt, ...) noexcept;

} // namespace acs
