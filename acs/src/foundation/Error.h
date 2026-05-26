// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Foundation — エラーコード型 (TResult<T,E> の E パラメータ)
// -----------------------------------------------------------------------------
// 例外を使わない方針のため、エラーは戻り値として伝搬する。FErrorCode は
// カテゴリ + サブコード + 任意の OS エラー値 + メッセージ + 発生位置 を
// まとめて持つ POD 構造体である。
//
// 設計の意図:
//   - カテゴリで大まかに分類し、ロガー側でフィルタしやすくする
//   - サブコードはモジュール固有の細分化に使う
//   - FSourceLoc を埋めることで「どこで失敗したか」を常時追跡できる
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/SourceLoc.h"

namespace acs {

// ---- エラーカテゴリ -------------------------------------------------------
// すべてのエラーはこのいずれかに分類される。新カテゴリの追加は安全。
enum class ErrCategory : u16 {
    None        = 0,    // 成功（IsOk() の判定に使用）
    Generic     = 1,    // どのカテゴリにも当てはまらない一般エラー
    Memory      = 2,    // メモリ割り当て失敗 / OOM / アライメント違反
    OS          = 3,    // OS / Win32 システムコールの失敗
    IO          = 4,    // ファイル / ネットワーク I/O 失敗
    Container   = 5,    // 範囲外参照 / 容量超過 / キー未検出
    Threading   = 6,    // 排他制御失敗 / デッドロック検出 / スレッド生成失敗
    Math        = 7,    // 数学領域エラー（NaN / 0 除算 等）
    Render      = 8,    // グラフィックスバックエンドの失敗 (DX12 等)
    Asset       = 9,    // アセット読み込み / デコード失敗
    UserCancel  = 10,   // ユーザーによるキャンセル
};

// ---- FErrorCode 本体 -------------------------------------------------------
// すべてのフィールドは public（POD として扱えるように）。トリビアル型なので
// TResult<T, FErrorCode> の中で零コストで保持できる。
struct FErrorCode {
    ErrCategory category   = ErrCategory::None;  // 大カテゴリ
    u16         subcode    = 0;                  // モジュール固有コード
    u32         os_error   = 0;                  // GetLastError() / HRESULT
    const char* message    = nullptr;            // 静的文字列リテラル推奨
    FSourceLoc   loc        = {};                 // 発生位置

    // デフォルト構築は「成功」状態。
    constexpr FErrorCode() noexcept = default;

    // 完全な初期化を行うコンストラクタ。ACS_ERR マクロから呼ばれる。
    constexpr FErrorCode(ErrCategory c,
                        u16 sub,
                        const char* msg,
                        FSourceLoc l = FSourceLoc::Current(),
                        u32 os = 0) noexcept
        : category(c), subcode(sub), os_error(os), message(msg), loc(l) {}

    // 成功状態かを判定（None カテゴリ == 成功）
    constexpr bool IsOk() const noexcept { return category == ErrCategory::None; }
    // bool 変換: true なら「エラーあり」を意味する（if (e) { ... } で判定可能）
    constexpr explicit operator bool() const noexcept { return !IsOk(); }
};

// カテゴリの文字列化（ロガー出力用）
constexpr const char* ToString(ErrCategory c) noexcept {
    switch (c) {
        case ErrCategory::None:       return "None";
        case ErrCategory::Generic:    return "Generic";
        case ErrCategory::Memory:     return "Memory";
        case ErrCategory::OS:         return "OS";
        case ErrCategory::IO:         return "IO";
        case ErrCategory::Container:  return "Container";
        case ErrCategory::Threading:  return "Threading";
        case ErrCategory::Math:       return "Math";
        case ErrCategory::Render:     return "Render";
        case ErrCategory::Asset:      return "Asset";
        case ErrCategory::UserCancel: return "UserCancel";
    }
    return "Unknown";
}

// ---- 簡易構築マクロ -------------------------------------------------------
// 呼び出し元の FSourceLoc を自動的にキャプチャしつつ FErrorCode を生成する。
//
// 使い方:
//   return ACS_ERR(IO, 1, "ファイルが開けません");
//   return ACS_ERR_OS(OS, 5, "CreateFile failed", GetLastError());
#define ACS_ERR(cat, sub, msg) \
    ::acs::FErrorCode(::acs::ErrCategory::cat, static_cast<::acs::u16>(sub), (msg), ::acs::FSourceLoc::Current())

#define ACS_ERR_OS(cat, sub, msg, os_err) \
    ::acs::FErrorCode(::acs::ErrCategory::cat, static_cast<::acs::u16>(sub), (msg), ::acs::FSourceLoc::Current(), (os_err))

} // namespace acs
