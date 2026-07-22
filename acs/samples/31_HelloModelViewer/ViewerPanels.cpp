// SPDX-License-Identifier: Apache-2.0
#include "ViewerPanels.h"

using namespace acs;
using namespace acs::game;

namespace hellomv {

void FViewerPanels::Init(const wchar_t* asset_root) noexcept {
    // Theme: ImGui context 取得後に Init (= Dark preset)。
    // ImGuiCtx::Init は ModelViewerApp::OnStart で済んでいる前提。
    m_Theme.Init();
    m_Theme.ApplyPreset(editor_core::EEditorThemePreset::Dark);

    // Workspace 本体。
    m_Workspace.Init();

    // FAssetBrowser: 引数で受けた asset_root を起点に列挙。
    m_AssetBrowser.Init(asset_root);
    m_AssetBrowser.Refresh();

    // 4 modelview panel を workspace に登録。RegisterPanel が OnInit を呼ぶので
    // 別途 panel.OnInit を呼ぶと二重初期化になる。
    m_Workspace.RegisterPanel(&m_ViewerPanel);
    m_Workspace.RegisterPanel(&m_InfoPanel);
    m_Workspace.RegisterPanel(&m_MaterialPanel);
    m_Workspace.RegisterPanel(&m_AnimationPanel);

    // FAssetBrowser は FEditorPanel 派生ではないので Workspace 登録対象外。
    // 1 フレームの DrawUI() は Draw() 内で直接呼ぶ。FEditorPanel ラッパが追加
    // されたら m_Workspace.RegisterPanel(&m_AssetBrowserPanel) に置き換える。
}

void FViewerPanels::Shutdown() noexcept {
    m_Workspace.Shutdown();
    m_AssetBrowser.Shutdown();
    // FEditorTheme は明示 Shutdown が無い (Dtor で十分)。
}

void FViewerPanels::Draw(f32 dt) noexcept {
    // Workspace 全描画 (OnFrameBegin → DockSpace → MenuBar → 各 panel DrawUI を
    // 1 行で発火する)。仕様は FEditorWorkspace.h §「メインループ」参照。
    // File メニューと同じ MainMenuBar に Window/Layout が push される。
    m_Workspace.TickAllPanels(dt);

    // FAssetBrowser を直接描画 (Workspace 登録対象外)。
    m_AssetBrowser.DrawUI();
}

} // namespace hellomv
