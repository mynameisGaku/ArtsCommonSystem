// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K — FHotReload 実装 (Phase 2 seam)
//
// 設計のポイント (詳細はヘッダ参照):
//   ・watched paths / callbacks / pending events の 3 つの TArray を持つレジストリ。
//   ・実 FS watcher の起動は Phase K-3 に委ね、Tick() は no-op。
//   ・FIFO 順を保つため pending events は ConsumeNextEvent で「先頭取り出し +
//     shift-left」する (swap-remove は順序を壊すので不採用)。pending size は
//     <= 数十想定なので shift コストは無視できる。
//   ・Ship build (`ACS_GAME_SHIPPING` 定義時) では全 method を no-op に。
//     ヘッダ側で member もろとも `#ifdef` で消えるため、`.cpp` 側も同じガードで
//     空の実装にする (シンボル定義は残し、呼び出し側コードが #ifdef だらけに
//     ならないようにする)。

#include "gameframework/HotReload.h"

#include "foundation/Log.h"

#ifndef ACS_GAME_SHIPPING
#include <cstring>   // strcmp
#endif

namespace acs::game {

#ifndef ACS_GAME_SHIPPING

// ============================================================================
// ライフサイクル
// ============================================================================

void HotReloadWatcher::Init() noexcept {
    // Phase 2 では何もしない (予約点)。
    //
    // TODO(Phase K-3):
    //   ・Windows: 各 watched directory に対し ReadDirectoryChangesW を
    //     OVERLAPPED + I/O completion port で発行する。
    //   ・POSIX: inotify_init1(IN_NONBLOCK) でディスクリプタを開き、
    //     後の WatchDirectory / WatchFile で inotify_add_watch を呼ぶ。
    //   ・macOS: FSEventStreamCreate を構成する。
    //   ・いずれも `IFileSystemWatcher` 抽象越しに呼び、本 class はその所有を
    //     `acs::TUniquePtr<IFileSystemWatcher>` で持つ予定。
    //
    // 多重呼び出し可: 何度呼んでも副作用なし。
}

void HotReloadWatcher::Shutdown() noexcept {
    // watched paths / callbacks / pending events を全クリア。
    // path 文字列自体は caller 所有 (借用) なので Free しない。
    // callbacks / events は POD なので個別後始末不要。
    m_WatchedPaths.Clear();
    m_Callbacks.Clear();
    m_PendingEvents.Clear();

    // TODO(Phase K-3): OS watcher ハンドル (ReadDirectoryChangesW の
    // OVERLAPPED / inotify fd / FSEventStream) をここで閉じる。
}

// ============================================================================
// 監視対象登録
// ============================================================================

void HotReloadWatcher::WatchDirectory(const char* dir_path, bool recursive) noexcept {
    if (dir_path == nullptr || dir_path[0] == '\0') {
        ACS_LOG_WARN("HotReloadWatcher::WatchDirectory: null/empty path (ignored)");
        return;
    }

    // 重複登録は no-op (二重 dispatch / Unwatch の対称性破壊を防ぐ)。
    for (usize i = 0; i < m_WatchedPaths.Size(); ++i) {
        if (std::strcmp(m_WatchedPaths[i], dir_path) == 0) {
            return;
        }
    }
    m_WatchedPaths.PushBack(dir_path);

    // Phase 2: recursive フラグは保持しない (パス文字列だけを記録)。
    // Phase K-3 で「path → recursive 希望 / OS watcher ハンドル」を持つ
    // WatchEntry 構造体に昇格して、recursive を OS API に伝える予定。
    (void)recursive;
}

void HotReloadWatcher::WatchFile(const char* file_path) noexcept {
    if (file_path == nullptr || file_path[0] == '\0') {
        ACS_LOG_WARN("HotReloadWatcher::WatchFile: null/empty path (ignored)");
        return;
    }

    // 重複登録は no-op (WatchDirectory と同じ理由)。
    for (usize i = 0; i < m_WatchedPaths.Size(); ++i) {
        if (std::strcmp(m_WatchedPaths[i], file_path) == 0) {
            return;
        }
    }
    m_WatchedPaths.PushBack(file_path);
}

void HotReloadWatcher::Unwatch(const char* path) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return;  // null/empty は no-op (境界条件吸収)
    }

    // 完全一致 1 件を削除。順序は保証しないので swap-remove (RemoveAtSwap)。
    // watched paths は描画レイアウト等の順序依存がないため OK。
    const usize n = m_WatchedPaths.Size();
    for (usize i = 0; i < n; ++i) {
        if (std::strcmp(m_WatchedPaths[i], path) == 0) {
            m_WatchedPaths.RemoveAtSwap(i);
            return;
        }
    }
    // 未登録の Unwatch は no-op (呼び出し側のライフサイクルミスを致命化しない)。
}

