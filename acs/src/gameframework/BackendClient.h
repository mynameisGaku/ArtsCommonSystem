// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar V — Backend Services seam (IBackendClient / IMatchmaker)
//
// 役割:
//   ゲーム本体から「サーバ側 (dedicated server, テレメトリ、マッチメイカー)」へ
//   問い合わせるための **抽象 seam**。
//   ACS 本体は具象な net stack (gRPC / HTTPS / WebSocket / Steam ISteamNetworking
//   等) を抱え込まず、interface だけを提供する。
//   ・タイトル側 (acs::FApplication) は IBackendClient* / IMatchmaker* を持ち、
//   ・実装 (BackendClientHttp, BackendClientSteam, MatchmakerGlicko2 等) は
//     プロジェクト個別に差し込む。
//   これにより、(a) ACS Foundation/GameFramework の依存最小化、(b) サーバ無し
//   オフラインビルドでもリンクが通る、(c) E2E テスト用 fake を簡単に差せる、
//   という 3 つの seam 要件を満たす。
//
// 想定される具象実装 (本ファイルには含めない):
//   ・BackendClientHttp        — REST + JSON テレメトリ送信 (libcurl 系)
//   ・BackendClientGrpc        — gRPC streaming (双方向 RPC)
//   ・BackendClientSteam       — Steamworks ISteamGameServer / ISteamUserStats
//   ・MatchmakerGlicko2        — Glicko-2 レーティングベースの非同期マッチ検索
//                                (ELO の後継。1v1 系で標準的な選択)
//   ・MatchmakerTrueSkill      — Microsoft TrueSkill (チーム戦/n人混合向け)
//   ・MatchmakerSteam          — Steam Lobby + GameCoordinator 連携
//   ・MatchmakerDedicatedSrv   — 自社 dedicated server (Agones / Unreal Pixel
//                                Streaming 風の placement service と RPC)
//
// 範囲外 (本フェーズでは扱わない):
//   ・実プロトコル / シリアライズ (Pillar J Serialize 側で必要なら整える)
//   ・認証トークン管理      (Pillar S Storefront のセッションを再利用する想定)
//   ・暗号化 / TLS 設定     (具象実装側の責務)
//   ・サーバ側のリーダーボード (Pillar O LiveOps 側で扱う)
//
// 設計選択:
//   ・**stub interface のみ**: 本ヘッダ + .cpp は IBackendClient / IMatchmaker を
//     **抽象 interface として宣言** し、合わせて **常に NotImplemented を返す
//     BackendClientStub / MatchmakerStub** を提供するだけにとどめる。
//     ACS 本体がリンク時に「最低 1 実装が居る」を保証するための fallback。
//   ・**TResult<T, FErrorCode>**: 例外不使用方針。通信失敗・タイムアウト・パース
//     エラー等はすべて FErrorCode で伝搬し、上位層が `if (r.IsErr())` で握る。
//   ・**const char* 非所有**: URL / event_name / mode 等はすべて呼び出し側が
//     寿命を保証する static / member バッファ。STL <string> 禁止方針。
//   ・**MatchTicket は不透明 u64**: マッチ検索の進行中状態を表す ID。実装側で
//     具体的な意味 (hash, pointer-as-u64 等) を持ってよいが、呼出側は触らない。
//   ・**全 noexcept**: 例外境界を関数単位で固定し、ABI として整える。
//   ・**Tick(f32 dt)**: 非同期 RPC の応答 pump。実装はメインスレッドで callback
//     を発火する想定 (副 thread からの直接 dispatch を避ける)。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

// =============================================================================
// 共通: stub 用エラーサブコード
// -----------------------------------------------------------------------------
// FSaveSlot 等と同じく、本ピラーでも「Phase 1 stub = NotImplemented」を
// `subcode = kSub_NotImplemented` で表現する。`ErrCategory` には IO を使う
// (ネットワーク = I/O の一形態)。
// =============================================================================
struct FBackendError {
    enum SubCode : u16 {
        kSub_NotConnected   = 1,  // Connect 前に Send/Tick された
        kSub_AlreadyConnected = 2,// 2 重 Connect (実装側で許容するかは任意)
        kSub_BadArgument    = 3,  // server_url / event_name が nullptr 等
        kSub_Timeout        = 4,  // 接続/送信タイムアウト
        kSub_NetworkFailure = 5,  // socket / TLS / DNS 等の下層失敗
        kSub_ServerError    = 6,  // 5xx / 認証拒否等のサーバ側エラー
        kSub_NotImplemented = 99, // stub: 未実装
    };
};

