// SPDX-License-Identifier: Apache-2.0
#include "gameframework/SpriteAnimComponent.h"
#include "gameframework/Sprite2DComponent.h"
#include "gameframework/Node2D.h"

namespace acs::game {

void FSpriteAnimComponent::InitGrid(u32 cols, u32 rows, u32 frame_count, f32 fps,
                                    EPlayMode mode) noexcept {
    m_FrameUvs.Clear();
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
        m_FrameUvs.PushBack(FVec4{u0, v0, u0 + cw, v0 + ch});
    }
    m_Anim.Init(frame_count, fps, mode);
}

void FSpriteAnimComponent::BeginFrames(f32 fps, EPlayMode mode) noexcept {
    m_FrameUvs.Clear();
    m_PendingFps  = fps;
    m_PendingMode = mode;
    m_Building    = true;
}

void FSpriteAnimComponent::AddFrameUv(FVec4 uv) noexcept {
    if (!m_Building) return;
    m_FrameUvs.PushBack(uv);
}

void FSpriteAnimComponent::EndFrames() noexcept {
    if (!m_Building) return;
    m_Building = false;
    const u32 n = m_FrameUvs.Size();
    m_Anim.Init(n == 0 ? 1u : n, m_PendingFps, m_PendingMode);
}

void FSpriteAnimComponent::ApplyCurrentFrame() noexcept {
    if (!m_Sprite || m_FrameUvs.Size() == 0) return;
    u32 i = m_Anim.CurrentFrame();
    if (i >= m_FrameUvs.Size()) i = m_FrameUvs.Size() - 1u;
    m_Sprite->SetUvRect(m_FrameUvs[i]);
}

void FSpriteAnimComponent::OnUpdate(f32 dt) noexcept {
    // add 順非依存にするため sibling sprite は初回 OnUpdate で遅延 lookup。
    if (m_Sprite == nullptr && HasOwner()) {
        m_Sprite = Owner().GetComponent<FSprite2DComponent>();
    }
    m_Anim.Tick(dt);
    ApplyCurrentFrame();
}

} // namespace acs::game
