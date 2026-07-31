// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace acs {

/**
 * 共有状態に指定した購読が残っているかを返す。
 * @param State 調べる共有状態。
 * @param Handle 調べる購読のハンドル。
 */
template<typename... Arguments>
bool TEvent<Arguments...>::IsSubscribedState(const FState& State, FTypedEventHandle Handle) noexcept {
    if (!Handle.IsValid() || Handle.event_id != State.event_id || Handle.slot_index >= State.slots.Size()) return false;
    /** ハンドルが指す購読情報。 */
    const FSlot& Slot = State.slots[Handle.slot_index];
    return Slot.active && Slot.generation == Handle.generation;
}

/**
 * 共有状態から指定した購読を解除する。
 * @param State 解除元の共有状態。
 * @param Handle 解除する購読のハンドル。
 */
template<typename... Arguments>
bool TEvent<Arguments...>::UnsubscribeState(FState& State, FTypedEventHandle Handle) noexcept {
    if (!IsSubscribedState(State, Handle)) return false;
    RetireSlot(State, Handle.slot_index);
    return true;
}

/**
 * 再利用待ちの購読枠を空き一覧へ移す。
 * @param State 対象の共有状態。
 */
template<typename... Arguments>
void TEvent<Arguments...>::CollectReusableSlots(FState& State) noexcept {
    for (/** 現在調べる購読枠の位置。 */ u32 Index = 0; Index < State.slots.Size(); ++Index) {
        /** 現在調べる購読情報。 */
        FSlot& Slot = State.slots[Index];
        if (!Slot.pending_reuse || Slot.retired) continue;
        if (!State.free_slots.TryPushBack(Index)) return;
        Slot.pending_reuse = false;
    }
}

/**
 * 指定した購読枠を無効にする。
 * @param State 対象の共有状態。
 * @param SlotIndex 無効にする購読枠の位置。
 */
template<typename... Arguments>
void TEvent<Arguments...>::RetireSlot(FState& State, u32 SlotIndex) noexcept {
    if (SlotIndex >= State.slots.Size()) return;
    /** 無効にする購読情報。 */
    FSlot& Slot = State.slots[SlotIndex];
    if (!Slot.active) return;
    Slot.callback = nullptr;
    Slot.user = nullptr;
    Slot.priority = 0;
    Slot.active = false;
    Slot.once = false;
    --State.active_count;
    if (Slot.generation == 0xffffffffu) {
        Slot.retired = true;
        Slot.pending_reuse = false;
        return;
    }
    ++Slot.generation;
    Slot.pending_reuse = !State.free_slots.TryPushBack(SlotIndex);
}

/**
 * 共有状態へ購読を追加する。
 * @param State 追加先の共有状態。
 * @param Callback 配信時に呼び出す関数。
 * @param User 呼び出す関数へ渡す値。
 * @param Once 一度の配信後に解除するか。
 * @param Priority 呼び出し順を決める優先度。
 */
template<typename... Arguments>
FTypedEventHandle TEvent<Arguments...>::AddSubscription(FState& State, FCallback Callback, void* User, bool Once, i32 Priority) noexcept {
    if (!Callback || State.event_id == 0 || State.latest_activation_sequence == ~u64(0)) return {};
    CollectReusableSlots(State);
    /** 追加先の購読枠の位置。 */
    u32 SlotIndex = 0;
    if (!State.free_slots.IsEmpty()) {
        SlotIndex = State.free_slots.Back();
        State.free_slots.PopBack();
    } else {
        if (State.slots.Size() >= 0xffffffffu || !State.slots.TryPushBack(FSlot{})) return {};
        SlotIndex = static_cast<u32>(State.slots.Size() - 1);
    }
    /** 追加先の購読情報。 */
    FSlot& Slot = State.slots[SlotIndex];
    Slot.callback = Callback;
    Slot.user = User;
    Slot.activation_sequence = ++State.latest_activation_sequence;
    Slot.priority = Priority;
    Slot.active = true;
    Slot.once = Once;
    Slot.pending_reuse = false;
    ++State.active_count;
    return {State.event_id, SlotIndex, Slot.generation};
}

/**
 * 現在の購読先へ値を配信する。
 * @param Values 配信する値。
 */
template<typename... Arguments>
void TEvent<Arguments...>::Publish(Arguments... Values) noexcept {
    /** 値で渡す引数は購読ごとに複写できる必要がある。 */
    static_assert(((IsLvalueRefV<Arguments> || IsCopyConstructibleV<Arguments>) && ...), "型付きイベントの値引数はコピー構築できる必要があります");
    /** 配信中も購読状態を保持する共有参照。 */
    TSharedPtr<FState> State = m_State;
    if (!State) return;
    /** 配信開始時点で存在した購読枠の数。 */
    const u32 DeliveryCount = static_cast<u32>(State->slots.Size());
    /** 配信開始後に追加された購読を除くための上限。 */
    const u64 ActivationLimit = State->latest_activation_sequence;
    ++State->publish_depth;
    /** 前回呼び出した購読があるかを示す。 */
    bool HasPrevious = false;
    /** 前回呼び出した購読の優先度。 */
    i32 PreviousPriority = 0;
    /** 前回呼び出した購読枠の位置。 */
    u32 PreviousIndex = 0;
    for (;;) {
        /** 次に呼び出す購読が見つかったかを示す。 */
        bool Found = false;
        /** 次に呼び出す購読枠の位置。 */
        u32 SelectedIndex = 0;
        /** 次に呼び出す購読の優先度。 */
        i32 SelectedPriority = 0;
        for (/** 現在調べる購読枠の位置。 */ u32 Index = 0; Index < DeliveryCount; ++Index) {
            /** 現在調べる購読情報。 */
            const FSlot& Slot = State->slots[Index];
            if (!Slot.active || !Slot.callback || Slot.activation_sequence > ActivationLimit) continue;
            if (HasPrevious && (Slot.priority > PreviousPriority || (Slot.priority == PreviousPriority && Index <= PreviousIndex))) continue;
            if (!Found || Slot.priority > SelectedPriority || (Slot.priority == SelectedPriority && Index < SelectedIndex)) {
                Found = true;
                SelectedIndex = Index;
                SelectedPriority = Slot.priority;
            }
        }
        if (!Found) break;
        /** 今回呼び出す関数。 */
        FCallback Callback = State->slots[SelectedIndex].callback;
        /** 今回の関数へ渡す値。 */
        void* User = State->slots[SelectedIndex].user;
        if (State->slots[SelectedIndex].once) RetireSlot(*State, SelectedIndex);
        HasPrevious = true;
        PreviousPriority = SelectedPriority;
        PreviousIndex = SelectedIndex;
        Callback(User, Values...);
    }
    --State->publish_depth;
}

/** 購読先が現在も有効かを返す。 */
template<typename... Arguments>
bool TEventSubscription<Arguments...>::IsValid() const noexcept {
    /** 購読先が残っている間だけ保持する共有参照。 */
    TSharedPtr<FState> State = m_State.Lock();
    return State && TEvent<Arguments...>::IsSubscribedState(*State, m_Handle);
}

/** 購読を解除し、解除できたかを返す。 */
template<typename... Arguments>
bool TEventSubscription<Arguments...>::Reset() noexcept {
    /** 解除処理中だけ保持する共有参照。 */
    TSharedPtr<FState> State = m_State.Lock();
    /** 有効な購読を解除できたかを示す。 */
    const bool Removed = State && TEvent<Arguments...>::UnsubscribeState(*State, m_Handle);
    m_State.Reset();
    m_Handle = {};
    return Removed;
}

} // namespace acs
