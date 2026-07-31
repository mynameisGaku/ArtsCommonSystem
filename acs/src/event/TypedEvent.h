// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "event/TypedEventCallback.h"
#include "event/TypedEventState.h"
#include "event/TypedEventSubscription.h"
#include "foundation/TypeTraits.h"

namespace acs {

/** 引数の型を保ったまま複数の購読先へ通知するイベント。 */
template<typename... Arguments>
class TEvent {
    /** 同じ値を複数の購読へ渡すため、所有権が一度だけ移る右辺値参照は受け付けない。 */
    static_assert((!IsRvalueRefV<Arguments> && ...), "型付きイベントの引数に右辺値参照は指定できません");
    /** 値引数は各購読へ同じ内容を渡せるようにコピー構築を要求する。 */
    static_assert(((IsLvalueRefV<Arguments> || IsCopyConstructibleV<Arguments>) && ...), "型付きイベントの値引数はコピー構築できる必要があります");

public:
    /** このイベントが呼び出す関数型。 */
    using FCallback = TEventCallback<Arguments...>;
    /** このイベントの所有権付き購読型。 */
    using FSubscription = TEventSubscription<Arguments...>;

    /** 空のイベントを作る。 */
    TEvent() noexcept : m_State(MakeShared<FState>(typed_event_detail::AllocateTypedEventIdentifier())) {}

    /** 全購読を解除する。 */
    ~TEvent() noexcept { Clear(); }

    /** イベント個体の重複を防ぐためコピー構築を禁止する。 */
    TEvent(const TEvent&) = delete;

    /** イベント個体の重複を防ぐためコピー代入を禁止する。 */
    TEvent& operator=(const TEvent&) = delete;

    /** 購読ハンドルの参照先を固定するため移動構築を禁止する。 */
    TEvent(TEvent&&) = delete;

    /** 購読ハンドルの参照先を固定するため移動代入を禁止する。 */
    TEvent& operator=(TEvent&&) = delete;

    /**
     * 関数を購読先へ追加する。
     * @param Callback 配信時に呼び出す関数。
     * @param User 呼び出す関数へ渡す値。
     */
    FTypedEventHandle Subscribe(FCallback Callback, void* User = nullptr) noexcept {
        return m_State ? AddSubscription(*m_State, Callback, User, false, 0) : FTypedEventHandle{};
    }

    /**
     * 一度だけ呼び出す関数を追加する。
     * @param Callback 配信時に呼び出す関数。
     * @param User 呼び出す関数へ渡す値。
     */
    FTypedEventHandle SubscribeOnce(FCallback Callback, void* User = nullptr) noexcept {
        return m_State ? AddSubscription(*m_State, Callback, User, true, 0) : FTypedEventHandle{};
    }

    /**
     * 優先度を指定して関数を追加する。
     * @param Callback 配信時に呼び出す関数。
     * @param Priority 大きいほど先に呼び出す優先度。
     * @param User 呼び出す関数へ渡す値。
     */
    FTypedEventHandle SubscribeWithPriority(FCallback Callback, i32 Priority, void* User = nullptr) noexcept {
        return m_State ? AddSubscription(*m_State, Callback, User, false, Priority) : FTypedEventHandle{};
    }

    /**
     * 優先度を指定して一度だけ呼び出す関数を追加する。
     * @param Callback 配信時に呼び出す関数。
     * @param Priority 大きいほど先に呼び出す優先度。
     * @param User 呼び出す関数へ渡す値。
     */
    FTypedEventHandle SubscribeOnceWithPriority(FCallback Callback, i32 Priority, void* User = nullptr) noexcept {
        return m_State ? AddSubscription(*m_State, Callback, User, true, Priority) : FTypedEventHandle{};
    }

    /**
     * 破棄時に自動解除する購読を追加する。
     * @param Callback 配信時に呼び出す関数。
     * @param User 呼び出す関数へ渡す値。
     */
    FSubscription SubscribeOwned(FCallback Callback, void* User = nullptr) noexcept {
        /** 登録した購読を識別するハンドル。 */
        const FTypedEventHandle Handle = Subscribe(Callback, User);
        return Handle.IsValid() ? FSubscription(m_State, Handle) : FSubscription{};
    }

    /**
     * 指定した購読を解除する。
     * @param Handle 解除する購読のハンドル。
     */
    bool Unsubscribe(FTypedEventHandle Handle) noexcept {
        return m_State && UnsubscribeState(*m_State, Handle);
    }

