// SPDX-License-Identifier: Apache-2.0
// HelloRaycast3D — CApplication 派生クラス。
//
// CStandardShader / CSpriteBatch / FFont を所有し、毎フレームの update / render を
// RaycastScene に委譲する。リソース所有とフレームループを分離する典型的な構成。
#pragma once

#include "app/Application.h"

#include "render/StandardShader.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"

#include "RaycastScene.h"

namespace helloraycast3d {

class CHelloRaycast3DApp : public acs::CApplication {
public:
    void OnStart()             noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()            noexcept override;
    void OnShutdown()          noexcept override;

private:
    acs::CStandardShader m_Shader;
    acs::CSpriteBatch    m_Batch;
    acs::FFont           m_Font;
    CRaycastScene        m_Scene;
};

} // namespace helloraycast3d
