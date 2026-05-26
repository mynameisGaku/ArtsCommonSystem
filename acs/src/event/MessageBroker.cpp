// SPDX-License-Identifier: Apache-2.0
// MessageBroker 実装
#include "event/MessageBroker.h"

namespace acs {

MessageBroker::~MessageBroker() noexcept {
    for (usize i = 0; i < _channels.Size(); ++i) {
        if (_channels[i]) {
            delete _channels[i];
            _channels[i] = nullptr;
        }
    }
}

MessageBroker::Channel* MessageBroker::GetChannel(EventTypeId id, bool create) noexcept {
    if (id >= _channels.Size()) {
        if (!create) return nullptr;
        usize old = _channels.Size();
        _channels.Resize(id + 1);
        for (usize i = old; i <= id; ++i) _channels[i] = nullptr;
    }
    if (!_channels[id] && create) {
        _channels[id] = new Channel();
    }
    return _channels[id];
}

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
    }
    Slot& s = ch->slots[idx];
    if (s.id == 0) s.id = ch->next_id++;
    s.generation++;
    s.active = true;
    s.cb     = cb;
    s.user   = user;
    return SubscriptionHandle{ channel, s.id, s.generation };
}

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
            u32 idx = ch->pending_cancel[i];
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

u32 MessageBroker::SubscriberCount(EventTypeId channel) const noexcept {
    if (channel >= _channels.Size() || !_channels[channel]) return 0;
    const Channel* ch = _channels[channel];
    u32 n = 0;
    for (usize i = 0; i < ch->slots.Size(); ++i) if (ch->slots[i].active) ++n;
    return n;
}

} // namespace acs
