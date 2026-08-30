// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_RENDER_VOLUMETRIC_CLOUD_AMBIENT_CACHE_INTERNAL_H
#define ACS_RENDER_VOLUMETRIC_CLOUD_AMBIENT_CACHE_INTERNAL_H

#include "render/Sky.h"
#include "render/VolumetricCloudDensityIntegrationInternal.h"

namespace acs::render_internal {

/** 中央の等間隔領域と外周の二次拡張を表す環境光キャッシュ写像。 */
struct FVolumetricCloudAmbientCacheMapTerms {
    /** キャッシュの物理半幅。 */
    f32 half_extent = 0.0f;
    /** 500 m間隔を保つ中心半径。 */
    f32 uniform_radius = 0.0f;
    /** テクスチャ半幅に対する中心半径の割合。 */
    f32 central_fraction = 0.0f;
    /** 外周が占める正規化幅。 */
    f32 outer_texture_span = 0.0f;
    /** 外周の元の物理幅。 */
    f32 outer_world_span = 0.0f;
    /** 最大描画距離を完全信頼境界へ入れる二次係数。 */
    f32 guard_coefficient = 0.0f;
    /** 完全信頼境界が中心から覆う物理距離。 */
    f32 guarded_distance = 0.0f;
};

/** 最大描画距離から、環境光キャッシュの可逆な水平写像を求める。 */
inline FVolumetricCloudAmbientCacheMapTerms ResolveVolumetricCloudAmbientCacheMapTerms_Internal(f32 maximum_view_distance) noexcept {
    FVolumetricCloudAmbientCacheMapTerms out{};
    f32 safeDistance = maximum_view_distance;
    if (!(safeDistance == safeDistance) || safeDistance < kVolumetricCloudMinDistance) safeDistance = kVolumetricCloudMinDistance;
    if (safeDistance > kVolumetricCloudMaxDistance) safeDistance = kVolumetricCloudMaxDistance;
    out.half_extent = kVolumetricCloudShadowCacheExtent * 0.5f;
    out.uniform_radius = kVolumetricCloudShadowCacheSafeRadius < out.half_extent ? kVolumetricCloudShadowCacheSafeRadius : out.half_extent;
    out.central_fraction = out.uniform_radius / out.half_extent;
    out.outer_texture_span = 1.0f - out.central_fraction;
    out.outer_world_span = out.half_extent - out.uniform_radius;
    const f32 fullWeightSignedAxis = 1.0f - 2.0f * kVolumetricCloudShadowCacheFilterFullCells / static_cast<f32>(kVolumetricCloudShadowCacheWidth);
    const f32 fullWeightOuterT = (fullWeightSignedAxis - out.central_fraction) / out.outer_texture_span;
    out.guarded_distance = safeDistance + out.uniform_radius;
    if (out.guarded_distance < out.half_extent) out.guarded_distance = out.half_extent;
    const f32 guardedDistanceRatio = (out.guarded_distance - out.uniform_radius) / out.outer_world_span;
    out.guard_coefficient = 2.0f * (guardedDistanceRatio - fullWeightOuterT) / (fullWeightOuterT * fullWeightOuterT);
    if (out.guard_coefficient < 0.0f) out.guard_coefficient = 0.0f;
    return out;
}

/** テクスチャの一軸から、キャッシュ中心を原点とした物質座標を求める。 */
inline f32 VolumetricCloudAmbientCacheMaterialOffset_Internal(const FVolumetricCloudAmbientCacheMapTerms& terms, f32 texture_axis) noexcept {
    const f32 signedAxis = texture_axis * 2.0f - 1.0f;
    const f32 absoluteAxis = signedAxis < 0.0f ? -signedAxis : signedAxis;
    if (absoluteAxis <= terms.central_fraction) return signedAxis * terms.half_extent;
    const f32 outerT = (absoluteAxis - terms.central_fraction) / terms.outer_texture_span;
    const f32 worldOffset = terms.uniform_radius + terms.outer_world_span * (outerT + 0.5f * terms.guard_coefficient * outerT * outerT);
    return signedAxis < 0.0f ? -worldOffset : worldOffset;
}

/** 中心からの物質座標を、数値安定な二次式の正根でテクスチャ座標へ戻す。 */
inline f32 VolumetricCloudAmbientCacheTextureAxis_Internal(const FVolumetricCloudAmbientCacheMapTerms& terms, f32 world_offset) noexcept {
    const f32 absoluteOffset = world_offset < 0.0f ? -world_offset : world_offset;
    if (absoluteOffset <= terms.uniform_radius) return 0.5f + 0.5f * world_offset / terms.half_extent;
    const f32 normalizedOffset = (absoluteOffset - terms.uniform_radius) / terms.outer_world_span;
    const f32 root = Sqrt(1.0f + 2.0f * terms.guard_coefficient * normalizedOffset);
    const f32 outerT = 2.0f * normalizedOffset / (root + 1.0f);
    const f32 absoluteAxis = terms.central_fraction + terms.outer_texture_span * outerT;
    const f32 signedAxis = world_offset < 0.0f ? -absoluteAxis : absoluteAxis;
    return 0.5f + 0.5f * signedAxis;
}

/** 一軸の画素中心で、非一様写像が覆う局所的な物理幅を求める。 */
inline f32 VolumetricCloudAmbientCacheCellWidth_Internal(const FVolumetricCloudAmbientCacheMapTerms& terms, f32 texture_axis) noexcept {
    const f32 signedAxis = texture_axis * 2.0f - 1.0f;
    const f32 absoluteAxis = signedAxis < 0.0f ? -signedAxis : signedAxis;
    if (absoluteAxis <= terms.central_fraction) return kVolumetricCloudShadowCacheCellSize;
    const f32 outerT = (absoluteAxis - terms.central_fraction) / terms.outer_texture_span;
    return kVolumetricCloudShadowCacheCellSize * (1.0f + terms.guard_coefficient * outerT);
}

/** 一画素を物理距離で四等分し、指定した中点標本の位置と担当幅を返す。 */
inline void ResolveVolumetricCloudAmbientCacheAxisSample_Internal(const FVolumetricCloudAmbientCacheMapTerms& terms, f32 texture_axis, u32 sample_index, f32& sample_offset, f32& sample_width) noexcept {
    const f32 halfTexel = 0.5f / static_cast<f32>(kVolumetricCloudShadowCacheWidth);
    const f32 lowerBoundary = VolumetricCloudAmbientCacheMaterialOffset_Internal(terms, texture_axis - halfTexel);
    const f32 upperBoundary = VolumetricCloudAmbientCacheMaterialOffset_Internal(terms, texture_axis + halfTexel);
    const u32 boundedIndex = sample_index < kVolumetricCloudAmbientCacheQuadratureAxis
        ? sample_index : kVolumetricCloudAmbientCacheQuadratureAxis - 1u;
    sample_width = (upperBoundary - lowerBoundary) /
        static_cast<f32>(kVolumetricCloudAmbientCacheQuadratureAxis);
    sample_offset = lowerBoundary +
        (static_cast<f32>(boundedIndex) + 0.5f) * sample_width;
}

/** 垂直光学的深さから、等方な半球放射が運ぶ照度の透過率を求める。 */
inline f32 ResolveVolumetricCloudHemisphericVisibility_Internal(f32 optical_depth) noexcept {
    if (!(optical_depth == optical_depth)) return 0.0f;
    if (optical_depth <= 0.0f) return 1.0f;
    if (optical_depth >= 80.0f) return 0.0f;
    constexpr f32 directionCosines[4] = {
        0.0694318442029737f,
        0.3300094782075719f,
        0.6699905217924281f,
        0.9305681557970262f,
    };
    constexpr f32 irradianceWeights[4] = {
        0.0241522034128332f,
        0.2152140822717850f,
        0.4369310725907611f,
        0.3237026417246206f,
    };
    f32 visibility = 0.0f;
    for (u32 sampleIndex = 0u; sampleIndex < 4u; ++sampleIndex) {
        visibility += irradianceWeights[sampleIndex] *
            Exp(-optical_depth / directionCosines[sampleIndex]);
    }
    if (visibility < 0.0f) return 0.0f;
    return visibility > 1.0f ? 1.0f : visibility;
}

/** 物理的な箱幅、対流勾配、高度せん断を形状の直交領域へ写し、最も広い一軸の幅を求める。 */
inline f32 ResolveVolumetricCloudShapeMaximumDomainFootprint_Internal(f32 footprint_x, f32 footprint_y, f32 footprint_z, f32 shape_scale, f32 inverse_layer_height, bool upper_band, f32 convection_gradient = 0.0f) noexcept {
    constexpr f32 maximumFiniteValue = 3.402823466e+38F;
    if (!CloudDensityIntegrationValueIsFinite_Internal(shape_scale))
        return maximumFiniteValue;
    if (!(shape_scale > 0.0f)) return 0.0f;
    if (!CloudDensityIntegrationValueIsFinite_Internal(footprint_x) ||
        !CloudDensityIntegrationValueIsFinite_Internal(footprint_y) ||
        !CloudDensityIntegrationValueIsFinite_Internal(footprint_z) ||
        !CloudDensityIntegrationValueIsFinite_Internal(inverse_layer_height) ||
        !CloudDensityIntegrationValueIsFinite_Internal(convection_gradient))
        return maximumFiniteValue;
    if (!(footprint_x > 0.0f)) footprint_x = 0.0f;
    if (!(footprint_y > 0.0f)) footprint_y = 0.0f;
    if (!(footprint_z > 0.0f)) footprint_z = 0.0f;
    if (!(inverse_layer_height > 0.0f)) inverse_layer_height = 0.0f;
    if (!(convection_gradient > 0.0f)) convection_gradient = 0.0f;
    const f32 altitudeWidth = Sqrt(
        footprint_x * footprint_x + footprint_y * footprint_y +
        footprint_z * footprint_z);
    // GPU側と同じく、対流変位の局所勾配だけを隣接点の担当幅へ加える。
    // 平行移動量を足さないことで、時間変位を誤ってLOD幅へ変換しない。
    footprint_x += convection_gradient * altitudeWidth;
    footprint_z += convection_gradient * altitudeWidth;
    if (!CloudDensityIntegrationValueIsFinite_Internal(footprint_x) ||
        !CloudDensityIntegrationValueIsFinite_Internal(footprint_z))
        return maximumFiniteValue;
    const f32 bandScale = upper_band ? 0.25f : 1.0f;
    const f32 shearScale = 850.0f * bandScale * inverse_layer_height;
    const f32 canonicalX =
        (footprint_x + 0.9284767f * shearScale * altitudeWidth) *
        shape_scale;
    const f32 canonicalY = altitudeWidth * shape_scale;
    const f32 canonicalZ =
        (footprint_z + 0.3713907f * shearScale * altitudeWidth) *
        shape_scale;
    if (!CloudDensityIntegrationValueIsFinite_Internal(canonicalX) ||
        !CloudDensityIntegrationValueIsFinite_Internal(canonicalY) ||
        !CloudDensityIntegrationValueIsFinite_Internal(canonicalZ))
        return maximumFiniteValue;
    const f32 rotatedX = 0.8f * canonicalY + 0.6f * canonicalZ;
    const f32 rotatedY = 0.7071068f * canonicalX +
        0.4242641f * canonicalY + 0.5656854f * canonicalZ;
    return rotatedX > rotatedY ? rotatedX : rotatedY;
}

/** 0から1の可視率を、共有メモリで加算できる16bit精度の整数へ変換する。 */
inline u32 QuantizeVolumetricCloudAmbientVisibility_Internal(f32 visibility) noexcept {
    if (!(visibility == visibility) || visibility <= 0.0f) return 0u;
    if (visibility >= 1.0f) return 65535u;
    return static_cast<u32>(Round(visibility * 65535.0f));
}

} // namespace acs::render_internal

#endif // ACS_RENDER_VOLUMETRIC_CLOUD_AMBIENT_CACHE_INTERNAL_H
