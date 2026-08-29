// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_RENDER_VOLUMETRIC_CLOUD_DENSITY_INTEGRATION_INTERNAL_H
#define ACS_RENDER_VOLUMETRIC_CLOUD_DENSITY_INTEGRATION_INTERNAL_H

#include "foundation/Types.h"
#include "math/Math.h"

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

/** 符号付き凝結場の三つの空間周波数帯を混ぜる重み。 */
struct FVolumetricCloudShapeFrequencyBlendInternal {
    /** 周波数2の湿度核も担当幅で解像できない場合の横断面分布の重み。 */
    f32 unresolved = 0.0f;

    /** 周波数2の湿度核だけで作る形状の重み。 */
    f32 coarse = 0.0f;

    /** 周波数4まで含む中間形状の重み。 */
    f32 middle = 0.0f;

    /** 周波数8まで含む完成形状の重み。 */
    f32 complete = 1.0f;

};

/** 一画素内の2×2実空間位置を代表し、視線方向へ個別に積算する4本の密度レーン。 */
struct FVolumetricCloudDensityLanesInternal {
    /** 左下、右下、左上、右上の順に並ぶ実空間サブレイ密度。 */
    f32 densities[4]{};
};

/** 最粗形状を解像できない担当範囲に残る、4点求積の密度分布。 */
struct FVolumetricCloudDensityDistributionInternal {
    /** 低湿度側から高湿度側へ並ぶ、同じ物理点条件での密度状態。 */
    f32 state_densities[4]{};
};

/** 四状態の未解像密度を相関セル間で継承する条件付き生存状態。 */
struct FVolumetricCloudFourStateTransportStateInternal {
    /** 現在まで生存した光が各密度状態に属する条件付き確率。 */
    f32 conditional_survival[4]{};

    /** 現在の相関セル境界までに残っている物理距離。 */
    f32 remaining_boundary_distance = 0.0f;

    /** 現在の境界を決めた相関セルの物理長。 */
    f32 correlation_cell_length = 0.0f;

    /** 境界へ到達済みで、次の正距離入力から新しいセル長を採用するか。 */
    bool awaiting_next_cell_length = false;

    /** 初期確率を設定済みか。 */
    bool initialized = false;
};

/** 四状態有限相関媒質の一輸送区間で得た透過率と吸収位置。 */
struct FVolumetricCloudFourStateTransportIntervalInternal {
    /** 区間始点で生存している光に対する区間透過率。 */
    f32 transmittance = 1.0f;

    /** 1との差へ戻さずに積算した区間吸収率。 */
    f32 absorption = 0.0f;

    /** 区間始点から測った平均吸収距離。 */
    f32 absorption_centroid_distance = 0.0f;
};

/** 横断面4レーンへ視線方向の光学的深さを個別に積算する状態。 */
struct FVolumetricCloudLaneOpticalDepthInternal {
    /** 各横断面レーンに沿って積算済みの光学的深さ。 */
    f32 optical_depths[4]{};
};

/** 周期Perlinの勾配分布と五次補間を全セル位置で積分した最粗ポテンシャル分散。 */
inline constexpr f32 kVolumetricCloudUnresolvedCoarsePotentialVariance =
    0.0364594728f;

/** 標準正規分布へ換算した4点Gauss-Hermite求積の内側最粗ポテンシャル。 */
inline constexpr f32 kVolumetricCloudUnresolvedCoarseInnerPotential =
    0.141673264f;

/** 標準正規分布へ換算した4点Gauss-Hermite求積の外側最粗ポテンシャル。 */
inline constexpr f32 kVolumetricCloudUnresolvedCoarseOuterPotential =
    0.445741543f;

/** 4点Gauss-Hermite求積の内側各点へ割り当てる確率。 */
inline constexpr f32 kVolumetricCloudUnresolvedCoarseInnerWeight =
    0.454124145f;

/** 4点Gauss-Hermite求積の外側各点へ割り当てる確率。 */
inline constexpr f32 kVolumetricCloudUnresolvedCoarseOuterWeight =
    0.0458758548f;

/** 焼き込み後の最粗形状をX方向へずらした、正の自己相関の積分距離。 */
inline constexpr f32 kVolumetricCloudCoarseCorrelationDomainLengthX =
    0.251507194f;

/** 焼き込み後の最粗形状をY方向へずらした、正の自己相関の積分距離。 */
inline constexpr f32 kVolumetricCloudCoarseCorrelationDomainLengthY =
    0.188147797f;

/** 焼き込み後の最粗形状をZ方向へずらした、正の自己相関の積分距離。 */
inline constexpr f32 kVolumetricCloudCoarseCorrelationDomainLengthZ =
    0.197809963f;

/** 2×2実空間サブレイがそれぞれ所有する画素面積。 */
inline constexpr f32 kVolumetricCloudSubrayAreaWeight = 0.25f;

/** 画素中心から各2点Gauss-Legendre位置までの画素幅比。 */
inline constexpr f32 kVolumetricCloudSubrayGaussOffset =
    0.2886751346f;

/** 雲底の凝結補助を符号付き場へ加える倍率。 */
inline constexpr f32 kVolumetricCloudCondensationBaseSupportScale =
    0.20f;

/** 雲底補助が取り得る最大値。 */
inline constexpr f32 kVolumetricCloudCondensationMaximumBaseSupport =
    0.42f;

/** 低周波房が凝結場へ加え得る最大値。 */
inline constexpr f32 kVolumetricCloudCondensationMaximumBillowOffset =
    0.13f;

/** 高周波侵食が凝結場へ加え得る最大値。 */
inline constexpr f32 kVolumetricCloudCondensationMaximumErosionOffset =
    0.12f;

/** 天候と高さ以外の後段処理が凝結場へ加え得る最大値。 */
inline constexpr f32 kVolumetricCloudCondensationMaximumPositiveOffset =
    kVolumetricCloudCondensationMaximumBaseSupport *
        kVolumetricCloudCondensationBaseSupportScale +
    kVolumetricCloudCondensationMaximumBillowOffset +
    kVolumetricCloudCondensationMaximumErosionOffset;

/** 基本形状1へ全ての正方向変位が重なったときの最大凝結場。 */
inline constexpr f32 kVolumetricCloudCondensationMaximumPotential =
    1.0f + kVolumetricCloudCondensationMaximumPositiveOffset;

/** 正の凝結場をC1連続に立ち上げる幅。 */
inline constexpr f32 kVolumetricCloudCondensationTransitionWidth =
    0.06f;

/** 包絡が0なら、最大の形状と詳細変位を凝結境界以下へ戻す片側障壁。 */
inline constexpr f32 kVolumetricCloudCondensationEnvelopeRejection =
    1.0f + kVolumetricCloudCondensationMaximumPositiveOffset;

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
 * 実交差区間の始点と終点を保ったまま、正規化位置を実距離へ写像する。
 * 無効な始点は0、無効または逆転した終点は始点を返す。
 * @param interval_start 実交差区間の始点。
 * @param interval_end 実交差区間の終点。
 * @param fraction 区間内の正規化位置。0～1へ制限して扱う。
 * @return 単精度で表現可能な、閉区間内の実距離。
 */
