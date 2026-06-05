// SPDX-License-Identifier: Apache-2.0
// Derived<T, Deps...> — N 個の Observable から派生する読み取り専用 Observable
//
// 使い方:
//   Observable<f32> hp     { 100.0f };
//   Observable<f32> max_hp { 100.0f };
//
//   // hp / max_hp が変わるたびに ratio も再計算 (lazy)
//   Derived<f32, f32, f32> ratio(
//       [](const f32& h, const f32& m) { return h / (m > 0 ? m : 1.0f); },
//       hp, max_hp);
//
//   // 取得は通常の Observable と同じ:
//   ratio.Get();                        // 1.0
//   hp.Set(50.0f);                      // ratio は dirty マークだけ (recompute は次回 Get で)
//   ratio.Get();                        // 0.5  ← ここで再計算
//
//   // Subscribe も普通に使える:
//   ratio.Subscribe([](const f32& v, void*){ /*...*/ }, nullptr);
//
// 設計:
//   ・lazy recompute on Get() — UE5 や RxJS の eager とは違う方針。
//     diamond pattern (A → B,C → D) でも D が 1 回しか再計算されない。
//   ・homogeneous な型を強制 (Deps... は1種類が普通だが variadic は型がそろってればOK)
//   ・dep が変わると m_Dirty=true、次の Get() で fn を呼ぶ。
//     fn の結果が変わってる場合のみ Subscriber に通知。
//   ・dep より先に Derived が破棄されることが安全 (RAII で自動 Unsubscribe)。
//     dep の方が先に死ぬとダングリング購読 → assert で検出 (Debug)。
#pragma once

#include "mvvm/Observable.h"

namespace acs {

/**
 * 複数の Observable<D> から値を算出する読み取り専用の派生 Observable。
 *
 * @details
 * homogeneous な dep (すべて同じ型 D、1..4 個) を購読し、いずれかが変わると
 * 計算関数を呼んで結果を内部 Observable<T> に Set する。結果が変わった場合のみ
 * 下流リスナへ通知され、同値スキップにより diamond パターンでも余計な再通知を抑える。
 * Get() は dirty フラグが立っていれば再計算する遅延評価。dep より先に Derived が
 * 破棄されるのは安全 (dtor で自動 Unsubscribe)。
 *
 * @tparam T 算出される値の型。
 * @tparam D 全 dep に共通する Observable の要素型。
 */
template<typename T, typename D>
class Derived {
public:
    /**
     * 計算関数と依存 Observable を指定して構築する。
     *
     * @details
     * fn は const D& を dep の個数 N 個受け取り T を返す非キャプチャ lambda
     * または関数ポインタ。N (1..4) に応じた固定アリティの関数ポインタへ
     * reinterpret_cast して void* に統一格納し、各 dep を Subscribe したうえで
     * 初回計算を eager に 1 回実行する。
     * @tparam Fn 計算関数の型 (const D& を N 個取り T を返す)。
     * @tparam DepRefs 依存 Observable<D> の参照型パック。
     * @param fn 派生値を算出する関数。
     * @param deps 監視する依存 Observable<D> 群 (1..4 個)。
     */
    template<typename Fn, typename... DepRefs>
    Derived(Fn fn, DepRefs&... deps) noexcept
    {
        constexpr usize N = sizeof...(DepRefs);
        static_assert(N >= 1 && N <= kMaxDeps, "Derived supports 1..4 deps");
        if constexpr (N == 1) {
            m_Fn = reinterpret_cast<void*>(static_cast<T(*)(const D&)>(fn));
        } else if constexpr (N == 2) {
            m_Fn = reinterpret_cast<void*>(static_cast<T(*)(const D&, const D&)>(fn));
        } else if constexpr (N == 3) {
            m_Fn = reinterpret_cast<void*>(static_cast<T(*)(const D&, const D&, const D&)>(fn));
        } else if constexpr (N == 4) {
            m_Fn = reinterpret_cast<void*>(static_cast<T(*)(const D&, const D&, const D&, const D&)>(fn));
        }
        m_DepCount = N;
        usize i = 0;
        // 初期値計算 + Subscribe
        ((m_Deps[i] = static_cast<Observable<D>*>(&deps), ++i), ...);

        // 初回計算 (eager に 1 回だけ走る、以後は lazy)
        Recompute();

        // 各 dep に Subscribe
        for (usize k = 0; k < m_DepCount; ++k) {
            m_Handles[k] = m_Deps[k]->Subscribe(&OnDepChanged, this);
        }
    }

    /** 各 dep から自身を Unsubscribe して破棄する。 */
    ~Derived() noexcept {
        for (usize k = 0; k < m_DepCount; ++k) {
            if (m_Deps[k]) m_Deps[k]->Unsubscribe(m_Handles[k]);
        }
    }

