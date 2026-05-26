// SPDX-License-Identifier: Apache-2.0
// HelloRaycast3D — Application 派生クラス。
//
// StandardShader / SpriteBatch / Font を所有し、毎フレームの update / render を
// RaycastScene に委譲する。リソース所有とフレームループを分離する典型的な構成。
#pragma once

#include "app/Application.h"

#include "render/StandardShader.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"

#include "RaycastScene.h"

namespace helloraycast3d {

class HelloRaycast3DApp : public acs::Application {
public:
    void OnStart()             noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()            noexcept override;
    void OnShutdown()          noexcept override;

private:
    acs::StandardShader _shader;
    acs::SpriteBatch    _batch;
    acs::Font           _font;
    RaycastScene        _scene;
};

} // namespace helloraycast3d
