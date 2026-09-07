// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_RENDER_VOLUMETRIC_CLOUD_AMBIENT_CACHE_INTERNAL_H
#define ACS_RENDER_VOLUMETRIC_CLOUD_AMBIENT_CACHE_INTERNAL_H

#include "render/Sky.h"
#include "render/VolumetricCloudDensityIntegrationInternal.h"
#include "render/VolumetricCloudRayMarchInternal.h"

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

/** 公開または生成中の中心が、現在地点を完全信頼領域へ含むかを判定する。 */
inline bool VolumetricCloudAmbientCacheCenterSupportsMaterialPoint_Internal(
    FVec2 center_material_xz, FVec2 point_material_xz) noexcept {
    if (!CloudDensityIntegrationValueIsFinite_Internal(
            center_material_xz.x) ||
        !CloudDensityIntegrationValueIsFinite_Internal(
            center_material_xz.y) ||
        !CloudDensityIntegrationValueIsFinite_Internal(
            point_material_xz.x) ||
        !CloudDensityIntegrationValueIsFinite_Internal(
            point_material_xz.y))
        return false;
    f32 offsetX = point_material_xz.x - center_material_xz.x;
    f32 offsetZ = point_material_xz.y - center_material_xz.y;
    if (offsetX < 0.0f) offsetX = -offsetX;
    if (offsetZ < 0.0f) offsetZ = -offsetZ;
    return offsetX <= kVolumetricCloudShadowCacheSafeRadius &&
        offsetZ <= kVolumetricCloudShadowCacheSafeRadius;
}

/** 自己影を生成した媒質と、現在の媒質との互換性判定。 */
struct FVolumetricCloudAmbientCacheMediumDecisionInternal {
    /** 設定世代、雲量、密度が同じ場合はtrue。 */
    bool fixed_terms_match = false;
    /** 対流位相から求めた変位上限が有限な場合はtrue。 */
    bool evolution_displacement_resolved = false;
    /** 対流位相差が雲形状を動かし得る物理距離の上限。 */
    f32 maximum_evolution_displacement = 0.0f;
    /** 完成キャッシュを現在の雲へ採取できる場合はtrue。 */
    bool compatible = false;
};

/**
 * 自己影の生成世代を現在の媒質と比較する。
 *
 * 雲量と密度は光学的深さを直接変えるため完全一致を要求する。対流位相は、
 * 低周波形状の層厚変位と最も広い詳細領域の物理移動を上から押さえ、最小セルの
 * 半幅を越えた場合だけ不一致とする。非有限値は採取可能と推測せず失敗へ閉じる。
 */
inline FVolumetricCloudAmbientCacheMediumDecisionInternal
ResolveVolumetricCloudAmbientCacheMediumDecision_Internal(
    bool cached_initialized, u64 cached_content_revision,
    f32 cached_coverage, f32 cached_density,
    const FVolumetricCloudEvolutionFrameTerms& cached_evolution,
    u64 current_content_revision, f32 current_coverage,
    f32 current_density,
    const FVolumetricCloudEvolutionFrameTerms& current_evolution,
    f32 maximum_layer_span) noexcept {
    FVolumetricCloudAmbientCacheMediumDecisionInternal out{};
    const bool finiteFixedTerms =
        CloudDensityIntegrationValueIsFinite_Internal(cached_coverage) &&
        CloudDensityIntegrationValueIsFinite_Internal(cached_density) &&
        CloudDensityIntegrationValueIsFinite_Internal(current_coverage) &&
        CloudDensityIntegrationValueIsFinite_Internal(current_density);
    out.fixed_terms_match = cached_initialized && finiteFixedTerms &&
        cached_content_revision == current_content_revision &&
        cached_coverage == current_coverage &&
        cached_density == current_density;

    const f32 signedDeltas[4] = {
        current_evolution.shape_phase.x - cached_evolution.shape_phase.x,
        current_evolution.shape_phase.y - cached_evolution.shape_phase.y,
        current_evolution.fine_phase.x - cached_evolution.fine_phase.x,
        current_evolution.fine_phase.y - cached_evolution.fine_phase.y};
    f32 phaseDeltas[4]{};
    bool finiteEvolution =
        CloudDensityIntegrationValueIsFinite_Internal(maximum_layer_span) &&
        maximum_layer_span >= 0.0f;
    for (u32 index = 0u; index < 4u; ++index) {
        finiteEvolution = finiteEvolution &&
            CloudDensityIntegrationValueIsFinite_Internal(
                signedDeltas[index]);
        phaseDeltas[index] = signedDeltas[index] < 0.0f
            ? -signedDeltas[index] : signedDeltas[index];
    }
    if (finiteEvolution) {
        // 二つの局所形状成分は位相差のL1上限を持ち、直交二軸への変換で
        // 長さが最大sqrt(2)倍になる。詳細領域の最小倍率0.00018は、同じ
        // 位相差を最も大きなワールド移動へ戻す保守的な尺度である。
        constexpr f32 squareRootTwo = 1.4142135623730951f;
        constexpr f32 minimumDetailDomainScale = 0.00018f;
        const f32 shapeDisplacement = maximum_layer_span * squareRootTwo *
            (phaseDeltas[0] + phaseDeltas[1]);
        const f32 detailDisplacement = squareRootTwo *
            (phaseDeltas[2] + phaseDeltas[3]) /
            minimumDetailDomainScale;
        out.maximum_evolution_displacement =
            shapeDisplacement + detailDisplacement;
        out.evolution_displacement_resolved =
            CloudDensityIntegrationValueIsFinite_Internal(
                out.maximum_evolution_displacement);
    }
    const f32 maximumSupportedDisplacement =
        kVolumetricCloudShadowCacheCellSize * 0.5f;
    out.compatible = out.fixed_terms_match &&
        out.evolution_displacement_resolved &&
        out.maximum_evolution_displacement < maximumSupportedDisplacement;
    return out;
}

