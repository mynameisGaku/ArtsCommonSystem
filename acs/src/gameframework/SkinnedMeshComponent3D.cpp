// SPDX-License-Identifier: Apache-2.0
// ASkinnedMeshComponent3D 実装
#include "gameframework/SkinnedMeshComponent3D.h"

#include "foundation/Move.h"

namespace acs::game {

void ASkinnedMeshComponent3D::SetMeshAsset(TSharedPtr<ASkinnedMeshAsset> asset) noexcept {
    m_Mesh = Move(asset);
    // プレイヤは生ポインタで参照する。差し替えたら必ず教える (古い骨を指したままにしない)。
    m_Player.SetMesh(m_Mesh.Get());
}

bool ASkinnedMeshComponent3D::IsRenderable() const noexcept {
    if (!m_Mesh) return false;
    if (m_Mesh->Vertices().IsEmpty() || m_Mesh->Indices().IsEmpty()) return false;
    return !m_Mesh->Bones().IsEmpty();
}

void ASkinnedMeshComponent3D::Play(u32 index, bool loop) noexcept {
    m_Player.Play(index, loop);
}

bool ASkinnedMeshComponent3D::BlendTo(u32 index, f32 blend_seconds, bool loop) noexcept {
    return m_Player.BlendTo(index, blend_seconds, loop);
}

bool ASkinnedMeshComponent3D::FindAnimationByName_Internal(FStringView name, u32& out_index) const noexcept
{
    if (!m_Mesh || name.Size() == 0u) return false;

    const TArray<FAnimation>& animations = m_Mesh->Animations();
    for (usize index = 0u; index < animations.Num(); ++index) {
        const FStringView candidate = animations[index].name.View();
        if (candidate.Size() != name.Size()) continue;

        bool same = true;
        for (usize i = 0u; i < name.Size() && same; ++i) {
            if (candidate[i] != name[i]) same = false;
        }
        if (!same) continue;

        out_index = static_cast<u32>(index);
        return true;
    }
    return false;
}

bool ASkinnedMeshComponent3D::PlayByName(FStringView name, bool loop) noexcept {
    u32 index = 0u;
    if (!FindAnimationByName_Internal(name, index)) return false;
    m_Player.Play(index, loop);
    return true;
}

bool ASkinnedMeshComponent3D::BlendToByName(FStringView name, f32 blend_seconds, bool loop) noexcept
{
    u32 index = 0u;
    if (!FindAnimationByName_Internal(name, index)) return false;
    return m_Player.BlendTo(index, blend_seconds, loop);
}

void ASkinnedMeshComponent3D::Pause() noexcept {
    m_Player.Pause();
}

void ASkinnedMeshComponent3D::OnUpdate(f32 dt) noexcept {
    if (!m_Advancing) return;
    m_Player.Update(dt);
}

} // namespace acs::game
