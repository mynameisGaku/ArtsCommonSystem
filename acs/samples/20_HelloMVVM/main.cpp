// SPDX-License-Identifier: Apache-2.0
// ACS の HelloMVVM サンプル
//
// 動作: 1 ウィンドウ内を 4 セクションに分けて MVVM の機能を順番に紹介。
//   ① 5 分入門   — Observable<T>::Set/Get + imgui::Bind だけで動く最小例
//   ② バインダ    — TwoWayBinder / OneWayBinder / OneWayConvertBinder
//   ③ Derived     — HP/MaxHP から ratio を自動算出
//   ④ ObservableArray + Command — インベントリ追加削除 + 攻撃ボタン
//
// 注: ACS_RENDER_DILIGENT との関係はない。Imgui モジュールが有効ならビルド可。

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "app/Sample.h"
#include "platform/Input.h"
#include "imgui/ImGuiContext.h"

#include "mvvm/ViewModel.h"
#include "mvvm/Derived.h"
#include "mvvm/ObservableArray.h"
#include "mvvm/Command.h"
#include "mvvm/ImguiBindings.h"
#include "container/String.h"
#include "foundation/Log.h"

#include <imgui.h>
#include <cstdio>

using namespace acs;

// ============================================================
// ① ViewModel 定義
// ============================================================
class PlayerVM : public ViewModel {
public:
    Observable<f32>     hp        { 100.0f };
    Observable<f32>     max_hp    { 100.0f };
    Observable<f32>     mana      { 50.0f };
    Observable<i32>     level     { 1 };
    Observable<bool>    invincible{ false };
    Observable<String>  name      { String{"勇者"} };
    Observable<Vec3>    color     { Vec3{1.0f, 0.85f, 0.4f} };
    Observable<i32>     class_idx { 0 };  // Combo 用 (0=戦士, 1=魔法使い, 2=盗賊)

    // インベントリ
    ObservableArray<i32> inventory;

    // Attack コマンド: invincible が false のときだけ実行可能
    // (Observable<bool>* で can_execute を渡す。null なら常時実行可能)
    Command attack {
        [](void* user) {
            auto* self = static_cast<PlayerVM*>(user);
            self->hp.Set(self->hp.Get() - 10.0f);
        },
        this,
        nullptr   // 常時実行可能 (invincible は別途見て切替)
    };
};

class HelloMVVM : public Application {
public:
    void OnStart() noexcept override {
        ACS_SAMPLE_INIT(_imgui.Init(GetWindow(), GetRenderer()));

        // VM 値変更をログに流す Subscribe (デバッグ目的)
        _vm.hp.Subscribe([](const f32& v, void*){
            ACS_LOG_INFO("VM hp: %.1f", v);
        }, nullptr);

        // OneWayBinder: VM の hp が hp_mirror に流れる
        // MakeBind は UniquePtr を返すので、自分で new/delete する必要なし。
        _hp_mirror_binder = MakeBind(_vm.hp, _hp_mirror);

        // 暗黙変換: f32 → String を組込みの DefaultConverter で 1 行 (lambda 不要)
        _hp_text_binder = MakeBindConvert<f32, String>(_vm.hp, _hp_text);

        // Derived: hp / max_hp = ratio (lazy recompute)
        _ratio = new Derived<f32, f32>(
            [](const f32& h, const f32& m) {
                return m > 0 ? h / m : 0.0f;
            },
            _vm.hp, _vm.max_hp);

        // 初期インベントリ
        _vm.inventory.PushBack(7);
        _vm.inventory.PushBack(15);
        _vm.inventory.PushBack(99);

        // インベントリ変更ログ
        _vm.inventory.Subscribe([](EArrayChange k, usize i, const i32* v, void*){
            switch (k) {
                case EArrayChange::Inserted: ACS_LOG_INFO("Inv[+%zu] = %d", i, *v); break;
                case EArrayChange::Removed:  ACS_LOG_INFO("Inv[-%zu]", i); break;
                case EArrayChange::Changed:  ACS_LOG_INFO("Inv[%zu] = %d", i, *v); break;
                case EArrayChange::Cleared:  ACS_LOG_INFO("Inv cleared"); break;
            }
        }, nullptr);

        ACS_LOG_INFO("HelloMVVM initialized");
    }

    void OnUpdate(f32 /*dt*/) noexcept override {
        if (Input::IsKeyPressed(EKey::Escape)) Quit();
    }

