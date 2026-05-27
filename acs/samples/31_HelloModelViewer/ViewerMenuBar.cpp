// SPDX-License-Identifier: Apache-2.0
#include "ViewerMenuBar.h"

#include "gameframework/Game.h"
#include "gameframework/tools/editor_core/EditorWorkspace.h"
#include "gameframework/tools/editor_core/EditorTheme.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

namespace hellomv {

void ViewerMenuBar::Draw(FGame& game,
                         editor_core::FEditorWorkspace& workspace,
                         editor_core::FEditorTheme& theme,
                         const char* default_model_path,
                         const wchar_t* layout_path,
                         const wchar_t* theme_path) noexcept {
    // ImGui は同一フレーム内で BeginMainMenuBar を複数回呼んでも 1 個の bar に
    // マージするので、本 sample 専用の File メニューを Workspace の FWindow/
    // Layout メニューと並べて表示できる。
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Model...")) {
                // 実ダイアログは未配線。callback hook だけ走らせる。
                ACS_LOG_INFO("[ModelViewer] Open Model... -> '%s' (stub, no-op)", default_model_path);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save Layout")) {
                workspace.SaveLayout(layout_path);
                ACS_LOG_INFO("[ModelViewer] Save Layout -> 'data/editor/modelviewer.acslayout'");
            }
            if (ImGui::MenuItem("Load Layout")) {
                workspace.LoadLayout(layout_path);
                ACS_LOG_INFO("[ModelViewer] Load Layout <- 'data/editor/modelviewer.acslayout'");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Theme...")) {
                m_ShowThemeSettings = !m_ShowThemeSettings;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) {
                game.Quit();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // ---- Theme FSettings 窓 (File > Theme... で toggle) ----
    if (m_ShowThemeSettings) {
        theme.DrawThemeSettingsUI();
        // DrawThemeSettingsUI 内にも Save/Load ボタンはあるが、固定パスで明示
        // 保存できるショートカットを別 window で出す (本 sample 専用のクイック
        // アクション)。
        if (ImGui::Begin("Theme Quick Actions")) {
            if (ImGui::Button("Save theme to file")) {
                theme.SaveTheme(theme_path);
                ACS_LOG_INFO("[ModelViewer] Theme saved -> 'data/editor/theme.acstheme'");
            }
            ImGui::SameLine();
            if (ImGui::Button("Load theme from file")) {
                theme.LoadTheme(theme_path);
                ACS_LOG_INFO("[ModelViewer] Theme loaded <- 'data/editor/theme.acstheme'");
            }
        }
        ImGui::End();
    }
}

} // namespace hellomv