// =============================================================================
// IBackendClient — テレメトリ / 設定取得等の汎用バックエンド窓口
// -----------------------------------------------------------------------------
// 1 タイトルにつき通常 1 インスタンス (Singleton 的運用)。
// 寿命はタイトル側 (acs::FApplication 等) が握る。
// =============================================================================
class IBackendClient {
public:
    IBackendClient() noexcept = default;
    virtual ~IBackendClient() noexcept = default;

    IBackendClient(const IBackendClient&)            = delete;
    IBackendClient& operator=(const IBackendClient&) = delete;
    IBackendClient(IBackendClient&&)                 = delete;
    IBackendClient& operator=(IBackendClient&&)      = delete;

    // 指定 URL のサーバに接続する。`server_url` は呼出側が寿命保証する
    // 静的文字列 / member バッファ (例: "https://api.example.com/v1")。
    // 同期/非同期は実装次第だが、API 上は完了/失敗を TResult で返す約束。
    virtual TResult<void> Connect(const char* server_url) noexcept = 0;

    // 接続を切る。多重呼出 / 未接続呼出は no-op で許容 (べき等)。
    virtual void Disconnect() noexcept = 0;

    // 現在接続中か。Tick で内部状態更新後の最新値を返す。
    virtual bool IsConnected() const noexcept = 0;

    // テレメトリイベント送信。`event_name` (例: "level_completed") +
    // `json_payload` (例: `{"level":3,"time":42.0}`) のセット。
    // 送信失敗は kSub_NetworkFailure / kSub_NotConnected 等で返る。
    // 実装は内部キューイング + 非同期 flush を選んでよい (要 Tick)。
    virtual TResult<void> SendTelemetry(const char* event_name,
                                       const char* json_payload) noexcept = 0;

    // 非同期 RPC pump。毎フレーム呼ばれる前提で、内部キューや
    // 受信スレッドからのメッセージをメインスレッドに引き上げる用途。
    // `dt` は前フレームからの経過秒。タイムアウト判定等に使う。
    virtual void Tick(f32 dt) noexcept = 0;
};

// =============================================================================
// IMatchmaker — マッチング検索 (Glicko-2 / TrueSkill 等の rating ベース想定)
// -----------------------------------------------------------------------------
// `MatchTicket` は検索 1 件分の不透明 handle。StartSearch で発行され、
// PollStatus で状態を引き、AcceptMatch / CancelSearch で完了させる流れ。
// =============================================================================

// マッチ検索の進行状態。
enum class EMatchStatus : u8 {
    Searching = 0,  // まだ相手を探している
    Matched   = 1,  // 相手が見つかり、AcceptMatch 待ち
    Cancelled = 2,  // 呼出側が CancelSearch した
    Failed    = 3,  // タイムアウト / サーバエラー / ticket 無効
};

// 検索 handle。`m_Opaque == 0` は無効値 (NULL ticket) を意味する。
// 値は実装側が自由に解釈してよい (連番 ID / pointer / hash 等)。
struct MatchTicket {
    u64 m_Opaque = 0;

    // 有効な ticket か (StartSearch 成功 = 非ゼロ)。
    bool IsValid() const noexcept { return m_Opaque != 0; }
};

class IMatchmaker {
public:
    IMatchmaker() noexcept = default;
    virtual ~IMatchmaker() noexcept = default;

    IMatchmaker(const IMatchmaker&)            = delete;
    IMatchmaker& operator=(const IMatchmaker&) = delete;
    IMatchmaker(IMatchmaker&&)                 = delete;
    IMatchmaker& operator=(IMatchmaker&&)      = delete;

