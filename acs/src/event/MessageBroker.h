// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "event/EventTypeId.h"
#include "event/SubscriptionHandle.h"
#include "threading/Atomic.h"

#include <type_traits>

namespace acs {

namespace event_detail {
/** 次に割り当てるメッセージ型番号。 */
inline TAtomic<u32> g_next_event_type_id{0};
} // namespace event_detail

/**
 * メッセージ型に固有の通路番号を返す。
 * 型番号の採番だけは複数スレッドから安全に呼び出せる。
 * @tparam E 番号を割り当てるメッセージ型。
 */
template<typename E>
EventTypeId GetEventTypeId() noexcept {
    /** この型へ一度だけ割り当てる通路番号。 */
    static const EventTypeId id = event_detail::g_next_event_type_id.FetchAdd(1);
    return id;
}

/**
 * メッセージを受け取る関数。
 * @param payload 配信された値。
 * @param user 購読時に登録した値。
 */
using MessageCallback = void (*)(const void* payload, void* user);

/**
 * 型ごとのメッセージを同じスレッド内で購読・配信する。
 * 配信開始後の追加は次の配信から有効になり、配信中の解除と全解除も安全に処理する。
 * 配信が戻るまでは、この仲介器と呼出し対象の寿命を呼出し側で保つ必要がある。
 * 異なるスレッド間の受け渡しにはTMessagePipeを使う。
 */
class FMessageBroker {
public:
    /** 空の仲介器を作る。 */
    FMessageBroker() noexcept = default;

    /** 全通路を解放する。 */
    ~FMessageBroker() noexcept;

    /** 通路の重複所有を防ぐためコピー構築を禁止する。 */
    FMessageBroker(const FMessageBroker&) = delete;

    /** 通路の重複所有を防ぐためコピー代入を禁止する。 */
    FMessageBroker& operator=(const FMessageBroker&) = delete;

    /**
     * 全購読を直ちに無効化し、通路を解放する。
     * 配信中に呼んだ場合は新しい購読を拒否し、最外側の配信終了時に通路を解放する。
     */
    void Clear() noexcept;

    /**
     * 指定した型のメッセージを購読する。
     * 関数が空、型数が上限外、全解除中、世代番号を使い切った、または保持領域を確保できない場合は無効なハンドルを返す。
     * @tparam E 購読するメッセージ型。
     * @param cb 配信時に呼び出す関数。
     * @param user 呼び出す関数へ渡す値。
     */
    template<typename E>
    FSubscriptionHandle Subscribe(MessageCallback cb, void* user) noexcept {
        return SubscribeRaw(GetEventTypeId<E>(), cb, user);
    }

    /**
     * 型付きコールバックをコンパイル時に検証して購読する。
     * @tparam E 購読するメッセージ型。
     * @tparam Callback コンパイル時に確定する型付きコールバック。
     * @param user コールバックへ渡す値。
     */
    template<typename E, auto Callback>
    FSubscriptionHandle SubscribeTyped(void* user) noexcept {
        static_assert(std::is_nothrow_invocable_r_v<void, decltype(Callback), const E&, void*>, "型付きイベントコールバックは void(const E&, void*) noexcept として呼べる必要があります");
        return SubscribeRaw(GetEventTypeId<E>(), &TypedCallbackThunk<E, Callback>, user);
    }

    /**
     * 指定した購読を解除する。
     * ハンドルが無効または解除済みの場合はfalseを返す。
     * ハンドルは取得元の仲介器にだけ渡す。
     * @param handle 解除する購読のハンドル。
     */
    bool Unsubscribe(FSubscriptionHandle handle) noexcept;

    /**
     * 指定した型のメッセージを購読先へ配信する。
     * 配信中の全解除後は残りの購読を呼ばない。
     * @tparam E 配信するメッセージ型。
     * @param payload 配信する値。
     */
    template<typename E>
    void Publish(const E& payload) noexcept {
        PublishRaw(GetEventTypeId<E>(), &payload);
    }

    /**
     * 指定した通路の有効な購読数を返す。
     * @param channel 調べる通路番号。
     */
    u32 SubscriberCount(EventTypeId channel) const noexcept;

private:
    /** 一件分の購読情報。 */
    struct FSlot {
        /** 通路内の購読番号。 */
        u32 id = 0;
        /** 再利用された購読枠を見分ける世代番号。 */
        u32 generation = 0;
        /** 現在購読中かを示す。 */
        bool active = false;
        /** 配信終了後または次回登録前に再利用一覧へ移すかを示す。 */
        bool pending_reuse = false;
        /** 配信時に呼び出す関数。 */
        MessageCallback cb = nullptr;
        /** 呼び出す関数へ渡す値。 */
        void* user = nullptr;
    };

    /** 一つのメッセージ型に属する購読群。 */
    struct FChannel {
        /** 確保済みの購読枠。 */
        TArray<FSlot> slots;
        /** 再利用できる購読枠の位置。 */
        TArray<u32> free_indices;
        /** 次に割り当てる購読番号。 */
        u32 next_id = 1;
        /** 入れ子になった配信の深さ。 */
        i32 publish_depth = 0;
        /** 現在有効な購読数。 */
        u32 active_count = 0;
        /** 通路0の制御領域で保持する、全通路を合計した配信深度。 */
        u32 broker_publish_depth = 0;
        /** 通路0の制御領域で保持する、配信終了後の全解除要求。 */
        bool broker_clear_pending = false;
    };

    /** 型付きコールバックを既存の MessageCallback へ接続する。 */
    template<typename E, auto Callback>
    static void TypedCallbackThunk(const void* payload, void* user) noexcept {
        Callback(*static_cast<const E*>(payload), user);
    }

    /**
     * 型を消去した関数を指定した通路へ登録する。
     * @param channel 登録先の通路番号。
     * @param cb 配信時に呼び出す関数。
     * @param user 呼び出す関数へ渡す値。
     */
    FSubscriptionHandle SubscribeRaw(EventTypeId channel, MessageCallback cb, void* user) noexcept;

    /**
     * 型を消去した値を指定した通路へ配信する。
     * @param channel 配信先の通路番号。
     * @param payload 配信する値。
     */
    void PublishRaw(EventTypeId channel, const void* payload) noexcept;

    /**
     * 指定した通路を取得し、必要なら作成する。
     * @param id 取得する通路番号。
     * @param create 存在しない通路を作成するか。
     */
    FChannel* GetChannel(EventTypeId id, bool create) noexcept;

    /** 制御領域を返し、未作成ならnullptrを返す。 */
    FChannel* GetControlChannel() noexcept;

    /** 配信終了まで全解除を保留しているかを返す。 */
    bool IsClearPending() const noexcept;

    /**
     * 再利用待ちの購読枠を空き一覧へ移す。
     * @param channel 整理する通路。
     */
    static void CollectReusableSlots(FChannel& channel) noexcept;

    /** 全通路の購読を無効化し、配信中の参照が指す領域は維持する。 */
    void InvalidateAllChannels() noexcept;

    /** 全通路と保持領域を解放し、保留中の全解除を完了する。 */
    void ReleaseChannels() noexcept;

    /** 通路番号から通路を引き、通路0を全通路の制御領域にも使う配列。 */
    TArray<FChannel*> m_Channels;
    /** 最後に割り当てた仲介器内で一意の世代番号。最大値では新規購読を拒否する。 */
    u32 m_GenerationSeed = 0;
};

/** 旧名を使う既存コード向けの互換別名。 */
using CMessageBroker = FMessageBroker;

} // namespace acs
