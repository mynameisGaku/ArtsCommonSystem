// SPDX-License-Identifier: Apache-2.0
// TDerived<T, D> — 同じ要素型の複数 TObservable から派生する読み取り専用 TObservable
//
// 使い方:
//   TObservable<f32> hp     { 100.0f };
//   TObservable<f32> max_hp { 100.0f };
//
//   // hp / max_hp が変わるたびに ratio も即時再計算
//   TDerived<f32, f32> ratio(
//       [](const f32& h, const f32& m) { return h / (m > 0 ? m : 1.0f); },
//       hp, max_hp);
//
//   // 取得は通常の TObservable と同じ:
//   ratio.Get();                        // 1.0
//   hp.Set(50.0f);                      // 依存通知中に ratio も 0.5 へ更新
//   ratio.Get();                        // 0.5
//
//   // Subscribe も普通に使える:
//   ratio.Subscribe([](const f32& v, void*){ /*...*/ }, nullptr);
//
// 設計:
//   ・dep が変わるとその通知中に再計算し、結果が変わった場合だけ Subscriber へ通知する。
//   ・菱形依存 (A → B,C → D) では D が中間状態と最終状態を複数回観測し得る。
//     原子的な一括更新が必要な処理は、呼び出し側で batch 化する。
//   ・すべての dep は同じ要素型 D とし、1〜4 個まで受け付ける。
//   ・Get() は通常、通知時に更新済みの値を返す。dirty が残る経路では再計算して補完する。
//   ・dep より先に TDerived が破棄されることが安全 (RAII で自動 Unsubscribe)。
//     dep の方が先に死ぬとダングリング購読 → assert で検出 (Debug)。
#pragma once

#include "mvvm/Observable.h"

namespace acs {

/**
 * 複数の TObservable<D> から値を算出する読み取り専用の派生 TObservable。
 *
 * @details
 * homogeneous な dep (すべて同じ型 D、1..4 個) を購読し、いずれかが変わると
 * 計算関数を呼んで結果を内部 TObservable<T> に Set する。結果が変わった場合のみ
 * 下流リスナへ通知される。菱形依存では各 dep の通知ごとに再計算するため、中間状態と
 * 最終状態を複数回観測し得る。Get() は通常、通知時に更新済みの値を返し、dirty が
 * 残る経路では返す前に再計算する。dep より先に TDerived が破棄されるのは安全
 * (dtor で自動 Unsubscribe)。
 *
 * @tparam T 算出される値の型。
 * @tparam D 全 dep に共通する TObservable の要素型。
 */
template<typename T, typename D>
class TDerived {
public:
    /**
     * 計算関数と依存 TObservable を指定して構築する。
     *
     * @details
     * fn は const D& を dep の個数 N 個受け取り T を返す非キャプチャ lambda
     * または関数ポインタ。N (1..4) に応じた固定アリティの関数ポインタへ
     * reinterpret_cast して void* に統一格納し、各 dep を Subscribe したうえで
     * 初回計算を eager に 1 回実行する。
     * @tparam Fn 計算関数の型 (const D& を N 個取り T を返す)。
     * @tparam DepRefs 依存 TObservable<D> の参照型パック。
     * @param fn 派生値を算出する関数。
     * @param deps 監視する依存 TObservable<D> 群 (1..4 個)。
     */
    template<typename Fn, typename... DepRefs>
    TDerived(Fn fn, DepRefs&... deps) noexcept
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
        ((m_Deps[i] = static_cast<TObservable<D>*>(&deps), ++i), ...);

        // 初回値を即時計算する。以後も依存通知ごとに OnDepChanged から再計算する。
        Recompute();

        // 各 dep に Subscribe
        for (usize k = 0; k < m_DepCount; ++k) {
            m_Handles[k] = m_Deps[k]->Subscribe(&OnDepChanged, this);
        }
    }

