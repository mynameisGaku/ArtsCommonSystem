// SPDX-License-Identifier: Apache-2.0
// HelloPhysics2D — Application 派生クラス。
// SpriteBatch / Font / ボールテクスチャを所有し、PhysicsScene に毎フレーム
// の update / render を委譲する。
#pragma once

#include "app/Application.h"

#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/IRhiTexture.h"

#include "memory/UniquePtr.h"

#include "PhysicsScene.h"

namespace hellophysics2d {

class HelloPhysics2DApp : public acs::Application {
public:
    void OnStart()                noexcept override;
    void OnUpdate(acs::f32 dt)    noexcept override;
    void OnRender()               noexcept override;
    void OnShutdown()             noexcept override;

private:
    acs::SpriteBatch                _batch;
    acs::Font                       _font;
    acs::TUniquePtr<acs::IRhiTexture> _tex;
    PhysicsScene                    _scene;
};

} // namespace hellophysics2d
