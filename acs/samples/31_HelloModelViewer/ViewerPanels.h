// SPDX-License-Identifier: Apache-2.0
// HelloModelViewer — エディタ UI 集約レイヤ (workspace + 4 panel + asset browser + theme)。
//
// 役割:
//   ・editor_core (Workspace / FAssetBrowser / FEditorTheme) と modelview 配下の
//     4 panel (Viewer / Inspector / Material / FAnimation) を 1 個のオブジェクトに
//     束ねる。
//   ・各 panel の登録順 / shutdown 順 / 1 フレーム描画順を FScene から隠す。
//
// なぜ独立クラスにしたか:
//   panel 並びの増減 / theme 切替 / asset browser の差替は UI 工事の度に発生する。
//   この変更点を FScene 本体から物理的に分離することで、FScene は「3D + UI」の
//   2 ブロックを呼ぶだけのオーケストレータに専念できる。
#pragma once

#include "gameframework/tools/editor_core/EditorWorkspace.h"
#include "gameframework/tools/editor_core/AssetBrowser.h"
#include "gameframework/tools/editor_core/EditorTheme.h"
#include "gameframework/tools/modelview/ModelViewerPanel.h"
#include "gameframework/tools/modelview/ModelInspectorPanel.h"
#include "gameframework/tools/modelview/ModelMaterialPanel.h"
#include "gameframework/tools/modelview/ModelAnimationPanel.h"

namespace hellomv {

class ViewerPanels {
public:
    // theme preset 適用 → workspace 初期化 → asset browser 起動 → 4 panel 登録。
    // FEditorWorkspace::RegisterPanel は内部で `panel->OnInit(*this)` を呼ぶので、
    // 個別に panel.OnInit を呼ぶと二重初期化になる (= 呼ばない)。
    void Init(const wchar_t* asset_root) noexcept;

    // 逆順 shutdown。FEditorWorkspace::Shutdown は登録済み panel に OnShutdown を
    // 1 度ずつ呼んでから list を Clear するので、個別 UnregisterPanel は不要。
    // FEditorTheme::Shutdown は API として存在しない (Dtor で十分)。
    void Shutdown() noexcept;

    // OnUpdate からカメラ姿勢を引くため viewer panel を露出。
    acs::game::modelview::FModelViewerPanel&    Viewer()    noexcept { return _viewer_panel; }

    // OnUpdate で animation 時間を進めるため animation panel を露出。
    acs::game::modelview::FModelAnimationPanel& FAnimation() noexcept { return _animation_panel; }

    // OnRender が File メニュー描画のために theme / workspace を必要とする。
    acs::game::editor_core::FEditorWorkspace&   Workspace() noexcept { return _workspace; }
    acs::game::editor_core::FEditorTheme&       Theme()     noexcept { return _theme; }

    // 1 フレーム分の panel 群 + asset browser を描画する。
    // ImGui::Begin 系を含むため NewFrame() と Render() の間でしか呼べない。
    void Draw(acs::f32 dt) noexcept;

private:
    acs::game::editor_core::FEditorWorkspace   _workspace;
    acs::game::editor_core::FAssetBrowser      _asset_browser;
    acs::game::editor_core::FEditorTheme       _theme;
    acs::game::modelview::FModelViewerPanel    _viewer_panel;
    acs::game::modelview::FModelInspectorPanel _info_panel;
    acs::game::modelview::FModelMaterialPanel  _material_panel;
    acs::game::modelview::FModelAnimationPanel _animation_panel;
};

} // namespace hellomv
