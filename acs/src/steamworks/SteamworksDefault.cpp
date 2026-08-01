// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// SteamworksDefault — CSteamworksBridgeImpl を gameframework の既定 Bridge
// provider へ結線する
// -----------------------------------------------------------------------------
// gameframework は ACS::Steamworks に依存できない (循環依存) ため、結線は backend
// 側 (本 TU) から `acs::game::SetSteamworksBridgeProvider()` を呼んで行う。アプリは
// 起動時に一度 `acs::steamworks::InstallSteamworksAsDefault()` を呼ぶだけで、以降は
// backend 非依存に `acs::game::GetDefaultSteamworksBridge()` で実 Bridge を取得できる。
//
// provider が返す Bridge はプロセス共有 singleton。初回アクセス時に Init() を 1 回
// 走らせ、すぐ使える状態で返す (m_Social = &acs::game::GetDefaultSteamworksBridge()
// にそのまま差せる)。Steam クライアント未起動 / AppID 不一致 等で Init() が失敗して
// も参照は返す (呼び出し側が IsInitialized() で判断)。
// =============================================================================
#include "steamworks/SteamworksBridgeImpl.h"

namespace acs::steamworks {

acs::game::ISteamworksBridge& GetDefaultSteamworksBridge() noexcept {
    // Meyers singleton。プロセス内 1 個の実 Bridge を既定として共有する。
    static CSteamworksBridgeImpl s_bridge;
    // 実際の初期化状態を判定に使い、明示 Shutdown 後や Steam client 未起動による
    // 失敗後も、次の取得で Init を再試行できるようにする。
    if (!s_bridge.IsInitialized()) {
        (void)s_bridge.Init();   // 失敗しても Bridge 参照は返す (IsInitialized 等で判断)
    }
    return s_bridge;
}

void InstallSteamworksAsDefault() noexcept {
    acs::game::SetSteamworksBridgeProvider(&GetDefaultSteamworksBridge);
}

} // namespace acs::steamworks