/** 自己影キャッシュを現在フレームで再生成するかを表す。 */
struct FVolumetricCloudAmbientCacheRefreshDecisionInternal {
    /** 密度、方向別光路、半球解決の三処理を実行する場合はtrue。 */
    bool rebuild = false;
};

/** 影世代の曲面原点をGPUへ渡せる有限値として検査する。 */
inline bool VolumetricCloudAmbientCacheWorldOriginIsFinite_Internal(
    FVec3 world_origin) noexcept {
    return CloudDensityIntegrationValueIsFinite_Internal(world_origin.x) &&
        CloudDensityIntegrationValueIsFinite_Internal(world_origin.y) &&
        CloudDensityIntegrationValueIsFinite_Internal(world_origin.z);
}

/** 表示中の影世代と現在密度が共有する曲面原点の選択結果。 */
struct FVolumetricCloudAmbientCacheWorldOriginDecisionInternal {
    /** 密度、影、画面履歴が共通に使う曲面原点。 */
    FVec3 world_origin{};
    /** どちらかの有限な原点を選べた場合はtrue。 */
    bool valid = false;
    /** 公開済み影世代の原点を選んだ場合はtrue。 */
    bool uses_published_generation = false;
};

/** 公開原点と再基準化原点を検査し、共有できる曲面原点を選ぶ。 */
inline FVolumetricCloudAmbientCacheWorldOriginDecisionInternal
ResolveVolumetricCloudAmbientCacheWorldOrigin_Internal(
    bool published_generation_valid, bool reference_mode,
    FVec3 published_world_origin, FVec3 rebased_world_origin) noexcept {
    FVolumetricCloudAmbientCacheWorldOriginDecisionInternal out{};
    const bool publishedOriginFinite =
        VolumetricCloudAmbientCacheWorldOriginIsFinite_Internal(
            published_world_origin);
    if (published_generation_valid && !reference_mode &&
        publishedOriginFinite) {
        out.world_origin = published_world_origin;
        out.valid = true;
        out.uses_published_generation = true;
        return out;
    }
    if (VolumetricCloudAmbientCacheWorldOriginIsFinite_Internal(
            rebased_world_origin)) {
        out.world_origin = rebased_world_origin;
        out.valid = true;
    }
    return out;
}

/** 地平線付近で直接太陽光キャッシュだけを止める判断結果。 */
struct FVolumetricCloudAmbientCacheSunDecisionInternal {
    /** 現在の太陽方向で直接光キャッシュを採取できる場合はtrue。 */
    bool direct_sampling_supported = false;
    /** 表示中世代を太陽方向のために再生成する必要がある場合はtrue。 */
    bool published_generation_requires_refresh = false;
    /** 完成候補が周囲光を含む表示世代として利用できる場合はtrue。 */
    bool completed_generation_supports_current_sun = false;
};

/**
 * 太陽方向の水平投影が成立しない場合も、方向別周囲光の完成世代は保持する。
 * 直接光を再び採取できる高度へ戻った時だけ、古い太陽方向との不一致を再生成する。
 */
inline FVolumetricCloudAmbientCacheSunDecisionInternal
ResolveVolumetricCloudAmbientCacheSunDecision_Internal(
    bool published_generation_valid, bool direct_sampling_supported,
    bool projection_resolved, f32 projection_distance,
    f32 maximum_projection_distance) noexcept {
    FVolumetricCloudAmbientCacheSunDecisionInternal out{};
    const bool finiteDistance =
        CloudDensityIntegrationValueIsFinite_Internal(projection_distance) &&
        CloudDensityIntegrationValueIsFinite_Internal(
            maximum_projection_distance) &&
        projection_distance >= 0.0f && maximum_projection_distance > 0.0f;
    const bool projectionWithinCache = projection_resolved &&
        finiteDistance && projection_distance < maximum_projection_distance;
    out.direct_sampling_supported = direct_sampling_supported;
    out.published_generation_requires_refresh =
        published_generation_valid && direct_sampling_supported &&
        !projectionWithinCache;
    out.completed_generation_supports_current_sun =
        !direct_sampling_supported || projectionWithinCache;
    return out;
}

/** 4x4時間位相を一巡して、全画素を自身の新世代標本へ移すフレーム数。 */
inline constexpr u32 kVolumetricCloudAmbientCacheTransitionFrameCount = 16u;

/** 新しい影世代を画面履歴へ混ぜる、提出前後の状態。 */
struct FVolumetricCloudAmbientCacheTransitionDecisionInternal {
    /** 今回の時間解決で世代遷移を行う場合はtrue。 */
    bool active = false;
    /** 今回のGPU提出が成功した後に残る遷移フレーム数。 */
    u32 frames_remaining_after_submit = 0u;
};

/**
 * 影世代交換後の16位相を数え、各画素自身の等倍標本が一度更新される期間を示す。
 * 画素と視線の対応が変わらない場合だけ、未採取画素の正確な履歴を保持する。
 * カメラまたは射影が変わった場合は遷移を中止し、通常の再投影へ戻す。
 */
inline FVolumetricCloudAmbientCacheTransitionDecisionInternal
ResolveVolumetricCloudAmbientCacheTransitionDecision_Internal(
    u32 previous_frames_remaining, bool replace_generation,
    bool history_pixel_mapping_unchanged,
    bool phase_scheduled_output) noexcept {
    FVolumetricCloudAmbientCacheTransitionDecisionInternal out{};
    if (!history_pixel_mapping_unchanged || !phase_scheduled_output)
        return out;
    u32 framesRemaining = replace_generation
        ? kVolumetricCloudAmbientCacheTransitionFrameCount
        : previous_frames_remaining;
    if (framesRemaining > kVolumetricCloudAmbientCacheTransitionFrameCount) {
        framesRemaining = kVolumetricCloudAmbientCacheTransitionFrameCount;
    }
    if (framesRemaining == 0u) return out;
    out.active = true;
    out.frames_remaining_after_submit = framesRemaining - 1u;
    return out;
}

