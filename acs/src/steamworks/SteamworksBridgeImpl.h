// SPDX-License-Identifier: Apache-2.0
// Steamworks SDK の real backend 実装 (Phase 26)。
// `acs::game::ISteamworksBridge` を Override し、SteamAPI_Init() 等の本物の
// SDK 関数を呼ぶ。ビルド有効化は CMake の ACS_BUILD_STEAMWORKS=ON。
//
// 利用方法:
//   #include "steamworks/SteamworksBridgeImpl.h"
//   acs::steamworks::FSteamworksBridgeImpl impl;
//   if (impl.Init().IsErr()) {
//       // Steam クライアントが起動していない / AppID 不一致 等
//       fallback_to_stub();
//   }
//   m_Social = &impl;
//
// 設計:
//   ・SteamAPI_Init() を Init() 内で呼び、Steam クライアント連携を確立。
//   ・GetLocalPlayer() は ISteamFriends::GetPersonaName() + SteamID64 を返す。
//   ・UnlockAchievement(name) は ISteamUserStats::SetAchievement() + StoreStats() を即時呼び出し。
//   ・SetLeaderboardScore は async (Find -> Upload の 2 段呼出)。同期 wait は
//     Tick() 経由でコールバックポンプを進めて完了を待つ (= 数フレーム遅延)。
//   ・GetLeaderboardScore も同様に async (Find -> Download)。
//   ・Tick(dt) で SteamAPI_RunCallbacks() を毎フレーム呼ぶ (必須)。
//
// Phase 2/3 (未実装、本ファイルで段階拡張予定):
//   - Stats (Set/Get)
//   - DLC owns (IsDlcOwned)
//   - RichPresence (SetRichPresence)
//   - Friend list (GetFriendCount, GetFriendByIndex)
//   - Cloud Save (FileWrite/FileRead via RemoteStorage)
//   - Workshop (UGC API: SubscribeItem, GetItemInstallInfo, etc.)
//   - Voice Chat (StartVoiceRecording, GetAvailableVoice, DecompressVoice)
//   - Steam Input (Action handles)
//
// 注意:
//   ・Steam クライアントが起動していないと SteamAPI_Init() は false を返す。
//   ・dev では steam_appid.txt = 480 (Spacewar) を実行 dir に置く。
//   ・Release では Steam Direct で取得した本番 AppID を steam_appid.txt に。
#pragma once

#include "foundation/Result.h"
#include "gameframework/SteamworksBridge.h"

namespace acs::steamworks {

// ---- subcode 定義 (gameframework/SteamworksBridge.h の SubSteamworks*  の続き) -
inline constexpr u16 kSubSteamworksInitFailed       = 1003;  // SteamAPI_Init 失敗
inline constexpr u16 kSubSteamworksFindBoardFailed  = 1004;  // FindLeaderboard 失敗
inline constexpr u16 kSubSteamworksUploadFailed     = 1005;  // UploadLeaderboardScore 失敗
inline constexpr u16 kSubSteamworksDownloadFailed   = 1006;  // DownloadLeaderboardEntries 失敗
inline constexpr u16 kSubSteamworksTimeout          = 1007;  // async API のタイムアウト

class FSteamworksBridgeImpl final : public acs::game::ISteamworksBridge {
public:
    FSteamworksBridgeImpl() noexcept;
    ~FSteamworksBridgeImpl() noexcept override;

    FSteamworksBridgeImpl(const FSteamworksBridgeImpl&)            = delete;
    FSteamworksBridgeImpl& operator=(const FSteamworksBridgeImpl&) = delete;
    FSteamworksBridgeImpl(FSteamworksBridgeImpl&&)                 = delete;
    FSteamworksBridgeImpl& operator=(FSteamworksBridgeImpl&&)      = delete;

