// SPDX-License-Identifier: Apache-2.0
// Steamworks SDK real backend 実装。
#include "steamworks/SteamworksBridgeImpl.h"

#include "foundation/Log.h"
#include "foundation/Error.h"

// Steamworks SDK ヘッダ。include path は acs_third_party::steamworks 経由
// (cmake/ACSThirdParty.cmake の target_include_directories でセット済)。
#include "steam/steam_api.h"

#include <cstring>   // strncmp
#include <cstdio>    // snprintf

namespace acs::steamworks {

namespace {

/**
 * 単一の leaderboard 操作 (upload/download) の完了待ち state。
 *
 * @details
 * async result が到着すると m_bDone=true とスコアが埋められ、Tick() 経由で取り出される。
 * m_BoardId を相関キーとして保持することで、結果がどの board のものかを識別する
 * (これが無いと GetLeaderboardScore が無関係な board のスコアを成功として返してしまう)。
 */
struct FAsyncLeaderboardOp {
    /** 要求時の board_id (結果相関キー。FindLeaderboard で解決する handle とは別物)。 */
    char               m_BoardId[64]  = {};

    /** FindLeaderboard 完了後に解決される Steam 側 leaderboard handle。 */
    SteamLeaderboard_t m_BoardHandle  = 0;

    /** true なら upload 操作、false なら download 操作。 */
    bool               m_bUpload      = false;

    /** upload 時に送信するスコア。 */
    acs::i64           m_ScoreToUpload= 0;

    /** download 完了時に格納される取得スコア。 */
    acs::i64           m_ResultScore  = 0;

    /** 操作が完了したかを示すフラグ。 */
    bool               m_bDone        = false;

    /** 操作が成功したかを示すフラグ。 */
    bool               m_bSuccess     = false;
};

} // namespace

/**
 * FSteamworksBridgeImpl の内部 state と Steam async コールバックハンドラ。
 *
 * @details
 * PlayerIdentity が返す const char* の寿命を保持する文字列バッファと、leaderboard
 * の async 操作を追跡する固定プールを持つ。各コールバックは CCallResult 経由で
 * Steam から呼ばれ、対応する pending op を完了状態に遷移させる。
 */
struct FSteamworksBridgeImpl::Impl {
    /** persona name の保持バッファ (GetLocalPlayer が返す寿命を確保、Tick で更新)。 */
    char m_PersonaName[64]  = {};

    /** SteamID64 文字列の保持バッファ (GetLocalPlayer が返す寿命を確保、Tick で更新)。 */
    char m_SteamId64Str[32] = {};

    /** 現在の SteamID64 をそのまま入れたセッショントークン。 */
    u64  m_SessionToken     = 0;

    /** pending leaderboard op の固定プール上限。 */
    static constexpr u32 kMaxPendingOps = 16;

    /** async leaderboard 操作を追跡する固定プール。 */
    FAsyncLeaderboardOp m_PendingOps[kMaxPendingOps] = {};

    /** プールで使用中の pending op 数。 */
    u32                 m_NumPending                  = 0;

    /**
     * FindLeaderboard 完了コールバック。board handle を保存し upload/download を起動する。
     *
     * @details
     * IO 失敗 / board 未発見なら、handle 未解決の未完了 op を順に fail にする。成功時は
     * 解決した handle を未完了 op に格納し、m_bUpload に応じて UploadLeaderboardScore
     * または DownloadLeaderboardEntries を起動する。
     * @param result FindLeaderboard の結果 (解決した leaderboard handle を含む)。
     * @param bIOFailure 通信失敗なら true。
     */
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
    /** FindLeaderboard の async 結果を OnLeaderboardFindResult に結び付ける call result。 */
    CCallResult<Impl, LeaderboardFindResult_t> m_FindResult;

    /**
     * UploadLeaderboardScore 完了コールバック。対応する upload op を完了にする。
     *
     * @param result upload の結果 (対象 leaderboard handle と成否を含む)。
     * @param bIOFailure 通信失敗なら true。
     */
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
    /** UploadLeaderboardScore の async 結果を OnLeaderboardScoreUploaded に結び付ける call result。 */
    CCallResult<Impl, LeaderboardScoreUploaded_t> m_UploadResult;

