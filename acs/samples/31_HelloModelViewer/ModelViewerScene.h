// SPDX-License-Identifier: Apache-2.0
// HelloModelViewer — Scene。Workspace + 4 panel + CAssetBrowser + Theme + 3D cube。
//
// editor_core と modelview の 4 panel を 1 個の Workspace に集約し、ImGui の
// 外側 (= フレームバッファ直書き) に sample 17 同等の cube を MVP で描画する。
// MVP は AModelViewerPanel::Camera() の view/proj を使うので、editor 上のマウス
// ドラッグで orbit / dolly した姿勢がそのまま 3D 像に反映される。
//
// 本ファイルは 3 つのサブモジュールを束ねる薄いオーケストレータ:
//   ・ViewerScenePipeline — 3D RHI リソース所有 / MVP 更新 / draw call 発行
//   ・ViewerPanels        — workspace + asset browser + 4 panel + theme の集約
//   ・ViewerMenuBar       — File メニュー + Theme クイックアクション窓
#pragma once

#include "gameframework/GameFramework.h"

#include "ViewerScenePipeline.h"
#include "ViewerPanels.h"
#include "ViewerMenuBar.h"

namespace hellomv {

class AModelViewerScene : public acs::game::AScene {
public:
    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::FRenderContext& rc) noexcept override;

private:
    // File menu stub (実 dialog / serializer は未配線)。
    static constexpr const char*    kDefaultModelPath = "assets/sample_cube.mdl";
    static constexpr const wchar_t* kLayoutPath       = L"data/editor/modelviewer.acslayout";
    static constexpr const wchar_t* kThemePath        = L"data/editor/theme.acstheme";
    static constexpr const wchar_t* kAssetRoot        = L"assets/";

    CViewerPanels         m_Panels;
    CViewerScenePipeline  m_Pipeline;
    CViewerMenuBar        m_MenuBar;
    acs::f32             m_Angle = 0.0f;
};

} // namespace hellomv
