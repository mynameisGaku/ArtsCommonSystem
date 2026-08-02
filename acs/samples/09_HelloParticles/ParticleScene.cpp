// SPDX-License-Identifier: Apache-2.0
// HelloParticles — ParticleScene 実装。
#include "ParticleScene.h"

#include "platform/Input.h"

#include <cstdio>

using namespace acs;

namespace helloparticles {

bool CParticleScene::Init(IRhiTexture* glow_tex, FVec2 initial_pos) noexcept {
    if (auto r = m_Ps.Init(kParticleCapacity); r.IsErr()) return false;
    m_Ps.SetTexture(glow_tex);
    ApplyPreset(0, initial_pos);
    return true;
}

void CParticleScene::Shutdown() noexcept {
    m_Ps.Shutdown();
}

void CParticleScene::Update(f32 dt) noexcept {
    if (CInput::IsKeyPressed(EKey::Space)) {
        m_Ps.Emitter().active = !m_Ps.Emitter().active;
    }

    FVec2 mp = CInput::MousePos();
    if (CInput::IsKeyPressed(EKey::Num1)) ApplyPreset(0, mp);
    if (CInput::IsKeyPressed(EKey::Num2)) ApplyPreset(1, mp);
    if (CInput::IsKeyPressed(EKey::Num3)) ApplyPreset(2, mp);
    if (CInput::IsKeyPressed(EKey::Num4)) ApplyPreset(3, mp);

    // エミッタを毎フレームマウス位置に追従
    m_Ps.Emitter().position = mp;

    // 左クリックで爆発
    if (CInput::IsMouseButtonPressed(EMouseButton::Left)) {
        FEmitterDesc burst = FEmitterDesc::Sparks(mp);
        burst.rate_per_sec = 0;            // バーストのみ
        burst.life_seconds = 0.8f;
        FEmitterDesc save = m_Ps.Emitter();
        m_Ps.SetEmitter(burst);
        m_Ps.EmitBurst(120);
        m_Ps.SetEmitter(save);
    }

    if (dt > 0.05f) dt = 0.05f;            // 安全 clamp
    m_Ps.Update(dt);
}

void CParticleScene::Render(CSpriteBatch& batch,
                           FFont& font,
                           u32 screen_h,
                           f32 fps) noexcept {
    m_Ps.Render(batch);

    if (font.AtlasTexture()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "粒子: %u / %u   FPS: %.1f   %s",
                      m_Ps.ActiveCount(), m_Ps.Capacity(),
                      static_cast<double>(fps),
                      m_Ps.Emitter().active ? "ON" : "OFF");
        batch.DrawString(font, buf, 20, 20, FVec4{1,1,1,1});
        batch.DrawString(font, kPresetNames[m_Preset], 20, 44, FVec4{1, 0.85f, 0.4f, 1});
        batch.DrawString(font,
                        "1/2/3/4: 種類  Space: 連続 ON/OFF  左クリック: 爆発  Esc: 終了",
                        20, static_cast<f32>(screen_h - 32),
                        FVec4{0.7f, 0.8f, 0.95f, 1});
    }
}

void CParticleScene::ApplyPreset(u32 idx, FVec2 pos) noexcept {
    m_Preset = idx;
    FEmitterDesc d;
    switch (idx) {
        case 0: d = FEmitterDesc::Fire(pos);     break;
        case 1: d = FEmitterDesc::Sparks(pos);   break;
        case 2: d = FEmitterDesc::Fountain(pos); break;
        case 3: d = FEmitterDesc::Smoke(pos);    break;
        default:d = FEmitterDesc::Fire(pos);     break;
    }
    m_Ps.SetEmitter(d);
}

} // namespace helloparticles
