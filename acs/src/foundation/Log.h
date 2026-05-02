// =============================================================================
// ACS Foundation — 非同期スレッドセーフ Logger
// -----------------------------------------------------------------------------
// Phase 1 の構成:
//   - Vyukov 境界付き MPMC リング（シーケンス番号方式）
//   - プロデューサ側: vsnprintf でセル内に書き込み、シーケンス番号の release
//     ストアで公開
//   - コンシューマ側: 専用ライタースレッドが定期的にドレイン
//   - 出力先 (cfg で選択):
//       * stdout (cfg.console)
//       * OutputDebugString (cfg.debug_output)
//       * ファイル (cfg.file_path)
//   - バックプレッシャ: リングが満杯なら DROP し、dropped カウンタを加算
//
// すべての _Interlocked* は ARM64 では _acq / _rel サフィックス版を使い、
// x64 では完全バリアの基本版を使う（ハードウェアレベルで等価）。
// STL / 例外 / RTTI を一切使わない。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/SourceLoc.h"

namespace acs {

// ---- ログレベル ---------------------------------------------------------
// 数値が小さいほど詳細。Off は出力完全無効化。
enum class LogSeverity : u8 {
    Trace = 0,  // 詳細トレース（毎フレームの細かいイベント）
    Debug = 1,  // デバッグ情報（開発時のみ有用）
    Info  = 2,  // 通常情報（起動完了、状態遷移など）
    Warn  = 3,  // 警告（動作は続くが要注意）
    Error = 4,  // エラー（処理失敗）
    Fatal = 5,  // 致命エラー（プロセス継続困難）
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

// ---- Logger 設定 -------------------------------------------------------
struct LogConfig {
    const wchar_t* file_path     = nullptr;            // nullptr ならファイル無効
    LogSeverity    min_severity  = LogSeverity::Info;  // 出力最小レベル
    u32            ring_capacity = 4096;               // リング長 (2 のべき乗、16 未満は 16 に切り上げ)
    bool           console       = true;               // stdout 出力する
    bool           debug_output  = true;               // OutputDebugStringA 出力する
};

// ---- Logger 静的 API ---------------------------------------------------
class Logger {
public:
    // 初期化。多重呼び出しは無視される（単一ロガー想定）。
    static void Init(const LogConfig& cfg) noexcept;

    // ライタースレッドを停止し、リソースを開放する。
    static void Shutdown() noexcept;

    // 同期点。残っているレコードをすべて書き出す（テスト/シャットダウン用）。
    static void Flush() noexcept;

    // 最小ログレベルを動的に変更する。スレッドセーフ。
    static void SetMinSeverity(LogSeverity s) noexcept;

    // 指定レベルの出力が有効かを取得（マクロ側で軽量フィルタするため）。
    static bool Enabled(LogSeverity s) noexcept;

    // リング満杯で破棄されたレコード数の合計。
    static u64 DroppedCount() noexcept;

    // 実際の書き込み関数。フォーマットは printf 互換。
    // ※ ホットパスのインライン肥大化を避けるため NEVERINLINE を付与。
    ACS_NEVERINLINE static void Write(LogSeverity sev,
                                      SourceLoc   loc,
                                      const char* fmt,
                                      ...) noexcept;
};

} // namespace acs

// =============================================================================
// レベル別マクロ
// -----------------------------------------------------------------------------
// 例:
//   ACS_LOG_INFO("started: build=%s", build_tag);
//   ACS_LOG_ERROR("could not open %s (err=%lu)", path, GetLastError());
// =============================================================================
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