/** 通常の照明差と4x4影世代遷移を、一つの有限なGPU入力へ符号化する。 */
inline f32 EncodeVolumetricCloudAmbientCacheTransitionMismatch_Internal(
    bool transition_active, f32 lighting_mismatch) noexcept {
    f32 mismatch = CloudDensityIntegrationValueIsFinite_Internal(
            lighting_mismatch)
        ? lighting_mismatch : 1.0f;
    if (mismatch < 0.0f) mismatch = 0.0f;
    if (mismatch > 1.0f) mismatch = 1.0f;
    // -1自体をゼロ照明差の有効な遷移状態として予約する。
    return transition_active ? -1.0f - mismatch : mismatch;
}

/**
 * 完成世代を保持できる通常フレームでは再生成せず、無効化済みまたは
 * 参照描画の場合だけ三処理を実行する。必要資源が無い場合は失敗へ閉じる。
 */
inline FVolumetricCloudAmbientCacheRefreshDecisionInternal
ResolveVolumetricCloudAmbientCacheRefreshDecision_Internal(
    bool resources_ready, bool density_fields_ready,
    bool reference_mode, bool published_generation_valid) noexcept {
    FVolumetricCloudAmbientCacheRefreshDecisionInternal out{};
    out.rebuild = resources_ready && density_fields_ready &&
        (reference_mode || !published_generation_valid);
    return out;
}

/** 提出済み命令から、画面履歴と影生成を個別に確定できるかを表す。 */
struct FVolumetricCloudSubmissionCompatibilityDecisionInternal {
    /** 記録時と現在の影内容世代が同じ場合はtrue。 */
    bool commit_shadow_state = false;
    /** 記録時と現在の画面履歴世代が同じ場合はtrue。 */
    bool commit_history_state = false;
};

/** 画面だけの設定変更が、無関係な影生成を巻き戻さない提出判断を返す。 */
inline FVolumetricCloudSubmissionCompatibilityDecisionInternal
ResolveVolumetricCloudSubmissionCompatibilityDecision_Internal(
    u64 recorded_history_revision, u64 current_history_revision,
    u64 recorded_shadow_revision, u64 current_shadow_revision) noexcept {
    FVolumetricCloudSubmissionCompatibilityDecisionInternal out{};
    out.commit_shadow_state =
        recorded_shadow_revision == current_shadow_revision;
    out.commit_history_state =
        recorded_history_revision == current_history_revision;
    return out;
}

/** 表示中の世代と、完成提出後に公開する世代の有効性を分けた判断結果。 */
struct FVolumetricCloudAmbientCachePublicationDecisionInternal {
    /** 現在記録している命令一覧で、既に公開済みの表示テクスチャを読むか。 */
    bool sample_published_this_frame = false;
    /** 同じ命令一覧で完成させる生成用テクスチャを、後続の雲描画で読むか。 */
    bool sample_completed_this_frame = false;
    /** 今回の提出が成功した後、新しい表示テクスチャを有効とするか。 */
    bool valid_after_submit = false;
};

/**
 * 完成交換の有無を考慮し、同一提出で読む世代と提出後世代を独立に決める。
 *
 * 完成世代は生成、方向別光路、半球解決を同じ命令一覧で終えた場合だけ読む。
 * 提出に失敗した場合は画面も公開状態も確定しないため、未完成世代が次回へ漏れない。
 */
inline FVolumetricCloudAmbientCachePublicationDecisionInternal
ResolveVolumetricCloudAmbientCachePublicationDecision_Internal(
    bool published_valid, bool replace_with_completed_generation,
    bool completed_generation_usable) noexcept {
    FVolumetricCloudAmbientCachePublicationDecisionInternal out{};
    out.sample_completed_this_frame =
        replace_with_completed_generation && completed_generation_usable;
    out.sample_published_this_frame =
        published_valid && !out.sample_completed_this_frame;
    out.valid_after_submit = replace_with_completed_generation
        ? completed_generation_usable : published_valid;
    return out;
}

