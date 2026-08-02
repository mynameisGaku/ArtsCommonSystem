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

void AParticleEditorScene::OnEnter() noexcept {
    // 連続放出 + Burst のピークを吸収するため pool 容量を多めに取る。
    m_ParticleSystem.Init(4096);

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
    m_DefaultEmitter = m_ParticleSystem.CreateEmitter(def, FVec2{0.0f, 0.0f});
    m_ParticleSystem.SetEmitterActive(m_DefaultEmitter, true);

    // Panel 側は SelectedIndex() のデフォルト 0 で十分動く前提なので、
    // 明示的な SyncFromSystem 系 API は呼ばない (最小契約)。
    ACS_LOG_INFO("[ParticleEditor] entered (default emitter created, capacity=4096)");
}

void AParticleEditorScene::OnExit() noexcept {
    m_ParticleSystem.ClearAll();
    ACS_LOG_INFO("[ParticleEditor] exited");
}

void AParticleEditorScene::OnUpdate(f32 dt) noexcept {
    if (CInput::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }

    // CApplication 側の大 dt スパイクで particle が飛び散らないよう clamp。
    if (dt > 0.1f) dt = 0.1f;

    m_ParticleSystem.Tick(dt);

    // Preview 内部の auto-rotate / カメラ追従などを進める。
    m_EditorPreview.Tick(dt, m_ParticleSystem);
}

void AParticleEditorScene::OnRender(FRenderContext& /*rc*/) noexcept {
    // ImGui の draw コマンドは CGame::OnRender の NewFrame() と Render() の間で
    // 発行される。Scene::OnRender はその内側なのでそのまま ImGui::* を呼べる。
    m_DrawFileMenu();

    // emitter パラメータ編集 panel。
    m_EditorPanel.SetTargetSystem(&m_ParticleSystem);
    m_EditorPanel.DrawUI();

    // Preview 描画 + Burst/Restart ボタン。
    m_EditorPreview.DrawUI(
        m_ParticleSystem,
        m_EditorPanel.GetEmitterDef(m_EditorPanel.SelectedIndex()));
}

// ----------------------------------------------------------------------------
// File メニュー: Save / Load / Quit のディスパッチのみ。実処理は m_SavePreset /
// m_LoadPreset に逃がして、OnRender 側の見通しを保つ。
// ----------------------------------------------------------------------------
void AParticleEditorScene::m_DrawFileMenu() noexcept {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save .fxedit")) {
            m_SavePreset();
        }
        if (ImGui::MenuItem("Load .fxedit")) {
            m_LoadPreset();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Esc")) {
            GetGame().Quit();
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void AParticleEditorScene::m_SavePreset() noexcept {
    // CFxeditSerializer は emitter 群 (def 配列 + 名前 + 個数) を保存する契約。
    // panel の全 emitter def を集めて配列で渡す。
    constexpr u32 kMax = fxedit::AParticleEditorPanel::kMaxEmitters;
    const u32 count = m_EditorPanel.EmitterCount();
    if (count == 0) {
        ACS_LOG_WARN("[ParticleEditor] 保存対象の emitter がありません");
        return;
    }
    const u32 n = count < kMax ? count : kMax;
    FParticleEmitterDef defs[kMax];
    const char*        names[kMax];
    for (u32 i = 0; i < n; ++i) {
        const FParticleEmitterDef* d = m_EditorPanel.GetEmitterDef(static_cast<i32>(i));
        defs[i]  = (d != nullptr) ? *d : FParticleEmitterDef{};
        names[i] = nullptr;   // panel は emitter 名を保持しないため "" 扱い
    }
    if (auto r = fxedit::CFxeditSerializer::Save(kPresetPathW, defs, names, n); r.IsErr()) {
        ACS_LOG_ERROR("[ParticleEditor] Save '%s' failed", kPresetPath);
    } else {
        ACS_LOG_INFO("[ParticleEditor] saved %u emitter(s) -> %s", n, kPresetPath);
    }
}

void AParticleEditorScene::m_LoadPreset() noexcept {
    constexpr u32 kMax = fxedit::AParticleEditorPanel::kMaxEmitters;
    FParticleEmitterDef defs[kMax];
    char               name_buf[2048];
    auto r = fxedit::CFxeditSerializer::Load(kPresetPathW, defs, name_buf,
                                             static_cast<u32>(sizeof(name_buf)), kMax);
    if (r.IsErr()) {
        ACS_LOG_ERROR("[ParticleEditor] Load '%s' failed", kPresetPath);
        return;
    }
    const u32 loaded = r.Value();
    // panel の emitter 数を loaded まで増やし、各 def を上書きする。
    while (m_EditorPanel.EmitterCount() < loaded) m_EditorPanel.AddEmitter();
    for (u32 i = 0; i < loaded; ++i) {
        FParticleEmitterDef* d = m_EditorPanel.GetEmitterDefMutable(static_cast<i32>(i));
        if (d != nullptr) *d = defs[i];
    }
    ACS_LOG_INFO("[ParticleEditor] loaded %u emitter(s) <- %s", loaded, kPresetPath);
}

} // namespace helloparticleed
