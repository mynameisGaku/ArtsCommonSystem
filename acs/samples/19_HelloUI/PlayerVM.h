// SPDX-License-Identifier: Apache-2.0
// HelloUI — Player ViewModel。
//
// hp / mana / invincible / name の Observable プロパティと、HP の表示用に
// f32 から変換した文字列を載せる hp_label を持つ。View (Slider / Checkbox /
// TextInput) と TwoWayBinder で接続する。
#pragma once

#include "mvvm/ViewModel.h"
#include "foundation/Types.h"

namespace helloui {

class PlayerVM : public acs::ViewModel {
public:
    acs::Observable<acs::f32>    hp        { 100.0f };
    acs::Observable<acs::f32>    mana      { 50.0f };
    acs::Observable<bool>        invincible{ false };
    acs::Observable<acs::String> name      { acs::String{"勇者"} };
    // HP の文字表示用。hp (f32) から変換 binder (MakeBindConvert) で自動同期する。
    acs::Observable<acs::String> hp_label;
};

} // namespace helloui
