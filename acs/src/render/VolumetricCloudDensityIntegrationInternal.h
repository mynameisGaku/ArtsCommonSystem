// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_RENDER_VOLUMETRIC_CLOUD_DENSITY_INTEGRATION_INTERNAL_H
#define ACS_RENDER_VOLUMETRIC_CLOUD_DENSITY_INTEGRATION_INTERNAL_H

#include "foundation/Types.h"
#include "math/Math.h"
#include "memory/Memory.h"

namespace acs::render_internal {

/** 一つの視線セルへ常に使うGauss-Legendre求積点数。 */
inline constexpr u32 kVolumetricCloudDensityGaussSampleCount = 4u;

/** 隣接するGauss点のうち最も広い間隔を区間長で割った値。 */
inline constexpr f32 kVolumetricCloudDensityGaussMaximumGapFraction =
    0.3399810436f;

/** 密度区間境界と照明採取位置を合わせた解析輸送区間数。 */
inline constexpr u32 kVolumetricCloudBeerTransportSegmentCount = 8u;

/** 1付近の単精度値で隣接表現までに必要な差。 */
inline constexpr f32 kVolumetricCloudFloatUlpAtOne =
    0.00000011920928955078125f;

/**
 * 一輸送区間の指数、減算、乗算、加算を上限側へ包む単精度表現数。
 * 実積分の吸収率を物理上限へ制限したうえで、残る演算を8 ULPで覆う。
 */
inline constexpr u32 kVolumetricCloudTransportRoundingUlpsPerSegment = 8u;

/** 一輸送区間が残差上限へ加える演算誤差。 */
inline constexpr f32 kVolumetricCloudTransportRoundingErrorPerSegment =
    kVolumetricCloudFloatUlpAtOne *
    static_cast<f32>(kVolumetricCloudTransportRoundingUlpsPerSegment);

/**
 * Gauss重みで区切った密度区間とGauss採取位置を昇順にまとめた境界。
 * 密度は二区間ずつ同じ採取値を使い、照明は隣接採取値を線形補間する。
 */
inline constexpr f32 kVolumetricCloudBeerTransportBoundaries[9]{
    0.0f, 0.0694318442f, 0.1739274226f,
    0.3300094782f, 0.5f, 0.6699905218f,
    0.8260725774f, 0.9305681558f, 1.0f};

/** 一つの視線区間で実密度を採取する位置と重み。 */
struct FVolumetricCloudDensityQuadratureInternal {
    /** 区間を0～1へ正規化した採取位置。 */
    f32 fractions[4]{
        0.0694318442f, 0.3300094782f,
        0.6699905218f, 0.9305681558f};

    /** 区間平均へ使う採取重み。合計は1。 */
    f32 weights[4]{
        0.1739274226f, 0.3260725774f,
        0.3260725774f, 0.1739274226f};

    /** 有効な採取数。 */
    u32 sample_count = kVolumetricCloudDensityGaussSampleCount;
};

/** 完成形状と低周波湿度核を混ぜる重み。 */
struct FVolumetricCloudShapeFrequencyBlendInternal {
    /** 周波数2の湿度核だけで作る形状の重み。 */
    f32 coarse = 0.0f;

    /** 周波数4まで含む中間形状の重み。 */
    f32 middle = 0.0f;

    /** 周波数8まで含む完成形状の重み。 */
    f32 complete = 1.0f;
};

/** 視線上の求積点を前から順に合成した一次散乱状態。 */
struct FVolumetricCloudBeerIntegrationInternal {
    /** まだ遮られていない視線透過率。 */
    f32 transmittance = 1.0f;

    /** これまでの区間が作った不透明度。 */
    f32 opacity = 0.0f;

    /** 三次数までの局所散乱源へ各次数の寄与を掛けた放射輝度。 */
    f32 radiance = 0.0f;

    /** 一次散乱だけが作った放射輝度。 */
    f32 first_order_radiance = 0.0f;

    /** 二次散乱だけが作った放射輝度。 */
    f32 second_order_radiance = 0.0f;

    /** 三次散乱だけが作った放射輝度。 */
    f32 third_order_radiance = 0.0f;

    /** 二次散乱の縮小消散を適用した残り透過率。 */
    f32 second_order_transmittance = 1.0f;

    /** 三次散乱の縮小消散を適用した残り透過率。 */
    f32 third_order_transmittance = 1.0f;

    /** 不透明度寄与と正規化距離を掛けた一次モーメント。 */
    f32 normalized_distance_moment = 0.0f;

};

/** 一つのGauss点で同じ位置から採取した密度と一次散乱源。 */
struct FVolumetricCloudDensityLightingSampleInternal {
    /** 消散倍率を含み、区間長と消散係数を掛ける前の密度。 */
    f32 density = 0.0f;

    /** 同じ位置で求めた非負の局所散乱源。 */
    f32 source_radiance = 0.0f;

    /** 同じ位置で求めた二次散乱源。 */
    f32 second_order_source_radiance = 0.0f;

