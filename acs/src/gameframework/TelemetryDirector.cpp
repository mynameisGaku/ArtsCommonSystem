// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar V — CTelemetryDirector 実装
//
// 設計上のポイント:
//   ・category 比較は const char* per-byte 比較。STL <string> / <cstring> 不使用。
//     CAchievementManager / FEntitlement と同じ StrEq を anonymous namespace に
//     再掲する (cross-file の inline helper を増やすよりピラー単位で独立して
//     いるほうが読みやすい)。
//   ・pending queue 上限 100。到達したら RemoveAtSwap(0) で最古 (= index 0) を
//     drop して末尾に新規を積む。FIFO 順序は保証しないが、各 event は
//     timestamp を持つので利用側でソート可能。
//   ・Flush は backend->SendTelemetry() に 1 件ずつ流し、Err なら pending に
//     残す = 次回 Flush で再送される。stub backend は必ず Err を
//     返すため、実機テストでは m_FailedCount が増える挙動が観察される。
//   ・consent ガード: CPrivacyDirector が attach されていれば
//     HasConsent(EConsentCategory::Telemetry) を毎 TrackEvent / Flush で確認。
//     nullptr 注入時はガードスキップ (テスト用)。
//   ・timestamp は Clock::MillisSinceStartup()。Pillar S の Achievement と同じ
//     起動相対 ms 設計 (将来 wall clock 切替予定)。
#include "gameframework/TelemetryDirector.h"

#include "gameframework/BackendClient.h"      // IBackendClient::SendTelemetry()
#include "gameframework/PrivacyDirector.h"    // EConsentCategory::Telemetry
#include "platform/Time.h"                    // Clock::MillisSinceStartup()
#include "foundation/Result.h"                // Result<void>::IsErr()

namespace acs::game {

namespace {

/**
 * const char* の per-byte 安全比較を行う (CAchievementManager.cpp / FEntitlement.cpp と同設計)。
 *
 * @details どちらかが nullptr なら false を返す (StrEq("a", nullptr) は false)。
 * @param a 比較する文字列 1。
 * @param b 比較する文字列 2。
 * @return 内容が完全一致すれば true。
 */
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

} // namespace

/** category が明示 deny されていなければ enabled を返す (未登録は既定 enabled)。 */
bool CTelemetryDirector::IsCategoryEnabledInternal(const char* category) const noexcept {
    // category == nullptr は許可扱い (defensive。TrackEvent 側で更に弾く)。
    if (category == nullptr) return true;

    // 線形検索: 明示 false に設定されたカテゴリのみ deny、それ以外は enabled。
    const usize n = m_Filters.Num();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Filters[i].category, category)) {
            return m_Filters[i].enabled;
        }
    }
    // 未登録カテゴリは既定 enabled。
    return true;
}

/** pending queue が上限に達していれば最古 1 件 (index 0) を drop する。 */
void CTelemetryDirector::DropOldestIfFull() noexcept {
    // 上限未満なら何もしない。
    if (m_Pending.Num() < static_cast<usize>(kMaxPending)) return;

    // 上限に達している = 最古 1 件 (index 0) を捨てて 1 枠空ける。
    // RemoveAtSwap は末尾要素を index 0 にムーブするため FIFO 順序は壊れる
    // が、event 側 timestamp で利用側ソート可能。
    ACS_LOG_WARN("CTelemetryDirector: pending queue full (%u), dropping oldest event",
                 kMaxPending);
    m_Pending.RemoveAtSwap(0);
}

/** backend / privacy 参照を差し込んで初期化する (pending と統計は温存)。 */
void CTelemetryDirector::Init(IBackendClient* backend, CPrivacyDirector* privacy) noexcept {
    m_Backend            = backend;
    m_Privacy            = privacy;
    m_Initialized        = true;
    m_ElapsedSinceFlush = 0.0f;
    // 統計と pending queue は Init() で初期化しない。
    // 既に投入された event を保持したまま backend を差し替える運用を許す。
}

/** pending queue とフィルタを空にし、参照を切る (送信は試みない)。 */
void CTelemetryDirector::Shutdown() noexcept {
    // pending queue を空にし、参照を切る。送信は試みない (呼出側が事前に Flush)。
    m_Pending.Reset();
    m_Filters.Reset();
    m_Backend            = nullptr;
    m_Privacy            = nullptr;
    m_Initialized        = false;
    m_ElapsedSinceFlush = 0.0f;
    // m_SentCount / m_FailedCount は監査用にリセットしない (Clear() で 0 に戻す)。
}

/** consent / category フィルタを通過した event を pending queue に積む。 */
void CTelemetryDirector::TrackEvent(const char*   event_name,
                                   const char*   json_payload,
                                   EEventPriority priority,
                                   const char*   category) noexcept {
    // Init() 前 / event_name nullptr / json_payload nullptr は no-op。
    if (!m_Initialized) return;
    if (event_name == nullptr || json_payload == nullptr) return;

    // consent ガード: privacy attach されていて Telemetry consent が無ければ無視。
    // GDPR 要件 (同意取得前に追跡を開始しない)。
    if (m_Privacy != nullptr &&
        !m_Privacy->HasConsent(EConsentCategory::Telemetry)) {
        return;
    }

    // category フィルタ: 明示 false が設定されていれば落とす。
    if (!IsCategoryEnabledInternal(category)) {
        return;
    }

    // 上限到達なら最古 1 件を drop してから新規を積む。
    DropOldestIfFull();

    FTelemetryEvent e{};
    e.event_name   = event_name;
    // category nullptr は "general" 既定に丸める (TrackEvent の default 引数で
    // 通常は "general" になるが、明示 nullptr 渡し対策)。
    e.category     = category != nullptr ? category : "general";
    e.json_payload = json_payload;
    e.priority     = priority;
    e.timestamp    = CClock::MillisSinceStartup();

    m_Pending.Add(e);
}

