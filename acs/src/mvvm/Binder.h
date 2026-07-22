// SPDX-License-Identifier: Apache-2.0
// TTwoWayBinder<T> — 2 つの TObservable<T> を双方向同期させる
//
// 使い方:
//   TObservable<f32> view_hp;
//   TObservable<f32> model_hp { 100.0f };
//
//   TTwoWayBinder<f32> bind(view_hp, model_hp);  // 両方が同期
//   model_hp.Set(50.0f);   // → view_hp も 50 になる
//   view_hp.Set(0.0f);     // → model_hp も 0 になる
//
//   // bind が破棄されたら同期解除
//
// 設計:
//   ・Set() はループしないように、現在更新中フラグで再帰防止
//   ・TOneWayBinder<T>(src, dst) で片方向版もアリ
#pragma once

#include "mvvm/Observable.h"
#include "mvvm/Convert.h"
#include "memory/UniquePtr.h"

namespace acs {

/**
 * 2 つの TObservable<T> を双方向に同期させるバインダ。
 *
 * @details
 * 構築時にまず a の値を b へ反映し、その後両方を Subscribe する。一方が変わると
 * もう一方へ Set で伝播し、m_bUpdating フラグで往復ループ (再帰) を防ぐ。
 * dtor で両 TObservable から Unsubscribe する。
 *
 * @tparam T 同期する値の型。
 */
template<typename T>
class TTwoWayBinder {
public:
    /**
     * 2 つの TObservable を双方向バインドして構築する。
     *
     * @details a の現在値を b に初期反映してから両方を Subscribe する (a→b が初期方向)。
     * @param a 一方の TObservable (初期同期の source)。
     * @param b もう一方の TObservable (初期同期の destination)。
     */
    TTwoWayBinder(TObservable<T>& a, TObservable<T>& b) noexcept
        : m_A(&a), m_B(&b) {
        // 初期同期: a の値を b に反映 (FViewModel → View 想定)
        b.Set(a.Get());
        m_HA = a.Subscribe(&OnAChanged, this);
        m_HB = b.Subscribe(&OnBChanged, this);
    }

    /**
     * 実行中 callback を寿命切れにしてから両 TObservable の購読を解除する。
     *
     * @details 伝播先 Set の listener が本 binder を同期破棄した場合、callback の
     * stack guard を先に dead にし、復帰後に破棄済み member を書き換えない。
     */
    ~TTwoWayBinder() noexcept {
        mvvm_detail::FCallbackLifetimeGuard::InvalidateChain(
            m_CallbackLifetimeGuards);
        if (m_A) m_A->Unsubscribe(m_HA);
        if (m_B) m_B->Unsubscribe(m_HB);
    }

    /** コピー禁止 (購読を単独所有するため)。 */
    TTwoWayBinder(const TTwoWayBinder&) = delete;

    /** コピー代入も禁止。 */
    TTwoWayBinder& operator=(const TTwoWayBinder&) = delete;

private:
    /**
     * a が変化したとき b へ伝播するリスナ。
     *
     * @details m_bUpdating が立っていれば往復を避けるため何もしない。
     * @param v a の新値。
     * @param user this を指すポインタ。
     */
    static void OnAChanged(const T& v, void* user) noexcept {
        auto* self = static_cast<TTwoWayBinder*>(user);
        mvvm_detail::FCallbackLifetimeGuard lifetime_guard(
            self->m_CallbackLifetimeGuards);
        if (self->m_bUpdating) return;
        self->m_bUpdating = true;
        TObservable<T>* const destination = self->m_B;
        destination->Set(v);
        if (!lifetime_guard.IsAlive()) return;
        self->m_bUpdating = false;
    }

    /**
     * b が変化したとき a へ伝播するリスナ。
     *
     * @details m_bUpdating が立っていれば往復を避けるため何もしない。
     * @param v b の新値。
     * @param user this を指すポインタ。
     */
    static void OnBChanged(const T& v, void* user) noexcept {
        auto* self = static_cast<TTwoWayBinder*>(user);
        mvvm_detail::FCallbackLifetimeGuard lifetime_guard(
            self->m_CallbackLifetimeGuards);
        if (self->m_bUpdating) return;
        self->m_bUpdating = true;
        TObservable<T>* const destination = self->m_A;
        destination->Set(v);
        if (!lifetime_guard.IsAlive()) return;
        self->m_bUpdating = false;
    }

    /** 一方の TObservable。 */
    TObservable<T>*    m_A = nullptr;

    /** もう一方の TObservable。 */
    TObservable<T>*    m_B = nullptr;

    /** m_A への購読ハンドル。 */
    FObservableHandle  m_HA;

    /** m_B への購読ハンドル。 */
    FObservableHandle  m_HB;

    /** 伝播中フラグ (往復ループ防止)。 */
    bool              m_bUpdating = false;

