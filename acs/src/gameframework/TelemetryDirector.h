// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "container/Array.h"
#include "gameframework/Forward.h"

namespace acs::game {

// BackendClient.h / PrivacyDirector.h を引き込むと TResult<T> 等 foundation 系も
// 芋づるで広がる。本ヘッダは公開 API のみ薄く保つ方針なので、interface /
// class は forward declare に留めて、実体 include は .cpp 側で行う。
class IBackendClient;
/**
 * analytics event の重要度ヒント。
 *
 * @details
 * queue 溢れ時のドロップ優先度・送信失敗時のリトライ回数・将来の sampling rate を
 * 切り替えるためのマーカ。現状は送信側が全て同じ扱いだが、API 形状だけ確定して
 * 将来の sampling/retry 実装で差別化する。
 */
enum class EEventPriority : u8 {
    /** 開発用 (デフォルトの出荷ビルドではフィルタされる想定)。 */
    Debug     = 0,

    /** 通常のゲーム進行イベント (level_started 等)。 */
    Info      = 1,

    /** KPI / 収益 / 主要進行 (purchase_completed, boss_defeated)。 */
    Important = 2,

    /** 障害監視 / 異常検知 (exception_caught, soft_lock_detected)。 */
    Critical  = 3,
};

/**
 * pending queue に積まれる 1 イベントの POD。
 *
 * @details
 * 文字列はすべて呼出側保証の static lifetime ポインタで、Director 側ではコピーしない。
 */
struct FTelemetryEvent {
    /** イベント名 (非所有。呼出側保証の static lifetime)。 */
    const char*   event_name   = nullptr;

    /** カテゴリ名 (非所有。フィルタ判定に使う)。 */
    const char*   category     = nullptr;

    /** JSON ペイロード文字列 (非所有)。 */
    const char*   json_payload = nullptr;

    /** イベントの重要度ヒント。 */
    EEventPriority priority     = EEventPriority::Info;

    /** TrackEvent 時の CClock::MillisSinceStartup() (起動からの ms、0 = 未取得)。 */
    u64           timestamp    = 0;
};

/**
 * analytics event を集約してサーバへまとめて送出する高レベル director。
 *
 * @details
 * TrackEvent() で投入したイベントを内部 pending queue に積み、一定間隔または明示
 * Flush() のタイミングで IBackendClient::SendTelemetry() を経由して送出する。Backend
 * 未 attach ならオフラインモードで pending に積むだけ、CPrivacyDirector の Telemetry
 * consent が無ければ TrackEvent/Flush は no-op。アプリ全体で 1 個運用される想定のため
 * 非コピー・非ムーブ。
 */
class CTelemetryDirector {
public:
    /** 空状態で構築する (Init で backend / privacy を注入)。 */
    CTelemetryDirector()  noexcept = default;

    /** 破棄する (pending queue は TArray が解放)。 */
    ~CTelemetryDirector() noexcept = default;

    /** コピー禁止 (アプリ全体で単独運用するため)。 */
    CTelemetryDirector(const CTelemetryDirector&)            = delete;

    /** コピー代入も禁止。 */
    CTelemetryDirector& operator=(const CTelemetryDirector&) = delete;

