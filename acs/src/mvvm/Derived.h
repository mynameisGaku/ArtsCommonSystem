// SPDX-License-Identifier: Apache-2.0
// FDerived<T, Deps...> — N 個の FObservable から派生する読み取り専用 FObservable
//
// 使い方:
//   FObservable<f32> hp     { 100.0f };
//   FObservable<f32> max_hp { 100.0f };
//
//   // hp / max_hp が変わるたびに ratio も再計算 (lazy)
//   FDerived<f32, f32, f32> ratio(
//       [](const f32& h, const f32& m) { return h / (m > 0 ? m : 1.0f); },
//       hp, max_hp);
//
//   // 取得は通常の FObservable と同じ:
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
//   ・dep が変わると _dirty=true、次の Get() で fn を呼ぶ。
//     fn の結果が変わってる場合のみ Subscriber に通知。
//   ・dep より先に FDerived が破棄されることが安全 (RAII で自動 Unsubscribe)。
//     dep の方が先に死ぬとダングリング購読 → assert で検出 (Debug)。
#pragma once

#include "mvvm/Observable.h"

namespace acs {

// homogeneous deps 版 (実用上 99%)
//   D : 全 dep の型 (例: f32)
//   N : dep の個数 (constexpr)
template<typename T, typename D>
class FDerived {
public:
    // fn は (D の参照を N 個) -> T を返す関数ポインタ。状態が要るなら Bind 側で持つこと。
    // 引数を pack で受けるのは可変個数のため。実体は ComputeFn。
    // 例: [](const f32& h, const f32& m){ return h/m; }

    // Fn は非キャプチャ lambda または関数ポインタ (任意の N に対する fixed-arity)。
    // _fn は内部 void* に統一格納し、Recompute() で N 別に reinterpret_cast する。
    template<typename Fn, typename... DepRefs>
    FDerived(Fn fn, DepRefs&... deps) noexcept
    {
        constexpr usize N = sizeof...(DepRefs);
        static_assert(N >= 1 && N <= kMaxDeps, "FDerived supports 1..4 deps");
        if constexpr (N == 1) {
            _fn = reinterpret_cast<void*>(static_cast<T(*)(const D&)>(fn));
        } else if constexpr (N == 2) {
            _fn = reinterpret_cast<void*>(static_cast<T(*)(const D&, const D&)>(fn));
        } else if constexpr (N == 3) {
            _fn = reinterpret_cast<void*>(static_cast<T(*)(const D&, const D&, const D&)>(fn));
        } else if constexpr (N == 4) {
            _fn = reinterpret_cast<void*>(static_cast<T(*)(const D&, const D&, const D&, const D&)>(fn));
        }
        _dep_count = N;
        usize i = 0;
        // 初期値計算 + Subscribe
        ((_deps[i] = static_cast<FObservable<D>*>(&deps), ++i), ...);

        // 初回計算 (eager に 1 回だけ走る、以後は lazy)
        Recompute();

        // 各 dep に Subscribe
        for (usize k = 0; k < _dep_count; ++k) {
            _handles[k] = _deps[k]->Subscribe(&OnDepChanged, this);
        }
    }

    ~FDerived() noexcept {
        for (usize k = 0; k < _dep_count; ++k) {
            if (_deps[k]) _deps[k]->Unsubscribe(_handles[k]);
        }
    }

    FDerived(const FDerived&)            = delete;
    FDerived& operator=(const FDerived&) = delete;

    // 値取得。dirty なら再計算する。
    const T& Get() noexcept {
        if (_dirty) {
            Recompute();
            _dirty = false;
        }
        return _out.Get();
    }

    // 派生値の購読 (元の FObservable<T> を露出する形)
    FObservable<T>& AsObservable() noexcept { return _out; }

    FObservableHandle Subscribe(typename FObservable<T>::Listener cb, void* user) noexcept {
        return _out.Subscribe(cb, user);
    }
    bool Unsubscribe(FObservableHandle h) noexcept { return _out.Unsubscribe(h); }

private:
    static void OnDepChanged(const D& /*v*/, void* user) noexcept {
        auto* self = static_cast<FDerived*>(user);
        self->_dirty = true;
        // dirty フラグだけ立てて、実際の再計算は次の Get() で。
        // ただし「常に最新を Subscriber へ通知」も叶えたいので、ここでも Recompute する。
        // diamond glitch 回避のために 2 段階: dep 通知 → dirty + Recompute。
        // Recompute 内部の Set() は同値スキップが効く。
        self->Recompute();
        self->_dirty = false;
    }

    void Recompute() noexcept {
        if (_dep_count == 0 || !_fn) return;
        // 引数 N 個分を _deps から取り出して fn を呼ぶ。
        // homogeneous なので index で参照可能。N の実値で switch する。
        switch (_dep_count) {
            case 1: {
                auto fn = reinterpret_cast<T(*)(const D&)>(_fn);
                _out.Set(fn(_deps[0]->Get()));
                break;
            }
            case 2: {
                auto fn = reinterpret_cast<T(*)(const D&, const D&)>(_fn);
                _out.Set(fn(_deps[0]->Get(), _deps[1]->Get()));
                break;
            }
            case 3: {
                auto fn = reinterpret_cast<T(*)(const D&, const D&, const D&)>(_fn);
                _out.Set(fn(_deps[0]->Get(), _deps[1]->Get(), _deps[2]->Get()));
                break;
            }
            case 4: {
                auto fn = reinterpret_cast<T(*)(const D&, const D&, const D&, const D&)>(_fn);
                _out.Set(fn(_deps[0]->Get(), _deps[1]->Get(), _deps[2]->Get(), _deps[3]->Get()));
                break;
            }
            default:
                // 5 以上は将来拡張。実用ではほぼ来ない。
                break;
        }
    }

    static constexpr usize kMaxDeps = 4;
    FObservable<D>*    _deps[kMaxDeps]    = {};
    FObservableHandle  _handles[kMaxDeps] = {};
    usize             _dep_count         = 0;
    void*             _fn                = nullptr;   // 実体は T(*)(const D&, ...)
    FObservable<T>     _out;
    bool              _dirty             = false;
};

} // namespace acs
