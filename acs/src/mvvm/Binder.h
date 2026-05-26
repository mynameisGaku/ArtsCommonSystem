// SPDX-License-Identifier: Apache-2.0
// FTwoWayBinder<T> — 2 つの FObservable<T> を双方向同期させる
//
// 使い方:
//   FObservable<f32> view_hp;
//   FObservable<f32> model_hp { 100.0f };
//
//   FTwoWayBinder<f32> bind(view_hp, model_hp);  // 両方が同期
//   model_hp.Set(50.0f);   // → view_hp も 50 になる
//   view_hp.Set(0.0f);     // → model_hp も 0 になる
//
//   // bind が破棄されたら同期解除
//
// 設計:
//   ・Set() はループしないように、現在更新中フラグで再帰防止
//   ・FOneWayBinder<T>(src, dst) で片方向版もアリ (将来追加)
#pragma once

#include "mvvm/Observable.h"
#include "mvvm/Convert.h"
#include "memory/UniquePtr.h"

namespace acs {

template<typename T>
class FTwoWayBinder {
public:
    FTwoWayBinder(FObservable<T>& a, FObservable<T>& b) noexcept
        : _a(&a), _b(&b) {
        // 初期同期: a の値を b に反映 (FViewModel → View 想定)
        b.Set(a.Get());
        _h_a = a.Subscribe(&OnAChanged, this);
        _h_b = b.Subscribe(&OnBChanged, this);
    }

    ~FTwoWayBinder() noexcept {
        if (_a) _a->Unsubscribe(_h_a);
        if (_b) _b->Unsubscribe(_h_b);
    }

    FTwoWayBinder(const FTwoWayBinder&) = delete;
    FTwoWayBinder& operator=(const FTwoWayBinder&) = delete;

private:
    static void OnAChanged(const T& v, void* user) noexcept {
        auto* self = static_cast<FTwoWayBinder*>(user);
        if (self->_updating) return;
        self->_updating = true;
        self->_b->Set(v);
        self->_updating = false;
    }
    static void OnBChanged(const T& v, void* user) noexcept {
        auto* self = static_cast<FTwoWayBinder*>(user);
        if (self->_updating) return;
        self->_updating = true;
        self->_a->Set(v);
        self->_updating = false;
    }

    FObservable<T>*    _a = nullptr;
    FObservable<T>*    _b = nullptr;
    FObservableHandle  _h_a;
    FObservableHandle  _h_b;
    bool              _updating = false;
};

// 片方向 (src → dst のみ)
template<typename T>
class FOneWayBinder {
public:
    FOneWayBinder(FObservable<T>& src, FObservable<T>& dst) noexcept
        : _src(&src), _dst(&dst) {
        dst.Set(src.Get());
        _h = src.Subscribe(&OnChanged, this);
    }
    ~FOneWayBinder() noexcept {
        if (_src) _src->Unsubscribe(_h);
    }
    FOneWayBinder(const FOneWayBinder&) = delete;
    FOneWayBinder& operator=(const FOneWayBinder&) = delete;

private:
    static void OnChanged(const T& v, void* user) noexcept {
        auto* self = static_cast<FOneWayBinder*>(user);
        self->_dst->Set(v);
    }
    FObservable<T>*   _src = nullptr;
    FObservable<T>*   _dst = nullptr;
    FObservableHandle _h;
};

// View → FViewModel 方向は FOneWayBinder(view, vm) と書く (alias は提供しない、混乱の元)。

// 変換関数つき OneWay Binder (Src → Dst、別の型に変換)
//
// 使い方:
//   FObservable<i32>      hp { 100 };
//   FObservable<FString>   hp_text;
//   static FString IntToText(const i32& v, void* /*user*/) noexcept {
//       char buf[32]; std::snprintf(buf, sizeof(buf), "HP: %d", v);
//       return FString{buf};
//   }
//   FOneWayConvertBinder<i32, FString> bind(hp, hp_text, &IntToText, nullptr);
template<typename Src, typename Dst>
class FOneWayConvertBinder {
public:
    using ConvertFn = Dst (*)(const Src& v, void* user);