inline f32 ResolveVolumetricCloudIntervalDistance_Internal(
    f32 interval_start, f32 interval_end, f32 fraction) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(interval_start))
        return 0.0f;
    if (!CloudDensityIntegrationValueIsFinite_Internal(interval_end) ||
        interval_end <= interval_start)
        return interval_start;
    if (!CloudDensityIntegrationValueIsFinite_Internal(fraction))
        fraction = 0.0f;
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    f32 distance = interval_start +
        fraction * (interval_end - interval_start);
    if (!CloudDensityIntegrationValueIsFinite_Internal(distance) ||
        distance < interval_start)
        distance = interval_start;
    if (distance > interval_end) distance = interval_end;
    return distance;
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
 * 完成形状の最高周波数が担当幅の1/4周期を越えてから中間、最粗形状へ移す。
 * 最粗形状も解像できなくなった後は、点標本ではなく解析的な密度統計へ移す。
 */
inline FVolumetricCloudShapeFrequencyBlendInternal
ResolveVolumetricCloudShapeFrequencyBlend_Internal(
    f32 maximum_domain_footprint) noexcept
{
    FVolumetricCloudShapeFrequencyBlendInternal out{};
    if (!CloudDensityIntegrationValueIsFinite_Internal(
            maximum_domain_footprint))
    {
        out.unresolved = 1.0f;
        out.coarse = 0.0f;
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
    f32 coarseTransition = (safeFootprint * 2.0f - 0.25f) * 4.0f;
    if (coarseTransition < 0.0f) coarseTransition = 0.0f;
    if (coarseTransition > 1.0f) coarseTransition = 1.0f;
    const f32 coarseVisibility = 1.0f -
        coarseTransition * coarseTransition * (3.0f - 2.0f * coarseTransition);
    out.complete = fineVisibility;
    out.middle = (1.0f - fineVisibility) * middleVisibility;
    const f32 coarseOwner =
        (1.0f - fineVisibility) * (1.0f - middleVisibility);
    out.coarse = coarseOwner * coarseVisibility;
    out.unresolved = coarseOwner * (1.0f - coarseVisibility);
    return out;
}

/** 0～1の天候または高さ包絡を、湿度不足分だけを引く片側障壁へ変換する。 */
inline f32 ResolveVolumetricCloudCondensationEnvelopeGate_Internal(
    f32 envelope) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(envelope))
        envelope = 0.0f;
    if (envelope < 0.0f) envelope = 0.0f;
    if (envelope > 1.0f) envelope = 1.0f;
    const f32 missingEnvelope = 1.0f - envelope;
    return -missingEnvelope * missingEnvelope *
        kVolumetricCloudCondensationEnvelopeRejection;
}

/**
 * 3D湿度、2D天候、高さ分布、雲底補助を一つの符号付き凝結場へ合成する。
 * 天候と高さは密度への乗算ではなく偏りとして働き、3D境界の連続性を保つ。
 */
inline f32 ResolveVolumetricCloudCondensationPotential_Internal(
    f32 shape_potential, f32 weather_envelope,
    f32 height_envelope, f32 base_support) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(shape_potential))
        shape_potential = -1.0f;
    if (shape_potential < -1.0f) shape_potential = -1.0f;
    if (shape_potential > 1.0f) shape_potential = 1.0f;
    if (!CloudDensityIntegrationValueIsFinite_Internal(base_support))
        base_support = 0.0f;
    if (base_support < 0.0f) base_support = 0.0f;
    if (base_support > 1.0f) base_support = 1.0f;
    return shape_potential +
        ResolveVolumetricCloudCondensationEnvelopeGate_Internal(
            weather_envelope) +
        ResolveVolumetricCloudCondensationEnvelopeGate_Internal(
            height_envelope) +
        base_support * kVolumetricCloudCondensationBaseSupportScale;
}

/**
 * 符号付き凝結場の0以下を厳密に空へ保ち、正側をC1連続な密度へ変換する。
 * 境界より十分内側では値を飽和させず、3D湿度の内部変化を光学密度へ残す。
 */
inline f32 ResolveVolumetricCloudCondensationDensity_Internal(
    f32 potential) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(potential))
        return 0.0f;
    constexpr f32 transitionWidth =
        kVolumetricCloudCondensationTransitionWidth;
    if (potential <= 0.0f) return 0.0f;
    if (potential >= transitionWidth) return potential;
    const f32 transition = potential / transitionWidth;
    return potential * transition * (2.0f - transition);
}

/** 凝結場の定義から導いた、後段の全密度処理に共通する最大密度。 */
inline f32 ResolveVolumetricCloudMaximumCondensationDensity_Internal()
    noexcept
{
    return ResolveVolumetricCloudCondensationDensity_Internal(
        kVolumetricCloudCondensationMaximumPotential);
}

/** 4本の横断面レーンを面積重みで平均する。 */
inline f32 ResolveVolumetricCloudDensityLaneAverage_Internal(
    const FVolumetricCloudDensityLanesInternal& lanes) noexcept
{
    f32 average = 0.0f;
    for (u32 index = 0u; index < 4u; ++index)
    {
        const f32 density = lanes.densities[index];
        if (CloudDensityIntegrationValueIsFinite_Internal(density) &&
            density > 0.0f)
            average += kVolumetricCloudSubrayAreaWeight * density;
    }
    return average;
}

/** 画素中心密度から4本の実空間サブレイ密度へ、位置対応を保って移行する。 */
inline FVolumetricCloudDensityLanesInternal
ResolveVolumetricCloudPhysicalSubrayDensityLanes_Internal(
    f32 central_density,
    const FVolumetricCloudDensityLanesInternal& physical_subrays,
    f32 blend) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(central_density) ||
        central_density < 0.0f)
        central_density = 0.0f;
    if (!CloudDensityIntegrationValueIsFinite_Internal(blend)) blend = 0.0f;
    if (blend < 0.0f) blend = 0.0f;
    if (blend > 1.0f) blend = 1.0f;
    FVolumetricCloudDensityLanesInternal out{};
    for (u32 index = 0u; index < 4u; ++index)
    {
        f32 physicalDensity = physical_subrays.densities[index];
        if (!CloudDensityIntegrationValueIsFinite_Internal(physicalDensity) ||
            physicalDensity < 0.0f)
            physicalDensity = 0.0f;
        out.densities[index] = central_density +
            (physicalDensity - central_density) * blend;
    }
    return out;
}

/** 密度倍率を4本の横断面レーンへ同じ回数だけ適用する。 */
inline FVolumetricCloudDensityLanesInternal
ScaleVolumetricCloudDensityLanes_Internal(
    FVolumetricCloudDensityLanesInternal lanes,
    f32 density_scale) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(density_scale) ||
        density_scale <= 0.0f)
        return FVolumetricCloudDensityLanesInternal{};
    for (u32 index = 0u; index < 4u; ++index)
        lanes.densities[index] *= density_scale;
    return lanes;
}