    /** 同じ位置で求めた三次散乱源。 */
    f32 third_order_source_radiance = 0.0f;
};

/** 早期終了した場合に残りの媒質が作り得る画面誤差の上限。 */
struct FVolumetricCloudTransportErrorBoundInternal {
    /** 一次散乱と深度が今後も変わり得るかを示す残り透過率。 */
    f32 first_order_transmittance = 1.0f;

    /** 有限な残り経路で増え得る一次不透明度の上限。 */
    f32 remaining_first_order_opacity = 1.0f;

    /** 有限な残り経路で全散乱次数が増やし得る雲放射輝度の上限。 */
    f32 remaining_radiance = 0.0f;
};

/** 標準ライブラリへ依存せず、単精度値が有限かを判定する。 */
inline bool CloudDensityIntegrationValueIsFinite_Internal(f32 value) noexcept
{
    constexpr f32 maximumFiniteValue = 3.402823466e+38F;
    return value == value && value >= -maximumFiniteValue &&
           value <= maximumFiniteValue;
}

/**
 * 均質区間のBeer-Lambert吸収率を安定して求める。
 * 指数近似が物理上限 min(光学的深さ, 1) を越えた場合は上限へ戻す。
 */
inline f32 ResolveVolumetricCloudBeerAbsorptionFraction_Internal(
    f32 optical_depth) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(optical_depth))
        return 1.0f;
    if (optical_depth <= 0.0f) return 0.0f;
    const f32 maximumAbsorption =
        optical_depth < 1.0f ? optical_depth : 1.0f;
    f32 absorption = maximumAbsorption;
    if (optical_depth <= 0.125f)
    {
        const f32 opticalDepthSquared = optical_depth * optical_depth;
        absorption = optical_depth *
            (1.0f - optical_depth * 0.5f +
             opticalDepthSquared / 6.0f -
             opticalDepthSquared * optical_depth / 24.0f +
             opticalDepthSquared * opticalDepthSquared / 120.0f -
             opticalDepthSquared * opticalDepthSquared * optical_depth /
                 720.0f);
    }
    else
    {
        absorption = 1.0f - Exp(-optical_depth);
    }
    if (!CloudDensityIntegrationValueIsFinite_Internal(absorption))
        return maximumAbsorption;
    if (absorption < 0.0f) absorption = 0.0f;
    if (absorption > maximumAbsorption) absorption = maximumAbsorption;
    return absorption;
}

/**
 * 区間長に依存しない4点Gauss-Legendre求積を返す。
 * 採取数が境界で切り替わらないため、カメラ移動で密度推定が段差を作らない。
 * 未解像帯域は求積前に別の低周波形状へ連続移行させる。
 */
inline FVolumetricCloudDensityQuadratureInternal
ResolveVolumetricCloudDensityQuadrature_Internal(f32 interval_length) noexcept
{
    (void)interval_length;
    return FVolumetricCloudDensityQuadratureInternal{};
}

/**
 * 完成形状の最高周波数が担当幅の1/4周期を越えてから低周波形状へ移し、
 * Nyquist限界の1/2周期までに移行を完了する。平均値へ変えないため、
 * 晴天列と雲列を一様な灰色媒質へ混ぜない。
 */
inline FVolumetricCloudShapeFrequencyBlendInternal
ResolveVolumetricCloudShapeFrequencyBlend_Internal(
    f32 maximum_domain_footprint) noexcept
{
    FVolumetricCloudShapeFrequencyBlendInternal out{};
    if (!CloudDensityIntegrationValueIsFinite_Internal(
            maximum_domain_footprint))
    {
        out.coarse = 1.0f;
        out.middle = 0.0f;
        out.complete = 0.0f;
        return out;
    }
    const f32 safeFootprint =
        maximum_domain_footprint > 0.0f
            ? maximum_domain_footprint : 0.0f;
    f32 fineTransition = (safeFootprint * 8.0f - 0.25f) * 4.0f;
    if (fineTransition < 0.0f) fineTransition = 0.0f;
    if (fineTransition > 1.0f) fineTransition = 1.0f;
    const f32 fineVisibility = 1.0f -
        fineTransition * fineTransition * (3.0f - 2.0f * fineTransition);
    f32 middleTransition = (safeFootprint * 4.0f - 0.25f) * 4.0f;
    if (middleTransition < 0.0f) middleTransition = 0.0f;
    if (middleTransition > 1.0f) middleTransition = 1.0f;
    const f32 middleVisibility = 1.0f -
        middleTransition * middleTransition * (3.0f - 2.0f * middleTransition);
    out.complete = fineVisibility;
    out.middle = (1.0f - fineVisibility) * middleVisibility;
    out.coarse = (1.0f - fineVisibility) * (1.0f - middleVisibility);
    return out;
}

/**
 * 均質区間で吸収が起きる平均位置を区間始点からの比率で返す。
 * 既に求めた透過率を再利用し、散乱次数ごとの指数演算を重複させない。
 */
