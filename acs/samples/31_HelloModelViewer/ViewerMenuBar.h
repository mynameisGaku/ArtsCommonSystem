// SPDX-License-Identifier: Apache-2.0
// HelloModelViewer — File メニュー + Theme クイックアクション窓。
//
// 役割:
//   ・MainMenuBar に File > Open Model.../Save Layout/Load Layout/Theme.../Quit
//     を 1 列追加する (Workspace の Window/Layout メニューと同じバーへマージ)。
//   ・File > Theme... のトグル状態を保持し、ON の間だけ Theme Settings 窓と
//     Theme Quick Actions 窓を描画する。
//
// なぜ独立クラスにしたか:
//   File 系コールバックは UI 入力を Scene 状態へ届ける薄いブリッジでしかなく、
//   3D 描画 (ViewerScenePipeline) や panel layout (ViewerPanels) と完全に
//   直交する。Scene 本体から切り出すと UI 変更が render path に波及しなくなる。
#pragma once

#include "foundation/Types.h"

namespace acs::game {
class FGame;
namespace editor_core {
class FEditorWorkspace;
class FEditorTheme;
}
}

namespace hellomv {

class FViewerMenuBar {
public:
    // 1 フレーム分の MainMenuBar 描画と Theme Settings 窓の描画をまとめて呼ぶ。
    // Quit や Layout 保存/復元など Scene 横断の副作用は FGame/Workspace/Theme に
    // 直接書き込む。FEditorWorkspace と FEditorTheme は caller 所有。
    void Draw(acs::game::FGame& game,
              acs::game::editor_core::FEditorWorkspace& workspace,
              acs::game::editor_core::FEditorTheme& theme,
              const char* default_model_path,
              const wchar_t* layout_path,
              const wchar_t* theme_path) noexcept;

private:
    // File > Theme... の toggle 状態。連打で開閉する設計のため永続化する。
    bool m_bShowThemeSettings = false;
};

} // namespace hellomv