/** 二つの横断面密度を、各レーンの対応を保ったまま連続補間する。 */
inline FVolumetricCloudDensityLanesInternal
LerpVolumetricCloudDensityLanes_Internal(
    const FVolumetricCloudDensityLanesInternal& from,
    const FVolumetricCloudDensityLanesInternal& to,
    f32 weight) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(weight)) weight = 0.0f;
    if (weight < 0.0f) weight = 0.0f;
    if (weight > 1.0f) weight = 1.0f;
    FVolumetricCloudDensityLanesInternal out{};
    for (u32 index = 0u; index < 4u; ++index)
        out.densities[index] = from.densities[index] +
            (to.densities[index] - from.densities[index]) * weight;
    return out;
}

/** 最粗形状の未解像ポテンシャルを正値化し、4点求積の密度分布を返す。 */
inline FVolumetricCloudDensityDistributionInternal
ResolveVolumetricCloudUnresolvedCoarseDensityDistribution_Internal(
    f32 common_potential, f32 detail_potential) noexcept
{
    const f32 offset = common_potential + detail_potential;
    FVolumetricCloudDensityDistributionInternal out{};
    out.state_densities[0] =
        ResolveVolumetricCloudCondensationDensity_Internal(
            offset - kVolumetricCloudUnresolvedCoarseOuterPotential);
    out.state_densities[1] =
        ResolveVolumetricCloudCondensationDensity_Internal(
            offset - kVolumetricCloudUnresolvedCoarseInnerPotential);
    out.state_densities[2] =
        ResolveVolumetricCloudCondensationDensity_Internal(
            offset + kVolumetricCloudUnresolvedCoarseInnerPotential);
    out.state_densities[3] =
        ResolveVolumetricCloudCondensationDensity_Internal(
            offset + kVolumetricCloudUnresolvedCoarseOuterPotential);
    return out;
}

/** 4点Gauss-Hermite密度分布の確率平均を返す。 */
inline f32 ResolveVolumetricCloudDensityDistributionMean_Internal(
    const FVolumetricCloudDensityDistributionInternal& distribution) noexcept
{
    const f32 weights[4]{
        kVolumetricCloudUnresolvedCoarseOuterWeight,
        kVolumetricCloudUnresolvedCoarseInnerWeight,
        kVolumetricCloudUnresolvedCoarseInnerWeight,
        kVolumetricCloudUnresolvedCoarseOuterWeight};
    f32 meanDensity = 0.0f;
    for (u32 index = 0u; index < 4u; ++index)
    {
        const f32 density = distribution.state_densities[index];
        if (CloudDensityIntegrationValueIsFinite_Internal(density) &&
            density > 0.0f)
            meanDensity += weights[index] * density;
    }
    return meanDensity;
}

/**
 * 最粗形状の未解像分布を4点Gauss-Hermite求積し、局所面積の平均密度を返す。
 * 互換用の密度値であり、Beer-Lambert輸送では分布の透過率期待値を使う。
 */
inline f32 ResolveVolumetricCloudUnresolvedCoarseMeanDensity_Internal(
    f32 common_potential, f32 detail_potential) noexcept
{
    return ResolveVolumetricCloudDensityDistributionMean_Internal(
        ResolveVolumetricCloudUnresolvedCoarseDensityDistribution_Internal(
            common_potential, detail_potential));
}

/**
 * 解像済み周波数帯と未解像最粗形状を同じ密度状態へ合成する。
 * 未解像重みが0なら四状態は一致し、通常の均質密度へ厳密に戻る。
 */
inline FVolumetricCloudDensityDistributionInternal
ResolveVolumetricCloudBandDensityDistribution_Internal(
    f32 coarse_potential, f32 middle_potential,
    f32 complete_potential,
    const FVolumetricCloudShapeFrequencyBlendInternal& blend,
    f32 common_potential, f32 detail_potential) noexcept
{
    const f32 offset = common_potential + detail_potential;
    const auto positiveWeight = [](f32 weight) noexcept {
        return CloudDensityIntegrationValueIsFinite_Internal(weight) &&
            weight > 0.0f ? weight : 0.0f;
    };
    const f32 pointDensity =
        positiveWeight(blend.coarse) *
            ResolveVolumetricCloudCondensationDensity_Internal(
                coarse_potential + offset) +
        positiveWeight(blend.middle) *
            ResolveVolumetricCloudCondensationDensity_Internal(
                middle_potential + offset) +
        positiveWeight(blend.complete) *
            ResolveVolumetricCloudCondensationDensity_Internal(
                complete_potential + offset);
    const f32 unresolvedWeight = positiveWeight(blend.unresolved);
    const auto unresolved =
        ResolveVolumetricCloudUnresolvedCoarseDensityDistribution_Internal(
            common_potential, detail_potential);
    FVolumetricCloudDensityDistributionInternal out{};
    for (u32 index = 0u; index < 4u; ++index)
        out.state_densities[index] = pointDensity +
            unresolvedWeight * unresolved.state_densities[index];
    return out;
}

/**
 * 焼き込み形状の三軸自己相関と、回転後の物質座標方向から物理相関距離を返す。
 * domain_directionはshape_scaleを掛ける前の、一メートル当たりの座標変化を渡す。
 */
inline f32 ResolveVolumetricCloudShapeCorrelationLength_Internal(
    f32 shape_scale, f32 domain_direction_x,
    f32 domain_direction_y, f32 domain_direction_z) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(shape_scale) ||
        shape_scale <= 0.0f)
        return 0.0f;
    const f32 scaledX = domain_direction_x /
        kVolumetricCloudCoarseCorrelationDomainLengthX;
    const f32 scaledY = domain_direction_y /
        kVolumetricCloudCoarseCorrelationDomainLengthY;
    const f32 scaledZ = domain_direction_z /
        kVolumetricCloudCoarseCorrelationDomainLengthZ;
    const f32 inverseDomainDistance = Sqrt(
        scaledX * scaledX + scaledY * scaledY + scaledZ * scaledZ);
    if (!CloudDensityIntegrationValueIsFinite_Internal(inverseDomainDistance) ||
        inverseDomainDistance <= 0.0f)
        return 0.0f;
    return 1.0f / (shape_scale * inverseDomainDistance);
}

/** 1からの減少量を、薄い媒質でも桁落ちさせず光学的深さへ戻す。 */
inline f32 ResolveVolumetricCloudOpticalDepthFromAbsorption_Internal(
    f32 absorption) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(absorption) ||
        absorption <= 0.0f)
        return 0.0f;
    if (absorption >= 1.0f) return 80.0f;
    if (absorption <= 0.125f)
    {
        const f32 squared = absorption * absorption;
        const f32 cubed = squared * absorption;
        const f32 fourth = squared * squared;
        const f32 fifth = fourth * absorption;
        const f32 sixth = cubed * cubed;
        return absorption + squared * 0.5f + cubed / 3.0f +
            fourth * 0.25f + fifth * 0.2f + sixth / 6.0f;
    }
    const f32 transmittance = 1.0f - absorption;
    const f32 opticalDepth = -Log(transmittance);
    if (!CloudDensityIntegrationValueIsFinite_Internal(opticalDepth) ||
        opticalDepth < 0.0f)
        return 80.0f;
    return opticalDepth < 80.0f ? opticalDepth : 80.0f;
}

