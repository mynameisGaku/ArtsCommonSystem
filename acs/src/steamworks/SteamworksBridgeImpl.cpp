// SPDX-License-Identifier: Apache-2.0
// Steamworks SDK real backend 実装 (Phase 26)。
#include "steamworks/SteamworksBridgeImpl.h"

#include "foundation/Log.h"
#include "foundation/Error.h"

// Steamworks SDK ヘッダ。include path は acs_third_party::steamworks 経由
// (cmake/ACSThirdParty.cmake の target_include_directories でセット済)。
#include "steam/steam_api.h"

#include <cstring>   // strncmp

namespace acs::steamworks {

// ============================================================================
// Pimpl: SDK 固有の async callback handler / state を保持
// ============================================================================
namespace {

// 単一の leaderboard 操作の完了待ち state。async result が来たら m_bDone=true
// + m_Score を埋めて Tick() スレッドから取り出される。
struct FAsyncLeaderboardOp {
    SteamLeaderboard_t m_BoardHandle  = 0;
    bool               m_bUpload      = false;  // true=upload, false=download
    acs::i64           m_ScoreToUpload= 0;
    acs::i64           m_ResultScore  = 0;
    bool               m_bDone        = false;
    bool               m_bSuccess     = false;
};

} // namespace

// ============================================================================
// FSteamworksBridgeImpl::Impl — 内部 state + Steam callback handler
// ============================================================================
struct FSteamworksBridgeImpl::Impl {
    // PlayerIdentity の string 寿命を本クラスが保持 (GetLocalPlayer は
    // const char* を返すので Tick 毎に persona name を取得して保存)。
    char m_PersonaName[64]  = {};
    char m_SteamId64Str[32] = {};
    u64  m_SessionToken     = 0;

    // pending leaderboard ops (small fixed pool、async コールバックの追跡用)
    static constexpr u32 kMaxPendingOps = 16;
    FAsyncLeaderboardOp m_PendingOps[kMaxPendingOps] = {};
    u32                 m_NumPending                  = 0;

    // ---- Steam callback (callback registration マクロは SteamworksAPI 流) ----
    // FindLeaderboard 完了通知
    void OnLeaderboardFindResult(LeaderboardFindResult_t* result, bool bIOFailure) noexcept {
        if (bIOFailure || result->m_bLeaderboardFound == 0) {
            // どの pending op か特定できないので、未完了 op で board_handle=0 のものを
            // 順次 fail とする (簡略化、production ならコールバック call result handle で個別追跡)
            for (u32 i = 0; i < m_NumPending; ++i) {
                if (!m_PendingOps[i].m_bDone && m_PendingOps[i].m_BoardHandle == 0) {
                    m_PendingOps[i].m_bDone    = true;
                    m_PendingOps[i].m_bSuccess = false;
                    return;
                }
            }
            return;
        }
        // 成功: board_handle を保存、対応する pending op の Upload/Download を起動
        for (u32 i = 0; i < m_NumPending; ++i) {
            if (!m_PendingOps[i].m_bDone && m_PendingOps[i].m_BoardHandle == 0) {
                m_PendingOps[i].m_BoardHandle = result->m_hSteamLeaderboard;
                if (m_PendingOps[i].m_bUpload) {
                    SteamUserStats()->UploadLeaderboardScore(
                        m_PendingOps[i].m_BoardHandle,
                        k_ELeaderboardUploadScoreMethodKeepBest,
                        static_cast<int32>(m_PendingOps[i].m_ScoreToUpload),
                        nullptr, 0);
                } else {
                    SteamUserStats()->DownloadLeaderboardEntries(
                        m_PendingOps[i].m_BoardHandle,
                        k_ELeaderboardDataRequestGlobalAroundUser,
                        0, 0);
                }
                return;
            }
        }
    }
    CCallResult<Impl, LeaderboardFindResult_t> m_FindResult;

    void OnLeaderboardScoreUploaded(LeaderboardScoreUploaded_t* result, bool bIOFailure) noexcept {
        for (u32 i = 0; i < m_NumPending; ++i) {
            if (!m_PendingOps[i].m_bDone && m_PendingOps[i].m_BoardHandle == result->m_hSteamLeaderboard
                && m_PendingOps[i].m_bUpload) {
                m_PendingOps[i].m_bDone    = true;
                m_PendingOps[i].m_bSuccess = (!bIOFailure && result->m_bSuccess);
                return;
            }
        }
    }
    CCallResult<Impl, LeaderboardScoreUploaded_t> m_UploadResult;

