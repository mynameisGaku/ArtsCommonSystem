// SPDX-License-Identifier: Apache-2.0
#include "ViewerPanels.h"

using namespace acs;
using namespace acs::game;

namespace hellomv {

void ViewerPanels::Init(const wchar_t* asset_root) noexcept {
    // Theme: ImGui context 取得後に Init (= Dark preset)。
    // ImGuiCtx::Init は ModelViewerApp::OnStart で済んでいる前提。
    _theme.Init();
    _theme.ApplyPreset(editor_core::EEditorThemePreset::Dark);

    // Workspace 本体。
    _workspace.Init();

    // AssetBrowser: 引数で受けた asset_root を起点に列挙。
    _asset_browser.Init(asset_root);
    _asset_browser.Refresh();

    // 4 modelview panel を workspace に登録。RegisterPanel が OnInit を呼ぶので
    // 別途 panel.OnInit を呼ぶと二重初期化になる。
    _workspace.RegisterPanel(&_viewer_panel);
    _workspace.RegisterPanel(&_info_panel);
    _workspace.RegisterPanel(&_material_panel);
    _workspace.RegisterPanel(&_animation_panel);

    // AssetBrowser は EditorPanel 派生ではないので Workspace 登録対象外。
    // 1 フレームの DrawUI() は Draw() 内で直接呼ぶ。EditorPanel ラッパが追加
    // されたら _workspace.RegisterPanel(&_asset_browser_panel) に置き換える。
}

void ViewerPanels::Shutdown() noexcept {
    _workspace.Shutdown();
    _asset_browser.Shutdown();
    // EditorTheme は明示 Shutdown が無い (Dtor で十分)。
}

void ViewerPanels::Draw(f32 dt) noexcept {
    // Workspace 全描画 (OnFrameBegin → DockSpace → MenuBar → 各 panel DrawUI を
    // 1 行で発火する)。仕様は EditorWorkspace.h §「メインループ」参照。
    // File メニューと同じ MainMenuBar に Window/Layout が push される。
    _workspace.TickAllPanels(dt);

    // AssetBrowser を直接描画 (Workspace 登録対象外)。
    _asset_browser.DrawUI();
}

} // namespace hellomv
