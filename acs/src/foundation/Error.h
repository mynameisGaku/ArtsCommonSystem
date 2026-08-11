// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/SourceLoc.h"

namespace acs {

/**
 * エラーの大カテゴリ。
 *
 * @details
 * すべてのエラーはこのいずれかに分類され、ロガー側でのフィルタに使う。
 * 末尾への新カテゴリ追加は安全 (既存値は不変)。
 */
enum class EErrCategory : u16 {
    /** 成功 (IsOk() の判定に使う、エラーなしを表す)。 */
    None        = 0,

    /** どのカテゴリにも当てはまらない一般エラー。 */
    Generic     = 1,

    /** メモリ割り当て失敗 / OOM / アライメント違反。 */
    Memory      = 2,

    /** OS / Win32 システムコールの失敗。 */
    OS          = 3,

    /** ファイル / ネットワーク I/O 失敗。 */
    IO          = 4,

    /** 範囲外参照 / 容量超過 / キー未検出。 */
    Container   = 5,

    /** 排他制御失敗 / デッドロック検出 / スレッド生成失敗。 */
    Threading   = 6,

    /** 数学領域エラー (NaN / 0 除算 等)。 */
    Math        = 7,

    /** グラフィックスバックエンドの失敗 (DX12 等)。 */
    Render      = 8,

    /** アセット読み込み / デコード失敗。 */
    Asset       = 9,

    /** ユーザーによるキャンセル。 */
    UserCancel  = 10,
};

/**
 * 1 件のエラーを表す POD 構造体 (TResult の E パラメータの既定型)。
 *
 * @details
 * カテゴリ + サブコード + 任意の OS エラー値 + メッセージ + 発生位置をまとめて持つ。
 * 全フィールドが public でトリビアル型なので、TResult<T, FErrorCode> の中で
 * ゼロコストに保持できる。
 */
struct FErrorCode {
    /** 大カテゴリ (None は成功を表す)。 */
    EErrCategory category   = EErrCategory::None;

    /** モジュール固有の細分化コード。 */
    u16         subcode    = 0;

    /** OS エラー値 (GetLastError() / HRESULT 等)。 */
    u32         os_error   = 0;

    /** エラーメッセージ (静的文字列リテラル推奨)。 */
    const char* message    = nullptr;

    /** エラーの発生位置。 */
    FSourceLoc   loc        = {};

    /** デフォルト構築する (category=None の「成功」状態)。 */
    constexpr FErrorCode() noexcept = default;

    /**
     * 全フィールドを初期化する (ACS_ERR マクロから呼ばれる)。
     *
     * @param c 大カテゴリ。
     * @param sub モジュール固有のサブコード。
     * @param msg エラーメッセージ (静的文字列推奨)。
     * @param l 発生位置 (既定: 呼び出し位置)。
     * @param os OS エラー値 (既定 0)。
     */
    constexpr FErrorCode(EErrCategory c,
                        u16 sub,
                        const char* msg,
                        FSourceLoc l = FSourceLoc::Current(),
                        u32 os = 0) noexcept
        : category(c), subcode(sub), os_error(os), message(msg), loc(l) {}

    /**
     * 成功状態かを判定する。
     *
     * @return category が None (= 成功) なら true。
     */
    constexpr bool IsOk() const noexcept { return category == EErrCategory::None; }

    /**
     * bool 変換: エラーを保持しているかを返す (`if (e) { ... }` で判定可能)。
     *
     * @return エラーあり (category != None) なら true。
     */
    constexpr explicit operator bool() const noexcept { return !IsOk(); }
};

/**
 * エラーカテゴリを文字列化する (ロガー出力用)。
 *
 * @param c 文字列化するカテゴリ。
 * @return カテゴリ名の文字列 (未知の値は "Unknown")。
 */
constexpr const char* ToString(EErrCategory c) noexcept {
    switch (c) {
        case EErrCategory::None:       return "None";
        case EErrCategory::Generic:    return "Generic";
        case EErrCategory::Memory:     return "Memory";
        case EErrCategory::OS:         return "OS";
        case EErrCategory::IO:         return "IO";
        case EErrCategory::Container:  return "Container";
        case EErrCategory::Threading:  return "Threading";
        case EErrCategory::Math:       return "Math";
        case EErrCategory::Render:     return "Render";
        case EErrCategory::Asset:      return "Asset";
        case EErrCategory::UserCancel: return "UserCancel";
    }
    return "Unknown";
}

// 呼び出し元の FSourceLoc を自動的にキャプチャしつつ FErrorCode を生成する。
//
#define ACS_ERR(cat, sub, msg) \
    ::acs::FErrorCode(::acs::EErrCategory::cat, static_cast<::acs::u16>(sub), (msg), ::acs::FSourceLoc::Current())

#define ACS_ERR_OS(cat, sub, msg, os_err) \
    ::acs::FErrorCode(::acs::EErrCategory::cat, static_cast<::acs::u16>(sub), (msg), ::acs::FSourceLoc::Current(), (os_err))

} // namespace acs