    FOneWayConvertBinder(FObservable<Src>& src, FObservable<Dst>& dst,
                        ConvertFn fn, void* user) noexcept
        : _src(&src), _dst(&dst), _fn(fn), _user(user) {
        dst.Set(fn(src.Get(), user));
        _h = src.Subscribe(&OnChanged, this);
    }
    ~FOneWayConvertBinder() noexcept {
        if (_src) _src->Unsubscribe(_h);
    }
    FOneWayConvertBinder(const FOneWayConvertBinder&)            = delete;
    FOneWayConvertBinder& operator=(const FOneWayConvertBinder&) = delete;

private:
    static void OnChanged(const Src& v, void* user) noexcept {
        auto* self = static_cast<FOneWayConvertBinder*>(user);
        if (self->_fn) self->_dst->Set(self->_fn(v, self->_user));
    }
    FObservable<Src>*  _src  = nullptr;
    FObservable<Dst>*  _dst  = nullptr;
    ConvertFn         _fn   = nullptr;
    void*             _user = nullptr;
    FObservableHandle  _h;
};

// 1 回だけ src の値を dst にコピーする (live binding ではなく初期化用)。
// クラスではなく関数: lifetime を持たないので class にする意義がない。
template<typename T>
inline void CopyOnce(const FObservable<T>& src, FObservable<T>& dst) noexcept {
    dst.Set(src.Get());
}

// ============================================================================
// Bind(src, dst) — 暗黙変換つき汎用バインドファクトリ
// ----------------------------------------------------------------------------
// 同じ型なら FOneWayBinder、違う型なら TDefaultConverter<Src, Dst> を経由した
// FOneWayConvertBinder を返す。返り値は prvalue なので C++17 の copy elision で
// auto に直接受けられる。
//
// 例:
//   FObservable<f32> hp{100};
//   FObservable<FString> hp_text;
//   auto b = Bind(hp, hp_text);    // f32 → FString 自動変換
//
//   FObservable<i32> a, b;
//   auto b2 = Bind(a, b);          // 同型 → FOneWayBinder<i32>
// ============================================================================
template<typename T>
inline FOneWayBinder<T> Bind(FObservable<T>& src, FObservable<T>& dst) noexcept {
    return FOneWayBinder<T>(src, dst);
}

template<typename Src, typename Dst>
inline FOneWayConvertBinder<Src, Dst> Bind(FObservable<Src>& src, FObservable<Dst>& dst) noexcept {
    return FOneWayConvertBinder<Src, Dst>(
        src, dst, &mvvm::TDefaultConverter<Src, Dst>::Convert, nullptr);
}

// 双方向版
template<typename T>
inline FTwoWayBinder<T> TwoWayBind(FObservable<T>& a, FObservable<T>& b) noexcept {
    return FTwoWayBinder<T>(a, b);
}

// ============================================================================
// MakeBind / MakeTwoWayBind / MakeBindConvert — TUniquePtr 版
// ----------------------------------------------------------------------------
// Bind() の生 FOneWayBinder を返す版だと、コピー/ムーブ不可なため member 変数として
// 持てない (member 初期化順、ヒープ確保が必要)。Make* 系は TUniquePtr を返すので、
// クラスメンバとして自然に持てる + dtor で自動的に Unsubscribe される。
//
//   class MyApp {
//       TUniquePtr<FOneWayBinder<f32>> _bind;
//       void OnStart() {
//           _bind = MakeBind(vm.hp, view.hp);
//           // OnShutdown で _bind.Reset() するか、デストラクタ任せ
//       }
//   };
// ============================================================================
template<typename T>
inline TUniquePtr<FOneWayBinder<T>> MakeBind(FObservable<T>& src, FObservable<T>& dst) noexcept {
    return MakeUnique<FOneWayBinder<T>>(src, dst);
}

template<typename T>
inline TUniquePtr<FTwoWayBinder<T>> MakeTwoWayBind(FObservable<T>& a, FObservable<T>& b) noexcept {
    return MakeUnique<FTwoWayBinder<T>>(a, b);
}

template<typename Src, typename Dst>
inline TUniquePtr<FOneWayConvertBinder<Src, Dst>>
MakeBindConvert(FObservable<Src>& src, FObservable<Dst>& dst) noexcept {
    return MakeUnique<FOneWayConvertBinder<Src, Dst>>(
        src, dst, &mvvm::TDefaultConverter<Src, Dst>::Convert, nullptr);
}

template<typename Src, typename Dst>
inline TUniquePtr<FOneWayConvertBinder<Src, Dst>>
MakeBindConvert(FObservable<Src>& src, FObservable<Dst>& dst,
                Dst (*fn)(const Src&, void*), void* user) noexcept {
    return MakeUnique<FOneWayConvertBinder<Src, Dst>>(src, dst, fn, user);
}

} // namespace acs