    void OnRender() noexcept override {
        _imgui.NewFrame();

        ImGui::SetNextWindowSize(ImVec2(440, 720), ImGuiCond_FirstUseEver);
        ImGui::Begin("ACS MVVM Demo");

        // ① 5 分入門 ============================================
        if (ImGui::CollapsingHeader("① 5 分入門 (Set/Get + Bind)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Observable<T> をスライダーで編集する最小例:");
            mvvm::imgui::Bind("HP",     _vm.hp,     0.0f, 100.0f);
            mvvm::imgui::Bind("Mana",   _vm.mana,   0.0f, 100.0f);
            mvvm::imgui::Bind("Lv",     _vm.level,  1, 99);
            mvvm::imgui::Bind("無敵",   _vm.invincible);
            ImGui::Separator();

            // 文字列入力
            mvvm::imgui::BindText("名前", _vm.name, _name_buf, sizeof(_name_buf));

            // カラーピッカー
            mvvm::imgui::BindColor3("シンボル色", _vm.color);

            // Combo (職業)
            const char* classes[] = { "戦士", "魔法使い", "盗賊" };
            mvvm::imgui::BindCombo("クラス", _vm.class_idx, classes, 3);
        }

        // ② バインダ ============================================
        if (ImGui::CollapsingHeader("② Binder (自動同期)")) {
            ImGui::Text("OneWayBinder: HP が変わると自動で hp_mirror も更新");
            mvvm::imgui::BindProgress("Mirror HP", _hp_mirror, 0.0f, 100.0f);

            ImGui::Spacing();
            ImGui::Text("組込み暗黙変換: f32 → String (DefaultConverter 自動選択)");
            ImGui::Text("HP 文字列: \"%s\"", _hp_text.Get().Data());

            ImGui::Spacing();
            ImGui::Text("Bind(src, dst) ファクトリは型違いでも自動で converter 選択:");
            ImGui::BulletText("Observable<f32> → Observable<String>: \"%s\"", _hp_text.Get().Data());
            ImGui::BulletText("Observable<i32> → Observable<f32>: 同型なら OneWayBinder");
            ImGui::BulletText("Observable<String> → Observable<i32>: パース失敗時 0");
        }

        // ③ Derived =============================================
        if (ImGui::CollapsingHeader("③ Derived (派生 Observable)")) {
            ImGui::Text("Max HP を変えると ratio が自動再計算 (lazy):");
            mvvm::imgui::Bind("Max HP", _vm.max_hp, 1.0f, 200.0f);
            ImGui::Spacing();
            // Derived は AsObservable() を経由するか Get() で値を取得
            mvvm::imgui::BindFormat<f32>("HP / MaxHP = %.2f", _ratio->AsObservable());
            mvvm::imgui::BindProgress("Ratio", _ratio->AsObservable(), 0.0f, 1.0f);
        }

        // ④ ObservableArray + Command ===========================
        if (ImGui::CollapsingHeader("④ Array + Command")) {
            ImGui::Text("Inventory (%zu 個):", _vm.inventory.Size());
            for (usize i = 0; i < _vm.inventory.Size(); ++i) {
                ImGui::BulletText("[%zu] = %d", i, _vm.inventory.At(i));
            }
            if (ImGui::Button("ランダム追加")) {
                _vm.inventory.PushBack((rand() % 100) + 1);
            }
            ImGui::SameLine();
            if (ImGui::Button("末尾削除") && _vm.inventory.Size() > 0) {
                _vm.inventory.PopBack();
            }
            ImGui::SameLine();
            if (ImGui::Button("全削除")) {
                _vm.inventory.Clear();
            }

            ImGui::Spacing();
            ImGui::Text("Command (ボタン化、無敵中は grayout):");
            // 無敵フラグの値で can_execute をエミュ (簡易: 直接 Execute を gate)
            if (_vm.invincible.Get()) ImGui::BeginDisabled();
            mvvm::imgui::BindCommand("攻撃を受ける (-10 HP)", _vm.attack);
            if (_vm.invincible.Get()) ImGui::EndDisabled();
        }

        ImGui::End();

        _imgui.Render();
    }

    void OnShutdown() noexcept override {
        delete _ratio; _ratio = nullptr;     // Derived はまだ生 new (将来 MakeDerived 化)
        _hp_text_binder.Reset();              // UniquePtr — 自動で Unsubscribe
        _hp_mirror_binder.Reset();
        _imgui.Shutdown();
    }

    void OnEvent(const Event& e) noexcept override {
        _imgui.OnEvent(e);
    }

private:
    ImGuiCtx                                  _imgui;
    PlayerVM                                  _vm;
    Observable<f32>                           _hp_mirror{};
    Observable<String>                        _hp_text{ String{} };
    UniquePtr<OneWayBinder<f32>>              _hp_mirror_binder;
    UniquePtr<OneWayConvertBinder<f32, String>> _hp_text_binder;
    Derived<f32, f32>*                        _ratio            = nullptr;
    char                                      _name_buf[64]     = "勇者";
};

ACS_DEFINE_MAIN(HelloMVVM)
