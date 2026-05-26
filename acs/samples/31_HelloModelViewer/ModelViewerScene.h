// SPDX-License-Identifier: Apache-2.0
// HelloModelViewer — Scene。Workspace + 4 panel + AssetBrowser + Theme + 3D cube。
//
// editor_core (Phase 21a) と modelview (Phase 21b) の 4 panel を 1 個の
// Workspace に集約し、ImGui の外側に sample 17 同等の cube を MVP で描画する。
// MVP は ModelViewerPanel::Camera() の view/proj を使うので、editor 上のマウス
// ドラッグで orbit / dolly した姿勢がそのまま 3D 像に反映される。
#pragma once

#include "gameframework/GameFramework.h"

// ----- editor_core (Phase 21a) -----
#include "gameframework/tools/editor_core/EditorWorkspace.h"
#include "gameframework/tools/editor_core/AssetBrowser.h"
#include "gameframework/tools/editor_core/EditorTheme.h"
#include "gameframework/tools/editor_core/EditorCamera.h"

// ----- modelview (Phase 21b) -----
#include "gameframework/tools/modelview/ModelViewerPanel.h"
#include "gameframework/tools/modelview/ModelInspectorPanel.h"
#include "gameframework/tools/modelview/ModelMaterialPanel.h"
#include "gameframework/tools/modelview/ModelAnimationPanel.h"

// ----- 3D rendering (raw RHI、sample 17 と同じ依存) -----
#include "render/IRhiBuffer.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "memory/UniquePtr.h"

namespace hellomv {

class ModelViewerScene : public acs::game::Scene {
public:
    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::RenderContext& rc) noexcept override;

private:
    // ---- File menu stub (実 dialog / serializer は Phase 21c 以降) ----
    static constexpr const char*    kDefaultModelPath = "assets/sample_cube.mdl";
    static constexpr const wchar_t* kLayoutPath       = L"data/editor/modelviewer.acslayout";
    static constexpr const wchar_t* kThemePath        = L"data/editor/theme.acstheme";
    static constexpr const wchar_t* kAssetRoot        = L"assets/";

    // ---- editor_core (Phase 21a) ----
    acs::game::editor_core::EditorWorkspace   _workspace;
    acs::game::editor_core::AssetBrowser      _asset_browser;
    acs::game::editor_core::EditorTheme       _theme;

    // ---- modelview (Phase 21b、並列実装中) ----
    acs::game::modelview::ModelViewerPanel    _viewer_panel;
    acs::game::modelview::ModelInspectorPanel _info_panel;
    acs::game::modelview::ModelMaterialPanel  _material_panel;
    acs::game::modelview::ModelAnimationPanel _animation_panel;

    // ---- 3D resource (sample 17 と同形) ----
    acs::UniquePtr<acs::IRhiShader>   _vs;
    acs::UniquePtr<acs::IRhiShader>   _ps;
    acs::UniquePtr<acs::IRhiBuffer>   _vb;
    acs::UniquePtr<acs::IRhiBuffer>   _ib;
    acs::UniquePtr<acs::IRhiBuffer>   _cb;
    acs::UniquePtr<acs::IRhiPipeline> _pipeline;
    acs::f32                          _angle = 0.0f;

    // ---- UI 状態 ----
    bool _show_theme_settings = false;
};

} // namespace hellomv
