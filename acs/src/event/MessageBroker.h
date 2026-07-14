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
//     キューイング (MessagePipe を使う)。
//   ・Subscribe / Unsubscribe は Publish 中でも安全 (予約バッファに溜めて遅延適用)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "threading/Atomic.h"

namespace acs {

/** イベント型 ID (型 E ごとに一意な ChannelId)。 */
using EventTypeId = u32;

/** 同時に扱えるイベント型の上限。 */
inline constexpr EventTypeId kMaxEventTypes = 256;

namespace event_detail {
/** 全 E 共通の採番カウンタ (次に割り当てる EventTypeId を保持)。 */
inline TAtomic<u32> g_next_event_type_id{0};
} // namespace event_detail

/**
 * イベント型 E に固有な EventTypeId を返す (初回呼び出しで採番、以降キャッシュ)。
 *
 * @details
 * magic statics + TAtomic::FetchAdd で採番するため、複数スレッドから同じ型 E を
 * 初めて呼んでも一意な id に確定する (型 ID 採番だけはスレッドセーフ)。
 * @tparam E ID を割り当てるイベント型。
 * @return 型 E に対応する EventTypeId。
 */
template<typename E>
EventTypeId GetEventTypeId() noexcept {
    static const EventTypeId id = event_detail::g_next_event_type_id.FetchAdd(1);
    return id;
}

/**
 * 購読を一意に指すハンドル (チャンネル + 世代付き slot id)。
 *
 * @details
 * id は 1-based で 0 は無効。slot 再利用を generation で見分けるため、Unsubscribe
 * 済みのハンドルで誤って別購読を解除することを防ぐ。
 */
struct SubscriptionHandle {
    /** 購読先チャンネルの EventTypeId。 */
    EventTypeId channel    = 0xFFFFFFFFu;

    /** チャンネル内 slot の 1-based id (0 は無効)。 */
    u32         id         = 0;

    /** slot 再利用を見分ける世代番号。 */
    u32         generation = 0;

    /**
     * ハンドルが有効値かを返す。
     *
     * @return id が 0 でなければ true。
     */
    bool IsValid() const noexcept { return id != 0; }

    /**
     * チャンネル・id・世代の全てが一致するかを返す。
     *
     * @param o 比較対象のハンドル。
     * @return 完全一致すれば true。
     */
    bool operator==(const SubscriptionHandle& o) const noexcept {
        return channel == o.channel && id == o.id && generation == o.generation;
    }
};

/** 無効な SubscriptionHandle (戻り値や初期値で使う番兵)。 */
inline constexpr SubscriptionHandle kInvalidSubscription{};

/** 購読コールバック型 (payload は const void*、user はユーザーデータ)。 */
using MessageCallback = void (*)(const void* payload, void* user);

/**
 * 型ベースの同期 pub/sub イベントバス (シングルスレッド前提)。
 *
 * @details
 * イベント型 E ごとに ChannelId を割り当て、購読コールバック (関数ポインタ + user) を
 * slot に登録する。Publish は発行時点の購読者集合を同期で即座に呼ぶ。Subscribe /
 * Unsubscribe は Publish 中でも安全 (解除は遅延適用)。Subscribe/Publish/Unsubscribe を
 * 異なるスレッドから呼ぶのは未定義動作で、スレッド間通信には MessagePipe<T> を使う。
 */
class MessageBroker {
public:
    /** 空のブローカを構築する。 */
    MessageBroker() noexcept = default;

    /** 全チャンネルを解放して破棄する。 */
    ~MessageBroker() noexcept;

    /**
     * 全購読とチャンネルを解放し、空のブローカに戻す。
     *
     * @details 実行中の既定アロケータで作られたチャンネルを MemorySystem より先に
     * 破棄するため、アプリケーション終了処理からも呼ばれる。繰り返し呼んでも安全。
     * Publish のコールバック内からは呼ばないこと。
     */
    void Clear() noexcept;

    /** コピー禁止 (チャンネルを所有するため)。 */
    MessageBroker(const MessageBroker&) = delete;

    /** コピー代入も禁止。 */
    MessageBroker& operator=(const MessageBroker&) = delete;