    /**
     * DownloadLeaderboardEntries 完了コールバック。download op を完了にしてスコアを格納する。
     *
     * @details エントリが 1 件以上あれば先頭エントリのスコアを m_ResultScore に格納する。
     * @param result download の結果 (対象 leaderboard handle とエントリ列を含む)。
     * @param bIOFailure 通信失敗なら true。
     */
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
    /** DownloadLeaderboardEntries の async 結果を OnLeaderboardScoresDownloaded に結び付ける call result。 */
    CCallResult<Impl, LeaderboardScoresDownloaded_t> m_DownloadResult;
};

/** Pimpl 状態を確保する。 */
FSteamworksBridgeImpl::FSteamworksBridgeImpl() noexcept {
    m_Impl = new Impl();
}

/** 必要なら Shutdown し、Pimpl 状態を解放する。 */
FSteamworksBridgeImpl::~FSteamworksBridgeImpl() noexcept {
    if (m_bInitialized) {
        Shutdown();
    }
    delete m_Impl;
    m_Impl = nullptr;
}

/** SteamAPI_Init() を呼んで連携を確立し、persona/SteamID を初回 cache する。 */
acs::TResult<void> FSteamworksBridgeImpl::Init() noexcept {
    if (m_bInitialized) return acs::OkInit;
    if (!SteamAPI_Init()) {
        // 代表的な失敗理由:
        //   1) Steam クライアントが起動していない
        //   2) steam_appid.txt が見つからない / AppID 不一致
        //   3) SDK バージョンと Steam クライアントが不整合
        ACS_LOG_ERROR("SteamAPI_Init failed (Steam クライアント起動 / steam_appid.txt 確認)");
        return ACS_ERR(Generic, kSubSteamworksInitFailed,
                       "SteamAPI_Init returned false");
    }
    m_bInitialized = true;
    ACS_LOG_INFO("SteamAPI_Init success");

    // persona name + steam id を初回 cache (Tick で更新される)
    Tick(0.0f);

    return acs::OkInit;
}

/** SteamAPI_Shutdown() を呼んで連携を終了する。 */
void FSteamworksBridgeImpl::Shutdown() noexcept {
    if (!m_bInitialized) return;
    SteamAPI_Shutdown();
    m_bInitialized = false;
    ACS_LOG_INFO("SteamAPI_Shutdown");
}

/** Steam 連携が確立済みかを返す。 */
bool FSteamworksBridgeImpl::IsInitialized() const noexcept {
    return m_bInitialized;
}

/** persona name + SteamID64 + session token を詰めた PlayerIdentity を返す。 */
acs::game::PlayerIdentity FSteamworksBridgeImpl::GetLocalPlayer() const noexcept {
    acs::game::PlayerIdentity id{};
    if (!m_bInitialized) return id;
    id.platform_id   = m_Impl->m_SteamId64Str;
    id.display_name  = m_Impl->m_PersonaName;
    id.session_token = m_Impl->m_SessionToken;
    return id;
}

/** SetAchievement + StoreStats を即時実行して実績を解除する。 */
acs::TResult<void> FSteamworksBridgeImpl::UnlockAchievement(const char* ach_id) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before UnlockAchievement");
    }
    if (ach_id == nullptr || ach_id[0] == 0) {
        return ACS_ERR(Generic, 0, "ach_id is null or empty");
    }
    ISteamUserStats* stats = SteamUserStats();
    if (!stats) {
        return ACS_ERR(Generic, kSubSteamworksInitFailed, "SteamUserStats() null");
    }
    stats->SetAchievement(ach_id);
    stats->StoreStats();
    return acs::OkInit;
}

