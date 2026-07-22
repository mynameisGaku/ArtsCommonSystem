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
// 範囲外:
//   ・実プロトコル / シリアライズ (Pillar J Serialize 側で必要なら整える)
//   ・認証トークン管理      (Pillar S Storefront のセッションを再利用する想定)
//   ・暗号化 / TLS 設定     (具象実装側の責務)
//   ・サーバ側のリーダーボード (Pillar O LiveOps 側で扱う)
//
// 設計選択:
//   ・**stub interface のみ**: 本ヘッダ + .cpp は IBackendClient / IMatchmaker を
//     **抽象 interface として宣言** し、合わせて **常に NotImplemented を返す
//     GetBackendStub() / GetMatchmakerStub()** を提供するだけにとどめる。
//     ACS 本体がリンク時に「最低 1 実装が居る」を保証するための fallback。
//   ・**TResult<T, FErrorCode>**: 例外不使用方針。通信失敗・タイムアウト・パース
//     エラー等はすべて FErrorCode で伝搬し、上位層が `if (r.IsErr())` で握る。
//   ・**const char* 非所有**: URL / event_name / mode 等はすべて呼び出し側が
//     寿命を保証する static / member バッファ。STL <string> 禁止方針。
//   ・**FMatchTicket は不透明 u64**: マッチ検索の進行中状態を表す ID。実装側で
//     具体的な意味 (hash, pointer-as-u64 等) を持ってよいが、呼出側は触らない。
//   ・**全 noexcept**: 例外境界を関数単位で固定し、ABI として整える。
//   ・**Tick(f32 dt)**: 非同期 RPC の応答 pump。実装はメインスレッドで callback
//     を発火する想定 (副 thread からの直接 dispatch を避ける)。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

/**
 * backend / matchmaker 共通の失敗サブコード集。
 *
 * @details
 * TSaveSlot 等と同じく、本ピラーでも「stub = NotImplemented」を
 * subcode = kSub_NotImplemented で表現する。EErrCategory には IO を使う
 * (ネットワーク = I/O の一形態)。
 */
struct FBackendError {
    /** backend 操作の失敗内容を表すサブコード。 */
    enum ESubCode : u16 {
        /** Connect 前に Send/Tick された。 */
        kSub_NotConnected   = 1,

        /** 2 重 Connect (実装側で許容するかは任意)。 */
        kSub_AlreadyConnected = 2,

        /** server_url / event_name が nullptr 等の不正引数。 */
        kSub_BadArgument    = 3,

        /** 接続/送信タイムアウト。 */
        kSub_Timeout        = 4,

        /** socket / TLS / DNS 等の下層失敗。 */
        kSub_NetworkFailure = 5,

        /** 5xx / 認証拒否等のサーバ側エラー。 */
        kSub_ServerError    = 6,

        /** stub: 未実装。 */
        kSub_NotImplemented = 99,
    };
};

/**
 * テレメトリ / 設定取得等の汎用バックエンド窓口の抽象 interface。
 *
 * @details
 * 1 タイトルにつき通常 1 インスタンス (Singleton 的運用)。
 * 寿命はタイトル側 (acs::FApplication 等) が握る。
 */
class IBackendClient {
public:
    /** 空のクライアントを構築する。 */
    IBackendClient() noexcept = default;

    /** 派生実装を正しく破棄するための仮想デストラクタ。 */
    virtual ~IBackendClient() noexcept = default;

    /** コピー禁止 (1 タイトル 1 インスタンス前提のため)。 */
    IBackendClient(const IBackendClient&)            = delete;

    /** コピー代入も禁止。 */
    IBackendClient& operator=(const IBackendClient&) = delete;

    /** ムーブ禁止。 */
    IBackendClient(IBackendClient&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IBackendClient& operator=(IBackendClient&&)      = delete;

    /**
     * 指定 URL のサーバに接続する。
     *
     * @details
     * server_url は呼出側が寿命保証する静的文字列 / member バッファ
     * (例: "https://api.example.com/v1")。同期/非同期は実装次第だが、
     * API 上は完了/失敗を TResult で返す約束。
     * @param server_url 接続先サーバの URL (呼出側が寿命を保証)。
     * @return 成功なら空の TResult、接続失敗ならエラー。
     */
    virtual TResult<void> Connect(const char* server_url) noexcept = 0;

    /** 接続を切る。多重呼出 / 未接続呼出は no-op で許容 (べき等)。 */
    virtual void Disconnect() noexcept = 0;

    /**
     * 現在接続中かを返す。
     *
     * @return Tick で内部状態更新後の最新の接続状態。
     */
    virtual bool IsConnected() const noexcept = 0;

    /**
     * テレメトリイベントを送信する。
     *
     * @details
     * event_name (例: "level_completed") + json_payload
     * (例: {"level":3,"time":42.0}) のセット。送信失敗は
     * kSub_NetworkFailure / kSub_NotConnected 等で返る。実装は内部
     * キューイング + 非同期 flush を選んでよい (要 Tick)。
     * @param event_name イベント名キー。
     * @param json_payload イベント本体の JSON 文字列。
     * @return 成功なら空の TResult、送信失敗ならエラー。
     */
    virtual TResult<void> SendTelemetry(const char* event_name,
                                       const char* json_payload) noexcept = 0;

    /**
     * 非同期 RPC を pump する。
     *
     * @details
     * 毎フレーム呼ばれる前提で、内部キューや受信スレッドからのメッセージを
     * メインスレッドに引き上げる用途。dt はタイムアウト判定等に使う。
     * @param dt 前フレームからの経過秒。
     */
    virtual void Tick(f32 dt) noexcept = 0;
};

/**
 * マッチ検索の進行状態。
 *
 * @details
 * FMatchTicket は検索 1 件分の不透明 handle。StartSearch で発行され、
 * PollStatus で状態を引き、AcceptMatch / CancelSearch で完了させる流れ。
 */
enum class EMatchStatus : u8 {
    /** まだ相手を探している。 */
    Searching = 0,

