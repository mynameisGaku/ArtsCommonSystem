// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "threading/Atomic.h"

#include <type_traits>

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
struct FSubscriptionHandle {
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
    bool operator==(const FSubscriptionHandle& o) const noexcept {
        return channel == o.channel && id == o.id && generation == o.generation;
    }
};

/** 無効な FSubscriptionHandle (戻り値や初期値で使う番兵)。 */
inline constexpr FSubscriptionHandle kInvalidSubscription{};

/** 購読コールバック型 (payload は const void*、user はユーザーデータ)。 */
using MessageCallback = void (*)(const void* payload, void* user);

/**
 * 型ベースの同期 pub/sub イベントバス (シングルスレッド前提)。
 *
 * @details
 * イベント型 E ごとに ChannelId を割り当て、購読コールバック (関数ポインタ + user) を
 * slot に登録する。Publish は発行時点の購読者集合を同期で即座に呼ぶ。Subscribe /
 * Unsubscribe は Publish 中でも安全 (解除は遅延適用)。Subscribe/Publish/Unsubscribe を
 * 異なるスレッドから呼ぶのは未定義動作で、スレッド間通信には TMessagePipe<T> を使う。
 */
class FMessageBroker {
public:
    /** 空のブローカを構築する。 */
    FMessageBroker() noexcept = default;

    /** 全チャンネルを解放して破棄する。 */
    ~FMessageBroker() noexcept;

    /**
     * 全購読とチャンネルを解放し、空のブローカに戻す。
     *
     * @details 実行中の既定アロケータで作られたチャンネルを FMemorySystem より先に
     * 破棄するため、アプリケーション終了処理からも呼ばれる。繰り返し呼んでも安全。
     * Publish のコールバック内からは呼ばないこと。
     */
    void Clear() noexcept;

    /** コピー禁止 (チャンネルを所有するため)。 */
    FMessageBroker(const FMessageBroker&) = delete;

    /** コピー代入も禁止。 */
    FMessageBroker& operator=(const FMessageBroker&) = delete;

    /**
     * 型 E のイベントを購読する。
     *
     * @tparam E 購読するイベント型。
     * @param cb 発行時に呼ぶコールバック (payload は const E* として渡る)。
     * @param user コールバックへ渡すユーザーデータ。
     * @return 購読を指すハンドル (cb が null なら kInvalidSubscription)。
     */
    template<typename E>
    FSubscriptionHandle Subscribe(MessageCallback cb, void* user) noexcept {
        return SubscribeRaw(GetEventTypeId<E>(), cb, user);
    }

    /**
     * 型付きコールバックをコンパイル時に検証して購読する。
     *
     * @details Callback は `void(const E&, void*) noexcept` として呼べる必要がある。関数自体を
     * 非型テンプレート引数にするため、slot には型タグや実行時 cast 用情報を保持せず、
     * Publish の ABI は従来の MessageCallback のまま維持できる。
     * @tparam E 購読するイベント型。
     * @tparam Callback コンパイル時に確定する型付きコールバック。
     * @param user Callback へ渡すユーザーデータ。
     * @return 購読を指すハンドル。
     */
    template<typename E, auto Callback>
    FSubscriptionHandle SubscribeTyped(void* user) noexcept
    {
        static_assert(std::is_nothrow_invocable_r_v<void, decltype(Callback), const E&, void*>, "型付きイベントコールバックは void(const E&, void*) noexcept として呼べる必要があります");
        return SubscribeRaw(GetEventTypeId<E>(), &TypedCallbackThunk<E, Callback>, user);
    }

    /**
     * 購読を解除する (Publish 中に呼んでも安全)。
     *
     * @details Publish 反復中の場合は slot 解放を遅延しつつ即座に「呼ばれない」状態にする。
     * @param h 解除する購読ハンドル。
     * @return 該当購読が見つかり解除できたら true。
     */
    bool Unsubscribe(FSubscriptionHandle h) noexcept;

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
    struct FSlot {
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
    struct FChannel {
        /** 購読 slot の配列。 */
        TArray<FSlot>  slots;

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

        /** 現在 active な購読 slot 数。 */
        u32 active_count = 0;
    };

    /** 型付きコールバックを既存 MessageCallback ABI へ接続するコンパイル時 thunk。 */
    template<typename E, auto Callback>
    static void TypedCallbackThunk(const void* payload, void* user) noexcept
    {
        Callback(*static_cast<const E*>(payload), user);
    }

    /**
     * 型消去された購読登録の実体 (Subscribe から委譲)。
     *
     * @param channel 購読先チャンネルの EventTypeId。
     * @param cb 登録するコールバック。
     * @param user コールバックへ渡すユーザーデータ。
     * @return 購読ハンドル (cb が null・チャンネル確保失敗なら kInvalidSubscription)。
     */
    FSubscriptionHandle SubscribeRaw(EventTypeId channel,
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
    FChannel* GetChannel(EventTypeId id, bool create) noexcept;

    /** チャンネル配列 (EventTypeId → FChannel*、所有権を持つ)。 */
    TArray<FChannel*> m_Channels;

    /** Clear のたびに進める、新規チャンネル用の世代。 */
    u32 m_GenerationSeed = 0;
};

} // namespace acs
