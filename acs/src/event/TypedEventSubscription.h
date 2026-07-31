// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "event/TypedEventHandle.h"
#include "memory/SharedPtr.h"

namespace acs {

namespace typed_event_detail {
/** 型付きイベントの共有状態。 */
template<typename... Arguments>
struct TEventState;
}

/** 破棄時に購読を解除する所有権付き購読。 */
template<typename... Arguments>
class TEventSubscription {
public:
    /** 空の購読を作る。 */
    TEventSubscription() noexcept = default;

    /** 保持中の購読を解除する。 */
    ~TEventSubscription() noexcept { Reset(); }

    /** 購読の重複所有を防ぐためコピー構築を禁止する。 */
    TEventSubscription(const TEventSubscription&) = delete;

    /** 購読の重複所有を防ぐためコピー代入を禁止する。 */
    TEventSubscription& operator=(const TEventSubscription&) = delete;

    /**
     * 購読の所有権を移す。
     * @param Other 移動元の購読。
     */
    TEventSubscription(TEventSubscription&& Other) noexcept : m_State(Move(Other.m_State)), m_Handle(Other.m_Handle) {
        Other.m_Handle = {};
    }

    /**
     * 現在の購読を解除して所有権を移す。
     * @param Other 移動元の購読。
     */
    TEventSubscription& operator=(TEventSubscription&& Other) noexcept {
        if (this == &Other) return *this;
        Reset();
        m_State = Move(Other.m_State);
        m_Handle = Other.m_Handle;
        Other.m_Handle = {};
        return *this;
    }

    /** 購読先が現在も有効かを返す。 */
    bool IsValid() const noexcept;

    /** 購読を解除し、解除できたかを返す。 */
    bool Reset() noexcept;

    /** 保持中の購読ハンドルを返す。 */
    FTypedEventHandle Handle() const noexcept { return m_Handle; }

private:
    /** この購読が参照する共有状態。 */
    using FState = typed_event_detail::TEventState<Arguments...>;

    /**
     * イベントが登録した購読を所有する。
     * @param State 購読先の共有状態。
     * @param Handle 登録済みの購読ハンドル。
     */
    TEventSubscription(const TSharedPtr<FState>& State, FTypedEventHandle Handle) noexcept : m_State(State), m_Handle(Handle) {}

    /** 購読先の寿命を延ばさない共有状態参照。 */
    TWeakPtr<FState> m_State;
    /** 解除対象を識別するハンドル。 */
    FTypedEventHandle m_Handle{};

    /** 所有権付き購読を作成できるイベント。 */
    template<typename...>
    friend class TEvent;
};

} // namespace acs