// ============================================================================
// コールバック登録
// ============================================================================

void HotReloadWatcher::RegisterCallback(HotReloadCallback cb, void* user) noexcept {
    if (cb == nullptr) {
        ACS_LOG_WARN("HotReloadWatcher::RegisterCallback: null cb (ignored)");
        return;
    }

    // (cb, user) ペア重複は no-op。誤って二重 register しても二重 dispatch を防ぐ。
    for (usize i = 0; i < m_Callbacks.Size(); ++i) {
        if (m_Callbacks[i].cb == cb && m_Callbacks[i].user == user) {
            return;
        }
    }
    CallbackEntry e{};
    e.cb   = cb;
    e.user = user;
    m_Callbacks.PushBack(e);
}

// ============================================================================
// 駆動
// ============================================================================

void HotReloadWatcher::Tick(f32 dt) noexcept {
    (void)dt;

    // Phase 2 スタブ: 何もしない。
    //
    // TODO(Phase K-3):
    //   ・Windows: GetQueuedCompletionStatus で ReadDirectoryChangesW の
    //     完了通知を取り、FILE_NOTIFY_INFORMATION から filename と Action を抽出、
    //     HotReloadEvent を構成して m_PendingEvents に push、続けて再度
    //     ReadDirectoryChangesW を発行 (one-shot を継続化)。
    //   ・POSIX: read(inotify_fd, ...) で inotify_event を取り、IN_MODIFY /
    //     IN_DELETE を HotReloadEvent.removed にマップ。
    //   ・event 構成後は登録済み callback に dispatch (ループ内で
    //     ConsumeNextEvent するか、内部で m_Callbacks を直接回す。Phase K-3 で
    //     決定)。
    //   ・debounce: 同一 path / 100ms 以内の連続変更は最後の 1 件にまとめる。
    //     これは「save → swap-rename」型エディタの揺らぎを吸収するため。
    //
    // 現状は m_PendingEvents への push を本ヘッダ層からは行わない。
    // (テスト用 / bridge コードから内部 helper 経由で push する手段は Phase K-3
    //  で追加する予定。)
}

// ============================================================================
// 状態取得
// ============================================================================

u32 HotReloadWatcher::WatchedCount() const noexcept {
    return static_cast<u32>(m_WatchedPaths.Size());
}

u32 HotReloadWatcher::PendingEventCount() const noexcept {
    return static_cast<u32>(m_PendingEvents.Size());
}

bool HotReloadWatcher::ConsumeNextEvent(HotReloadEvent& out) noexcept {
    if (m_PendingEvents.Size() == 0) {
        return false;  // 空なら out は触らず false
    }

    // FIFO 順を保つため先頭を取り出して shift-left する。
    // pending size は <= 数十想定なので shift コストは実用上無視できる。
    // (swap-remove は順序を壊すため不採用 — hot reload は「最初に届いた変更を
    //  最初に処理する」のが直感的)
    out = m_PendingEvents[0];
    for (usize i = 1; i < m_PendingEvents.Size(); ++i) {
        m_PendingEvents[i - 1] = m_PendingEvents[i];
    }
    m_PendingEvents.PopBack();
    return true;
}

void HotReloadWatcher::ClearEvents() noexcept {
    // pending events は POD (path は借用 const char*) なので個別後始末不要。
    m_PendingEvents.Clear();
}

#else // ACS_GAME_SHIPPING

// ============================================================================
// Ship build no-op 実装
// ============================================================================
// 全 method を no-op に。シンボル定義は残し、呼び出し側コードが #ifdef だらけに
// ならないようにする。戻り値は安全な既定値 (0 / false)。

void HotReloadWatcher::Init() noexcept {}
void HotReloadWatcher::Shutdown() noexcept {}
void HotReloadWatcher::WatchDirectory(const char* /*dir_path*/, bool /*recursive*/) noexcept {}
void HotReloadWatcher::WatchFile(const char* /*file_path*/) noexcept {}
void HotReloadWatcher::Unwatch(const char* /*path*/) noexcept {}
void HotReloadWatcher::RegisterCallback(HotReloadCallback /*cb*/, void* /*user*/) noexcept {}
void HotReloadWatcher::Tick(f32 /*dt*/) noexcept {}
u32  HotReloadWatcher::WatchedCount() const noexcept { return 0; }
u32  HotReloadWatcher::PendingEventCount() const noexcept { return 0; }
bool HotReloadWatcher::ConsumeNextEvent(HotReloadEvent& /*out*/) noexcept { return false; }
void HotReloadWatcher::ClearEvents() noexcept {}

#endif // ACS_GAME_SHIPPING

} // namespace acs::game