    /** 実行中 OnAChanged / OnBChanged の stack lifetime guard 先頭。 */
    mvvm_detail::FCallbackLifetimeGuard* m_CallbackLifetimeGuards = nullptr;
};

/**
 * src の変化を dst へ片方向に流すバインダ (同じ型)。
 *
 * @details 構築時に src の現在値を dst へ反映し、以後 src の変化を dst へ Set する。
 * @tparam T 同期する値の型。
 */
template<typename T>
class TOneWayBinder {
public:
    /**
     * 片方向バインドして構築する。
     *
     * @param src 監視元の TObservable。
     * @param dst 反映先の TObservable。
     */
    TOneWayBinder(TObservable<T>& src, TObservable<T>& dst) noexcept
        : m_Src(&src), m_Dst(&dst) {
        dst.Set(src.Get());
        m_H = src.Subscribe(&OnChanged, this);
    }

    /** src から Unsubscribe して同期を解除する。 */
    ~TOneWayBinder() noexcept {
        if (m_Src) m_Src->Unsubscribe(m_H);
    }

    /** コピー禁止 (購読を単独所有するため)。 */
    TOneWayBinder(const TOneWayBinder&) = delete;

    /** コピー代入も禁止。 */
    TOneWayBinder& operator=(const TOneWayBinder&) = delete;

private:
    /**
     * src が変化したとき dst へ反映するリスナ。
     *
     * @param v src の新値。
     * @param user this を指すポインタ。
     */
    static void OnChanged(const T& v, void* user) noexcept {
        auto* self = static_cast<TOneWayBinder*>(user);
        self->m_Dst->Set(v);
    }

    /** 監視元の TObservable。 */
    TObservable<T>*   m_Src = nullptr;

    /** 反映先の TObservable。 */
    TObservable<T>*   m_Dst = nullptr;

    /** src への購読ハンドル。 */
    FObservableHandle m_H;
};

/**
 * src の変化を型変換して dst へ片方向に流すバインダ (異なる型)。
 *
 * @details
 * 構築時に変換関数で src の現在値を変換して dst へ反映し、以後 src の変化のたびに
 * 同じ変換を通して dst へ Set する。dtor で src から Unsubscribe する。
 * @tparam Src 監視元の値の型。
 * @tparam Dst 反映先の値の型。
 */
template<typename Src, typename Dst>
class TOneWayConvertBinder {
public:
    /** Src を Dst へ変換する関数型 (値と user を受け取る)。 */
    using ConvertFn = Dst (*)(const Src& v, void* user);

    /**
     * 変換つき片方向バインドして構築する。
     *
     * @param src 監視元の TObservable。
     * @param dst 反映先の TObservable。
     * @param fn Src を Dst へ変換する関数。
     * @param user 変換関数へそのまま渡される任意ポインタ。
     */
    TOneWayConvertBinder(TObservable<Src>& src, TObservable<Dst>& dst,
                        ConvertFn fn, void* user) noexcept
        : m_Src(&src), m_Dst(&dst), m_Fn(fn), m_User(user) {
        dst.Set(fn(src.Get(), user));
        m_H = src.Subscribe(&OnChanged, this);
    }

    /** src から Unsubscribe して同期を解除する。 */
    ~TOneWayConvertBinder() noexcept {
        if (m_Src) m_Src->Unsubscribe(m_H);
    }

    /** コピー禁止 (購読を単独所有するため)。 */
    TOneWayConvertBinder(const TOneWayConvertBinder&)            = delete;

    /** コピー代入も禁止。 */
    TOneWayConvertBinder& operator=(const TOneWayConvertBinder&) = delete;

private:
    /**
     * src が変化したとき変換して dst へ反映するリスナ。
     *
     * @param v src の新値。
     * @param user this を指すポインタ。
     */
    static void OnChanged(const Src& v, void* user) noexcept {
        auto* self = static_cast<TOneWayConvertBinder*>(user);
        if (self->m_Fn) self->m_Dst->Set(self->m_Fn(v, self->m_User));
    }

    /** 監視元の TObservable。 */
    TObservable<Src>*  m_Src  = nullptr;

    /** 反映先の TObservable。 */
    TObservable<Dst>*  m_Dst  = nullptr;

    /** Src→Dst の変換関数。 */
    ConvertFn         m_Fn   = nullptr;

    /** 変換関数へ渡す user ポインタ。 */
    void*             m_User = nullptr;