/** 最大描画距離と方向別光路の横移動から、環境光キャッシュの可逆な水平写像を求める。 */
inline FVolumetricCloudAmbientCacheMapTerms ResolveVolumetricCloudAmbientCacheMapTerms_Internal(
    f32 maximum_view_distance,
    f32 maximum_directional_horizontal_travel = 0.0f) noexcept {
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
    f32 safeDirectionalTravel = maximum_directional_horizontal_travel;
    if (!CloudDensityIntegrationValueIsFinite_Internal(safeDirectionalTravel) ||
        safeDirectionalTravel < 0.0f)
        safeDirectionalTravel = 0.0f;
    out.guarded_distance = safeDistance + out.uniform_radius +
        safeDirectionalTravel;
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
    if (!CloudDensityIntegrationValueIsFinite_Internal(optical_depth)) return 0.0f;
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

/** 半球照度の4方向へ対応する、相関輸送後の透過率。 */
struct FVolumetricCloudAmbientDirectionTransmittancesInternal {
    /** 天頂角の余弦が小さい順に並ぶ方向別透過率。 */
    f32 values[4]{1.0f, 1.0f, 1.0f, 1.0f};
};

/** 4点Gauss-Legendre半球積分の指定方向が持つ天頂角余弦を返す。 */
inline f32 VolumetricCloudAmbientDirectionCosine_Internal(u32 direction_index) noexcept {
    constexpr f32 directionCosines[4] = {
        0.0694318442029737f,
        0.3300094782075719f,
        0.6699905217924281f,
        0.9305681557970262f,
    };
    const u32 boundedIndex = direction_index < 4u ? direction_index : 3u;
    return directionCosines[boundedIndex];
}

/** 4点Gauss-Legendre半球積分の指定方向が照度へ占める重みを返す。 */
inline f32 VolumetricCloudAmbientIrradianceWeight_Internal(u32 direction_index) noexcept {
    constexpr f32 irradianceWeights[4] = {
        0.0241522034128332f,
        0.2152140822717850f,
        0.4369310725907611f,
        0.3237026417246206f,
    };
    const u32 boundedIndex = direction_index < 4u ? direction_index : 3u;
    return irradianceWeights[boundedIndex];
}

/**
 * 最低仰角の方向が完成密度を読む最大水平距離を、雲帯の物理高度差から保守的に求める。
 * 水平成分そのものではなく全光路長で上から押さえ、局所鉛直が傾く曲面上でも不足させない。
 * 上層が有効な場合は、下層底から上層頂までを通る光路も密度支持域へ含める。
 */
inline f32 ResolveVolumetricCloudAmbientMaximumHorizontalTravel_Internal(
    const FVolumetricCloudLayer& lower_layer,
    const FVolumetricCloudUpperLayer& upper_layer) noexcept {
    f32 minimumAltitude = lower_layer.base_height;
    f32 maximumAltitude = lower_layer.top_height;
    if (!CloudDensityIntegrationValueIsFinite_Internal(minimumAltitude) ||
        !CloudDensityIntegrationValueIsFinite_Internal(maximumAltitude) ||
        maximumAltitude <= minimumAltitude)
        return 0.0f;
    if (CloudDensityIntegrationValueIsFinite_Internal(
            upper_layer.base_height) &&
        CloudDensityIntegrationValueIsFinite_Internal(
            upper_layer.top_height) &&
        upper_layer.base_height >= maximumAltitude &&
        upper_layer.top_height > upper_layer.base_height)
        maximumAltitude = upper_layer.top_height;
    const f32 verticalDistance = maximumAltitude - minimumAltitude;
    const f32 directionCosine =
        VolumetricCloudAmbientDirectionCosine_Internal(0u);
    const f32 horizontalTravel = verticalDistance / directionCosine;
    if (!CloudDensityIntegrationValueIsFinite_Internal(horizontalTravel) ||
        horizontalTravel < 0.0f)
        return kVolumetricCloudMaxDistance;
    return horizontalTravel;
}

/** 半球の天頂角4点と方位角4点を組み合わせた、一つの実3D積分方向。 */
struct FVolumetricCloudAmbientDirectionInternal {
    /** 雲帯の底から空へ向く正規化済み方向。 */
    FVec3 direction{0.0f, 1.0f, 0.0f};

    /** 等方放射輝度が水平面へ運ぶ照度に対する積分重み。 */
    f32 irradiance_weight = 0.0f;

    /** 天頂角のGauss-Legendre点番号。 */
    u32 zenith_index = 0u;

    /** 同じ天頂角を周回する方位角番号。 */
    u32 azimuth_index = 0u;
};

/**
 * 4天頂角と4方位角を組み合わせ、方向別キャッシュと最終解決で共有する
 * 正規化方向と照度重みを返す。
 */
inline FVolumetricCloudAmbientDirectionInternal
ResolveVolumetricCloudAmbientDirection_Internal(u32 direction_index) noexcept {
    const u32 boundedIndex = direction_index <
        kVolumetricCloudAmbientDirectionCount
        ? direction_index : kVolumetricCloudAmbientDirectionCount - 1u;
    FVolumetricCloudAmbientDirectionInternal out{};
    out.zenith_index = boundedIndex %
        kVolumetricCloudAmbientZenithDirectionCount;
    out.azimuth_index = boundedIndex /
        kVolumetricCloudAmbientZenithDirectionCount;
    const f32 directionCosine =
        VolumetricCloudAmbientDirectionCosine_Internal(out.zenith_index);
    const f32 horizontalLength = Sqrt(
        1.0f - directionCosine * directionCosine);
    constexpr f32 azimuthStep = 1.5707963267948966f;
    constexpr f32 zenithRingRotation = 0.3926990816987242f;
    const f32 azimuth =
        static_cast<f32>(out.azimuth_index) * azimuthStep +
        static_cast<f32>(out.zenith_index) * zenithRingRotation;
    out.direction = FVec3{
        horizontalLength * Cos(azimuth),
        directionCosine,
        horizontalLength * Sin(azimuth)};
    out.irradiance_weight =
        VolumetricCloudAmbientIrradianceWeight_Internal(out.zenith_index) /
        static_cast<f32>(kVolumetricCloudAmbientAzimuthDirectionCount);
    return out;
}

/**
 * 曲面惑星上の指定点で局所半径方向を鉛直とし、固定方位を接平面へ射影して求積方向を作る。
 * 入力点が不正な場合は原点の接平面方向を返し、非有限値を後段へ伝播させない。
 */
inline FVolumetricCloudAmbientDirectionInternal
ResolveVolumetricCloudAmbientDirectionAtPoint_Internal(
    u32 direction_index, FVec3 point, FVec3 world_origin) noexcept {
    FVolumetricCloudAmbientDirectionInternal out =
        ResolveVolumetricCloudAmbientDirection_Internal(direction_index);
    const FVec3 radial{
        point.x - world_origin.x,
        kVolumetricCloudPlanetRadius + point.y - world_origin.y,
        point.z - world_origin.z};
    const f32 radialLengthSquared = radial.x * radial.x +
        radial.y * radial.y + radial.z * radial.z;
    if (!CloudDensityIntegrationValueIsFinite_Internal(radial.x) ||
        !CloudDensityIntegrationValueIsFinite_Internal(radial.y) ||
        !CloudDensityIntegrationValueIsFinite_Internal(radial.z) ||
        !CloudDensityIntegrationValueIsFinite_Internal(radialLengthSquared) ||
        radialLengthSquared <= 1.0f)
        return out;
    const f32 inverseRadialLength = 1.0f / Sqrt(radialLengthSquared);
    const FVec3 localUp{
        radial.x * inverseRadialLength,
        radial.y * inverseRadialLength,
        radial.z * inverseRadialLength};
    const FVec3 tangentDirection = out.direction;
    const f32 horizontalLength = Sqrt(
        tangentDirection.x * tangentDirection.x +
        tangentDirection.z * tangentDirection.z);
    if (!CloudDensityIntegrationValueIsFinite_Internal(horizontalLength) ||
        horizontalLength <= 1.0e-4f)
        return out;
    const FVec3 azimuthReference{
        tangentDirection.x / horizontalLength,
        0.0f,
        tangentDirection.z / horizontalLength};
    const f32 azimuthRadialDot =
        azimuthReference.x * localUp.x +
        azimuthReference.z * localUp.z;
    const FVec3 projectedAzimuth{
        azimuthReference.x - azimuthRadialDot * localUp.x,
        -azimuthRadialDot * localUp.y,
        azimuthReference.z - azimuthRadialDot * localUp.z};
    const f32 projectedLengthSquared =
        projectedAzimuth.x * projectedAzimuth.x +
        projectedAzimuth.y * projectedAzimuth.y +
        projectedAzimuth.z * projectedAzimuth.z;
    if (!CloudDensityIntegrationValueIsFinite_Internal(
            projectedLengthSquared) ||
        projectedLengthSquared <= 1.0e-6f)
        return out;
    const f32 inverseProjectedLength =
        1.0f / Sqrt(projectedLengthSquared);
    out.direction = FVec3{
        tangentDirection.y * localUp.x +
            horizontalLength * projectedAzimuth.x *
                inverseProjectedLength,
        tangentDirection.y * localUp.y +
            horizontalLength * projectedAzimuth.y *
                inverseProjectedLength,
        tangentDirection.y * localUp.z +
            horizontalLength * projectedAzimuth.z *
                inverseProjectedLength};
    return out;
}

/**
 * 固定方位を接平面へ射影した直線光路について、雲内の点から底面アンカーを解析的に戻す。
 * 反復近似を使わないため、厚い層でも生成側と同じ底面列を決定できる。
 */
inline bool ResolveVolumetricCloudAmbientDirectionalBandBaseAnchor_Internal(
    FVec3 point, u32 direction_index, f32 base_altitude,
    f32 target_altitude,
    FVec3 world_origin, FVec3& base_point) noexcept {
    base_point = point;
    if (!CloudDensityIntegrationValueIsFinite_Internal(point.x) ||
        !CloudDensityIntegrationValueIsFinite_Internal(point.y) ||
        !CloudDensityIntegrationValueIsFinite_Internal(point.z) ||
        !CloudDensityIntegrationValueIsFinite_Internal(base_altitude) ||
        !CloudDensityIntegrationValueIsFinite_Internal(target_altitude) ||
        target_altitude < base_altitude)
        return false;
    const auto directionSample =
        ResolveVolumetricCloudAmbientDirection_Internal(direction_index);
    const f32 directionCosine = directionSample.direction.y;
    const f32 horizontalLength = Sqrt(
        directionSample.direction.x * directionSample.direction.x +
        directionSample.direction.z * directionSample.direction.z);
    if (!(directionCosine > 0.0f) || !(horizontalLength > 0.0f))
        return false;
    const FVec3 azimuthReference{
        directionSample.direction.x / horizontalLength,
        0.0f,
        directionSample.direction.z / horizontalLength};
    const FVec3 centerOffset{
        point.x - world_origin.x,
        kVolumetricCloudPlanetRadius + point.y - world_origin.y,
        point.z - world_origin.z};
    const f32 baseRadius =
        kVolumetricCloudPlanetRadius + base_altitude;
    const f32 targetRadius =
        kVolumetricCloudPlanetRadius + target_altitude;
    const f32 baseRadiusSquared = baseRadius * baseRadius;
    const f32 targetRadiusSquared = targetRadius * targetRadius;
    const f32 radialDifference =
        (target_altitude - base_altitude) *
        (2.0f * kVolumetricCloudPlanetRadius +
         target_altitude + base_altitude);
    const f32 rootTerm = baseRadiusSquared *
        directionCosine * directionCosine +
        radialDifference;
    if (!CloudDensityIntegrationValueIsFinite_Internal(
            targetRadiusSquared) ||
        !CloudDensityIntegrationValueIsFinite_Internal(baseRadius) ||
        !CloudDensityIntegrationValueIsFinite_Internal(targetRadius) ||
        !CloudDensityIntegrationValueIsFinite_Internal(rootTerm) ||
        baseRadius <= 1.0f || targetRadius < baseRadius ||
        rootTerm < 0.0f)
        return false;
    const f32 rayDistance = -baseRadius * directionCosine +
        Sqrt(rootTerm);
    if (!CloudDensityIntegrationValueIsFinite_Internal(rayDistance) ||
        rayDistance < 0.0f)
        return false;
    const f32 radialAlongRay =
        baseRadius + rayDistance * directionCosine;
    const f32 tangentAlongRay = rayDistance * horizontalLength;
    const f32 azimuthCoordinate =
        centerOffset.x * azimuthReference.x +
        centerOffset.z * azimuthReference.z;
    const FVec3 perpendicular{
        centerOffset.x - azimuthCoordinate * azimuthReference.x,
        centerOffset.y,
        centerOffset.z - azimuthCoordinate * azimuthReference.z};
    const f32 perpendicularLengthSquared =
        perpendicular.x * perpendicular.x +
        perpendicular.y * perpendicular.y +
        perpendicular.z * perpendicular.z;
    if (!CloudDensityIntegrationValueIsFinite_Internal(
            perpendicularLengthSquared) ||
        perpendicularLengthSquared <= 1.0f ||
        targetRadiusSquared <= 1.0f)
        return false;
    const f32 perpendicularLength = Sqrt(perpendicularLengthSquared);
    f32 baseAzimuthCosine =
        (radialAlongRay * azimuthCoordinate -
         tangentAlongRay * perpendicularLength) /
        targetRadiusSquared;
    if (!CloudDensityIntegrationValueIsFinite_Internal(
            baseAzimuthCosine))
        return false;
    if (baseAzimuthCosine < -1.0f) baseAzimuthCosine = -1.0f;
    if (baseAzimuthCosine > 1.0f) baseAzimuthCosine = 1.0f;
    const f32 basePerpendicularSine = Sqrt(
        1.0f - baseAzimuthCosine * baseAzimuthCosine);
    const f32 perpendicularScale =
        basePerpendicularSine / perpendicularLength;
    const FVec3 baseNormal{
        baseAzimuthCosine * azimuthReference.x +
            perpendicularScale * perpendicular.x,
        perpendicularScale * perpendicular.y,
        baseAzimuthCosine * azimuthReference.z +
            perpendicularScale * perpendicular.z};
    base_point = FVec3{
        world_origin.x + baseRadius * baseNormal.x,
        world_origin.y - kVolumetricCloudPlanetRadius +
            baseRadius * baseNormal.y,
        world_origin.z + baseRadius * baseNormal.z};
    return CloudDensityIntegrationValueIsFinite_Internal(base_point.x) &&
        CloudDensityIntegrationValueIsFinite_Internal(base_point.y) &&
        CloudDensityIntegrationValueIsFinite_Internal(base_point.z);
}

/** 方向別高度キャッシュを端点で表したときの、重複しない物理区間数を返す。 */
inline u32 VolumetricCloudAmbientProfileIntervalCount_Internal(
    u32 profile_count) noexcept {
    return profile_count > 1u ? profile_count - 1u : 0u;
}

/** 一水平列の周囲光生成が完成密度を読む最大回数を返す。 */
inline u32 VolumetricCloudAmbientDensitySamplesPerColumn_Internal(
    bool upper_layer_enabled) noexcept {
    const u32 bandPathCount = upper_layer_enabled ? 4u : 1u;
    return VolumetricCloudAmbientProfileIntervalCount_Internal(
        kVolumetricCloudShadowCacheHeight) *
        kVolumetricCloudAmbientDirectionCount * bandPathCount;
}

/** 一水平列の太陽光生成が完成密度を読む最大回数を返す。 */
inline u32 VolumetricCloudSunDensitySamplesPerColumn_Internal(
    bool upper_layer_enabled) noexcept {
    const u32 bandPathCount = upper_layer_enabled ? 3u : 1u;
    return VolumetricCloudAmbientProfileIntervalCount_Internal(
        kVolumetricCloudShadowCacheHeight) *
        kVolumetricCloudSunDiskDirectionCount * bandPathCount;
}

/**
 * 四状態の条件付き生存率と物質空間のセル位相を区間間で継承し、環境光の一区間を進める。
 * 同じ媒質を分割しても境界を再開始せず、微小吸収も1との差へ丸めず光学的深さへ戻す。
 */
inline f32 ResolveVolumetricCloudAmbientCorrelatedSegmentOpticalDepth_Internal(
    const FVolumetricCloudDensityDistributionInternal& distribution,
    f32 extinction_per_density, f32 correlation_length,
    f32 segment_length, f32 path_coordinate,
    FVolumetricCloudFourStateTransportStateInternal& state) noexcept {
    constexpr f32 opaqueOpticalDepth = 80.0f;
    if (!CloudDensityIntegrationValueIsFinite_Internal(extinction_per_density) ||
        !CloudDensityIntegrationValueIsFinite_Internal(correlation_length) ||
        !CloudDensityIntegrationValueIsFinite_Internal(segment_length) ||
        !CloudDensityIntegrationValueIsFinite_Internal(path_coordinate) ||
        extinction_per_density < 0.0f || correlation_length < 0.0f ||
        segment_length < 0.0f) {
        ResetVolumetricCloudFourStateTransport_Internal(state);
        state.initialized = false;
        return opaqueOpticalDepth;
    }
    for (u32 densityIndex = 0u; densityIndex < 4u; ++densityIndex) {
        const f32 density = distribution.state_densities[densityIndex];
        if (!CloudDensityIntegrationValueIsFinite_Internal(density) ||
            density < 0.0f || density > 65504.0f) {
            ResetVolumetricCloudFourStateTransport_Internal(state);
            state.initialized = false;
            return opaqueOpticalDepth;
        }
    }
    if (segment_length == 0.0f || extinction_per_density == 0.0f)
        return 0.0f;

    f32 resolvedCorrelationLength = correlation_length;
    const bool validBoundaryState = state.initialized &&
        !state.awaiting_next_cell_length &&
        CloudDensityIntegrationValueIsFinite_Internal(
            state.remaining_boundary_distance) &&
        state.remaining_boundary_distance > 0.0f &&
        CloudDensityIntegrationValueIsFinite_Internal(
            state.correlation_cell_length) &&
        state.correlation_cell_length > 0.0f &&
        state.remaining_boundary_distance <= state.correlation_cell_length;
    if (resolvedCorrelationLength > 1.0e-6f && !validBoundaryState &&
        !InitializeVolumetricCloudFourStateTransportPhase_Internal(
            state, path_coordinate, resolvedCorrelationLength))
        resolvedCorrelationLength = 0.0f;
    if (resolvedCorrelationLength <= 1.0e-6f)
        resolvedCorrelationLength = 0.0f;
    const FVolumetricCloudFourStateTransportIntervalInternal interval =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, extinction_per_density,
            resolvedCorrelationLength, segment_length, state);
    const f32 depth =
        ResolveVolumetricCloudOpticalDepthFromAbsorption_Internal(
            interval.absorption);
    if (!CloudDensityIntegrationValueIsFinite_Internal(depth) ||
        depth >= opaqueOpticalDepth)
        return opaqueOpticalDepth;
    return depth > 0.0f ? depth : 0.0f;
}

