// SPDX-License-Identifier: Apache-2.0
// MessageBroker 実装
#include "event/MessageBroker.h"

namespace acs {

/** 全チャンネルを解放して破棄する。 */
MessageBroker::~MessageBroker() noexcept {
    Clear();
}

/** 全購読とチャンネルを解放して初期状態へ戻す。 */
void MessageBroker::Clear() noexcept
{
    for (usize i = 0; i < m_Channels.Size(); ++i) {
        if (m_Channels[i]) {
            delete m_Channels[i];
            m_Channels[i] = nullptr;
        }
    }
    m_Channels = TArray<Channel*>{*m_Channels.GetAllocator()};
    ++m_GenerationSeed;
}

/** EventTypeId に対応するチャンネルを返す (create=true なら未生成時に確保)。 */
MessageBroker::Channel* MessageBroker::GetChannel(EventTypeId id, bool create) noexcept {
    if (id >= m_Channels.Size()) {
        if (!create) return nullptr;
        const usize old = m_Channels.Size();
        m_Channels.Resize(id + 1);
        for (usize i = old; i <= id; ++i) m_Channels[i] = nullptr;
    }
    if (!m_Channels[id] && create) {
        m_Channels[id] = new Channel();
        if (m_Channels[id]) m_Channels[id]->generation_seed = m_GenerationSeed;
    }
    return m_Channels[id];
}

/** 型消去された購読登録の実体 (空き slot を再利用または新規確保し世代を進める)。 */
SubscriptionHandle MessageBroker::SubscribeRaw(EventTypeId channel,
                                                MessageCallback cb, void* user) noexcept {
    if (!cb) return kInvalidSubscription;
    Channel* ch = GetChannel(channel, true);
    if (!ch) return kInvalidSubscription;

    u32 idx;
    if (ch->free_indices.Size() > 0) {
        idx = ch->free_indices[ch->free_indices.Size() - 1];
        ch->free_indices.PopBack();
    } else {
        idx = static_cast<u32>(ch->slots.Size());
        ch->slots.PushBack(Slot{});
        ch->slots[idx].generation = ch->generation_seed;
    }
    Slot& s = ch->slots[idx];
    if (s.id == 0) s.id = ch->next_id++;
    s.generation++;
    s.active = true;
    s.cb     = cb;
    s.user   = user;
    return SubscriptionHandle{ channel, s.id, s.generation };
}

/** 購読を解除する (Publish 反復中なら slot 解放を遅延しつつ即座に非アクティブ化)。 */
bool MessageBroker::Unsubscribe(SubscriptionHandle h) noexcept {
    if (!h.IsValid()) return false;
    Channel* ch = GetChannel(h.channel, false);
    if (!ch) return false;

    for (usize i = 0; i < ch->slots.Size(); ++i) {
        Slot& s = ch->slots[i];
        if (s.id == h.id && s.generation == h.generation && s.active) {
            if (ch->publish_depth > 0) {
                // Publish 中はスロット解放を遅延 (反復中の崩しを防ぐ)
                ch->pending_cancel.PushBack(static_cast<u32>(i));
                s.active = false;   // 即座に「呼ばれない」状態にだけはする
            } else {
                s.active = false;
                s.cb     = nullptr;
                s.user   = nullptr;
                ch->free_indices.PushBack(static_cast<u32>(i));
            }
            return true;
        }
    }
    return false;
}

/** 発行時点の購読者集合を同期で呼び、ネスト解消後に遅延 Cancel を反映する。 */
void MessageBroker::PublishRaw(EventTypeId channel, const void* payload) noexcept {
    Channel* ch = GetChannel(channel, false);
    if (!ch) return;

    ++ch->publish_depth;
    // ループ中に Subscribe 追加されても、新規スロットは末尾なので走査が広がるだけ。
    // ここでは「発行時点の購読者集合」だけを呼びたいので size を先に固定する。
    const usize n = ch->slots.Size();
    for (usize i = 0; i < n; ++i) {
        Slot& s = ch->slots[i];
        if (!s.active || !s.cb) continue;
        s.cb(payload, s.user);
    }
    --ch->publish_depth;

    // Publish ネスト解消後に遅延 Cancel を反映
    if (ch->publish_depth == 0 && ch->pending_cancel.Size() > 0) {
        for (usize i = 0; i < ch->pending_cancel.Size(); ++i) {
            const u32 idx = ch->pending_cancel[i];
            if (idx < ch->slots.Size()) {
                Slot& s = ch->slots[idx];
                s.cb   = nullptr;
                s.user = nullptr;
                ch->free_indices.PushBack(idx);
            }
        }
        ch->pending_cancel.Clear();
    }
}

/** 指定チャンネルの active な購読 slot 数を数えて返す。 */
u32 MessageBroker::SubscriberCount(EventTypeId channel) const noexcept {
    if (channel >= m_Channels.Size() || !m_Channels[channel]) return 0;
    const Channel* ch = m_Channels[channel];
    u32 n = 0;
    for (usize i = 0; i < ch->slots.Size(); ++i) if (ch->slots[i].active) ++n;
    return n;
}

} // namespace acs
