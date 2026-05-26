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

    // Subscribe で値の変化をフックできる。ここではログに流して
    // bind が走っていることを目で確認できるようにしておく。
    _vm.hp.Subscribe([](const f32& v, void*){
        ACS_LOG_INFO("VM hp: %.1f", v);
    }, nullptr);

    // MakeBind は TUniquePtr を返すので、所有権だけ持っていれば
    // 寿命管理 (Unsubscribe) は自動で走る。
    _hp_mirror_binder = MakeBind(_vm.hp, _hp_mirror);

    // 型が異なる FObservable 同士は MakeBindConvert で繋ぐ。
    // 組込みの TDefaultConverter<f32, FString> が選択されるので
    // 変換ラムダを書く必要はない。
    _hp_text_binder = MakeBindConvert<f32, FString>(_vm.hp, _hp_text);

    // FDerived は依存元 FObservable が変わると lazy に再計算される派生 FObservable。
    _ratio = new FDerived<f32, f32>(
        [](const f32& h, const f32& m) {
            return m > 0 ? h / m : 0.0f;
        },
        _vm.hp, _vm.max_hp);

    _vm.inventory.PushBack(7);
    _vm.inventory.PushBack(15);
    _vm.inventory.PushBack(99);

    // FObservableArray は要素単位の Insert/Remove/Change/Clear イベントを
    // 配るので、UI 全更新ではなく差分更新が書ける。
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
    if (FInput::IsKeyPressed(EKey::Escape)) Quit();
}

void HelloMVVMApp::OnRender() noexcept {
    _imgui.NewFrame();

    ImGui::SetNextWindowSize(ImVec2(440, 720), ImGuiCond_FirstUseEver);
    ImGui::Begin("ACS MVVM Demo");

    // ============================================================
    // ① 5 分入門 — FObservable<T> + imgui::Bind 最小例
    // ============================================================
    if (ImGui::CollapsingHeader("① 5 分入門 (Set/Get + Bind)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("FObservable<T> をスライダーで編集する最小例:");
        mvvm::imgui::Bind("HP",     _vm.hp,     0.0f, 100.0f);
        mvvm::imgui::Bind("Mana",   _vm.mana,   0.0f, 100.0f);
        mvvm::imgui::Bind("Lv",     _vm.level,  1, 99);
        mvvm::imgui::Bind("無敵",   _vm.invincible);
        ImGui::Separator();

        mvvm::imgui::BindText("名前", _vm.name, _name_buf, sizeof(_name_buf));
        mvvm::imgui::BindColor3("シンボル色", _vm.color);

        const char* classes[] = { "戦士", "魔法使い", "盗賊" };
        mvvm::imgui::BindCombo("クラス", _vm.class_idx, classes, 3);
    }

    // ============================================================
    // ② Binder — FObservable 同士の自動同期
    // ============================================================
    if (ImGui::CollapsingHeader("② Binder (自動同期)")) {
        ImGui::Text("FOneWayBinder: HP が変わると自動で hp_mirror も更新");
        mvvm::imgui::BindProgress("Mirror HP", _hp_mirror, 0.0f, 100.0f);

        ImGui::Spacing();
        ImGui::Text("組込み暗黙変換: f32 → FString (TDefaultConverter 自動選択)");
        ImGui::Text("HP 文字列: \"%s\"", _hp_text.Get().Data());

        ImGui::Spacing();
        ImGui::Text("Bind(src, dst) ファクトリは型違いでも自動で converter 選択:");
        ImGui::BulletText("FObservable<f32> → FObservable<FString>: \"%s\"", _hp_text.Get().Data());
        ImGui::BulletText("FObservable<i32> → FObservable<f32>: 同型なら FOneWayBinder");
        ImGui::BulletText("FObservable<FString> → FObservable<i32>: パース失敗時 0");
    }

    // ============================================================
    // ③ FDerived — 依存元から自動計算される派生 FObservable
    // ============================================================
    if (ImGui::CollapsingHeader("③ FDerived (派生 FObservable)")) {
        ImGui::Text("Max HP を変えると ratio が自動再計算 (lazy):");
        mvvm::imgui::Bind("Max HP", _vm.max_hp, 1.0f, 200.0f);
        ImGui::Spacing();
        // FDerived 本体は FObservable ではないので、bind には AsObservable() を経由する。
        mvvm::imgui::BindFormat<f32>("HP / MaxHP = %.2f", _ratio->AsObservable());
        mvvm::imgui::BindProgress("Ratio", _ratio->AsObservable(), 0.0f, 1.0f);
    }

    // ============================================================
    // ④ FObservableArray + FCommand — コレクションとアクション
    // ============================================================
    if (ImGui::CollapsingHeader("④ TArray + FCommand")) {
        ImGui::Text("Inventory (%zu 個):", _vm.inventory.Size());
        for (usize i = 0; i < _vm.inventory.Size(); ++i) {
            ImGui::BulletText("[%zu] = %d", i, _vm.inventory.At(i));
        }
        if (ImGui::FButton("ランダム追加")) {
            _vm.inventory.PushBack((rand() % 100) + 1);
        }
        ImGui::SameLine();
        if (ImGui::FButton("末尾削除") && _vm.inventory.Size() > 0) {
            _vm.inventory.PopBack();
        }
        ImGui::SameLine();
        if (ImGui::FButton("全削除")) {
            _vm.inventory.Clear();
        }

        ImGui::Spacing();
        ImGui::Text("FCommand (ボタン化、無敵中は grayout):");
        // FCommand 自体は can_execute=null で常時実行可能にしておき、
        // 無敵フラグによる UI 抑止だけを View 側 (BeginDisabled) で行う。
        if (_vm.invincible.Get()) ImGui::BeginDisabled();
        mvvm::imgui::BindCommand("攻撃を受ける (-10 HP)", _vm.attack);
        if (_vm.invincible.Get()) ImGui::EndDisabled();
    }

    ImGui::End();

    _imgui.Render();
}

void HelloMVVMApp::OnShutdown() noexcept {
    // Reset/delete を OnShutdown 側で明示的に走らせることで、
    // Subscribe の解除順を ImGui::Shutdown より前に固定する。
    delete _ratio; _ratio = nullptr;
    _hp_text_binder.Reset();
    _hp_mirror_binder.Reset();
    _imgui.Shutdown();
}

void HelloMVVMApp::OnEvent(const FEvent& e) noexcept {
    _imgui.OnEvent(e);
}

} // namespace hellomvvm
