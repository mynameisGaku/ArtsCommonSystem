// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Move.h"
#include "foundation/TypeTraits.h"
#include "foundation/Types.h"

namespace acs {

/** 関数形式ごとの単一デリゲートを宣言する。 */
template<typename Signature>
class TDelegate;

/** 戻り値を持つ関数を一つだけ結び付ける型付きデリゲート。 */
template<typename TResult, typename... Arguments>
class TDelegate<TResult(Arguments...)> final {
public:
    /** 呼出し対象と引数を受け取る内部関数の型。 */
    using FFunction = TResult (*)(void*, Arguments...);

    /** 呼出し対象が未設定のデリゲートを作る。 */
    constexpr TDelegate() noexcept = default;

    /**
     * 任意データを受け取る静的関数を結び付ける。
     * @param function 呼び出す関数。nullptrなら未設定を返す。
     * @param user 呼出し時に関数へ渡す任意データ。
     */
    static constexpr TDelegate CreateStatic(FFunction function, void* user = nullptr) noexcept {
        return function != nullptr ? TDelegate(function, user) : TDelegate{};
    }

    /** 任意データを使わない静的関数を結び付ける。 */
    template<auto Function>
    static constexpr TDelegate CreateStatic() noexcept {
        return TDelegate(&InvokeStatic<Function>, nullptr);
    }

    /**
     * 対象のメンバー関数を結び付ける。
     * 呼出し中は対象の生存を登録側で保つ必要がある。
     * @param object 呼出し対象。nullptrなら未設定のデリゲートを返す。
     */
    template<auto Method, typename TObject>
    static constexpr TDelegate CreateRaw(TObject* object) noexcept {
        /** const指定を外して保存に使う対象型。呼出し時には元の指定へ戻す。 */
        using FStoredObject = RemoveCVT<TObject>;
        return object != nullptr ? TDelegate(&InvokeMember<Method, TObject>, const_cast<FStoredObject*>(object)) : TDelegate{};
    }

    /** 呼出し対象が設定済みならtrueを返す。 */
    constexpr bool IsBound() const noexcept { return m_Function != nullptr; }

    /**
     * 実行結果を書き込み、未設定ならfalseを返す。
     * @param out_result 呼出し結果の書込先。
     * @param arguments 結び付けた関数へ渡す引数。
     */
    bool TryExecute(TResult& out_result, Arguments... arguments) const noexcept {
        if (m_Function == nullptr) return false;
        out_result = m_Function(m_User, Forward<Arguments>(arguments)...);
        return true;
    }

    /** 設定を解除する。 */
    constexpr void Unbind() noexcept {
        m_Function = nullptr;
        m_User = nullptr;
    }

    /** 低水準APIへ渡す関数を返す。 */
    constexpr FFunction Function() const noexcept { return m_Function; }

    /** 低水準APIへ渡す任意データを返す。 */
    constexpr void* User() const noexcept { return m_User; }

private:
    /**
     * 関数と任意データを直接保持する。
     * @param function 呼び出す関数。
     * @param user 関数へ渡す任意データ。
     */
    constexpr TDelegate(FFunction function, void* user) noexcept : m_Function(function), m_User(user) {}

    /**
     * @param arguments 静的関数へ渡す引数。
     */
    template<auto Function>
    static TResult InvokeStatic(void*, Arguments... arguments) {
        return Function(Forward<Arguments>(arguments)...);
    }

    /**
     * @param user メンバー関数を呼ぶ対象。
     * @param arguments メンバー関数へ渡す引数。
     */
    template<auto Method, typename TObject>
    static TResult InvokeMember(void* user, Arguments... arguments) {
        return (static_cast<TObject*>(user)->*Method)(Forward<Arguments>(arguments)...);
    }

    /** 呼出し先を型消去した関数。 */
    FFunction m_Function = nullptr;
    /** 呼出し先へ渡す任意データ。 */
    void* m_User = nullptr;
};

/** 戻り値のない関数を一つだけ結び付ける型付きデリゲート。 */
template<typename... Arguments>
class TDelegate<void(Arguments...)> final {
public:
    /** 呼出し対象と引数を受け取る内部関数の型。 */
    using FFunction = void (*)(void*, Arguments...);

    /** 呼出し対象が未設定のデリゲートを作る。 */
    constexpr TDelegate() noexcept = default;

    /**
     * 任意データを受け取る静的関数を結び付ける。
     * @param function 呼び出す関数。nullptrなら未設定を返す。
     * @param user 呼出し時に関数へ渡す任意データ。
     */
    static constexpr TDelegate CreateStatic(FFunction function, void* user = nullptr) noexcept {
        return function != nullptr ? TDelegate(function, user) : TDelegate{};
    }

    /** 任意データを使わない静的関数を結び付ける。 */
    template<auto Function>
    static constexpr TDelegate CreateStatic() noexcept {
        return TDelegate(&InvokeStatic<Function>, nullptr);
    }

    /**
     * 対象のメンバー関数を結び付ける。
     * 呼出し中は対象の生存を登録側で保つ必要がある。
     * @param object 呼出し対象。nullptrなら未設定のデリゲートを返す。
     */
    template<auto Method, typename TObject>
    static constexpr TDelegate CreateRaw(TObject* object) noexcept {
        /** const指定を外して保存に使う対象型。呼出し時には元の指定へ戻す。 */
        using FStoredObject = RemoveCVT<TObject>;
        return object != nullptr ? TDelegate(&InvokeMember<Method, TObject>, const_cast<FStoredObject*>(object)) : TDelegate{};
    }

    /** 呼出し対象が設定済みならtrueを返す。 */
    constexpr bool IsBound() const noexcept { return m_Function != nullptr; }

    /**
     * 設定済みの処理を呼び、未設定ならfalseを返す。
     * @param arguments 結び付けた関数へ渡す引数。
     */
    bool ExecuteIfBound(Arguments... arguments) const noexcept {
        if (m_Function == nullptr) return false;
        m_Function(m_User, Forward<Arguments>(arguments)...);
        return true;
    }

    /** 設定を解除する。 */
    constexpr void Unbind() noexcept {
        m_Function = nullptr;
        m_User = nullptr;
    }

    /** 低水準APIへ渡す関数を返す。 */
    constexpr FFunction Function() const noexcept { return m_Function; }

    /** 低水準APIへ渡す任意データを返す。 */
    constexpr void* User() const noexcept { return m_User; }

private:
    /**
     * 関数と任意データを直接保持する。
     * @param function 呼び出す関数。
     * @param user 関数へ渡す任意データ。
     */
    constexpr TDelegate(FFunction function, void* user) noexcept : m_Function(function), m_User(user) {}

    /**
     * @param arguments 静的関数へ渡す引数。
     */
    template<auto Function>
    static void InvokeStatic(void*, Arguments... arguments) {
        Function(Forward<Arguments>(arguments)...);
    }

    /**
     * @param user メンバー関数を呼ぶ対象。
     * @param arguments メンバー関数へ渡す引数。
     */
    template<auto Method, typename TObject>
    static void InvokeMember(void* user, Arguments... arguments) {
        (static_cast<TObject*>(user)->*Method)(Forward<Arguments>(arguments)...);
    }

    /** 呼出し先を型消去した関数。 */
    FFunction m_Function = nullptr;
    /** 呼出し先へ渡す任意データ。 */
    void* m_User = nullptr;
};

} // namespace acs
