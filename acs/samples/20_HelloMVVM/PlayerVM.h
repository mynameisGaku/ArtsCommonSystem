// SPDX-License-Identifier: Apache-2.0
// HelloMVVM — ViewModel 定義。
// Observable<T> / ObservableArray<T> / Command を組み合わせた最小 PlayerVM。
// HelloMVVMApp が hp / mana / level / inventory / attack をスライダー・
// ボタンに bind して MVVM の各機能を順番に紹介する。
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
    acs::Observable<acs::i32>     class_idx { 0 };  // Combo 用 (0=戦士, 1=魔法使い, 2=盗賊)

    // インベントリ
    acs::ObservableArray<acs::i32> inventory;

    // Attack コマンド: invincible が false のときだけ実行可能
    // (Observable<bool>* で can_execute を渡す。null なら常時実行可能)
    acs::Command attack {
        [](void* user) {
            auto* self = static_cast<PlayerVM*>(user);
            self->hp.Set(self->hp.Get() - 10.0f);
        },
        this,
        nullptr   // 常時実行可能 (invincible は別途見て切替)
    };
};

} // namespace hellomvvm
