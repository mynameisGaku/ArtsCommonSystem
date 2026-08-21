// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/String.h"
#include "container/StringView.h"
#include "gameframework/AComponent.h"
#include "gameframework/PrefabNodeProperty3D.h"

namespace acs::game {

/** Prefab原本内の対応nodeとinstance側の明示property overrideを保持する3D node metadata。 */
class APrefabNodeIdentity3DComponent final : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(APrefabNodeIdentity3DComponent)

    /** PSID3Dに記録された32桁小文字hexのsource node IDを返す。 */
    FStringView SourceNodeId() const noexcept;

    /** 32桁小文字hexのsource node IDを設定する。不正な入力ではfalseを返して既存値を保持する。 */
    bool TrySetSourceNodeId(FStringView source_node_id) noexcept;

    /** PNOVR3Dに記録されたchild node property override maskを返す。 */
    u32 NodePropertyOverrideMask() const noexcept;

    /** 定義済みbitだけのchild node property override maskを設定する。未知bitでは失敗する。 */
    bool TrySetNodePropertyOverrideMask(u32 mask) noexcept;

private:
    /** 同じPrefab原本から作られた全instanceで共有するsource node ID。 */
    FString m_SourceNodeId;

    /** 原本更新後もinstance値を維持するchild node propertyのbit集合。 */
    u32 m_NodePropertyOverrideMask = 0u;
};

} // namespace acs::game
