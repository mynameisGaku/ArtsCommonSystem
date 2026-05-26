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
    if (Input::IsKeyPressed(EKey::Escape)) {
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
    // Phase 24: EditorPanel 基底に乗ったため SetTargetSystem 後に no-arg DrawUI。
    _editor_panel.SetTargetSystem(&_particle_system);
    _editor_panel.DrawUI();

    // ----- Preview: 描画 + Burst/Restart ボタン -----
    _editor_preview.DrawUI(
        _particle_system,
        _editor_panel.GetEmitterDef(_editor_panel.SelectedIndex()));
}

} // namespace helloparticleed
