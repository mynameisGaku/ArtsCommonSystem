// SPDX-License-Identifier: Apache-2.0
#include "gameframework/AComponent.h"

/** AComponent.hだけで公開forward型をoverrideできることを固定する。 */
class CAComponentHeaderFirstConsumer final : public acs::AComponent {
public:
    void OnAttach(acs::ANode&) noexcept override {}
    void OnDraw(acs::FRenderContext&) noexcept override {}
    void OnAttachServices(acs::CSceneServices&) noexcept override {}
    bool QueryLight(acs::FLightDesc2D&) const noexcept override { return false; }
};