    /**
     * 実行中 callback を寿命切れにしてから各 dep の購読を解除する。
     *
     * @details m_Out の listener が本 TDerived を同期破棄しても、OnDepChanged は
     * stack guard だけを確認して終了し、破棄済み m_Dirty へ書き込まない。
     */
    ~TDerived() noexcept {
        mvvm_detail::FCallbackLifetimeGuard::InvalidateChain(
            m_CallbackLifetimeGuards);
        for (usize k = 0; k < m_DepCount; ++k) {
            if (m_Deps[k]) m_Deps[k]->Unsubscribe(m_Handles[k]);
        }
    }

    /** コピー禁止 (dep への購読を単独所有するため)。 */
    TDerived(const TDerived&)            = delete;

    /** コピー代入も禁止。 */
    TDerived& operator=(const TDerived&) = delete;

    /**
     * 派生値を返す (通常は通知時に更新済み。dirty なら返す前に再計算する)。
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
     * 派生値を保持する内部 TObservable<T> を露出する。
     *
     * @return 派生値の TObservable<T> への参照 (Bind/Subscribe 対象)。
     */
    TObservable<T>& AsObservable() noexcept { return m_Out; }

    /**
     * 派生値の変更リスナを登録する (内部 TObservable へ委譲)。
     *
     * @param cb 値変更時に呼ぶリスナ。
     * @param user リスナへ渡す任意ポインタ。
     * @return 登録した購読を指すハンドル。
     */
    FObservableHandle Subscribe(typename TObservable<T>::Listener cb, void* user) noexcept {
        return m_Out.Subscribe(cb, user);
    }

    /**
     * 派生値の変更リスナを解除する (内部 TObservable へ委譲)。
     *
     * @param h Subscribe が返したハンドル。
     * @return 解除できたら true。
     */
    bool Unsubscribe(FObservableHandle h) noexcept { return m_Out.Unsubscribe(h); }

private:
    /**
     * いずれかの dep が変化したときに呼ばれるリスナ。
     *
     * @details
     * dirty を立てた直後に Recompute し、結果が変わった場合だけ下流へ即時通知する。
     * Recompute 内の Set は同値を通知しない。
     * @param v 変化した dep の新値 (未使用)。
     * @param user this を指すポインタ。
     */
    static void OnDepChanged(const D& /*v*/, void* user) noexcept {
        auto* self = static_cast<TDerived*>(user);
        mvvm_detail::FCallbackLifetimeGuard lifetime_guard(
            self->m_CallbackLifetimeGuards);
        self->m_Dirty = true;
        // 依存通知中に即時再計算し、結果が変わった場合だけ Subscriber へ通知する。
        // 出力 listener が owner を同期破棄した場合は guard 確認後に直ちに終了する。
        self->Recompute();
        if (!lifetime_guard.IsAlive()) return;
        self->m_Dirty = false;
    }

    /**
     * 全 dep の現在値を計算関数に渡し、結果を内部 TObservable へ Set する。
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

    /** 監視中の依存 TObservable へのポインタ配列 (未使用枠は nullptr)。 */
    TObservable<D>*    m_Deps[kMaxDeps]    = {};

    /** 各 dep への購読ハンドル (dtor での Unsubscribe に使う)。 */
    FObservableHandle  m_Handles[kMaxDeps] = {};

    /** 有効な dep の個数 (1..4)。 */
    usize             m_DepCount         = 0;

    /** 型消去した計算関数 (実体は T(*)(const D&, ...))。 */
    void*             m_Fn                = nullptr;

    /** 派生値を保持し下流へ通知する内部 TObservable。 */
    TObservable<T>     m_Out;

    /** 再計算が必要かを示すフラグ。 */
    bool              m_Dirty             = false;

    /** 実行中 OnDepChanged の stack lifetime guard 先頭。 */
    mvvm_detail::FCallbackLifetimeGuard* m_CallbackLifetimeGuards = nullptr;
};

} // namespace acs