/** FindLeaderboard を kick off し、pending upload op をプールに積む (async)。 */
acs::TResult<void> FSteamworksBridgeImpl::SetLeaderboardScore(const char* board_id, acs::i64 score) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before SetLeaderboardScore");
    }
    if (board_id == nullptr || board_id[0] == 0) {
        return ACS_ERR(Generic, 0, "board_id is null or empty");
    }
    // pool に空き op を確保 (Pimpl 内 fixed pool)
    if (m_Impl->m_NumPending >= Impl::kMaxPendingOps) {
        return ACS_ERR(Generic, kSubSteamworksUploadFailed,
                       "too many pending leaderboard ops");
    }
    FAsyncLeaderboardOp& op = m_Impl->m_PendingOps[m_Impl->m_NumPending++];
    op = FAsyncLeaderboardOp{};
    op.m_bUpload        = true;
    op.m_ScoreToUpload  = score;
    op.m_bDone          = false;
    // 要求 board_id を記録 (結果相関用)。
    std::strncpy(op.m_BoardId, board_id, sizeof(op.m_BoardId) - 1);
    op.m_BoardId[sizeof(op.m_BoardId) - 1] = 0;
    // FindLeaderboard kick off (async、result は OnLeaderboardFindResult で受ける)
    SteamAPICall_t hCall = SteamUserStats()->FindLeaderboard(board_id);
    m_Impl->m_FindResult.Set(hCall, m_Impl, &Impl::OnLeaderboardFindResult);
    return acs::OkInit;
}

/** 同一 board の完了 download op があれば返し、無ければ Find->Download を kick off する (async)。 */
acs::TResult<acs::i64> FSteamworksBridgeImpl::GetLeaderboardScore(const char* board_id) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before GetLeaderboardScore");
    }
    if (board_id == nullptr || board_id[0] == 0) {
        return ACS_ERR(Generic, 0, "board_id is null or empty");
    }
    // この board_id について既に download op が完了していれば、その結果を相関を
    // 取った上で返す。古い未消化 op を再利用しないよう、download op に限定して
    // 「同じ board_id」の完了 op を探す (新規 op を積む前にチェック)。
    //   ・完了 & 成功      → そのスコアを返す
    //   ・完了 & 失敗      → DownloadFailed エラー (誤った成功を返さない)
    //   ・pending (未完了) → 後段で kick off 済みなので Timeout/Pending エラー
    // バグ修正前は「どの board でも最初に見つかった成功 download op の score」を
    // OkInit として返しており、要求 board と無関係/古い board のスコアを成功扱い
    // していた。board_id を相関キーにすることで誤った成功を排除する。
    for (u32 i = 0; i < m_Impl->m_NumPending; ++i) {
        FAsyncLeaderboardOp& prev = m_Impl->m_PendingOps[i];
        if (prev.m_bUpload) continue;
        if (std::strncmp(prev.m_BoardId, board_id, sizeof(prev.m_BoardId)) != 0) continue;
        if (!prev.m_bDone) {
            // 同じ board の download が進行中。まだ結果が無いので成功を偽装しない。
            return acs::TResult<acs::i64>(
                ACS_ERR(Generic, kSubSteamworksTimeout,
                        "GetLeaderboardScore: download still pending (call Tick to pump)"));
        }
        if (!prev.m_bSuccess) {
            // 完了したが失敗。0 成功ではなく明示エラーを返す。
            return acs::TResult<acs::i64>(
                ACS_ERR(Generic, kSubSteamworksDownloadFailed,
                        "GetLeaderboardScore: download failed for requested board"));
        }
        // 完了 & 成功: 要求 board と相関の取れたスコアを返す。
        return acs::TResult<acs::i64>(acs::OkInit, prev.m_ResultScore);
    }

    // この board の download op がまだ存在しない場合のみ新規に kick off する。
    if (m_Impl->m_NumPending >= Impl::kMaxPendingOps) {
        return acs::TResult<acs::i64>(
            ACS_ERR(Generic, kSubSteamworksDownloadFailed,
                    "too many pending leaderboard ops"));
    }
    FAsyncLeaderboardOp& op = m_Impl->m_PendingOps[m_Impl->m_NumPending++];
    op = FAsyncLeaderboardOp{};
    op.m_bUpload = false;
    op.m_bDone   = false;
    // 要求 board_id を記録 (結果相関用)。
    std::strncpy(op.m_BoardId, board_id, sizeof(op.m_BoardId) - 1);
    op.m_BoardId[sizeof(op.m_BoardId) - 1] = 0;
    SteamAPICall_t hCall = SteamUserStats()->FindLeaderboard(board_id);
    m_Impl->m_FindResult.Set(hCall, m_Impl, &Impl::OnLeaderboardFindResult);
    // 同期 wait はしない (async)。Find→Download 完了後に再度本関数を呼べば、上の
    // 相関ループが完了 op を拾って正しいスコアを返す。初回呼び出しでは結果が無い
    // ため、誤った成功 (旧実装の OkInit, 0) ではなく Timeout/Pending を返す。
    return acs::TResult<acs::i64>(
        ACS_ERR(Generic, kSubSteamworksTimeout,
                "GetLeaderboardScore: request issued, result pending (call Tick then retry)"));
}

