// SPDX-License-Identifier: Apache-2.0
// HelloFontEditor — GameFramework Phase 23: FontEditor デモ
//
// 動作:
//   ・editor_core (Phase 21a) の EditorWorkspace を 1 個立てて、Phase 23 で
//     並列実装された `fontedit::FontEditorPanel` を register する。
//   ・起動時に 3 face を fallback chain に初期登録:
//       [00] "Noto Sans JP"   : 日本語/英数共通プライマリ (path は stub)
//       [01] "Noto Sans Mono" : 等幅 ASCII / 記号 (path は stub)
//       [02] "fallback emoji" : Emoji / Symbol 拡張 (path は stub)
//   ・preview text には "ACS Font Editor サンプル 123 αβγ ★★★" を入れる。
//   ・MainMenuBar > File に Save / Load ".acsfont" を stub callback で配線。
//     Phase 23 範囲外なので serializer 本体は未配線、ACS_LOG_INFO で発火を
//     ログするだけ。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (samples/21/29/30/31/35 と同じ理由で、
//                  ImGuiCtx が DX12 raw backend 経由のため)。
//
// 設計メモ:
//   ・ImGuiCtx は **Game クラスが所有**。OnStart / OnShutdown / OnEvent で
//     lifecycle を回し、OnRender を override して NewFrame() → Scene OnRender
//     → Render() の順で挟む (= sample 29/30/31/35 と完全に同形)。
//   ・Scene は WantedServices() を None のままにする (= scene clock 等の自動
//     tick は不要)。
//   ・font face の file_path / family_name は文字列リテラルを直接 panel に
//     渡す (= ACS 規約: STL/`<string>` 禁止、caller 所有のリテラル前提)。
//     実 atlas ロードは Phase 23 範囲外なので path は stub。

#include "gameframework/GameFramework.h"
#include "gameframework/tools/editor_core/EditorWorkspace.h"
#include "gameframework/tools/fontedit/FontEditorPanel.h"

#include "imgui/ImGuiContext.h"
#include "platform/Input.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

// ============================================================================
// FontEditorScene — Workspace + FontEditorPanel + 初期 3 face 登録
// ============================================================================
class FontEditorScene : public Scene {
public:
    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnRender(RenderContext& rc) noexcept override;

private:
    // File menu stub の保存先 (現状 callback だけ走らせるため未使用)。
    static constexpr const char* kFontFilePath = "preset.acsfont";

    // ---- editor_core ----
    editor_core::EditorWorkspace        _workspace;

    // ---- fontedit (Phase 23) ----
    fontedit::FontEditorPanel           _editor_panel;
};

// ----------------------------------------------------------------------------
// OnEnter — workspace 初期化 + 初期 3 face 登録 + editor panel 登録
// ----------------------------------------------------------------------------
void FontEditorScene::OnEnter() noexcept {
    // editor らしいニュートラルグレー (背景は ImGui に隠れるが viewport の
    // 外側のクリア色を編集向けに揃える、sample 35 と同調)。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    // ---- workspace 本体 ----
    _workspace.Init();

    // ---- editor panel を workspace に register ----
    // EditorWorkspace::RegisterPanel は内部で panel->OnInit(*this) を呼ぶ
    // (Phase 21a 規約)。よって panel.OnInit を別途呼ぶ必要は無い。
    // SetPreviewText / AddFontFace は OnInit より後でも構わない。
    _editor_panel.Init();
    _workspace.RegisterPanel(&_editor_panel);

    // ---- 初期 3 face を fallback chain に登録 ----
    // 注意: file_path は stub (実際の loader 統合は Phase 23+)。path 文字列は
    // 静的リテラル (.rdata) なので panel から非所有参照しても安全。
    {
        fontedit::FontFaceInfo jp{};
        jp.file_path      = L"assets/fonts/NotoSansJP-Regular.otf";
        jp.family_name    = "Noto Sans JP";
        jp.base_size_px   = 24.0f;
        jp.char_range_min = 0x0020u;
        jp.char_range_max = 0xFFFFu;
        jp.is_msdf        = true;
        _editor_panel.AddFontFace(jp);
    }
    {
        fontedit::FontFaceInfo mono{};
        mono.file_path      = L"assets/fonts/NotoSansMono-Regular.ttf";
        mono.family_name    = "Noto Sans Mono";
        mono.base_size_px   = 20.0f;
        mono.char_range_min = 0x0020u;
        mono.char_range_max = 0x024Fu;
        mono.is_msdf        = false;
        _editor_panel.AddFontFace(mono);
    }
    {
        fontedit::FontFaceInfo emoji{};
        emoji.file_path      = L"assets/fonts/EmojiOne-Color.otf";
        emoji.family_name    = "fallback emoji";
        emoji.base_size_px   = 32.0f;
        emoji.char_range_min = 0x1F300u;
        emoji.char_range_max = 0x1FAFFu;
        emoji.is_msdf        = false;
        _editor_panel.AddFontFace(emoji);
    }

    // ---- preview text ----
    _editor_panel.SetPreviewText("ACS Font Editor サンプル 123 αβγ ★★★");
    _editor_panel.SetPreviewFontSize(28.0f);

    // 最初の face を選択 (UX: 起動直後に inspector が表示される)。
    _editor_panel.SelectFace(0);

    ACS_LOG_INFO("[FontEditor] entered (3 faces: Noto Sans JP / Noto Sans Mono / fallback emoji)");
}