inline f32 ResolveVolumetricCloudBeerAbsorptionCentroidFromTransmittance_Internal(
    f32 optical_depth, f32 interval_transmittance) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(optical_depth) ||
        optical_depth <= 0.0f)
        return 0.5f;
    f32 centroid = 0.5f;
    if (optical_depth <= 1.0f)
    {
        const f32 opticalDepthSquared = optical_depth * optical_depth;
        const f32 opticalDepthCubed =
            opticalDepthSquared * optical_depth;
        const f32 opticalDepthFifth =
            opticalDepthCubed * opticalDepthSquared;
        const f32 opticalDepthSeventh =
            opticalDepthFifth * opticalDepthSquared;
        const f32 opticalDepthNinth =
            opticalDepthSeventh * opticalDepthSquared;
        centroid = 0.5f - optical_depth / 12.0f +
            opticalDepthCubed / 720.0f -
            opticalDepthFifth / 30240.0f +
            opticalDepthSeventh / 1209600.0f -
            opticalDepthNinth / 47900160.0f;
    }
    else if (optical_depth > 20.0f)
    {
        centroid = 1.0f / optical_depth;
    }
    else
    {
        f32 transmittance = interval_transmittance;
        if (!CloudDensityIntegrationValueIsFinite_Internal(transmittance))
            transmittance = 1.0f -
                ResolveVolumetricCloudBeerAbsorptionFraction_Internal(
                    optical_depth);
        if (transmittance < 0.0f) transmittance = 0.0f;
        if (transmittance > 1.0f) transmittance = 1.0f;
        const f32 absorbed = 1.0f - transmittance;
        centroid = 1.0f / optical_depth -
            transmittance / (absorbed > 1.0e-20f ? absorbed : 1.0e-20f);
    }
    if (centroid < 0.0f) centroid = 0.0f;
    if (centroid > 0.5f) centroid = 0.5f;
    return centroid;
}

/**
 * 透過率をまだ持たない呼び出し向けに、安定した吸収率から重心を求める。
 */
inline f32 ResolveVolumetricCloudBeerAbsorptionCentroid_Internal(
    f32 optical_depth) noexcept
{
    const f32 intervalTransmittance = 1.0f -
        ResolveVolumetricCloudBeerAbsorptionFraction_Internal(optical_depth);
    return ResolveVolumetricCloudBeerAbsorptionCentroidFromTransmittance_Internal(
        optical_depth, intervalTransmittance);
}

/**
 * 隣接Gauss点の散乱源を指定位置へ線形補間する。
 * 外側はセル端で負にならない傾きへ制限し、線形照明の吸収重心を保つ。
 * 片側が空なら有効側の値だけを使う。
 */
inline f32 ResolveVolumetricCloudLinearSourcePair_Internal(
    f32 left_source, f32 right_source,
    bool left_valid, bool right_valid,
    f32 left_fraction, f32 right_fraction,
    f32 normalized_distance, f32 source_upper_bound) noexcept
{
    f32 source = 0.0f;
    if (left_valid && right_valid)
    {
        const f32 fractionSpan = right_fraction - left_fraction;
        f32 sourceSlope =
            (right_source - left_source) / fractionSpan;
        f32 sourceUpper = source_upper_bound;
        if (!CloudDensityIntegrationValueIsFinite_Internal(sourceUpper) ||
            sourceUpper < left_source)
            sourceUpper = left_source;
        if (sourceUpper < right_source) sourceUpper = right_source;
        if (sourceUpper < 0.0f) sourceUpper = 0.0f;
        if (normalized_distance < left_fraction)
        {
            const f32 maximumPositiveSlope =
                (left_source > 0.0f ? left_source : 0.0f) /
                (left_fraction > 1.0e-6f ? left_fraction : 1.0e-6f);
            const f32 minimumNegativeSlope =
                (left_source - sourceUpper) /
                (left_fraction > 1.0e-6f ? left_fraction : 1.0e-6f);
            if (sourceSlope > maximumPositiveSlope)
                sourceSlope = maximumPositiveSlope;
            if (sourceSlope < minimumNegativeSlope)
                sourceSlope = minimumNegativeSlope;
        }
        else if (normalized_distance > right_fraction)
        {
            const f32 remainingFraction = 1.0f - right_fraction;
            const f32 minimumNegativeSlope =
                -(right_source > 0.0f ? right_source : 0.0f) /
                (remainingFraction > 1.0e-6f
                    ? remainingFraction : 1.0e-6f);
            const f32 maximumPositiveSlope =
                (sourceUpper - right_source) /
                (remainingFraction > 1.0e-6f
                    ? remainingFraction : 1.0e-6f);
            if (sourceSlope < minimumNegativeSlope)
                sourceSlope = minimumNegativeSlope;
            if (sourceSlope > maximumPositiveSlope)
                sourceSlope = maximumPositiveSlope;
        }
        source = left_source + sourceSlope *
            (normalized_distance - left_fraction);
    }
    else if (left_valid)
    {
        source = left_source;
    }
    else if (right_valid)
    {
        source = right_source;
    }
    return source > 0.0f ? source : 0.0f;
}

/**
 * 消散縮小率を掛けた均質区間の散乱重みを解析的に求める。
 * xが0へ近づく場合はphi(x)=(1-exp(-x))/xの級数を使い、
 * 重みだけを異なる極限へ切り替えない。
 */