    void OnLeaderboardScoresDownloaded(LeaderboardScoresDownloaded_t* result, bool bIOFailure) noexcept {
        for (u32 i = 0; i < m_NumPending; ++i) {
            if (!m_PendingOps[i].m_bDone && m_PendingOps[i].m_BoardHandle == result->m_hSteamLeaderboard
                && !m_PendingOps[i].m_bUpload) {
                m_PendingOps[i].m_bDone    = true;
                m_PendingOps[i].m_bSuccess = !bIOFailure;
                if (result->m_cEntryCount > 0) {
                    LeaderboardEntry_t entry{};
                    SteamUserStats()->GetDownloadedLeaderboardEntry(
                        result->m_hSteamLeaderboardEntries, 0, &entry, nullptr, 0);
                    m_PendingOps[i].m_ResultScore = entry.m_nScore;
                }
                return;
            }
        }
    }
    CCallResult<Impl, LeaderboardScoresDownloaded_t> m_DownloadResult;
};

// ============================================================================
// 公開 API 実装
// ============================================================================

FSteamworksBridgeImpl::FSteamworksBridgeImpl() noexcept {
    m_Impl = new Impl();
}

FSteamworksBridgeImpl::~FSteamworksBridgeImpl() noexcept {
    if (m_bInitialized) {
        Shutdown();
    }
    delete m_Impl;
    m_Impl = nullptr;
}

acs::TResult<void> FSteamworksBridgeImpl::Init() noexcept {
    if (m_bInitialized) return acs::OkInit;
    if (!SteamAPI_Init()) {
        // 代表的な失敗理由:
        //   1) Steam クライアントが起動していない
        //   2) steam_appid.txt が見つからない / AppID 不一致
        //   3) SDK バージョンと Steam クライアントが不整合
        ACS_LOG_ERROR("SteamAPI_Init failed (Steam クライアント起動 / steam_appid.txt 確認)");
        return ACS_ERR(EErrCategory::Generic, kSubSteamworksInitFailed,
                       "SteamAPI_Init returned false");
    }
    m_bInitialized = true;
    ACS_LOG_INFO("SteamAPI_Init success");

    // persona name + steam id を初回 cache (Tick で更新される)
    Tick(0.0f);

    return acs::OkInit;
}

void FSteamworksBridgeImpl::Shutdown() noexcept {
    if (!m_bInitialized) return;
    SteamAPI_Shutdown();
    m_bInitialized = false;
    ACS_LOG_INFO("SteamAPI_Shutdown");
}

bool FSteamworksBridgeImpl::IsInitialized() const noexcept {
    return m_bInitialized;
}

acs::game::PlayerIdentity FSteamworksBridgeImpl::GetLocalPlayer() const noexcept {
    acs::game::PlayerIdentity id{};
    if (!m_bInitialized) return id;
    id.platform_id   = m_Impl->m_SteamId64Str;
    id.display_name  = m_Impl->m_PersonaName;
    id.session_token = m_Impl->m_SessionToken;
    return id;
}

acs::TResult<void> FSteamworksBridgeImpl::UnlockAchievement(const char* ach_id) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(EErrCategory::Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before UnlockAchievement");
    }
    if (ach_id == nullptr || ach_id[0] == 0) {
        return ACS_ERR(EErrCategory::Generic, 0, "ach_id is null or empty");
    }
    ISteamUserStats* stats = SteamUserStats();
    if (!stats) {
        return ACS_ERR(EErrCategory::Generic, kSubSteamworksInitFailed, "SteamUserStats() null");
    }
    stats->SetAchievement(ach_id);
    stats->StoreStats();
    return acs::OkInit;
}

