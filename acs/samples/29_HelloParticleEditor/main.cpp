// SPDX-License-Identifier: Apache-2.0
// HelloParticleEditor — GameFramework Phase 19b: 対話的 Particle エディタ
//
// 動作:
//   ・GameFramework Pillar A の Scene 上に ParticleEffectSystem (Pillar I Phase 2)
//     を 1 個立て、`fxedit::ParticleEditorPanel` がそのパラメータを ImGui で編集、
//     `fxedit::ParticleEditorPreview` がプレビュー描画 + Burst/Restart ボタン
//     を提供する。
//   ・main menu bar "File > Save .fxedit / Load .fxedit" で `FxeditSerializer`
//     経由の永続化を行う (保存先は実行ディレクトリの "preset.fxedit")。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (samples/21_HelloImGui と同じ理由で、
// ImGuiContext が DX12 raw backend 経由のため)。
//
// 設計メモ:
//   ・ImGuiCtx は **Game クラスが所有**。`OnStart/OnShutdown/OnEvent` で
//     lifecycle を回し、`OnRender` を override して `NewFrame() → Scene OnRender
//     → Render()` の順で挟む。これで Scene 側はそのまま ImGui::* を呼べる。
//   ・Scene は `WantedServices()` を None のままにする (=サービスは要らない、
//     emitter 駆動は自前)。
//   ・ParticleEditorPanel / Preview / FxeditSerializer は同 Phase 19b で並列
//     生成される `gameframework/tools/fxedit/` 配下のヘッダを include。
//     API 想定:
//       - Panel:   `void DrawUI(ParticleEffectSystem&)`, `u32 SelectedIndex()`,
//                  `ParticleEmitterDef& GetEmitterDef(u32 idx)`
//       - Preview: `void Tick(f32 dt, ParticleEffectSystem&)`,
//                  `void DrawUI(ParticleEffectSystem&, const ParticleEmitterDef&)`
//       - Serializer: `static Result<void> Save(const char* path, const ParticleEmitterDef&)`,
//                     `static Result<ParticleEmitterDef> Load(const char* path)`

#include "gameframework/GameFramework.h"
#include "gameframework/ParticleEffectSystem.h"
#include "gameframework/tools/fxedit/ParticleEditorPanel.h"
#include "gameframework/tools/fxedit/ParticleEditorPreview.h"
#include "gameframework/tools/fxedit/FxeditSerializer.h"

#include "imgui/ImGuiContext.h"
#include "platform/Input.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

// ----------------------------------------------------------------------------
// ParticleEditorScene — emitter 1 個 + 編集 Panel + プレビュー
// ----------------------------------------------------------------------------
class ParticleEditorScene : public Scene {
public:
    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnRender(RenderContext& rc) noexcept override;

private:
    // File メニューで Save/Load を選んだときのファイルパス (固定)。
    static constexpr const char* kPresetPath = "preset.fxedit";

    ParticleEffectSystem        _particle_system;
    fxedit::ParticleEditorPanel _editor_panel;
    fxedit::ParticleEditorPreview _editor_preview;
    EmitterHandle               _default_emitter;     // OnEnter で作る emitter
};

void ParticleEditorScene::OnEnter() noexcept {
    // pool 容量を多めに (連続放出 + Burst のピークを吸収)。
    _particle_system.Init(4096);

    // emitter 1 個を default param で作成 (画面中央付近の world 座標 = 0,0)。
    // Panel が後で同じ slot のパラメータを編集する。Preview が原点を中心に
    // 描画する想定。
    ParticleEmitterDef def{};
    // ParticleEffectSystem の既定値は「無発射 / 黒粒子」なので、最低限の見た目
    // を default で出すために連続放出を on にしておく。
    def.lifetime_sec       = 1.5f;
    def.emit_rate_per_sec  = 30.0f;
    def.burst_count        = 16.0f;
    def.speed_min          = 40.0f;
    def.speed_max          = 80.0f;
    def.scale_start        = 8.0f;
    def.scale_end          = 0.0f;
    def.color_start        = Vec3{1.0f, 0.6f, 0.1f};   // 橙
    def.color_end          = Vec3{0.5f, 0.0f, 0.0f};   // 暗赤
    def.gravity            = Vec2{0.0f, 60.0f};

    _default_emitter = _particle_system.CreateEmitter(def, Vec2{0.0f, 0.0f});
    _particle_system.SetEmitterActive(_default_emitter, true);

    // Panel に default emitter を選択させる (slot 0、index 0 を選んだ状態にする
    // 想定。Panel 側の Reset/SyncFromSystem 系 API が無くても、SelectedIndex()
    // のデフォルト 0 で十分動く想定の最小契約)。
    ACS_LOG_INFO("[ParticleEditor] entered (default emitter created, capacity=4096)");
}