// ----------------------------------------------------------------------------
// OnExit — 逆順 shutdown
// ----------------------------------------------------------------------------
void FontEditorScene::OnExit() noexcept {
    // EditorWorkspace::Shutdown は登録済み全 panel に OnShutdown を 1 度ずつ
    // 呼んでから list を Clear する (Phase 21a 規約)。よって個別
    // UnregisterPanel を呼ぶ必要は無い。
    _workspace.Shutdown();
    // panel 本体の internal state を解放 (face 配列 / preview バッファクリア)。
    _editor_panel.Shutdown();

    ACS_LOG_INFO("[FontEditor] exited");
}

// ----------------------------------------------------------------------------
// OnUpdate — Escape による終了のみ (panel ロジックは OnRender 内に集約)
// ----------------------------------------------------------------------------
// ImGui 関連 (Workspace::TickAllPanels が呼ぶ DrawDockSpace / MenuBar / panel
// DrawUI) はすべて OnRender 側へ。ImGui::Begin 等は NewFrame() と Render() の
// 間でしか呼べないため、ここで Workspace::TickAllPanels は呼ばない。
void FontEditorScene::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }
    (void)dt;
}

// ----------------------------------------------------------------------------
// OnRender — File menu (Save/Load stub) → Workspace 全描画
// ----------------------------------------------------------------------------
void FontEditorScene::OnRender(RenderContext& /*rc*/) noexcept {
    // ---- File メニュー (Workspace::DrawMenuBar の前に push) ----
    // ImGui は同一フレーム内で BeginMainMenuBar を複数回呼んでも 1 個の bar に
    // マージするので、本 sample 専用の File メニューを Workspace の Window/
    // Layout メニューと並べて表示できる。
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save .acsfont")) {
                // Phase 23 範囲外: serializer 本体は未配線。callback hook だけ
                // 走らせる stub。将来は FontSerializer::Save(kFontFilePath,
                //                                            _editor_panel) を呼ぶ。
                ACS_LOG_INFO("[FontEditor] Save .acsfont -> '%s' (stub, no-op, %u faces)",
                             kFontFilePath,
                             static_cast<unsigned>(_editor_panel.FontFaceCount()));
            }
            if (ImGui::MenuItem("Load .acsfont")) {
                ACS_LOG_INFO("[FontEditor] Load .acsfont <- '%s' (stub, no-op)", kFontFilePath);
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
// FontEditorGame — ImGui lifecycle を Game に持たせる薄いラッパ
//                  (sample 29/30/31/35 と完全に同形)
// ============================================================================
class FontEditorGame : public Game {
public:
    void OnStart() noexcept override {
        // ImGui を Window + Renderer に紐付け。失敗時は早期 Quit。
        if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
            ACS_LOG_ERROR("[FontEditorGame] ImGuiCtx.Init failed -> Quit");
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
        return MakeUnique<FontEditorScene>();
    }

private:
    ImGuiCtx _imgui;
};

ACS_GAME_MAIN(FontEditorGame)
