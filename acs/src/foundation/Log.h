// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/Move.h"
#include "foundation/SourceLoc.h"
#include "foundation/TypeTraits.h"

#ifndef ACS_COMPILED_LOG_MIN_SEVERITY
    // 0=Trace ～ 6=Off。既定値は従来どおり全レベルをコンパイル対象にする。
    #define ACS_COMPILED_LOG_MIN_SEVERITY 0
#endif

#if ACS_COMPILED_LOG_MIN_SEVERITY < 0 || ACS_COMPILED_LOG_MIN_SEVERITY > 6
    #error "ACS_COMPILED_LOG_MIN_SEVERITY は 0～6 の範囲で指定してください"
#endif

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

namespace detail {

/**
 * 公開位置が読み取り位置と一致するかを返す。
 *
 * @param published_position プロデューサが公開したリング位置。
 * @param dequeue_position コンシューマが次に読み取るリング位置。
 * @return 空から非空への遷移なら true。
 */
constexpr bool ShouldSignalLogConsumer(u64 published_position, u64 dequeue_position) noexcept
{
    return published_position == dequeue_position;
}

/**
 * 文字列リテラルに printf 書式開始文字が含まれるかを判定する。
 *
 * @tparam N 終端文字を含む文字配列長。
 * @param message 判定する文字配列。
 * @return パーセント記号を含む場合は true。
 */
template <usize N>
constexpr bool ContainsLogFormatMarker(const char (&message)[N]) noexcept
{
    /** 終端文字を除いて確認する文字位置。 */
    for (usize character_index = 0; character_index + 1 < N; ++character_index) {
        if (message[character_index] == '%') return true;
    }
    return false;
}

/** ログ呼び出しが直接コピー経路か printf 整形経路かを表す。 */
enum class ELogDispatchKind : u8 {
    /** 整形せずに直接コピーする。 */
    Literal,

    /** printf 互換の書式処理を行う。 */
    Formatted,
};

/**
 * 文字列リテラルの内容から書き込み経路を分類する。
 *
 * @tparam N 終端文字を含む文字配列長。
 * @param message 分類する文字配列。
 * @return 直接コピーまたは書式処理の種別。
 */
template <usize N>
constexpr ELogDispatchKind ClassifyLogDispatch(const char (&message)[N]) noexcept
{
    return ContainsLogFormatMarker(message) ? ELogDispatchKind::Formatted : ELogDispatchKind::Literal;
}

/**
 * 転送参照から文字列リテラルの配列長を取り出す型特性。
 *
 * @tparam T 判定する書式引数型。
 */
template <typename T>
struct TLogLiteralInfo {
    /** 型が安全な文字列リテラルなら true。 */
    static constexpr bool IsLiteral = false;

    /** 終端文字を含む文字配列長。非リテラルは0。 */
    static constexpr usize Extent = 0;
};

/**
 * const文字配列を安全な文字列リテラルとして扱う特殊化。
 *
 * @tparam N 終端文字を含む文字配列長。
 */
template <usize N>
struct TLogLiteralInfo<const char[N]> {
    /** const文字配列なので常に true。 */
    static constexpr bool IsLiteral = true;

