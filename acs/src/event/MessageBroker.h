// SPDX-License-Identifier: Apache-2.0
// MessageBroker — 型ベースの pub/sub イベントバス
//
// **スレッド契約 (重要)**: MessageBroker は **シングルスレッド前提** です。
//   Subscribe / Publish / Unsubscribe を異なるスレッドから呼ぶと未定義動作。
//   スレッド間通信が必要な場合は MessagePipe<T> (event/MessagePipe.h) を使う。
//   (型 ID 採番だけ TAtomic を使うのは複数スレッドで同じ型 E の id が一致するため)
//
// 使い方:
//   struct DamageEvent { EntityId target; f32 amount; };
//
//   static void OnDamage(const void* payload, void* user) {
//       const auto& e = *static_cast<const DamageEvent*>(payload);
//       auto* self = static_cast<MyState*>(user);
//       // ...
//   }
//
//   MessageBroker bus;
//   SubscriptionHandle h = bus.Subscribe<DamageEvent>(&OnDamage, &my_state);
//
//   DamageEvent e{ enemy, 25.0f };
//   bus.Publish<DamageEvent>(e);
//
//   bus.Unsubscribe(h);
//
// 設計:
//   ・コールバックは関数ポインタ + payload (const void*) + user (void*)
//   ・型 E ごとに ChannelId を割り当てる (EventTypeId<E>)
//   ・Publish は同期実行 (即座にハンドラ呼ぶ)。非同期にしたければユーザー側で
//     キューイング (将来 MessagePipe を別途追加予定)。
//   ・Subscribe / Unsubscribe は Publish 中でも安全 (予約バッファに溜めて遅延適用)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "threading/Atomic.h"

namespace acs {

// イベント型 ID (型ごとに一意な番号)
using EventTypeId = u32;
inline constexpr EventTypeId kMaxEventTypes = 256;

namespace event_detail {
inline TAtomic<u32> g_next_event_type_id{0};
} // namespace event_detail

template<typename E>
EventTypeId GetEventTypeId() noexcept {
    static const EventTypeId id = event_detail::g_next_event_type_id.FetchAdd(1);
    return id;
}

// 購読ハンドル (id=0 なら無効)
struct SubscriptionHandle {
    EventTypeId channel    = 0xFFFFFFFFu;
    u32         id         = 0;
    u32         generation = 0;

    bool IsValid() const noexcept { return id != 0; }
    bool operator==(const SubscriptionHandle& o) const noexcept {
        return channel == o.channel && id == o.id && generation == o.generation;
    }
};

inline constexpr SubscriptionHandle kInvalidSubscription{};

// 購読コールバック型
using MessageCallback = void (*)(const void* payload, void* user);

class MessageBroker {
public:
    MessageBroker() noexcept = default;
    ~MessageBroker() noexcept;

    MessageBroker(const MessageBroker&) = delete;
    MessageBroker& operator=(const MessageBroker&) = delete;

    // E 型のイベントを購読する
    template<typename E>
    SubscriptionHandle Subscribe(MessageCallback cb, void* user) noexcept {
        return SubscribeRaw(GetEventTypeId<E>(), cb, user);
    }

    // 購読を解除 (Publish 中に呼んでも安全)
    bool Unsubscribe(SubscriptionHandle h) noexcept;

    // E 型のイベントを発行 — 全購読者を同期で呼ぶ
    template<typename E>
    void Publish(const E& payload) noexcept {
        PublishRaw(GetEventTypeId<E>(), &payload);
    }

    // チャンネルごとの購読数 (デバッグ用)
    u32 SubscriberCount(EventTypeId channel) const noexcept;

private:
    struct Slot {
        u32           id          = 0;        // 1-based
        u32           generation  = 0;
        bool          active      = false;
        MessageCallback cb          = nullptr;
        void*         user        = nullptr;
    };

    struct Channel {
        TArray<Slot>  slots;
        TArray<u32>   free_indices;
        u32          next_id      = 1;
        i32          publish_depth = 0;       // Publish 中ネストレベル (>0 なら遅延 Cancel)
        TArray<u32>   pending_cancel;          // publish_depth > 0 のとき Cancel を貯める
    };

    SubscriptionHandle SubscribeRaw(EventTypeId channel,
                                     MessageCallback cb, void* user) noexcept;
    void PublishRaw(EventTypeId channel, const void* payload) noexcept;
    Channel* GetChannel(EventTypeId id, bool create) noexcept;

    TArray<Channel*> m_Channels;   // EventTypeId -> Channel*
};

} // namespace acs
