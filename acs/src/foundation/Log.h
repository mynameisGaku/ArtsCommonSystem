// SPDX-License-Identifier: Apache-2.0
// 非同期スレッドセーフ Logger（Vyukov MPMC ring + writer thread）
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/SourceLoc.h"

namespace acs {

// ログレベル（数値が小さいほど詳細）
enum class ELogSeverity : u8 {
    Trace = 0,  // 詳細トレース
    Debug = 1,  // デバッグ情報
    Info  = 2,  // 通常情報
    Warn  = 3,  // 警告
    Error = 4,  // エラー
    Fatal = 5,  // 致命エラー
    Off   = 6,  // 出力停止
};

// ログレベルを文字列化（出力フォーマット用）
constexpr const char* ToString(ELogSeverity s) noexcept {
    switch (s) {
        case ELogSeverity::Trace: return "TRACE";
        case ELogSeverity::Debug: return "DEBUG";
        case ELogSeverity::Info:  return "INFO";
        case ELogSeverity::Warn:  return "WARN";
        case ELogSeverity::Error: return "ERROR";
        case ELogSeverity::Fatal: return "FATAL";
        case ELogSeverity::Off:   return "OFF";
    }
    return "?";
}

// Logger 設定
struct FLogConfig {
    const wchar_t* file_path     = nullptr;            // nullptr ならファイル無効
    ELogSeverity    min_severity  = ELogSeverity::Info;  // 出力最小レベル
    u32            ring_capacity = 4096;               // リング長（2 のべき乗、16 未満は 16 に切り上げ）
    bool           console       = true;               // stdout 出力する
    bool           debug_output  = true;               // OutputDebugStringA 出力する
};

class Logger {
public:
    // 初期化（多重呼び出しは無視）
    static void Init(const FLogConfig& cfg) noexcept;

    // ライタースレッドを停止しリソース解放
    static void Shutdown() noexcept;

    // 残レコードを書き出すまで待つ
    static void Flush() noexcept;

    // 最小ログレベルを動的に変更（スレッドセーフ）
    static void SetMinSeverity(ELogSeverity s) noexcept;

    // 指定レベルが出力対象か
    static bool Enabled(ELogSeverity s) noexcept;

    // リング満杯で破棄されたレコード総数
    static u64 DroppedCount() noexcept;

    // 実書き込み関数（printf 互換、ホットパス肥大化を避けるため NEVERINLINE）
    ACS_NEVERINLINE static void Write(ELogSeverity sev,
                                      FSourceLoc   loc,
                                      const char* fmt,
                                      ...) noexcept;
};

} // namespace acs

// 内部マクロ: 指定レベルが有効ならログを出力
#define ACS_LOG(sev, fmt, ...)                                                 \
    do {                                                                       \
        if (::acs::Logger::Enabled(sev))                                       \
            ::acs::Logger::Write(sev, ::acs::FSourceLoc::Current(),             \
                                 fmt, ##__VA_ARGS__);                          \
    } while (0)

#define ACS_LOG_TRACE(fmt, ...) ACS_LOG(::acs::ELogSeverity::Trace, fmt, ##__VA_ARGS__)
#define ACS_LOG_DEBUG(fmt, ...) ACS_LOG(::acs::ELogSeverity::Debug, fmt, ##__VA_ARGS__)
#define ACS_LOG_INFO(fmt, ...)  ACS_LOG(::acs::ELogSeverity::Info,  fmt, ##__VA_ARGS__)
#define ACS_LOG_WARN(fmt, ...)  ACS_LOG(::acs::ELogSeverity::Warn,  fmt, ##__VA_ARGS__)
#define ACS_LOG_ERROR(fmt, ...) ACS_LOG(::acs::ELogSeverity::Error, fmt, ##__VA_ARGS__)
#define ACS_LOG_FATAL(fmt, ...) ACS_LOG(::acs::ELogSeverity::Fatal, fmt, ##__VA_ARGS__)
