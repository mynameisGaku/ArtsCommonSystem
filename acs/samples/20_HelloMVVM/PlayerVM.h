// SPDX-License-Identifier: Apache-2.0
// HelloMVVM — ViewModel 定義。
// Observable<T> / ObservableArray<T> / Command を組み合わせた最小 PlayerVM。
// 全フィールドを public にして View 側から直接 bind できるようにしてある
// (ViewModel は通常 public フィールドのままで View に晒すのが MVVM の流儀)。
#pragma once

#include "mvvm/ViewModel.h"
#include "mvvm/ObservableArray.h"
#include "mvvm/Command.h"
#include "container/String.h"
#include "math/Vec.h"

namespace hellomvvm {

class PlayerVM : public acs::ViewModel {
public:
    acs::Observable<acs::f32>     hp        { 100.0f };
    acs::Observable<acs::f32>     max_hp    { 100.0f };
    acs::Observable<acs::f32>     mana      { 50.0f };
    acs::Observable<acs::i32>     level     { 1 };
    acs::Observable<bool>         invincible{ false };
    acs::Observable<acs::String>  name      { acs::String{"勇者"} };
    acs::Observable<acs::Vec3>    color     { acs::Vec3{1.0f, 0.85f, 0.4f} };
    acs::Observable<acs::i32>     class_idx { 0 };   // 0=戦士 / 1=魔法使い / 2=盗賊

    acs::ObservableArray<acs::i32> inventory;

    // Command の第 3 引数は can_execute (Observable<bool>*) で、null なら
    // 常時実行可能。ここでは View 側で BeginDisabled/EndDisabled で
    // gate しているため null を渡す。
    acs::Command attack {
        [](void* user) {
            auto* self = static_cast<PlayerVM*>(user);
            self->hp.Set(self->hp.Get() - 10.0f);
        },
        this,
        nullptr
    };
};

} // namespace hellomvvm
