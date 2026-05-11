// ACS の HelloUI サンプル
//
// やってること:
//   ・ACS 純正 UI フレームワーク (src/ui/) を使ってステータスパネルを構築
//   ・PlayerVM の Observable を Slider/Checkbox/TextInput に MakeTwoWayBind
//   ・Button のクリックで HP を減らす (Subscribe 経由)
//   ・ImGui 不要、SpriteBatch + Font + ACS Widget のみ
//
// 学習ポイント:
//   ・Widget tree (StackPanel + 子) で UI を retained mode で構築
//   ・Observable<T> プロパティ経由で VM ↔ UI を接続
//   ・MakeTwoWayBind で双方向自動同期
//   ・MakeBindConvert で f32 → String 変換 (HP 値 → ラベル文字)

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "app/Sample.h"
#include "platform/Input.h"

#include "ui/UiRenderer.h"
#include "ui/Widgets.h"
#include "mvvm/ViewModel.h"

#include "render/Font.h"

#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

class PlayerVM : public ViewModel {
public:
    Observable<f32>    hp        { 100.0f };
    Observable<f32>    mana      { 50.0f };
    Observable<bool>   invincible{ false };
    Observable<String> name      { String{"勇者"} };
    Observable<String> hp_label;     // HP の文字表示用 (Derived 不使用、変換 binder で同期)
};

class HelloUI : public Application {
public:
    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) { Quit(); return; }

        ACS_SAMPLE_INIT(Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f));
        ACS_SAMPLE_INIT(_ui.Init(*dev, GetRenderer().ColorFormat(), &_font));

        BuildUI();

        // f32 → String の変換 binder で HP の表示を自動更新
        _hp_text_binder = MakeBindConvert<f32, String>(_vm.hp, _vm.hp_label);
        _hp_label_binder = MakeBind(_vm.hp_label, _lbl_hp->text);

        ACS_LOG_INFO("HelloUI initialized");
    }

    void OnUpdate(f32 /*dt*/) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();
        // Widget tree への入力配信
        _input.Dispatch(*_root);
    }

    void OnRender() noexcept override {
        IRhiCommandList* cl = GetRenderer().CommandList();
        if (!cl) return;
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();

        _ui.Render(*_root, *cl, sw, sh);
    }

    void OnShutdown() noexcept override {
        // Binder と _root (Widget tree) は member 宣言順の自動破棄に任せる:
        //   ・declaration order: _root → ... → _hp_*_binder (binder 系が後)
        //   ・dtor order は declaration order の REVERSE → binder が先に死ぬ
        //   ・→ Binder::~ が Observable::Unsubscribe する時点で Observable は生きてる
        // ここで明示的に _root.Reset() すると Observable<f32>::value 等が先に死に、
        // 後の binder の ~ が dangling pointer を叩いて AV する。
        _ui.Shutdown();
        _font.Shutdown();
    }

private:
    void BuildUI() noexcept {
        // ルートは画面全体に配置される StackPanel (左寄せパネル)
        _root = MakeUnique<StackPanel>();
        _root->dir = StackDir::Vertical;
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
            auto* self = static_cast<HelloUI*>(user);
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
            auto* self = static_cast<HelloUI*>(user);
            self->_vm.hp.Set(100.0f);
            self->_vm.mana.Set(100.0f);
        }, this);

        _root->Add<Label>("Esc: 終了");
    }

    Font                                          _font;
    UiRenderer                                    _ui;
    UiInput                                       _input;
    PlayerVM                                      _vm;
    UniquePtr<StackPanel>                         _root;
    Label*                                        _lbl_hp = nullptr;
    UniquePtr<TwoWayBinder<f32>>                  _hp_slider_binder;
    UniquePtr<TwoWayBinder<f32>>                  _mp_slider_binder;
    UniquePtr<TwoWayBinder<bool>>                 _invincible_binder;
    UniquePtr<TwoWayBinder<String>>               _name_binder;
    UniquePtr<OneWayConvertBinder<f32, String>>   _hp_text_binder;
    UniquePtr<OneWayBinder<String>>               _hp_label_binder;
};

ACS_DEFINE_MAIN(HelloUI)