/**
 * 一つの相関ブロック全長における透過率期待値から、均質な有効消散率を求める。
 * 任意長の相関を決める式ではないため、相関ブロックより短い光路へ流用しない。
 */
inline f32 ResolveVolumetricCloudEffectiveExtinction_Internal(
    const FVolumetricCloudDensityDistributionInternal& distribution,
    f32 extinction_per_density, f32 correlation_length) noexcept
{
    constexpr f32 maximumFiniteValue = 3.402823466e+38F;
    if (extinction_per_density != extinction_per_density ||
        extinction_per_density <= 0.0f)
        return 0.0f;
    const f32 meanDensity =
        ResolveVolumetricCloudDensityDistributionMean_Internal(distribution);
    if (!CloudDensityIntegrationValueIsFinite_Internal(correlation_length) ||
        correlation_length <= 0.0f)
    {
        if (extinction_per_density > maximumFiniteValue ||
            meanDensity > maximumFiniteValue / extinction_per_density)
            return meanDensity > 0.0f ? maximumFiniteValue : 0.0f;
        return meanDensity * extinction_per_density;
    }
    const f32 weights[4]{
        kVolumetricCloudUnresolvedCoarseOuterWeight,
        kVolumetricCloudUnresolvedCoarseInnerWeight,
        kVolumetricCloudUnresolvedCoarseInnerWeight,
        kVolumetricCloudUnresolvedCoarseOuterWeight};
    f32 opticalDepths[4]{};
    f32 minimumOpticalDepth = maximumFiniteValue;
    for (u32 index = 0u; index < 4u; ++index)
    {
        f32 density = distribution.state_densities[index];
        if (!CloudDensityIntegrationValueIsFinite_Internal(density) ||
            density <= 0.0f)
            density = 0.0f;
        f32 opticalDepth = maximumFiniteValue;
        if (density <= 0.0f)
            opticalDepth = 0.0f;
        else if (extinction_per_density <= maximumFiniteValue &&
                 density <= maximumFiniteValue / extinction_per_density)
        {
            const f32 extinction = density * extinction_per_density;
            if (extinction <= maximumFiniteValue / correlation_length)
                opticalDepth = extinction * correlation_length;
        }
        opticalDepths[index] = opticalDepth;
        if (opticalDepth < minimumOpticalDepth)
            minimumOpticalDepth = opticalDepth;
    }
    f32 scaledTransmittance = 0.0f;
    for (u32 index = 0u; index < 4u; ++index)
    {
        const f32 relativeDepth =
            opticalDepths[index] - minimumOpticalDepth;
        scaledTransmittance += weights[index] *
            Exp(-(relativeDepth > 0.0f ? relativeDepth : 0.0f));
    }
    if (!CloudDensityIntegrationValueIsFinite_Internal(scaledTransmittance) ||
        scaledTransmittance <= 0.0f)
        return maximumFiniteValue;
    const f32 correlationOpticalDepth =
        minimumOpticalDepth - Log(scaledTransmittance);
    if (!CloudDensityIntegrationValueIsFinite_Internal(correlationOpticalDepth) ||
        correlationOpticalDepth < 0.0f)
        return maximumFiniteValue;
    return correlationOpticalDepth / correlation_length;
}

/**
 * 密度状態が光路中で変わらない一相関ブロック内の透過率期待値を求める。
 * 相関ブロック全長を均質化した値の平方根とは一般に一致しない。
 */
inline f32 ResolveVolumetricCloudPersistentStateTransmittance_Internal(
    const FVolumetricCloudDensityDistributionInternal& distribution,
    f32 extinction_per_density, f32 path_length) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(extinction_per_density) ||
        !CloudDensityIntegrationValueIsFinite_Internal(path_length) ||
        extinction_per_density <= 0.0f || path_length <= 0.0f)
        return 1.0f;
    const f32 weights[4]{
        kVolumetricCloudUnresolvedCoarseOuterWeight,
        kVolumetricCloudUnresolvedCoarseInnerWeight,
        kVolumetricCloudUnresolvedCoarseInnerWeight,
        kVolumetricCloudUnresolvedCoarseOuterWeight};
    f32 transmittance = 0.0f;
    for (u32 index = 0u; index < 4u; ++index)
    {
        f32 density = distribution.state_densities[index];
        if (!CloudDensityIntegrationValueIsFinite_Internal(density) ||
            density <= 0.0f)
            density = 0.0f;
        f32 opticalDepth = density * extinction_per_density * path_length;
        if (!CloudDensityIntegrationValueIsFinite_Internal(opticalDepth))
            opticalDepth = density > 0.0f ? 80.0f : 0.0f;
        transmittance += weights[index] *
            Exp(-(opticalDepth < 80.0f ? opticalDepth : 80.0f));
    }
    return transmittance > 0.0f ? transmittance : 0.0f;
}

/** 各周波数帯を正値化し、未解像終端を局所面積の平均密度へ移す。 */
inline FVolumetricCloudDensityLanesInternal
ResolveVolumetricCloudBandDensityLanes_Internal(
    f32 coarse_potential, f32 middle_potential,
    f32 complete_potential,
    const FVolumetricCloudShapeFrequencyBlendInternal& blend,
    f32 common_potential, f32 detail_potential) noexcept
{
    const auto distribution =
        ResolveVolumetricCloudBandDensityDistribution_Internal(
            coarse_potential, middle_potential, complete_potential,
            blend, common_potential, detail_potential);
    const f32 meanDensity =
        ResolveVolumetricCloudDensityDistributionMean_Internal(distribution);
    FVolumetricCloudDensityLanesInternal out{};
    for (u32 index = 0u; index < 4u; ++index)
        out.densities[index] = meanDensity;
    return out;
}

/** 従来のスカラー利用箇所へ、4本の横断面レーンの面積平均密度を返す。 */
inline f32 ResolveVolumetricCloudBandDensity_Internal(
    f32 coarse_potential, f32 middle_potential,
    f32 complete_potential,
    const FVolumetricCloudShapeFrequencyBlendInternal& blend,
    f32 common_potential, f32 detail_potential) noexcept
{
    return ResolveVolumetricCloudDensityLaneAverage_Internal(
        ResolveVolumetricCloudBandDensityLanes_Internal(
            coarse_potential, middle_potential, complete_potential,
            blend, common_potential, detail_potential));
}

/**
 * 探索セルに点形状と未解像分布が作り得る最大密度を返す。
 * 各形状を正値化してから担当重みを掛け、非線形変換前の平均で上限を失わない。
 */
