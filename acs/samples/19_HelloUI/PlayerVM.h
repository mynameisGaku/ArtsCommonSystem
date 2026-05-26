// SPDX-License-Identifier: Apache-2.0
// HelloUI — Player FViewModel。
//
// hp / mana / invincible / name の FObservable プロパティと、HP の表示用に
// f32 から変換した文字列を載せる hp_label を持つ。View (FSlider / FCheckbox /
// FTextInput) と FTwoWayBinder で接続する。
#pragma once

#include "mvvm/ViewModel.h"
#include "foundation/Types.h"

namespace helloui {

class PlayerVM : public acs::FViewModel {
public:
    acs::FObservable<acs::f32>    hp        { 100.0f };
    acs::FObservable<acs::f32>    mana      { 50.0f };
    acs::FObservable<bool>        invincible{ false };
    acs::FObservable<acs::FString> name      { acs::FString{"勇者"} };
    // HP の文字表示用。hp (f32) から変換 binder (MakeBindConvert) で自動同期する。
    acs::FObservable<acs::FString> hp_label;
};

} // namespace helloui
