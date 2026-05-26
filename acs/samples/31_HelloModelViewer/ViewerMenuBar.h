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
class Game;
namespace editor_core {
class EditorWorkspace;
class EditorTheme;
}
}

namespace hellomv {

class ViewerMenuBar {
public:
    // 1 フレーム分の MainMenuBar 描画と Theme Settings 窓の描画をまとめて呼ぶ。
    // Quit や Layout 保存/復元など Scene 横断の副作用は Game/Workspace/Theme に
    // 直接書き込む。EditorWorkspace と EditorTheme は caller 所有。
    void Draw(acs::game::Game& game,
              acs::game::editor_core::EditorWorkspace& workspace,
              acs::game::editor_core::EditorTheme& theme,
              const char* default_model_path,
              const wchar_t* layout_path,
              const wchar_t* theme_path) noexcept;

private:
    // File > Theme... の toggle 状態。連打で開閉する設計のため永続化する。
    bool _show_theme_settings = false;
};

} // namespace hellomv
