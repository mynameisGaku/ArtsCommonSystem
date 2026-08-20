// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Sprite3DComponent.h"

#include "foundation/Move.h"

#include <cmath>

namespace acs::game {

namespace {

/** 3D座標がすべて有限ならtrueを返す。 */
bool IsFiniteVector(FVec3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

} // namespace

FStringView ASprite3DComponent::TexturePath() const noexcept
{
    return m_TexturePath.View();
}

void ASprite3DComponent::SetTexturePath(FStringView path) noexcept
{
    m_TexturePath = FString(path);
}

void ASprite3DComponent::SetImageAsset(TSharedPtr<AAsset> image) noexcept
{
    m_ImageAsset = Move(image);
}

const TSharedPtr<AAsset>& ASprite3DComponent::ImageAsset() const noexcept
{
    return m_ImageAsset;
}

bool ASprite3DComponent::HasImageAsset() const noexcept
{
    return static_cast<bool>(m_ImageAsset);
}

AImageAsset* ASprite3DComponent::Image() const noexcept
{
    return static_cast<AImageAsset*>(m_ImageAsset.Get());
}

void ASprite3DComponent::LocalBounds(FVec3& minimum, FVec3& maximum) const noexcept
{
    minimum = FVec3{-0.5f, -0.5f, 0.0f};
    maximum = FVec3{0.5f, 0.5f, 0.0f};
}

FRayHit3 ASprite3DComponent::RaycastLocalGeometry(const FRay3& ray, f32 maximum_t) const noexcept
{
    if (!IsFiniteVector(ray.origin) || !IsFiniteVector(ray.direction) || !std::isfinite(maximum_t) || maximum_t < 0.0f) return FRayHit3{};
    const FRayHit3 hit = RaycastPlane(ray, FPlane::FromPointNormal(FVec3{0.0f, 0.0f, 0.0f}, FVec3{0.0f, 0.0f, 1.0f}), maximum_t);
    if (!hit.hit || !std::isfinite(hit.t) || !IsFiniteVector(hit.point) || Abs(hit.point.x) > 0.5f || Abs(hit.point.y) > 0.5f) return FRayHit3{};
    return hit;
}

} // namespace acs::game
