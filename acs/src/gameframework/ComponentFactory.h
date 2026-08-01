// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — リフレクション名による Component 実体化 (ComponentFactory)
// -----------------------------------------------------------------------------
// 「反射名 → 生きた AComponent」を埋める enabling layer の要。reflection factory
// (CTypeRegistry::Create) はエンジンアロケータ確保になったので、その生成物を
// ANode が所有できる TUniquePtr<AComponent> に安全に包んで返す。これにより
// データ駆動のシーン復元 / Play モードが「型を知らずに」コンポーネントを取り付けられる。
//
// 前提: コンポーネントは AComponent を単一継承する (基底が offset 0 → void*==AComponent*)。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "memory/UniquePtr.h"

namespace acs::game {

class AComponent;

/**
 * 反射名で Component を生成し TUniquePtr<AComponent> で返す (ANode::AttachComponent 用)。
 *
 * @param name 反射カタログの登録名 (= クラス名、AComponent::ReflectName と一致)。
 * @return 生成したコンポーネント。未登録 / 非 Component / default 構築不可 (Abstract) は空。
 */
TUniquePtr<AComponent> CreateComponentByName(const char* name) noexcept;

} // namespace acs::game
