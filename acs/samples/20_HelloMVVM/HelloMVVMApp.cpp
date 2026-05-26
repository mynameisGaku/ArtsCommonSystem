// SPDX-License-Identifier: Apache-2.0
// HelloMVVM — HelloMVVMApp 実装。
#include "HelloMVVMApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "foundation/Log.h"

#include <imgui.h>
#include <cstdio>
#include <cstdlib>

using namespace acs;

namespace hellomvvm {

void HelloMVVMApp::OnStart() noexcept {
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

void HelloMVVMApp::OnUpdate(f32 /*dt*/) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
}

void HelloMVVMApp::OnRender() noexcept {
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

void HelloMVVMApp::OnShutdown() noexcept {
    delete _ratio; _ratio = nullptr;     // Derived はまだ生 new (将来 MakeDerived 化)
    _hp_text_binder.Reset();              // UniquePtr — 自動で Unsubscribe
    _hp_mirror_binder.Reset();
    _imgui.Shutdown();
}

void HelloMVVMApp::OnEvent(const Event& e) noexcept {
    _imgui.OnEvent(e);
}

} // namespace hellomvvm
