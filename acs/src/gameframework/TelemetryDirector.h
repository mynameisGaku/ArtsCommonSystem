// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar V — FTelemetryDirector (game analytics event 集約)
//
// 役割:
//   ゲームロジック側から「level_completed」「player_died」「item_purchased」等の
//   解析イベントを TrackEvent() で投入し、内部 pending queue に積んだ上で、
//   一定間隔 (既定 5 秒) または明示 Flush() のタイミングで IBackendClient::
//   SendTelemetry() を経由してサーバへまとめて送出する高レベル director。
//
//   FAchievementManager と同様に、低レイヤの IBackendClient は seam 注入で
//   差し替え可能 (実装は HTTP/gRPC/Steam 等プロジェクト個別)。Backend 未 attach
//   なら local pending queue に積むだけのオフラインモードで動作する
//   (デバッグ / Demo build / 単体テスト用)。
//
//   GDPR / CCPA 要件: FPrivacyDirector の EConsentCategory::Telemetry が無ければ
//   TrackEvent() / Flush() はすべて no-op。consent 未取得状態で TrackEvent 経由
//   の暗黙 opt-in を発生させない (consent dialogue 表示前の起動シーケンスでも
//   安全に呼べる)。
//
// 使い方:
//   FTelemetryDirector td;
//   td.Init(&my_backend, &my_privacy);
//   td.EnableCategory("ui", false);                  // UI 系イベントは送らない
//
//   td.TrackEvent("level_started",
//                 R"({"level":3,"difficulty":"hard"})",
//                 EventPriority::Important,
//                 "gameplay");
//
//   // 毎フレーム
//   td.Tick(dt);
//
//   // タイトル終了時
//   td.Flush();
//   td.Shutdown();
//
// 設計選択 (Pillar V Phase 2):
//   ・**pending queue 上限 100 件 + 古いものから drop**: メモリ枯渇防止と、
//     オフライン中に hammer された場合でも RSS を予測可能に保つため。
//     上限到達時は ACS_LOG_WARN を 1 件出してから RemoveAtSwap(0) で
//     最古を捨てる (順序は保証しない、ただし「送信成功側 vs drop 側」の
//     カウンタは独立に進む)。
//   ・**send は Phase 2 stub**: 実 backend protocol が確定するまでは Flush()
//     内で `_backend->SendTelemetry()` を呼ぶだけの thin pump にとどめる。
//     Stub backend は必ず Err を返すので、_failed_count が上がり pending は
//     残る。具象 backend が attach されたら自然に sent_count が進む。
//   ・**const char* 非所有**: event_name / category / json_payload は呼び出し
//     側 (ゲームコード or リテラル) が保証する static / member lifetime の
//     文字列。Director 側ではコピーしない (STL <string> 禁止方針)。これは
//     特に json_payload が「呼出後すぐに無効化されるスタックバッファ」だと
//     危険なので、利用側で寿命設計を要する (典型的にはリテラル or thread-local
//     static char[N] 等)。
//   ・**category enable/disable**: "ui" や "debug" など、ビルドや A/B テストで
//     送りたくないカテゴリを runtime で間引けるよう、許可リスト方式の bit に
//     せず可変 TArray<FCategoryFilter> で持つ。件数は通常 10〜30 程度なので
//     線形検索で十分。default は「未登録 = enabled」とし、明示的に false を
//     設定したカテゴリだけ落とす (= explicit deny list)。
//   ・**FPrivacyDirector attach optional**: nullptr 注入時は consent ガードを
//     スキップ (= 全イベント許可)。テスト / オフラインビルド用の逃げ道。
//     production では必ず privacy を渡すこと。
//   ・**非コピー・非ムーブ、全 noexcept**: 他 Director 系と統一。
//
// 範囲外 (Phase 3+ で):
//   ・実 backend protocol (REST JSON / gRPC streaming / Steam ISteamUserStats)
//   ・event の永続化 (オフライン → オンライン復帰時の再送 backlog)
//   ・event sampling / rate limiting (server 側 schema と合わせて実装)
//   ・bind-able callback (event 1 件送信成功 / 失敗時の subscribe)
//   ・session id / user id の自動付与 (Pillar S Storefront のセッション統合)
//   ・event のローカルバッファ (SQLite / 自前 binary log)
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "container/Array.h"

namespace acs::game {

// FBackendClient.h / FPrivacyDirector.h を引き込むと TResult<T> 等 foundation 系も
// 芋づるで広がる。本ヘッダは公開 API のみ薄く保つ方針なので、interface /
// class は forward declare に留めて、実体 include は .cpp 側で行う。
class IBackendClient;
class FPrivacyDirector;

// =============================================================================
// EventPriority — analytics event の重要度ヒント
// -----------------------------------------------------------------------------
// queue 溢れ時のドロップ優先度・送信失敗時のリトライ回数・将来の sampling rate
// を切り替えるためのマーカ。Debug は実行ログ目的、Info は通常イベント、
// Important は KPI 系 (購入 / 進行)、Critical は障害監視 (例外 / クラッシュ前
// 兆) を想定する。Phase 2 では送信側がまだ全て同じ扱いだが、API 形状だけ
// 確定して Phase 3 の sampling/retry 実装で差別化する。
// =============================================================================
enum class EventPriority : u8 {
    Debug     = 0,  // 開発用 (デフォルトの出荷ビルドではフィルタされる想定)
    Info      = 1,  // 通常のゲーム進行イベント (level_started 等)
    Important = 2,  // KPI / 収益 / 主要進行 (purchase_completed, boss_defeated)
    Critical  = 3,  // 障害監視 / 異常検知 (exception_caught, soft_lock_detected)
};

// =============================================================================
// TelemetryEvent — pending queue に積まれる 1 イベントの POD
// -----------------------------------------------------------------------------
// 文字列はすべて呼出側保証の static lifetime ポインタ。Director 側ではコピー
// しない。timestamp は Clock::MillisSinceStartup() の値 (起動からの ms)、
// 0 は「Clock 未取得 / TrackEvent 直前にゼロクリア」を表す。
// =============================================================================
struct TelemetryEvent {
    const char*   event_name   = nullptr;
    const char*   category     = nullptr;
    const char*   json_payload = nullptr;
    EventPriority priority     = EventPriority::Info;
    u64           timestamp    = 0;  // Clock::MillisSinceStartup() at TrackEvent
};

// =============================================================================
// FTelemetryDirector — analytics event 集約・送信ハブ
// -----------------------------------------------------------------------------
// アプリ全体で 1 個運用される想定。複数同時運用しないため非コピー・非ムーブ。
// =============================================================================
class FTelemetryDirector {
public:
    FTelemetryDirector()  noexcept = default;
    ~FTelemetryDirector() noexcept = default;

