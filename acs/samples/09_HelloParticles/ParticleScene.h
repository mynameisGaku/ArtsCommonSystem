// SPDX-License-Identifier: Apache-2.0
// HelloParticles — パーティクルシステムの管理 + 描画を担当する scene。
// Application 派生はリソース所有 (SpriteBatch / Font / Glow テクスチャ) を
// 担当し、毎フレームの update / render を ParticleScene に委譲する。
#pragma once

#include "Types.h"

#include "render/SpriteBatch.h"
#include "render/Particles.h"
#include "render/Font.h"
#include "render/IRhiTexture.h"
#include "math/Vec.h"

namespace helloparticles {

class ParticleScene {
public:
    // ParticleSystem の初期化 + 初期 preset の適用。
    // 失敗時は false。
    bool Init(acs::IRhiTexture* glow_tex, acs::Vec2 initial_pos) noexcept;

    void Shutdown() noexcept;

    // ユーザー入力 + 1 フレームのシミュレーション。
    void Update(acs::f32 dt) noexcept;

    // ParticleSystem の draw + HUD。
    void Render(acs::SpriteBatch& batch,
                acs::Font& font,
                acs::u32 screen_w, acs::u32 screen_h,
                acs::f32 fps) noexcept;

private:
    void ApplyPreset(acs::u32 idx, acs::Vec2 pos) noexcept;

    acs::ParticleSystem _ps;
    acs::u32            _preset = 0;
};

} // namespace helloparticles
