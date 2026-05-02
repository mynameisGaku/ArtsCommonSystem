// 非同期スレッドセーフ Logger（Vyukov MPMC ring + writer thread）
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/SourceLoc.h"

namespace acs {

// ログレベル（数値が小さいほど詳細）
enum class LogSeverity : u8 {
    Trace = 0,  // 詳細トレース
    Debug = 1,  // デバッグ情報
    Info  = 2,  // 通常情報
    Warn  = 3,  // 警告
    Error = 4,  // エラー
    Fatal = 5,  // 致命エラー
    Off   = 6,  // 出力停止
};

// ログレベルを文字列化（出力フォーマット用）
constexpr const char* ToString(LogSeverity s) noexcept {
    switch (s) {
        case LogSeverity::Trace: return "TRACE";
        case LogSeverity::Debug: return "DEBUG";
        case LogSeverity::Info:  return "INFO";
        case LogSeverity::Warn:  return "WARN";
        case LogSeverity::Error: return "ERROR";
        case LogSeverity::Fatal: return "FATAL";
        case LogSeverity::Off:   return "OFF";
    }
    return "?";
}

// Logger 設定
struct LogConfig {
    const wchar_t* file_path     = nullptr;            // nullptr ならファイル無効
    LogSeverity    min_severity  = LogSeverity::Info;  // 出力最小レベル
    u32            ring_capacity = 4096;               // リング長（2 のべき乗、16 未満は 16 に切り上げ）
    bool           console       = true;               // stdout 出力する
    bool           debug_output  = true;               // OutputDebugStringA 出力する
};

class Logger {
public:
    // 初期化（多重呼び出しは無視）
    static void Init(const LogConfig& cfg) noexcept;

    // ライタースレッドを停止しリソース解放
    static void Shutdown() noexcept;

    // 残レコードを書き出すまで待つ
    static void Flush() noexcept;

    // 最小ログレベルを動的に変更（スレッドセーフ）
    static void SetMinSeverity(LogSeverity s) noexcept;

    // 指定レベルが出力対象か
    static bool Enabled(LogSeverity s) noexcept;

    // リング満杯で破棄されたレコード総数
    static u64 DroppedCount() noexcept;

    // 実書き込み関数（printf 互換、ホットパス肥大化を避けるため NEVERINLINE）
    ACS_NEVERINLINE static void Write(LogSeverity sev,
                                      SourceLoc   loc,
                                      const char* fmt,
                                      ...) noexcept;
};

} // namespace acs

// 内部マクロ: 指定レベルが有効ならログを出力
#define ACS_LOG(sev, fmt, ...)                                                 \
    do {                                                                       \
        if (::acs::Logger::Enabled(sev))                                       \
            ::acs::Logger::Write(sev, ::acs::SourceLoc::Current(),             \
                                 fmt, ##__VA_ARGS__);                          \
    } while (0)

#define ACS_LOG_TRACE(fmt, ...) ACS_LOG(::acs::LogSeverity::Trace, fmt, ##__VA_ARGS__)
#define ACS_LOG_DEBUG(fmt, ...) ACS_LOG(::acs::LogSeverity::Debug, fmt, ##__VA_ARGS__)
#define ACS_LOG_INFO(fmt, ...)  ACS_LOG(::acs::LogSeverity::Info,  fmt, ##__VA_ARGS__)
#define ACS_LOG_WARN(fmt, ...)  ACS_LOG(::acs::LogSeverity::Warn,  fmt, ##__VA_ARGS__)
#define ACS_LOG_ERROR(fmt, ...) ACS_LOG(::acs::LogSeverity::Error, fmt, ##__VA_ARGS__)
#define ACS_LOG_FATAL(fmt, ...) ACS_LOG(::acs::LogSeverity::Fatal, fmt, ##__VA_ARGS__)
