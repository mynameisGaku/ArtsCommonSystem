// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/String.h"
#include "container/StringView.h"
#include "gameframework/AComponent.h"

namespace acs::game {

/**
 * 実体化済み3DサブツリーとPrefab/Blueprint原本を結ぶコンポーネント。
 *
 * @details runtime状態はシーンに保存済みのノード群であり、本型は原本を再展開しない。
 * EditorのApply/Revertや参照追跡に必要なPFAB3Dパスだけを所有する。
 */
class APrefabLink3DComponent final : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(APrefabLink3DComponent)

    /** PFAB3Dに記録されたPrefabまたはBlueprintの原本パスを返す。 */
    FStringView SourcePath() const noexcept;

    /** PFAB3Dに記録するPrefabまたはBlueprintの原本パスを設定する。 */
    void SetSourcePath(FStringView path) noexcept;

private:
    /** 実体化済みサブツリーの原本を指す非実行リンク。 */
    FString m_SourcePath;
};

} // namespace acs::game