/** i64 値を int32 へ clamp して SetStat + StoreStats する。 */
acs::TResult<void> FSteamworksBridgeImpl::SetStat(const char* stat_name, acs::i64 value) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before SetStat");
    }
    if (stat_name == nullptr || stat_name[0] == 0) {
        return ACS_ERR(Generic, 0, "stat_name null");
    }
    ISteamUserStats* stats = SteamUserStats();
    if (!stats) {
        return ACS_ERR(Generic, kSubSteamworksInitFailed, "SteamUserStats() null");
    }
    // Steamworks API は int32、本ヘッダの方が広い (i64) ので clamp
    const int32 v = (value > 0x7fffffffLL)  ? 0x7fffffff
            : (value < -0x80000000LL) ? static_cast<int32>(-0x80000000LL)
                                       : static_cast<int32>(value);
    stats->SetStat(stat_name, v);
    stats->StoreStats();
    return acs::OkInit;
}

/** 整数 stat を読み出して i64 で返す。 */
acs::TResult<acs::i64> FSteamworksBridgeImpl::GetStat(const char* stat_name) noexcept {
    if (!m_bInitialized) {
        return acs::TResult<acs::i64>(ACS_ERR(Generic,
                                              acs::game::kSubSteamworksNotInitialized,
                                              "Init() before GetStat"));
    }
    if (stat_name == nullptr || stat_name[0] == 0) {
        return acs::TResult<acs::i64>(ACS_ERR(Generic, 0, "stat_name null"));
    }
    ISteamUserStats* stats = SteamUserStats();
    if (!stats) {
        return acs::TResult<acs::i64>(ACS_ERR(Generic,
                                              kSubSteamworksInitFailed,
                                              "SteamUserStats() null"));
    }
    int32 v = 0;
    stats->GetStat(stat_name, &v);
    return acs::TResult<acs::i64>(acs::OkInit, static_cast<acs::i64>(v));
}

/** float overload の SetStat + StoreStats で浮動小数 stat を設定する。 */
acs::TResult<void> FSteamworksBridgeImpl::SetFloatStat(const char* stat_name, acs::f32 value) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before SetFloatStat");
    }
    if (stat_name == nullptr || stat_name[0] == 0) {
        return ACS_ERR(Generic, 0, "stat_name null");
    }
    ISteamUserStats* stats = SteamUserStats();
    if (!stats) {
        return ACS_ERR(Generic, kSubSteamworksInitFailed, "SteamUserStats() null");
    }
    stats->SetStat(stat_name, value);  // overload for float
    stats->StoreStats();
    return acs::OkInit;
}

/** 浮動小数 stat を読み出して返す。 */
acs::TResult<acs::f32> FSteamworksBridgeImpl::GetFloatStat(const char* stat_name) noexcept {
    if (!m_bInitialized) {
        return acs::TResult<acs::f32>(ACS_ERR(Generic,
                                              acs::game::kSubSteamworksNotInitialized,
                                              "Init() before GetFloatStat"));
    }
    if (stat_name == nullptr || stat_name[0] == 0) {
        return acs::TResult<acs::f32>(ACS_ERR(Generic, 0, "stat_name null"));
    }
    ISteamUserStats* stats = SteamUserStats();
    if (!stats) {
        return acs::TResult<acs::f32>(ACS_ERR(Generic,
                                              kSubSteamworksInitFailed,
                                              "SteamUserStats() null"));
    }
    float v = 0.0f;
    stats->GetStat(stat_name, &v);
    return acs::TResult<acs::f32>(acs::OkInit, v);
}

/** BIsDlcInstalled で指定 AppID の DLC 所有・インストール状態を返す。 */
bool FSteamworksBridgeImpl::IsDlcOwned(acs::u32 app_id) const noexcept {
    if (!m_bInitialized) return false;
    ISteamApps* apps = SteamApps();
    if (!apps) return false;
    return apps->BIsDlcInstalled(static_cast<AppId_t>(app_id));
}