acs::TResult<void> FSteamworksBridgeImpl::SetLeaderboardScore(const char* board_id, acs::i64 score) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(EErrCategory::Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before SetLeaderboardScore");
    }
    if (board_id == nullptr || board_id[0] == 0) {
        return ACS_ERR(EErrCategory::Generic, 0, "board_id is null or empty");
    }
    // pool に空き op を確保 (Pimpl 内 fixed pool)
    if (m_Impl->m_NumPending >= Impl::kMaxPendingOps) {
        return ACS_ERR(EErrCategory::Generic, kSubSteamworksUploadFailed,
                       "too many pending leaderboard ops");
    }
    FAsyncLeaderboardOp& op = m_Impl->m_PendingOps[m_Impl->m_NumPending++];
    op = FAsyncLeaderboardOp{};
    op.m_bUpload        = true;
    op.m_ScoreToUpload  = score;
    op.m_bDone          = false;
    // FindLeaderboard kick off (async、result は OnLeaderboardFindResult で受ける)
    SteamAPICall_t hCall = SteamUserStats()->FindLeaderboard(board_id);
    m_Impl->m_FindResult.Set(hCall, m_Impl, &Impl::OnLeaderboardFindResult);
    return acs::OkInit;
}

acs::TResult<acs::i64> FSteamworksBridgeImpl::GetLeaderboardScore(const char* board_id) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(EErrCategory::Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before GetLeaderboardScore");
    }
    if (board_id == nullptr || board_id[0] == 0) {
        return ACS_ERR(EErrCategory::Generic, 0, "board_id is null or empty");
    }
    if (m_Impl->m_NumPending >= Impl::kMaxPendingOps) {
        return ACS_ERR(EErrCategory::Generic, kSubSteamworksDownloadFailed,
                       "too many pending leaderboard ops");
    }
    FAsyncLeaderboardOp& op = m_Impl->m_PendingOps[m_Impl->m_NumPending++];
    op = FAsyncLeaderboardOp{};
    op.m_bUpload = false;
    op.m_bDone   = false;
    SteamAPICall_t hCall = SteamUserStats()->FindLeaderboard(board_id);
    m_Impl->m_FindResult.Set(hCall, m_Impl, &Impl::OnLeaderboardFindResult);
    // 同期 wait はせず、Phase 1 では「最新の完了済み op の score」を即返す。
    // production ならコールバック完了を future で返す API にする (Phase 2 拡張)。
    for (u32 i = 0; i < m_Impl->m_NumPending; ++i) {
        if (m_Impl->m_PendingOps[i].m_bDone && m_Impl->m_PendingOps[i].m_bSuccess
            && !m_Impl->m_PendingOps[i].m_bUpload) {
            return acs::TResult<acs::i64>(acs::OkInit, m_Impl->m_PendingOps[i].m_ResultScore);
        }
    }
    return acs::TResult<acs::i64>(acs::OkInit, 0);  // 未完了時は 0 を返す
}

// ============================================================================
// Phase 2 (Stats / DLC / RichPresence / Friends)
// ============================================================================

acs::TResult<void> FSteamworksBridgeImpl::SetStat(const char* stat_name, acs::i64 value) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(EErrCategory::Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before SetStat");
    }
    if (stat_name == nullptr || stat_name[0] == 0) {
        return ACS_ERR(EErrCategory::Generic, 0, "stat_name null");
    }
    ISteamUserStats* stats = SteamUserStats();
    if (!stats) {
        return ACS_ERR(EErrCategory::Generic, kSubSteamworksInitFailed, "SteamUserStats() null");
    }
    // Steamworks API は int32、本ヘッダの方が広い (i64) ので clamp
    int32 v = (value > 0x7fffffffLL)  ? 0x7fffffff
            : (value < -0x80000000LL) ? static_cast<int32>(-0x80000000LL)
                                       : static_cast<int32>(value);
    stats->SetStat(stat_name, v);
    stats->StoreStats();
    return acs::OkInit;
}

acs::TResult<acs::i64> FSteamworksBridgeImpl::GetStat(const char* stat_name) noexcept {
    if (!m_bInitialized) {
        return acs::TResult<acs::i64>(ACS_ERR(EErrCategory::Generic,
                                              acs::game::kSubSteamworksNotInitialized,
                                              "Init() before GetStat"));
    }
    if (stat_name == nullptr || stat_name[0] == 0) {
        return acs::TResult<acs::i64>(ACS_ERR(EErrCategory::Generic, 0, "stat_name null"));
    }
    ISteamUserStats* stats = SteamUserStats();
    if (!stats) {
        return acs::TResult<acs::i64>(ACS_ERR(EErrCategory::Generic,
                                              kSubSteamworksInitFailed,
                                              "SteamUserStats() null"));
    }
    int32 v = 0;
    stats->GetStat(stat_name, &v);
    return acs::TResult<acs::i64>(acs::OkInit, static_cast<acs::i64>(v));
}

