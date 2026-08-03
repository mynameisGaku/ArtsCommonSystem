// SPDX-License-Identifier: Apache-2.0
#include "gameframework/SpriteAnimComponent.h"
#include "gameframework/Sprite2DComponent.h"
#include "gameframework/ANode.h"

namespace acs::game {

void ASpriteAnimComponent::InitGrid(u32 cols, u32 rows, u32 frame_count, f32 fps,
                                    EPlayMode mode) noexcept {
    m_FrameUvs.Reset();
    if (cols == 0) cols = 1;
    if (rows == 0) rows = 1;
    const u32 total = cols * rows;
    if (frame_count == 0 || frame_count > total) frame_count = total;

    const f32 cw = 1.0f / static_cast<f32>(cols);
    const f32 ch = 1.0f / static_cast<f32>(rows);
    for (u32 i = 0; i < frame_count; ++i) {
        const u32 cxi = i % cols;
        const u32 cyi = i / cols;
        const f32 u0 = static_cast<f32>(cxi) * cw;
        const f32 v0 = static_cast<f32>(cyi) * ch;
        m_FrameUvs.Add(FVec4{u0, v0, u0 + cw, v0 + ch});
    }
    m_Anim.Init(frame_count, fps, mode);
}

void ASpriteAnimComponent::BeginFrames(f32 fps, EPlayMode mode) noexcept {
    m_FrameUvs.Reset();
    m_PendingFps  = fps;
    m_PendingMode = mode;
    m_Building    = true;
}

void ASpriteAnimComponent::AddFrameUv(FVec4 uv) noexcept {
    if (!m_Building) return;
    m_FrameUvs.Add(uv);
}

void ASpriteAnimComponent::EndFrames() noexcept {
    if (!m_Building) return;
    m_Building = false;
    const u32 n = m_FrameUvs.Num();
    m_Anim.Init(n == 0 ? 1u : n, m_PendingFps, m_PendingMode);
}

void ASpriteAnimComponent::ApplyCurrentFrame() noexcept {
    if (!m_Sprite || m_FrameUvs.Num() == 0) return;
    u32 i = m_Anim.CurrentFrame();
    if (i >= m_FrameUvs.Num()) i = m_FrameUvs.Num() - 1u;
    m_Sprite->SetUvRect(m_FrameUvs[i]);
}

void ASpriteAnimComponent::OnRequire(ANode& owner) noexcept {
    // 描画先が無ければ既定の ASprite2DComponent を自動追加する。
    // 利用者が先にテクスチャ付きで AddComponent していればそれを使う。
    owner.GetOrAddComponent<ASprite2DComponent>();
}

void ASpriteAnimComponent::OnUpdate(f32 dt) noexcept {
    // add 順非依存にするため sibling sprite は初回 OnUpdate で遅延 lookup。
    if (m_Sprite == nullptr && HasOwner()) {
        m_Sprite = Owner().GetComponent<ASprite2DComponent>();
    }
    m_Anim.Tick(dt);
    ApplyCurrentFrame();
}

} // namespace acs::game
