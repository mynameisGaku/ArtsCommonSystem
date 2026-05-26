// SPDX-License-Identifier: Apache-2.0
// HelloSpriteAtlasEditor — SpriteAtlasScene 実装。
#include "SpriteAtlasScene.h"

#include "platform/Input.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

namespace hellosa {

// ----------------------------------------------------------------------------
// OnEnter — workspace 初期化 + dummy FSpritePack 構築 + editor panel 登録
// ----------------------------------------------------------------------------
void SpriteAtlasScene::OnEnter() noexcept {
    // editor らしいニュートラルグレー (背景は ImGui に隠れるが viewport の
    // 外側のクリア色を編集向けに揃える)。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    _workspace.Init();

    // 実 texture path は文字列リテラルとして渡すだけ (loader 未配線)。
    // panel の viewport は texture を描かず grid placeholder で代用する。
    FSpritePackInfo info{};
    info.atlas_texture_path = "assets/dummy_atlas.png";
    info.atlas_width        = 256u;
    info.atlas_height       = 256u;
    _pack.Init(info);

    // 3 frame (Idle / Walk / Jump 各 32x32) を登録。
    // name は文字列リテラル (FSpritePack 規約: caller 所有 / リテラル前提)。
    FSpriteFrame f{};
    f.w = 32u;
    f.h = 32u;
    f.pivot_x = 0.5f;
    f.pivot_y = 0.5f;

    f.name = "Idle";
    f.x = 0u;
    f.y = 0u;
    _pack.AddFrame(f);

    f.name = "Walk";
    f.x = 32u;
    f.y = 0u;
    _pack.AddFrame(f);

    f.name = "Jump";
    f.x = 64u;
    f.y = 0u;
    _pack.AddFrame(f);

    // FEditorWorkspace::RegisterPanel は内部で panel->OnInit(*this) を呼ぶため、
    // panel.OnInit を別途呼ぶ必要は無い。SetSpritePack は Init / OnInit の
    // どちらより後でも構わない (panel が pack の存在を毎フレーム確認するため)。
    _editor_panel.Init();
    _workspace.RegisterPanel(&_editor_panel);
    _editor_panel.SetSpritePack(&_pack);

    ACS_LOG_INFO("[SpriteAtlasEditor] entered (256x256 dummy atlas, 3 frames: Idle/Walk/Jump)");
}

// ----------------------------------------------------------------------------
// OnExit — 逆順 shutdown
// ----------------------------------------------------------------------------
void SpriteAtlasScene::OnExit() noexcept {
    // FEditorWorkspace::Shutdown は登録済み全 panel に OnShutdown を 1 度ずつ
    // 呼んでから list を Clear するため、個別 UnregisterPanel は不要。
    _workspace.Shutdown();
    // panel 本体の internal state を解放 (name pool / selection クリア)。
    _editor_panel.Shutdown();

    // FSpritePack は Dtor で frame 配列を解放する (TArray の所有を持つ)。
    // 明示 ClearAll しても良いが、scene 破棄と同時なので無くても問題なし。
    _pack.ClearAll();

    ACS_LOG_INFO("[SpriteAtlasEditor] exited");
}

// ----------------------------------------------------------------------------
// OnUpdate — Escape による終了のみ
// ----------------------------------------------------------------------------
// ImGui 関連 (Workspace::TickAllPanels が呼ぶ DrawDockSpace / MenuBar / panel
// DrawUI) はすべて OnRender 側へ。ImGui::Begin 等は NewFrame() と Render() の
// 間でしか呼べないため、ここで Workspace::TickAllPanels は呼ばない。
void SpriteAtlasScene::OnUpdate(f32 dt) noexcept {
    if (FInput::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }
    (void)dt;
}

// ----------------------------------------------------------------------------
// OnRender — File menu (Save/Load stub) → Workspace 全描画
// ----------------------------------------------------------------------------
void SpriteAtlasScene::OnRender(FRenderContext& /*rc*/) noexcept {
    // ImGui は同一フレーム内で BeginMainMenuBar を複数回呼んでも 1 個の bar に
    // マージするので、本 sample 専用の File メニューを Workspace の FWindow/
    // Layout メニューと並べて表示できる。
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save .acsatlas")) {
                // serializer 本体は未配線。callback hook だけ走らせる stub。
                // 将来は AtlasSerializer::Save(kAtlasFilePath, _pack) を呼ぶ。
                ACS_LOG_INFO("[SpriteAtlasEditor] Save .acsatlas -> '%s' (stub, no-op)", kAtlasFilePath);
            }
            if (ImGui::MenuItem("Load .acsatlas")) {
                ACS_LOG_INFO("[SpriteAtlasEditor] Load .acsatlas <- '%s' (stub, no-op)", kAtlasFilePath);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) {
                GetGame().Quit();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // OnFrameBegin → DockSpace → MenuBar → 各 panel DrawUI を順に発火。
    _workspace.TickAllPanels(GetGame().DeltaTime());
}

} // namespace hellosa
