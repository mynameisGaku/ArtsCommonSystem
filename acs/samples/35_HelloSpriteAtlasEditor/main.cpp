// SPDX-License-Identifier: Apache-2.0
// HelloSpriteAtlasEditor — GameFramework Phase 22: SpriteAtlasEditor デモ
//
// 動作:
//   ・editor_core (Phase 21a) の EditorWorkspace を 1 個立てて、Phase 22 で
//     並列実装中の `spriteatlas::SpriteAtlasEditorPanel` を register する。
//   ・シーン内に 256x256 の dummy atlas (= 実 texture は無し、panel 側 grid
//     placeholder で代用) を `SpritePack` に初期登録し、3 frame
//     (Idle / Walk / Jump 各 32x32) を AddFrame で先に入れておく。
//   ・MainMenuBar > File に Save / Load ".acsatlas" を stub callback で配線。
//     Phase 22 範囲外なので serializer 本体は未配線、ACS_LOG_INFO で発火を
//     ログするだけ。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (samples/21/29/30/31 と同じ理由で、
//                  ImGuiCtx が DX12 raw backend 経由のため)。
//
// 設計メモ:
//   ・ImGuiCtx は **Game クラスが所有**。OnStart / OnShutdown / OnEvent で
//     lifecycle を回し、OnRender を override して NewFrame() → Scene OnRender
//     → Render() の順で挟む (= sample 29/30/31 と同形)。
//   ・Scene は WantedServices() を None のままにする (= scene clock 等の自動
//     tick は不要)。
//   ・atlas texture は本サンプルでは実体を持たず、SpritePackInfo の
//     atlas_texture_path に文字列リテラルだけ入れる。panel 側 viewport は
//     atlas size (256x256) ぶんの空背景に grid 線を描いて placeholder とする。
//   ・SpritePack は非コピー / 非ムーブのため、メンバとして直接保持し
//     SpriteAtlasEditorPanel に SetSpritePack で raw ポインタを注入する。
//   ・frame name は文字列リテラル ("Idle" / "Walk" / "Jump") を直接 SpritePack
//     に渡す。SpritePack 規約 (name は caller 所有のリテラル前提) に従う。

#include "gameframework/GameFramework.h"
#include "gameframework/SpritePack.h"
#include "gameframework/tools/editor_core/EditorWorkspace.h"
#include "gameframework/tools/spriteatlas/SpriteAtlasEditorPanel.h"

#include "imgui/ImGuiContext.h"
#include "platform/Input.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

// ============================================================================
// SpriteAtlasScene — Workspace + SpriteAtlasEditorPanel + dummy SpritePack
// ============================================================================
class SpriteAtlasScene : public Scene {
public:
    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnRender(RenderContext& rc) noexcept override;

private:
    // File menu stub の保存先 (現状 callback だけ走らせるため未使用)。
    static constexpr const char* kAtlasFilePath = "preset.acsatlas";

    // ---- editor_core ----
    editor_core::EditorWorkspace                _workspace;

    // ---- spriteatlas (Phase 22) ----
    spriteatlas::SpriteAtlasEditorPanel         _editor_panel;

    // ---- 編集対象 SpritePack (256x256 dummy atlas + 3 frame) ----
    // SpritePack は非コピー / 非ムーブなのでメンバ直保持。`_editor_panel` に
    // SetSpritePack(&_pack) で raw 注入する。
    SpritePack                                  _pack;
};

// ----------------------------------------------------------------------------
// OnEnter — workspace 初期化 + dummy SpritePack 構築 + editor panel 登録
// ----------------------------------------------------------------------------
void SpriteAtlasScene::OnEnter() noexcept {
    // editor らしいニュートラルグレー (背景は ImGui に隠れるが viewport の
    // 外側のクリア色を編集向けに揃える)。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    // ---- workspace 本体 ----
    _workspace.Init();

    // ---- dummy atlas (256x256) を SpritePack に Init ----
    // 実 texture path は文字列リテラルとして渡すだけ (loader 未配線)。
    // panel の viewport は texture を描かず grid placeholder で代用する。
    SpritePackInfo info{};
    info.atlas_texture_path = "assets/dummy_atlas.png";
    info.atlas_width        = 256u;
    info.atlas_height       = 256u;
    _pack.Init(info);

    // ---- 3 frame (Idle / Walk / Jump 各 32x32) を AddFrame ----
    // name は文字列リテラル (SpritePack 規約: caller 所有 / リテラル前提)。
    SpriteFrame f{};
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

    // ---- editor panel を workspace に register ----
    // EditorWorkspace::RegisterPanel は内部で panel->OnInit(*this) を呼ぶ
    // (Phase 21a 規約)。よって panel.OnInit を別途呼ぶ必要は無い。
    // SetSpritePack は Init / OnInit のどちらより後でも構わない (panel が
    // pack の存在を毎フレーム確認しているため)。
    _editor_panel.Init();
    _workspace.RegisterPanel(&_editor_panel);
    _editor_panel.SetSpritePack(&_pack);

    ACS_LOG_INFO("[SpriteAtlasEditor] entered (256x256 dummy atlas, 3 frames: Idle/Walk/Jump)");
}

