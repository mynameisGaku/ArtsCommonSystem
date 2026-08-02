// SPDX-License-Identifier: Apache-2.0
// HelloUI — CApplication 実装。
#include "HelloUIApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "foundation/Log.h"

using namespace acs;

namespace helloui {

void CHelloUiApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(FSample::TryLoadDefaultUIFont(m_Font, *dev, 18.0f));
    ACS_SAMPLE_INIT(m_Ui.Init(*dev, GetRenderer().ColorFormat(), &m_Font));

    BuildUI();

    // hp (f32) → hp_label (FString) → m_LblHp->text の二段同期。
    // Label が FString しか持てないため、間に f32→FString 変換 binder を挟む。
    m_HpTextBinder  = MakeBindConvert<f32, FString>(m_Vm.hp, m_Vm.hp_label);
    m_HpLabelBinder = MakeBind(m_Vm.hp_label, m_LblHp->text);

    ACS_LOG_INFO("HelloUI initialized");
}

void CHelloUiApp::OnUpdate(f32 /*dt*/) noexcept {
    if (CInput::IsKeyPressed(EKey::Escape)) Quit();
    m_Input.Dispatch(*m_Root);
}

void CHelloUiApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;
    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();

    m_Ui.Render(*m_Root, *cl, sw, sh);
}

void CHelloUiApp::OnShutdown() noexcept {
    // Binder と m_Root (Widget tree) は member 宣言順の自動破棄に任せる。
    // dtor は declaration order の REVERSE で走るため、後ろに宣言した binder
    // が先に死に、Observable がまだ生きている状態で Unsubscribe できる。
    // ここで明示的に m_Root.Reset() すると TObservable<f32>::value 等が先に死に、
    // 後の binder の ~ が dangling pointer を叩いて AV する。
    m_Ui.Shutdown();
    m_Font.Shutdown();
}

void CHelloUiApp::BuildUI() noexcept {
    // 画面左上に縦並びパネルを置く。requested.w / requested.h を未指定にした
    // widget は Label の content size か StackPanel の内側幅にフィットする。
    m_Root = MakeUnique<AStackPanel>();
    m_Root->dir = EStackDir::Vertical;
    m_Root->padding = FUiPadding{ 24, 24, 24, 24 };
    m_Root->spacing = 8.0f;

    auto* title = m_Root->Add<ALabel>("=== ACS UI Demo ===");
    title->requested.h = 28.0f;

    m_LblHp = m_Root->Add<ALabel>("HP: 100");

    // Slider/Checkbox/TextInput は TTwoWayBinder で VM と双方向同期する。
    // どちらの側を書き換えても他方が追従する。
    auto* sl_hp = m_Root->Add<ASlider>(0.0f, 100.0f);
    sl_hp->requested.w = 360.0f;
    m_HpSliderBinder = MakeTwoWayBind(m_Vm.hp, sl_hp->value);

    m_Root->Add<ALabel>("Mana");
    auto* sl_mp = m_Root->Add<ASlider>(0.0f, 100.0f);
    m_MpSliderBinder = MakeTwoWayBind(m_Vm.mana, sl_mp->value);

    auto* cb = m_Root->Add<ACheckbox>("無敵モード");
    m_InvincibleBinder = MakeTwoWayBind(m_Vm.invincible, cb->checked);

    m_Root->Add<ALabel>("名前");
    auto* ti = m_Root->Add<ATextInput>();
    ti->requested.w = 280.0f;
    m_NameBinder = MakeTwoWayBind(m_Vm.name, ti->text);

    // Button::clicked は bool Observable。push down (true) と release (false)
    // の両方が通知されるため v==false は無視して click edge だけ拾う。
    auto* btn = m_Root->Add<AButton>("攻撃を受ける (-10 HP)");
    btn->requested.w = 240.0f;
    btn->clicked.Subscribe([](const bool& v, void* user){
        if (!v) return;
        auto* self = static_cast<CHelloUiApp*>(user);
        if (self->m_Vm.invincible.Get()) {
            ACS_LOG_INFO("ダメージ無効 (無敵中)");
            return;
        }
        self->m_Vm.hp.Set(self->m_Vm.hp.Get() - 10.0f);
    }, this);

    auto* btn_heal = m_Root->Add<AButton>("満タン回復");
    btn_heal->requested.w = 240.0f;
    btn_heal->clicked.Subscribe([](const bool& v, void* user){
        if (!v) return;
        auto* self = static_cast<CHelloUiApp*>(user);
        self->m_Vm.hp.Set(100.0f);
        self->m_Vm.mana.Set(100.0f);
    }, this);

    m_Root->Add<ALabel>("Esc: 終了");
}

} // namespace helloui