    /** 相手が見つかり、AcceptMatch 待ち。 */
    Matched   = 1,

    /** 呼出側が CancelSearch した。 */
    Cancelled = 2,

    /** タイムアウト / サーバエラー / ticket 無効。 */
    Failed    = 3,
};

/**
 * マッチ検索 1 件分の不透明 handle。
 *
 * @details
 * m_Opaque == 0 は無効値 (NULL ticket) を意味する。値は実装側が自由に
 * 解釈してよい (連番 ID / pointer / hash 等)。
 */
struct FMatchTicket {
    /** 実装側が意味を持つ不透明値 (0 = 無効)。 */
    u64 m_Opaque = 0;

    /**
     * 有効な ticket かを返す。
     *
     * @return StartSearch 成功で発行された非ゼロ値なら true。
     */
    bool IsValid() const noexcept { return m_Opaque != 0; }
};

/**
 * rating ベース (Glicko-2 / TrueSkill 等) のマッチング検索の抽象 interface。
 */
class IMatchmaker {
public:
    /** 空の matchmaker を構築する。 */
    IMatchmaker() noexcept = default;

    /** 派生実装を正しく破棄するための仮想デストラクタ。 */
    virtual ~IMatchmaker() noexcept = default;

    /** コピー禁止。 */
    IMatchmaker(const IMatchmaker&)            = delete;

    /** コピー代入も禁止。 */
    IMatchmaker& operator=(const IMatchmaker&) = delete;

    /** ムーブ禁止。 */
    IMatchmaker(IMatchmaker&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IMatchmaker& operator=(IMatchmaker&&)      = delete;

    /**
     * マッチ検索を開始する。
     *
     * @details
     * mode は "ranked_1v1" / "casual_4v4" 等の文字列キー、elo_hint は
     * 呼出側が知っている rating 推定値 (新規プレイヤーは 1500 等の中央値で
     * 渡す)。実装は Glicko-2 / TrueSkill で近い rating を引く。
     * @param mode 検索モードの文字列キー。
     * @param elo_hint 呼出側が知っている rating 推定値。
     * @return 成功なら発行された FMatchTicket、失敗ならエラー。
     */
    virtual TResult<FMatchTicket> StartSearch(const char* mode,
                                            u32 elo_hint) noexcept = 0;

    /**
     * 検索をキャンセルする。
     *
     * @param t キャンセル対象の ticket (無効 ticket は no-op、べき等)。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> CancelSearch(FMatchTicket t) noexcept = 0;

    /**
     * 現在の検索状態を返す。
     *
     * @details 副作用なし (内部 pump は IBackendClient::Tick 等に委譲する想定)。
     * @param t 状態を引く ticket (無効 ticket は Failed)。
     * @return 現在の EMatchStatus。
     */
    virtual EMatchStatus PollStatus(FMatchTicket t) noexcept = 0;

    /**
     * Matched 状態の ticket を確定する。
     *
     * @details
     * 失敗時は kSub_Timeout / kSub_ServerError 等。他プレイヤーが
     * Decline / Timeout した場合は再度 Searching 状態に戻る設計を実装側で
     * 選んでよい (本 interface では結果のみ返す)。
     * @param t 確定する ticket。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> AcceptMatch(FMatchTicket t) noexcept = 0;
};

/**
 * プロセス共有の stub IBackendClient を返す。
 *
 * @details 本体側 (タイトル / サンプル) はまずこれを使ってリンクを通す。
 * @return 常に NotImplemented を返す stub クライアントへの参照。
 */
IBackendClient& GetBackendStub() noexcept;

/**
 * プロセス共有の stub IMatchmaker を返す。
 *
 * @return 常に NotImplemented を返す stub matchmaker への参照。
 */
IMatchmaker& GetMatchmakerStub() noexcept;

/**
 * 既定 IBackendClient provider を返す関数の型。
 *
 * @details
 * gameframework は実 backend モジュール (例: ACS::TelemetryFile / ACS::LocalMatch)
 * に依存できない (循環依存になる)。そこで実 backend 側がこの型の関数を
 * SetBackendClientProvider で登録し、ゲームコードは GetDefaultBackendClient を
 * 通じて backend 非依存に既定実装を取得する。
 */
using BackendClientProvider = IBackendClient& (*)() noexcept;

/**
 * 既定 backend provider を登録する (実 backend モジュールの Install* から呼ぶ)。
 *
 * @details 後勝ちで上書きする。
 * @param provider 既定実装を返す関数 (nullptr 登録で stub に戻す)。
 */
void SetBackendClientProvider(BackendClientProvider provider) noexcept;

/**
 * 既定 IBackendClient を返す。
 *
 * @return provider 登録済みならその実装、未登録なら GetBackendStub()。
 */
IBackendClient& GetDefaultBackendClient() noexcept;

/**
 * 既定 IMatchmaker provider を返す関数の型。
 */
using MatchmakerProvider = IMatchmaker& (*)() noexcept;

/**
 * 既定 matchmaker provider を登録する (実 backend モジュールの Install* から呼ぶ)。
 *
 * @details 後勝ちで上書きする。
 * @param provider 既定実装を返す関数 (nullptr 登録で stub に戻す)。
 */
void SetMatchmakerProvider(MatchmakerProvider provider) noexcept;

/**
 * 既定 IMatchmaker を返す。
 *
 * @return provider 登録済みならその実装、未登録なら GetMatchmakerStub()。
 */
IMatchmaker& GetDefaultMatchmaker() noexcept;

} // namespace acs::game
