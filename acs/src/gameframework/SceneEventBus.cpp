// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — FSceneEventBus 実装 (Phase 1 polish)
#include "gameframework/SceneEventBus.h"

namespace acs::game {

u32 FSceneEventBus::Subscribe(EventId id, HandlerFn fn, void* user) noexcept {
    if (fn == nullptr) {
        ACS_LOG_WARN("FSceneEventBus::Subscribe: null handler for event id=%u ignored", id.value);
        return 0u;
    }

    // 既に inactive になっている Entry があれば再利用してメモリを節約。
    // Publish 中の再エントランシ安全性のため、Entry の物理削除はしない設計。
    for (usize i = 0; i < m_Entries.Size(); ++i) {
        Entry& e = m_Entries[i];
        if (!e.active) {
            // handle は新規発行 (古い handle で Unsubscribe されても誤爆しないように)
            const u32 h = m_NextHandle++;
            if (m_NextHandle == 0u) m_NextHandle = 1u;  // wrap-around 時に 0 (invalid) を避ける
            e.id     = id;
            e.fn     = fn;
            e.user   = user;
            e.handle = h;
            e.active = true;
            return h;
        }
    }

    // 空きが無ければ末尾に追加
    const u32 h = m_NextHandle++;
    if (m_NextHandle == 0u) m_NextHandle = 1u;
    Entry e;
    e.id     = id;
    e.fn     = fn;
    e.user   = user;
    e.handle = h;
    e.active = true;
    m_Entries.PushBack(e);
    return h;
}

void FSceneEventBus::Unsubscribe(u32 handle) noexcept {
    if (handle == 0u) return;  // invalid handle は静かに無視 (RAII 解除パスで頻出)
    for (usize i = 0; i < m_Entries.Size(); ++i) {
        Entry& e = m_Entries[i];
        if (e.handle == handle && e.active) {
            e.active = false;
            // fn / user は debug 観察用に残しておく (どうせ active=false で見ない)
            return;
        }
    }
    // 既に Unsubscribe 済 / 未知 handle: no-op (Warn は出さない、二重解除は良くある)
}

void FSceneEventBus::Publish(EventId id, const void* payload, u32 payload_size) noexcept {
    // 走査範囲を呼び出し時点で固定。Publish 中に Subscribe された Entry は
    // 次回以降の Publish で呼ばれる (循環 publish の防止にもなる)。
    const usize snapshot_size = m_Entries.Size();
    for (usize i = 0; i < snapshot_size; ++i) {
        Entry& e = m_Entries[i];
        if (!e.active) continue;
        if (e.id != id) continue;
        // ハンドラ実行中に Unsubscribe / 別 event の Subscribe が走っても
        // m_Entries の物理削除はしない設計なので、参照 e は有効なまま。
        // ただし Subscribe が再 alloc を起こすと参照が無効化されるリスクが
        // あるため、fn / user / payload を local にコピーしてから呼ぶ。
        const HandlerFn fn   = e.fn;
        void* const     usr  = e.user;
        if (fn != nullptr) {
            fn(usr, payload, payload_size);
        }
    }
}

u32 FSceneEventBus::SubscriberCount(EventId id) const noexcept {
    u32 count = 0u;
    for (usize i = 0; i < m_Entries.Size(); ++i) {
        const Entry& e = m_Entries[i];
        if (e.active && e.id == id) ++count;
    }
    return count;
}

void FSceneEventBus::ClearAll() noexcept {
    m_Entries.Clear();
    // m_NextHandle はリセットしない: ClearAll 後も以前払い出した handle と
    // 衝突しないように単調増加を維持。
}

} // namespace acs::game