inline f32 ResolveVolumetricCloudReducedIntervalScatteringWeight_Internal(
    f32 optical_depth, f32 interval_transmittance,
    f32 contribution, f32 occlusion) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(optical_depth) ||
        !CloudDensityIntegrationValueIsFinite_Internal(interval_transmittance) ||
        !CloudDensityIntegrationValueIsFinite_Internal(contribution) ||
        !CloudDensityIntegrationValueIsFinite_Internal(occlusion) ||
        optical_depth <= 0.0f || contribution <= 0.0f)
        return 0.0f;
    const f32 safeOcclusion = occlusion > 0.0f ? occlusion : 0.0f;
    const f32 reducedOpticalDepth = optical_depth * safeOcclusion;
    f32 normalizedAbsorption = 1.0f;
    if (reducedOpticalDepth <= 0.001f)
    {
        const f32 squared = reducedOpticalDepth * reducedOpticalDepth;
        normalizedAbsorption = 1.0f - reducedOpticalDepth * 0.5f +
            squared / 6.0f - squared * reducedOpticalDepth / 24.0f;
    }
    else
    {
        f32 transmittance = interval_transmittance;
        if (transmittance < 0.0f) transmittance = 0.0f;
        if (transmittance > 1.0f) transmittance = 1.0f;
        normalizedAbsorption =
            (1.0f - transmittance) / reducedOpticalDepth;
    }
    const f32 maximumWeight = contribution * optical_depth;
    f32 weight = maximumWeight * normalizedAbsorption;
    if (!CloudDensityIntegrationValueIsFinite_Internal(weight) ||
        weight < 0.0f)
        weight = maximumWeight;
    if (weight > maximumWeight) weight = maximumWeight;
    return weight;
}

/**
 * 一セルの密度をGauss重み幅の四つの均質層として保持し、各層内の
 * 線形散乱源とBeer-Lambert吸収を解析積分する。
 */