/** 曲面惑星上の水平位置と高度から、桁落ちを避けて雲内のワールド位置を求める。 */
inline FVec3 ResolveVolumetricCloudAmbientWorldPosition_Internal(
    FVec2 world_xz, f32 altitude, FVec3 world_origin) noexcept {
    const f32 dx = world_xz.x - world_origin.x;
    const f32 dz = world_xz.y - world_origin.z;
    const f32 horizontalDistanceSquared = dx * dx + dz * dz;
    const f32 radius = kVolumetricCloudPlanetRadius + altitude;
    const f32 root = Sqrt(
        horizontalDistanceSquared < radius * radius
            ? radius * radius - horizontalDistanceSquared : 0.0f);
    const f32 sag = horizontalDistanceSquared /
        ((radius + root) > 1.0f ? radius + root : 1.0f);
    return FVec3{
        world_xz.x,
        world_origin.y + altitude - sag,
        world_xz.y};
}

/**
 * 雲帯底のアンカーから上向き3D方向へ進み、指定した曲面高度へ到達する距離を求める。
 */
inline bool ResolveVolumetricCloudAmbientRayDistance_Internal(
    FVec3 base_point, FVec3 direction, f32 base_altitude,
    f32 target_altitude, FVec3 world_origin,
    f32& ray_distance) noexcept {
    ray_distance = 0.0f;
    if (!CloudDensityIntegrationValueIsFinite_Internal(base_point.x) ||
        !CloudDensityIntegrationValueIsFinite_Internal(base_point.y) ||
        !CloudDensityIntegrationValueIsFinite_Internal(base_point.z) ||
        !CloudDensityIntegrationValueIsFinite_Internal(direction.x) ||
        !CloudDensityIntegrationValueIsFinite_Internal(direction.y) ||
        !CloudDensityIntegrationValueIsFinite_Internal(direction.z) ||
        !CloudDensityIntegrationValueIsFinite_Internal(base_altitude) ||
        !CloudDensityIntegrationValueIsFinite_Internal(target_altitude) ||
        target_altitude < base_altitude)
        return false;
    if (target_altitude == base_altitude) return true;
    const FVec3 local{
        base_point.x - world_origin.x,
        base_point.y - world_origin.y,
        base_point.z - world_origin.z};
    const FVec3 centerOffset{
        local.x, kVolumetricCloudPlanetRadius + local.y, local.z};
    const f32 centerDot = centerOffset.x * direction.x +
        centerOffset.y * direction.y + centerOffset.z * direction.z;
    if (!(centerDot > 0.0f)) return false;
    const f32 shellC = local.x * local.x + local.z * local.z +
        (local.y - target_altitude) *
            (2.0f * kVolumetricCloudPlanetRadius +
             local.y + target_altitude);
    f32 nearDistance = 0.0f;
    f32 farDistance = 0.0f;
    if (!ResolveVolumetricCloudSphereRoots_Internal(
            centerDot, shellC, true, nearDistance, farDistance) ||
        farDistance < 0.0f ||
        !CloudDensityIntegrationValueIsFinite_Internal(farDistance))
        return false;
    ray_distance = farDistance;
    return true;
}