/** ISteamFriends::SetRichPresence でリッチプレゼンスのキーと値を設定する。 */
acs::TResult<void> FSteamworksBridgeImpl::SetRichPresence(const char* key, const char* value) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before SetRichPresence");
    }
    if (key == nullptr || key[0] == 0) {
        return ACS_ERR(Generic, 0, "key null");
    }
    ISteamFriends* friends = SteamFriends();
    if (!friends) {
        return ACS_ERR(Generic, kSubSteamworksInitFailed, "SteamFriends() null");
    }
    const bool ok = friends->SetRichPresence(key, value);
    if (!ok) {
        return ACS_ERR(Generic, 0, "SetRichPresence returned false");
    }
    return acs::OkInit;
}

/** 即時フレンドのフレンド数を返す。 */
acs::u32 FSteamworksBridgeImpl::GetFriendCount() const noexcept {
    if (!m_bInitialized) return 0;
    ISteamFriends* friends = SteamFriends();
    if (!friends) return 0;
    const int n = friends->GetFriendCount(k_EFriendFlagImmediate);
    return n > 0 ? static_cast<acs::u32>(n) : 0;
}

/** index 番目のフレンドの persona name / SteamID64 を thread_local バッファ越しに返す。 */
acs::game::PlayerIdentity FSteamworksBridgeImpl::GetFriendByIndex(acs::u32 index) const noexcept {
    acs::game::PlayerIdentity id{};
    if (!m_bInitialized) return id;
    ISteamFriends* friends = SteamFriends();
    if (!friends) return id;
    const int n = friends->GetFriendCount(k_EFriendFlagImmediate);
    if (n <= 0 || static_cast<int>(index) >= n) return id;
    const CSteamID friend_id = friends->GetFriendByIndex(static_cast<int>(index),
                                                   k_EFriendFlagImmediate);
    // friend の persona name / SteamID64 を返す (寿命: 次の Tick まで保証)
    // friend ごとに別 buffer が要るので、本 impl では「最後にクエリされた 1 名のみ」
    // を m_Impl->m_PersonaName に上書き保存する形にする。production では
    // friend index ごとに別 buffer (= fixed pool) を用意するのが望ましい。
    const char* const name = friends->GetFriendPersonaName(friend_id);
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

/** ISteamRemoteStorage::FileWrite で Steam Cloud へファイルを書き込む。 */
acs::TResult<void> FSteamworksBridgeImpl::CloudWriteFile(const char* path, const void* data, acs::u32 size) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before CloudWriteFile");
    }
    ISteamRemoteStorage* rs = SteamRemoteStorage();
    if (!rs) {
        return ACS_ERR(Generic, kSubSteamworksInitFailed, "SteamRemoteStorage() null");
    }
    const bool ok = rs->FileWrite(path, data, static_cast<int32>(size));
    if (!ok) return ACS_ERR(Generic, 0, "FileWrite failed (quota / Cloud disabled?)");
    return acs::OkInit;
}

/** ISteamRemoteStorage::FileRead で Steam Cloud からファイルを読み込み、読み取りバイト数を返す。 */
acs::TResult<acs::u32> FSteamworksBridgeImpl::CloudReadFile(const char* path, void* out_buf, acs::u32 buf_size) noexcept {
    if (!m_bInitialized) {
        return acs::TResult<acs::u32>(ACS_ERR(Generic,
                                              acs::game::kSubSteamworksNotInitialized,
                                              "Init() before CloudReadFile"));
    }
    ISteamRemoteStorage* rs = SteamRemoteStorage();
    if (!rs) {
        return acs::TResult<acs::u32>(ACS_ERR(Generic,
                                              kSubSteamworksInitFailed,
                                              "SteamRemoteStorage() null"));
    }
    const int32 actual = rs->FileRead(path, out_buf, static_cast<int32>(buf_size));
    return acs::TResult<acs::u32>(acs::OkInit, actual > 0 ? static_cast<acs::u32>(actual) : 0u);
}

/** ISteamRemoteStorage::FileExists で Steam Cloud にファイルが存在するかを返す。 */
bool FSteamworksBridgeImpl::CloudFileExists(const char* path) const noexcept {
    if (!m_bInitialized) return false;
    ISteamRemoteStorage* rs = SteamRemoteStorage();
    if (!rs) return false;
    return rs->FileExists(path);
}

