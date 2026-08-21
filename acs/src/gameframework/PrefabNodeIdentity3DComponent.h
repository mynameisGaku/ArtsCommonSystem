// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/String.h"
#include "container/StringView.h"
#include "gameframework/AComponent.h"

namespace acs::game {

/** Prefab原本内の対応nodeをApply/Revert後も識別する3D node metadata。 */
class APrefabNodeIdentity3DComponent final : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(APrefabNodeIdentity3DComponent)

    /** PSID3Dに記録された32桁小文字hexのsource node IDを返す。 */
    FStringView SourceNodeId() const noexcept;

    /** 32桁小文字hexのsource node IDを設定する。不正な入力ではfalseを返して既存値を保持する。 */
    bool TrySetSourceNodeId(FStringView source_node_id) noexcept;

private:
    /** 同じPrefab原本から作られた全instanceで共有するsource node ID。 */
    FString m_SourceNodeId;
};

} // namespace acs::game