/**
 * 雲内の点から指定方向を逆に辿り、指定した雲帯底にある同一直線のアンカーを求める。
 */
inline bool ResolveVolumetricCloudAmbientBandBaseAnchor_Internal(
    FVec3 point, FVec3 direction, f32 base_altitude,
    FVec3 world_origin, FVec3& base_point) noexcept {
    base_point = point;
    if (!CloudDensityIntegrationValueIsFinite_Internal(point.x) ||
        !CloudDensityIntegrationValueIsFinite_Internal(point.y) ||
        !CloudDensityIntegrationValueIsFinite_Internal(point.z) ||
        !CloudDensityIntegrationValueIsFinite_Internal(direction.x) ||
        !CloudDensityIntegrationValueIsFinite_Internal(direction.y) ||
        !CloudDensityIntegrationValueIsFinite_Internal(direction.z) ||
        !CloudDensityIntegrationValueIsFinite_Internal(base_altitude))
        return false;
    const FVec3 local{
        point.x - world_origin.x,
        point.y - world_origin.y,
        point.z - world_origin.z};
    const FVec3 centerOffset{
        local.x, kVolumetricCloudPlanetRadius + local.y, local.z};
    const f32 shellC = local.x * local.x + local.z * local.z +
        (local.y - base_altitude) *
            (2.0f * kVolumetricCloudPlanetRadius +
             local.y + base_altitude);
    f32 distance = 0.0f;
    if (shellC > 0.0f) {
        const f32 reverseCenterDot = -(
            centerOffset.x * direction.x +
            centerOffset.y * direction.y +
            centerOffset.z * direction.z);
        f32 nearDistance = 0.0f;
        f32 farDistance = 0.0f;
        if (!ResolveVolumetricCloudSphereRoots_Internal(
                reverseCenterDot, shellC, false,
                nearDistance, farDistance) ||
            nearDistance < 0.0f ||
            !CloudDensityIntegrationValueIsFinite_Internal(nearDistance))
            return false;
        distance = nearDistance;
    }
    base_point = FVec3{
        point.x - direction.x * distance,
        point.y - direction.y * distance,
        point.z - direction.z * distance};
    return CloudDensityIntegrationValueIsFinite_Internal(base_point.x) &&
        CloudDensityIntegrationValueIsFinite_Internal(base_point.y) &&
        CloudDensityIntegrationValueIsFinite_Internal(base_point.z);
}

