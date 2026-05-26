// SPDX-License-Identifier: Apache-2.0
// HelloRaycast3D — Application 派生クラス。
// StandardShader / SpriteBatch / Font を所有し、RaycastScene に毎フレーム
// の update / render を委譲する。
#pragma once

#include "app/Application.h"

#include "render/StandardShader.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"

#include "RaycastScene.h"

namespace helloraycast3d {

class HelloRaycast3DApp : public acs::Application {
public:
    void OnStart()                noexcept override;
    void OnUpdate(acs::f32 dt)    noexcept override;
    void OnRender()               noexcept override;
    void OnShutdown()             noexcept override;

private:
    acs::StandardShader _shader;
    acs::SpriteBatch    _batch;
    acs::Font           _font;
    RaycastScene        _scene;
};

} // namespace helloraycast3d