/** pending を逆順に backend へ送信し、成功分のみ除去する (失敗分は次回再送)。 */
void CTelemetryDirector::Flush() noexcept {
    // Init() 前 / backend 無し は no-op (pending はそのまま温存)。
    if (!m_Initialized || m_Backend == nullptr) {
        // タイマーだけはリセット (次回試行までに再度待たせる)。
        m_ElapsedSinceFlush = 0.0f;
        return;
    }

    // consent ガード: 投入時に弾いているが、TrackEvent 後に Revoke された場合に
    // 備えて Flush でも再確認。consent 無しなら送信せずタイマーだけリセット。
    if (m_Privacy != nullptr &&
        !m_Privacy->HasConsent(EConsentCategory::Telemetry)) {
        m_ElapsedSinceFlush = 0.0f;
        return;
    }

    // pending を末尾から走査して送信。成功 → 該当 index を RemoveAtSwap で除去、
    // 失敗 → 残す。逆順走査により RemoveAtSwap で起こる末尾→該当位置のムーブ
    // で「未走査側を壊す」事故を防ぐ (走査済み末尾が消えるだけ)。
    for (usize i = m_Pending.Num(); i-- > 0;) {
        const FTelemetryEvent& e = m_Pending[i];
        TResult<void> r = m_Backend->SendTelemetry(e.event_name, e.json_payload);
        if (r.IsErr()) {
            // 失敗カウンタを上げ、pending には残す = 次 Flush で再送試行。
            // stub backend では常にこの分岐 (NotImplemented) を踏む。
            ++m_FailedCount;
            // ログは多発するので Warn ではなく Debug に抑える。
            ACS_LOG_DEBUG("CTelemetryDirector: SendTelemetry('%s') failed (will retry)",
                          e.event_name != nullptr ? e.event_name : "<null>");
        } else {
            // 送信成功。pending から除去。RemoveAtSwap は末尾を i 位置に
            // ムーブする = 順序は保たれないが、本ループが逆順走査なので
            // 走査済み末尾だけ消える形になり、未走査側に影響しない。
            ++m_SentCount;
            m_Pending.RemoveAtSwap(i);
        }
    }

    m_ElapsedSinceFlush = 0.0f;
}

/** 現在 pending queue に溜まっている event 数を返す。 */
u32 CTelemetryDirector::PendingCount() const noexcept {
    return static_cast<u32>(m_Pending.Num());
}

/** 送信に成功した累計 event 数を返す。 */
u32 CTelemetryDirector::SentCount() const noexcept {
    return m_SentCount;
}

/** 送信に失敗した累計 event 数を返す。 */
u32 CTelemetryDirector::FailedCount() const noexcept {
    return m_FailedCount;
}

/** 経過時間を積算し、flush 間隔を超えたら自動 Flush する。 */
void CTelemetryDirector::Tick(f32 dt) noexcept {
    if (!m_Initialized) return;
    // 異常値 (NaN / 負値) ガード。負の dt は 0 扱いに丸める。
    if (dt > 0.0f) {
        m_ElapsedSinceFlush += dt;
    }
    // 間隔超過なら自動 Flush。pending が空でも Flush 自体は安価。
    if (m_ElapsedSinceFlush >= m_FlushInterval) {
        Flush();
    }
}

/** 自動 Flush の間隔を設定する (0 以下は 5 秒既定に丸める)。 */
void CTelemetryDirector::SetFlushInterval(f32 sec) noexcept {
    // 0 以下は 5 秒既定に丸める (= 暴走防止)。
    m_FlushInterval = sec > 0.0f ? sec : 5.0f;
}

/** category の許可フラグを設定する (deny のみフィルタへ登録、enabled は登録不要)。 */
void CTelemetryDirector::EnableCategory(const char* category, bool enabled) noexcept {
    if (category == nullptr) return;

    // 既存登録があれば in-place 更新。
    const usize n = m_Filters.Num();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Filters[i].category, category)) {
            m_Filters[i].enabled = enabled;
            return;
        }
    }

    // 未登録。enabled = true (= 既定) なら登録不要 (TArray サイズを増やさない)。
    // enabled = false (= deny) のみ新規登録する。
    if (!enabled) {
        FCategoryFilter f;
        f.category = category;
        f.enabled  = false;
        m_Filters.Add(f);
    }
}

/** pending・フィルタ・統計をリセットする (backend / privacy 参照は維持)。 */
void CTelemetryDirector::Clear() noexcept {
    m_Pending.Reset();
    m_Filters.Reset();
    m_SentCount          = 0;
    m_FailedCount        = 0;
    m_ElapsedSinceFlush = 0.0f;
    // backend / privacy / m_Initialized は維持 (テスト中の再利用を許す)。
}

} // namespace acs::game
