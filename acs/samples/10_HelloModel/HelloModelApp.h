// SPDX-License-Identifier: Apache-2.0
// HelloModel — FApplication 派生クラス。
// FStandardShader / 非同期 mesh ロードを所有し、ModelScene に毎フレームの
// update / render を委譲する。
#pragma once

#include "app/Application.h"

#include "asset/AssetFuture.h"
#include "render/StandardShader.h"

#include "ModelScene.h"

namespace hellomodel {

class HelloModelApp : public acs::FApplication {
public:
    void OnStart()                noexcept override;
    void OnUpdate(acs::f32 dt)    noexcept override;
    void OnRender()               noexcept override;
    void OnShutdown()             noexcept override;

private:
    acs::FStandardShader m_Shader;
    acs::FAssetFuture    m_AsyncMesh;
    bool                m_bAsyncLoaded = false;
    ModelScene          m_Scene;
};

} // namespace hellomodel