void ParticleEditorScene::OnExit() noexcept {
    _particle_system.ClearAll();
    ACS_LOG_INFO("[ParticleEditor] exited");
}

void ParticleEditorScene::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(Key::Escape)) {
        GetGame().Quit();
        return;
    }

    // dt の安全 clamp (Application 側の大 dt スパイクで particle が飛び散らない)。
    if (dt > 0.1f) dt = 0.1f;

    // particle 物理を進める。
    _particle_system.Tick(dt);

    // Preview の内部状態 (auto-rotate / カメラ追従など) を進める。
    _editor_preview.Tick(dt, _particle_system);
}

void ParticleEditorScene::OnRender(RenderContext& /*rc*/) noexcept {
    // ----- main menu bar: File > Save .fxedit / Load .fxedit -----
    // ImGui の draw コマンドは Game::OnRender の NewFrame() と Render() の間で
    // 発行される。Scene::OnRender はその内側なのでそのまま ImGui::* を呼べる。
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save .fxedit")) {
                const u32 idx = _editor_panel.SelectedIndex();
                const ParticleEmitterDef& def = _editor_panel.GetEmitterDef(idx);
                if (auto r = fxedit::FxeditSerializer::Save(kPresetPath, def); r.IsErr()) {
                    ACS_LOG_ERROR("[ParticleEditor] Save '%s' failed", kPresetPath);
                } else {
                    ACS_LOG_INFO("[ParticleEditor] saved -> %s", kPresetPath);
                }
            }
            if (ImGui::MenuItem("Load .fxedit")) {
                auto r = fxedit::FxeditSerializer::Load(kPresetPath);
                if (r.IsErr()) {
                    ACS_LOG_ERROR("[ParticleEditor] Load '%s' failed", kPresetPath);
                } else {
                    const u32 idx = _editor_panel.SelectedIndex();
                    // panel が保持している def を上書き。Panel の API として
                    // GetEmitterDef(idx) は **参照** を返す想定 (= mutable
                    // access)。Save 側と対称になる。
                    _editor_panel.GetEmitterDef(idx) = r.Value();
                    ACS_LOG_INFO("[ParticleEditor] loaded <- %s", kPresetPath);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) {
                GetGame().Quit();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // ----- Panel: emitter パラメータ編集 -----
    _editor_panel.DrawUI(_particle_system);

    // ----- Preview: 描画 + Burst/Restart ボタン -----
    _editor_preview.DrawUI(
        _particle_system,
        _editor_panel.GetEmitterDef(_editor_panel.SelectedIndex()));
}

// ----------------------------------------------------------------------------
// ParticleEditorGame — ImGui lifecycle を Game に持たせる薄いラッパ
// ----------------------------------------------------------------------------
class ParticleEditorGame : public Game {
public:
    void OnStart() noexcept override {
        // ImGui を Window + Renderer に紐付け。
        if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
            ACS_LOG_ERROR("[ParticleEditorGame] ImGuiCtx.Init failed -> Quit");
            Quit();
            return;
        }
        // 基底の OnStart は InitialScene() を push する。
        Game::OnStart();
    }

    void OnRender() noexcept override {
        // ImGui フレーム開始 -> Scene::OnRender で ImGui::* が呼ばれる ->
        // ImGui の描画コマンドをコマンドリストに発行、の順。Scene の Render
        // ロジックは Game::OnRender が SceneManager 経由で実行する。
        _imgui.NewFrame();
        Game::OnRender();
        _imgui.Render();
    }

    void OnShutdown() noexcept override {
        // Scene 側を先に止めてから ImGui を落とす (Scene が ImGui::* を握って
        // ないことを保証)。
        Game::OnShutdown();
        _imgui.Shutdown();
    }

    void OnEvent(const Event& e) noexcept override {
        _imgui.OnEvent(e);
        Game::OnEvent(e);
    }

protected:
    UniquePtr<Scene> InitialScene() noexcept override {
        return MakeUnique<ParticleEditorScene>();
    }

private:
    ImGuiCtx _imgui;
};

ACS_GAME_MAIN(ParticleEditorGame)
