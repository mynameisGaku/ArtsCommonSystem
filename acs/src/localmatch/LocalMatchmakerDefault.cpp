// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// LocalMatchmakerDefault — CLocalMatchmaker を gameframework の既定 IMatchmaker
// provider へ結線する
// -----------------------------------------------------------------------------
// gameframework は ACS::LocalMatch に依存できない (循環依存) ため、結線は backend
// 側 (本 TU) から `acs::game::SetMatchmakerProvider()` を呼んで行う。アプリは起動
// 時に一度 `acs::localmatch::InstallLocalMatchmakerAsDefault()` を呼ぶだけで、以降
// は backend 非依存に `acs::game::GetDefaultMatchmaker()` で実ローカル matchmaker
// を取得できる。
//
// provider が返す matchmaker はプロセス共有 singleton。ローカル実装は既定構築だけ
// で即利用可能 (Init は持たず、既定 MaxRatingDelta で動く) ので、Lua VM のような
// 初回 Init は不要 (function-local static の構築自体は thread-safe)。
// =============================================================================
#include "localmatch/LocalMatchmaker.h"
#include "gameframework/BackendClient.h"

namespace acs::localmatch {

acs::game::IMatchmaker& GetDefaultLocalMatchmaker() noexcept {
    // Meyers singleton。プロセス内 1 個のローカル matchmaker を既定として共有する。
    // 既定構築で即使用可能 (StartSearch から直接利用できる)。
    static CLocalMatchmaker s_matchmaker;
    return s_matchmaker;
}

void InstallLocalMatchmakerAsDefault() noexcept {
    acs::game::SetMatchmakerProvider(&GetDefaultLocalMatchmaker);
}

} // namespace acs::localmatch