// ----------------------------------------------------------------------------
// OnExit — 逆順 shutdown
// ----------------------------------------------------------------------------
void SpriteAtlasScene::OnExit() noexcept {
    // EditorWorkspace::Shutdown は登録済み全 panel に OnShutdown を 1 度ずつ
    // 呼んでから list を Clear する (Phase 21a 規約)。よって個別
    // UnregisterPanel を呼ぶ必要は無い。
    _workspace.Shutdown();
    // panel 本体の internal state を解放 (name pool / selection クリア)。
    _editor_panel.Shutdown();

    // SpritePack は Dtor で frame 配列を解放する (Array の所有を持つ)。
    // 明示 ClearAll しても良いが、scene 破棄と同時なので無くても問題なし。
    _pack.ClearAll();

    ACS_LOG_INFO("[SpriteAtlasEditor] exited");
}

// ----------------------------------------------------------------------------
// OnUpdate — Escape による終了のみ (panel ロジックは OnRender 内に集約)
// ----------------------------------------------------------------------------
// ImGui 関連 (Workspace::TickAllPanels が呼ぶ DrawDockSpace / MenuBar / panel
// DrawUI) はすべて OnRender 側へ。ImGui::Begin 等は NewFrame() と Render() の
// 間でしか呼べないため、ここで Workspace::TickAllPanels は呼ばない。
void SpriteAtlasScene::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }
    (void)dt;
}

// ----------------------------------------------------------------------------
// OnRender — File menu (Save/Load stub) → Workspace 全描画
// ----------------------------------------------------------------------------
void SpriteAtlasScene::OnRender(RenderContext& /*rc*/) noexcept {
    // ---- File メニュー (Workspace::DrawMenuBar の前に push) ----
    // ImGui は同一フレーム内で BeginMainMenuBar を複数回呼んでも 1 個の bar に
    // マージするので、本 sample 専用の File メニューを Workspace の Window/
    // Layout メニューと並べて表示できる。
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save .acsatlas")) {
                // Phase 22 範囲外: serializer 本体は未配線。callback hook だけ
                // 走らせる stub。将来は AtlasSerializer::Save(kAtlasFilePath, _pack)
                // を呼ぶ。
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

    // ---- Workspace 全描画 (1 行で OnFrameBegin → DockSpace → MenuBar →
    //      各 panel DrawUI を順に発火) ----
    _workspace.TickAllPanels(GetGame().DeltaTime());
}

// ============================================================================
// SpriteAtlasGame — ImGui lifecycle を Game に持たせる薄いラッパ
//                   (sample 29/30/31 と完全に同形)
// ============================================================================
class SpriteAtlasGame : public Game {
public:
    void OnStart() noexcept override {
        // ImGui を Window + Renderer に紐付け。失敗時は早期 Quit。
        if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
            ACS_LOG_ERROR("[SpriteAtlasGame] ImGuiCtx.Init failed -> Quit");
            Quit();
            return;
        }
        // 基底の OnStart は InitialScene() を push する。
        Game::OnStart();
    }

    void OnRender() noexcept override {
        // ImGui フレーム開始 → Scene::OnRender で ImGui::* が呼ばれる →
        // ImGui の描画コマンドをコマンドリストに発行、の順。
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
        return MakeUnique<SpriteAtlasScene>();
    }

private:
    ImGuiCtx _imgui;
};

ACS_GAME_MAIN(SpriteAtlasGame)
