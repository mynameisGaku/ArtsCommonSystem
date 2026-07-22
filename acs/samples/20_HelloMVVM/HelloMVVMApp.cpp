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

void FHelloMvvmApp::OnStart() noexcept {
    ACS_SAMPLE_INIT(m_Imgui.Init(GetWindow(), GetRenderer()));

    // Subscribe で値の変化をフックできる。ここではログに流して
    // bind が走っていることを目で確認できるようにしておく。
    m_Vm.hp.Subscribe([](const f32& v, void*){
        ACS_LOG_INFO("VM hp: %.1f", v);
    }, nullptr);

    // MakeBind は TUniquePtr を返すので、所有権だけ持っていれば
    // 寿命管理 (Unsubscribe) は自動で走る。
    m_HpMirrorBinder = MakeBind(m_Vm.hp, m_HpMirror);

    // 型が異なる Observable 同士は MakeBindConvert で繋ぐ。
    // 組込みの TDefaultConverter<f32, FString> が選択されるので
    // 変換ラムダを書く必要はない。
    m_HpTextBinder = MakeBindConvert<f32, FString>(m_Vm.hp, m_HpText);

    // Derived は依存元 Observable が変わると lazy に再計算される派生 Observable。
    m_Ratio = new TDerived<f32, f32>(
        [](const f32& h, const f32& m) {
            return m > 0 ? h / m : 0.0f;
        },
        m_Vm.hp, m_Vm.max_hp);

    m_Vm.inventory.PushBack(7);
    m_Vm.inventory.PushBack(15);
    m_Vm.inventory.PushBack(99);

    // ObservableArray は要素単位の Insert/Remove/Change/Clear イベントを
    // 配るので、UI 全更新ではなく差分更新が書ける。
    m_Vm.inventory.Subscribe([](EArrayChange k, usize i, const i32* v, void*){
        switch (k) {
            case EArrayChange::Inserted: ACS_LOG_INFO("Inv[+%zu] = %d", i, *v); break;
            case EArrayChange::Removed:  ACS_LOG_INFO("Inv[-%zu]", i); break;
            case EArrayChange::Changed:  ACS_LOG_INFO("Inv[%zu] = %d", i, *v); break;
            case EArrayChange::Cleared:  ACS_LOG_INFO("Inv cleared"); break;
        }
    }, nullptr);

    ACS_LOG_INFO("HelloMVVM initialized");
}

void FHelloMvvmApp::OnUpdate(f32 /*dt*/) noexcept {
    if (FInput::IsKeyPressed(EKey::Escape)) Quit();
}

void FHelloMvvmApp::OnRender() noexcept {
    m_Imgui.NewFrame();

    ImGui::SetNextWindowSize(ImVec2(440, 720), ImGuiCond_FirstUseEver);
    ImGui::Begin("ACS MVVM Demo");

    // ============================================================
    // ① 5 分入門 — TObservable<T> + imgui::Bind 最小例
    // ============================================================
    if (ImGui::CollapsingHeader("① 5 分入門 (Set/Get + Bind)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("TObservable<T> をスライダーで編集する最小例:");
        mvvm::imgui::Bind("HP",     m_Vm.hp,     0.0f, 100.0f);
        mvvm::imgui::Bind("Mana",   m_Vm.mana,   0.0f, 100.0f);
        mvvm::imgui::Bind("Lv",     m_Vm.level,  1, 99);
        mvvm::imgui::Bind("無敵",   m_Vm.invincible);
        ImGui::Separator();

        mvvm::imgui::BindText("名前", m_Vm.name, m_NameBuf, sizeof(m_NameBuf));
        mvvm::imgui::BindColor3("シンボル色", m_Vm.color);

        const char* classes[] = { "戦士", "魔法使い", "盗賊" };
        mvvm::imgui::BindCombo("クラス", m_Vm.class_idx, classes, 3);
    }

    // ============================================================
    // ② Binder — Observable 同士の自動同期
    // ============================================================
    if (ImGui::CollapsingHeader("② Binder (自動同期)")) {
        ImGui::Text("OneWayBinder: HP が変わると自動で hp_mirror も更新");
        mvvm::imgui::BindProgress("Mirror HP", m_HpMirror, 0.0f, 100.0f);

        ImGui::Spacing();
        ImGui::Text("組込み暗黙変換: f32 → FString (TDefaultConverter 自動選択)");
        ImGui::Text("HP 文字列: \"%s\"", m_HpText.Get().Data());

        ImGui::Spacing();
        ImGui::Text("Bind(src, dst) ファクトリは型違いでも自動で converter 選択:");
        ImGui::BulletText("TObservable<f32> → TObservable<FString>: \"%s\"", m_HpText.Get().Data());
        ImGui::BulletText("TObservable<i32> → TObservable<f32>: 同型なら OneWayBinder");
        ImGui::BulletText("TObservable<FString> → TObservable<i32>: パース失敗時 0");
    }

    // ============================================================
    // ③ Derived — 依存元から自動計算される派生 Observable
    // ============================================================
    if (ImGui::CollapsingHeader("③ Derived (派生 Observable)")) {
        ImGui::Text("Max HP を変えると ratio が自動再計算 (lazy):");
        mvvm::imgui::Bind("Max HP", m_Vm.max_hp, 1.0f, 200.0f);
        ImGui::Spacing();
        // Derived 本体は Observable ではないので、bind には AsObservable() を経由する。
        mvvm::imgui::BindFormat<f32>("HP / MaxHP = %.2f", m_Ratio->AsObservable());
        mvvm::imgui::BindProgress("Ratio", m_Ratio->AsObservable(), 0.0f, 1.0f);
    }

    // ============================================================
    // ④ TObservableArray + FCommand — コレクションとアクション
    // ============================================================
    if (ImGui::CollapsingHeader("④ TArray + Command")) {
        ImGui::Text("Inventory (%zu 個):", m_Vm.inventory.Size());
        for (usize i = 0; i < m_Vm.inventory.Size(); ++i) {
            ImGui::BulletText("[%zu] = %d", i, m_Vm.inventory.At(i));
        }
        if (ImGui::Button("ランダム追加")) {
            m_Vm.inventory.PushBack((rand() % 100) + 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("末尾削除") && m_Vm.inventory.Size() > 0) {
            m_Vm.inventory.PopBack();
        }
        ImGui::SameLine();
        if (ImGui::Button("全削除")) {
            m_Vm.inventory.Clear();
        }

        ImGui::Spacing();
        ImGui::Text("Command (ボタン化、無敵中は grayout):");
        // FCommand 自体は can_execute=null で常時実行可能にしておき、
        // 無敵フラグによる UI 抑止だけを View 側 (BeginDisabled) で行う。
        if (m_Vm.invincible.Get()) ImGui::BeginDisabled();
        mvvm::imgui::BindCommand("攻撃を受ける (-10 HP)", m_Vm.attack);
        if (m_Vm.invincible.Get()) ImGui::EndDisabled();
    }

    ImGui::End();

    m_Imgui.Render();
}

void FHelloMvvmApp::OnShutdown() noexcept {
    // Reset/delete を OnShutdown 側で明示的に走らせることで、
    // Subscribe の解除順を ImGui::Shutdown より前に固定する。
    delete m_Ratio; m_Ratio = nullptr;
    m_HpTextBinder.Reset();
    m_HpMirrorBinder.Reset();
    m_Imgui.Shutdown();
}

void FHelloMvvmApp::OnEvent(const FEvent& e) noexcept {
    m_Imgui.OnEvent(e);
}

} // namespace hellomvvm
