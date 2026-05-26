// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar S — SteamworksBridge 実装 (Stub のみ)
//
// 本 .cpp では `ISteamworksBridge` の I/F 自体は提供せず、Steamworks SDK 未統合
// ビルドでも常に使える `SteamworksBridgeStub` のみを実装する。実 SDK を使う
// `GoldenSteamworksBridge` は Steamworks SDK ヘッダ / ライブラリ依存を含むため、
// 別モジュール (将来の `acs_steamworks` 等) で独立に実装し、本ファイルには
// 持ち込まない。
//
// 設計のポイント:
//   ・Stub は副作用ゼロ。`Init()` は単に `_initialized = true` を立てるのみ。
//   ・Achievement / Leaderboard 系は `ACS_ERR(Generic, kSubSteamworksNotImplemented,
//     ...)` を返し、上位ロジックでサイレントに無視するか or ログ出力するかを
//     呼び出し側に委ねる。
//   ・`GetStub()` は Meyer's singleton。スレッド初回構築は C++11 以降の規格で
//     保証されているため、追加同期は不要。
//   ・文字列リテラルは static storage duration なので、PlayerIdentity に
//     そのまま入れても寿命は永続。

#include "gameframework/SteamworksBridge.h"

#include "foundation/Error.h"

namespace acs::game {

// ---- Stub: Init / Shutdown ------------------------------------------------

TResult<void> SteamworksBridgeStub::Init() noexcept {
    // 多重 Init は明示的に許容する。実 SDK の SteamAPI_Init() は失敗時 false を
    // 返すが、Stub はテスト容易性のため常に成功。
    _initialized = true;
    return Ok();
}

void SteamworksBridgeStub::Shutdown() noexcept {
    // Init() 前に呼ばれても安全。
    _initialized = false;
}

// ---- Stub: ローカルプレイヤー -------------------------------------------

PlayerIdentity SteamworksBridgeStub::GetLocalPlayer() const noexcept {
    PlayerIdentity id{};
    if (!_initialized) {
        // 未初期化時は全フィールド空のまま返す (Bridge は throw しない方針)。
        return id;
    }
    // Stub の固定ダミー値。`platform_id` / `display_name` は string literal なので
    // プロセス終了まで生存可能。
    id.platform_id   = "stub_player";
    id.display_name  = "Player";
    id.session_token = 0;  // Stub にはセッション概念がない
    return id;
}

// ---- Stub: Achievement / Leaderboard ------------------------------------

TResult<void> SteamworksBridgeStub::UnlockAchievement(const char* ach_id) noexcept {
    (void)ach_id;  // 未使用引数 (Stub なので no-op)
    if (!_initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized,
                       "SteamworksBridgeStub::UnlockAchievement called before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented,
                   "SteamworksBridgeStub: UnlockAchievement is not implemented (link real SDK)");
}

TResult<void> SteamworksBridgeStub::SetLeaderboardScore(const char* board_id, i64 score) noexcept {
    (void)board_id;
    (void)score;
    if (!_initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized,
                       "SteamworksBridgeStub::SetLeaderboardScore called before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented,
                   "SteamworksBridgeStub: SetLeaderboardScore is not implemented (link real SDK)");
}

TResult<i64> SteamworksBridgeStub::GetLeaderboardScore(const char* board_id) noexcept {
    (void)board_id;
    if (!_initialized) {
        return TResult<i64>(ACS_ERR(Generic, kSubSteamworksNotInitialized,
                                   "SteamworksBridgeStub::GetLeaderboardScore called before Init()"));
    }
    return TResult<i64>(ACS_ERR(Generic, kSubSteamworksNotImplemented,
                               "SteamworksBridgeStub: GetLeaderboardScore is not implemented (link real SDK)"));
}

// ---- Stub: Tick ----------------------------------------------------------

void SteamworksBridgeStub::Tick(f32 dt) noexcept {
    (void)dt;  // Stub は callback pump を持たないので何もしない
}

// ---- static singleton ---------------------------------------------------

SteamworksBridgeStub& SteamworksBridgeStub::GetStub() noexcept {
    // C++11 以降、関数スコープ static の初期化は thread-safe。
    static SteamworksBridgeStub _instance;
    return _instance;
}

} // namespace acs::game
