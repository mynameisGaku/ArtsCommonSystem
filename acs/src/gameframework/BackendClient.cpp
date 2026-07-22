// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar V — IBackendClient / IMatchmaker stub 実装
//
// 本ファイルは FBackendClient.h で宣言した 2 つの interface に対し、
// 「常に NotImplemented を返すだけ」の defensive stub を提供する。
//
// 目的:
//   ・ACS 本体 / サンプルがサーバ実装の有無に関わらずリンクを通せるようにする
//     (Pillar V の seam 要件)。
//   ・タイトル側が `IBackendClient* p = &acs::game::GetBackendStub();` のように
//     null-object パターンで保持し、後から具象実装に差し替える経路を確保する。
//   ・stub に対する Connect / SendTelemetry 呼び出しは「成功扱いで黙る」ではなく
//     **必ず TResult<...> Err を返す**ことで、本番ビルドに stub が紛れ込んだ
//     ケースを QA 工程で検出可能にしておく。
//
// 設計メモ:
//   ・Stub は **process-wide singleton** で十分。`static` ローカル変数で
//     thread-safe initialization (C++11 magic statics) を活用する。
//   ・コピー/ムーブは IBackendClient/IMatchmaker 側で delete 済みなので、
//     stub 派生クラスも自然に non-copy / non-movable。
//   ・全関数 noexcept。stub なので分岐も最小限。
//   ・引数バリデーション (nullptr 等) は本実装ではしない: NotImplemented を
//     先に返してしまうため。具象実装側で kSub_BadArgument を返す責務になる。
#include "gameframework/BackendClient.h"
#include "threading/Atomic.h"

namespace acs::game {

namespace {

/**
 * IBackendClient の null-object stub 実装。
 *
 * @details
 * すべての呼び出しに対して ACS_ERR(IO, kSub_NotImplemented, ...) を返す。
 * IsConnected() は常に false、Tick / Disconnect は副作用なしの no-op。
 */
class FBackendClientStub final : public IBackendClient {
public:
    /** 既定構築する。 */
    FBackendClientStub() noexcept = default;

    /** 破棄する。 */
    ~FBackendClientStub() noexcept override = default;

    /**
     * NotImplemented を返す (stub)。
     *
     * @param server_url 無視される。
     * @return 常に kSub_NotImplemented エラー。
     */
    TResult<void> Connect(const char* server_url) noexcept override {
        (void)server_url;
        return ACS_ERR(IO, FBackendError::kSub_NotImplemented,
                       "IBackendClient::Connect is not implemented "
                       "(stub: link a concrete backend implementation)");
    }

    /** 副作用なしの no-op (stub は never-connected)。 */
    void Disconnect() noexcept override {
        // stub は never-connected 状態。no-op で安全に通す。
    }

    /**
     * 常に未接続を返す (stub)。
     *
     * @return 常に false。
     */
    bool IsConnected() const noexcept override {
        return false;
    }

    /**
     * NotImplemented を返す (stub)。
     *
     * @param event_name 無視される。
     * @param json_payload 無視される。
     * @return 常に kSub_NotImplemented エラー。
     */
    TResult<void> SendTelemetry(const char* event_name,
                               const char* json_payload) noexcept override {
        (void)event_name;
        (void)json_payload;
        return ACS_ERR(IO, FBackendError::kSub_NotImplemented,
                       "IBackendClient::SendTelemetry is not implemented "
                       "(stub: link a concrete backend implementation)");
    }

    /**
     * 副作用なしの no-op (stub には pump 対象なし)。
     *
     * @param dt 無視される。
     */
    void Tick(f32 dt) noexcept override {
        (void)dt;
        // stub には pump 対象なし。no-op。
    }
};

/**
 * IMatchmaker の null-object stub 実装。
 *
 * @details
 * StartSearch / CancelSearch / AcceptMatch は NotImplemented を返し、PollStatus は
 * Failed を返す (= 「無効な ticket」を表すデフォルト挙動)。
 */
class FMatchmakerStub final : public IMatchmaker {
public:
    /** 既定構築する。 */
    FMatchmakerStub() noexcept = default;

