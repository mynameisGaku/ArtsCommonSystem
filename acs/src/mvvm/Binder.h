// SPDX-License-Identifier: Apache-2.0
// TwoWayBinder<T> — 2 つの Observable<T> を双方向同期させる
//
// 使い方:
//   Observable<f32> view_hp;
//   Observable<f32> model_hp { 100.0f };
//
//   TwoWayBinder<f32> bind(view_hp, model_hp);  // 両方が同期
//   model_hp.Set(50.0f);   // → view_hp も 50 になる
//   view_hp.Set(0.0f);     // → model_hp も 0 になる
//
//   // bind が破棄されたら同期解除
//
// 設計:
//   ・Set() はループしないように、現在更新中フラグで再帰防止
//   ・OneWayBinder<T>(src, dst) で片方向版もアリ (将来追加)
#pragma once

#include "mvvm/Observable.h"
#include "mvvm/Convert.h"
#include "memory/UniquePtr.h"

namespace acs {

template<typename T>
class TwoWayBinder {
public:
    TwoWayBinder(Observable<T>& a, Observable<T>& b) noexcept
        : _a(&a), _b(&b) {
        // 初期同期: a の値を b に反映 (ViewModel → View 想定)
        b.Set(a.Get());
        _h_a = a.Subscribe(&OnAChanged, this);
        _h_b = b.Subscribe(&OnBChanged, this);
    }

    ~TwoWayBinder() noexcept {
        if (_a) _a->Unsubscribe(_h_a);
        if (_b) _b->Unsubscribe(_h_b);
    }

    TwoWayBinder(const TwoWayBinder&) = delete;
    TwoWayBinder& operator=(const TwoWayBinder&) = delete;

private:
    static void OnAChanged(const T& v, void* user) noexcept {
        auto* self = static_cast<TwoWayBinder*>(user);
        if (self->_updating) return;
        self->_updating = true;
        self->_b->Set(v);
        self->_updating = false;
    }
    static void OnBChanged(const T& v, void* user) noexcept {
        auto* self = static_cast<TwoWayBinder*>(user);
        if (self->_updating) return;
        self->_updating = true;
        self->_a->Set(v);
        self->_updating = false;
    }

    Observable<T>*    _a = nullptr;
    Observable<T>*    _b = nullptr;
    ObservableHandle  _h_a;
    ObservableHandle  _h_b;
    bool              _updating = false;
};

// 片方向 (src → dst のみ)
template<typename T>
class OneWayBinder {
public:
    OneWayBinder(Observable<T>& src, Observable<T>& dst) noexcept
        : _src(&src), _dst(&dst) {
        dst.Set(src.Get());
        _h = src.Subscribe(&OnChanged, this);
    }
    ~OneWayBinder() noexcept {
        if (_src) _src->Unsubscribe(_h);
    }
    OneWayBinder(const OneWayBinder&) = delete;
    OneWayBinder& operator=(const OneWayBinder&) = delete;

private:
    static void OnChanged(const T& v, void* user) noexcept {
        auto* self = static_cast<OneWayBinder*>(user);
        self->_dst->Set(v);
    }
    Observable<T>*   _src = nullptr;
    Observable<T>*   _dst = nullptr;
    ObservableHandle _h;
};

// View → ViewModel 方向は OneWayBinder(view, vm) と書く (alias は提供しない、混乱の元)。

// 変換関数つき OneWay Binder (Src → Dst、別の型に変換)
//
// 使い方:
//   Observable<i32>      hp { 100 };
//   Observable<FString>   hp_text;
//   static FString IntToText(const i32& v, void* /*user*/) noexcept {
//       char buf[32]; std::snprintf(buf, sizeof(buf), "HP: %d", v);
//       return FString{buf};
//   }
//   OneWayConvertBinder<i32, FString> bind(hp, hp_text, &IntToText, nullptr);
template<typename Src, typename Dst>
class OneWayConvertBinder {
public:
    using ConvertFn = Dst (*)(const Src& v, void* user);

