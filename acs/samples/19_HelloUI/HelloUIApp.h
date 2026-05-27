// SPDX-License-Identifier: Apache-2.0
// HelloUI — FApplication 派生クラス。
//
// ACS 純正 UI フレームワーク (src/ui/) を retained mode で使い、
// PlayerVM の Observable を Slider/Checkbox/TextInput に MakeTwoWayBind で接続する。
//
// 学習ポイント:
//   - Widget tree (StackPanel + 子) で UI を組み立て
//   - Observable<T> プロパティ経由で VM ↔ UI を接続
//   - MakeTwoWayBind で双方向自動同期
//   - MakeBindConvert で f32 → FString 変換 (HP 値 → ラベル文字)
//
// 破棄順の注意 (詳細は HelloUIApp.cpp::OnShutdown のコメント参照):
//   _root / Binder 群はメンバ宣言順の自動破棄に任せる。手動で _root.Reset() を
//   先に呼ぶと、Binder の dtor が dangling pointer を叩いて AV する。
#pragma once

#include "app/Application.h"
#include "ui/UiRenderer.h"
#include "ui/Widgets.h"
#include "render/Font.h"
#include "memory/UniquePtr.h"
#include "foundation/Types.h"

#include "PlayerVM.h"

namespace helloui {

class HelloUIApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    void BuildUI() noexcept;

    acs::Font                                                _font;
    acs::FUiRenderer                                          _ui;
    acs::FUiInput                                             _input;
    PlayerVM                                                 _vm;
    acs::TUniquePtr<acs::StackPanel>                          _root;
    acs::Label*                                              _lbl_hp = nullptr;
    // Binder 群は _root より後に宣言 → dtor で先に死ぬ。OnShutdown コメント参照。
    acs::TUniquePtr<acs::FTwoWayBinder<acs::f32>>              _hp_slider_binder;
    acs::TUniquePtr<acs::FTwoWayBinder<acs::f32>>              _mp_slider_binder;
    acs::TUniquePtr<acs::FTwoWayBinder<bool>>                  _invincible_binder;
    acs::TUniquePtr<acs::FTwoWayBinder<acs::FString>>           _name_binder;
    acs::TUniquePtr<acs::OneWayConvertBinder<acs::f32, acs::FString>> _hp_text_binder;
    acs::TUniquePtr<acs::OneWayBinder<acs::FString>>           _hp_label_binder;
};

} // namespace helloui
