// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_RENDER_VOLUMETRIC_CLOUD_TEMPORAL_INTERNAL_H
#define ACS_RENDER_VOLUMETRIC_CLOUD_TEMPORAL_INTERNAL_H

#include "render/Sky.h"

namespace acs::render_internal {

/** 対流位相差を時間再構成の現在値追従率へ変換する倍率。 */
inline constexpr f32 kCloudEvolutionResponseScale = 220.0f;

/** 対流位相差が時間再構成の現在値へ完全追従する境界。 */
inline constexpr f32 kCloudEvolutionFullResponseDelta = 1.0f / kCloudEvolutionResponseScale;

/** 自己影と立体物用雲影を一巡する部分更新の位相数。 */
inline constexpr u32 kCloudShadowTemporalPhaseCount = kVolumetricCloudShadowTemporalDivisor * kVolumetricCloudShadowTemporalDivisor;
static_assert(kCloudShadowTemporalPhaseCount == 4u);

/** 四つの偶奇位置をすべて生成済みであることを表すbit列。 */
inline constexpr u8 kCloudShadowTemporalCompleteMask =
    static_cast<u8>((1u << kCloudShadowTemporalPhaseCount) - 1u);

/**
 * 一回の影更新で完成した偶奇位置を記録する。
 *
 * @param current_mask すでに生成済みの偶奇位置。
 * @param phase 今回生成する偶奇位置。
 * @param full_refresh 今回だけで全位置を生成する場合はtrue。
 * @return 今回の更新を反映した生成済みbit列。
 */
inline u8 ResolveVolumetricCloudShadowWarmupMask_Internal(
    u8 current_mask, u32 phase, bool full_refresh) noexcept
{
    if (full_refresh) return kCloudShadowTemporalCompleteMask;
    const u32 safePhase = phase % kCloudShadowTemporalPhaseCount;
    return static_cast<u8>(
        current_mask | static_cast<u8>(1u << safePhase));
}

/** 4位相の影更新で、現在フレームが担当する位置と全更新の要否。 */
struct FVolumetricCloudShadowTemporalDecision {
    /** 00、10、01、11の順で巡回する現在の更新位相。 */
    u32 phase = 0u;
    /** 部分更新時に担当するX方向の偶奇位置。 */
    u32 partial_update_offset_x = 0u;
    /** 部分更新時に担当するY方向の偶奇位置。 */
    u32 partial_update_offset_y = 0u;
    /** 対流形状差が部分更新の成立範囲を超えた場合に真。 */
    bool self_shadow_requires_full_refresh = false;
    /** 対流形状差または固定地図上の移流差が成立範囲を超えた場合に真。 */
    bool world_shadow_requires_full_refresh = false;
};

/** 標準ライブラリへ依存せず、単精度値が有限かを判定する。 */
inline bool CloudTemporalValueIsFinite_Internal(f32 value) noexcept {
    constexpr f32 maximumFiniteValue = 3.402823466e+38F;
    return value == value && value >= -maximumFiniteValue && value <= maximumFiniteValue;
}

/** 有限な差分の絶対値を返し、非有限値は失敗としてfalseを返す。 */
inline bool ResolveCloudTemporalStepMagnitude_Internal(f32 value, f32& magnitude) noexcept {
    if (!CloudTemporalValueIsFinite_Internal(value)) {
        magnitude = 0.0f;
        return false;
    }
    magnitude = value < 0.0f ? -value : value;
    return true;
}

/**
 * 太陽方向の一段変化が雲層の水平投影を動かす最大距離を返す。
 *
 * @details 影の投影は方向差そのものではなく sun.xz / sun.y で決まる。
 * 地平線付近でY成分だけが僅かに変わる場合も、この比を直接比較して過小評価しない。
 * @param current 現在の正規化済み太陽方向。
 * @param previous 直前の正規化済み太陽方向。
 * @param vertical_span 影光路が跨ぐ最大高度差。
 * @param displacement 成功時に水平投影の最大移動距離を受け取る。
 * @return 有限で上向きの二方向から計算できた場合はtrue。
 */
inline bool ResolveVolumetricCloudSunProjectionDelta_Internal(
    FVec3 current, FVec3 previous, f32 vertical_span,
    f32& displacement) noexcept {
    displacement = 0.0f;
    if (!CloudTemporalValueIsFinite_Internal(vertical_span) ||
        vertical_span < 0.0f ||
        !CloudTemporalValueIsFinite_Internal(current.x) ||
        !CloudTemporalValueIsFinite_Internal(current.y) ||
        !CloudTemporalValueIsFinite_Internal(current.z) ||
        !CloudTemporalValueIsFinite_Internal(previous.x) ||
        !CloudTemporalValueIsFinite_Internal(previous.y) ||
        !CloudTemporalValueIsFinite_Internal(previous.z) ||
        current.y <= kVolumetricCloudWorldShadowMinimumSunY ||
        previous.y <= kVolumetricCloudWorldShadowMinimumSunY) {
        return false;
    }

    const f32 projectionDeltaX =
        current.x / current.y - previous.x / previous.y;
    const f32 projectionDeltaZ =
        current.z / current.y - previous.z / previous.y;
    const f32 projectionDeltaSquared =
        projectionDeltaX * projectionDeltaX +
        projectionDeltaZ * projectionDeltaZ;
    if (!CloudTemporalValueIsFinite_Internal(projectionDeltaSquared) ||
        projectionDeltaSquared < 0.0f) {
        return false;
    }
    displacement = vertical_span * Sqrt(projectionDeltaSquared);
    return CloudTemporalValueIsFinite_Internal(displacement);
}

/** 二つの対流状態に含まれる最大位相差を返し、非有限値ではfalseを返す。 */
inline bool ResolveCloudEvolutionDelta_Internal(const FVolumetricCloudEvolutionFrameTerms& current, const FVolumetricCloudEvolutionFrameTerms& previous, f32& maximum_delta) noexcept {
    const f32 signedDeltas[] = {current.shape_phase.x - previous.shape_phase.x, current.shape_phase.y - previous.shape_phase.y, current.fine_phase.x - previous.fine_phase.x, current.fine_phase.y - previous.fine_phase.y};
    maximum_delta = 0.0f;
    for (const f32 signedDelta : signedDeltas) {
        f32 magnitude = 0.0f;
        if (!ResolveCloudTemporalStepMagnitude_Internal(signedDelta, magnitude)) return false;
        if (magnitude > maximum_delta) maximum_delta = magnitude;
    }
    return true;
}

/**
 * 現在値で置換する4位相影更新の担当位置と、部分更新を続けられるかを返す。
 * 各部分更新フレームの差を許容差の3分の1未満へ制限するため、最大3フレーム古い
 * 画素までの実差分は三角不等式により許容差未満になる。
 *
 * @param frame_index 成功した雲描画を0から数える番号。
 * @param current_evolution 現在フレームの対流位相。
 * @param previous_evolution 直前フレームの対流位相。
 * @param current_world_advection 現在フレームの風移流距離。
 * @param previous_world_advection 直前フレームの風移流距離。
 * @return 非有限値では対応する影の全更新を要求する。
 */
inline FVolumetricCloudShadowTemporalDecision ResolveVolumetricCloudShadowTemporalDecision(u32 frame_index, const FVolumetricCloudEvolutionFrameTerms& current_evolution, const FVolumetricCloudEvolutionFrameTerms& previous_evolution, f32 current_world_advection, f32 previous_world_advection) noexcept {
    FVolumetricCloudShadowTemporalDecision out{};
    out.phase = frame_index % kCloudShadowTemporalPhaseCount;
    out.partial_update_offset_x = out.phase & 1u;
    out.partial_update_offset_y = (out.phase >> 1u) & 1u;
    f32 evolutionStepMagnitude = 0.0f;
    f32 worldAdvectionStepMagnitude = 0.0f;
    const bool evolutionStepIsFinite = ResolveCloudEvolutionDelta_Internal(current_evolution, previous_evolution, evolutionStepMagnitude);
    const bool worldAdvectionStepIsFinite = ResolveCloudTemporalStepMagnitude_Internal(current_world_advection - previous_world_advection, worldAdvectionStepMagnitude);
    const f32 maximumPartialUpdateAge = static_cast<f32>(kCloudShadowTemporalPhaseCount - 1u);
    out.self_shadow_requires_full_refresh = !evolutionStepIsFinite || evolutionStepMagnitude >= kCloudEvolutionFullResponseDelta / maximumPartialUpdateAge;
    out.world_shadow_requires_full_refresh = out.self_shadow_requires_full_refresh || !worldAdvectionStepIsFinite || worldAdvectionStepMagnitude >= kVolumetricCloudWorldShadowMapTexelSize / maximumPartialUpdateAge;
    return out;
}

} // namespace acs::render_internal

#endif // ACS_RENDER_VOLUMETRIC_CLOUD_TEMPORAL_INTERNAL_H
