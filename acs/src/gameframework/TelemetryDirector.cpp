// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar V — TelemetryDirector 実装
//
// 設計上のポイント:
//   ・category 比較は const char* per-byte 比較。STL <string> / <cstring> 不使用。
//     AchievementManager / Entitlement と同じ StrEq を anonymous namespace に
//     再掲する (cross-file の inline helper を増やすよりピラー単位で独立して
//     いるほうが読みやすい)。
//   ・pending queue 上限 100。到達したら RemoveAtSwap(0) で最古 (= index 0) を
//     drop して末尾に新規を積む。FIFO 順序は保証しないが、各 event は
//     timestamp を持つので利用側でソート可能。
//   ・Flush は backend->SendTelemetry() に 1 件ずつ流し、Err なら pending に
//     残す = 次回 Flush で再送される。Phase 2 の stub backend は必ず Err を
//     返すため、実機テストでは _failed_count が増える挙動が観察される。
//   ・consent ガード: PrivacyDirector が attach されていれば
//     HasConsent(EConsentCategory::Telemetry) を毎 TrackEvent / Flush で確認。
//     nullptr 注入時はガードスキップ (テスト用)。
//   ・timestamp は Clock::MillisSinceStartup()。Pillar S の Achievement と同じ
//     起動相対 ms 設計 (Phase 3 で wall clock 切替予定)。
#include "gameframework/TelemetryDirector.h"

#include "gameframework/BackendClient.h"      // IBackendClient::SendTelemetry()
#include "gameframework/PrivacyDirector.h"    // EConsentCategory::Telemetry
#include "platform/Time.h"                    // Clock::MillisSinceStartup()
#include "foundation/Result.h"                // Result<void>::IsErr()

namespace acs::game {

namespace {

// const char* の per-byte 安全比較。AchievementManager.cpp / Entitlement.cpp と同設計。
// どちらかが nullptr なら false を返す (StrEq("a", nullptr) は false)。
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

// =============================================================================
// 内部ユーティリティ
// =============================================================================

bool TelemetryDirector::IsCategoryEnabledInternal(const char* category) const noexcept {
    // category == nullptr は許可扱い (defensive。TrackEvent 側で更に弾く)。
    if (category == nullptr) return true;

    // 線形検索: 明示 false に設定されたカテゴリのみ deny、それ以外は enabled。
    const usize n = _filters.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(_filters[i].category, category)) {
            return _filters[i].enabled;
        }
    }
    // 未登録カテゴリは既定 enabled。
    return true;
}

void TelemetryDirector::DropOldestIfFull() noexcept {
    // 上限未満なら何もしない。
    if (_pending.Size() < static_cast<usize>(kMaxPending)) return;

    // 上限に達している = 最古 1 件 (index 0) を捨てて 1 枠空ける。
    // RemoveAtSwap は末尾要素を index 0 にムーブするため FIFO 順序は壊れる
    // が、event 側 timestamp で利用側ソート可能。
    ACS_LOG_WARN("TelemetryDirector: pending queue full (%u), dropping oldest event",
                 kMaxPending);
    _pending.RemoveAtSwap(0);
}

// =============================================================================
// 初期化 / 終了
// =============================================================================

void TelemetryDirector::Init(IBackendClient* backend, PrivacyDirector* privacy) noexcept {
    _backend            = backend;
    _privacy            = privacy;
    _initialized        = true;
    _elapsed_since_flush = 0.0f;
    // 統計と pending queue は Init() で初期化しない。
    // 既に投入された event を保持したまま backend を差し替える運用を許す。
}

void TelemetryDirector::Shutdown() noexcept {
    // pending queue を空にし、参照を切る。送信は試みない (呼出側が事前に Flush)。
    _pending.Clear();
    _filters.Clear();
    _backend            = nullptr;
    _privacy            = nullptr;
    _initialized        = false;
    _elapsed_since_flush = 0.0f;
    // _sent_count / _failed_count は監査用にリセットしない (Clear() で 0 に戻す)。
}

// =============================================================================
// event 投入
// =============================================================================

void TelemetryDirector::TrackEvent(const char*   event_name,
                                   const char*   json_payload,
                                   EventPriority priority,
                                   const char*   category) noexcept {
    // Init() 前 / event_name nullptr / json_payload nullptr は no-op。
    if (!_initialized) return;
    if (event_name == nullptr || json_payload == nullptr) return;

    // consent ガード: privacy attach されていて Telemetry consent が無ければ無視。
    // GDPR 要件 (同意取得前に追跡を開始しない)。
    if (_privacy != nullptr &&
        !_privacy->HasConsent(EConsentCategory::Telemetry)) {
        return;
    }

    // category フィルタ: 明示 false が設定されていれば落とす。
    if (!IsCategoryEnabledInternal(category)) {
        return;
    }

    // 上限到達なら最古 1 件を drop してから新規を積む。
    DropOldestIfFull();

    TelemetryEvent e{};
    e.event_name   = event_name;
    // category nullptr は "general" 既定に丸める (TrackEvent の default 引数で
    // 通常は "general" になるが、明示 nullptr 渡し対策)。
    e.category     = category != nullptr ? category : "general";
    e.json_payload = json_payload;
    e.priority     = priority;
    e.timestamp    = Clock::MillisSinceStartup();

    _pending.PushBack(e);
}

