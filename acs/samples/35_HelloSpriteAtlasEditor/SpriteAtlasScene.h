// SPDX-License-Identifier: Apache-2.0
// HelloSpriteAtlasEditor — SpriteAtlasScene。
// FEditorWorkspace に FSpriteAtlasEditorPanel を register し、256x256 dummy atlas
// + 3 frame (Idle / Walk / Jump) を初期登録する Scene。
#pragma once

#include "gameframework/GameFramework.h"
#include "gameframework/SpritePack.h"
#include "gameframework/tools/editor_core/EditorWorkspace.h"
#include "gameframework/tools/spriteatlas/SpriteAtlasEditorPanel.h"

namespace hellosa {

class FSpriteAtlasScene : public acs::game::FScene {
public:
    void OnEnter() noexcept override;
    void OnExit()  noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::FRenderContext& rc) noexcept override;

private:
    // File menu stub の保存先 (現状 callback だけ走らせるため未使用)。
    static constexpr const char* kAtlasFilePath = "preset.acsatlas";

    acs::game::editor_core::FEditorWorkspace           m_Workspace;
    acs::game::spriteatlas::FSpriteAtlasEditorPanel    m_EditorPanel;

    // 編集対象 FSpritePack (256x256 dummy atlas + 3 frame)。
    // FSpritePack は非コピー / 非ムーブなのでメンバ直保持。`m_EditorPanel` に
    // SetSpritePack(&m_Pack) で raw 注入する。
    acs::game::FSpritePack                             m_Pack;
};

} // namespace hellosa
