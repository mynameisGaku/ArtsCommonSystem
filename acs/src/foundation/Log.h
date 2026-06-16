// SPDX-License-Identifier: Apache-2.0
// 非同期スレッドセーフ FLogger（Vyukov MPMC ring + writer thread）
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/SourceLoc.h"

namespace acs {

/**
 * ログの重大度レベル (数値が小さいほど詳細)。
 *
 * @details min_severity と比較して出力可否を決める。Off は全出力を止める番兵。
 */
enum class ELogSeverity : u8 {
    /** 詳細トレース。 */
    Trace = 0,

    /** デバッグ情報。 */
    Debug = 1,

    /** 通常情報。 */
    Info  = 2,

    /** 警告。 */
    Warn  = 3,

    /** エラー。 */
    Error = 4,

    /** 致命エラー。 */
    Fatal = 5,

    /** 出力停止 (この値を最小レベルにすると何も出力しない)。 */
    Off   = 6,
};

/**
 * ログレベルを文字列化する (出力フォーマット用)。
 *
 * @param s 文字列化するレベル。
 * @return レベル名の文字列 (未知の値は "?")。
 */
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

/**
 * FLogger の初期化設定。
 */
struct FLogConfig {
    /** ログファイルのパス (nullptr ならファイル出力を無効化)。 */
    const wchar_t* file_path     = nullptr;

    /** 出力する最小レベル (これ未満は破棄)。 */
    ELogSeverity    min_severity  = ELogSeverity::Info;

    /** リングバッファ長 (2 のべき乗、16 未満は 16 に切り上げ)。 */
    u32            ring_capacity = 4096;

    /** stdout へ出力するか。 */
    bool           console       = true;

    /** OutputDebugStringA へ出力するか。 */
    bool           debug_output  = true;
};

/**
 * 非同期・スレッドセーフなグローバルロガー。
 *
 * @details
 * Vyukov 風 MPMC リングにレコードを積み、専用ライタースレッドが取り出して
 * コンソール / ファイル / デバッガに書き出す。全メンバが static のシングルトン。
 */
class FLogger {
public:
    /**
     * ロガーを初期化する (多重呼び出しは無視)。
     *
     * @param cfg リング容量・出力先・最小レベルなどの設定。
     */
    static void Init(const FLogConfig& cfg) noexcept;

    /** ライタースレッドを停止し、リング・ファイルなどのリソースを解放する (再 Init 可能になる)。 */
    static void Shutdown() noexcept;

    /** リングに残ったレコードがすべて書き出されるまで待つ (最大約 1 秒)。 */
    static void Flush() noexcept;

    /**
     * 最小ログレベルを動的に変更する (スレッドセーフ)。
     *
     * @param s 新しい最小レベル (これ未満は破棄される)。
     */
    static void SetMinSeverity(ELogSeverity s) noexcept;

    /**
     * 追加のログシンク (コールバック) を設定する。各レコードを writer スレッドが
     * コンソール/ファイルへ出した後に sink(severity, message) も呼ぶ。nullptr で解除。
     *
     * @details エディタがエンジンログを自前のコンソールへ取り込む等に使う。sink は
     *          writer スレッドから呼ばれるため、実装側でスレッド安全にすること。
     * @param sink レコード毎に呼ぶコールバック (message は null 終端)。
     */
    static void SetSink(void (*sink)(ELogSeverity severity, const char* message)) noexcept;

    /**
     * 指定レベルが現在の設定で出力対象かを返す。
     *
     * @param s 判定するレベル。
     * @return 初期化済みかつ s が最小レベル以上なら true。
     */
    static bool Enabled(ELogSeverity s) noexcept;

    /**
     * リング満杯で破棄されたレコードの累積総数を返す。
     *
     * @return 起動以降に drop したレコード数の累計。
     */
    static u64 DroppedCount() noexcept;

    /**
     * 1 レコードをリングに積む実書き込み関数 (printf 互換)。
     *
     * @details ホットパスの肥大化を避けるため NEVERINLINE。通常は ACS_LOG_* マクロ経由で呼ぶ。
     * @param sev レコードの重大度。
     * @param loc 呼び出し位置。
     * @param fmt printf 形式のメッセージ書式。
     * @param ... fmt に対応する可変長引数。
     */
    ACS_NEVERINLINE static void Write(ELogSeverity sev,
                                      FSourceLoc   loc,
                                      const char* fmt,
                                      ...) noexcept;
};

} // namespace acs

// 内部マクロ: 指定レベルが有効ならログを出力
#define ACS_LOG(sev, fmt, ...)                                                 \
    do {                                                                       \
        if (::acs::FLogger::Enabled(sev))                                       \
            ::acs::FLogger::Write(sev, ::acs::FSourceLoc::Current(),             \
                                 fmt, ##__VA_ARGS__);                          \
    } while (0)

#define ACS_LOG_TRACE(fmt, ...) ACS_LOG(::acs::ELogSeverity::Trace, fmt, ##__VA_ARGS__)
#define ACS_LOG_DEBUG(fmt, ...) ACS_LOG(::acs::ELogSeverity::Debug, fmt, ##__VA_ARGS__)
#define ACS_LOG_INFO(fmt, ...)  ACS_LOG(::acs::ELogSeverity::Info,  fmt, ##__VA_ARGS__)
#define ACS_LOG_WARN(fmt, ...)  ACS_LOG(::acs::ELogSeverity::Warn,  fmt, ##__VA_ARGS__)
#define ACS_LOG_ERROR(fmt, ...) ACS_LOG(::acs::ELogSeverity::Error, fmt, ##__VA_ARGS__)
#define ACS_LOG_FATAL(fmt, ...) ACS_LOG(::acs::ELogSeverity::Fatal, fmt, ##__VA_ARGS__)