    /** 破棄する。 */
    ~FMatchmakerStub() noexcept override = default;

    /**
     * NotImplemented を返す (stub)。
     *
     * @param mode 無視される。
     * @param elo_hint 無視される。
     * @return 常に kSub_NotImplemented エラー。
     */
    TResult<FMatchTicket> StartSearch(const char* mode,
                                    u32 elo_hint) noexcept override {
        (void)mode;
        (void)elo_hint;
        return ACS_ERR(IO, FBackendError::kSub_NotImplemented,
                       "IMatchmaker::StartSearch is not implemented "
                       "(stub: link a concrete matchmaker implementation)");
    }

    /**
     * NotImplemented を返す (stub)。
     *
     * @param t 無視される。
     * @return 常に kSub_NotImplemented エラー。
     */
    TResult<void> CancelSearch(FMatchTicket t) noexcept override {
        (void)t;
        return ACS_ERR(IO, FBackendError::kSub_NotImplemented,
                       "IMatchmaker::CancelSearch is not implemented "
                       "(stub: link a concrete matchmaker implementation)");
    }

    /**
     * 常に Failed を返す (stub は検索を走らせない)。
     *
     * @param t 無視される。
     * @return 常に EMatchStatus::Failed。
     */
    EMatchStatus PollStatus(FMatchTicket t) noexcept override {
        (void)t;
        // stub は検索を一切走らせないので、どの ticket も Failed 扱い。
        return EMatchStatus::Failed;
    }

    /**
     * NotImplemented を返す (stub)。
     *
     * @param t 無視される。
     * @return 常に kSub_NotImplemented エラー。
     */
    TResult<void> AcceptMatch(FMatchTicket t) noexcept override {
        (void)t;
        return ACS_ERR(IO, FBackendError::kSub_NotImplemented,
                       "IMatchmaker::AcceptMatch is not implemented "
                       "(stub: link a concrete matchmaker implementation)");
    }
};

} // namespace

/** 既定 stub の IBackendClient (C++11 magic statics による process-wide singleton) を返す。 */
IBackendClient& GetBackendStub() noexcept {
    static FBackendClientStub s_instance;
    return s_instance;
}

/** 既定 stub の IMatchmaker (process-wide singleton) を返す。 */
IMatchmaker& GetMatchmakerStub() noexcept {
    static FMatchmakerStub s_instance;
    return s_instance;
}

// 既定 backend provider 結線 (実 backend への委譲)。
// gameframework 単体 (SDK / backend 無し) でもリンクが通るよう、provider は
// 関数ポインタ 1 個で保持し、未登録なら stub にフォールバックする。
namespace {
/** 既定 IBackendClient provider (未登録なら nullptr で stub にフォールバック)。 */
TAtomic<BackendClientProvider> g_BackendClientProvider{nullptr};

/** 既定 IMatchmaker provider (未登録なら nullptr で stub にフォールバック)。 */
TAtomic<MatchmakerProvider> g_MatchmakerProvider{nullptr};
} // namespace

/** 既定 IBackendClient provider を登録する (nullptr で stub に戻す)。 */
void SetBackendClientProvider(BackendClientProvider Provider) noexcept
{
    g_BackendClientProvider.Store(Provider, EMemoryOrder::Release);
}

/** provider 登録済みならその実装、未登録なら GetBackendStub() を返す。 */
IBackendClient& GetDefaultBackendClient() noexcept
{
    const BackendClientProvider Provider = g_BackendClientProvider.Load(EMemoryOrder::Acquire);
    return Provider ? Provider() : GetBackendStub();
}

/** 既定 IMatchmaker provider を登録する (nullptr で stub に戻す)。 */
void SetMatchmakerProvider(MatchmakerProvider Provider) noexcept
{
    g_MatchmakerProvider.Store(Provider, EMemoryOrder::Release);
}

/** provider 登録済みならその実装、未登録なら GetMatchmakerStub() を返す。 */
IMatchmaker& GetDefaultMatchmaker() noexcept
{
    const MatchmakerProvider Provider = g_MatchmakerProvider.Load(EMemoryOrder::Acquire);
    return Provider ? Provider() : GetMatchmakerStub();
}

} // namespace acs::game
