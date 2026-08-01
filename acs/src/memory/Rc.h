// SPDX-License-Identifier: Apache-2.0
// ACS Memory — Rc.h【非推奨・互換シム】
//
// TRc<T> / MakeRc / MakeRcIn は TSharedPtr<T> / MakeShared / MakeSharedIn に
// 改名された。本ヘッダは旧名で書かれた既存コードを壊さないためのエイリアス。
// 新規コードでは memory/SharedPtr.h の TSharedPtr / MakeShared を直接使うこと。
#pragma once

#include "memory/SharedPtr.h"

namespace acs {

/**
 * TSharedPtr<T> の旧名エイリアス (非推奨)。
 *
 * @details 旧名 TRc で書かれた既存コードの互換用。新規コードでは TSharedPtr を使うこと。
 * @tparam T 共有所有する対象の型。
 */
template<typename T> using TRc = TSharedPtr<T>;

/**
 * MakeShared の旧名エイリアス (非推奨)。
 *
 * @details デフォルトアロケータで T を構築し共有ポインタを返す。新規コードでは MakeShared を使うこと。
 * @tparam T 構築するオブジェクト型。
 * @tparam Args T のコンストラクタ引数型。
 * @param args T のコンストラクタへ転送する引数。
 * @return 構築した対象を共有所有する TRc (確保失敗時は空)。
 */
template<typename T, typename... Args>
ACS_FORCEINLINE TRc<T> MakeRc(Args&&... args) noexcept {
    return MakeShared<T>(Forward<Args>(args)...);
}

/**
 * MakeSharedIn の旧名エイリアス (非推奨)。
 *
 * @details 指定アロケータで T を構築し共有ポインタを返す。新規コードでは MakeSharedIn を使うこと。
 * @tparam T 構築するオブジェクト型。
 * @tparam Args T のコンストラクタ引数型。
 * @param a 確保・解放に使うアロケータ。
 * @param args T のコンストラクタへ転送する引数。
 * @return 構築した対象を共有所有する TRc (確保失敗時は空)。
 */
template<typename T, typename... Args>
ACS_FORCEINLINE TRc<T> MakeRcIn(IAllocator& a, Args&&... args) noexcept {
    return MakeSharedIn<T>(a, Forward<Args>(args)...);
}

} // namespace acs