inline void IntegrateVolumetricCloudBeerCell_Internal(
    FVolumetricCloudBeerIntegrationInternal& integration,
    const FVolumetricCloudDensityLightingSampleInternal samples[4],
    f32 interval_length, f32 extinction,
    f32 first_order_source_upper_bound,
    f32 second_order_source_upper_bound,
    f32 third_order_source_upper_bound,
    f32 second_order_contribution = 0.0f,
    f32 second_order_occlusion = 0.0f,
    f32 third_order_contribution = 0.0f,
    f32 third_order_occlusion = 0.0f) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(interval_length) ||
        !CloudDensityIntegrationValueIsFinite_Internal(extinction) ||
        interval_length <= 0.0f || extinction <= 0.0f)
        return;
    const FVolumetricCloudDensityQuadratureInternal quadrature{};
    const f32 safeSecondOcclusion = second_order_occlusion > 0.0f
        ? second_order_occlusion : 0.0f;
    const f32 safeThirdOcclusion = third_order_occlusion > 0.0f
        ? third_order_occlusion : 0.0f;
    for (u32 segmentIndex = 0u;
         segmentIndex < kVolumetricCloudBeerTransportSegmentCount;
         ++segmentIndex)
    {
        const u32 densityIndex = segmentIndex >> 1u;
        const f32 density = samples[densityIndex].density;
        if (!CloudDensityIntegrationValueIsFinite_Internal(density) ||
            density <= 0.0f)
            continue;
        const f32 segmentStart =
            kVolumetricCloudBeerTransportBoundaries[segmentIndex];
        const f32 segmentEnd =
            kVolumetricCloudBeerTransportBoundaries[segmentIndex + 1u];
        const f32 segmentWidth = segmentEnd - segmentStart;
        const f32 opticalDepth = density * segmentWidth *
            interval_length * extinction;
        const f32 intervalTransmittance = 1.0f -
            ResolveVolumetricCloudBeerAbsorptionFraction_Internal(
                opticalDepth);
        const f32 secondOpticalDepth =
            opticalDepth * safeSecondOcclusion;
        const f32 thirdOpticalDepth =
            opticalDepth * safeThirdOcclusion;
        const f32 secondIntervalTransmittance = 1.0f -
            ResolveVolumetricCloudBeerAbsorptionFraction_Internal(
                secondOpticalDepth);
        const f32 thirdIntervalTransmittance = 1.0f -
            ResolveVolumetricCloudBeerAbsorptionFraction_Internal(
                thirdOpticalDepth);
        const f32 nextTransmittance =
            integration.transmittance * intervalTransmittance;
        const f32 opacityContribution =
            integration.transmittance > nextTransmittance
                ? integration.transmittance - nextTransmittance : 0.0f;
        const f32 absorptionCentroid = segmentStart + segmentWidth *
            ResolveVolumetricCloudBeerAbsorptionCentroidFromTransmittance_Internal(
                opticalDepth, intervalTransmittance);
        const f32 secondAbsorptionCentroid = segmentStart + segmentWidth *
            ResolveVolumetricCloudBeerAbsorptionCentroidFromTransmittance_Internal(
                secondOpticalDepth, secondIntervalTransmittance);
        const f32 thirdAbsorptionCentroid = segmentStart + segmentWidth *
            ResolveVolumetricCloudBeerAbsorptionCentroidFromTransmittance_Internal(
                thirdOpticalDepth, thirdIntervalTransmittance);
        u32 sourceLeftIndex = 0u;
        if (segmentIndex >= 3u) sourceLeftIndex = 1u;
        if (segmentIndex >= 5u) sourceLeftIndex = 2u;
        const u32 sourceRightIndex = sourceLeftIndex + 1u;
        const bool leftDensityValid =
            samples[sourceLeftIndex].density > 0.0f;
        const bool rightDensityValid =
            samples[sourceRightIndex].density > 0.0f;
        const f32 leftFraction = quadrature.fractions[sourceLeftIndex];
        const f32 rightFraction = quadrature.fractions[sourceRightIndex];
        const f32 firstSource = ResolveVolumetricCloudLinearSourcePair_Internal(
            samples[sourceLeftIndex].source_radiance,
            samples[sourceRightIndex].source_radiance,
            leftDensityValid && CloudDensityIntegrationValueIsFinite_Internal(
                samples[sourceLeftIndex].source_radiance),
            rightDensityValid && CloudDensityIntegrationValueIsFinite_Internal(
                samples[sourceRightIndex].source_radiance),
            leftFraction, rightFraction, absorptionCentroid,
            first_order_source_upper_bound);
        const f32 secondSource = ResolveVolumetricCloudLinearSourcePair_Internal(
            samples[sourceLeftIndex].second_order_source_radiance,
            samples[sourceRightIndex].second_order_source_radiance,
            leftDensityValid && CloudDensityIntegrationValueIsFinite_Internal(
                samples[sourceLeftIndex].second_order_source_radiance),
            rightDensityValid && CloudDensityIntegrationValueIsFinite_Internal(
                samples[sourceRightIndex].second_order_source_radiance),
            leftFraction, rightFraction, secondAbsorptionCentroid,
            second_order_source_upper_bound);
        const f32 thirdSource = ResolveVolumetricCloudLinearSourcePair_Internal(
            samples[sourceLeftIndex].third_order_source_radiance,
            samples[sourceRightIndex].third_order_source_radiance,
            leftDensityValid && CloudDensityIntegrationValueIsFinite_Internal(
                samples[sourceLeftIndex].third_order_source_radiance),
            rightDensityValid && CloudDensityIntegrationValueIsFinite_Internal(
                samples[sourceRightIndex].third_order_source_radiance),
            leftFraction, rightFraction, thirdAbsorptionCentroid,
            third_order_source_upper_bound);
        const f32 secondContribution =
            integration.second_order_transmittance *
            ResolveVolumetricCloudReducedIntervalScatteringWeight_Internal(
                opticalDepth, secondIntervalTransmittance,
                second_order_contribution, safeSecondOcclusion);
        const f32 thirdContribution =
            integration.third_order_transmittance *
            ResolveVolumetricCloudReducedIntervalScatteringWeight_Internal(
                opticalDepth, thirdIntervalTransmittance,
                third_order_contribution, safeThirdOcclusion);
        integration.opacity += opacityContribution;
        const f32 firstRadiance = opacityContribution * firstSource;
        const f32 secondRadiance = secondContribution * secondSource;
        const f32 thirdRadiance = thirdContribution * thirdSource;
        integration.first_order_radiance += firstRadiance;
        integration.second_order_radiance += secondRadiance;
        integration.third_order_radiance += thirdRadiance;
        integration.radiance +=
            firstRadiance + secondRadiance + thirdRadiance;
        integration.normalized_distance_moment +=
            opacityContribution * absorptionCentroid;
        integration.transmittance = nextTransmittance;
        integration.second_order_transmittance *=
            secondIntervalTransmittance;
        integration.third_order_transmittance *=
            thirdIntervalTransmittance;
    }
}

/** 正の有限値を指定数の単精度表現だけ上へ広げ、丸め誤差を上限へ含める。 */
inline f32 InflateVolumetricCloudPositiveUpper_Internal(
    f32 value, u32 ulp_count = 32u) noexcept
{
    constexpr f32 maximumFiniteValue = 3.402823466e+38F;
    constexpr u32 maximumFiniteCode = 0x7F7FFFFFu;
    if (!CloudDensityIntegrationValueIsFinite_Internal(value) || value < 0.0f)
        return maximumFiniteValue;
    if (value == 0.0f || ulp_count == 0u) return value;
    u32 code = 0u;
    MemCopy(&code, &value, sizeof(code));
    if (code >= maximumFiniteCode ||
        ulp_count > maximumFiniteCode - code)
        return maximumFiniteValue;
    code += ulp_count;
    f32 inflated = 0.0f;
    MemCopy(&inflated, &code, sizeof(inflated));
    return inflated;
}

/** 正の有限値を指定数の単精度表現だけ下へ広げ、負値にはしない。 */
inline f32 DeflateVolumetricCloudPositiveLower_Internal(
    f32 value, u32 ulp_count = 32u) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(value) ||
        value <= 0.0f)
        return 0.0f;
    u32 code = 0u;
    MemCopy(&code, &value, sizeof(code));
    if (code <= ulp_count) return 0.0f;
    code -= ulp_count;
    f32 deflated = 0.0f;
    MemCopy(&deflated, &code, sizeof(deflated));
    return deflated;
}

