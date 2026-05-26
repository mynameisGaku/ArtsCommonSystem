// SPDX-License-Identifier: Apache-2.0
// HelloParticleEditor — ParticleEditorScene 実装。
#include "ParticleEditorScene.h"

#include "gameframework/tools/fxedit/FxeditSerializer.h"

#include "platform/Input.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

namespace helloparticleed {

void ParticleEditorScene::OnEnter() noexcept {
    // 連続放出 + Burst のピークを吸収するため pool 容量を多めに取る。
    _particle_system.Init(4096);

    // 既定値が「無発射 / 黒粒子」なので、起動直後に何も見えない事故を避ける
    // ため、最低限の見た目が出る連続放出パラメータで emitter 1 個を作る。
    // Panel が同じ slot のパラメータを編集する想定。
    FParticleEmitterDef def{};
    def.lifetime_sec       = 1.5f;
    def.emit_rate_per_sec  = 30.0f;
    def.burst_count        = 16.0f;
    def.speed_min          = 40.0f;
    def.speed_max          = 80.0f;
    def.scale_start        = 8.0f;
    def.scale_end          = 0.0f;
    def.color_start        = FVec3{1.0f, 0.6f, 0.1f};   // 橙
    def.color_end          = FVec3{0.5f, 0.0f, 0.0f};   // 暗赤
    def.gravity            = FVec2{0.0f, 60.0f};

    // Preview が原点中心に描画する想定なので world 座標 (0,0) に配置。
    _default_emitter = _particle_system.CreateEmitter(def, FVec2{0.0f, 0.0f});
    _particle_system.SetEmitterActive(_default_emitter, true);

    // Panel 側は SelectedIndex() のデフォルト 0 で十分動く前提なので、
    // 明示的な SyncFromSystem 系 API は呼ばない (最小契約)。
    ACS_LOG_INFO("[ParticleEditor] entered (default emitter created, capacity=4096)");
}

void ParticleEditorScene::OnExit() noexcept {
    _particle_system.ClearAll();
    ACS_LOG_INFO("[ParticleEditor] exited");
}

void ParticleEditorScene::OnUpdate(f32 dt) noexcept {
    if (FInput::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }

    // FApplication 側の大 dt スパイクで particle が飛び散らないよう clamp。
    if (dt > 0.1f) dt = 0.1f;

    _particle_system.Tick(dt);

    // Preview 内部の auto-rotate / カメラ追従などを進める。
    _editor_preview.Tick(dt, _particle_system);
}

void ParticleEditorScene::OnRender(FRenderContext& /*rc*/) noexcept {
    // ImGui の draw コマンドは FGame::OnRender の NewFrame() と Render() の間で
    // 発行される。FScene::OnRender はその内側なのでそのまま ImGui::* を呼べる。
    _draw_file_menu();

    // emitter パラメータ編集 panel。
    _editor_panel.SetTargetSystem(&_particle_system);
    _editor_panel.DrawUI();

    // Preview 描画 + Burst/Restart ボタン。
    _editor_preview.DrawUI(
        _particle_system,
        _editor_panel.GetEmitterDef(_editor_panel.SelectedIndex()));
}

// ----------------------------------------------------------------------------
// File メニュー: Save / Load / Quit のディスパッチのみ。実処理は _save_preset /
// _load_preset に逃がして、OnRender 側の見通しを保つ。
// ----------------------------------------------------------------------------
void ParticleEditorScene::_draw_file_menu() noexcept {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save .fxedit")) {
            _save_preset();
        }
        if (ImGui::MenuItem("Load .fxedit")) {
            _load_preset();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Esc")) {
            GetGame().Quit();
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void ParticleEditorScene::_save_preset() noexcept {
    const u32 idx = _editor_panel.SelectedIndex();
    const FParticleEmitterDef& def = _editor_panel.GetEmitterDef(idx);
    if (auto r = fxedit::FFxeditSerializer::Save(kPresetPath, def); r.IsErr()) {
        ACS_LOG_ERROR("[ParticleEditor] Save '%s' failed", kPresetPath);
    } else {
        ACS_LOG_INFO("[ParticleEditor] saved -> %s", kPresetPath);
    }
}

void ParticleEditorScene::_load_preset() noexcept {
    auto r = fxedit::FFxeditSerializer::Load(kPresetPath);
    if (r.IsErr()) {
        ACS_LOG_ERROR("[ParticleEditor] Load '%s' failed", kPresetPath);
        return;
    }
    // GetEmitterDef(idx) は mutable 参照を返す契約 (Save 側と対称)。
    const u32 idx = _editor_panel.SelectedIndex();
    _editor_panel.GetEmitterDef(idx) = r.Value();
    ACS_LOG_INFO("[ParticleEditor] loaded <- %s", kPresetPath);
}

} // namespace helloparticleed