    OneWayConvertBinder(Observable<Src>& src, Observable<Dst>& dst,
                        ConvertFn fn, void* user) noexcept
        : _src(&src), _dst(&dst), _fn(fn), _user(user) {
        dst.Set(fn(src.Get(), user));
        _h = src.Subscribe(&OnChanged, this);
    }
    ~OneWayConvertBinder() noexcept {
        if (_src) _src->Unsubscribe(_h);
    }
    OneWayConvertBinder(const OneWayConvertBinder&)            = delete;
    OneWayConvertBinder& operator=(const OneWayConvertBinder&) = delete;

private:
    static void OnChanged(const Src& v, void* user) noexcept {
        auto* self = static_cast<OneWayConvertBinder*>(user);
        if (self->_fn) self->_dst->Set(self->_fn(v, self->_user));
    }
    Observable<Src>*  _src  = nullptr;
    Observable<Dst>*  _dst  = nullptr;
    ConvertFn         _fn   = nullptr;
    void*             _user = nullptr;
    ObservableHandle  _h;
};

// 1 回だけ src の値を dst にコピーする (live binding ではなく初期化用)。
// クラスではなく関数: lifetime を持たないので class にする意義がない。
template<typename T>
inline void CopyOnce(const Observable<T>& src, Observable<T>& dst) noexcept {
    dst.Set(src.Get());
}

// ============================================================================
// Bind(src, dst) — 暗黙変換つき汎用バインドファクトリ
// ----------------------------------------------------------------------------
// 同じ型なら OneWayBinder、違う型なら TDefaultConverter<Src, Dst> を経由した
// OneWayConvertBinder を返す。返り値は prvalue なので C++17 の copy elision で
// auto に直接受けられる。
//
// 例:
//   Observable<f32> hp{100};
//   Observable<FString> hp_text;
//   auto b = Bind(hp, hp_text);    // f32 → FString 自動変換
//
//   Observable<i32> a, b;
//   auto b2 = Bind(a, b);          // 同型 → OneWayBinder<i32>
// ============================================================================
template<typename T>
inline OneWayBinder<T> Bind(Observable<T>& src, Observable<T>& dst) noexcept {
    return OneWayBinder<T>(src, dst);
}

template<typename Src, typename Dst>
inline OneWayConvertBinder<Src, Dst> Bind(Observable<Src>& src, Observable<Dst>& dst) noexcept {
    return OneWayConvertBinder<Src, Dst>(
        src, dst, &mvvm::TDefaultConverter<Src, Dst>::Convert, nullptr);
}

// 双方向版
template<typename T>
inline TwoWayBinder<T> TwoWayBind(Observable<T>& a, Observable<T>& b) noexcept {
    return TwoWayBinder<T>(a, b);
}

// ============================================================================
// MakeBind / MakeTwoWayBind / MakeBindConvert — TUniquePtr 版
// ----------------------------------------------------------------------------
// Bind() の生 OneWayBinder を返す版だと、コピー/ムーブ不可なため member 変数として
// 持てない (member 初期化順、ヒープ確保が必要)。Make* 系は TUniquePtr を返すので、
// クラスメンバとして自然に持てる + dtor で自動的に Unsubscribe される。
//
//   class MyApp {
//       TUniquePtr<OneWayBinder<f32>> _bind;
//       void OnStart() {
//           _bind = MakeBind(vm.hp, view.hp);
//           // OnShutdown で _bind.Reset() するか、デストラクタ任せ
//       }
//   };
// ============================================================================
template<typename T>
inline TUniquePtr<OneWayBinder<T>> MakeBind(Observable<T>& src, Observable<T>& dst) noexcept {
    return MakeUnique<OneWayBinder<T>>(src, dst);
}

template<typename T>
inline TUniquePtr<TwoWayBinder<T>> MakeTwoWayBind(Observable<T>& a, Observable<T>& b) noexcept {
    return MakeUnique<TwoWayBinder<T>>(a, b);
}

template<typename Src, typename Dst>
inline TUniquePtr<OneWayConvertBinder<Src, Dst>>
MakeBindConvert(Observable<Src>& src, Observable<Dst>& dst) noexcept {
    return MakeUnique<OneWayConvertBinder<Src, Dst>>(
        src, dst, &mvvm::TDefaultConverter<Src, Dst>::Convert, nullptr);
}

template<typename Src, typename Dst>
inline TUniquePtr<OneWayConvertBinder<Src, Dst>>
MakeBindConvert(Observable<Src>& src, Observable<Dst>& dst,
                Dst (*fn)(const Src&, void*), void* user) noexcept {
    return MakeUnique<OneWayConvertBinder<Src, Dst>>(src, dst, fn, user);
}

} // namespace acs