inline f32 ResolveVolumetricCloudOccupancyDensityUpperBound_Internal(
    f32 maximum_shape_potential,
    const FVolumetricCloudShapeFrequencyBlendInternal& blend,
    f32 maximum_positive_offset) noexcept
{
    const auto positiveWeight = [](f32 weight) noexcept {
        return CloudDensityIntegrationValueIsFinite_Internal(weight) &&
            weight > 0.0f ? weight : 0.0f;
    };
    if (!CloudDensityIntegrationValueIsFinite_Internal(
            maximum_shape_potential))
        maximum_shape_potential = -1.0f;
    if (!CloudDensityIntegrationValueIsFinite_Internal(
            maximum_positive_offset))
        maximum_positive_offset = 0.0f;
    const f32 pointOwner = positiveWeight(blend.coarse) +
        positiveWeight(blend.middle) +
        positiveWeight(blend.complete);
    const f32 pointDensity =
        ResolveVolumetricCloudCondensationDensity_Internal(
            maximum_shape_potential + maximum_positive_offset);
    const f32 unresolvedDensity =
        ResolveVolumetricCloudCondensationDensity_Internal(
            kVolumetricCloudUnresolvedCoarseOuterPotential +
            maximum_positive_offset);
    return pointOwner * pointDensity +
        positiveWeight(blend.unresolved) * unresolvedDensity;
}

/** 密度レーンを視線方向へ足し、横断面の対応を区間境界で入れ替えない。 */
inline void AccumulateVolumetricCloudLaneOpticalDepth_Internal(
    FVolumetricCloudLaneOpticalDepthInternal& state,
    const FVolumetricCloudDensityLanesInternal& lanes,
    f32 optical_scale) noexcept
{
    constexpr f32 maximumFiniteValue = 3.402823466e+38F;
    if (optical_scale != optical_scale || optical_scale <= 0.0f) return;
    const bool infiniteScale = optical_scale > maximumFiniteValue;
    for (u32 index = 0u; index < 4u; ++index)
    {
        const f32 density = lanes.densities[index];
        if (!CloudDensityIntegrationValueIsFinite_Internal(density) ||
            density <= 0.0f)
            continue;
        f32 contribution = maximumFiniteValue;
        if (!infiniteScale && density <= maximumFiniteValue / optical_scale)
            contribution = density * optical_scale;
        const f32 previous = state.optical_depths[index];
        if (previous >= maximumFiniteValue - contribution)
            state.optical_depths[index] = maximumFiniteValue;
        else
            state.optical_depths[index] = previous + contribution;
    }
}

/** 積算済みの4光路を個別に指数変換してから面積平均する。 */
inline f32 ResolveVolumetricCloudLaneTransmittance_Internal(
    const FVolumetricCloudLaneOpticalDepthInternal& state) noexcept
{
    f32 transmittance = 0.0f;
    for (u32 index = 0u; index < 4u; ++index)
    {
        const f32 depth = state.optical_depths[index];
        f32 laneTransmittance = 0.0f;
        if (depth == depth && depth <= 0.0f)
            laneTransmittance = 1.0f;
        else if (CloudDensityIntegrationValueIsFinite_Internal(depth))
            laneTransmittance = Exp(-(depth < 80.0f ? depth : 80.0f));
        transmittance +=
            kVolumetricCloudSubrayAreaWeight * laneTransmittance;
    }
    if (transmittance < 0.0f) transmittance = 0.0f;
    if (transmittance > 1.0f) transmittance = 1.0f;
    return transmittance;
}

/**
 * 太陽円盤4方向のキャッシュ透過率へ、方向ごとの光学的厚さ残差を適用して平均する。
 * 一つの中心方向残差を全方向へ流用せず、雲縁を横切る光路差を保持する。
 */
inline f32 ResolveVolumetricCloudDirectionalResidualAverage_Internal(
    const f32 cached_transmittances[4],
    const f32 optical_depth_residuals[4]) noexcept
{
    f32 average = 0.0f;
    for (u32 directionIndex = 0u; directionIndex < 4u; ++directionIndex)
    {
        f32 cachedTransmittance = cached_transmittances[directionIndex];
        if (!CloudDensityIntegrationValueIsFinite_Internal(cachedTransmittance))
            cachedTransmittance = 0.0f;
        if (cachedTransmittance < 0.0f) cachedTransmittance = 0.0f;
        if (cachedTransmittance > 1.0f) cachedTransmittance = 1.0f;

        f32 residual = optical_depth_residuals[directionIndex];
        if (!CloudDensityIntegrationValueIsFinite_Internal(residual))
            residual = 0.0f;
        f32 exponent = -residual;
        if (exponent < -16.0f) exponent = -16.0f;
        if (exponent > 16.0f) exponent = 16.0f;
        f32 correctedTransmittance = cachedTransmittance * Exp(exponent);
        if (correctedTransmittance > 1.0f) correctedTransmittance = 1.0f;
        average += kVolumetricCloudSubrayAreaWeight * correctedTransmittance;
    }
    if (average < 0.0f) average = 0.0f;
    if (average > 1.0f) average = 1.0f;
    return average;
}

/**
 * 房なし・二段階の房・侵食有無を各レーンで正値化してから連続補間する。
 * 密度0を跨いでもレーン数を変えず、透過率を不連続にしない。
 */
inline FVolumetricCloudDensityLanesInternal
ResolveVolumetricCloudDetailFilteredBandDensityLanes_Internal(
    f32 coarse_potential, f32 middle_potential,
    f32 complete_potential,
    const FVolumetricCloudShapeFrequencyBlendInternal& blend,
    f32 common_potential,
    f32 coarse_billow_potential,
    f32 resolved_billow_potential,
    f32 billow_visibility,
    f32 middle_billow_visibility,
    f32 erosion_potential,
    f32 erosion_visibility) noexcept
{
    const auto sanitizeVisibility = [](f32 visibility) noexcept {
        if (!CloudDensityIntegrationValueIsFinite_Internal(visibility))
            return 0.0f;
        if (visibility < 0.0f) return 0.0f;
        if (visibility > 1.0f) return 1.0f;
        return visibility;
    };
    const f32 safeBillowVisibility = sanitizeVisibility(billow_visibility);
    const f32 safeMiddleBillowVisibility =
        sanitizeVisibility(middle_billow_visibility);
    const f32 safeErosionVisibility = sanitizeVisibility(erosion_visibility);
    const auto densityLanes = [&](f32 detailPotential) noexcept {
        return ResolveVolumetricCloudBandDensityLanes_Internal(
            coarse_potential, middle_potential, complete_potential,
            blend, common_potential, detailPotential);
    };

    const auto baseDensity = densityLanes(0.0f);
    const auto coarseBillowDensity = densityLanes(coarse_billow_potential);
    const auto resolvedBillowDensity = densityLanes(resolved_billow_potential);
    const auto billowDensity = LerpVolumetricCloudDensityLanes_Internal(
        coarseBillowDensity, resolvedBillowDensity,
        safeMiddleBillowVisibility);
    const auto withoutErosionDensity =
        LerpVolumetricCloudDensityLanes_Internal(
            baseDensity, billowDensity, safeBillowVisibility);

    const auto erosionDensity = densityLanes(erosion_potential);
    const auto coarseBillowErosionDensity = densityLanes(
        coarse_billow_potential + erosion_potential);
    const auto resolvedBillowErosionDensity = densityLanes(
        resolved_billow_potential + erosion_potential);
    const auto billowErosionDensity =
        LerpVolumetricCloudDensityLanes_Internal(
            coarseBillowErosionDensity, resolvedBillowErosionDensity,
            safeMiddleBillowVisibility);
    const auto withErosionDensity =
        LerpVolumetricCloudDensityLanes_Internal(
            erosionDensity, billowErosionDensity, safeBillowVisibility);
    return LerpVolumetricCloudDensityLanes_Internal(
        withoutErosionDensity, withErosionDensity,
        safeErosionVisibility);
}