acs::TResult<void> FSteamworksBridgeImpl::SetFloatStat(const char* stat_name, acs::f32 value) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(EErrCategory::Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before SetFloatStat");
    }
    if (stat_name == nullptr || stat_name[0] == 0) {
        return ACS_ERR(EErrCategory::Generic, 0, "stat_name null");
    }
    ISteamUserStats* stats = SteamUserStats();
    if (!stats) {
        return ACS_ERR(EErrCategory::Generic, kSubSteamworksInitFailed, "SteamUserStats() null");
    }
    stats->SetStat(stat_name, value);  // overload for float
    stats->StoreStats();
    return acs::OkInit;
}

acs::TResult<acs::f32> FSteamworksBridgeImpl::GetFloatStat(const char* stat_name) noexcept {
    if (!m_bInitialized) {
        return acs::TResult<acs::f32>(ACS_ERR(EErrCategory::Generic,
                                              acs::game::kSubSteamworksNotInitialized,
                                              "Init() before GetFloatStat"));
    }
    if (stat_name == nullptr || stat_name[0] == 0) {
        return acs::TResult<acs::f32>(ACS_ERR(EErrCategory::Generic, 0, "stat_name null"));
    }
    ISteamUserStats* stats = SteamUserStats();
    if (!stats) {
        return acs::TResult<acs::f32>(ACS_ERR(EErrCategory::Generic,
                                              kSubSteamworksInitFailed,
                                              "SteamUserStats() null"));
    }
    float v = 0.0f;
    stats->GetStat(stat_name, &v);
    return acs::TResult<acs::f32>(acs::OkInit, v);
}

bool FSteamworksBridgeImpl::IsDlcOwned(acs::u32 app_id) const noexcept {
    if (!m_bInitialized) return false;
    ISteamApps* apps = SteamApps();
    if (!apps) return false;
    return apps->BIsDlcInstalled(static_cast<AppId_t>(app_id));
}

acs::TResult<void> FSteamworksBridgeImpl::SetRichPresence(const char* key, const char* value) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(EErrCategory::Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before SetRichPresence");
    }
    if (key == nullptr || key[0] == 0) {
        return ACS_ERR(EErrCategory::Generic, 0, "key null");
    }
    ISteamFriends* friends = SteamFriends();
    if (!friends) {
        return ACS_ERR(EErrCategory::Generic, kSubSteamworksInitFailed, "SteamFriends() null");
    }
    bool ok = friends->SetRichPresence(key, value);
    if (!ok) {
        return ACS_ERR(EErrCategory::Generic, 0, "SetRichPresence returned false");
    }
    return acs::OkInit;
}

acs::u32 FSteamworksBridgeImpl::GetFriendCount() const noexcept {
    if (!m_bInitialized) return 0;
    ISteamFriends* friends = SteamFriends();
    if (!friends) return 0;
    int n = friends->GetFriendCount(k_EFriendFlagImmediate);
    return n > 0 ? static_cast<acs::u32>(n) : 0;
}

acs::game::PlayerIdentity FSteamworksBridgeImpl::GetFriendByIndex(acs::u32 index) const noexcept {
    acs::game::PlayerIdentity id{};
    if (!m_bInitialized) return id;
    ISteamFriends* friends = SteamFriends();
    if (!friends) return id;
    int n = friends->GetFriendCount(k_EFriendFlagImmediate);
    if (n <= 0 || static_cast<int>(index) >= n) return id;
    CSteamID friend_id = friends->GetFriendByIndex(static_cast<int>(index),
                                                   k_EFriendFlagImmediate);
    // friend の persona name / SteamID64 を返す (寿命: 次の Tick まで保証)
    // friend ごとに別 buffer が要るので、本 impl では「最後にクエリされた 1 名のみ」
    // を m_Impl->m_PersonaName に上書き保存する形にする。production では
    // friend index ごとに別 buffer (= fixed pool) を用意するのが望ましい。
    const char* name = friends->GetFriendPersonaName(friend_id);
    static thread_local char s_FriendName[64] = {};
    static thread_local char s_FriendIdStr[32] = {};
    if (name) {
        std::strncpy(s_FriendName, name, sizeof(s_FriendName) - 1);
        s_FriendName[sizeof(s_FriendName) - 1] = 0;
    } else {
        s_FriendName[0] = 0;
    }
    std::snprintf(s_FriendIdStr, sizeof(s_FriendIdStr), "%llu",
                  static_cast<unsigned long long>(friend_id.ConvertToUint64()));
    id.platform_id   = s_FriendIdStr;
    id.display_name  = s_FriendName;
    id.session_token = friend_id.ConvertToUint64();
    return id;
}