    /** 終端文字を含む文字配列長。 */
    static constexpr usize Extent = N;
};

} // detail 名前空間

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
     * 通常レコードの投入によってライタースレッドへ送った起床通知数を返す。
     *
     * @return 現在のロガー世代で送った通知の累積数。
     */
    static u64 WakeSignalCount() noexcept;

    /**
     * 指定レベルがコンパイル時の最小レベル以上かを返す。
     *
     * @tparam Severity 判定する固定重大度。
     * @return コンパイル対象なら true。
     */
    template <ELogSeverity Severity>
    static constexpr bool CompiledEnabled() noexcept
    {
        return static_cast<u8>(Severity) >= static_cast<u8>(ACS_COMPILED_LOG_MIN_SEVERITY);
    }

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

    /**
     * 整形済みメッセージをリングへ直接積む。
     *
     * printf 置換が不要な文字列リテラル用。length は終端 NUL を含めない。
     * ロガー未初期化、重大度の対象外、またはリング満杯なら出力しない。
     *
     * @param severity 出力する重大度。
     * @param location 呼び出し元のソース位置。
     * @param message 整形済みの文字列。nullptrは"(null)"として扱う。
     * @param length messageの終端文字を含まない長さ。
     */
    ACS_NEVERINLINE static void WriteMessage(ELogSeverity severity, FSourceLoc location, const char* message, usize length) noexcept;

    /**
     * 書式指定子を含まない文字列リテラルを直接書き込みへ振り分ける。
     *
     * '%' を含む場合は従来の printf 互換経路へ戻し、"%%" の意味も維持する。
     * 実際の失敗条件はWriteまたはWriteMessageと同じ。
     *
     * @tparam TFormat 書式引数の転送型。
     * @tparam TArgs 可変長の置換引数型。
     * @param severity 出力する重大度。
     * @param location 呼び出し元のソース位置。
     * @param format 文字列リテラルまたは printf 互換書式。
     * @param args formatへ適用する置換引数。
     */
    template <typename TFormat, typename... TArgs>
    ACS_FORCEINLINE static void WriteDispatch(ELogSeverity severity, FSourceLoc location, TFormat&& format, TArgs&&... args) noexcept
    {
        /** 転送された書式引数から参照を除いた型。 */
        using FFormat = RemoveRefT<TFormat>;
        /** 置換引数がなく、安全な文字配列を直接扱える場合は true。 */
        constexpr bool use_literal_path = sizeof...(TArgs) == 0 && detail::TLogLiteralInfo<FFormat>::IsLiteral;
        if constexpr (use_literal_path) {
            if (detail::ClassifyLogDispatch(format) == detail::ELogDispatchKind::Literal) {
                /** 終端文字を含む文字配列長。 */
                constexpr usize extent = detail::TLogLiteralInfo<FFormat>::Extent;
                WriteMessage(severity, location, format, extent > 0 ? extent - 1 : 0);
                return;
            }
            Write(severity, location, format);
        } else {
            Write(severity, location, format, Forward<TArgs>(args)...);
        }
    }
};

} // acs 名前空間

// 内部マクロ: 指定レベルが有効ならログを出力
#define ACS_LOG(sev, fmt, ...)                                                                                          \
    do {                                                                                                                \
        if (::acs::FLogger::Enabled(sev))                                                                               \
            ::acs::FLogger::WriteDispatch(sev, ::acs::FSourceLoc::Current(), fmt __VA_OPT__(,) __VA_ARGS__);           \
    } while (0)

// 固定レベルは if constexpr で無効レベルの引数評価とコード生成を完全に除去する。
#define ACS_LOG_STATIC(sev, fmt, ...)                                                \
    do {                                                                            \
        if constexpr (::acs::FLogger::CompiledEnabled<sev>()) {                     \
            ACS_LOG(sev, fmt __VA_OPT__(,) __VA_ARGS__);                            \
        }                                                                           \
    } while (0)

#define ACS_LOG_TRACE(fmt, ...) ACS_LOG_STATIC(::acs::ELogSeverity::Trace, fmt __VA_OPT__(,) __VA_ARGS__)
#define ACS_LOG_DEBUG(fmt, ...) ACS_LOG_STATIC(::acs::ELogSeverity::Debug, fmt __VA_OPT__(,) __VA_ARGS__)
#define ACS_LOG_INFO(fmt, ...)  ACS_LOG_STATIC(::acs::ELogSeverity::Info,  fmt __VA_OPT__(,) __VA_ARGS__)
#define ACS_LOG_WARN(fmt, ...)  ACS_LOG_STATIC(::acs::ELogSeverity::Warn,  fmt __VA_OPT__(,) __VA_ARGS__)
#define ACS_LOG_ERROR(fmt, ...) ACS_LOG_STATIC(::acs::ELogSeverity::Error, fmt __VA_OPT__(,) __VA_ARGS__)
#define ACS_LOG_FATAL(fmt, ...) ACS_LOG_STATIC(::acs::ELogSeverity::Fatal, fmt __VA_OPT__(,) __VA_ARGS__)
