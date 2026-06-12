// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — リフレクション名による Component 実体化 (ComponentFactory)
// -----------------------------------------------------------------------------
// 「反射名 → 生きた FComponent2D」を埋める enabling layer の要。reflection factory
// (FTypeRegistry::Create) はエンジンアロケータ確保になったので、その生成物を
// FNode2D が所有できる TUniquePtr<FComponent2D> に安全に包んで返す。これにより
// データ駆動のシーン復元 / Play モードが「型を知らずに」コンポーネントを取り付けられる。
//
// 前提: コンポーネントは FComponent2D を単一継承する (基底が offset 0 → void*==FComponent2D*)。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "memory/UniquePtr.h"

namespace acs::game {

class FComponent2D;

/**
 * 反射名で Component を生成し TUniquePtr<FComponent2D> で返す (FNode2D::AttachComponent 用)。
 *
 * @param name 反射カタログの登録名 (= クラス名、FComponent2D::ReflectName と一致)。
 * @return 生成したコンポーネント。未登録 / 非 Component / default 構築不可 (Abstract) は空。
 */
TUniquePtr<FComponent2D> CreateComponentByName(const char* name) noexcept;

} // namespace acs::game