// ============================================================================
// Phase 3 (Cloud / Workshop / Voice / Input)
// ============================================================================

acs::TResult<void> FSteamworksBridgeImpl::CloudWriteFile(const char* path, const void* data, acs::u32 size) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(EErrCategory::Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before CloudWriteFile");
    }
    ISteamRemoteStorage* rs = SteamRemoteStorage();
    if (!rs) {
        return ACS_ERR(EErrCategory::Generic, kSubSteamworksInitFailed, "SteamRemoteStorage() null");
    }
    bool ok = rs->FileWrite(path, data, static_cast<int32>(size));
    if (!ok) return ACS_ERR(EErrCategory::Generic, 0, "FileWrite failed (quota / Cloud disabled?)");
    return acs::OkInit;
}

acs::TResult<acs::u32> FSteamworksBridgeImpl::CloudReadFile(const char* path, void* out_buf, acs::u32 buf_size) noexcept {
    if (!m_bInitialized) {
        return acs::TResult<acs::u32>(ACS_ERR(EErrCategory::Generic,
                                              acs::game::kSubSteamworksNotInitialized,
                                              "Init() before CloudReadFile"));
    }
    ISteamRemoteStorage* rs = SteamRemoteStorage();
    if (!rs) {
        return acs::TResult<acs::u32>(ACS_ERR(EErrCategory::Generic,
                                              kSubSteamworksInitFailed,
                                              "SteamRemoteStorage() null"));
    }
    int32 actual = rs->FileRead(path, out_buf, static_cast<int32>(buf_size));
    return acs::TResult<acs::u32>(acs::OkInit, actual > 0 ? static_cast<acs::u32>(actual) : 0u);
}

bool FSteamworksBridgeImpl::CloudFileExists(const char* path) const noexcept {
    if (!m_bInitialized) return false;
    ISteamRemoteStorage* rs = SteamRemoteStorage();
    if (!rs) return false;
    return rs->FileExists(path);
}

acs::TResult<void> FSteamworksBridgeImpl::CloudDeleteFile(const char* path) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(EErrCategory::Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before CloudDeleteFile");
    }
    ISteamRemoteStorage* rs = SteamRemoteStorage();
    if (!rs) {
        return ACS_ERR(EErrCategory::Generic, kSubSteamworksInitFailed, "SteamRemoteStorage() null");
    }
    rs->FileDelete(path);  // 戻り値 bool だが、存在しない path も成功扱い
    return acs::OkInit;
}

void FSteamworksBridgeImpl::CloudGetQuota(acs::u64& out_available_bytes, acs::u64& out_total_bytes) const noexcept {
    out_available_bytes = 0;
    out_total_bytes = 0;
    if (!m_bInitialized) return;
    ISteamRemoteStorage* rs = SteamRemoteStorage();
    if (!rs) return;
    uint64 total = 0;
    uint64 available = 0;
    rs->GetQuota(&total, &available);
    out_available_bytes = available;
    out_total_bytes     = total;
}

acs::u32 FSteamworksBridgeImpl::WorkshopGetSubscribedCount() const noexcept {
    if (!m_bInitialized) return 0;
    ISteamUGC* ugc = SteamUGC();
    if (!ugc) return 0;
    return ugc->GetNumSubscribedItems();
}

void FSteamworksBridgeImpl::WorkshopGetSubscribedItem(acs::u32 index, acs::u64& out_item_id,
                                                      const char*& out_install_path) const noexcept {
    out_item_id      = 0;
    out_install_path = nullptr;
    if (!m_bInitialized) return;
    ISteamUGC* ugc = SteamUGC();
    if (!ugc) return;
    uint32 count = ugc->GetNumSubscribedItems();
    if (index >= count) return;
    // 全部取って index 番目 (SDK API には index 取得が無いので一括取得)
    static thread_local PublishedFileId_t s_Ids[256] = {};
    uint32 actual = ugc->GetSubscribedItems(s_Ids, count > 256 ? 256 : count);
    if (index >= actual) return;
    out_item_id = static_cast<acs::u64>(s_Ids[index]);

    // install path
    static thread_local char s_InstallPath[1024] = {};
    uint64 sizeOnDisk = 0;
    uint32 timestamp = 0;
    bool ok = ugc->GetItemInstallInfo(s_Ids[index], &sizeOnDisk,
                                      s_InstallPath, sizeof(s_InstallPath), &timestamp);
    out_install_path = ok ? s_InstallPath : nullptr;
}

