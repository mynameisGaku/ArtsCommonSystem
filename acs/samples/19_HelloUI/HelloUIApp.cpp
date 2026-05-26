// SPDX-License-Identifier: Apache-2.0
// HelloUI — Application 実装。
#include "HelloUIApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "foundation/Log.h"

using namespace acs;

namespace helloui {

void HelloUIApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f));
    ACS_SAMPLE_INIT(_ui.Init(*dev, GetRenderer().ColorFormat(), &_font));

    BuildUI();

    // f32 → String の変換 binder で HP の表示を自動更新
    _hp_text_binder  = MakeBindConvert<f32, String>(_vm.hp, _vm.hp_label);
    _hp_label_binder = MakeBind(_vm.hp_label, _lbl_hp->text);

    ACS_LOG_INFO("HelloUI initialized");
}

void HelloUIApp::OnUpdate(f32 /*dt*/) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    // Widget tree への入力配信
    _input.Dispatch(*_root);
}

void HelloUIApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;
    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();

    _ui.Render(*_root, *cl, sw, sh);
}

void HelloUIApp::OnShutdown() noexcept {
    // Binder と _root (Widget tree) は member 宣言順の自動破棄に任せる:
    //   ・declaration order: _root → ... → _hp_*_binder (binder 系が後)
    //   ・dtor order は declaration order の REVERSE → binder が先に死ぬ
    //   ・→ Binder::~ が Observable::Unsubscribe する時点で Observable は生きてる
    // ここで明示的に _root.Reset() すると Observable<f32>::value 等が先に死に、
    // 後の binder の ~ が dangling pointer を叩いて AV する。
    _ui.Shutdown();
    _font.Shutdown();
}

void HelloUIApp::BuildUI() noexcept {
    // ルートは画面全体に配置される StackPanel (左寄せパネル)
    _root = MakeUnique<StackPanel>();
    _root->dir = EStackDir::Vertical;
    _root->padding = UiPadding{ 24, 24, 24, 24 };
    _root->spacing = 8.0f;

    // タイトル
    auto* title = _root->Add<Label>("=== ACS UI Demo ===");
    title->requested.h = 28.0f;

    // HP 表示ラベル
    _lbl_hp = _root->Add<Label>("HP: 100");

    // HP slider (TwoWayBinder で VM と同期)
    auto* sl_hp = _root->Add<Slider>(0.0f, 100.0f);
    sl_hp->requested.w = 360.0f;
    _hp_slider_binder = MakeTwoWayBind(_vm.hp, sl_hp->value);

    // Mana slider
    _root->Add<Label>("Mana");
    auto* sl_mp = _root->Add<Slider>(0.0f, 100.0f);
    _mp_slider_binder = MakeTwoWayBind(_vm.mana, sl_mp->value);

    // 無敵チェック
    auto* cb = _root->Add<Checkbox>("無敵モード");
    _invincible_binder = MakeTwoWayBind(_vm.invincible, cb->checked);

    // 名前入力
    _root->Add<Label>("名前");
    auto* ti = _root->Add<TextInput>();
    ti->requested.w = 280.0f;
    _name_binder = MakeTwoWayBind(_vm.name, ti->text);

    // 攻撃ボタン
    auto* btn = _root->Add<Button>("攻撃を受ける (-10 HP)");
    btn->requested.w = 240.0f;
    btn->clicked.Subscribe([](const bool& v, void* user){
        if (!v) return;
        auto* self = static_cast<HelloUIApp*>(user);
        if (self->_vm.invincible.Get()) {
            ACS_LOG_INFO("ダメージ無効 (無敵中)");
            return;
        }
        self->_vm.hp.Set(self->_vm.hp.Get() - 10.0f);
    }, this);

    // 回復ボタン
    auto* btn_heal = _root->Add<Button>("満タン回復");
    btn_heal->requested.w = 240.0f;
    btn_heal->clicked.Subscribe([](const bool& v, void* user){
        if (!v) return;
        auto* self = static_cast<HelloUIApp*>(user);
        self->_vm.hp.Set(100.0f);
        self->_vm.mana.Set(100.0f);
    }, this);

    _root->Add<Label>("Esc: 終了");
}

} // namespace helloui
