// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Scene.h"

/** Scene.hだけで正規描画コンテキストをoverrideできることを固定する。 */
class FSceneHeaderFirstConsumer final : public acs::AScene {
public:
    void OnRender(acs::FRenderContext&) noexcept override {}

    /** scene ownerとmanagerを正規namespaceだけで受け取る。 */
    void Accept(acs::CGame*, acs::CSceneManager*) noexcept {}
};