/**
 * 一つの垂直区間を半球4方向の実光路長へ展開し、未解像4密度状態を
 * Beer-Lambert変換してから方向ごとの相関輸送を進める。
 */
inline FVolumetricCloudAmbientDirectionTransmittancesInternal
ResolveVolumetricCloudAmbientFourStateSegment_Internal(
    const FVolumetricCloudDensityDistributionInternal& distribution,
    f32 extinction_per_density, f32 vertical_segment_length,
    const f32 (&correlation_lengths)[4],
    FVolumetricCloudFourStateTransportStateInternal (&states)[4]) noexcept {
    FVolumetricCloudAmbientDirectionTransmittancesInternal out{};
    if (!CloudDensityIntegrationValueIsFinite_Internal(vertical_segment_length) ||
        vertical_segment_length <= 0.0f)
        return out;
    for (u32 directionIndex = 0u; directionIndex < 4u; ++directionIndex) {
        const f32 directionCosine =
            VolumetricCloudAmbientDirectionCosine_Internal(directionIndex);
        const auto interval =
            ResolveVolumetricCloudFourStateTransportInterval_Internal(
                distribution, extinction_per_density,
                correlation_lengths[directionIndex],
                vertical_segment_length / directionCosine,
                states[directionIndex]);
        out.values[directionIndex] = interval.transmittance;
    }
    return out;
}