/** 正の二値の積を上向きに広げ、無効値は最大有限値として扱う。 */
inline f32 MultiplyVolumetricCloudPositiveUpper_Internal(
    f32 left, f32 right) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(left) ||
        !CloudDensityIntegrationValueIsFinite_Internal(right) ||
        left < 0.0f || right < 0.0f)
    {
        constexpr f32 maximumFiniteValue = 3.402823466e+38F;
        return maximumFiniteValue;
    }
    if (left == 0.0f || right == 0.0f) return 0.0f;
    return InflateVolumetricCloudPositiveUpper_Internal(left * right);
}

/** 正の二値の和を上向きに広げ、無効値は最大有限値として扱う。 */
inline f32 AddVolumetricCloudPositiveUpper_Internal(
    f32 left, f32 right) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(left) ||
        !CloudDensityIntegrationValueIsFinite_Internal(right) ||
        left < 0.0f || right < 0.0f)
    {
        constexpr f32 maximumFiniteValue = 3.402823466e+38F;
        return maximumFiniteValue;
    }
    return InflateVolumetricCloudPositiveUpper_Internal(left + right);
}

/**
 * 上限側へ広げ済みの終点から現在位置を引き、桁落ちを含む残距離の上限を返す。
 * 無効値または負の位置は最大有限値へ退避し、早期終了を許可しない。
 */
inline f32 SubtractVolumetricCloudPositiveUpper_Internal(
    f32 upper_end, f32 current_end) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(upper_end) ||
        !CloudDensityIntegrationValueIsFinite_Internal(current_end) ||
        upper_end < 0.0f || current_end < 0.0f)
    {
        constexpr f32 maximumFiniteValue = 3.402823466e+38F;
        return maximumFiniteValue;
    }
    const f32 difference = upper_end > current_end
        ? upper_end - current_end : 0.0f;
    return InflateVolumetricCloudPositiveUpper_Internal(difference);
}

/**
 * 現在帯域と未処理の次帯域に残る細密セルを、Beer輸送区間数へ変換する。
 * 粗探索を一セルへ細分化した直後も、未処理の兄弟セルを必ず数える。
 */
inline u32 ResolveVolumetricCloudRemainingTransportSegmentCount_Internal(
    u32 current_fine_cell_count, u32 next_fine_cell_index,
    bool has_next_interval, u32 next_fine_cell_count) noexcept
{
    constexpr u32 maximumCellCount =
        0xFFFFFFFFu / kVolumetricCloudBeerTransportSegmentCount;
    const u32 currentRemainingCellCount =
        next_fine_cell_index < current_fine_cell_count
            ? current_fine_cell_count - next_fine_cell_index : 0u;
    const u32 nextRemainingCellCount = has_next_interval
        ? next_fine_cell_count : 0u;
    if (currentRemainingCellCount > maximumCellCount ||
        nextRemainingCellCount >
            maximumCellCount - currentRemainingCellCount)
    {
        return 0xFFFFFFFFu;
    }
    return (currentRemainingCellCount + nextRemainingCellCount) *
        kVolumetricCloudBeerTransportSegmentCount;
}

/**
 * 残り区間の解析的散乱重みを、指数関数を使わず上限側で求める。
 * 1-exp(-x) <= min(x,1) と残り区間数の演算誤差だけを利用する。
 */
inline f32 ResolveVolumetricCloudTransportWeightUpper_Internal(
    f32 transmittance, f32 primary_optical_depth_upper_bound,
    f32 contribution, f32 occlusion,
    u32 remaining_transport_segment_count) noexcept
{
    const f32 values[]{
        transmittance, primary_optical_depth_upper_bound,
        contribution, occlusion};
    for (u32 valueIndex = 0u; valueIndex < 4u; ++valueIndex)
    {
        if (!CloudDensityIntegrationValueIsFinite_Internal(
                values[valueIndex]) || values[valueIndex] < 0.0f)
        {
            constexpr f32 maximumFiniteValue = 3.402823466e+38F;
            return maximumFiniteValue;
        }
    }
    const f32 segmentCount =
        static_cast<f32>(remaining_transport_segment_count);
    const f32 arithmeticMargin =
        MultiplyVolumetricCloudPositiveUpper_Internal(
            segmentCount,
            kVolumetricCloudTransportRoundingErrorPerSegment);
    const f32 effectiveOpticalDepth =
        AddVolumetricCloudPositiveUpper_Internal(
            primary_optical_depth_upper_bound, arithmeticMargin);
    f32 reducedWeight =
        MultiplyVolumetricCloudPositiveUpper_Internal(
            contribution, effectiveOpticalDepth);
    if (occlusion > 0.0f)
    {
        const f32 saturatedWeight =
            InflateVolumetricCloudPositiveUpper_Internal(
                contribution / occlusion);
        if (saturatedWeight < reducedWeight)
            reducedWeight = saturatedWeight;
    }
    const f32 transportedWeight =
        MultiplyVolumetricCloudPositiveUpper_Internal(
            transmittance, reducedWeight);
    const f32 accumulationScale =
        AddVolumetricCloudPositiveUpper_Internal(1.0f, arithmeticMargin);
    return MultiplyVolumetricCloudPositiveUpper_Internal(
        transportedWeight, accumulationScale);
}

