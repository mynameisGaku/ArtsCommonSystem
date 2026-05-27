// SPDX-License-Identifier: Apache-2.0
// HelloParticles — FApplication 派生クラス。
// FSpriteBatch / Font / Glow テクスチャを所有し、ParticleScene に毎フレーム
// の update / render を委譲する。
#pragma once

#include "app/Application.h"

#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/IRhiTexture.h"

#include "memory/UniquePtr.h"

#include "ParticleScene.h"

namespace helloparticles {

class HelloParticlesApp : public acs::FApplication {
public:
    void OnStart()                noexcept override;
    void OnUpdate(acs::f32 dt)    noexcept override;
    void OnRender()               noexcept override;
    void OnShutdown()             noexcept override;

private:
    acs::FSpriteBatch                 m_Batch;
    acs::Font                        m_Font;
    acs::TUniquePtr<acs::IRhiTexture> m_Glow;
    ParticleScene                    m_Scene;
};

} // namespace helloparticles