/** ISteamRemoteStorage::FileDelete で Steam Cloud のファイルを削除する。 */
acs::TResult<void> FSteamworksBridgeImpl::CloudDeleteFile(const char* path) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before CloudDeleteFile");
    }
    ISteamRemoteStorage* rs = SteamRemoteStorage();
    if (!rs) {
        return ACS_ERR(Generic, kSubSteamworksInitFailed, "SteamRemoteStorage() null");
    }
    rs->FileDelete(path);  // 戻り値 bool だが、存在しない path も成功扱い
    return acs::OkInit;
}

/** ISteamRemoteStorage::GetQuota で Steam Cloud の空き/総量バイト数を出力する。 */
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

/** ISteamUGC::GetNumSubscribedItems で購読中の Workshop アイテム数を返す。 */
acs::u32 FSteamworksBridgeImpl::WorkshopGetSubscribedCount() const noexcept {
    if (!m_bInitialized) return 0;
    ISteamUGC* ugc = SteamUGC();
    if (!ugc) return 0;
    return ugc->GetNumSubscribedItems();
}

/** 購読アイテムを一括取得し、index 番目の ID とインストールパスを出力する。 */
void FSteamworksBridgeImpl::WorkshopGetSubscribedItem(acs::u32 index, acs::u64& out_item_id,
                                                      const char*& out_install_path) const noexcept {
    out_item_id      = 0;
    out_install_path = nullptr;
    if (!m_bInitialized) return;
    ISteamUGC* ugc = SteamUGC();
    if (!ugc) return;
    const uint32 count = ugc->GetNumSubscribedItems();
    if (index >= count) return;
    // 全部取って index 番目 (SDK API には index 取得が無いので一括取得)
    static thread_local PublishedFileId_t s_Ids[256] = {};
    const uint32 actual = ugc->GetSubscribedItems(s_Ids, count > 256 ? 256 : count);
    if (index >= actual) return;
    out_item_id = static_cast<acs::u64>(s_Ids[index]);

    // install path
    static thread_local char s_InstallPath[1024] = {};
    uint64 sizeOnDisk = 0;
    uint32 timestamp = 0;
    const bool ok = ugc->GetItemInstallInfo(s_Ids[index], &sizeOnDisk,
                                      s_InstallPath, sizeof(s_InstallPath), &timestamp);
    out_install_path = ok ? s_InstallPath : nullptr;
}

/** ISteamUser::StartVoiceRecording でボイス録音を開始する。 */
acs::TResult<void> FSteamworksBridgeImpl::VoiceStartRecording() noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before VoiceStartRecording");
    }
    ISteamUser* user = SteamUser();
    if (!user) {
        return ACS_ERR(Generic, kSubSteamworksInitFailed, "SteamUser() null");
    }
    user->StartVoiceRecording();
    return acs::OkInit;
}

/** ISteamUser::StopVoiceRecording でボイス録音を停止する (未初期化なら何もしない)。 */
acs::TResult<void> FSteamworksBridgeImpl::VoiceStopRecording() noexcept {
    if (!m_bInitialized) return acs::OkInit;
    ISteamUser* user = SteamUser();
    if (user) user->StopVoiceRecording();
    return acs::OkInit;
}

/** ISteamUser::GetVoice で圧縮ボイスを取り出し、書き込みバイト数を返す。 */
acs::TResult<acs::u32> FSteamworksBridgeImpl::VoiceGetCompressed(void* out_buf, acs::u32 buf_size) noexcept {
    if (!m_bInitialized) {
        return acs::TResult<acs::u32>(acs::OkInit, 0u);
    }
    ISteamUser* user = SteamUser();
    if (!user) {
        return acs::TResult<acs::u32>(ACS_ERR(Generic,
                                              kSubSteamworksInitFailed,
                                              "SteamUser() null"));
    }
    uint32 written = 0;
    const EVoiceResult r = user->GetVoice(true, out_buf, buf_size, &written,
                                    false, nullptr, 0, nullptr, 0);
    if (r == k_EVoiceResultOK || r == k_EVoiceResultNoData) {
        return acs::TResult<acs::u32>(acs::OkInit, written);
    }
    return acs::TResult<acs::u32>(ACS_ERR(Generic, 0, "GetVoice error"));
}