    /**
     * 指定した購読が有効かを返す。
     * @param Handle 調べる購読のハンドル。
     */
    bool IsSubscribed(FTypedEventHandle Handle) const noexcept {
        return m_State && IsSubscribedState(*m_State, Handle);
    }

    /**
     * 指定した購読の優先度を取得する。
     * @param Handle 調べる購読のハンドル。
     * @param Priority 取得した優先度の格納先。
     */
    bool TryGetPriority(FTypedEventHandle Handle, i32& Priority) const noexcept {
        if (!IsSubscribed(Handle)) return false;
        Priority = m_State->slots[Handle.slot_index].priority;
        return true;
    }

    /**
     * 有効な購読ハンドルを呼び出し元の配列へ複写する。
     * @param Output ハンドルの格納先。
     * @param OutputCapacity 格納先の要素数。
     * @param OutputCount 複写した要素数の格納先。
     */
    bool TryCopySubscriptionHandles(FTypedEventHandle* Output, u32 OutputCapacity, u32& OutputCount) const noexcept {
        if (!m_State) {
            if (!Output && OutputCapacity != 0) return false;
            OutputCount = 0;
            return true;
        }
        if ((!Output && OutputCapacity != 0) || m_State->active_count > OutputCapacity || (m_State->active_count != 0 && !Output)) return false;
        /** 格納済みのハンドル数。 */
        u32 Written = 0;
        for (/** 現在調べる購読枠の位置。 */ u32 Index = 0; Index < m_State->slots.Size(); ++Index) {
            /** 現在調べる購読情報。 */
            const FSlot& Slot = m_State->slots[Index];
            if (!Slot.active) continue;
            if (!Slot.callback || Slot.generation == 0 || Slot.retired || Slot.pending_reuse) return false;
            Output[Written++] = {m_State->event_id, Index, Slot.generation};
        }
        if (Written != m_State->active_count) return false;
        OutputCount = Written;
        return true;
    }

    /** 有効な購読数を返す。 */
    u32 SubscriptionCount() const noexcept {
        return m_State ? m_State->active_count : 0;
    }

    /** 現在配信中かを返す。 */
    bool IsPublishing() const noexcept {
        return m_State && m_State->publish_depth != 0;
    }

    /** 全購読を解除する。 */
    void Clear() noexcept {
        if (!m_State) return;
        for (/** 現在解除する購読枠の位置。 */ u32 Index = 0; Index < m_State->slots.Size(); ++Index) {
            if (m_State->slots[Index].active) RetireSlot(*m_State, Index);
        }
    }

    /**
     * 現在の購読先へ値を配信する。
     * @param Values 配信する値。
     */
    void Publish(Arguments... Values) noexcept;

private:
    /** イベントの共有状態。 */
    using FState = typed_event_detail::TEventState<Arguments...>;
    /** 一件分の購読情報。 */
    using FSlot = typed_event_detail::TEventSlot<Arguments...>;

    /**
     * 共有状態へ購読を追加する。
     * @param State 追加先の共有状態。
     * @param Callback 配信時に呼び出す関数。
     * @param User 呼び出す関数へ渡す値。
     * @param Once 一度の配信後に解除するか。
     * @param Priority 呼び出し順を決める優先度。
     */
    static FTypedEventHandle AddSubscription(FState& State, FCallback Callback, void* User, bool Once, i32 Priority) noexcept;

    /**
     * 共有状態に指定した購読が残っているかを返す。
     * @param State 調べる共有状態。
     * @param Handle 調べる購読のハンドル。
     */
    static bool IsSubscribedState(const FState& State, FTypedEventHandle Handle) noexcept;

    /**
     * 共有状態から指定した購読を解除する。
     * @param State 解除元の共有状態。
     * @param Handle 解除する購読のハンドル。
     */
    static bool UnsubscribeState(FState& State, FTypedEventHandle Handle) noexcept;

    /**
     * 指定した購読枠を無効にする。
     * @param State 対象の共有状態。
     * @param SlotIndex 無効にする購読枠の位置。
     */
    static void RetireSlot(FState& State, u32 SlotIndex) noexcept;

    /**
     * 再利用待ちの購読枠を空き一覧へ移す。
     * @param State 対象の共有状態。
     */
    static void CollectReusableSlots(FState& State) noexcept;

    /** 購読情報を保持する共有状態。 */
    TSharedPtr<FState> m_State;

    /** 所有権付き購読から状態を確認できるようにする。 */
    friend TEventSubscription<Arguments...>;
};

} // namespace acs

#include "event/TypedEventStorage.inl"
