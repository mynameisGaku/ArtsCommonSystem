// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Foundation — アサーションマクロ
// -----------------------------------------------------------------------------
// 提供マクロ (3 段階):
//   [debug-only]   ACS_ASSERT(expr)            — 安いチェック。リリースで除去
//   [debug-only]   ACS_ASSERTF(expr, fmt, ...) — printf 風メッセージ付き
//   [debug-only]   ACS_VERIFY(expr)            — リリースでも expr は評価する
//   [always-fire]  ACS_CHECK(expr)             — 安全性/正しさのため常に検査
//   [always-fire]  ACS_CHECKF(expr, fmt, ...)  — メッセージ付き常時検査
//   [always-fire]  ACS_NOTREACHED()            — 到達したら必ず panic
//   [always-fire]  ACS_NOT_IMPLEMENTED()       — 未実装パニック
//   [debug-fire / release-UB] ACS_UNREACHABLE() — 最適化ヒント (絶対到達しない)
//
// 使い分け:
//   ACS_ASSERT  : "デバッグ中に気づきたい" 範囲チェックや事前条件
//   ACS_CHECK   : "リリースでも倒れたほうがマシ" な不変条件 (security 含む)
//   ACS_NOTREACHED : "ここに来たら必ずバグ。落とす"
//   ACS_UNREACHABLE: "ここに来ない事を最適化器に教える" (本当に到達しないとき)
//
// 失敗したアサートはすべて FPanic() を経由する。FPanic はファイル/行/関数名/
// 失敗式/メッセージ/シンボル化済みスタックトレースを stderr + デバッガに
// 出力した後、プロセスを TerminateProcess で終了させる。
//
// マクロを切る方法: CMake configure 時に -DACS_ENABLE_ASSERTS=OFF を指定する
// （ASSERT/ASSERTF/VERIFY のみ無効化。CHECK/CHECKF/NOTREACHED/NOT_IMPLEMENTED は
// 常に有効）。
// =============================================================================
#pragma once

#include "foundation/Compiler.h"
#include "foundation/SourceLoc.h"
#include "foundation/Panic.h"

// アサート有効化フラグ。CMake が定義しなければデフォルト ON。
#ifndef ACS_ASSERTS_ENABLED
    #define ACS_ASSERTS_ENABLED 1
#endif

#if ACS_ASSERTS_ENABLED

    // ACS_ASSERT(expr): expr が偽なら FPanic を呼ぶ。expr 文字列もパニックメッセージに含まれる。
    #define ACS_ASSERT(expr)                                                    \
        do {                                                                    \
            if (ACS_UNLIKELY(!(expr))) {                                        \
                ::acs::FPanic(::acs::FSourceLoc::Current(),                       \
                             ACS_STRINGIFY(expr), "assertion failed");          \
            }                                                                   \
        } while (0)

    // ACS_ASSERTF(expr, fmt, ...): ACS_ASSERT に printf 風のメッセージを追加できる版。
    // 例: ACS_ASSERTF(idx < size, "index %u out of range (size=%u)", idx, size);
    #define ACS_ASSERTF(expr, fmt, ...)                                         \
        do {                                                                    \
            if (ACS_UNLIKELY(!(expr))) {                                        \
                ::acs::FPanic(::acs::FSourceLoc::Current(),                       \
                             ACS_STRINGIFY(expr), fmt, ##__VA_ARGS__);          \
            }                                                                   \
        } while (0)

    // ACS_UNREACHABLE(): 「ここには絶対に到達しない」と宣言する箇所に置く。デバッグでパニック。
    #define ACS_UNREACHABLE()                                                   \
        ::acs::FPanic(::acs::FSourceLoc::Current(), "<unreachable>",              \
                     "unreachable code reached")

    // ACS_NOT_IMPLEMENTED(): まだ実装していない関数や分岐に置く。
    #define ACS_NOT_IMPLEMENTED()                                               \
        ::acs::FPanic(::acs::FSourceLoc::Current(), "<not implemented>",          \
                     "feature not implemented")

#else // ACS_ASSERTS_ENABLED == 0 — リリース等でアサート除去

    #define ACS_ASSERT(expr)                ((void)0)
    #define ACS_ASSERTF(expr, fmt, ...)     ((void)0)
    // ACS_UNREACHABLE はオプティマイザに「ここは到達しない」と教える組み込みに置換。
    #if ACS_COMPILER_MSVC
        #define ACS_UNREACHABLE() __assume(0)
    #else
        #define ACS_UNREACHABLE() __builtin_unreachable()
    #endif
    // 未実装はリリースでもパニックする（致命的なバグなので隠さない）
    #define ACS_NOT_IMPLEMENTED()           ::acs::FPanic(::acs::FSourceLoc::Current(), "<not implemented>", "feature not implemented")

#endif

// ACS_VERIFY(expr): ACS_ASSERT と異なり、リリースでも expr は必ず評価される。
// 副作用のある式（関数呼び出しなど）に対するチェックで使用。
#if ACS_ASSERTS_ENABLED
    #define ACS_VERIFY(expr) ACS_ASSERT(expr)
#else
    #define ACS_VERIFY(expr) do { (void)(expr); } while (0)
#endif

// 常時有効マクロ — リリースでも検査する不変条件用。
// ACS_ASSERTS_ENABLED に関係なく必ず検査する。
// 安全性・正しさを保証する不変条件 (out-of-bounds, null deref 前段、
// プロトコル違反、整合性破壊、セキュリティ前提など) に使う。

// ACS_CHECK(expr): expr が偽なら必ず FPanic を呼ぶ (リリースでも除去されない)。
// 例: ACS_CHECK(buffer != nullptr);
//     ACS_CHECK(size <= capacity);
#define ACS_CHECK(expr)                                                         \
    do {                                                                        \
        if (ACS_UNLIKELY(!(expr))) {                                            \
            ::acs::FPanic(::acs::FSourceLoc::Current(),                           \
                         ACS_STRINGIFY(expr), "check failed");                  \
        }                                                                       \
    } while (0)

// ACS_CHECKF(expr, fmt, ...): ACS_CHECK に printf 風のメッセージを追加できる版。
// 例: ACS_CHECKF(idx < size, "index %u out of range (size=%u)", idx, size);
#define ACS_CHECKF(expr, fmt, ...)                                              \
    do {                                                                        \
        if (ACS_UNLIKELY(!(expr))) {                                            \
            ::acs::FPanic(::acs::FSourceLoc::Current(),                           \
                         ACS_STRINGIFY(expr), fmt, ##__VA_ARGS__);              \
        }                                                                       \
    } while (0)

// ACS_NOTREACHED(): 到達したら必ず panic する防御的マーカー (リリースでも有効)。
// 「ここには来ないはずだが、もし来たら検知してログを残して安全に落とす」。
// 最適化を効かせたい (本当に到達しないと証明済み) なら ACS_UNREACHABLE() を使う。
#define ACS_NOTREACHED()                                                        \
    ::acs::FPanic(::acs::FSourceLoc::Current(), "<not reached>",                  \
                 "supposedly unreachable code was reached")
