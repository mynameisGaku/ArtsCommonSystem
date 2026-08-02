// SPDX-License-Identifier: Apache-2.0
// HelloFontEditor — FontEditorScene。
// editor_core の CEditorWorkspace に fontedit::AFontEditorPanel を register し、
// 3 face (Noto Sans JP / Noto Sans Mono / fallback emoji) を fallback chain に
// 初期登録する Scene。
#pragma once

#include "gameframework/GameFramework.h"
#include "gameframework/tools/editor_core/EditorWorkspace.h"
#include "gameframework/tools/fontedit/FontEditorPanel.h"

namespace hellofont {

class AFontEditorScene : public acs::game::AScene {
public:
    void OnEnter() noexcept override;
    void OnExit()  noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::FRenderContext& rc) noexcept override;

private:
    // File menu stub の保存先 (現状 callback だけ走らせるため未使用)。
    static constexpr const char* kFontFilePath = "preset.acsfont";

    acs::game::editor_core::CEditorWorkspace  m_Workspace;
    acs::game::fontedit::AFontEditorPanel     m_EditorPanel;
};

} // namespace hellofont
