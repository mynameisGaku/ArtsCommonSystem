// SPDX-License-Identifier: Apache-2.0
// HelloMVVM — FViewModel 定義。
// TObservable<T> / TObservableArray<T> / FCommand を組み合わせた最小 FPlayerVm。
// 全フィールドを public にして View 側から直接 bind できるようにしてある
// (FViewModel は通常 public フィールドのままで View に晒すのが MVVM の流儀)。
#pragma once

#include "mvvm/ViewModel.h"
#include "mvvm/ObservableArray.h"
#include "mvvm/Command.h"
#include "container/String.h"
#include "math/Vec.h"

namespace hellomvvm {

class FPlayerVm : public acs::FViewModel {
public:
    acs::TObservable<acs::f32>     hp        { 100.0f };
    acs::TObservable<acs::f32>     max_hp    { 100.0f };
    acs::TObservable<acs::f32>     mana      { 50.0f };
    acs::TObservable<acs::i32>     level     { 1 };
    acs::TObservable<bool>         invincible{ false };
    acs::TObservable<acs::FString>  name      { acs::FString{"勇者"} };
    acs::TObservable<acs::FVec3>    color     { acs::FVec3{1.0f, 0.85f, 0.4f} };
    acs::TObservable<acs::i32>     class_idx { 0 };   // 0=戦士 / 1=魔法使い / 2=盗賊

    acs::TObservableArray<acs::i32> inventory;

    // FCommand の第 3 引数は can_execute (TObservable<bool>*) で、null なら
    // 常時実行可能。ここでは View 側で BeginDisabled/EndDisabled で
    // gate しているため null を渡す。
    acs::FCommand attack {
        [](void* user) {
            auto* self = static_cast<FPlayerVm*>(user);
            self->hp.Set(self->hp.Get() - 10.0f);
        },
        this,
        nullptr
    };
};

} // namespace hellomvvm
