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
    Info = 2,

    /** 警告。 */
    Warn = 3,

    /** エラー。 */
    Error = 4,

    /** 致命エラー。 */
    Fatal = 5,

    /** 出力停止 (この値を最小レベルにすると何も出力しない)。 */
    Off = 6,
};

/**
 * ログレベルを文字列化する (出力フォーマット用)。
 *
 * @param s 文字列化するレベル。
 * @return レベル名の文字列 (未知の値は "?")。
 */
constexpr const char* ToString(ELogSeverity s) noexcept
{
    switch (s) {
    case ELogSeverity::Trace:
        return "TRACE";
    case ELogSeverity::Debug:
        return "DEBUG";
    case ELogSeverity::Info:
        return "INFO";
    case ELogSeverity::Warn:
        return "WARN";
    case ELogSeverity::Error:
        return "ERROR";
    case ELogSeverity::Fatal:
        return "FATAL";
    case ELogSeverity::Off:
        return "OFF";
    }
    return "?";
}

/**
 * FLogger の初期化設定。
 */
struct FLogConfig {
    /** ログファイルのパス (nullptr ならファイル出力を無効化)。 */
    const wchar_t* file_path = nullptr;

    /** 出力する最小レベル (これ未満は破棄)。 */
    ELogSeverity min_severity = ELogSeverity::Info;

    /** リングバッファ長 (16～65536 の 2 のべき乗へ切り上げ・範囲制限)。 */
    u32 ring_capacity = 4096;

    /** stdout へ出力するか。 */
    bool console = true;

    /** OutputDebugStringA へ出力するか。 */
    bool debug_output = true;
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
     * @param configuration リング容量・出力先・最小レベルなどの設定。
     */
    static void Init(const FLogConfig& configuration) noexcept;

    /**
     * ロガーの全資源が初期化済みで、Write を受け付ける状態かを返す。
     *
     * @return 初期化完了から Shutdown 開始までの間は true。それ以外は false。
     */
    static bool IsInitialized() noexcept;

    /**
     * ライタースレッドを停止し、リング・ファイルなどのリソースを解放する (再 Init 可能になる)。
     * sink callback 内からの呼び出しは、writer 自身の join を避けるため無視される。
     */
    static void Shutdown() noexcept;

    /**
     * 呼び出し時点で受理済みのレコードが、追加シンクへの通知を含めてすべて書き出されるまで待つ
     * (最大約 1 秒)。
     * sink callback 内からの呼び出しは無視される。
     */
    static void Flush() noexcept;

    /**
     * 最小ログレベルを動的に変更する (スレッドセーフ)。
     *
     * @param severity 新しい最小レベル (これ未満は破棄される)。
     */
    static void SetMinSeverity(ELogSeverity severity) noexcept;

    /**
     * 追加のログシンク (コールバック) を設定する。各レコードを writer スレッドが
     * コンソール/ファイルへ出した後に sink(severity, message) も呼ぶ。nullptr で解除。
     *
     * @details エディタがエンジンログを自前のコンソールへ取り込む等に使う。sink は
     *          writer スレッドから呼ばれるため、実装側でスレッド安全にすること。
     *          通常スレッドから呼んだ場合、交換前の sink callback が完了してから戻る。
     *          sink callback 自身から呼んだ場合は現在実行中の自身を待たず、後続レコード用の
     *          pointer だけを交換する。
     *          ロガーの初期化前または終了後に呼んだ場合は無視される。
     * @param sink レコード毎に呼ぶコールバック (message は null 終端)。
     */
    static void SetSink(void (*sink)(ELogSeverity severity, const char* message)) noexcept;

    /**
     * 指定レベルが現在の設定で出力対象かを返す。
     *
     * @param severity 判定するレベル。
     * @return 初期化済みかつ severity が最小レベル以上なら true。
     */
    static bool Enabled(ELogSeverity severity) noexcept;

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
     * @param severity レコードの重大度。
     * @param location 呼び出し位置。
     * @param format printf 形式のメッセージ書式。
     * @param ... format に対応する可変長引数。
     */
    ACS_NEVERINLINE static void Write(ELogSeverity severity, FSourceLoc location, const char* format, ...) noexcept;
};

} // namespace acs

// 内部マクロ: 指定レベルが有効ならログを出力
#define ACS_LOG(sev, fmt, ...)                                                            \
    do {                                                                                  \
        if (::acs::FLogger::Enabled(sev))                                                 \
            ::acs::FLogger::Write(sev, ::acs::FSourceLoc::Current(), fmt, ##__VA_ARGS__); \
    } while (0)

#define ACS_LOG_TRACE(fmt, ...) ACS_LOG(::acs::ELogSeverity::Trace, fmt, ##__VA_ARGS__)
#define ACS_LOG_DEBUG(fmt, ...) ACS_LOG(::acs::ELogSeverity::Debug, fmt, ##__VA_ARGS__)
#define ACS_LOG_INFO(fmt, ...)  ACS_LOG(::acs::ELogSeverity::Info, fmt, ##__VA_ARGS__)
#define ACS_LOG_WARN(fmt, ...)  ACS_LOG(::acs::ELogSeverity::Warn, fmt, ##__VA_ARGS__)
#define ACS_LOG_ERROR(fmt, ...) ACS_LOG(::acs::ELogSeverity::Error, fmt, ##__VA_ARGS__)
#define ACS_LOG_FATAL(fmt, ...) ACS_LOG(::acs::ELogSeverity::Fatal, fmt, ##__VA_ARGS__)