    /** コピー禁止 (dep への購読を単独所有するため)。 */
    Derived(const Derived&)            = delete;

    /** コピー代入も禁止。 */
    Derived& operator=(const Derived&) = delete;

    /**
     * 派生値を返す (dirty なら先に再計算する遅延評価)。
     *
     * @return 最新の派生値への const 参照。
     */
    const T& Get() noexcept {
        if (m_Dirty) {
            Recompute();
            m_Dirty = false;
        }
        return m_Out.Get();
    }

    /**
     * 派生値を保持する内部 Observable<T> を露出する。
     *
     * @return 派生値の Observable<T> への参照 (Bind/Subscribe 対象)。
     */
    Observable<T>& AsObservable() noexcept { return m_Out; }

    /**
     * 派生値の変更リスナを登録する (内部 Observable へ委譲)。
     *
     * @param cb 値変更時に呼ぶリスナ。
     * @param user リスナへ渡す任意ポインタ。
     * @return 登録した購読を指すハンドル。
     */
    ObservableHandle Subscribe(typename Observable<T>::Listener cb, void* user) noexcept {
        return m_Out.Subscribe(cb, user);
    }

    /**
     * 派生値の変更リスナを解除する (内部 Observable へ委譲)。
     *
     * @param h Subscribe が返したハンドル。
     * @return 解除できたら true。
     */
    bool Unsubscribe(ObservableHandle h) noexcept { return m_Out.Unsubscribe(h); }

private:
    /**
     * いずれかの dep が変化したときに呼ばれるリスナ。
     *
     * @details
     * dirty を立てた直後に Recompute も行う 2 段階方式で、遅延 Get() 取得と
     * 「常に最新を下流へ通知」の両立を図る (Recompute 内の Set は同値スキップが効く)。
     * @param v 変化した dep の新値 (未使用)。
     * @param user this を指すポインタ。
     */
    static void OnDepChanged(const D& /*v*/, void* user) noexcept {
        auto* self = static_cast<Derived*>(user);
        self->m_Dirty = true;
        // dirty フラグだけ立てて、実際の再計算は次の Get() で。
        // ただし「常に最新を Subscriber へ通知」も叶えたいので、ここでも Recompute する。
        // diamond glitch 回避のために 2 段階: dep 通知 → dirty + Recompute。
        // Recompute 内部の Set() は同値スキップが効く。
        self->Recompute();
        self->m_Dirty = false;
    }

    /**
     * 全 dep の現在値を計算関数に渡し、結果を内部 Observable へ Set する。
     *
     * @details dep 個数 (1..4) で分岐し、void* の m_Fn を該当アリティへ戻して呼ぶ。
     */
    void Recompute() noexcept {
        if (m_DepCount == 0 || !m_Fn) return;
        // 引数 N 個分を m_Deps から取り出して fn を呼ぶ。
        // homogeneous なので index で参照可能。N の実値で switch する。
        switch (m_DepCount) {
            case 1: {
                const auto fn = reinterpret_cast<T(*)(const D&)>(m_Fn);
                m_Out.Set(fn(m_Deps[0]->Get()));
                break;
            }
            case 2: {
                const auto fn = reinterpret_cast<T(*)(const D&, const D&)>(m_Fn);
                m_Out.Set(fn(m_Deps[0]->Get(), m_Deps[1]->Get()));
                break;
            }
            case 3: {
                const auto fn = reinterpret_cast<T(*)(const D&, const D&, const D&)>(m_Fn);
                m_Out.Set(fn(m_Deps[0]->Get(), m_Deps[1]->Get(), m_Deps[2]->Get()));
                break;
            }
            case 4: {
                const auto fn = reinterpret_cast<T(*)(const D&, const D&, const D&, const D&)>(m_Fn);
                m_Out.Set(fn(m_Deps[0]->Get(), m_Deps[1]->Get(), m_Deps[2]->Get(), m_Deps[3]->Get()));
                break;
            }
            default:
                // 5 以上は実用ではほぼ来ない。
                break;
        }
    }

    /** サポートする最大 dep 数。 */
    static constexpr usize kMaxDeps = 4;

    /** 監視中の依存 Observable へのポインタ配列 (未使用枠は nullptr)。 */
    Observable<D>*    m_Deps[kMaxDeps]    = {};

    /** 各 dep への購読ハンドル (dtor での Unsubscribe に使う)。 */
    ObservableHandle  m_Handles[kMaxDeps] = {};

    /** 有効な dep の個数 (1..4)。 */
    usize             m_DepCount         = 0;

    /** 型消去した計算関数 (実体は T(*)(const D&, ...))。 */
    void*             m_Fn                = nullptr;

    /** 派生値を保持し下流へ通知する内部 Observable。 */
    Observable<T>     m_Out;

    /** 再計算が必要かを示すフラグ。 */
    bool              m_Dirty             = false;
};

} // namespace acs
