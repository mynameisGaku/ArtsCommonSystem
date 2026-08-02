// SPDX-License-Identifier: Apache-2.0
#include "gameframework/ANode.h"

#include <type_traits>

/** ANode.hだけで公開forward型を参照できることを固定する。 */
class CANodeHeaderFirstConsumer final : public acs::ANode {
public:
    void OnDraw(acs::FRenderContext&) noexcept override {}

    /** nodeの公開forward型を正規namespaceだけで受け取る。 */
    void Accept(acs::CSceneServices&, const acs::FMaterial2D&) noexcept {}
};

static_assert(std::is_same_v<
              decltype(&acs::ANode::TryAddChild),
              acs::EAddChildResult (acs::ANode::*)(acs::TObjectPtr<acs::ANode>&) noexcept>);
static_assert(acs::EAddChildResult::Added == static_cast<acs::EAddChildResult>(0u));
static_assert(acs::kNodeMaxTreeDepth > 0u);