/** 従来のスカラー利用箇所へ4本の横断面密度の面積平均を返す。 */
inline f32 ResolveVolumetricCloudDetailFilteredBandDensity_Internal(
    f32 coarse_potential, f32 middle_potential,
    f32 complete_potential,
    const FVolumetricCloudShapeFrequencyBlendInternal& blend,
    f32 common_potential,
    f32 coarse_billow_potential,
    f32 resolved_billow_potential,
    f32 billow_visibility,
    f32 middle_billow_visibility,
    f32 erosion_potential,
    f32 erosion_visibility) noexcept
{
    return ResolveVolumetricCloudDensityLaneAverage_Internal(
        ResolveVolumetricCloudDetailFilteredBandDensityLanes_Internal(
            coarse_potential, middle_potential, complete_potential,
            blend, common_potential,
            coarse_billow_potential, resolved_billow_potential,
            billow_visibility, middle_billow_visibility,
            erosion_potential, erosion_visibility));
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

/** 四状態輸送を未解像形状の定常確率へ戻し、物理境界を未設定にする。 */
inline void ResetVolumetricCloudFourStateTransport_Internal(
    FVolumetricCloudFourStateTransportStateInternal& state) noexcept
{
    state.conditional_survival[0] =
        kVolumetricCloudUnresolvedCoarseOuterWeight;
    state.conditional_survival[1] =
        kVolumetricCloudUnresolvedCoarseInnerWeight;
    state.conditional_survival[2] =
        kVolumetricCloudUnresolvedCoarseInnerWeight;
    state.conditional_survival[3] =
        kVolumetricCloudUnresolvedCoarseOuterWeight;
    state.remaining_boundary_distance = 0.0f;
    state.correlation_cell_length = 0.0f;
    state.awaiting_next_cell_length = false;
    state.initialized = true;
}

/** 一つの相関セル内で四状態を更新した結果。 */
struct FVolumetricCloudFourStateChunkInternal
{
    /** 区間始点の条件付き生存光に対する透過率。 */
    f32 transmittance = 1.0f;

    /** 極薄区間でも1との差へ丸めず保持する吸収率。 */
    f32 absorption = 0.0f;

    /** 区間始点から測った、吸収量を掛けた一次モーメント。 */
    f32 absorption_moment = 0.0f;

    /** 区間終端で生存した光の条件付き状態確率。 */
    f32 conditional_survival[4]{};
};

/**
 * 相関セルを跨がない区間で、四つの密度状態をBeer-Lambert積分する。
 * 状態ごとの指数平均を保ち、平均と分散だけの縮約は行わない。
 */
inline FVolumetricCloudFourStateChunkInternal
ResolveVolumetricCloudFourStateChunk_Internal(
    const FVolumetricCloudDensityDistributionInternal& distribution,
    const f32 requested_conditional_survival[4],
    f32 extinction_per_density, f32 segment_length) noexcept
{
    FVolumetricCloudFourStateChunkInternal out{};
    const f32 stationaryWeights[4]{
        kVolumetricCloudUnresolvedCoarseOuterWeight,
        kVolumetricCloudUnresolvedCoarseInnerWeight,
        kVolumetricCloudUnresolvedCoarseInnerWeight,
        kVolumetricCloudUnresolvedCoarseOuterWeight};
    f32 conditionalWeights[4]{};
    f32 weightSum = 0.0f;
    for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex)
    {
        f32 weight = requested_conditional_survival[stateIndex];
        if (!CloudDensityIntegrationValueIsFinite_Internal(weight) ||
            weight < 0.0f)
            weight = 0.0f;
        conditionalWeights[stateIndex] = weight;
        weightSum += weight;
    }
    if (!CloudDensityIntegrationValueIsFinite_Internal(weightSum) ||
        weightSum <= 1.0e-30f)
    {
        weightSum = 1.0f;
        for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex)
            conditionalWeights[stateIndex] = stationaryWeights[stateIndex];
    }
    const f32 inverseWeightSum = 1.0f / weightSum;
    for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex)
        conditionalWeights[stateIndex] *= inverseWeightSum;

    if (!CloudDensityIntegrationValueIsFinite_Internal(segment_length) ||
        segment_length <= 0.0f ||
        !CloudDensityIntegrationValueIsFinite_Internal(extinction_per_density) ||
        extinction_per_density <= 0.0f)
    {
        for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex)
            out.conditional_survival[stateIndex] =
                conditionalWeights[stateIndex];
        return out;
    }

    out.transmittance = 0.0f;
    for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex)
    {
        f32 density = distribution.state_densities[stateIndex];
        if (!CloudDensityIntegrationValueIsFinite_Internal(density) ||
            density <= 0.0f)
            density = 0.0f;
        f32 opticalDepth = density * extinction_per_density * segment_length;
        if (!CloudDensityIntegrationValueIsFinite_Internal(opticalDepth) ||
            opticalDepth < 0.0f)
            opticalDepth = density > 0.0f ? 3.402823466e+38F : 0.0f;
        const f32 absorption =
            ResolveVolumetricCloudBeerAbsorptionFraction_Internal(opticalDepth);
        const f32 stateTransmittance = opticalDepth <= 0.125f
            ? 1.0f - absorption : Exp(-opticalDepth);
        const f32 weightedTransmittance =
            conditionalWeights[stateIndex] * stateTransmittance;
        out.conditional_survival[stateIndex] = weightedTransmittance;
        out.transmittance += weightedTransmittance;
        out.absorption += conditionalWeights[stateIndex] * absorption;
        out.absorption_moment += conditionalWeights[stateIndex] *
            absorption * segment_length *
            ResolveVolumetricCloudBeerAbsorptionCentroidFromTransmittance_Internal(
                opticalDepth, stateTransmittance);
    }
    if (!CloudDensityIntegrationValueIsFinite_Internal(out.transmittance) ||
        out.transmittance < 0.0f)
        out.transmittance = 0.0f;
    if (out.transmittance > 1.0f) out.transmittance = 1.0f;
    if (!CloudDensityIntegrationValueIsFinite_Internal(out.absorption) ||
        out.absorption < 0.0f)
        out.absorption = 0.0f;
    if (out.absorption > 1.0f) out.absorption = 1.0f;
    if (out.transmittance > 1.0e-30f)
    {
        const f32 inverseTransmittance = 1.0f / out.transmittance;
        for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex)
            out.conditional_survival[stateIndex] *= inverseTransmittance;
    }
    else
    {
        for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex)
            out.conditional_survival[stateIndex] = stationaryWeights[stateIndex];
    }
    return out;
}

