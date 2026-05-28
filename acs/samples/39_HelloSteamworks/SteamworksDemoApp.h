// SPDX-License-Identifier: Apache-2.0
// HelloSteamworks — ISteamworksBridge の使い方デモ。
//
// 動作:
//   ・起動時に Steamworks backend を初期化 (real / stub 自動選択)
//   ・1: 実績解除 (ACH_WIN_ONE_GAME)
//   ・2: stat "demo_clicks" を +1
//   ・3: leaderboard "demo_board" に score 投稿
//   ・4: Cloud save に "save.dat" を書き込み
//   ・画面に local player 名 / backend 種別 / 最後の操作結果を表示
//   ・Esc 終了
//
// ビルド:
//   stub mode (default): cmake (何も指定しない) → 全 API が NotImplemented
//   real mode: -DACS_BUILD_STEAMWORKS=ON -DACS_STEAMWORKS_SDK_DIR=<sdk>
//              + Steam クライアント起動 + steam_appid.txt (480)
#pragma once

#include "app/Application.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "gameframework/SteamworksBridge.h"
#if WITH_ACS_STEAMWORKS
#  include "steamworks/SteamworksBridgeImpl.h"
#endif
#include "foundation/Types.h"

namespace hellosteam {

class SteamworksDemoApp : public acs::FApplication {
public:
    void OnStart()             noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()            noexcept override;
    void OnShutdown()          noexcept override;

private:
    void SetStatus(const char* msg) noexcept;

    acs::FSpriteBatch m_Batch;
    acs::Font         m_Font;
    acs::Font         m_SmallFont;

    // bridge は I/F 経由で使い、実装は real / stub を DI。
    acs::game::ISteamworksBridge* m_Social = nullptr;
#if WITH_ACS_STEAMWORKS
    acs::steamworks::FSteamworksBridgeImpl m_RealSocial;
#endif
    bool      m_bUsingReal = false;
    bool      m_bFontReady = false;
    char      m_StatusLine[160] = "Ready";
    char      m_PlayerLine[128] = "(not initialized)";
    acs::i64  m_LastScore = 0;
};

} // namespace hellosteam