    FTelemetryDirector(const FTelemetryDirector&)            = delete;
    FTelemetryDirector& operator=(const FTelemetryDirector&) = delete;
    FTelemetryDirector(FTelemetryDirector&&)                 = delete;
    FTelemetryDirector& operator=(FTelemetryDirector&&)      = delete;

    // ----- 初期化 / 終了 --------------------------------------------------
    // backend: IBackendClient* (寿命は呼出側が保証、nullptr 不可 = 必ず stub 渡す)
    // privacy: FPrivacyDirector* (nullptr 可 = consent ガードをスキップ)
    // 2 重 Init は内部状態を上書き (= backend / privacy 差し替え)。
    void Init(IBackendClient* backend, FPrivacyDirector* privacy = nullptr) noexcept;

    // pending queue を空にし、backend / privacy 参照を切る。
    // 終了時の Flush は呼出側責務 (本関数では送信を試みない、確実に no-op で抜ける)。
    void Shutdown() noexcept;

    // ----- event 投入 -----------------------------------------------------
    // event_name == nullptr / json_payload == nullptr は no-op (defensive)。
    // consent 未取得 (FPrivacyDirector::HasConsent(Telemetry) == false) も no-op。
    // category が disabled 設定されていれば no-op。
    // queue 上限到達時は最古 1 件を drop して新規を末尾追加 (FIFO drop)。
    void TrackEvent(const char*   event_name,
                    const char*   json_payload = "{}",
                    EventPriority priority     = EventPriority::Info,
                    const char*   category     = "general") noexcept;

    // pending を 1 件ずつ backend->SendTelemetry() に流し込む。
    // 成功 → _sent_count++、pending から除去。
    // 失敗 → _failed_count++、pending には残す (= 次回 Flush で再送試行)。
    // backend == nullptr / consent 無し なら全件残したまま return。
    // Phase 2 では stub backend が常に Err を返すため、実質 _failed_count が
    // 増える挙動になる。
    void Flush() noexcept;

    // ----- 統計 -----------------------------------------------------------
    u32 PendingCount() const noexcept;
    u32 SentCount()    const noexcept;
    u32 FailedCount()  const noexcept;

    // ----- フレーム pump --------------------------------------------------
    // dt を蓄積し、_flush_interval を超えたら自動 Flush。0 < interval が前提。
    // Init() 未呼出時は no-op。
    void Tick(f32 dt) noexcept;

    // 自動 Flush の間隔 (秒)。0 以下を渡すと 5 秒既定にクランプする。
    void SetFlushInterval(f32 sec) noexcept;

    // ----- category フィルタ ---------------------------------------------
    // category == nullptr は no-op。enabled = false で deny に追加、true で
    // deny から削除 (= 既定の enabled に戻す)。既定動作は「未登録 = enabled」。
    void EnableCategory(const char* category, bool enabled) noexcept;

    // ----- デバッグ -------------------------------------------------------
    // pending queue / category filter / counter を初期化。
    // Init() が行った backend / privacy 参照は保持する (= テスト中の再利用可)。
    void Clear() noexcept;

private:
    // category が現状 enabled かどうか。未登録は true、明示 false なら false。
    // .cpp 内で線形検索 (件数 < 30 想定)。
    bool IsCategoryEnabledInternal(const char* category) const noexcept;

    // queue 上限到達時に最古 1 件を drop して 1 件分の枠を空ける。
    // FIFO 順序は保たない (TArray::RemoveAtSwap 経由) が、TimestampでGUI側
    // でソートする運用を想定。
    void DropOldestIfFull() noexcept;

    // category 名と enabled flag の組。TArray<...> の要素型。
    struct FCategoryFilter {
        const char* category = nullptr;  // 非所有 (リテラル想定)
        bool        enabled  = true;
    };

    // pending queue 上限。100 件 = 5 秒 flush 間隔で 20 evt/sec を 5 秒バッファ
    // できる目安。AAA タイトルでも通常 1〜2 evt/sec なので 100 件は十分。
    static constexpr u32 kMaxPending = 100;

    TArray<TelemetryEvent>  _pending;        // 送信待ち event queue
    TArray<FCategoryFilter>  _filters;        // category 別 enable/disable

    IBackendClient*        _backend  = nullptr;  // 注入 (寿命は呼出側)
    FPrivacyDirector*       _privacy  = nullptr;  // optional 注入

    u32  _sent_count     = 0;
    u32  _failed_count   = 0;

    f32  _flush_interval = 5.0f;   // 既定 5 秒
    f32  _elapsed_since_flush = 0.0f;

    bool _initialized    = false;
};

} // namespace acs::game