acs::TResult<void> FSteamworksBridgeImpl::VoiceStartRecording() noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(EErrCategory::Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before VoiceStartRecording");
    }
    ISteamUser* user = SteamUser();
    if (!user) {
        return ACS_ERR(EErrCategory::Generic, kSubSteamworksInitFailed, "SteamUser() null");
    }
    user->StartVoiceRecording();
    return acs::OkInit;
}

acs::TResult<void> FSteamworksBridgeImpl::VoiceStopRecording() noexcept {
    if (!m_bInitialized) return acs::OkInit;
    ISteamUser* user = SteamUser();
    if (user) user->StopVoiceRecording();
    return acs::OkInit;
}

acs::TResult<acs::u32> FSteamworksBridgeImpl::VoiceGetCompressed(void* out_buf, acs::u32 buf_size) noexcept {
    if (!m_bInitialized) {
        return acs::TResult<acs::u32>(acs::OkInit, 0u);
    }
    ISteamUser* user = SteamUser();
    if (!user) {
        return acs::TResult<acs::u32>(ACS_ERR(EErrCategory::Generic,
                                              kSubSteamworksInitFailed,
                                              "SteamUser() null"));
    }
    uint32 written = 0;
    EVoiceResult r = user->GetVoice(true, out_buf, buf_size, &written,
                                    false, nullptr, 0, nullptr, 0);
    if (r == k_EVoiceResultOK || r == k_EVoiceResultNoData) {
        return acs::TResult<acs::u32>(acs::OkInit, written);
    }
    return acs::TResult<acs::u32>(ACS_ERR(EErrCategory::Generic, 0, "GetVoice error"));
}

acs::TResult<void> FSteamworksBridgeImpl::InputInit() noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(EErrCategory::Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before InputInit");
    }
    ISteamInput* input = SteamInput();
    if (!input) {
        return ACS_ERR(EErrCategory::Generic, kSubSteamworksInitFailed, "SteamInput() null");
    }
    bool ok = input->Init(false);  // false = no explicit standalone (Steam runtime active)
    if (!ok) return ACS_ERR(EErrCategory::Generic, 0, "SteamInput::Init failed");
    return acs::OkInit;
}

acs::u32 FSteamworksBridgeImpl::InputGetControllerCount() const noexcept {
    if (!m_bInitialized) return 0;
    ISteamInput* input = SteamInput();
    if (!input) return 0;
    InputHandle_t handles[STEAM_INPUT_MAX_COUNT] = {};
    int n = input->GetConnectedControllers(handles);
    return n > 0 ? static_cast<acs::u32>(n) : 0;
}

void FSteamworksBridgeImpl::Tick(acs::f32 /*dt*/) noexcept {
    if (!m_bInitialized) return;
    SteamAPI_RunCallbacks();

    // persona name / steam id を毎フレーム refresh (= login 状態変化に追従)
    ISteamUser* user = SteamUser();
    if (user) {
        CSteamID id = user->GetSteamID();
        uint64 id64 = id.ConvertToUint64();
        std::snprintf(m_Impl->m_SteamId64Str, sizeof(m_Impl->m_SteamId64Str),
                      "%llu", static_cast<unsigned long long>(id64));
        m_Impl->m_SessionToken = id64;
    }
    ISteamFriends* friends = SteamFriends();
    if (friends) {
        const char* name = friends->GetPersonaName();
        if (name) {
            std::strncpy(m_Impl->m_PersonaName, name, sizeof(m_Impl->m_PersonaName) - 1);
            m_Impl->m_PersonaName[sizeof(m_Impl->m_PersonaName) - 1] = 0;
        }
    }

    // 完了 op を pool から compact 削除 (= 古い op が pool を埋めないように)
    u32 write = 0;
    for (u32 read = 0; read < m_Impl->m_NumPending; ++read) {
        if (!m_Impl->m_PendingOps[read].m_bDone) {
            if (write != read) {
                m_Impl->m_PendingOps[write] = m_Impl->m_PendingOps[read];
            }
            ++write;
        }
    }
    m_Impl->m_NumPending = write;
}

} // namespace acs::steamworks