    // ISteamworksBridge 実装 (gameframework/SteamworksBridge.h に対応)。
    acs::TResult<void>                Init()                          noexcept override;
    void                              Shutdown()                      noexcept override;
    bool                              IsInitialized()           const noexcept override;
    acs::game::PlayerIdentity         GetLocalPlayer()          const noexcept override;
    acs::TResult<void>                UnlockAchievement(const char* ach_id)               noexcept override;
    acs::TResult<void>                SetLeaderboardScore(const char* board_id, acs::i64 score) noexcept override;
    acs::TResult<acs::i64>            GetLeaderboardScore(const char* board_id)           noexcept override;
    void                              Tick(acs::f32 dt)               noexcept override;

    // Phase 2 (Stats / DLC / RichPresence / Friends)
    acs::TResult<void>                SetStat(const char* stat_name, acs::i64 value)      noexcept override;
    acs::TResult<acs::i64>            GetStat(const char* stat_name)                      noexcept override;
    acs::TResult<void>                SetFloatStat(const char* stat_name, acs::f32 value) noexcept override;
    acs::TResult<acs::f32>            GetFloatStat(const char* stat_name)                 noexcept override;
    bool                              IsDlcOwned(acs::u32 app_id)                   const noexcept override;
    acs::TResult<void>                SetRichPresence(const char* key, const char* value) noexcept override;
    acs::u32                          GetFriendCount()                              const noexcept override;
    acs::game::PlayerIdentity         GetFriendByIndex(acs::u32 index)              const noexcept override;

    // Phase 3 (Cloud / Workshop / Voice / Input)
    acs::TResult<void>                CloudWriteFile(const char* path, const void* data, acs::u32 size) noexcept override;
    acs::TResult<acs::u32>            CloudReadFile(const char* path, void* out_buf, acs::u32 buf_size) noexcept override;
    bool                              CloudFileExists(const char* path)             const noexcept override;
    acs::TResult<void>                CloudDeleteFile(const char* path)             noexcept override;
    void                              CloudGetQuota(acs::u64& out_available_bytes, acs::u64& out_total_bytes) const noexcept override;
    acs::u32                          WorkshopGetSubscribedCount()                  const noexcept override;
    void                              WorkshopGetSubscribedItem(acs::u32 index, acs::u64& out_item_id, const char*& out_install_path) const noexcept override;
    acs::TResult<void>                VoiceStartRecording()                         noexcept override;
    acs::TResult<void>                VoiceStopRecording()                          noexcept override;
    acs::TResult<acs::u32>            VoiceGetCompressed(void* out_buf, acs::u32 buf_size) noexcept override;
    acs::TResult<void>                InputInit()                                   noexcept override;
    acs::u32                          InputGetControllerCount()                     const noexcept override;

private:
    // Pimpl: SDK ヘッダ (steam_api.h) を public ヘッダから漏らさないため。
    // 実体は SteamworksBridgeImpl.cpp の anonymous namespace で定義。
    struct Impl;
    Impl* m_Impl = nullptr;
    bool  m_bInitialized = false;
};

// ---- 既定 Bridge provider 結線 (gameframework への登録点) -----------------
// gameframework は ACS::Steamworks に依存できない (循環依存) ため、結線は backend
// 側 (SteamworksDefault.cpp) から `acs::game::SetSteamworksBridgeProvider()` を呼んで
// 行う。アプリは起動時に一度 `acs::steamworks::InstallSteamworksAsDefault()` を呼ぶ
// だけで、以降は backend 非依存に `acs::game::GetDefaultSteamworksBridge()` で実
// Bridge を取得できる。

// プロセス共有 singleton の実 Bridge を返す。初回アクセス時に Init() を 1 回走らせる
// (Steam クライアント未起動等で失敗しても参照は返す — 呼び出し側が IsInitialized 等で判断)。
acs::game::ISteamworksBridge& GetDefaultSteamworksBridge() noexcept;

// 既定 Bridge provider を gameframework に登録する。起動時に一度だけ呼ぶ。
void InstallSteamworksAsDefault() noexcept;

} // namespace acs::steamworks