// =============================================================================
// Flush
// =============================================================================

void TelemetryDirector::Flush() noexcept {
    // Init() 前 / backend 無し は no-op (pending はそのまま温存)。
    if (!_initialized || _backend == nullptr) {
        // タイマーだけはリセット (次回試行までに再度待たせる)。
        _elapsed_since_flush = 0.0f;
        return;
    }

    // consent ガード: 投入時に弾いているが、TrackEvent 後に Revoke された場合に
    // 備えて Flush でも再確認。consent 無しなら送信せずタイマーだけリセット。
    if (_privacy != nullptr &&
        !_privacy->HasConsent(EConsentCategory::Telemetry)) {
        _elapsed_since_flush = 0.0f;
        return;
    }

    // pending を末尾から走査して送信。成功 → 該当 index を RemoveAtSwap で除去、
    // 失敗 → 残す。逆順走査により RemoveAtSwap で起こる末尾→該当位置のムーブ
    // で「未走査側を壊す」事故を防ぐ (走査済み末尾が消えるだけ)。
    for (usize i = _pending.Size(); i-- > 0;) {
        const TelemetryEvent& e = _pending[i];
        Result<void> r = _backend->SendTelemetry(e.event_name, e.json_payload);
        if (r.IsErr()) {
            // 失敗カウンタを上げ、pending には残す = 次 Flush で再送試行。
            // stub backend では常にこの分岐 (NotImplemented) を踏む。
            ++_failed_count;
            // ログは多発するので Warn ではなく Debug に抑える。
            ACS_LOG_DEBUG("TelemetryDirector: SendTelemetry('%s') failed (will retry)",
                          e.event_name != nullptr ? e.event_name : "<null>");
        } else {
            // 送信成功。pending から除去。RemoveAtSwap は末尾を i 位置に
            // ムーブする = 順序は保たれないが、本ループが逆順走査なので
            // 走査済み末尾だけ消える形になり、未走査側に影響しない。
            ++_sent_count;
            _pending.RemoveAtSwap(i);
        }
    }

    _elapsed_since_flush = 0.0f;
}

// =============================================================================
// 統計
// =============================================================================

u32 TelemetryDirector::PendingCount() const noexcept {
    return static_cast<u32>(_pending.Size());
}

u32 TelemetryDirector::SentCount() const noexcept {
    return _sent_count;
}

u32 TelemetryDirector::FailedCount() const noexcept {
    return _failed_count;
}

// =============================================================================
// Tick / 設定
// =============================================================================

void TelemetryDirector::Tick(f32 dt) noexcept {
    if (!_initialized) return;
    // 異常値 (NaN / 負値) ガード。負の dt は 0 扱いに丸める。
    if (dt > 0.0f) {
        _elapsed_since_flush += dt;
    }
    // 間隔超過なら自動 Flush。pending が空でも Flush 自体は安価。
    if (_elapsed_since_flush >= _flush_interval) {
        Flush();
    }
}

void TelemetryDirector::SetFlushInterval(f32 sec) noexcept {
    // 0 以下は 5 秒既定に丸める (= 暴走防止)。
    _flush_interval = sec > 0.0f ? sec : 5.0f;
}

// =============================================================================
// category フィルタ
// =============================================================================

void TelemetryDirector::EnableCategory(const char* category, bool enabled) noexcept {
    if (category == nullptr) return;

    // 既存登録があれば in-place 更新。
    const usize n = _filters.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(_filters[i].category, category)) {
            _filters[i].enabled = enabled;
            return;
        }
    }

    // 未登録。enabled = true (= 既定) なら登録不要 (Array サイズを増やさない)。
    // enabled = false (= deny) のみ新規登録する。
    if (!enabled) {
        CategoryFilter f;
        f.category = category;
        f.enabled  = false;
        _filters.PushBack(f);
    }
}

// =============================================================================
// デバッグ
// =============================================================================

void TelemetryDirector::Clear() noexcept {
    _pending.Clear();
    _filters.Clear();
    _sent_count          = 0;
    _failed_count        = 0;
    _elapsed_since_flush = 0.0f;
    // backend / privacy / _initialized は維持 (テスト中の再利用を許す)。
}

} // namespace acs::game