/**
 * 残りの有限経路を最大密度で満たした場合に増え得る不透明度と
 * 三次数までの雲放射輝度を求める。上向き丸め余裕もここで加える。
 */
inline FVolumetricCloudTransportErrorBoundInternal
ResolveVolumetricCloudTransportErrorBound_Internal(
    f32 first_order_transmittance, f32 second_order_transmittance,
    f32 third_order_transmittance,
    f32 remaining_primary_optical_depth_upper_bound,
    u32 remaining_transport_segment_count,
    f32 first_order_source_upper_bound,
    f32 second_order_source_upper_bound,
    f32 third_order_source_upper_bound,
    f32 second_order_contribution, f32 second_order_occlusion,
    f32 third_order_contribution, f32 third_order_occlusion) noexcept
{
    const f32 values[]{
        first_order_transmittance, second_order_transmittance,
        third_order_transmittance,
        remaining_primary_optical_depth_upper_bound,
        first_order_source_upper_bound,
        second_order_source_upper_bound,
        third_order_source_upper_bound,
        second_order_contribution, second_order_occlusion,
        third_order_contribution, third_order_occlusion};
    for (u32 valueIndex = 0u; valueIndex < 11u; ++valueIndex)
    {
        if (!CloudDensityIntegrationValueIsFinite_Internal(
                values[valueIndex]) || values[valueIndex] < 0.0f)
        {
            constexpr f32 maximumFiniteValue = 3.402823466e+38F;
            return FVolumetricCloudTransportErrorBoundInternal{
                maximumFiniteValue, maximumFiniteValue,
                maximumFiniteValue};
        }
    }
    const f32 firstTransmittance = first_order_transmittance;
    const f32 secondTransmittance = second_order_transmittance;
    const f32 thirdTransmittance = third_order_transmittance;
    const f32 remainingOpticalDepth =
        remaining_primary_optical_depth_upper_bound;
    const f32 firstSource = first_order_source_upper_bound;
    const f32 secondSource = second_order_source_upper_bound;
    const f32 thirdSource = third_order_source_upper_bound;
    const f32 remainingFirstOpacity =
        ResolveVolumetricCloudTransportWeightUpper_Internal(
            firstTransmittance, remainingOpticalDepth,
            1.0f, 1.0f, remaining_transport_segment_count);
    const f32 remainingSecondWeight =
        ResolveVolumetricCloudTransportWeightUpper_Internal(
            secondTransmittance, remainingOpticalDepth,
            second_order_contribution, second_order_occlusion,
            remaining_transport_segment_count);
    const f32 remainingThirdWeight =
        ResolveVolumetricCloudTransportWeightUpper_Internal(
            thirdTransmittance, remainingOpticalDepth,
            third_order_contribution, third_order_occlusion,
            remaining_transport_segment_count);
    FVolumetricCloudTransportErrorBoundInternal bound{};
    bound.first_order_transmittance = firstTransmittance;
    bound.remaining_first_order_opacity = remainingFirstOpacity;
    const f32 firstRadiance =
        MultiplyVolumetricCloudPositiveUpper_Internal(
            remainingFirstOpacity, firstSource);
    const f32 secondRadiance =
        MultiplyVolumetricCloudPositiveUpper_Internal(
            remainingSecondWeight, secondSource);
    const f32 thirdRadiance =
        MultiplyVolumetricCloudPositiveUpper_Internal(
            remainingThirdWeight, thirdSource);
    bound.remaining_radiance =
        AddVolumetricCloudPositiveUpper_Internal(
            AddVolumetricCloudPositiveUpper_Internal(
                firstRadiance, secondRadiance),
            thirdRadiance);
    if (!CloudDensityIntegrationValueIsFinite_Internal(
            bound.remaining_radiance))
    {
        constexpr f32 maximumFiniteValue = 3.402823466e+38F;
        bound.remaining_radiance = maximumFiniteValue;
    }
    return bound;
}

/** 非負有限の単精度値を、最近接偶数丸めのR16Fビット列へ変換する。 */
inline u16 ResolveVolumetricCloudR16Code_Internal(f32 value) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(value) ||
        value <= 0.0f)
        return 0u;
    if (value >= 65504.0f) return 0x7BFFu;
    u32 bits = 0u;
    MemCopy(&bits, &value, sizeof(bits));
    const u32 singleExponent = (bits >> 23u) & 0xFFu;
    u32 mantissa = bits & 0x7FFFFFu;
    i32 halfExponent = static_cast<i32>(singleExponent) - 127 + 15;
    if (halfExponent <= 0)
    {
        if (halfExponent < -10) return 0u;
        mantissa |= 0x800000u;
        const u32 shift = static_cast<u32>(14 - halfExponent);
        u32 halfMantissa = mantissa >> shift;
        const u32 remainderMask = (1u << shift) - 1u;
        const u32 remainder = mantissa & remainderMask;
        const u32 halfway = 1u << (shift - 1u);
        if (remainder > halfway ||
            (remainder == halfway && (halfMantissa & 1u) != 0u))
            ++halfMantissa;
        return static_cast<u16>(halfMantissa);
    }
    u32 halfMantissa = mantissa >> 13u;
    const u32 remainder = mantissa & 0x1FFFu;
    if (remainder > 0x1000u ||
        (remainder == 0x1000u && (halfMantissa & 1u) != 0u))
    {
        ++halfMantissa;
        if (halfMantissa == 0x400u)
        {
            halfMantissa = 0u;
            ++halfExponent;
        }
    }
    if (halfExponent >= 31) return 0x7BFFu;
    return static_cast<u16>(
        (static_cast<u32>(halfExponent) << 10u) | halfMantissa);
}

