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
