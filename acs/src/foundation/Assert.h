// =============================================================================
// ACS Foundation — アサーションマクロ
// -----------------------------------------------------------------------------
// 提供マクロ:
//   ACS_ASSERT(expr)            — expr が false ならパニック (リリースで除去可)
//   ACS_ASSERTF(expr, fmt, ...) — printf 風メッセージ付きアサート
//   ACS_VERIFY(expr)            — リリースでも expr は評価される版
//   ACS_UNREACHABLE()           — 到達しないはずの位置に置く
//   ACS_NOT_IMPLEMENTED()       — 未実装をパニックさせる
//
// 失敗したアサートはすべて Panic() を経由する。Panic はファイル/行/関数名/
// 失敗式/メッセージ/シンボル化済みスタックトレースを stderr + デバッガに
// 出力した後、プロセスを TerminateProcess で終了させる。
//
// マクロを切る方法: CMake で ACS_ASSERTS_ENABLED=0 を定義する。
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

    // ---- ACS_ASSERT(expr) ---------------------------------------------
    // expr が偽なら Panic を呼ぶ。expr 文字列もパニックメッセージに含まれる。
    #define ACS_ASSERT(expr)                                                    \
        do {                                                                    \
            if (ACS_UNLIKELY(!(expr))) {                                        \
                ::acs::Panic(::acs::SourceLoc::Current(),                       \
                             ACS_STRINGIFY(expr), "assertion failed");          \
            }                                                                   \
        } while (0)

    // ---- ACS_ASSERTF(expr, fmt, ...) ----------------------------------
    // ACS_ASSERT に printf 風のメッセージを追加できる版。
    // 例: ACS_ASSERTF(idx < size, "index %u out of range (size=%u)", idx, size);
    #define ACS_ASSERTF(expr, fmt, ...)                                         \
        do {                                                                    \
            if (ACS_UNLIKELY(!(expr))) {                                        \
                ::acs::Panic(::acs::SourceLoc::Current(),                       \
                             ACS_STRINGIFY(expr), fmt, ##__VA_ARGS__);          \
            }                                                                   \
        } while (0)

    // ---- ACS_UNREACHABLE() --------------------------------------------
    // 「ここには絶対に到達しない」と宣言する箇所に置く。デバッグでパニック。
    #define ACS_UNREACHABLE()                                                   \
        ::acs::Panic(::acs::SourceLoc::Current(), "<unreachable>",              \
                     "unreachable code reached")

    // ---- ACS_NOT_IMPLEMENTED() ----------------------------------------
    // まだ実装していない関数や分岐に置く。
    #define ACS_NOT_IMPLEMENTED()                                               \
        ::acs::Panic(::acs::SourceLoc::Current(), "<not implemented>",          \
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
    #define ACS_NOT_IMPLEMENTED()           ::acs::Panic(::acs::SourceLoc::Current(), "<not implemented>", "feature not implemented")

#endif

// ---- ACS_VERIFY(expr) ------------------------------------------------
// ACS_ASSERT と異なり、リリースでも expr は必ず評価される。
// 副作用のある式（関数呼び出しなど）に対するチェックで使用。
#if ACS_ASSERTS_ENABLED
    #define ACS_VERIFY(expr) ACS_ASSERT(expr)
#else
    #define ACS_VERIFY(expr) do { (void)(expr); } while (0)
#endif
