// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "event/TypedEvent.h"
#include "foundation/TypeTraits.h"

namespace acs {

/** 関数形式ごとの複数デリゲートを宣言する。 */
template<typename Signature>
class TMulticastDelegate;

/** 複数の処理を優先順に呼ぶ型付きデリゲート。 */
template<typename... Arguments>
class TMulticastDelegate<void(Arguments...)> final {
    /** 同じ値を複数の処理へ渡すため、所有権が一度だけ移る右辺値参照は受け付けない。 */
    static_assert((!IsRvalueRefV<Arguments> && ...), "複数デリゲートの引数に右辺値参照は指定できません");
    /** 値引数は各処理へ同じ内容を渡せるようにコピー構築を要求する。 */
    static_assert(((IsLvalueRefV<Arguments> || IsCopyConstructibleV<Arguments>) && ...), "複数デリゲートの値引数はコピー構築できる必要があります");

public:
    /** 登録できる関数の型。 */
    using FCallback = TEventCallback<Arguments...>;
    /** 生存中だけ登録を保つ購読の型。 */
    using FSubscription = TEventSubscription<Arguments...>;

    /**
     * 処理を追加する。
     * @param callback 呼び出す処理。
     * @param user 呼出し時に渡す任意データ。
     */
    FTypedEventHandle Add(FCallback callback, void* user = nullptr) noexcept { return m_Event.Subscribe(callback, user); }

    /** 任意データを使わない静的関数を追加する。 */
    template<auto Function>
    FTypedEventHandle AddStatic() noexcept {
        return Add(&InvokeStatic<Function>);
    }

    /**
     * 一度だけ呼ぶ処理を追加する。
     * @param callback 呼び出す処理。
     * @param user 呼出し時に渡す任意データ。
     */
    FTypedEventHandle AddOnce(FCallback callback, void* user = nullptr) noexcept { return m_Event.SubscribeOnce(callback, user); }

    /** 任意データを使わない静的関数を一度だけ呼ぶ処理として追加する。 */
    template<auto Function>
    FTypedEventHandle AddOnceStatic() noexcept {
        return AddOnce(&InvokeStatic<Function>);
    }

    /**
     * 優先度付きの処理を追加する。
     * @param callback 呼び出す処理。
     * @param priority 大きいほど先に呼ぶ優先度。
     * @param user 呼出し時に渡す任意データ。
     */
    FTypedEventHandle AddWithPriority(FCallback callback, i32 priority, void* user = nullptr) noexcept { return m_Event.SubscribeWithPriority(callback, priority, user); }

    /**
     * 一度だけ呼ぶ処理を優先度付きで追加する。
     * @param callback 呼び出す処理。
     * @param priority 大きいほど先に呼ぶ優先度。
     * @param user 呼出し時に渡す任意データ。
     */
    FTypedEventHandle AddOnceWithPriority(FCallback callback, i32 priority, void* user = nullptr) noexcept { return m_Event.SubscribeOnceWithPriority(callback, priority, user); }

    /**
     * 任意データを使わない静的関数を優先度付きで追加する。
     * @param priority 大きいほど先に呼ぶ優先度。
     */
    template<auto Function>
    FTypedEventHandle AddStaticWithPriority(i32 priority) noexcept {
        return AddWithPriority(&InvokeStatic<Function>, priority);
    }

    /**
     * 対象のメンバー関数を追加する。
     * 登録中は対象の生存を呼出し側で保つ必要がある。
     * @param object 呼出し対象。nullptrなら無効な識別値を返す。
     */
    template<auto Method, typename TObject>
    FTypedEventHandle AddRaw(TObject* object) noexcept {
        return object != nullptr ? Add(&InvokeMember<Method, TObject>, StoreObjectPointer(object)) : FTypedEventHandle{};
    }

    /**
     * 対象のメンバー関数を一度だけ呼ぶ処理として追加する。
     * 登録中は対象の生存を呼出し側で保つ必要がある。
     * @param object 呼出し対象。nullptrなら無効な識別値を返す。
     */
    template<auto Method, typename TObject>
    FTypedEventHandle AddOnceRaw(TObject* object) noexcept {
        return object != nullptr ? AddOnce(&InvokeMember<Method, TObject>, StoreObjectPointer(object)) : FTypedEventHandle{};
    }

    /**
     * 対象のメンバー関数を優先度付きで追加する。
     * 登録中は対象の生存を呼出し側で保つ必要がある。
     * @param object 呼出し対象。nullptrなら無効な識別値を返す。
     * @param priority 大きいほど先に呼ぶ優先度。
     */
    template<auto Method, typename TObject>
    FTypedEventHandle AddRawWithPriority(TObject* object, i32 priority) noexcept {
        return object != nullptr ? AddWithPriority(&InvokeMember<Method, TObject>, priority, StoreObjectPointer(object)) : FTypedEventHandle{};
    }

    /**
     * 対象のメンバー関数を所有購読として追加する。
     * 購読値が破棄されるまで対象の生存を呼出し側で保つ必要がある。
     * @param object 呼出し対象。nullptrなら空の購読を返す。
     */
    template<auto Method, typename TObject>
    FSubscription AddOwnedRaw(TObject* object) noexcept {
        return object != nullptr ? m_Event.SubscribeOwned(&InvokeMember<Method, TObject>, StoreObjectPointer(object)) : FSubscription{};
    }

    /**
     * 識別値に対応する処理を解除する。
     * @param handle 解除対象を世代付きで識別する値。世代は古い値の再利用を防ぐ番号。
     */
    bool Remove(FTypedEventHandle handle) noexcept { return m_Event.Unsubscribe(handle); }

    /** すべての処理を解除する。 */
    void Clear() noexcept { m_Event.Clear(); }

    /**
     * 登録した処理を呼ぶ。
     * @param arguments 各処理へ渡す引数。
     */
    void Broadcast(Arguments... arguments) noexcept { m_Event.Publish(arguments...); }

    /** 登録中の処理数を返す。 */
    u32 Count() const noexcept { return m_Event.SubscriptionCount(); }

    /** 一つ以上の処理が登録されているかを返す。 */
    bool IsBound() const noexcept { return Count() != 0; }

private:
    /**
     * 対象ポインターを内部の任意データ形式へ変換する。
     * 呼出し時は元のconst指定へ戻すため、対象そのものは変更しない。
     * @param object 保存する呼出し対象。
     */
    template<typename TObject>
    static void* StoreObjectPointer(TObject* object) noexcept {
        /** const指定を外して保存に使う対象型。 */
        using FStoredObject = RemoveCVT<TObject>;
        return const_cast<FStoredObject*>(object);
    }

    /**
     * 任意データを使わない静的関数を呼ぶ。
     * @param arguments 静的関数へ渡す引数。
     */
    template<auto Function>
    static void InvokeStatic(void*, Arguments... arguments) noexcept {
        static_assert(noexcept(Function(arguments...)), "複数デリゲートの静的関数は例外を送出しない宣言が必要です");
        Function(arguments...);
    }

    /**
     * 対象のメンバー関数を呼ぶ。
     * @param user 呼出し対象。
     * @param arguments メンバー関数へ渡す引数。
     */
    template<auto Method, typename TObject>
    static void InvokeMember(void* user, Arguments... arguments) noexcept {
        static_assert(noexcept((static_cast<TObject*>(user)->*Method)(arguments...)), "複数デリゲートのメンバー関数は例外を送出しない宣言が必要です");
        (static_cast<TObject*>(user)->*Method)(arguments...);
    }

    /** 購読順序と世代を管理する内部イベント。 */
    TEvent<Arguments...> m_Event;
};

} // namespace acs