/**
 * 実測した相関積分距離を持つ四状態媒質を、継承状態ごと一輸送区間進める。
 * 一セル幅を相関積分距離の二倍にすると、無作為位相の区分一定場が持つ
 * 三角自己相関の積分値と一致する。完全な相関セルは指数平均の等比列でまとめる。
 */
inline FVolumetricCloudFourStateTransportIntervalInternal
ResolveVolumetricCloudFourStateTransportInterval_Internal(
    const FVolumetricCloudDensityDistributionInternal& distribution,
    f32 extinction_per_density, f32 correlation_length,
    f32 segment_length,
    FVolumetricCloudFourStateTransportStateInternal& state) noexcept
{
    FVolumetricCloudFourStateTransportIntervalInternal out{};
    if (!CloudDensityIntegrationValueIsFinite_Internal(segment_length) ||
        segment_length <= 0.0f ||
        !CloudDensityIntegrationValueIsFinite_Internal(extinction_per_density) ||
        extinction_per_density <= 0.0f)
        return out;
    if (!state.initialized)
        ResetVolumetricCloudFourStateTransport_Internal(state);

    if (!CloudDensityIntegrationValueIsFinite_Internal(correlation_length) ||
        correlation_length <= 0.0f)
    {
        const f32 meanDensity =
            ResolveVolumetricCloudDensityDistributionMean_Internal(distribution);
        f32 opticalDepth =
            meanDensity * extinction_per_density * segment_length;
        if (!CloudDensityIntegrationValueIsFinite_Internal(opticalDepth) ||
            opticalDepth < 0.0f)
            opticalDepth = meanDensity > 0.0f ? 3.402823466e+38F : 0.0f;
        const f32 absorption =
            ResolveVolumetricCloudBeerAbsorptionFraction_Internal(opticalDepth);
        out.absorption = absorption;
        out.transmittance = opticalDepth <= 0.125f
            ? 1.0f - absorption : Exp(-opticalDepth);
        out.absorption_centroid_distance = segment_length *
            ResolveVolumetricCloudBeerAbsorptionCentroidFromTransmittance_Internal(
                opticalDepth, out.transmittance);
        ResetVolumetricCloudFourStateTransport_Internal(state);
        return out;
    }

    f32 cellLength = 2.0f * correlation_length;
    if (!CloudDensityIntegrationValueIsFinite_Internal(cellLength) ||
        cellLength <= 0.0f)
        cellLength = 3.402823466e+38F;
    f32 distanceToBoundary = state.remaining_boundary_distance;
    const f32 stateCellLength = state.correlation_cell_length;
    const bool beginsPendingCell = state.awaiting_next_cell_length;
    const bool validBoundaryState = !beginsPendingCell &&
        CloudDensityIntegrationValueIsFinite_Internal(distanceToBoundary) &&
        distanceToBoundary > 0.0f &&
        CloudDensityIntegrationValueIsFinite_Internal(stateCellLength) &&
        stateCellLength > 0.0f &&
        distanceToBoundary <= stateCellLength;
    if (beginsPendingCell)
    {
        state.correlation_cell_length = cellLength;
        distanceToBoundary = cellLength;
        state.remaining_boundary_distance = distanceToBoundary;
        state.awaiting_next_cell_length = false;
    }
    else if (!validBoundaryState)
    {
        state.correlation_cell_length = cellLength;
        distanceToBoundary = 0.5f * cellLength;
        state.remaining_boundary_distance = distanceToBoundary;
        state.awaiting_next_cell_length = false;
    }

    f32 remainingLength = segment_length;
    f32 traversedLength = 0.0f;
    f32 pathTransmittance = 1.0f;
    f32 pathAbsorption = 0.0f;
    f32 pathAbsorptionMoment = 0.0f;
    const bool reachedFirstBoundary = remainingLength >= distanceToBoundary;
    const f32 firstLength = reachedFirstBoundary
        ? distanceToBoundary : remainingLength;
    const auto firstChunk = ResolveVolumetricCloudFourStateChunk_Internal(
        distribution, state.conditional_survival,
        extinction_per_density, firstLength);
    const f32 firstAbsorption = firstChunk.absorption;
    pathAbsorptionMoment += traversedLength * firstAbsorption +
        firstChunk.absorption_moment;
    pathAbsorption += firstAbsorption;
    pathTransmittance *= firstChunk.transmittance;
    traversedLength += firstLength;
    remainingLength -= firstLength;
    if (remainingLength < 0.0f) remainingLength = 0.0f;
    for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex)
        state.conditional_survival[stateIndex] =
            firstChunk.conditional_survival[stateIndex];

    if (!reachedFirstBoundary)
    {
        state.remaining_boundary_distance =
            distanceToBoundary - firstLength;
    }
    else
    {
        ResetVolumetricCloudFourStateTransport_Internal(state);
        if (remainingLength > 0.0f)
        {
            state.correlation_cell_length = cellLength;
            state.remaining_boundary_distance = cellLength;
        }
        else
            state.awaiting_next_cell_length = true;
    }

    if (reachedFirstBoundary && remainingLength > 0.0f)
    {
        f32 fullCellCount = Floor(remainingLength / cellLength);
        if (fullCellCount < 0.0f) fullCellCount = 0.0f;
        const f32 fullCellsLength = fullCellCount * cellLength;
        if (fullCellCount >= 1.0f && fullCellsLength > 0.0f)
        {
            FVolumetricCloudFourStateTransportStateInternal cellState{};
            ResetVolumetricCloudFourStateTransport_Internal(cellState);
            const auto cellChunk =
                ResolveVolumetricCloudFourStateChunk_Internal(
                    distribution, cellState.conditional_survival,
                    extinction_per_density, cellLength);
            const f32 cellAbsorption = cellChunk.absorption;
            f32 cellOpticalDepth =
                ResolveVolumetricCloudOpticalDepthFromAbsorption_Internal(
                    cellAbsorption);
            f32 fullOpticalDepth = cellOpticalDepth * fullCellCount;
            if (!CloudDensityIntegrationValueIsFinite_Internal(
                    fullOpticalDepth) || fullOpticalDepth < 0.0f)
                fullOpticalDepth = 3.402823466e+38F;
            const f32 fullCellsAbsorption =
                ResolveVolumetricCloudBeerAbsorptionFraction_Internal(
                    fullOpticalDepth);
            const f32 fullCellsTransmittance = fullOpticalDepth <= 0.125f
                ? 1.0f - fullCellsAbsorption : Exp(-fullOpticalDepth);
            f32 fullCellsMoment = 0.0f;
            if (cellAbsorption > 1.0e-20f &&
                fullCellsAbsorption > 0.0f)
            {
                f32 meanCellIndex = 0.0f;
                if (fullOpticalDepth <= 1.0f)
                {
                    const f32 inverseCount = 1.0f / fullCellCount;
                    const f32 inverseCountSquared =
                        inverseCount * inverseCount;
                    const f32 inverseCountFourth =
                        inverseCountSquared * inverseCountSquared;
                    const f32 inverseCountSixth =
                        inverseCountFourth * inverseCountSquared;
                    const f32 inverseCountEighth =
                        inverseCountFourth * inverseCountFourth;
                    const f32 depthSquared =
                        fullOpticalDepth * fullOpticalDepth;
                    const f32 depthCubed =
                        depthSquared * fullOpticalDepth;
                    const f32 depthFifth =
                        depthCubed * depthSquared;
                    const f32 depthSeventh =
                        depthFifth * depthSquared;
                    meanCellIndex = fullCellCount *
                        (0.5f * (1.0f - inverseCount) -
                         fullOpticalDepth *
                             (1.0f - inverseCountSquared) / 12.0f +
                         depthCubed *
                             (1.0f - inverseCountFourth) / 720.0f -
                         depthFifth *
                             (1.0f - inverseCountSixth) / 30240.0f +
                         depthSeventh *
                             (1.0f - inverseCountEighth) / 1209600.0f);
                }
                else
                {
                    meanCellIndex =
                        cellChunk.transmittance / cellAbsorption -
                        fullCellCount * fullCellsTransmittance /
                            fullCellsAbsorption;
                }
                if (!CloudDensityIntegrationValueIsFinite_Internal(
                        meanCellIndex) || meanCellIndex < 0.0f)
                    meanCellIndex = 0.0f;
                if (meanCellIndex > fullCellCount - 1.0f)
                    meanCellIndex = fullCellCount - 1.0f;
                f32 withinCellCentroid =
                    cellChunk.absorption_moment / cellAbsorption;
                if (!CloudDensityIntegrationValueIsFinite_Internal(
                        withinCellCentroid) || withinCellCentroid < 0.0f)
                    withinCellCentroid = 0.5f * cellLength;
                if (withinCellCentroid > cellLength)
                    withinCellCentroid = cellLength;
                fullCellsMoment = fullCellsAbsorption *
                    (cellLength * meanCellIndex + withinCellCentroid);
            }
            pathAbsorptionMoment += pathTransmittance *
                (traversedLength * fullCellsAbsorption + fullCellsMoment);
            pathAbsorption += pathTransmittance * fullCellsAbsorption;
            pathTransmittance *= fullCellsTransmittance;
            traversedLength += fullCellsLength;
            remainingLength -= fullCellsLength;
            if (remainingLength < 0.0f) remainingLength = 0.0f;
            ResetVolumetricCloudFourStateTransport_Internal(state);
            if (remainingLength > 0.0f)
            {
                state.correlation_cell_length = cellLength;
                state.remaining_boundary_distance = cellLength;
            }
            else
                state.awaiting_next_cell_length = true;
        }

        if (remainingLength > 0.0f)
        {
            const auto tailChunk =
                ResolveVolumetricCloudFourStateChunk_Internal(
                    distribution, state.conditional_survival,
                    extinction_per_density, remainingLength);
            const f32 tailAbsorption = tailChunk.absorption;
            pathAbsorptionMoment += pathTransmittance *
                (traversedLength * tailAbsorption +
                 tailChunk.absorption_moment);
            pathAbsorption += pathTransmittance * tailAbsorption;
            pathTransmittance *= tailChunk.transmittance;
            traversedLength += remainingLength;
            state.correlation_cell_length = cellLength;
            state.remaining_boundary_distance =
                cellLength - remainingLength;
            state.awaiting_next_cell_length = false;
            for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex)
                state.conditional_survival[stateIndex] =
                    tailChunk.conditional_survival[stateIndex];
        }
    }

    out.transmittance = pathTransmittance;
    if (!CloudDensityIntegrationValueIsFinite_Internal(out.transmittance) ||
        out.transmittance < 0.0f)
        out.transmittance = 0.0f;
    if (out.transmittance > 1.0f) out.transmittance = 1.0f;
    out.absorption = pathAbsorption;
    if (!CloudDensityIntegrationValueIsFinite_Internal(out.absorption) ||
        out.absorption < 0.0f)
        out.absorption = 0.0f;
    if (out.absorption > 1.0f) out.absorption = 1.0f;
    if (out.absorption > 1.0e-30f)
        out.absorption_centroid_distance =
            pathAbsorptionMoment / out.absorption;
    else
        out.absorption_centroid_distance = 0.5f * segment_length;
    if (!CloudDensityIntegrationValueIsFinite_Internal(
            out.absorption_centroid_distance) ||
        out.absorption_centroid_distance < 0.0f)
        out.absorption_centroid_distance = 0.0f;
    if (out.absorption_centroid_distance > segment_length)
        out.absorption_centroid_distance = segment_length;
    return out;
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
 * 一次と高次で別々に閉じた光学的深さから、高次散乱重みを解析的に求める。
 * xが0へ近づく場合はphi(x)=(1-exp(-x))/xの級数を使い、
 * 重みだけを異なる極限へ切り替えない。
 */