    /** ムーブ禁止。 */
    CTelemetryDirector(CTelemetryDirector&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CTelemetryDirector& operator=(CTelemetryDirector&&)      = delete;

    /**
     * backend / privacy を注入して初期化する。
     *
     * @details
     * 2 重 Init は backend / privacy 参照を上書きする。pending queue と統計カウンタは
     * 初期化せず、既存イベントを保持したまま backend を差し替える運用を許す。
     * @param backend 送信先の IBackendClient (寿命は呼出側が保証)。
     * @param privacy consent ガードに使う CPrivacyDirector (nullptr 可 = ガードスキップ)。
     */
    void Init(IBackendClient* backend, CPrivacyDirector* privacy = nullptr) noexcept;

    /**
     * pending queue を空にし、backend / privacy 参照を切る。
     *
     * @details
     * 終了時の Flush は呼出側責務で、本関数は送信を試みず確実に no-op で抜ける。
     * 統計カウンタは監査用に残す (Clear で 0 に戻す)。
     */
    void Shutdown() noexcept;

    /**
     * 解析イベントを 1 件 pending queue に投入する。
     *
     * @details
     * Init 前 / event_name / json_payload が nullptr / consent 未取得 / category が
     * disabled のいずれかなら no-op。queue 上限到達時は最古 1 件を drop して末尾追加する。
     * @param event_name イベント名 (nullptr は no-op。非所有 static lifetime)。
     * @param json_payload JSON ペイロード (nullptr は no-op、既定 "{}")。
     * @param priority イベントの重要度ヒント (既定 Info)。
     * @param category カテゴリ名 (フィルタ判定に使う、既定 "general")。
     */
    void TrackEvent(const char*   event_name,
                    const char*   json_payload = "{}",
                    EEventPriority priority     = EEventPriority::Info,
                    const char*   category     = "general") noexcept;

    /**
     * pending を 1 件ずつ backend->SendTelemetry() に流し込む。
     *
     * @details
     * 成功した event は pending から除去し m_SentCount を加算、失敗した event は pending に
     * 残して m_FailedCount を加算する (次回 Flush で再送試行)。backend == nullptr または
     * consent 無しなら全件残したまま return する。stub backend は常に Err を返すため、実質
     * m_FailedCount が増える挙動になる。
     */
    void Flush() noexcept;

    /**
     * 送信待ちの event 件数を返す。
     *
     * @return pending queue のサイズ。
     */
    u32 PendingCount() const noexcept;

    /**
     * 送信に成功した累計 event 件数を返す。
     *
     * @return 送信成功カウンタ。
     */
    u32 SentCount()    const noexcept;

    /**
     * 送信に失敗した累計 event 件数を返す。
     *
     * @return 送信失敗カウンタ。
     */
    u32 FailedCount()  const noexcept;

    /**
     * 毎フレーム呼んで dt を蓄積し、間隔超過で自動 Flush する。
     *
     * @details Init 未呼出時は no-op。NaN / 負の dt は加算しない。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 自動 Flush の間隔 (秒) を設定する。
     *
     * @param sec Flush 間隔の秒 (0 以下は 5 秒既定にクランプ)。
     */
    void SetFlushInterval(f32 sec) noexcept;

    /**
     * category の enable/disable を切り替える (deny list 方式)。
     *
     * @details
     * category == nullptr は no-op。enabled = false で deny に追加、true で deny から削除
     * (= 既定の enabled に戻す)。既定動作は「未登録 = enabled」。
     * @param category 対象カテゴリ名 (非所有 static lifetime)。
     * @param enabled true で許可、false で送信を間引く。
     */
    void EnableCategory(const char* category, bool enabled) noexcept;

    /**
     * pending queue / category filter / 統計カウンタを初期化する。
     *
     * @details Init が行った backend / privacy 参照は保持する (テスト中の再利用可)。
     */
    void Clear() noexcept;

private:
    /**
     * category が現状 enabled かどうかを線形検索で判定する。
     *
     * @details 件数 < 30 想定。未登録カテゴリは既定 enabled。
     * @param category 判定するカテゴリ名 (nullptr は許可扱い)。
     * @return enabled なら true、明示 false 登録なら false。
     */
    bool IsCategoryEnabledInternal(const char* category) const noexcept;

    /**
     * queue 上限到達時に最古 1 件を drop して 1 件分の枠を空ける。
     *
     * @details
     * RemoveAtSwap(0) 経由のため FIFO 順序は保たないが、各 event の timestamp で
     * 利用側ソートできる前提。上限未満なら何もしない。
     */
    void DropOldestIfFull() noexcept;

    /**
     * category 名と enabled flag の組 (m_Filters の要素型)。
     */
    struct FCategoryFilter {
        /** カテゴリ名 (非所有。リテラル想定)。 */
        const char* category = nullptr;

        /** enabled フラグ (false で deny)。 */
        bool        enabled  = true;
    };

    /** pending queue 上限 (100 件で 5 秒 flush 間隔の余裕を確保)。 */
    static constexpr u32 kMaxPending = 100;

    /** 送信待ち event queue。 */
    TArray<FTelemetryEvent>  m_Pending;

    /** category 別の enable/disable フィルタ (deny list)。 */
    TArray<FCategoryFilter>  m_Filters;

    /** 送信先 backend (注入。寿命は呼出側)。 */
    IBackendClient*        m_Backend  = nullptr;

    /** consent ガード用の privacy director (optional 注入)。 */
    CPrivacyDirector*       m_Privacy  = nullptr;

    /** 送信成功した累計 event 件数。 */
    u32  m_SentCount     = 0;

    /** 送信失敗した累計 event 件数。 */
    u32  m_FailedCount   = 0;

    /** 自動 Flush の間隔 (秒、既定 5 秒)。 */
    f32  m_FlushInterval = 5.0f;

    /** 前回 Flush からの経過秒 (Tick で加算)。 */
    f32  m_ElapsedSinceFlush = 0.0f;

    /** Init 済みフラグ。 */
    bool m_Initialized    = false;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FTelemetryDirector = CTelemetryDirector;

} // namespace acs::game
