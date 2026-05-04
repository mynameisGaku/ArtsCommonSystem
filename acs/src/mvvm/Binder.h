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

} // namespace acs