/** ISteamInput::Init で Steam Input を初期化する。 */
acs::TResult<void> FSteamworksBridgeImpl::InputInit() noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(Generic, acs::game::kSubSteamworksNotInitialized,
                       "Init() before InputInit");
    }
    ISteamInput* input = SteamInput();
    if (!input) {
        return ACS_ERR(Generic, kSubSteamworksInitFailed, "SteamInput() null");
    }
    const bool ok = input->Init(false);  // false = no explicit standalone (Steam runtime active)
    if (!ok) return ACS_ERR(Generic, 0, "SteamInput::Init failed");
    return acs::OkInit;
}

/** ISteamInput::GetConnectedControllers で接続コントローラ数を返す。 */
acs::u32 FSteamworksBridgeImpl::InputGetControllerCount() const noexcept {
    if (!m_bInitialized) return 0;
    ISteamInput* input = SteamInput();
    if (!input) return 0;
    InputHandle_t handles[STEAM_INPUT_MAX_COUNT] = {};
    const int n = input->GetConnectedControllers(handles);
    return n > 0 ? static_cast<acs::u32>(n) : 0;
}

/** SteamAPI_RunCallbacks を進め、persona/SteamID を refresh し pending op を compact する。 */
void FSteamworksBridgeImpl::Tick(acs::f32 /*dt*/) noexcept {
    if (!m_bInitialized) return;
    SteamAPI_RunCallbacks();

    // persona name / steam id を毎フレーム refresh (= login 状態変化に追従)
    ISteamUser* user = SteamUser();
    if (user) {
        const CSteamID id = user->GetSteamID();
        const uint64 id64 = id.ConvertToUint64();
        std::snprintf(m_Impl->m_SteamId64Str, sizeof(m_Impl->m_SteamId64Str),
                      "%llu", static_cast<unsigned long long>(id64));
        m_Impl->m_SessionToken = id64;
    }
    ISteamFriends* friends = SteamFriends();
    if (friends) {
        const char* const name = friends->GetPersonaName();
        if (name) {
            std::strncpy(m_Impl->m_PersonaName, name, sizeof(m_Impl->m_PersonaName) - 1);
            m_Impl->m_PersonaName[sizeof(m_Impl->m_PersonaName) - 1] = 0;
        }
    }

    // op を pool から compact する。残すのは:
    //   ・未完了 op (進行中)
    //   ・完了済み「成功 download」op = GetLeaderboardScore が後で読み取る結果
    // 削除するのは完了済み upload / 失敗 download / IO エラー op。
    // 注意: 旧実装は完了 op を一律削除していたため、download 結果が「callback を
    // pump した Tick」で即消え、GetLeaderboardScore が結果を読めなかった
    // (= board 相関を付けても常に pending になる)。成功 download を board ごとに
    // 1 件保持することで、Find→Download 完了後の再呼び出しが正しいスコアを返せる。
    // 同一 board の古い成功 download は最新で上書きし、pool 肥大を防ぐ。
    u32 write = 0;
    for (u32 read = 0; read < m_Impl->m_NumPending; ++read) {
        FAsyncLeaderboardOp& cur = m_Impl->m_PendingOps[read];
        const bool keep =
            !cur.m_bDone ||                                   // 進行中
            (cur.m_bDone && cur.m_bSuccess && !cur.m_bUpload); // 読み取り可能な結果
        if (!keep) continue;

        // 完了 download を残す場合、既に保持済みの同一 board の完了 download を
        // 破棄して最新だけを残す (board ごとに 1 件、pool 肥大防止)。
        if (cur.m_bDone && !cur.m_bUpload) {
            for (u32 w = 0; w < write; ++w) {
                FAsyncLeaderboardOp& kept = m_Impl->m_PendingOps[w];
                if (kept.m_bDone && !kept.m_bUpload
                    && std::strncmp(kept.m_BoardId, cur.m_BoardId,
                                    sizeof(kept.m_BoardId)) == 0) {
                    // 既存を新しい方で上書きしてスキップ
                    kept = cur;
                    goto next_read;
                }
            }
        }
        if (write != read) {
            m_Impl->m_PendingOps[write] = cur;
        }
        ++write;
    next_read:;
    }
    m_Impl->m_NumPending = write;
}

} // namespace acs::steamworks
