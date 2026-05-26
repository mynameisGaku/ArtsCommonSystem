// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar S — WorkshopBridge 実装 (Stub のみ)
//
// 本 .cpp では `IWorkshopBridge` の実 SDK 結合は提供せず、Steamworks UGC 未統合
// ビルドでも常に使える `WorkshopBridgeStub` のみを実装する。実 SDK を使う
// `GoldenWorkshopBridge` は Steamworks SDK 依存を含むため、別モジュール (将来の
// `acs_steamworks` 等) で独立に実装し、本ファイルには持ち込まない。
//
// 設計のポイント:
//   ・Stub は副作用ゼロ。`Init()` は単に `_initialized = true` を立てるのみで、
//     `IsAvailable()` は header inline で常に false を返す (UI 側で Workshop
//     ボタンを非表示にする判定用)。
//   ・全 publish / subscribe / download / query 系は ACS_ERR(Generic,
//     kSubWorkshopNotImplemented, ...) を返す。SteamworksBridge と subcode 空間が
//     重ならないよう 1100 番台を使う。
//   ・`GetDownloadProgress()` は常に -1.0f を返す (「ダウンロード中ではない /
//     不明」の意味)。
//   ・`GetWorkshopStub()` は Meyer's singleton。スレッド初回構築は C++11 以降の
//     規格で保証されているため、追加同期は不要。

#include "gameframework/WorkshopBridge.h"

#include "foundation/Error.h"

namespace acs::game {

// ---- Stub: Init / Shutdown ------------------------------------------------

TResult<void> WorkshopBridgeStub::Init() noexcept {
    // 多重 Init は明示的に許容する。実 SDK の SteamUGC()->Init() 相当は本来
    // 失敗パスがあるが、Stub はテスト容易性のため常に成功。
    _initialized = true;
    return Ok();
}

void WorkshopBridgeStub::Shutdown() noexcept {
    // Init() 前に呼ばれても安全。
    _initialized = false;
}

// ---- Stub: Publish (Create / Update) ------------------------------------

TResult<u64> WorkshopBridgeStub::CreateItem(const char* title, const char* content_path) noexcept {
    (void)title;
    (void)content_path;
    if (!_initialized) {
        return TResult<u64>(ACS_ERR(Generic, kSubWorkshopNotInitialized,
                                   "WorkshopBridgeStub::CreateItem called before Init()"));
    }
    return TResult<u64>(ACS_ERR(Generic, kSubWorkshopNotImplemented,
                               "WorkshopBridgeStub: CreateItem is not implemented (link real SDK)"));
}

TResult<void> WorkshopBridgeStub::UpdateItem(u64 item_id,
                                            const char* content_path,
                                            const char* change_note) noexcept {
    (void)item_id;
    (void)content_path;
    (void)change_note;
    if (!_initialized) {
        return ACS_ERR(Generic, kSubWorkshopNotInitialized,
                       "WorkshopBridgeStub::UpdateItem called before Init()");
    }
    return ACS_ERR(Generic, kSubWorkshopNotImplemented,
                   "WorkshopBridgeStub: UpdateItem is not implemented (link real SDK)");
}

// ---- Stub: Query -------------------------------------------------------

TResult<WorkshopItem> WorkshopBridgeStub::QueryItem(u64 item_id) noexcept {
    (void)item_id;
    if (!_initialized) {
        return TResult<WorkshopItem>(ACS_ERR(Generic, kSubWorkshopNotInitialized,
                                            "WorkshopBridgeStub::QueryItem called before Init()"));
    }
    return TResult<WorkshopItem>(ACS_ERR(Generic, kSubWorkshopNotImplemented,
                                        "WorkshopBridgeStub: QueryItem is not implemented (link real SDK)"));
}

TResult<u32> WorkshopBridgeStub::QuerySubscribedCount() noexcept {
    if (!_initialized) {
        return TResult<u32>(ACS_ERR(Generic, kSubWorkshopNotInitialized,
                                   "WorkshopBridgeStub::QuerySubscribedCount called before Init()"));
    }
    return TResult<u32>(ACS_ERR(Generic, kSubWorkshopNotImplemented,
                               "WorkshopBridgeStub: QuerySubscribedCount is not implemented (link real SDK)"));
}

// ---- Stub: Subscribe / Download ----------------------------------------

TResult<void> WorkshopBridgeStub::SubscribeItem(u64 item_id) noexcept {
    (void)item_id;
    if (!_initialized) {
        return ACS_ERR(Generic, kSubWorkshopNotInitialized,
                       "WorkshopBridgeStub::SubscribeItem called before Init()");
    }
    return ACS_ERR(Generic, kSubWorkshopNotImplemented,
                   "WorkshopBridgeStub: SubscribeItem is not implemented (link real SDK)");
}

TResult<void> WorkshopBridgeStub::UnsubscribeItem(u64 item_id) noexcept {
    (void)item_id;
    if (!_initialized) {
        return ACS_ERR(Generic, kSubWorkshopNotInitialized,
                       "WorkshopBridgeStub::UnsubscribeItem called before Init()");
    }
    return ACS_ERR(Generic, kSubWorkshopNotImplemented,
                   "WorkshopBridgeStub: UnsubscribeItem is not implemented (link real SDK)");
}

TResult<void> WorkshopBridgeStub::DownloadItem(u64 item_id) noexcept {
    (void)item_id;
    if (!_initialized) {
        return ACS_ERR(Generic, kSubWorkshopNotInitialized,
                       "WorkshopBridgeStub::DownloadItem called before Init()");
    }
    return ACS_ERR(Generic, kSubWorkshopNotImplemented,
                   "WorkshopBridgeStub: DownloadItem is not implemented (link real SDK)");
}

f32 WorkshopBridgeStub::GetDownloadProgress(u64 item_id) noexcept {
    (void)item_id;
    // 「ダウンロード中ではない / 不明」を表すセンチネル値。
    // 実 SDK 実装では SteamUGC()->GetItemDownloadInfo() を投げて [0, 1] を返す。
    return -1.0f;
}

// ---- Stub: Tick ----------------------------------------------------------

void WorkshopBridgeStub::Tick(f32 dt) noexcept {
    (void)dt;  // Stub は callback pump を持たないので何もしない
}

// ---- static singleton ---------------------------------------------------

IWorkshopBridge& GetWorkshopStub() noexcept {
    // C++11 以降、関数スコープ static の初期化は thread-safe。
    static WorkshopBridgeStub _instance;
    return _instance;
}

} // namespace acs::game
