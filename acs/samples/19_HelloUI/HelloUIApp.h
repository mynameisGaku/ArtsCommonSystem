// SPDX-License-Identifier: Apache-2.0
// HelloUI — FApplication 派生クラス。
//
// ACS 純正 UI フレームワーク (src/ui/) を retained mode で使い、
// PlayerVM の Observable を Slider/Checkbox/TextInput に MakeTwoWayBind で接続する。
//
// 学習ポイント:
//   - Widget tree (StackPanel + 子) で UI を組み立て
//   - TObservable<T> プロパティ経由で VM ↔ UI を接続
//   - MakeTwoWayBind で双方向自動同期
//   - MakeBindConvert で f32 → FString 変換 (HP 値 → ラベル文字)
//
// 破棄順の注意 (詳細は HelloUIApp.cpp::OnShutdown のコメント参照):
//   m_Root / Binder 群はメンバ宣言順の自動破棄に任せる。手動で m_Root.Reset() を
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

class FHelloUiApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    void BuildUI() noexcept;

    acs::FFont                                                m_Font;
    acs::FUiRenderer                                          m_Ui;
    acs::FUiInput                                             m_Input;
    FPlayerVm                                                 m_Vm;
    acs::TUniquePtr<acs::FStackPanel>                          m_Root;
    acs::FLabel*                                              m_LblHp = nullptr;
    // Binder 群は m_Root より後に宣言 → dtor で先に死ぬ。OnShutdown コメント参照。
    acs::TUniquePtr<acs::TTwoWayBinder<acs::f32>>              m_HpSliderBinder;
    acs::TUniquePtr<acs::TTwoWayBinder<acs::f32>>              m_MpSliderBinder;
    acs::TUniquePtr<acs::TTwoWayBinder<bool>>                  m_InvincibleBinder;
    acs::TUniquePtr<acs::TTwoWayBinder<acs::FString>>           m_NameBinder;
    acs::TUniquePtr<acs::TOneWayConvertBinder<acs::f32, acs::FString>> m_HpTextBinder;
    acs::TUniquePtr<acs::TOneWayBinder<acs::FString>>           m_HpLabelBinder;
};

} // namespace helloui