    /**
     * 型 E のイベントを購読する。
     *
     * @tparam E 購読するイベント型。
     * @param cb 発行時に呼ぶコールバック (payload は const E* として渡る)。
     * @param user コールバックへ渡すユーザーデータ。
     * @return 購読を指すハンドル (cb が null なら kInvalidSubscription)。
     */
    template<typename E>
    SubscriptionHandle Subscribe(MessageCallback cb, void* user) noexcept {
        return SubscribeRaw(GetEventTypeId<E>(), cb, user);
    }

    /**
     * 購読を解除する (Publish 中に呼んでも安全)。
     *
     * @details Publish 反復中の場合は slot 解放を遅延しつつ即座に「呼ばれない」状態にする。
     * @param h 解除する購読ハンドル。
     * @return 該当購読が見つかり解除できたら true。
     */
    bool Unsubscribe(SubscriptionHandle h) noexcept;

    /**
     * 型 E のイベントを発行し、全購読者を同期で呼ぶ。
     *
     * @details 発行時点の購読者集合のみを呼ぶ (Publish 中に追加された購読は対象外)。
     * @tparam E 発行するイベント型。
     * @param payload 各購読者へ渡すイベント値。
     */
    template<typename E>
    void Publish(const E& payload) noexcept {
        PublishRaw(GetEventTypeId<E>(), &payload);
    }

    /**
     * 指定チャンネルのアクティブな購読数を返す (デバッグ用)。
     *
     * @param channel 問い合わせるチャンネルの EventTypeId。
     * @return active な購読 slot の数。
     */
    u32 SubscriberCount(EventTypeId channel) const noexcept;

private:
    /**
     * 1 購読 slot (コールバックと世代を保持)。
     */
    struct Slot {
        /** slot の 1-based id (0 は未割り当て)。 */
        u32           id          = 0;

        /** slot 再利用を見分ける世代番号。 */
        u32           generation  = 0;

        /** この slot が現在有効かのフラグ。 */
        bool          active      = false;

        /** 発行時に呼ぶコールバック。 */
        MessageCallback cb          = nullptr;

        /** コールバックへ渡すユーザーデータ。 */
        void*         user        = nullptr;
    };

    /**
     * 1 イベント型に対応するチャンネル (購読 slot 集合と遅延解除キュー)。
     */
    struct Channel {
        /** 購読 slot の配列。 */
        TArray<Slot>  slots;

        /** 解放済みで再利用待ちの slot 添字。 */
        TArray<u32>   free_indices;

        /** 次に割り当てる 1-based slot id。 */
        u32          next_id      = 1;

        /** Publish 中のネストレベル (>0 のとき解除を遅延)。 */
        i32          publish_depth = 0;

        /** Clear 前の購読ハンドルを無効化するため、新規 slot へ与える世代。 */
        u32 generation_seed = 0;

        /** publish_depth>0 の間に貯めた解除対象 slot 添字。 */
        TArray<u32>   pending_cancel;
    };

    /**
     * 型消去された購読登録の実体 (Subscribe から委譲)。
     *
     * @param channel 購読先チャンネルの EventTypeId。
     * @param cb 登録するコールバック。
     * @param user コールバックへ渡すユーザーデータ。
     * @return 購読ハンドル (cb が null・チャンネル確保失敗なら kInvalidSubscription)。
     */
    SubscriptionHandle SubscribeRaw(EventTypeId channel,
                                     MessageCallback cb, void* user) noexcept;

    /**
     * 型消去された発行の実体 (Publish から委譲)。
     *
     * @param channel 発行先チャンネルの EventTypeId。
     * @param payload 各購読者へ渡すペイロード先頭。
     */
    void PublishRaw(EventTypeId channel, const void* payload) noexcept;

    /**
     * EventTypeId に対応するチャンネルを返す (必要なら生成)。
     *
     * @param id 取得するチャンネルの EventTypeId。
     * @param create true なら未生成時に新規確保する。
     * @return チャンネルへのポインタ (未生成かつ create=false なら nullptr)。
     */
    Channel* GetChannel(EventTypeId id, bool create) noexcept;

    /** チャンネル配列 (EventTypeId → Channel*、所有権を持つ)。 */
    TArray<Channel*> m_Channels;

    /** Clear のたびに進める、新規チャンネル用の世代。 */
    u32 m_GenerationSeed = 0;
};

} // namespace acs
