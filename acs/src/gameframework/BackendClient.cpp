// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar V — IBackendClient / IMatchmaker stub 実装
//
// 本ファイルは BackendClient.h で宣言した 2 つの interface に対し、
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
// 将来 (Phase 2 以降, BackendServices モジュール本実装フェーズ):
//   ・libcurl + nlohmann::json 風の HTTPS テレメトリ client (BackendClientHttp)
//   ・Glicko-2 マッチメイカー本実装 (MatchmakerGlicko2)
//   ・dedicated server placement (Agones 互換 RPC) 連携
//   ・Steamworks 系アダプタは Pillar S Storefront とセットで実装する。
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

namespace acs::game {

namespace {

// -----------------------------------------------------------------------------
// BackendClientStub — IBackendClient の null-object 実装
// -----------------------------------------------------------------------------
// すべての呼び出しに対して ACS_ERR(IO, kSub_NotImplemented, ...) を返す。
// IsConnected() は常に false。Tick / Disconnect は副作用なしの no-op。
class BackendClientStub final : public IBackendClient {
public:
    BackendClientStub() noexcept = default;
    ~BackendClientStub() noexcept override = default;

    TResult<void> Connect(const char* server_url) noexcept override {
        (void)server_url;
        return ACS_ERR(IO, BackendError::kSub_NotImplemented,
                       "IBackendClient::Connect is not implemented "
                       "(stub: link a concrete backend implementation)");
    }

    void Disconnect() noexcept override {
        // stub は never-connected 状態。no-op で安全に通す。
    }

    bool IsConnected() const noexcept override {
        return false;
    }

    TResult<void> SendTelemetry(const char* event_name,
                               const char* json_payload) noexcept override {
        (void)event_name;
        (void)json_payload;
        return ACS_ERR(IO, BackendError::kSub_NotImplemented,
                       "IBackendClient::SendTelemetry is not implemented "
                       "(stub: link a concrete backend implementation)");
    }

    void Tick(f32 dt) noexcept override {
        (void)dt;
        // stub には pump 対象なし。no-op。
    }
};

// -----------------------------------------------------------------------------
// MatchmakerStub — IMatchmaker の null-object 実装
// -----------------------------------------------------------------------------
// StartSearch / CancelSearch / AcceptMatch は NotImplemented で返し、
// PollStatus は Failed を返す (= 「無効な ticket」を表すデフォルト挙動)。
class MatchmakerStub final : public IMatchmaker {
public:
    MatchmakerStub() noexcept = default;
    ~MatchmakerStub() noexcept override = default;

    TResult<MatchTicket> StartSearch(const char* mode,
                                    u32 elo_hint) noexcept override {
        (void)mode;
        (void)elo_hint;
        return ACS_ERR(IO, BackendError::kSub_NotImplemented,
                       "IMatchmaker::StartSearch is not implemented "
                       "(stub: link a concrete matchmaker implementation)");
    }

    TResult<void> CancelSearch(MatchTicket t) noexcept override {
        (void)t;
        return ACS_ERR(IO, BackendError::kSub_NotImplemented,
                       "IMatchmaker::CancelSearch is not implemented "
                       "(stub: link a concrete matchmaker implementation)");
    }

    EMatchStatus PollStatus(MatchTicket t) noexcept override {
        (void)t;
        // stub は検索を一切走らせないので、どの ticket も Failed 扱い。
        return EMatchStatus::Failed;
    }

    TResult<void> AcceptMatch(MatchTicket t) noexcept override {
        (void)t;
        return ACS_ERR(IO, BackendError::kSub_NotImplemented,
                       "IMatchmaker::AcceptMatch is not implemented "
                       "(stub: link a concrete matchmaker implementation)");
    }
};

} // namespace

// -----------------------------------------------------------------------------
// アクセサ: function-local static で process-wide singleton を保持
// -----------------------------------------------------------------------------
// C++11 magic statics により初期化は thread-safe。
// 破棄順序は他の static と独立で良い (相互依存なし)。
IBackendClient& GetBackendStub() noexcept {
    static BackendClientStub s_instance;
    return s_instance;
}

IMatchmaker& GetMatchmakerStub() noexcept {
    static MatchmakerStub s_instance;
    return s_instance;
}

} // namespace acs::game
