// SPDX-License-Identifier: Apache-2.0
// HelloFontEditor — FontEditorScene 実装。
#include "FontEditorScene.h"

#include "platform/Input.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

namespace hellofont {

// ----------------------------------------------------------------------------
// OnEnter — workspace 初期化 + 初期 3 face 登録 + editor panel 登録
// ----------------------------------------------------------------------------
void FontEditorScene::OnEnter() noexcept {
    // editor らしいニュートラルグレー (背景は ImGui に隠れるが viewport の
    // 外側のクリア色を編集向けに揃える)。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    _workspace.Init();

    // EditorWorkspace::RegisterPanel は内部で panel->OnInit(*this) を呼ぶ。
    // よって panel.OnInit を別途呼ぶ必要は無い。
    // SetPreviewText / AddFontFace は OnInit より後でも構わない。
    _editor_panel.Init();
    _workspace.RegisterPanel(&_editor_panel);

    // ---- 初期 3 face を fallback chain に登録 ----
    // file_path は stub (実 loader 統合は未着手)。path 文字列は静的リテラル
    // (.rdata) なので panel から非所有参照しても安全。
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
        emoji.file_path      = L"assets/fonts/EmojiOne-FColor.otf";
        emoji.family_name    = "fallback emoji";
        emoji.base_size_px   = 32.0f;
        emoji.char_range_min = 0x1F300u;
        emoji.char_range_max = 0x1FAFFu;
        emoji.is_msdf        = false;
        _editor_panel.AddFontFace(emoji);
    }

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
    // 呼んでから list を Clear する。よって個別 UnregisterPanel を呼ぶ必要は無い。
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
    // ImGui は同一フレーム内で BeginMainMenuBar を複数回呼んでも 1 個の bar に
    // マージするので、本 sample 専用の File メニューを Workspace の Window/
    // Layout メニューと並べて表示できる。
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save .acsfont")) {
                // serializer 本体は未配線。callback hook だけ走らせる stub。
                // 将来は FontSerializer::Save(kFontFilePath, _editor_panel) を呼ぶ。
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

    // Workspace 全描画 (1 行で OnFrameBegin → DockSpace → MenuBar → 各 panel
    // DrawUI を順に発火)。
    _workspace.TickAllPanels(GetGame().DeltaTime());
}

} // namespace hellofont