/** 半球4方向の透過率を、等方放射輝度が面へ運ぶ照度の可視率へ積分する。 */
inline f32 ResolveVolumetricCloudAmbientIrradianceVisibility_Internal(
    const FVolumetricCloudAmbientDirectionTransmittancesInternal&
        direction_transmittances) noexcept {
    f32 visibility = 0.0f;
    for (u32 directionIndex = 0u; directionIndex < 4u; ++directionIndex) {
        f32 transmittance = direction_transmittances.values[directionIndex];
        if (!CloudDensityIntegrationValueIsFinite_Internal(transmittance) ||
            transmittance < 0.0f)
            transmittance = 0.0f;
        if (transmittance > 1.0f) transmittance = 1.0f;
        visibility +=
            VolumetricCloudAmbientIrradianceWeight_Internal(directionIndex) *
            transmittance;
    }
    if (!CloudDensityIntegrationValueIsFinite_Internal(visibility) ||
        visibility < 0.0f)
        return 0.0f;
    return visibility > 1.0f ? 1.0f : visibility;
}

/**
 * 局所4密度状態が一つの雲柱内で固定される近似について、半球方向ごとに
 * Beer-Lambert変換してから照度可視率へ平均する。
 */
inline f32 ResolveVolumetricCloudAmbientFixedColumnVisibility_Internal(
    const FVolumetricCloudDensityDistributionInternal& distribution,
    f32 extinction_per_density, f32 vertical_path_length) noexcept {
    if (!CloudDensityIntegrationValueIsFinite_Internal(extinction_per_density) ||
        !CloudDensityIntegrationValueIsFinite_Internal(vertical_path_length) ||
        extinction_per_density < 0.0f || vertical_path_length < 0.0f)
        return 0.0f;
    constexpr f32 densityWeights[4] = {
        kVolumetricCloudUnresolvedCoarseOuterWeight,
        kVolumetricCloudUnresolvedCoarseInnerWeight,
        kVolumetricCloudUnresolvedCoarseInnerWeight,
        kVolumetricCloudUnresolvedCoarseOuterWeight,
    };
    FVolumetricCloudAmbientDirectionTransmittancesInternal transmittances{};
    for (u32 directionIndex = 0u; directionIndex < 4u; ++directionIndex) {
        const f32 directionScale = extinction_per_density *
            vertical_path_length /
            VolumetricCloudAmbientDirectionCosine_Internal(directionIndex);
        f32 directionTransmittance = 0.0f;
        for (u32 densityIndex = 0u; densityIndex < 4u; ++densityIndex) {
            f32 density = distribution.state_densities[densityIndex];
            if (!CloudDensityIntegrationValueIsFinite_Internal(density) ||
                density < 0.0f)
                return 0.0f;
            directionTransmittance += densityWeights[densityIndex] *
                Exp(-density * directionScale);
        }
        transmittances.values[directionIndex] = directionTransmittance;
    }
    return ResolveVolumetricCloudAmbientIrradianceVisibility_Internal(
        transmittances);
}

/** 焼き込み形状を層厚へ写す高さ方向の展開率を、GPUと同じ目標周期から求める。 */
inline f32 ResolveVolumetricCloudShapeVerticalVariation_Internal(f32 shape_scale, f32 inverse_layer_height, bool upper_band) noexcept {
    constexpr f32 maximumFiniteValue = 3.402823466e+38F;
    constexpr f32 minimumVerticalSpan = 0.08f;
    constexpr f32 lowerTargetCycles = 0.95f;
    constexpr f32 upperTargetCycles = 0.72f;
    constexpr f32 lowerMaximumVariation = 8.50f;
    constexpr f32 upperMaximumVariation = 4.00f;
    if (!CloudDensityIntegrationValueIsFinite_Internal(shape_scale) ||
        !CloudDensityIntegrationValueIsFinite_Internal(inverse_layer_height))
        return maximumFiniteValue;
    if (!(shape_scale > 0.0f)) return 0.0f;
    const f32 safeInverseHeight = inverse_layer_height > 1.0e-6f
        ? inverse_layer_height : 1.0e-6f;
    const f32 verticalSpan = shape_scale / safeInverseHeight;
    if (!CloudDensityIntegrationValueIsFinite_Internal(verticalSpan))
        return maximumFiniteValue;
    const f32 targetCycles = upper_band ? upperTargetCycles : lowerTargetCycles;
    const f32 boundedSpan = verticalSpan > minimumVerticalSpan
        ? verticalSpan : minimumVerticalSpan;
    const f32 variation = targetCycles / boundedSpan;
    const f32 maximumVariation = upper_band
        ? upperMaximumVariation : lowerMaximumVariation;
    return variation > maximumVariation ? maximumVariation : variation;
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
    const f32 verticalVariation = ResolveVolumetricCloudShapeVerticalVariation_Internal(
        shape_scale, inverse_layer_height, upper_band);
    if (!CloudDensityIntegrationValueIsFinite_Internal(verticalVariation))
        return maximumFiniteValue;
    const f32 canonicalY = altitudeWidth * shape_scale * verticalVariation;
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