inline f32 ResolveVolumetricCloudReducedIntervalScatteringWeightFromDepths_Internal(
    f32 optical_depth, f32 reduced_optical_depth,
    f32 interval_transmittance, f32 contribution) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(optical_depth) ||
        !CloudDensityIntegrationValueIsFinite_Internal(reduced_optical_depth) ||
        !CloudDensityIntegrationValueIsFinite_Internal(interval_transmittance) ||
        !CloudDensityIntegrationValueIsFinite_Internal(contribution) ||
        optical_depth <= 0.0f || contribution <= 0.0f)
        return 0.0f;
    const f32 reducedOpticalDepth =
        reduced_optical_depth > 0.0f ? reduced_optical_depth : 0.0f;
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
 * 従来の均質媒質契約を保つ補助処理。高次の光学的深さを一次深さと
 * 消散縮小率から作り、分布を別々に閉じる経路では使用しない。
 */
inline f32 ResolveVolumetricCloudReducedIntervalScatteringWeight_Internal(
    f32 optical_depth, f32 interval_transmittance,
    f32 contribution, f32 occlusion) noexcept
{
    if (!CloudDensityIntegrationValueIsFinite_Internal(occlusion))
        return 0.0f;
    const f32 safeOcclusion = occlusion > 0.0f ? occlusion : 0.0f;
    return ResolveVolumetricCloudReducedIntervalScatteringWeightFromDepths_Internal(
        optical_depth, optical_depth * safeOcclusion,
        interval_transmittance, contribution);
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