    /** src への購読ハンドル。 */
    FObservableHandle  m_H;
};

/**
 * src の現在値を dst へ 1 回だけコピーする (live binding ではなく初期化用)。
 *
 * @details 継続購読しないため class ではなく関数で提供する。
 * @tparam T 値の型。
 * @param src コピー元の TObservable。
 * @param dst コピー先の TObservable。
 */
template<typename T>
inline void CopyOnce(const TObservable<T>& src, TObservable<T>& dst) noexcept {
    dst.Set(src.Get());
}

/**
 * 同じ型の src→dst を片方向バインドする (TOneWayBinder ファクトリ)。
 *
 * @details 返り値は prvalue なので copy elision で auto に直接受けられる。
 * @tparam T 値の型。
 * @param src 監視元の TObservable。
 * @param dst 反映先の TObservable。
 * @return 構築した TOneWayBinder<T>。
 */
template<typename T>
inline TOneWayBinder<T> Bind(TObservable<T>& src, TObservable<T>& dst) noexcept {
    return TOneWayBinder<T>(src, dst);
}

/**
 * 異なる型の src→dst を既定変換を通して片方向バインドする。
 *
 * @details TDefaultConverter<Src, Dst> 経由の TOneWayConvertBinder を返す。
 * @tparam Src 監視元の値の型。
 * @tparam Dst 反映先の値の型。
 * @param src 監視元の TObservable。
 * @param dst 反映先の TObservable。
 * @return 構築した TOneWayConvertBinder<Src, Dst>。
 */
template<typename Src, typename Dst>
inline TOneWayConvertBinder<Src, Dst> Bind(TObservable<Src>& src, TObservable<Dst>& dst) noexcept {
    return TOneWayConvertBinder<Src, Dst>(
        src, dst, &mvvm::TDefaultConverter<Src, Dst>::Convert, nullptr);
}

/**
 * 同じ型の a と b を双方向バインドする (TTwoWayBinder ファクトリ)。
 *
 * @tparam T 値の型。
 * @param a 一方の TObservable。
 * @param b もう一方の TObservable。
 * @return 構築した TTwoWayBinder<T>。
 */
template<typename T>
inline TTwoWayBinder<T> TwoWayBind(TObservable<T>& a, TObservable<T>& b) noexcept {
    return TTwoWayBinder<T>(a, b);
}

/**
 * 同じ型の片方向バインドを TUniquePtr で生成する。
 *
 * @details
 * 生 Binder はコピー/ムーブ不可でメンバとして持ちにくいため、ヒープ確保した
 * TUniquePtr を返してクラスメンバとして保持でき、dtor で自動 Unsubscribe される。
 * @tparam T 値の型。
 * @param src 監視元の TObservable。
 * @param dst 反映先の TObservable。
 * @return 所有権を持つ TOneWayBinder<T> の TUniquePtr。
 */
template<typename T>
inline TUniquePtr<TOneWayBinder<T>> MakeBind(TObservable<T>& src, TObservable<T>& dst) noexcept {
    return MakeUnique<TOneWayBinder<T>>(src, dst);
}

/**
 * 同じ型の双方向バインドを TUniquePtr で生成する。
 *
 * @tparam T 値の型。
 * @param a 一方の TObservable。
 * @param b もう一方の TObservable。
 * @return 所有権を持つ TTwoWayBinder<T> の TUniquePtr。
 */
template<typename T>
inline TUniquePtr<TTwoWayBinder<T>> MakeTwoWayBind(TObservable<T>& a, TObservable<T>& b) noexcept {
    return MakeUnique<TTwoWayBinder<T>>(a, b);
}

/**
 * 異なる型の変換つき片方向バインドを既定変換で TUniquePtr 生成する。
 *
 * @tparam Src 監視元の値の型。
 * @tparam Dst 反映先の値の型。
 * @param src 監視元の TObservable。
 * @param dst 反映先の TObservable。
 * @return 所有権を持つ TOneWayConvertBinder<Src, Dst> の TUniquePtr。
 */
template<typename Src, typename Dst>
inline TUniquePtr<TOneWayConvertBinder<Src, Dst>>
MakeBindConvert(TObservable<Src>& src, TObservable<Dst>& dst) noexcept {
    return MakeUnique<TOneWayConvertBinder<Src, Dst>>(
        src, dst, &mvvm::TDefaultConverter<Src, Dst>::Convert, nullptr);
}

/**
 * 異なる型の変換つき片方向バインドを明示変換関数で TUniquePtr 生成する。
 *
 * @tparam Src 監視元の値の型。
 * @tparam Dst 反映先の値の型。
 * @param src 監視元の TObservable。
 * @param dst 反映先の TObservable。
 * @param fn Src を Dst へ変換する関数。
 * @param user 変換関数へ渡す任意ポインタ。
 * @return 所有権を持つ TOneWayConvertBinder<Src, Dst> の TUniquePtr。
 */
template<typename Src, typename Dst>
inline TUniquePtr<TOneWayConvertBinder<Src, Dst>>
MakeBindConvert(TObservable<Src>& src, TObservable<Dst>& dst,
                Dst (*fn)(const Src&, void*), void* user) noexcept {
    return MakeUnique<TOneWayConvertBinder<Src, Dst>>(src, dst, fn, user);
}

} // namespace acs