/**
 * 有限経路の残差を加えても、R16Fの色と不透明度、およびR32Fの
 * 不透明度と深度が同じ表現へ丸められる場合だけ終了してよい。
 */
inline bool CanTerminateVolumetricCloudTransport_Internal(
    const FVolumetricCloudTransportErrorBoundInternal& bound,
    f32 current_radiance, f32 current_distance_moment,
    f32 maximum_remaining_distance) noexcept
{
    const f32 values[]{
        bound.first_order_transmittance,
        bound.remaining_first_order_opacity,
        bound.remaining_radiance, current_radiance,
        current_distance_moment,
        maximum_remaining_distance};
    for (u32 valueIndex = 0u; valueIndex < 6u; ++valueIndex)
    {
        if (!CloudDensityIntegrationValueIsFinite_Internal(values[valueIndex]))
            return false;
    }
    if (bound.first_order_transmittance < 0.0f ||
        bound.first_order_transmittance > 1.0f ||
        bound.remaining_first_order_opacity < 0.0f ||
        bound.remaining_radiance < 0.0f || current_radiance <= 0.0f ||
        current_distance_moment < 0.0f ||
        maximum_remaining_distance < 0.0f)
        return false;
    const f32 currentOpacity = 1.0f - bound.first_order_transmittance;
    f32 maximumOpacity = bound.remaining_first_order_opacity > 0.0f
        ? AddVolumetricCloudPositiveUpper_Internal(
            currentOpacity, bound.remaining_first_order_opacity)
        : currentOpacity;
    if (maximumOpacity > 1.0f) maximumOpacity = 1.0f;
    if (currentOpacity <= 1.0e-4f || maximumOpacity < currentOpacity)
        return false;
    u32 currentOpacityCode = 0u;
    u32 maximumOpacityCode = 0u;
    MemCopy(&currentOpacityCode, &currentOpacity, sizeof(currentOpacityCode));
    MemCopy(&maximumOpacityCode, &maximumOpacity, sizeof(maximumOpacityCode));
    if (currentOpacityCode != maximumOpacityCode) return false;
    const f32 currentColor = current_radiance / currentOpacity;
    const f32 minimumColor = maximumOpacity > currentOpacity
        ? DeflateVolumetricCloudPositiveLower_Internal(
            current_radiance / maximumOpacity)
        : currentColor;
    const f32 maximumColor = bound.remaining_radiance > 0.0f
        ? InflateVolumetricCloudPositiveUpper_Internal(
            AddVolumetricCloudPositiveUpper_Internal(
                current_radiance, bound.remaining_radiance) /
            currentOpacity)
        : currentColor;
    if (maximumColor > 65504.0f ||
        ResolveVolumetricCloudR16Code_Internal(currentColor) !=
            ResolveVolumetricCloudR16Code_Internal(minimumColor) ||
        ResolveVolumetricCloudR16Code_Internal(currentColor) !=
            ResolveVolumetricCloudR16Code_Internal(maximumColor) ||
        ResolveVolumetricCloudR16Code_Internal(currentOpacity) !=
            ResolveVolumetricCloudR16Code_Internal(maximumOpacity))
        return false;
    const f32 currentDepth = current_distance_moment / currentOpacity;
    const f32 maximumDepth =
        bound.remaining_first_order_opacity > 0.0f
            ? InflateVolumetricCloudPositiveUpper_Internal(
                AddVolumetricCloudPositiveUpper_Internal(
                    current_distance_moment,
                    MultiplyVolumetricCloudPositiveUpper_Internal(
                        bound.remaining_first_order_opacity,
                        maximum_remaining_distance)) /
                currentOpacity)
            : currentDepth;
    if (!CloudDensityIntegrationValueIsFinite_Internal(currentDepth) ||
        !CloudDensityIntegrationValueIsFinite_Internal(maximumDepth) ||
        currentDepth < 0.0f || maximumDepth < currentDepth ||
        maximumDepth > 250000.0f)
        return false;
    u32 currentDepthCode = 0u;
    u32 maximumDepthCode = 0u;
    MemCopy(&currentDepthCode, &currentDepth, sizeof(currentDepthCode));
    MemCopy(&maximumDepthCode, &maximumDepth, sizeof(maximumDepthCode));
    return currentDepthCode == maximumDepthCode;
}

} // namespace acs::render_internal

#endif // ACS_RENDER_VOLUMETRIC_CLOUD_DENSITY_INTEGRATION_INTERNAL_H
