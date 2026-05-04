// ACS の HelloMVVM サンプル
//
// 動作:
//   ・PlayerViewModel に HP / Mana / Lv / 名前 / 無敵フラグの Observable
//   ・ImGui パネルで slider / checkbox 経由で編集
//   ・値変更を Subscribe で監視してログに出す (View → ViewModel → ログ)
//   ・別ウィンドウで OneWay バインドで HP のミラーを表示 (バインド連鎖の確認)
//
// 学習ポイント:
//   ・Observable<T>::Set() で全 Subscriber に通知が伝播
//   ・OneWayBinder で「値の自動同期」が無線で実現
//   ・MVVM 原則: View は ViewModel の Observable をだけ見る (モデルを直接触らない)
//
// 注: ACS_RENDER_DILIGENT との関係はない。Imgui モジュールが有効ならビルド可。

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "platform/Input.h"
#include "imgui/ImGuiContext.h"

#include "mvvm/ViewModel.h"
#include "mvvm/ImguiBindings.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;

class PlayerViewModel : public ViewModel {
public:
    Observable<f32>  hp        { 100.0f };
    Observable<f32>  mana      { 50.0f };
    Observable<i32>  level     { 1 };
    Observable<bool> invincible{ false };
};

class HelloMVVM : public Application {
public:
    void OnStart() noexcept override {
        if (_imgui.Init(GetWindow(), GetRenderer()).IsErr()) { Quit(); return; }

        // ViewModel の値変更をログに出す Subscriber を貼る
        _vm.hp.Subscribe([](const f32& v, void*){
            ACS_LOG_INFO("VM hp changed: %.1f", v);
        }, nullptr);
        _vm.invincible.Subscribe([](const bool& v, void*){
            ACS_LOG_INFO("VM invincible: %s", v ? "ON" : "OFF");
        }, nullptr);

        // OneWay バインド: メイン VM の hp が hp_mirror に連動
        // (View → ViewModel → 別 VM のチェーンを示す)
        _hp_mirror_binder = new OneWayBinder<f32>(_vm.hp, _hp_mirror);

        ACS_LOG_INFO("HelloMVVM initialized");
    }

    void OnUpdate(f32 /*dt*/) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();
    }

    void OnRender() noexcept override {
        _imgui.NewFrame();

        // ---- メインパネル: 編集可能 ----
        ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_FirstUseEver);
        ImGui::Begin("Player ViewModel");
        ImGui::Text("MVVM 双方向バインドのデモ");
        ImGui::Separator();
        mvvm::imgui::Bind("HP",   _vm.hp,   0.0f, 100.0f);
        mvvm::imgui::Bind("Mana", _vm.mana, 0.0f, 100.0f);
        mvvm::imgui::Bind("Lv",   _vm.level, 1, 99);
        mvvm::imgui::Bind("無敵", _vm.invincible);
        ImGui::Separator();
        if (ImGui::Button("外部からダメージ (-10 HP)")) {
            _vm.hp.Set(_vm.hp.Get() - 10.0f);
        }
        ImGui::SameLine();
        if (ImGui::Button("満タン回復")) {
            _vm.hp.Set(100.0f);
            _vm.mana.Set(100.0f);
        }
        ImGui::End();

        // ---- 読み取り専用パネル: OneWayBinder で連動 ----
        ImGui::SetNextWindowSize(ImVec2(360, 160), ImGuiCond_FirstUseEver);
        ImGui::Begin("Mirror View (OneWay)");
        ImGui::Text("View が編集を加えなくても、VM の値変更で自動更新される");
        ImGui::Separator();
        mvvm::imgui::BindProgress("HP", _hp_mirror, 0.0f, 100.0f);
        mvvm::imgui::BindReadOnly("HP (text)", _hp_mirror);
        ImGui::End();

        _imgui.Render();
    }

    void OnShutdown() noexcept override {
        delete _hp_mirror_binder;
        _hp_mirror_binder = nullptr;
        _imgui.Shutdown();
    }

    void OnEvent(const Event& e) noexcept override {
        _imgui.OnEvent(e);
    }

private:
    ImGuiCtx                  _imgui;
    PlayerViewModel           _vm;
    Observable<f32>           _hp_mirror{};
    OneWayBinder<f32>*        _hp_mirror_binder = nullptr;
};

ACS_DEFINE_MAIN(HelloMVVM)