    // マッチ検索開始。`mode` は "ranked_1v1" / "casual_4v4" 等の文字列キー、
    // `elo_hint` は呼出側が知っている rating 推定値 (新規プレイヤーは 1500
    // 等の中央値で渡す)。実装は Glicko-2 / TrueSkill で近い rating を引く。
    virtual TResult<MatchTicket> StartSearch(const char* mode,
                                            u32 elo_hint) noexcept = 0;

    // 検索キャンセル。`t.IsValid() == false` は no-op で許容 (べき等)。
    virtual TResult<void> CancelSearch(MatchTicket t) noexcept = 0;

    // 現在の検索状態を返す。`t.IsValid() == false` は Failed を返す。
    // 副作用なし (内部 pump は IBackendClient::Tick 等に委譲する想定)。
    virtual EMatchStatus PollStatus(MatchTicket t) noexcept = 0;

    // Matched 状態の ticket を確定。失敗時は kSub_Timeout / kSub_ServerError 等。
    // 他プレイヤーが Decline / Timeout した場合は再度 Searching 状態に戻る
    // 設計を実装側で選んでよい (本 interface では結果のみ返す)。
    virtual TResult<void> AcceptMatch(MatchTicket t) noexcept = 0;
};

// =============================================================================
// アクセサ: stub 実装への参照を取る
// -----------------------------------------------------------------------------
// 本体側 (タイトル / サンプル) はまずこの 2 つを使ってリンクを通す。
// 具象実装に切り替える際は IBackendClient* を持つメンバ変数に
// `BackendClientHttp` 等を差し替える。
// =============================================================================

// プロセス共有の stub IBackendClient。常に NotImplemented を返す。
IBackendClient& GetBackendStub() noexcept;

// プロセス共有の stub IMatchmaker。常に NotImplemented を返す。
IMatchmaker& GetMatchmakerStub() noexcept;

// =============================================================================
// 既定 backend の provider 結線 (実 backend モジュールへの委譲点)
// -----------------------------------------------------------------------------
// gameframework は実 backend モジュール (例: ACS::TelemetryFile / ACS::LocalMatch)
// に依存できない (循環依存になる: backend 側が本 interface に依存する)。そこで実
// backend 側が `SetBackendClientProvider()` / `SetMatchmakerProvider()` で「既定
// 実装を返す関数」を登録し、ゲームコードは `GetDefaultBackendClient()` /
// `GetDefaultMatchmaker()` を通じて backend 非依存に既定実装を取得する。
//
//   ・provider 未登録 (= backend 未リンク / flag OFF) → stub を返す。
//   ・provider 登録済み (= 実 backend リンク + Install 呼び出し済み) → 実装を返す。
//
// 典型: アプリ起動時に一度だけ
//   #if WITH_ACS_TELEMETRY_FILE
//       acs::telemetryfile::InstallFileTelemetryAsDefault();   // provider を登録
//   #endif
//   #if WITH_ACS_LOCAL_MATCHMAKER
//       acs::localmatch::InstallLocalMatchmakerAsDefault();     // provider を登録
//   #endif
// 以降はどこでも `acs::game::GetDefaultBackendClient()` /
// `GetDefaultMatchmaker()` で実 backend が得られる。
// =============================================================================

// 既定 IBackendClient provider を返す関数の型。
using BackendClientProvider = IBackendClient& (*)() noexcept;

// 既定 backend provider を登録する (実 backend モジュールの Install* から呼ぶ)。
// nullptr 登録で stub に戻す。後勝ち。
void SetBackendClientProvider(BackendClientProvider provider) noexcept;

// 既定 IBackendClient を返す。provider 登録済みならその実装、未登録なら
// GetBackendStub()。
IBackendClient& GetDefaultBackendClient() noexcept;

// 既定 IMatchmaker provider を返す関数の型。
using MatchmakerProvider = IMatchmaker& (*)() noexcept;

// 既定 matchmaker provider を登録する (実 backend モジュールの Install* から呼ぶ)。
// nullptr 登録で stub に戻す。後勝ち。
void SetMatchmakerProvider(MatchmakerProvider provider) noexcept;

// 既定 IMatchmaker を返す。provider 登録済みならその実装、未登録なら
// GetMatchmakerStub()。
IMatchmaker& GetDefaultMatchmaker() noexcept;

} // namespace acs::game
