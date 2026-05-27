// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar S — FSteamworksBridge 実装 (Stub のみ)
//
// 本 .cpp では `ISteamworksBridge` の I/F 自体は提供せず、Steamworks SDK 未統合
// ビルドでも常に使える `SteamworksBridgeStub` のみを実装する。実 SDK を使う
// `GoldenSteamworksBridge` は Steamworks SDK ヘッダ / ライブラリ依存を含むため、
// 別モジュール (将来の `acs_steamworks` 等) で独立に実装し、本ファイルには
// 持ち込まない。
//
// 設計のポイント:
//   ・Stub は副作用ゼロ。`Init()` は単に `m_Initialized = true` を立てるのみ。
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
    m_Initialized = true;
    return Ok();
}

void SteamworksBridgeStub::Shutdown() noexcept {
    // Init() 前に呼ばれても安全。
    m_Initialized = false;
}

// ---- Stub: ローカルプレイヤー -------------------------------------------

PlayerIdentity SteamworksBridgeStub::GetLocalPlayer() const noexcept {
    PlayerIdentity id{};
    if (!m_Initialized) {
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
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized,
                       "SteamworksBridgeStub::UnlockAchievement called before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented,
                   "SteamworksBridgeStub: UnlockAchievement is not implemented (link real SDK)");
}

TResult<void> SteamworksBridgeStub::SetLeaderboardScore(const char* board_id, i64 score) noexcept {
    (void)board_id;
    (void)score;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized,
                       "SteamworksBridgeStub::SetLeaderboardScore called before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented,
                   "SteamworksBridgeStub: SetLeaderboardScore is not implemented (link real SDK)");
}

TResult<i64> SteamworksBridgeStub::GetLeaderboardScore(const char* board_id) noexcept {
    (void)board_id;
    if (!m_Initialized) {
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

// ---- Stub: Phase 2 (Stats / DLC / RichPresence / Friends) ---------------

TResult<void> SteamworksBridgeStub::SetStat(const char* stat_name, i64 value) noexcept {
    (void)stat_name; (void)value;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized,
                       "SteamworksBridgeStub::SetStat called before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented,
                   "SteamworksBridgeStub: SetStat is not implemented (link real SDK)");
}

TResult<i64> SteamworksBridgeStub::GetStat(const char* stat_name) noexcept {
    (void)stat_name;
    if (!m_Initialized) {
        return TResult<i64>(ACS_ERR(Generic, kSubSteamworksNotInitialized,
                                   "SteamworksBridgeStub::GetStat called before Init()"));
    }
    return TResult<i64>(ACS_ERR(Generic, kSubSteamworksNotImplemented,
                               "SteamworksBridgeStub: GetStat is not implemented (link real SDK)"));
}

TResult<void> SteamworksBridgeStub::SetFloatStat(const char* stat_name, f32 value) noexcept {
    (void)stat_name; (void)value;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized,
                       "SteamworksBridgeStub::SetFloatStat called before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented,
                   "SteamworksBridgeStub: SetFloatStat is not implemented (link real SDK)");
}

TResult<f32> SteamworksBridgeStub::GetFloatStat(const char* stat_name) noexcept {
    (void)stat_name;
    if (!m_Initialized) {
        return TResult<f32>(ACS_ERR(Generic, kSubSteamworksNotInitialized,
                                   "SteamworksBridgeStub::GetFloatStat called before Init()"));
    }
    return TResult<f32>(ACS_ERR(Generic, kSubSteamworksNotImplemented,
                               "SteamworksBridgeStub: GetFloatStat is not implemented (link real SDK)"));
}

bool SteamworksBridgeStub::IsDlcOwned(u32 app_id) const noexcept {
    (void)app_id;
    return false;  // Stub: 何も所有していない
}

TResult<void> SteamworksBridgeStub::SetRichPresence(const char* key, const char* value) noexcept {
    (void)key; (void)value;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized,
                       "SteamworksBridgeStub::SetRichPresence called before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented,
                   "SteamworksBridgeStub: SetRichPresence is not implemented (link real SDK)");
}

u32 SteamworksBridgeStub::GetFriendCount() const noexcept {
    return 0;  // Stub: フレンドリスト無し
}

PlayerIdentity SteamworksBridgeStub::GetFriendByIndex(u32 index) const noexcept {
    (void)index;
    return PlayerIdentity{};  // Stub: 空
}

// ---- Stub: Phase 3 (Cloud / Workshop / Voice / Input) -------------------

TResult<void> SteamworksBridgeStub::CloudWriteFile(const char* path, const void* data, u32 size) noexcept {
    (void)path; (void)data; (void)size;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized, "CloudWriteFile before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented, "Stub: CloudWriteFile");
}

TResult<u32> SteamworksBridgeStub::CloudReadFile(const char* path, void* out_buf, u32 buf_size) noexcept {
    (void)path; (void)out_buf; (void)buf_size;
    if (!m_Initialized) {
        return TResult<u32>(ACS_ERR(Generic, kSubSteamworksNotInitialized, "CloudReadFile before Init()"));
    }
    return TResult<u32>(ACS_ERR(Generic, kSubSteamworksNotImplemented, "Stub: CloudReadFile"));
}

bool SteamworksBridgeStub::CloudFileExists(const char* path) const noexcept {
    (void)path;
    return false;
}

TResult<void> SteamworksBridgeStub::CloudDeleteFile(const char* path) noexcept {
    (void)path;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized, "CloudDeleteFile before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented, "Stub: CloudDeleteFile");
}

void SteamworksBridgeStub::CloudGetQuota(u64& out_available_bytes, u64& out_total_bytes) const noexcept {
    out_available_bytes = 0;
    out_total_bytes     = 0;
}

u32 SteamworksBridgeStub::WorkshopGetSubscribedCount() const noexcept {
    return 0;
}

void SteamworksBridgeStub::WorkshopGetSubscribedItem(u32 index, u64& out_item_id,
                                                     const char*& out_install_path) const noexcept {
    (void)index;
    out_item_id      = 0;
    out_install_path = nullptr;
}

TResult<void> SteamworksBridgeStub::VoiceStartRecording() noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized, "VoiceStartRecording before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented, "Stub: VoiceStartRecording");
}

TResult<void> SteamworksBridgeStub::VoiceStopRecording() noexcept {
    return Ok();  // 副作用無し
}

TResult<u32> SteamworksBridgeStub::VoiceGetCompressed(void* out_buf, u32 buf_size) noexcept {
    (void)out_buf; (void)buf_size;
    return TResult<u32>(OkInit, 0u);  // データなし
}

TResult<void> SteamworksBridgeStub::InputInit() noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubSteamworksNotInitialized, "InputInit before Init()");
    }
    return ACS_ERR(Generic, kSubSteamworksNotImplemented, "Stub: InputInit");
}

u32 SteamworksBridgeStub::InputGetControllerCount() const noexcept {
    return 0;
}

// ---- static singleton ---------------------------------------------------

SteamworksBridgeStub& SteamworksBridgeStub::GetStub() noexcept {
    // C++11 以降、関数スコープ static の初期化は thread-safe。
    static SteamworksBridgeStub m_Instance;
    return m_Instance;
}

} // namespace acs::game
