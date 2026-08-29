// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "memory/MemorySystem.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiDevice.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/IRhiTexture.h"
#include "render/Sky.h"
#include "render/VolumetricCloudAmbientCacheInternal.h"
#include "render/VolumetricCloudDensityIntegrationInternal.h"
#include "render/VolumetricCloudRayMarchInternal.h"
#include "render/VolumetricCloudTemporalInternal.h"
#include "editor_abi/EditorRenderPolicy.h"
#include "math/Camera.h"
#include "math/Math.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>

using namespace acs;

namespace {

/** GPU検証に必要なmemory systemを、このtestが起動した場合だけ所有する。 */
class CCloudGpuTestMemoryScope {
public:
    /** Resource segmentを利用可能にし、初期化できなければfalseを返す。 */
    bool Init() noexcept {
        if (CMemorySystem::Get(ESegment::Resource) != nullptr) return true;
        const TResult<void> result =
            CMemorySystem::Init(CMemorySystem::DefaultConfig());
        m_Owned = result.IsOk();
        return result.IsOk();
    }

    /** このscopeが起動したmemory systemだけを終了する。 */
    ~CCloudGpuTestMemoryScope() noexcept {
        if (m_Owned) CMemorySystem::Shutdown();
    }

private:
    /** 終了責任をこのscopeが持つ場合はtrue。 */
    bool m_Owned = false;
};

FVec3 NormalizeForTest(FVec3 v) noexcept {
    const f32 invLen = 1.0f / Sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return FVec3{v.x * invLen, v.y * invLen, v.z * invLen};
}

FVec3 PointOnRay(FVec3 origin, FVec3 direction, f32 distance) noexcept {
    return FVec3{origin.x + direction.x * distance,
                 origin.y + direction.y * distance,
                 origin.z + direction.z * distance};
}

// GPUのR16F保存と同じ丸めを通し、透過率の量子化後の値を求める。
f32 QuantizeR16FloatForTest(f32 value) noexcept {
    const DirectX::PackedVector::HALF encoded =
        DirectX::PackedVector::XMConvertFloatToHalf(value);
    return DirectX::PackedVector::XMConvertHalfToFloat(encoded);
}

// R16F透過率を丸めたときの、値0～1における半ULPを求める。
f32 R16TransmittanceHalfUlpForTest(f32 visibility) noexcept {
    constexpr f32 kMinimumNormal = 0.00006103515625f;
    constexpr f32 kMaximumBelowOne = 0.99951171875f;
    constexpr f32 kSubnormalHalfUlp =
        0.0000000298023223876953125f;
    const f32 magnitude = std::clamp(
        std::fabs(visibility), kMinimumNormal, kMaximumBelowOne);
    return std::max(
        std::exp2(std::floor(std::log2(magnitude)) - 11.0f),
        kSubnormalHalfUlp);
}

// 詳細残差で増幅した保存時半ULPを、補正後R16Fの半ULPと比較する。
f32 CloudSunDepthResidualCacheReliabilityForTest(
    f32 minimumCachedTransmittance,
    f32 opticalDepthResidual) noexcept {
    if (!std::isfinite(opticalDepthResidual) ||
        !std::isfinite(minimumCachedTransmittance)) return 0.0f;
    if (minimumCachedTransmittance >= 1.0f) return 1.0f;
    const f32 exponent = std::clamp(
        -opticalDepthResidual,
        0.0f, 16.0f);
    const f32 amplification = std::exp(exponent);
    const f32 corrected = std::clamp(
        std::max(minimumCachedTransmittance, 0.0f) * amplification,
        0.0f, 1.0f);
    const f32 amplifiedHalfUlp =
        R16TransmittanceHalfUlpForTest(minimumCachedTransmittance) *
        amplification;
    const f32 reliability =
        R16TransmittanceHalfUlpForTest(corrected) / amplifiedHalfUlp;
    if (reliability <= 0.0f) return 0.0f;
    if (reliability >= 1.0f) return 1.0f;
    return reliability;
}

void ExpectVec3Near(FVec3 actual, FVec3 expected, f32 epsilon) noexcept {
    EXPECT_NEAR(actual.x, expected.x, epsilon);
    EXPECT_NEAR(actual.y, expected.y, epsilon);
    EXPECT_NEAR(actual.z, expected.z, epsilon);
}

void ExpectMat4Near(const FMat4& actual, const FMat4& expected, f32 epsilon) noexcept {
    for (u32 row = 0u; row < 4u; ++row) {
        for (u32 column = 0u; column < 4u; ++column) {
            EXPECT_NEAR(actual.m[row][column], expected.m[row][column], epsilon);
        }
    }
}

FVolumetricCloudGroundHorizon
GroundHorizonHlslReferenceForTest(
    FVec3 cameraPosition, const FVolumetricCloudLayer& layer,
    FVec3 worldOrigin) noexcept {
    const FVec3 cameraLocal{
        cameraPosition.x - worldOrigin.x,
        cameraPosition.y - worldOrigin.y,
        cameraPosition.z - worldOrigin.z};
    FVolumetricCloudGroundHorizon out{};
    out.local_up = NormalizeForTest(FVec3{
        cameraLocal.x,
        cameraLocal.y + kVolumetricCloudPlanetRadius,
        cameraLocal.z});
    const f32 radialY =
        (kVolumetricCloudPlanetRadius + cameraLocal.y) > 1.0f
            ? kVolumetricCloudPlanetRadius + cameraLocal.y
            : 1.0f;
    const f32 radialXz2 =
        cameraLocal.x * cameraLocal.x +
        cameraLocal.z * cameraLocal.z;
    const f32 q = radialXz2 / radialY;
    const f32 cameraAltitude =
        cameraLocal.y + q * (0.5f - q / (8.0f * radialY));
    if (cameraAltitude < layer.base_height) {
        const f32 observerAltitude =
            cameraAltitude > 0.0f ? cameraAltitude : 0.0f;
        const f32 radiusRatio =
            kVolumetricCloudPlanetRadius /
            (kVolumetricCloudPlanetRadius + observerAltitude);
        f32 tangentSquared = 1.0f - radiusRatio * radiusRatio;
        if (tangentSquared < 0.0f) tangentSquared = 0.0f;
        if (tangentSquared > 1.0f) tangentSquared = 1.0f;
        out.ground_cutoff = -Sqrt(tangentSquared);
    }
    return out;
}

FVolumetricCloudDensityFrameTerms
DensityFrameTermsHlslReferenceForTest(
    const FVolumetricCloudLayer& layer, f32 windOffset) noexcept {
    FVolumetricCloudDensityFrameTerms out{};
    out.wind_world = FVec2{
        windOffset * 0.9284767f,
        windOffset * 0.3713907f};
    const f32 authoredShapeScale =
        layer.horizontal_noise_scale * 0.0030f;
    out.shape_scale =
        authoredShapeScale < 0.00004f
            ? 0.00004f
            : (authoredShapeScale > 0.00020f
                   ? 0.00020f
                   : authoredShapeScale);
    const f32 layerHeight =
        (layer.top_height - layer.base_height) > 1.0e-4f
            ? layer.top_height - layer.base_height
            : 1.0e-4f;
    out.inverse_layer_height = 1.0f / layerHeight;
    return out;
}

// 横方向と同じ物理尺度になる高さ方向領域を層厚から求める。
f32 CloudShapeVerticalSpanForTest(
    f32 shapeScale, f32 inverseLayerHeight) noexcept {
    const f32 safeInverseHeight =
        inverseLayerHeight > 1.0e-6f ? inverseLayerHeight : 1.0e-6f;
    return shapeScale / safeInverseHeight;
}

FVolumetricCloudLightBasis LightBasisHlslReferenceForTest(
    FVec3 sunDirection) noexcept {
    FVolumetricCloudLightBasis out{};
    // Runtime already normalized sunDir on the CPU; the former HLSL path then
    // normalized that float3 a second time in every invocation.
    out.direction = NormalizeForTest(NormalizeForTest(sunDirection));
    const f32 signY = out.direction.y >= 0.0f ? 1.0f : -1.0f;
    const f32 a = -1.0f / (signY + out.direction.y);
    const f32 b = out.direction.x * out.direction.z * a;
    out.tangent = FVec3{
        1.0f + signY * out.direction.x * out.direction.x * a,
        -signY * out.direction.x,
        signY * b};
    out.bitangent = FVec3{
        out.direction.y * out.tangent.z -
            out.direction.z * out.tangent.y,
        out.direction.z * out.tangent.x -
            out.direction.x * out.tangent.z,
        out.direction.x * out.tangent.y -
            out.direction.y * out.tangent.x};
    return out;
}

std::string ReadSkySource() {
    const std::filesystem::path testFile{__FILE__};
    const std::filesystem::path sourcePath =
        testFile.parent_path().parent_path() /
        "src" / "render" / "Sky.cpp";
    std::ifstream stream(sourcePath, std::ios::binary);
    if (!stream) {
        stream.open(
            std::filesystem::path{"acs"} / "src" /
            "render" / "Sky.cpp",
            std::ios::binary);
    }
    return std::string{
        std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}};
}

std::string ReadDiligentPipelineSource() {
    const std::filesystem::path testFile{__FILE__};
    const std::filesystem::path sourcePath =
        testFile.parent_path().parent_path() /
        "src" / "render" / "Diligent" / "DiligentPipeline.cpp";
    std::ifstream stream(sourcePath, std::ios::binary);
    if (!stream) {
        stream.open(
            std::filesystem::path{"acs"} / "src" / "render" /
            "Diligent" / "DiligentPipeline.cpp",
            std::ios::binary);
    }
    return std::string{
        std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}};
}

std::string ReadEditorAbiSource() {
    const std::filesystem::path testFile{__FILE__};
    const std::filesystem::path sourcePath =
        testFile.parent_path().parent_path() /
        "src" / "editor_abi" / "EditorAbi.cpp";
    std::ifstream stream(sourcePath, std::ios::binary);
    if (!stream) {
        stream.open(
            std::filesystem::path{"acs"} / "src" / "editor_abi" /
            "EditorAbi.cpp",
            std::ios::binary);
    }
    return std::string{
        std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}};
}

std::string ReadLegacyScene3DAdapterSource() {
    const std::filesystem::path testFile{__FILE__};
    const std::filesystem::path sourcePath =
        testFile.parent_path().parent_path() /
        "src" / "gameframework" / "LegacyScene3DAdapter.cpp";
    std::ifstream stream(sourcePath, std::ios::binary);
    if (!stream) {
        stream.open(std::filesystem::path{"acs"} / "src" / "gameframework" / "LegacyScene3DAdapter.cpp", std::ios::binary);
    }
    return std::string{
        std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}};
}

std::string ExtractRawShader(
    const std::string& source, const char* declaration) {
    const std::size_t declarationPos = source.find(declaration);
    if (declarationPos == std::string::npos) return {};
    const std::size_t begin = source.find("R\"(", declarationPos);
    if (begin == std::string::npos) return {};
    const std::size_t end = source.find(")\";", begin + 3u);
    if (end == std::string::npos) return {};
    return source.substr(begin + 3u, end - (begin + 3u));
}

bool Contains(const std::string& text, const char* token) {
    return text.find(token) != std::string::npos;
}

std::size_t CountOccurrences(
    const std::string& text, const char* token) {
    std::size_t count = 0;
    std::size_t position = 0;
    const std::size_t tokenLength = std::char_traits<char>::length(token);
    while ((position = text.find(token, position)) != std::string::npos) {
        ++count;
        position += tokenLength;
    }
    return count;
}

std::string CompactShader(const std::string& source) {
    std::string compact;
    compact.reserve(source.size());
    for (const char ch : source) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            compact.push_back(ch);
        }
    }
    return compact;
}

// 一つのrayセルが担当する閉区間と、占有判定・実密度の各標本位置。
struct FCloudFineSampleForTest {
    // 担当区間の始点。
    f32 cell_start = 0.0f;
    // 担当区間の終端。
    f32 cell_end = 0.0f;
    // 位相に依存しない占有判定位置。
    f32 occupancy_t = 0.0f;
    // 担当区間内の標本位置。
    f32 sample_t = 0.0f;
    // 光学的深さへ掛ける担当区間の長さ。
    f32 step_length = 0.0f;
    // 雲層の積分範囲内に担当区間が存在するか。
    bool valid = false;
};

// セル始点から連続区間を作り、占有と単一小区間の代表位置を中央へ置く。
FCloudFineSampleForTest ResolveCloudFineSampleForTest(
    f32 cellStart, f32 intervalEnd, f32 fineStep) noexcept {
    if (cellStart >= intervalEnd) return {};
    const f32 remainingLength = intervalEnd - cellStart;
    const f32 stepLength =
        remainingLength < fineStep ? remainingLength : fineStep;
    const f32 cellEnd = cellStart + stepLength;
    return FCloudFineSampleForTest{
        cellStart, cellEnd, cellStart + 0.5f * stepLength,
        cellStart + 0.5f * stepLength, stepLength, true};
}

f32 CloudHenyeyGreensteinForTest(f32 cosine, f32 anisotropy) noexcept {
    const f32 magnitude = std::abs(anisotropy);
    const f32 oneMinusMagnitude = 1.0f - magnitude;
    const f32 alignedCosine = anisotropy >= 0.0f
        ? cosine : -cosine;
    const f32 denominatorBase =
        oneMinusMagnitude * oneMinusMagnitude +
        2.0f * magnitude * ((1.0f - alignedCosine) > 0.0f
            ? (1.0f - alignedCosine) : 0.0f);
    const f32 denominator = std::pow(
        denominatorBase > 0.000001f
            ? denominatorBase : 0.000001f,
        1.5f);
    return (oneMinusMagnitude * (1.0f + magnitude)) /
           (12.566370f * denominator);
}

f32 DefaultCloudPhaseForTest(f32 cosine) noexcept {
    const FVolumetricCloudLighting lighting{};
    const f32 clampedCosine =
        cosine < -1.0f ? -1.0f : (cosine > 1.0f ? 1.0f : cosine);
    const f32 phase =
        CloudHenyeyGreensteinForTest(
             clampedCosine, lighting.PhaseForward) *
             lighting.PhaseBlend +
         CloudHenyeyGreensteinForTest(
             clampedCosine, lighting.PhaseBackward) *
             (1.0f - lighting.PhaseBlend);
    return phase < lighting.PhaseMin
        ? lighting.PhaseMin
        : (phase > lighting.PhaseMax ? lighting.PhaseMax : phase);
}

i32 ClampCloudCoordForTest(i32 value, i32 extent) noexcept {
    if (value < 0) return 0;
    if (value >= extent) return extent - 1;
    return value;
}

u32 CloudPixelPhaseForTest(i32 x, i32 y) noexcept {
    return static_cast<u32>(x & 3) |
           (static_cast<u32>(y & 3) << 2u);
}

f32 SaturateForTest(f32 value) noexcept {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

// 0～1 の値を指定区間から再配置し、シェーダーと同じ範囲へ収める。
f32 RemapUnitRangeForTest(f32 value, f32 lower, f32 upper) noexcept {
    const f32 width = upper - lower > 1.0e-5f
        ? upper - lower : 1.0e-5f;
    return SaturateForTest((value - lower) / width);
}

f32 SmoothStepForTest(f32 edge0, f32 edge1, f32 value) noexcept {
    const f32 t = SaturateForTest(
        (value - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

// 補間前の雲種と降水量から、塔状成長の強さを一度だけ求める。
f32 CloudToweringStrengthForTest(f32 cloudType, f32 precipitation) noexcept {
    const f32 typeTower = SmoothStepForTest(0.84f, 0.99f, cloudType);
    const f32 precipitationTower = SmoothStepForTest(0.25f, 0.85f, precipitation);
    return typeTower > precipitationTower ? typeTower : precipitationTower;
}

// 作者指定の積乱雲強度を、低周波天候場と被覆中心で連続した発達域へ局所化する。
f32 CloudLocalToweringStrengthForTest(f32 cloudType, f32 precipitation, f32 convectivePotential, f32 cloudInterior) noexcept
{
    const f32 authoredTower = CloudToweringStrengthForTest(cloudType, precipitation);
    // 明示した積乱雲設定が低周波天候場に抑制されないための発達域下限。
    const f32 authoredFloor = 0.45f * authoredTower;
    const f32 proceduralPotential = SmoothStepForTest(0.66f, 0.92f, SaturateForTest(convectivePotential));
    const f32 broadPotential = proceduralPotential > authoredFloor ? proceduralPotential : authoredFloor;
    const f32 interiorPotential = SmoothStepForTest(0.50f, 0.96f, SaturateForTest(cloudInterior));
    const f32 localPotential = broadPotential * interiorPotential;
    const f32 coherentPotential = SmoothStepForTest(0.12f, 0.82f, localPotential);
    return authoredTower * coherentPotential;
}

// 局所発達強度から積乱雲の高さ分布へ混ぜる割合を求める。
f32 CloudStormProfileMixForTest(f32 toweringStrength) noexcept
{
    return SaturateForTest(toweringStrength) * 0.92f;
}

// 縮小した散乱係数と消散係数で、均質な一つの区間を解析積分する。
f32 CloudReducedIntervalScatteringWeightForTest(
    f32 opticalDepth, f32 contribution, f32 occlusion) noexcept {
    const f32 boundedOcclusion = SaturateForTest(occlusion);
    const f32 boundedContribution = SaturateForTest(contribution) < boundedOcclusion
        ? SaturateForTest(contribution) : boundedOcclusion;
    const f32 boundedDepth = opticalDepth > 0.0f ? opticalDepth : 0.0f;
    const f32 reducedDepth = boundedDepth * boundedOcclusion;
    f32 normalizedAbsorption = 1.0f;
    if (reducedDepth <= 0.001f) {
        const f32 squared = reducedDepth * reducedDepth;
        normalizedAbsorption = 1.0f - reducedDepth * 0.5f +
            squared / 6.0f - squared * reducedDepth / 24.0f;
    } else {
        normalizedAbsorption =
            (1.0f - std::exp(-reducedDepth)) / reducedDepth;
    }
    return boundedContribution * boundedDepth * normalizedAbsorption;
}

// 均質な光学的深さを等分し、次数ごとの視線透過率を更新して積分する。
f32 AccumulateReducedCloudOrderForTest(
    f32 opticalDepth, u32 intervalCount,
    f32 contribution, f32 occlusion) noexcept {
    if (intervalCount == 0u) return 0.0f;
    const f32 intervalDepth = opticalDepth / static_cast<f32>(intervalCount);
    const f32 boundedOcclusion = SaturateForTest(occlusion);
    const f32 intervalTransmittance = std::exp(-intervalDepth * boundedOcclusion);
    f32 transmittance = 1.0f;
    f32 accumulated = 0.0f;
    for (u32 interval = 0u; interval < intervalCount; ++interval) {
        accumulated += transmittance * CloudReducedIntervalScatteringWeightForTest(
            intervalDepth, contribution, boundedOcclusion);
        transmittance *= intervalTransmittance;
    }
    return accumulated;
}

// 詳細体積が基本形状を膨張または侵食できる最大量を求める。
f32 CloudBillowMaximumOffsetForTest(f32 height) noexcept {
    return 0.024f + (0.130f - 0.024f) *
        SmoothStepForTest(0.18f, 0.92f, SaturateForTest(height));
}

// 詳細変位を確定した空と雲芯から除き、基本形状の境界だけへ割り当てる。
f32 CloudBillowBoundaryWeightForTest(f32 baseDensity) noexcept {
    const f32 signedShape = SaturateForTest(baseDensity) * 2.0f - 1.0f;
    return 1.0f - std::fabs(signedShape);
}

// 合成された詳細値から、低周波成分を除いた中間帯域を復元する。
f32 CloudDetailMiddleBandForTest(FVec2 detailBands) noexcept {
    return SaturateForTest((detailBands.y - detailBands.x * 0.55f) * (1.0f / 0.45f));
}

// 同分布の二領域を差し引き、雲頂へ中間帯域を混ぜた房状の形状移動量を求める。
f32 CloudBillowOffsetForTest(FVec2 detailA, FVec2 detailB, f32 height, f32 middleVisibility) noexcept {
    const f32 topMiddleWeight = 0.48f * SmoothStepForTest(0.38f, 0.90f, SaturateForTest(height)) * SaturateForTest(middleVisibility);
    const f32 coarseDifference = detailA.x - detailB.x;
    const f32 middleDifference = CloudDetailMiddleBandForTest(detailA) - CloudDetailMiddleBandForTest(detailB);
    const f32 difference = coarseDifference + (middleDifference - coarseDifference) * topMiddleWeight;
    return difference * CloudBillowMaximumOffsetForTest(height);
}

// 二つの天候領域を混ぜた雲種を、三種類の高さ形状で使う範囲へ広げる。
f32 CloudExpandedTypeForTest(f32 blendedType) noexcept {
    return SmoothStepForTest(0.42f, 0.66f, blendedType);
}

// 履歴を使えない採取画素だけを、等倍標本と現在再構成の混合へ戻す条件を表す。
bool CloudTemporalUsesInvalidHistoryBlendForTest(bool temporalSuperResolution, bool scheduled, bool historyAccepted) noexcept {
    return temporalSuperResolution && scheduled && !historyAccepted;
}

// 等倍の現在標本がある場合だけ、履歴との空／雲の不一致を判定する。
bool CloudTemporalOccupancyMismatchForTest(f32 currentAlpha, f32 historyAlpha, bool temporalSuperResolution, bool scheduled) noexcept {
    return (!temporalSuperResolution || scheduled) && ((currentAlpha < 0.02f && historyAlpha > 0.08f) || (currentAlpha > 0.08f && historyAlpha < 0.02f));
}

// 現在と前フレームの対流位相差を、シェーダーと同じ履歴再混合率へ変換する。
f32 CloudTemporalEvolutionMismatchForTest(FVec4 current, FVec4 previous) noexcept {
    const f32 slowX = std::fabs(current.x - previous.x);
    const f32 slowY = std::fabs(current.y - previous.y);
    const f32 fineX = std::fabs(current.z - previous.z);
    const f32 fineY = std::fabs(current.w - previous.w);
    const f32 delta = std::max(std::max(slowX, slowY), std::max(fineX, fineY));
    return SaturateForTest(delta * 220.0f);
}

// 16フレーム周期の等倍標本を、シェーダーと同じ反映率へ変換する。
f32 CloudTemporalScheduledCurrentWeightForTest(f32 evolutionMismatch) noexcept {
    return SaturateForTest(0.28f + SaturateForTest(evolutionMismatch) * 2.0f);
}

// 任意倍率の縮小描画で、毎フレーム得る現在再構成の反映率を求める。
f32 CloudTemporalScaledCurrentWeightForTest(f32 evolutionMismatch) noexcept {
    const f32 sanitizedMismatch = SaturateForTest(evolutionMismatch);
    return sanitizedMismatch > 0.18f ? sanitizedMismatch : 0.18f;
}

// 別レイの空間再構成を混ぜる量は、実際の対流変化量だけに制限する。
f32 CloudTemporalCurrentWeightForTest(f32 evolutionMismatch) noexcept {
    return SaturateForTest(evolutionMismatch);
}

// 現在近傍から外れた履歴成分だけを制限し、範囲内の細部は変更しない。
f32 CloudTemporalClipChannelForTest(f32 historyValue, f32 currentMinimum, f32 currentMaximum, f32 minimumRange) noexcept {
    const f32 measuredRange = currentMaximum - currentMinimum;
    const f32 currentRange = measuredRange > minimumRange ? measuredRange : minimumRange;
    const f32 lower = currentMinimum - currentRange * 0.35f;
    const f32 upper = currentMaximum + currentRange * 0.35f;
    return historyValue < lower ? lower : (historyValue > upper ? upper : historyValue);
}

// 視認できる差だけを十字近傍の確認対象にする。
bool CloudTemporalNeedsNeighborhoodClipForTest(f32 historyValue, f32 currentValue, f32 minimumRange) noexcept {
    const f32 difference = historyValue > currentValue ? historyValue - currentValue : currentValue - historyValue;
    return difference > minimumRange;
}

// 近傍確認の8位相分散を描画側と同じ符号なし整数演算で再現する。
bool CloudTemporalNeighborhoodClipScheduledForTest(u32 pixelX, u32 pixelY, u32 phaseIndex) noexcept {
    u32 pixelHash = pixelX * 0x8da6b343u ^ pixelY * 0xd8163841u;
    pixelHash ^= pixelHash >> 16u;
    pixelHash *= 0x7feb352du;
    pixelHash ^= pixelHash >> 15u;
    const u32 scrambledPhase = (phaseIndex + (pixelHash & 15u)) & 15u;
    return (scrambledPhase & 7u) == 0u;
}

f32 CloudWeatherMaskForTest(f32 weatherCoverage, f32 coverage) noexcept {
    const f32 threshold =
        0.72f + (0.36f - 0.72f) * SaturateForTest(coverage);
    const f32 upper =
        threshold + 0.14f < 0.98f ? threshold + 0.14f : 0.98f;
    return SmoothStepForTest(
        threshold, upper, weatherCoverage);
}

f32 CloudColumnTopShiftForTest(
    f32 cloudInterior, f32 cloudType, f32 precipitation,
    f32 warp, f32 shapePhaseX, f32 shapePhaseY) noexcept {
    const f32 core = SmoothStepForTest(0.08f, 0.92f, SaturateForTest(cloudInterior));
    const f32 toweringStrength = CloudLocalToweringStrengthForTest(cloudType, precipitation, warp, cloudInterior);
    const f32 typePuff = SmoothStepForTest(0.26f, 0.72f, SaturateForTest(cloudType));
    const f32 precipitationPuff = SmoothStepForTest(0.20f, 0.70f, SaturateForTest(precipitation));
    const f32 precipitationRelief = precipitationPuff * 0.80f;
    const f32 puffRelief =
        typePuff > precipitationRelief ? typePuff : precipitationRelief;
    const f32 boundedTowering = SaturateForTest(toweringStrength);
    const f32 reliefStrength =
        puffRelief > boundedTowering ? puffRelief : boundedTowering;
    const f32 amplitude = 0.018f + (0.100f - 0.018f) * reliefStrength;
    const f32 warpPattern =
        SmoothStepForTest(0.36f, 0.64f, warp) * 2.0f - 1.0f;
    const f32 typePattern = cloudType * 2.0f - 1.0f;
    const f32 localPhase =
        shapePhaseX * warpPattern + shapePhaseY * typePattern;
    f32 evolvingWarp = warp - 0.5f + localPhase * 0.45f;
    if (evolvingWarp < -0.5f) evolvingWarp = -0.5f;
    if (evolvingWarp > 0.5f) evolvingWarp = 0.5f;
    f32 signal =
        (core - 0.45f) * 1.45f + evolvingWarp * 0.65f;
    if (signal < -1.0f) signal = -1.0f;
    if (signal > 1.0f) signal = 1.0f;
    return signal * amplitude;
}

// 低周波天候模様から、共通の凝結高度を崩さない柱ごとの雲底持ち上げ量を求める。
f32 CloudColumnBaseLiftForTest(f32 cloudInterior, f32 cloudType, f32 precipitation, f32 warp) noexcept {
    const f32 verticalType = SaturateForTest(cloudType > precipitation ? cloudType : precipitation);
    const f32 broadPattern = SmoothStepForTest(0.18f, 0.82f, warp);
    const f32 edgePattern = 1.0f - SmoothStepForTest(0.08f, 0.86f, SaturateForTest(cloudInterior));
    const f32 toweringStrength = CloudLocalToweringStrengthForTest(cloudType, precipitation, warp, cloudInterior);
    f32 amplitude = 0.006f + (0.014f - 0.006f) * verticalType;
    amplitude *= 1.0f - 0.28f * toweringStrength;
    const f32 signal = SaturateForTest(0.10f + broadPattern * 0.75f + edgePattern * 0.15f);
    return amplitude * signal;
}

f32 CloudAnvilCoverageExpansionForTest(f32 layerHeight, f32 toweringStrength) noexcept
{
    const f32 anvilBand = SmoothStepForTest(0.50f, 0.66f, SaturateForTest(layerHeight)) *
        (1.0f - SmoothStepForTest(0.80f, 0.97f, SaturateForTest(layerHeight)));
    return 0.07f * toweringStrength * anvilBand;
}

// 物理距離から雲底密度の正規化終端を求め、極端な層厚でも指定範囲へ収める。
f32 CloudBaseRiseEndForTest(
    f32 layerThickness, f32 physicalRise,
    f32 minimumRise, f32 maximumRise) noexcept {
    const f32 safeThickness = layerThickness > 1.0f ? layerThickness : 1.0f;
    const f32 normalizedRise = physicalRise / safeThickness;
    return normalizedRise < minimumRise ? minimumRise :
        (normalizedRise > maximumRise ? maximumRise : normalizedRise);
}

// 層雲・層積雲・積雲・積乱雲の物理幅を局所雲柱座標へ変換する。
FVec4 CloudBaseRiseEndsForTest(
    f32 layerThickness, f32 columnSpan = 1.0f) noexcept {
    const f32 safeColumnSpan = columnSpan > 0.001f ? columnSpan : 0.001f;
    const FVec4 layerRiseEnds{
        CloudBaseRiseEndForTest(layerThickness, 140.0f, 0.012f, 0.070f),
        CloudBaseRiseEndForTest(layerThickness, 220.0f, 0.019f, 0.110f),
        CloudBaseRiseEndForTest(layerThickness, 320.0f, 0.027f, 0.160f),
        CloudBaseRiseEndForTest(layerThickness, 180.0f, 0.016f, 0.090f)};
    const auto localRise = [safeColumnSpan](f32 value) noexcept {
        const f32 local = value / safeColumnSpan;
        return local < 0.95f ? local : 0.95f;
    };
    return FVec4{
        localRise(layerRiseEnds.x), localRise(layerRiseEnds.y),
        localRise(layerRiseEnds.z), localRise(layerRiseEnds.w)};
}

// 雲種別の物理幅を補間し、現在の雲底立ち上がり終端を求める。
f32 CloudProfileBaseRiseEndForTest(f32 cloudType, f32 toweringStrength, f32 layerThickness, f32 columnSpan = 1.0f) noexcept
{
    const FVec4 riseEnds = CloudBaseRiseEndsForTest(
        layerThickness, columnSpan);
    const f32 stratocumulusWeight =
        SmoothStepForTest(0.18f, 0.52f, cloudType);
    const f32 cumulusWeight =
        SmoothStepForTest(0.50f, 0.84f, cloudType);
    f32 lowCloudRise = riseEnds.x +
        (riseEnds.y - riseEnds.x) * stratocumulusWeight;
    lowCloudRise +=
        (riseEnds.z - lowCloudRise) * cumulusWeight;
    const f32 stormMix = CloudStormProfileMixForTest(toweringStrength);
    return lowCloudRise + (riseEnds.w - lowCloudRise) * stormMix;
}

// 積乱雲の本体からかなとこまで、密度支持が途切れない高さ分布を再現する。
f32 CloudStormProfileForTest(f32 height, f32 cloudType, f32 toweringStrength, f32 layerThickness) noexcept
{
    const f32 stratocumulusWeight = SmoothStepForTest(0.18f, 0.52f, cloudType);
    const f32 cumulusWeight = SmoothStepForTest(0.50f, 0.84f, cloudType);
    const FVec4 riseEnds = CloudBaseRiseEndsForTest(layerThickness);
    const f32 stratus = SmoothStepForTest(0.0f, riseEnds.x, height) *
        (1.0f - SmoothStepForTest(0.38f, 0.50f, height));
    const f32 stratocumulus = SmoothStepForTest(0.0f, riseEnds.y, height) *
        (1.0f - SmoothStepForTest(0.72f, 0.98f, height)) *
        (0.78f + 0.22f * SmoothStepForTest(0.08f, 0.42f, height));
    const f32 cumulus = SmoothStepForTest(0.0f, riseEnds.z, height) *
        (1.0f - SmoothStepForTest(0.62f, 0.995f, height)) *
        (0.64f + 0.36f * SmoothStepForTest(0.12f, 0.52f, height));
    const f32 mixedLowCloud = stratus +
        (stratocumulus - stratus) * stratocumulusWeight;
    const f32 profile = mixedLowCloud +
        (cumulus - mixedLowCloud) * cumulusWeight;
    const f32 stormRiseEnd = riseEnds.w;
    const f32 stormRiseBegin = stormRiseEnd * 0.20f;
    const f32 stormBody = SmoothStepForTest(stormRiseBegin, stormRiseEnd, height) *
        (1.0f - 0.38f * SmoothStepForTest(0.30f, 0.78f, height)) *
        (1.0f - SmoothStepForTest(0.78f, 0.995f, height));
    const f32 stormShoulder = SmoothStepForTest(0.42f, 0.56f, height) *
        (1.0f - SmoothStepForTest(0.66f, 0.82f, height)) * 0.08f;
    const f32 anvil = SmoothStepForTest(0.56f, 0.70f, height) *
        (1.0f - SmoothStepForTest(0.80f, 0.995f, height)) * 0.24f;
    const f32 storm = SaturateForTest(
        stormBody + (stormShoulder + anvil) * (1.0f - stormBody));
    const f32 stormMix = CloudStormProfileMixForTest(toweringStrength);
    return profile + (storm - profile) * stormMix;
}

// 積乱雲の中層で天候場のしきい値を上げ、正の被覆領域そのものを細くする。
f32 CloudConvectiveWaistThresholdOffsetForTest(f32 height, f32 toweringStrength) noexcept
{
    const f32 waist = SmoothStepForTest(0.28f, 0.44f, height) *
        (1.0f - SmoothStepForTest(0.58f, 0.74f, height));
    return 0.018f * SaturateForTest(toweringStrength) * waist;
}

f32 CloudWeatherMaskForLayerForTest(f32 weatherCoverage, f32 threshold, f32 inverseTransitionWidth, f32 layerHeight, f32 toweringStrength) noexcept
{
    const f32 waistThreshold = threshold + CloudConvectiveWaistThresholdOffsetForTest(layerHeight, toweringStrength);
    const f32 narrowedT = SaturateForTest(
        (weatherCoverage - waistThreshold) * inverseTransitionWidth);
    const f32 narrowedBaseMask =
        narrowedT * narrowedT * (3.0f - 2.0f * narrowedT);
    const f32 expansion = CloudAnvilCoverageExpansionForTest(layerHeight, toweringStrength);
    const f32 anvilThreshold = threshold - expansion;
    const f32 anvilT = SaturateForTest(
        (weatherCoverage - anvilThreshold) * inverseTransitionWidth);
    const f32 anvilMask = anvilT * anvilT * (3.0f - 2.0f * anvilT);
    const f32 anvilBlend = SaturateForTest(expansion * 14.285714f);
    return narrowedBaseMask + (anvilMask - narrowedBaseMask) * anvilBlend;
}

// 天候被覆の中間域だけに局所的な成長量を与える。
f32 CloudWeatherCoverageEvolutionForTest(
    f32 weatherCoverage, f32 cloudType, f32 warp,
    FVec2 shapePhase, FVec2 finePhase) noexcept {
    const f32 warpPattern = warp * 2.0f - 1.0f;
    const f32 typePattern = cloudType * 2.0f - 1.0f;
    const f32 slow = shapePhase.x * warpPattern +
                     shapePhase.y * typePattern;
    const f32 fine = finePhase.x * typePattern +
                     finePhase.y * warpPattern;
    const f32 edgeBase = weatherCoverage * (1.0f - weatherCoverage);
    const f32 edgeResponse = 16.0f * edgeBase * edgeBase;
    f32 boundedPhase = slow * 0.38f + fine * 0.32f;
    if (boundedPhase < -0.14f) boundedPhase = -0.14f;
    if (boundedPhase > 0.14f) boundedPhase = 0.14f;
    return boundedPhase * edgeResponse;
}

// 低周波の湿度核を所有者とし、境界だけへ高周波の零平均変位を加える。
// 正値化前の符号を保ち、LODで未凝結側の距離を失わない。
f32 CloudHierarchicalShapeForTest(
    f32 macroPerlin, f32 middlePerlin, f32 finePerlin,
    f32 worleyA, f32 worleyB) noexcept {
    const f32 macroPotential =
        SaturateForTest(macroPerlin) * 2.0f - 1.0f;
    const f32 middleDisplacement =
        (SaturateForTest(middlePerlin) * 2.0f - 1.0f) * 0.50f;
    const f32 fineDisplacement =
        (SaturateForTest(finePerlin) * 2.0f - 1.0f) * 0.25f;
    const f32 worleyDisplacement =
        (SaturateForTest(worleyA) - SaturateForTest(worleyB)) * 0.25f;
    const f32 boundaryWeight = 1.0f - std::fabs(macroPotential);
    return std::clamp(
        macroPotential + boundaryWeight *
            (middleDisplacement + fineDisplacement + worleyDisplacement),
        -1.0f, 1.0f);
}

/** HLSLの形状生成と同じ三成分の計算点。 */
struct FCloudNoisePointForTest {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
};

// HLSLのfracと同じ床基準の小数部を返す。
f32 CloudNoiseFractionForTest(f32 value) noexcept {
    return value - std::floor(value);
}

// 初回焼き込みで使う超越関数なしの三成分hashをCPUで再現する。
FCloudNoisePointForTest CloudNoiseHashForTest(
    FCloudNoisePointForTest point) noexcept {
    point.x = CloudNoiseFractionForTest(point.x * 0.1031f);
    point.y = CloudNoiseFractionForTest(point.y * 0.1030f);
    point.z = CloudNoiseFractionForTest(point.z * 0.0973f);
    const f32 coupling =
        point.x * (point.y + 33.33f) +
        point.y * (point.x + 33.33f) +
        point.z * (point.z + 33.33f);
    point.x += coupling;
    point.y += coupling;
    point.z += coupling;
    return FCloudNoisePointForTest{
        CloudNoiseFractionForTest((point.x + point.y) * point.z),
        CloudNoiseFractionForTest((point.x + point.x) * point.y),
        CloudNoiseFractionForTest((point.y + point.x) * point.x)};
}

// 負座標を含む周期セルを0以上の範囲へ戻す。
f32 WrapCloudNoiseCellForTest(f32 cell, f32 frequency) noexcept {
    return cell - std::floor(cell / frequency) * frequency;
}

// 焼き込みシェーダーの周期PerlinをCPUで再現する。
f32 CloudGradientNoiseForTest(
    FCloudNoisePointForTest point, f32 frequency) noexcept {
    const FCloudNoisePointForTest cell{
        std::floor(point.x), std::floor(point.y), std::floor(point.z)};
    const FCloudNoisePointForTest local{
        CloudNoiseFractionForTest(point.x),
        CloudNoiseFractionForTest(point.y),
        CloudNoiseFractionForTest(point.z)};
    const auto fade = [](f32 value) noexcept {
        return value * value * value *
            (value * (value * 6.0f - 15.0f) + 10.0f);
    };
    const FCloudNoisePointForTest blend{
        fade(local.x), fade(local.y), fade(local.z)};
    f32 noise = 0.0f;
    for (u32 z = 0u; z < 2u; ++z) {
        for (u32 y = 0u; y < 2u; ++y) {
            for (u32 x = 0u; x < 2u; ++x) {
                const FCloudNoisePointForTest offset{
                    static_cast<f32>(x),
                    static_cast<f32>(y),
                    static_cast<f32>(z)};
                FCloudNoisePointForTest gradient = CloudNoiseHashForTest(
                    FCloudNoisePointForTest{
                        WrapCloudNoiseCellForTest(cell.x + offset.x, frequency),
                        WrapCloudNoiseCellForTest(cell.y + offset.y, frequency),
                        WrapCloudNoiseCellForTest(cell.z + offset.z, frequency)});
                gradient.x = gradient.x * 2.0f - 1.0f + 1.0e-4f;
                gradient.y = gradient.y * 2.0f - 1.0f + 1.0e-4f;
                gradient.z = gradient.z * 2.0f - 1.0f + 1.0e-4f;
                const f32 inverseLength = 1.0f / std::sqrt(
                    gradient.x * gradient.x +
                    gradient.y * gradient.y +
                    gradient.z * gradient.z);
                gradient.x *= inverseLength;
                gradient.y *= inverseLength;
                gradient.z *= inverseLength;
                const f32 weightX = x != 0u ? blend.x : 1.0f - blend.x;
                const f32 weightY = y != 0u ? blend.y : 1.0f - blend.y;
                const f32 weightZ = z != 0u ? blend.z : 1.0f - blend.z;
                noise += weightX * weightY * weightZ *
                    (gradient.x * (local.x - offset.x) +
                     gradient.y * (local.y - offset.y) +
                     gradient.z * (local.z - offset.z));
            }
        }
    }
    return noise * 0.5f + 0.5f;
}

// 焼き込みシェーダーの周期Worley最近接距離をCPUで再現する。
f32 CloudWorleyNoiseForTest(
    FCloudNoisePointForTest point, f32 frequency) noexcept {
    const FCloudNoisePointForTest scaled{
        point.x * frequency,
        point.y * frequency,
        point.z * frequency};
    const FCloudNoisePointForTest cell{
        std::floor(scaled.x),
        std::floor(scaled.y),
        std::floor(scaled.z)};
    const FCloudNoisePointForTest local{
        CloudNoiseFractionForTest(scaled.x),
        CloudNoiseFractionForTest(scaled.y),
        CloudNoiseFractionForTest(scaled.z)};
    f32 minimumDistanceSquared = 1.0f;
    for (i32 z = -1; z <= 1; ++z) {
        for (i32 y = -1; y <= 1; ++y) {
            for (i32 x = -1; x <= 1; ++x) {
                const FCloudNoisePointForTest feature = CloudNoiseHashForTest(
                    FCloudNoisePointForTest{
                        WrapCloudNoiseCellForTest(
                            cell.x + static_cast<f32>(x), frequency),
                        WrapCloudNoiseCellForTest(
                            cell.y + static_cast<f32>(y), frequency),
                        WrapCloudNoiseCellForTest(
                            cell.z + static_cast<f32>(z), frequency)});
                const f32 deltaX =
                    static_cast<f32>(x) + feature.x - local.x;
                const f32 deltaY =
                    static_cast<f32>(y) + feature.y - local.y;
                const f32 deltaZ =
                    static_cast<f32>(z) + feature.z - local.z;
                const f32 distanceSquared =
                    deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
                if (distanceSquared < minimumDistanceSquared) {
                    minimumDistanceSquared = distanceSquared;
                }
            }
        }
    }
    return 1.0f - std::sqrt(SaturateForTest(minimumDistanceSquared));
}

// 基本形状へ焼き込む低周波の三次元ゆがみをCPUで再現する。
FCloudNoisePointForTest CloudWarpedShapeDomainForTest(
    FCloudNoisePointForTest uvw) noexcept {
    const f32 warpX = CloudGradientNoiseForTest(
        FCloudNoisePointForTest{
            (uvw.x + 0.173f) * 2.0f,
            (uvw.y + 0.417f) * 2.0f,
            (uvw.z + 0.619f) * 2.0f},
        2.0f);
    const f32 warpZ = CloudGradientNoiseForTest(
        FCloudNoisePointForTest{
            (uvw.x + 0.731f) * 2.0f,
            (uvw.y + 0.251f) * 2.0f,
            (uvw.z + 0.847f) * 2.0f},
        2.0f);
    return FCloudNoisePointForTest{
        uvw.x + (warpX - 0.5f) * 0.22f,
        uvw.y + ((warpX + warpZ) * 0.5f - 0.5f) * 0.28f,
        uvw.z + (warpZ - 0.5f) * 0.22f};
}

// 同じ点で、旧細胞しきい値形状と新しい階層形状を同時に求める。
void ResolveCloudTopologyShapesForTest(
    FCloudNoisePointForTest uvw,
    f32& legacyShape, f32& hierarchicalShape) noexcept {
    const FCloudNoisePointForTest warped =
        CloudWarpedShapeDomainForTest(uvw);
    const auto scaled = [warped](f32 scale) noexcept {
        return FCloudNoisePointForTest{
            warped.x * scale, warped.y * scale, warped.z * scale};
    };
    const f32 perlin2 = CloudGradientNoiseForTest(scaled(2.0f), 2.0f);
    const f32 perlin4 = CloudGradientNoiseForTest(scaled(4.0f), 4.0f);
    const f32 perlin8 = CloudGradientNoiseForTest(scaled(8.0f), 8.0f);
    const f32 worleyA = CloudWorleyNoiseForTest(warped, 4.0f);
    const f32 worleyB = CloudWorleyNoiseForTest(
        FCloudNoisePointForTest{
            warped.x + 0.50f,
            warped.y + 0.25f,
            warped.z + 0.75f},
        4.0f);
    const f32 legacyPerlin =
        perlin2 * 0.62f + perlin4 * 0.30f + perlin8 * 0.08f;
    legacyShape = RemapUnitRangeForTest(
        legacyPerlin, 1.0f - SaturateForTest(worleyA), 1.0f);
    hierarchicalShape = CloudHierarchicalShapeForTest(
        perlin2, perlin4, perlin8, worleyA, worleyB);
}

/** 周期体積の6近傍連結性。 */
struct FCloudTopologyMetricsForTest {
    u32 occupied_count = 0u;
    u32 component_count = 0u;
    u32 largest_component_count = 0u;
};

// 周期境界を含む二値形状から、孤立成分と最大雲塊の大きさを求める。
template <u32 GridSize>
FCloudTopologyMetricsForTest ResolveCloudTopologyMetricsForTest(
    const f32* field, f32 threshold) noexcept {
    constexpr u32 voxelCount = GridSize * GridSize * GridSize;
    u8 occupied[voxelCount]{};
    u8 visited[voxelCount]{};
    u32 queue[voxelCount]{};
    FCloudTopologyMetricsForTest metrics{};
    for (u32 index = 0u; index < voxelCount; ++index) {
        if (field[index] > threshold) {
            occupied[index] = 1u;
            ++metrics.occupied_count;
        }
    }
    const auto indexOf = [](u32 x, u32 y, u32 z) noexcept {
        return (z * GridSize + y) * GridSize + x;
    };
    for (u32 seed = 0u; seed < voxelCount; ++seed) {
        if (occupied[seed] == 0u || visited[seed] != 0u) continue;
        ++metrics.component_count;
        u32 readIndex = 0u;
        u32 writeIndex = 1u;
        queue[0] = seed;
        visited[seed] = 1u;
        while (readIndex < writeIndex) {
            const u32 current = queue[readIndex++];
            const u32 x = current % GridSize;
            const u32 yz = current / GridSize;
            const u32 y = yz % GridSize;
            const u32 z = yz / GridSize;
            const u32 neighbors[6]{
                indexOf((x + 1u) % GridSize, y, z),
                indexOf((x + GridSize - 1u) % GridSize, y, z),
                indexOf(x, (y + 1u) % GridSize, z),
                indexOf(x, (y + GridSize - 1u) % GridSize, z),
                indexOf(x, y, (z + 1u) % GridSize),
                indexOf(x, y, (z + GridSize - 1u) % GridSize)};
            for (u32 neighbor : neighbors) {
                if (occupied[neighbor] == 0u || visited[neighbor] != 0u) {
                    continue;
                }
                visited[neighbor] = 1u;
                queue[writeIndex++] = neighbor;
            }
        }
        if (writeIndex > metrics.largest_component_count) {
            metrics.largest_component_count = writeIndex;
        }
    }
    return metrics;
}

// 符号付き形状の担当幅から、完成・中間・粗形状を連続補間する。
f32 CloudBandLimitedShapeForTest(
    f32 coarseShape, f32 middleShape, f32 completeShape,
    f32 maximumDomainFootprint) noexcept {
    const auto blend =
        render_internal::ResolveVolumetricCloudShapeFrequencyBlend_Internal(
            maximumDomainFootprint);
    return coarseShape * blend.coarse +
           middleShape * blend.middle +
           completeShape * blend.complete;
}

constexpr u32 kShapeFilterLineLengthForTest = 128u;

// 周期境界をまたぐ形状行の添字を0～127へ戻す。
u32 WrapShapeFilterLineIndexForTest(i32 index) noexcept {
    return static_cast<u32>(index) &
        (kShapeFilterLineLengthForTest - 1u);
}

// 共有メモリ上の倍加と同じ順で、周期前方区間の最大値を作る。
f32 ForwardPeriodicShapeMaximumSparseForTest(
    const f32* line, u32 startIndex, u32 width) noexcept {
    f32 maximumA[kShapeFilterLineLengthForTest]{};
    f32 maximumB[kShapeFilterLineLengthForTest]{};
    for (u32 index = 0u; index < kShapeFilterLineLengthForTest; ++index) {
        maximumA[index] = line[index];
    }
    for (u32 offset = 1u; offset < width; offset <<= 1u) {
        for (u32 index = 0u; index < kShapeFilterLineLengthForTest; ++index) {
            maximumB[index] = std::max(
                maximumA[index],
                maximumA[WrapShapeFilterLineIndexForTest(
                    static_cast<i32>(index + offset))]);
        }
        for (u32 index = 0u; index < kShapeFilterLineLengthForTest; ++index) {
            maximumA[index] = maximumB[index];
        }
    }
    return maximumA[startIndex];
}

// 前方最大値二つを重ね、三線形再構成の一セル分まで含む中心付き最大値を得る。
f32 CenteredPeriodicShapeMaximumSparseForTest(
    const f32* line, u32 centerIndex, u32 width) noexcept {
    const i32 radius = width == 4u ? 3 : (width == 16u ? 9 : 33);
    const i32 secondStartOffset = radius - static_cast<i32>(width) + 1;
    const u32 firstStart = WrapShapeFilterLineIndexForTest(
        static_cast<i32>(centerIndex) - radius);
    const u32 secondStart = WrapShapeFilterLineIndexForTest(
        static_cast<i32>(centerIndex) + secondStartOffset);
    return std::max(
        ForwardPeriodicShapeMaximumSparseForTest(line, firstStart, width),
        ForwardPeriodicShapeMaximumSparseForTest(line, secondStart, width));
}

// 全標本を直接比較し、倍加版の周期境界と担当範囲を独立に照合する。
f32 CenteredPeriodicShapeMaximumReferenceForTest(
    const f32* line, u32 centerIndex, u32 width) noexcept {
    const i32 radius = width == 4u ? 3 : (width == 16u ? 9 : 33);
    f32 maximum = 0.0f;
    for (i32 offset = -radius; offset <= radius; ++offset) {
        maximum = std::max(
            maximum,
            line[WrapShapeFilterLineIndexForTest(
                static_cast<i32>(centerIndex) + offset)]);
    }
    return maximum;
}

// 詳細領域の一周期に対する採取間隔から、その帯域を安全に残せる割合を求める。
f32 CloudDetailFrequencyVisibilityForTest(f32 sampleSpacing, f32 frequency, f32 fadeBegin, f32 fadeEnd) noexcept {
    const f32 boundedSpacing = sampleSpacing > 0.0f ? sampleSpacing : 0.0f;
    const f32 footprint = boundedSpacing * 0.00031f * frequency;
    return 1.0f - SmoothStepForTest(fadeBegin, fadeEnd, footprint);
}

// レイの採取間隔から、低周波の房形状を安全に採取できる割合を求める。
f32 CloudBillowVisibilityFromSampleSpacingForTest(f32 sampleSpacing) noexcept {
    return CloudDetailFrequencyVisibilityForTest(sampleSpacing, 4.0f, 0.15f, 0.52f);
}

// レイの採取間隔から、中間規模の房形状を安全に採取できる割合を求める。
f32 CloudMiddleBillowVisibilityFromSampleSpacingForTest(f32 sampleSpacing) noexcept {
    return CloudDetailFrequencyVisibilityForTest(sampleSpacing, 8.0f, 0.05f, 0.20f);
}

// レイの採取間隔から、高周波の侵食形状を安全に採取できる割合を求める。
f32 CloudErosionVisibilityFromSampleSpacingForTest(f32 sampleSpacing) noexcept {
    return CloudDetailFrequencyVisibilityForTest(sampleSpacing, 16.0f, 0.05f, 0.24f);
}

// 距離と画素角から、視線と交差する画素の物理幅だけを求める。
f32 CloudProjectedPixelWidthForTest(f32 sampleDistance, f32 angularPixelFootprint) noexcept {
    const f32 boundedDistance = sampleDistance > 0.0f ? sampleDistance : 0.0f;
    const f32 boundedFootprint = angularPixelFootprint > 0.0f ? angularPixelFootprint : 0.0f;
    return boundedDistance * boundedFootprint;
}

// 詳細帯域の折り返し判定だけに使う担当幅を求める。
f32 CloudDetailSampleSpacingForTest(f32 integrationSpacing, f32 projectedPixelWidth) noexcept {
    const f32 boundedSpacing = integrationSpacing > 0.0f ? integrationSpacing : 0.0f;
    const f32 boundedPixelWidth = projectedPixelWidth > 0.0f ? projectedPixelWidth : 0.0f;
    return boundedSpacing > boundedPixelWidth ? boundedSpacing : boundedPixelWidth;
}

// 固定刻みの整数セル位置から相対境界を求め、最後のセルだけで端数を吸収する。
f32 CloudRayCellOffsetForTest(
    f32 intervalSpan,
    u32 cellIndex, u32 cellCount, f32 nominalCellWidth) noexcept {
    if (cellIndex == 0u) return 0.0f;
    if (cellIndex >= cellCount) return intervalSpan;
    const f32 offset =
        static_cast<f32>(cellIndex) * nominalCellWidth;
    return offset < intervalSpan ? offset : intervalSpan;
}

// 高度から選択中の雲層内の高さ比率を求め、シェーダーと同じ範囲へ収める。
f32 CloudHeightFractionFromAltitudeForTest(f32 altitude, f32 lowerBase, f32 lowerInverseThickness, f32 upperBase, f32 upperInverseThickness, bool upperBand) noexcept {
    f32 height = (altitude - lowerBase) * lowerInverseThickness;
    if (upperBand) {
        height = (altitude - upperBase) * upperInverseThickness;
    }
    return SaturateForTest(height);
}

} // namespace

ACS_TEST(EditorStartup, FallbackSkyCompileIsBoundedAndOffOwnerThread) {
    const std::string skySource = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(skySource, "const char* kSkyHLSL"));
    EXPECT_TRUE(!shader.empty());

    // These fixed-count loops preserve every sample and its evaluation order.
    // Keeping them as real loops prevents FXC from recursively expanding each
    // 3D-noise octave into every view- and light-march call site.
    EXPECT_TRUE(Contains(
        shader, "[loop]for(inti=0;i<5;++i)"));
    EXPECT_TRUE(Contains(
        shader, "[loop]for(intl=1;l<=3;++l)"));
    EXPECT_FALSE(Contains(
        shader, "[unroll]for(inti=0;i<5;++i)"));
    EXPECT_FALSE(Contains(
        shader, "[unroll]for(intl=1;l<=3;++l)"));
    // FXC previously warned that the inlined CloudDensity3 result could be
    // uninitialized. On the fallback path that manifested as isolated dark or
    // bright speckles while the volumetric candidate was still compiling.
    EXPECT_TRUE(Contains(
        shader,
        "floatCloudDensity3(float3p,floatcoverage,floatwindOff){"));
    EXPECT_TRUE(Contains(shader, "floatresult=0.0;"));
    EXPECT_TRUE(Contains(shader, "returnresult;"));
    EXPECT_FALSE(Contains(
        shader, "if(profile<=0.001)return0.0;"));
    EXPECT_FALSE(Contains(
        shader, "if(shape<=0.0)return0.0;"));

    const std::string editorSource = ReadEditorAbiSource();
    const std::size_t workerBegin = editorSource.find(
        "void SkyCompileWorkerEntry(void* user) noexcept");
    const std::size_t workerEnd = editorSource.find(
        "bool BeginSkyCompileWorker", workerBegin);
    EXPECT_TRUE(workerBegin != std::string::npos);
    EXPECT_TRUE(workerEnd != std::string::npos);
    EXPECT_TRUE(workerBegin < workerEnd);
    const std::string worker = editorSource.substr(
        workerBegin, workerEnd - workerBegin);
    EXPECT_TRUE(Contains(worker, "CSky::CompileShadersCpu()"));
    EXPECT_FALSE(Contains(worker, "CreateRhi"));
    EXPECT_FALSE(Contains(worker, "renderer.Device()"));

    const std::string compactEditor = CompactShader(editorSource);
    EXPECT_TRUE(Contains(
        compactEditor, "h.startup_worker_kind=3u;"));
    EXPECT_TRUE(Contains(
        compactEditor, "if(BeginSkyCompileWorker(h)){"
                       "h.startup_phase_pending=true;returnfalse;}"));
    EXPECT_TRUE(Contains(
        compactEditor, "h.sky3d.InitWithCompiledShaders("
                       "*dev,Move(h.startup_sky_shaders),hdrf,df)"));
}

ACS_TEST(EditorStartup,
         VolumetricCloudCompileIsOffOwnerThreadAndCommitIsTransactional) {
    const std::string skySource = ReadSkySource();
    const std::string editorSource = ReadEditorAbiSource();
    EXPECT_TRUE(!skySource.empty());
    EXPECT_TRUE(!editorSource.empty());

    const std::size_t workerBegin = editorSource.find(
        "void CloudCompileWorkerEntry(void* user) noexcept");
    const std::size_t workerEnd = editorSource.find(
        "bool BeginCloudCompileWorker", workerBegin);
    EXPECT_TRUE(workerBegin != std::string::npos);
    EXPECT_TRUE(workerEnd != std::string::npos);
    EXPECT_TRUE(workerBegin < workerEnd);
    const std::string worker = editorSource.substr(
        workerBegin, workerEnd - workerBegin);
    EXPECT_TRUE(Contains(
        worker, "CVolumetricClouds::CompileShadersCpu()"));
    EXPECT_FALSE(Contains(worker, "CreateRhi"));
    EXPECT_FALSE(Contains(worker, "renderer.Device()"));

    const std::string compactEditor = CompactShader(editorSource);
    EXPECT_TRUE(Contains(
        compactEditor, "h.startup_worker_kind=4u;"));
    EXPECT_TRUE(Contains(
        compactEditor, "if(BeginCloudCompileWorker(h)){"
                       "h.startup_phase_pending=true;returnfalse;}"));
    EXPECT_TRUE(Contains(
        compactEditor,
        "h.vclouds3d.InitWithCompiledShaders("
        "*dev,Move(h.startup_cloud_shaders),"
        "EFormat::R16G16B16A16_Float)"));

    const std::size_t publishBegin = skySource.find(
        "TResult<void> CVolumetricClouds::InitWithCompiledShaders");
    const std::size_t candidateBuild = skySource.find(
        "candidate.InitCandidateWithCompiledShaders", publishBegin);
    const std::size_t failedBuild = skySource.find(
        "if (result.IsErr()) return result;", candidateBuild);
    const std::size_t oldShutdown = skySource.find(
        "Shutdown();", failedBuild);
    const std::size_t publish = skySource.find(
        "*this = Move(candidate);", oldShutdown);
    EXPECT_TRUE(publishBegin != std::string::npos);
    EXPECT_TRUE(candidateBuild != std::string::npos);
    EXPECT_TRUE(failedBuild != std::string::npos);
    EXPECT_TRUE(oldShutdown != std::string::npos);
    EXPECT_TRUE(publish != std::string::npos);
    EXPECT_TRUE(publishBegin < candidateBuild);
    EXPECT_TRUE(candidateBuild < failedBuild);
    EXPECT_TRUE(failedBuild < oldShutdown);
    EXPECT_TRUE(oldShutdown < publish);

    const std::size_t ownerBuildBegin = skySource.find(
        "TResult<void> CVolumetricClouds::InitCandidateWithCompiledShaders");
    const std::size_t ownerBuildEnd = skySource.find(
        "bool CVolumetricClouds::EnsureSize", ownerBuildBegin);
    EXPECT_TRUE(ownerBuildBegin != std::string::npos);
    EXPECT_TRUE(ownerBuildEnd != std::string::npos);
    EXPECT_TRUE(ownerBuildBegin < ownerBuildEnd);
    const std::string ownerBuild = skySource.substr(
        ownerBuildBegin, ownerBuildEnd - ownerBuildBegin);
    EXPECT_FALSE(Contains(ownerBuild, "CreateRhiShader"));
    EXPECT_TRUE(Contains(
        ownerBuild,
        "bool shadowOk = kVolumetricCloudShadowCacheEnabled &&"));
    EXPECT_TRUE(Contains(
        ownerBuild,
        "exact lighting fallback remains active"));
}

ACS_TEST(VolumetricClouds,
         LocalFogConsumerRejectsStaleDisabledAndOrthographicVolumes) {
    using acs::editor_render_policy::ShouldCompositeLocalFog;
    using acs::editor_render_policy::ShouldUseAnalyticLocalFog;

    // Perspective + enabled fog is the producer domain and consumes a valid
    // current volume.
    EXPECT_TRUE(ShouldCompositeLocalFog(
        false, true, true, true, true));
    EXPECT_FALSE(ShouldUseAnalyticLocalFog(
        false, true, true));

    // Fog ON -> OFF leaves the GPU texture allocated.  Availability must not
    // make that stale previous-frame volume visible after the setting changes.
    EXPECT_FALSE(ShouldCompositeLocalFog(
        false, false, true, true, true));
    EXPECT_FALSE(ShouldUseAnalyticLocalFog(
        false, false, true));

    // Perspective fog ON -> 2D orthographic also skips the producer.  The
    // volumetric and analytic consumers must both follow it instead of making
    // the 2D view depend on whether the retained texture exists.
    EXPECT_FALSE(ShouldCompositeLocalFog(
        true, true, true, true, true));
    EXPECT_FALSE(ShouldUseAnalyticLocalFog(
        true, true, true));
    EXPECT_FALSE(ShouldUseAnalyticLocalFog(
        true, true, false));

    // Existing graceful fallback remains intact when either GPU input is
    // unavailable: the volumetric composite is skipped and the established
    // analytic path continues for a perspective volume-build failure.
    EXPECT_FALSE(ShouldCompositeLocalFog(
        false, true, false, true, true));
    EXPECT_TRUE(ShouldUseAnalyticLocalFog(
        false, true, false));
    EXPECT_FALSE(ShouldCompositeLocalFog(
        false, true, true, false, true));
    EXPECT_TRUE(ShouldUseAnalyticLocalFog(
        false, true, false));

    // A generated volume still cannot be consumed without the HDR/post output
    // chain.  Keep perspective fog alive through the analytic fallback.
    EXPECT_FALSE(ShouldCompositeLocalFog(
        false, true, true, true, false));
    EXPECT_TRUE(ShouldUseAnalyticLocalFog(
        false, true, false));

    // The frame-local pointer starts empty and is assigned only immediately
    // after this frame actually invokes the producer.  The later consumers
    // share one complete availability decision instead of rereading a stale
    // retained member texture.
    const std::string editorSource = ReadEditorAbiSource();
    EXPECT_TRUE(Contains(
        editorSource, "IRhiTexture* localFogVol = nullptr;"));
    const std::size_t producerCall = editorSource.find("builtAp = h.sky_atmo.BuildAerialPerspectiveCameraRelative(");
    const std::size_t frameLocalAssignment = editorSource.find(
        "localFogVol = h.sky_atmo.LocalFogVolume();", producerCall);
    const std::size_t availabilityDecision = editorSource.find(
        "const bool canCompositeLocalFog =", frameLocalAssignment);
    const std::size_t availabilityPolicy = editorSource.find(
        "ShouldCompositeLocalFog(", availabilityDecision);
    const std::size_t depthPredicate = editorSource.find(
        "localFogSceneDepth != nullptr", availabilityPolicy);
    const std::size_t fullscreenPredicate = editorSource.find(
        "hdrRt != nullptr && scSwap != nullptr", depthPredicate);
    const std::size_t analyticPolicy = editorSource.find(
        "ShouldUseAnalyticLocalFog(", availabilityDecision);
    const std::size_t analyticUsesSharedDecision = editorSource.find(
        "canCompositeLocalFog))", analyticPolicy);
    const std::size_t consumerGate = editorSource.find(
        "if (canCompositeLocalFog)", availabilityDecision);
    EXPECT_TRUE(producerCall != std::string::npos);
    EXPECT_TRUE(frameLocalAssignment != std::string::npos);
    EXPECT_TRUE(availabilityDecision != std::string::npos);
    EXPECT_TRUE(availabilityPolicy != std::string::npos);
    EXPECT_TRUE(depthPredicate != std::string::npos);
    EXPECT_TRUE(fullscreenPredicate != std::string::npos);
    EXPECT_TRUE(analyticPolicy != std::string::npos);
    EXPECT_TRUE(analyticUsesSharedDecision != std::string::npos);
    EXPECT_TRUE(consumerGate != std::string::npos);
    EXPECT_TRUE(producerCall < frameLocalAssignment);
    EXPECT_TRUE(frameLocalAssignment < availabilityDecision);
    EXPECT_TRUE(availabilityDecision < availabilityPolicy);
    EXPECT_TRUE(availabilityPolicy < depthPredicate);
    EXPECT_TRUE(depthPredicate < fullscreenPredicate);
    EXPECT_TRUE(fullscreenPredicate < analyticPolicy);
    EXPECT_TRUE(availabilityDecision < analyticPolicy);
    EXPECT_TRUE(analyticPolicy < analyticUsesSharedDecision);
    EXPECT_TRUE(analyticUsesSharedDecision < consumerGate);
    EXPECT_TRUE(availabilityDecision < consumerGate);
    EXPECT_FALSE(Contains(
        editorSource,
        "IRhiTexture* localFogVol = h.sky_atmo.LocalFogVolume();"));
}

ACS_TEST(VolumetricClouds,
         OrthographicRenderCameraDisablesGpuAndFallbackCloudMarches) {
    const std::string editorSource =
        CompactShader(ReadEditorAbiSource());
    EXPECT_TRUE(!editorSource.empty());

    // Perspective keeps the existing CSky fallback when the volumetric path
    // is unavailable.  Orthographic mode must disable both ray marchers:
    // the GPU gate above and CSky's 48-step analytic fallback.
    const char* volumetricGate =
        "if(h.q_cloud_coverage>0.001f&&!renderOrtho&&"
        "hdrRt!=nullptr){";
    const char* fallbackGate =
        "h.sky3d.SetFallbackCloudsEnabled("
        "h.q_cloud_coverage>0.001f&&"
        "!renderOrtho&&!cloudsActive);";
    const char* modernReadyGate =
        "if(h.vclouds_ready&&"
        "h.vclouds3d.EnsureSize("
        "*cdev,scW,scH,h.q_cloud_render_scale,"
        "h.q_cloud_reference)){"
        "cloudsActive=true;}";
    EXPECT_TRUE(Contains(editorSource, volumetricGate));
    EXPECT_TRUE(Contains(editorSource, "h.vclouds3d.SetReferenceMode(h.q_cloud_reference);"));
    EXPECT_TRUE(Contains(editorSource, modernReadyGate));
    EXPECT_TRUE(Contains(editorSource, fallbackGate));
    EXPECT_FALSE(Contains(
        editorSource,
        "h.sky3d.SetFallbackCloudsEnabled("
        "h.q_cloud_coverage>0.001f&&!cloudsActive);"));
    EXPECT_FALSE(Contains(
        editorSource,
        "if(h.q_cloud_coverage>0.001f&&!h.ortho3d&&"
        "hdrRt!=nullptr){"));
    EXPECT_TRUE(
        editorSource.find(volumetricGate) <
        editorSource.find(fallbackGate));
    EXPECT_TRUE(
        editorSource.find(modernReadyGate) <
        editorSource.find(fallbackGate));
    EXPECT_EQ(
        CountOccurrences(editorSource, fallbackGate),
        static_cast<std::size_t>(1));
    EXPECT_TRUE(Contains(
        editorSource, "h.sky3d.SetFallbackCloudTime(h.time);"));

    // Editor の描画時刻はフレーム差分を加算してから 3D 描画へ渡し、
    // ボリュメトリック雲も同じ積算時刻を描画呼び出しの直前に受け取る。
    const std::size_t frameDelta = editorSource.find(
        "CommitEditorFrameDelta(*host,safe_dt);");
    const std::size_t accumulatedTime = editorSource.find(
        "host->time+=safe_dt;", frameDelta);
    const std::size_t draw3d = editorSource.find(
        "DrawScene3D(*host,sc->Width(),sc->Height());", accumulatedTime);
    const std::size_t cloudTimeCopy = editorSource.find(
        "h.vclouds_time=h.time;");
    const std::size_t cloudRender = editorSource.find(
        "h.vclouds3d.RenderComputeCameraRelative(", cloudTimeCopy);
    EXPECT_TRUE(frameDelta != std::string::npos);
    EXPECT_TRUE(accumulatedTime != std::string::npos);
    EXPECT_TRUE(draw3d != std::string::npos);
    EXPECT_TRUE(frameDelta < accumulatedTime);
    EXPECT_TRUE(accumulatedTime < draw3d);
    EXPECT_TRUE(cloudTimeCopy != std::string::npos);
    EXPECT_TRUE(cloudRender != std::string::npos);
    EXPECT_TRUE(cloudTimeCopy < cloudRender);
}

ACS_TEST(VolumetricClouds, LayerEntryRemainsWorldAnchoredAcrossCameraTranslation) {
    const FVolumetricCloudLayer layer{96.0f, 128.0f, 0.035f};
    const FVec3 worldAnchor{18.0f, layer.base_height, -27.0f};

    const FVec3 cameraA{-4.0f, 8.0f, 11.0f};
    const FVec3 directionA = NormalizeForTest(
        FVec3{worldAnchor.x - cameraA.x, worldAnchor.y - cameraA.y,
              worldAnchor.z - cameraA.z});
    const FVolumetricCloudRayInterval intervalA =
        IntersectVolumetricCloudLayer(cameraA, directionA, layer);
    EXPECT_TRUE(intervalA.hit);
    ExpectVec3Near(PointOnRay(cameraA, directionA, intervalA.enter),
                   worldAnchor, 1e-3f);

    // A vertical and lateral editor-camera pan must still intersect the same
    // authored world point, not a layer translated to camera.y + constant.
    const FVec3 cameraB{7.0f, 31.0f, -3.0f};
    const FVec3 directionB = NormalizeForTest(
        FVec3{worldAnchor.x - cameraB.x, worldAnchor.y - cameraB.y,
              worldAnchor.z - cameraB.z});
    const FVolumetricCloudRayInterval intervalB =
        IntersectVolumetricCloudLayer(cameraB, directionB, layer);
    EXPECT_TRUE(intervalB.hit);
    ExpectVec3Near(PointOnRay(cameraB, directionB, intervalB.enter),
                   worldAnchor, 1e-3f);
}

ACS_TEST(VolumetricClouds, LayerIntersectionHandlesInsideAndAboveCamera) {
    const FVolumetricCloudLayer layer{96.0f, 128.0f, 0.035f};

    const FVolumetricCloudRayInterval inside =
        IntersectVolumetricCloudLayer(FVec3{0.0f, 110.0f, 0.0f},
                                      FVec3{0.0f, 1.0f, 0.0f}, layer);
    EXPECT_TRUE(inside.hit);
    EXPECT_NEAR(inside.enter, 0.0f, 1e-6f);
    EXPECT_NEAR(inside.exit, 18.0f, 1e-5f);

    const FVolumetricCloudRayInterval above =
        IntersectVolumetricCloudLayer(FVec3{0.0f, 140.0f, 0.0f},
                                      FVec3{0.0f, 1.0f, 0.0f}, layer);
    EXPECT_FALSE(above.hit);

    const FVolumetricCloudRayInterval aboveLookingDown =
        IntersectVolumetricCloudLayer(FVec3{0.0f, 140.0f, 0.0f},
                                      FVec3{0.0f, -1.0f, 0.0f}, layer);
    EXPECT_TRUE(aboveLookingDown.hit);
    EXPECT_NEAR(aboveLookingDown.enter, 12.0f, 1e-5f);
    EXPECT_NEAR(aboveLookingDown.exit, 44.0f, 1e-5f);

    const FVolumetricCloudRayInterval insideLookingDown =
        IntersectVolumetricCloudLayer(FVec3{0.0f, 110.0f, 0.0f},
                                      FVec3{0.0f, -1.0f, 0.0f}, layer);
    EXPECT_TRUE(insideLookingDown.hit);
    EXPECT_NEAR(insideLookingDown.enter, 0.0f, 1e-6f);
    EXPECT_NEAR(insideLookingDown.exit, 14.0f, 1e-5f);
}

ACS_TEST(VolumetricClouds,
         DensityPipelinePreservesContinuousExtinctionVariation) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader,
        "densityDistribution=cloudScaleDensityDistribution("
        "cloudDetailFilteredCondensationDistribution("
        "macro.shapePotential,macro.shapeFrequencyWeights,"
        "commonPotential,billowPotentials,"
        "billowVisibility,middleBillowVisibility,"
        "erosionPotential,erosionVisibility),densityScale);"));
    EXPECT_TRUE(Contains(
        shader,
        "returncloudScaleDensityDistribution("
        "condensationDistribution,"
        "cloudHeightPrecipitationDensityScale(h,macro.weather.b));"));
    EXPECT_FALSE(Contains(shader, "cloudInteriorDensityContrast("));
    EXPECT_FALSE(Contains(shader, "cloudTopReliefDensity("));
    EXPECT_FALSE(Contains(shader, "floatinteriorLobe="));
    EXPECT_FALSE(Contains(shader, "densityResult=saturate(d*densityScale);"));

    // 密度は確率ではなく消散係数の倍率なので、1を越える値も切り捨てない。
    constexpr f32 densityScale = 1.20f;
    constexpr f32 lowDensity = 0.20f * densityScale;
    constexpr f32 middleDensity = 0.50f * densityScale;
    constexpr f32 denseDensity = 1.10f * densityScale;
    EXPECT_TRUE(lowDensity < middleDensity);
    EXPECT_TRUE(middleDensity < denseDensity);
    EXPECT_TRUE(denseDensity > 1.0f);
}

ACS_TEST(VolumetricClouds, LayerSettingsAreSanitized) {
    CVolumetricClouds clouds;
    clouds.SetLayer(FVolumetricCloudLayer{20.0f, 10.0f, 0.0f});

    EXPECT_NEAR(clouds.Layer().base_height, 10.0f, 1e-6f);
    EXPECT_NEAR(clouds.Layer().top_height, 20.0f, 1e-6f);
    EXPECT_NEAR(clouds.Layer().horizontal_noise_scale, 0.001f, 1e-6f);

    clouds.SetLayer(FVolumetricCloudLayer{30.0f, 30.1f, 2.0f});

    EXPECT_NEAR(clouds.Layer().base_height, 30.0f, 1e-6f);
    EXPECT_NEAR(clouds.Layer().top_height, 30.25f, 1e-6f);
    EXPECT_NEAR(clouds.Layer().horizontal_noise_scale, 1.0f, 1e-6f);
}

ACS_TEST(VolumetricClouds, NonFiniteLayerSettingsUseFiniteDefaults) {
    CVolumetricClouds clouds;
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();

    clouds.SetLayer(FVolumetricCloudLayer{nan, infinity, -infinity});

    const FVolumetricCloudLayer& layer = clouds.Layer();
    EXPECT_TRUE(std::isfinite(layer.base_height));
    EXPECT_TRUE(std::isfinite(layer.top_height));
    EXPECT_TRUE(std::isfinite(layer.horizontal_noise_scale));
    EXPECT_NEAR(layer.base_height, 1500.0f, 1e-6f);
    EXPECT_NEAR(layer.top_height, 4000.0f, 1e-6f);
    EXPECT_NEAR(layer.horizontal_noise_scale, 0.035f, 1e-6f);
}

ACS_TEST(VolumetricClouds, UnrepresentableLayerThicknessUsesFiniteDefaults) {
    CVolumetricClouds clouds;
    const f32 maximum = std::numeric_limits<f32>::max();

    clouds.SetLayer(FVolumetricCloudLayer{maximum, maximum, 2.0f});

    const FVolumetricCloudLayer& layer = clouds.Layer();
    EXPECT_NEAR(layer.base_height, 1500.0f, 1e-6f);
    EXPECT_NEAR(layer.top_height, 4000.0f, 1e-6f);
    EXPECT_NEAR(layer.horizontal_noise_scale, 1.0f, 1e-6f);
}

ACS_TEST(VolumetricClouds,
         TraceQualityMultiplierIsMonotonicFromEighthToFullResolution) {
    EXPECT_EQ(kVolumetricCloudUltraTraceDivisor, 4u);
    EXPECT_NEAR(
        SanitizeVolumetricCloudQualityMultiplier(0.0f), 0.5f, 1e-6f);
    EXPECT_NEAR(
        SanitizeVolumetricCloudQualityMultiplier(1.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(
        SanitizeVolumetricCloudQualityMultiplier(4.0f), 4.0f, 1e-6f);
    EXPECT_NEAR(
        SanitizeVolumetricCloudQualityMultiplier(8.0f), 4.0f, 1e-6f);
    EXPECT_NEAR(
        SanitizeVolumetricCloudQualityMultiplier(
            std::numeric_limits<f32>::quiet_NaN()),
        1.0f, 1e-6f);

    const FVolumetricCloudTraceResolution ultra =
        ResolveVolumetricCloudTraceResolution(1920u, 1080u, 1.0f);
    EXPECT_NEAR(ultra.quality_multiplier, 1.0f, 1e-6f);
    EXPECT_NEAR(ultra.effective_dimension_scale, 0.25f, 1e-6f);
    EXPECT_EQ(ultra.width, 480u);
    EXPECT_EQ(ultra.height, 270u);

    const FVolumetricCloudTraceResolution oddUltra =
        ResolveVolumetricCloudTraceResolution(1919u, 1079u, 1.0f);
    EXPECT_EQ(oddUltra.width, 480u);
    EXPECT_EQ(oddUltra.height, 270u);

    // CloudRenderScale is an authored multiplier over the internal policy,
    // not an absolute viewport scale. Lower quality therefore always launches
    // less work than Ultra without changing the full resolved dimensions.
    const FVolumetricCloudTraceResolution high =
        ResolveVolumetricCloudTraceResolution(1920u, 1080u, 0.75f);
    EXPECT_NEAR(high.quality_multiplier, 0.75f, 1e-6f);
    EXPECT_NEAR(high.effective_dimension_scale, 0.1875f, 1e-6f);
    EXPECT_EQ(high.width, 360u);
    EXPECT_EQ(high.height, 203u);

    const FVolumetricCloudTraceResolution low =
        ResolveVolumetricCloudTraceResolution(1920u, 1080u, 0.50f);
    EXPECT_NEAR(low.quality_multiplier, 0.50f, 1e-6f);
    EXPECT_NEAR(low.effective_dimension_scale, 0.125f, 1e-6f);
    EXPECT_EQ(low.width, 240u);
    EXPECT_EQ(low.height, 135u);
    EXPECT_TRUE(low.width <= high.width);
    EXPECT_TRUE(high.width <= ultra.width);
    EXPECT_TRUE(low.height <= high.height);
    EXPECT_TRUE(high.height <= ultra.height);

    const FVolumetricCloudTraceResolution clampedLow =
        ResolveVolumetricCloudTraceResolution(1919u, 1079u, 0.25f);
    EXPECT_NEAR(clampedLow.quality_multiplier, 0.50f, 1e-6f);
    EXPECT_NEAR(clampedLow.effective_dimension_scale, 0.125f, 1e-6f);
    EXPECT_EQ(clampedLow.width, 240u);
    EXPECT_EQ(clampedLow.height, 135u);

    const FVolumetricCloudTraceResolution clampedHigh =
        ResolveVolumetricCloudTraceResolution(1919u, 1079u, 8.0f);
    EXPECT_NEAR(
        clampedHigh.quality_multiplier,
        static_cast<f32>(kVolumetricCloudUltraTraceDivisor), 1e-6f);
    EXPECT_NEAR(clampedHigh.effective_dimension_scale, 1.0f, 1e-6f);
    EXPECT_EQ(clampedHigh.width, 1919u);
    EXPECT_EQ(clampedHigh.height, 1079u);
    EXPECT_TRUE(ultra.width <= clampedHigh.width);
    EXPECT_TRUE(ultra.height <= clampedHigh.height);

    const FVolumetricCloudTraceResolution reference =
        ResolveVolumetricCloudTraceResolution(
            1919u, 1079u, 0.5f, true);
    EXPECT_NEAR(reference.effective_dimension_scale, 1.0f, 1e-6f);
    EXPECT_EQ(reference.width, 1919u);
    EXPECT_EQ(reference.height, 1079u);

    const FVolumetricCloudTraceResolution sanitized =
        ResolveVolumetricCloudTraceResolution(
            0u, 0u, std::numeric_limits<f32>::quiet_NaN());
    EXPECT_NEAR(sanitized.quality_multiplier, 1.0f, 1e-6f);
    EXPECT_NEAR(sanitized.effective_dimension_scale, 0.25f, 1e-6f);
    EXPECT_EQ(sanitized.width, 1u);
    EXPECT_EQ(sanitized.height, 1u);
}

ACS_TEST(VolumetricClouds,
         EditorUsesTheSharedFullResolutionQualityRange) {
    const std::string editorSource =
        CompactShader(ReadEditorAbiSource());
    EXPECT_TRUE(!editorSource.empty());
    EXPECT_TRUE(Contains(
        editorSource,
        "h.q_cloud_render_scale="
        "SanitizeVolumetricCloudQualityMultiplier(cloudRenderScale);"));
    EXPECT_FALSE(Contains(
        editorSource,
        "cloudRenderScale>1.0f?1.0f:cloudRenderScale"));
    EXPECT_TRUE(Contains(editorSource, "h.q_cloud_reference=h.settings.GetBool(\"Rendering\",\"CloudReferenceMode\",false);"));
    EXPECT_TRUE(Contains(editorSource, "h.q_cloud_max_distance=h.settings.GetFloat(\"Rendering\",\"CloudMaxDistance\",60000.0f);"));
    EXPECT_TRUE(Contains(editorSource, "h.q_cloud_upper_base=h.settings.GetFloat(\"Rendering\",\"CloudUpperBaseHeight\",0.0f);"));
    EXPECT_TRUE(Contains(editorSource, "h.q_cloud_upper_top=h.settings.GetFloat(\"Rendering\",\"CloudUpperTopHeight\",0.0f);"));
    EXPECT_TRUE(Contains(editorSource, "h.q_cloud_upper_coverage=h.settings.GetFloat(\"Rendering\",\"CloudUpperCoverageScale\",0.55f);"));
    EXPECT_TRUE(Contains(editorSource, "h.q_cloud_upper_density=h.settings.GetFloat(\"Rendering\",\"CloudUpperDensityScale\",0.30f);"));
    EXPECT_TRUE(Contains(editorSource, "h.q_cloud_fade_fraction=h.settings.GetFloat(\"Rendering\",\"CloudFadeFraction\",0.35f);"));
    EXPECT_TRUE(Contains(editorSource, "h.q_cloud_step_growth=h.settings.GetFloat(\"Rendering\",\"CloudStepGrowth\",0.0f);"));
    EXPECT_TRUE(Contains(editorSource, "h.q_cloud_type=h.settings.GetFloat(\"Rendering\",\"CloudType\",0.78f);"));
    EXPECT_TRUE(Contains(editorSource, "h.q_cloud_type_influence=h.settings.GetFloat(\"Rendering\",\"CloudTypeInfluence\",0.0f);"));
    EXPECT_TRUE(Contains(editorSource, "h.q_cloud_precipitation=h.settings.GetFloat(\"Rendering\",\"CloudPrecipitation\",0.0f);"));
    EXPECT_TRUE(Contains(editorSource, "h.q_cloud_precipitation_influence=h.settings.GetFloat(\"Rendering\",\"CloudPrecipitationInfluence\",0.0f);"));
    EXPECT_TRUE(Contains(editorSource, "cloudRange.MaxDistance=host.q_cloud_max_distance;"));
    EXPECT_TRUE(Contains(editorSource, "cloudRange.FadeFraction=host.q_cloud_fade_fraction;"));
    EXPECT_TRUE(Contains(editorSource, "cloudRange.StepGrowth=host.q_cloud_step_growth;"));
    EXPECT_TRUE(Contains(editorSource, "host.vclouds3d.SetRange(cloudRange);"));
    EXPECT_TRUE(Contains(editorSource, "cloudWeather.CloudType=host.q_cloud_type;"));
    EXPECT_TRUE(Contains(editorSource, "cloudWeather.CloudTypeInfluence=host.q_cloud_type_influence;"));
    EXPECT_TRUE(Contains(editorSource, "cloudWeather.Precipitation=host.q_cloud_precipitation;"));
    EXPECT_TRUE(Contains(editorSource, "cloudWeather.PrecipitationInfluence=host.q_cloud_precipitation_influence;"));
    EXPECT_TRUE(Contains(editorSource, "host.vclouds3d.SetWeather(cloudWeather);"));
    EXPECT_TRUE(Contains(editorSource, "host.vclouds3d.SetUpperLayer(FVolumetricCloudUpperLayer{host.q_cloud_upper_base,host.q_cloud_upper_top,host.q_cloud_upper_coverage,host.q_cloud_upper_density});"));
    EXPECT_TRUE(Contains(editorSource, "host.q_cloud_render_scale,host.q_cloud_reference);"));
}

ACS_TEST(VolumetricClouds,
         WorkloadDiagnosticsSeparateSteadyBakeAndShadowDispatches) {
    FVolumetricCloudFrameWorkloadPlan steadyPlan{};
    steadyPlan.trace_width = 480u;
    steadyPlan.trace_height = 270u;
    steadyPlan.output_width = 1920u;
    steadyPlan.output_height = 1080u;
    steadyPlan.shadow_update_divisor = kVolumetricCloudShadowTemporalDivisor;
    steadyPlan.rebuild_shadow_cache = true;
    steadyPlan.rebuild_world_shadow = true;
    const FVolumetricCloudFrameWorkload steady =
        PlanVolumetricCloudFrameWorkload(steadyPlan);

    EXPECT_EQ(steady.steady_dispatches, 2u);
    EXPECT_EQ(steady.one_time_bake_dispatches, 0u);
    EXPECT_EQ(steady.shadow_cache_dispatches, 1u);
    EXPECT_EQ(steady.world_shadow_dispatches, 1u);
    EXPECT_EQ(steady.total_compute_dispatches, 4u);
    EXPECT_EQ(steady.trace_logical_invocations, 129600u);
    EXPECT_EQ(steady.trace_launched_threads, 130560u);
    EXPECT_EQ(steady.resolve_logical_invocations, 2073600u);
    EXPECT_EQ(steady.resolve_launched_threads, 2073600u);
    EXPECT_EQ(steady.shadow_cache_logical_invocations, 36864u);
    EXPECT_EQ(steady.shadow_cache_launched_threads, 36864u);
    EXPECT_EQ(steady.world_shadow_logical_invocations, 16384u);
    EXPECT_EQ(steady.world_shadow_launched_threads, 16384u);
    EXPECT_EQ(steady.total_logical_invocations, 2256448u);
    EXPECT_EQ(steady.total_launched_threads, 2257408u);
    EXPECT_EQ(steady.maximum_view_samples, 199065600u);
    EXPECT_EQ(
        steady.maximum_light_samples,
        steady.maximum_view_samples *
            (4u * kVolumetricCloudMaxLightMarchSamples + 3u));
    EXPECT_EQ(
        steady.maximum_light_samples,
        steady.maximum_view_samples *
            (4u * kVolumetricCloudMaxLightMarchSamples + 3u));
    EXPECT_EQ(steady.maximum_world_shadow_samples, 524288u);
    EXPECT_TRUE(steady.temporal_super_resolution);
    EXPECT_FALSE(steady.attempted);
    EXPECT_FALSE(steady.submitted);

    FVolumetricCloudFrameWorkloadPlan referencePlan = steadyPlan;
    referencePlan.maximum_view_steps =
        kVolumetricCloudReferenceViewSteps;
    referencePlan.shadow_update_divisor = 1u;
    const FVolumetricCloudFrameWorkload reference =
        PlanVolumetricCloudFrameWorkload(referencePlan);
    EXPECT_EQ(reference.maximum_view_samples, 265420800u);
    EXPECT_EQ(
        reference.maximum_light_samples,
        reference.maximum_view_samples *
            (4u * kVolumetricCloudMaxLightMarchSamples + 3u));
    EXPECT_EQ(
        reference.maximum_light_samples,
        reference.maximum_view_samples *
            (4u * kVolumetricCloudMaxLightMarchSamples + 3u));
    EXPECT_EQ(reference.shadow_cache_logical_invocations, 147456u);
    EXPECT_EQ(reference.shadow_cache_launched_threads, 147456u);
    EXPECT_EQ(reference.world_shadow_logical_invocations, 65536u);
    EXPECT_EQ(reference.maximum_world_shadow_samples, 2097152u);

    FVolumetricCloudFrameWorkloadPlan coldPlan = steadyPlan;
    coldPlan.bake_shape_noise = true;
    coldPlan.bake_weather = true;
    coldPlan.bake_detail_noise = true;
    coldPlan.bake_curl_noise = true;
    coldPlan.shadow_update_divisor = 1u;
    coldPlan.rebuild_shadow_cache = true;
    const FVolumetricCloudFrameWorkload cold =
        PlanVolumetricCloudFrameWorkload(coldPlan);

    EXPECT_EQ(cold.steady_dispatches, 2u);
    EXPECT_EQ(cold.one_time_bake_dispatches, 7u);
    EXPECT_EQ(cold.shadow_cache_dispatches, 1u);
    EXPECT_EQ(cold.world_shadow_dispatches, 1u);
    EXPECT_EQ(cold.total_compute_dispatches, 11u);
    EXPECT_EQ(cold.one_time_bake_logical_invocations, 8929280u);
    EXPECT_EQ(cold.one_time_bake_launched_threads, 8929280u);
    EXPECT_EQ(cold.shadow_cache_logical_invocations, 147456u);
    EXPECT_EQ(cold.shadow_cache_launched_threads, 147456u);
    EXPECT_EQ(cold.world_shadow_logical_invocations, 65536u);
    EXPECT_EQ(cold.world_shadow_launched_threads, 65536u);
    EXPECT_EQ(cold.total_logical_invocations, 11345472u);
    EXPECT_EQ(cold.total_launched_threads, 11346432u);
    EXPECT_EQ(cold.maximum_view_samples, steady.maximum_view_samples);
    EXPECT_EQ(cold.maximum_light_samples, steady.maximum_light_samples);
    EXPECT_EQ(cold.maximum_world_shadow_samples, 2097152u);
}

ACS_TEST(VolumetricClouds,
         WorkloadDiagnosticsAccountForPaddingAndSaturateHostileSizes) {
    // 内部の準備状態を公開入力へ足さず、既存の公開診断入力の大きさを維持する。
    EXPECT_EQ(sizeof(FVolumetricCloudFrameWorkloadPlan), 32u);

    FVolumetricCloudFrameWorkloadPlan oddPlan{};
    oddPlan.trace_width = 480u;
    oddPlan.trace_height = 270u;
    oddPlan.output_width = 1919u;
    oddPlan.output_height = 1079u;
    const FVolumetricCloudFrameWorkload odd =
        PlanVolumetricCloudFrameWorkload(oddPlan);
    EXPECT_EQ(odd.trace_logical_invocations, 129600u);
    EXPECT_EQ(odd.trace_launched_threads, 130560u);
    EXPECT_EQ(odd.resolve_logical_invocations, 2070601u);
    EXPECT_EQ(odd.resolve_launched_threads, 2073600u);
    EXPECT_TRUE(odd.temporal_super_resolution);

    FVolumetricCloudFrameWorkloadPlan hostilePlan{};
    hostilePlan.trace_width = std::numeric_limits<u32>::max();
    hostilePlan.trace_height = std::numeric_limits<u32>::max();
    hostilePlan.output_width = std::numeric_limits<u32>::max();
    hostilePlan.output_height = std::numeric_limits<u32>::max();
    const FVolumetricCloudFrameWorkload hostile =
        PlanVolumetricCloudFrameWorkload(hostilePlan);
    const u64 maximumU32 = std::numeric_limits<u32>::max();
    const u64 maximumLogical2D = maximumU32 * maximumU32;
    EXPECT_EQ(
        hostile.trace_logical_invocations,
        maximumLogical2D);
    EXPECT_EQ(
        hostile.trace_launched_threads,
        std::numeric_limits<u64>::max());
    EXPECT_EQ(
        hostile.resolve_logical_invocations,
        maximumLogical2D);
    EXPECT_EQ(
        hostile.total_logical_invocations,
        std::numeric_limits<u64>::max());
    EXPECT_EQ(
        hostile.total_launched_threads,
        std::numeric_limits<u64>::max());
    EXPECT_EQ(
        hostile.maximum_view_samples,
        std::numeric_limits<u64>::max());
    EXPECT_EQ(
        hostile.maximum_light_samples,
        std::numeric_limits<u64>::max());
    EXPECT_FALSE(hostile.temporal_super_resolution);

    FVolumetricCloudFrameWorkloadPlan unsupportedShadowPlan{};
    unsupportedShadowPlan.shadow_update_divisor = 3u;
    unsupportedShadowPlan.rebuild_shadow_cache = true;
    unsupportedShadowPlan.rebuild_world_shadow = true;
    const FVolumetricCloudFrameWorkload unsupportedShadow = PlanVolumetricCloudFrameWorkload(unsupportedShadowPlan);
    EXPECT_EQ(unsupportedShadow.shadow_cache_logical_invocations, 147456u);
    EXPECT_EQ(unsupportedShadow.world_shadow_logical_invocations, 65536u);

}

ACS_TEST(VolumetricClouds,
         RuntimeWorkloadPublicationFollowsActualDispatchAndCompositeSites) {
    const std::string source = ReadSkySource();
    const std::string compact = CompactShader(source);
    EXPECT_TRUE(!compact.empty());

    EXPECT_TRUE(Contains(
        compact,
        "m_LastFrameWorkload={};"
        "m_LastFrameWorkload.attempted=true;"));
    EXPECT_FALSE(Contains(compact, "constboolhistoryWasAvailable=m_HistoryValid;m_WorldShadowValid=false;m_LastFrameWorkload={};"));
    EXPECT_TRUE(Contains(
        compact,
        "m_ShadowCacheValid=false;m_WorldShadowValid=false;"
        "SetShadowCacheWarmupMask_Internal(0u);"
        "SetWorldShadowWarmupMask_Internal(0u);"
        "m_LastFrameWorkload.skip_reason="
        "EVolumetricCloudFrameSkipReason::ResourcesNotReady;return;"));
    EXPECT_TRUE(CountOccurrences(compact, "m_ShadowCacheValid=false;m_WorldShadowValid=false;") >= static_cast<std::size_t>(3));
    EXPECT_TRUE(Contains(
        compact,
        "m_LastFrameWorkload="
        "PlanVolumetricCloudFrameWorkload_Internal("
        "workloadPlan,workloadOptions);"));
    EXPECT_TRUE(Contains(
        compact,
        "if(bakeShapeNoiseThisFrame){"));
    EXPECT_TRUE(Contains(
        compact,
        "if(bakeWeatherThisFrame){"));
    EXPECT_TRUE(Contains(
        compact,
        "if(bakeDetailNoiseThisFrame){"));
    EXPECT_TRUE(Contains(
        compact,
        "if(bakeCurlNoiseThisFrame){"));
    EXPECT_TRUE(Contains(
        compact,
        "if(rebuildShadowCacheThisFrame){"));
    const std::size_t densityPreparation = compact.find(
        "if(preparingDensityFields){");
    const std::size_t cloudDispatch = compact.find(
        "cl.SetComputePipeline(*m_CloudPipe);", densityPreparation);
    EXPECT_TRUE(densityPreparation != std::string::npos);
    EXPECT_TRUE(cloudDispatch != std::string::npos);
    EXPECT_TRUE(densityPreparation < cloudDispatch);
    EXPECT_FALSE(Contains(compact, "if(preparingShadowCache){"));
    EXPECT_TRUE(Contains(
        compact,
        "workloadOptions.render_cloud=!preparingDensityFields;"));
    EXPECT_TRUE(Contains(
        compact,
        "workloadOptions.shape_bake_dispatches="
        "bakeShapeNoiseThisFrame?1u:0u;"));
    EXPECT_TRUE(Contains(
        compact,
        "m_LastFrameWorkload.submitted=true;"));
    EXPECT_TRUE(Contains(
        compact,
        "if((recordedCloudFrame||m_LastFrameWorkload.submitted)&&"
        "m_LastFrameWorkload.composite_draws!=~u32{0}){"));
    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 384u);
    EXPECT_EQ(kVolumetricCloudMaxLightMarchSamples, 16u);
}

ACS_TEST(VolumetricClouds, MarchPlanUsesWorldDistanceInsteadOfHeightSlices) {
    EXPECT_NEAR(kVolumetricCloudMaxDistance, 250000.0f, 1e-3f);
    const FVolumetricCloudLayer layer{96.0f, 128.0f, 0.035f};
    const FVolumetricCloudMarchPlan vertical =
        PlanVolumetricCloudRayMarch(FVec3{0.0f, 8.0f, 0.0f},
                                    FVec3{0.0f, 1.0f, 0.0f}, layer);
    const FVolumetricCloudMarchPlan oblique =
        PlanVolumetricCloudRayMarch(FVec3{0.0f, 8.0f, 0.0f},
                                    NormalizeForTest(FVec3{1.0f, 0.2f, 0.0f}),
                                    layer);

    EXPECT_TRUE(vertical.hit);
    EXPECT_TRUE(oblique.hit);
    EXPECT_NEAR(vertical.fine_step, 1.0f, 1e-6f);
    EXPECT_TRUE(oblique.fine_step >= vertical.fine_step);
    EXPECT_TRUE(oblique.coarse_step >= vertical.coarse_step);
    EXPECT_TRUE(oblique.exit - oblique.enter >
                vertical.exit - vertical.enter);
    EXPECT_TRUE(vertical.fine_step * 336.0f >=
                 vertical.exit - vertical.enter);
    EXPECT_TRUE(oblique.fine_step * 336.0f >=
                 oblique.exit - oblique.enter);
    EXPECT_TRUE(vertical.coarse_step * 168.0f >=
                 vertical.exit - vertical.enter);
    EXPECT_TRUE(oblique.coarse_step * 168.0f >=
                 oblique.exit - oblique.enter);
    EXPECT_EQ(vertical.max_samples, 384u);
    EXPECT_EQ(oblique.max_samples, 384u);
}

ACS_TEST(VolumetricClouds,
         ProductionMarchBudgetHalvesTheFormerNearCloudInterval) {
    const FVolumetricCloudLayer layer{2600.0f, 5200.0f, 0.035f};
    const FVec3 camera{0.0f, 0.0f, 0.0f};
    const FVec3 upward{0.0f, 1.0f, 0.0f};
    const FVolumetricCloudMarchPlan former =
        PlanVolumetricCloudRayMarch(
            camera, upward, layer, kVolumetricCloudMaxDistance,
            FVec3{}, 192u);
    const FVolumetricCloudMarchPlan production =
        PlanVolumetricCloudRayMarch(
            camera, upward, layer, kVolumetricCloudMaxDistance,
            FVec3{}, kVolumetricCloudViewSteps);

    EXPECT_TRUE(former.hit);
    EXPECT_TRUE(production.hit);
    EXPECT_EQ(former.max_samples, 192u);
    EXPECT_EQ(production.max_samples, 384u);
    EXPECT_NEAR(production.enter, former.enter, 1e-4f);
    EXPECT_NEAR(production.exit, former.exit, 1e-4f);
    // 2600 m厚の雲層では、旧上限が約15.5 m間隔だったのに対し、
    // 通常描画は約7.7 m間隔となり、小さな雲塊を飛び越えにくくする。
    EXPECT_TRUE(former.fine_step > 15.0f);
    EXPECT_TRUE(production.fine_step < 8.0f);
    EXPECT_NEAR(
        production.fine_step * 2.0f, former.fine_step, 1e-4f);
    EXPECT_NEAR(
        production.coarse_step * 2.0f, former.coarse_step, 1e-4f);
}

ACS_TEST(VolumetricClouds, MarchPlanFadesAndBoundsGrazingRays) {
    const FVolumetricCloudLayer layer{96.0f, 128.0f, 0.035f};
    const FVolumetricCloudMarchPlan stable =
        PlanVolumetricCloudRayMarch(FVec3{0.0f, 8.0f, 0.0f},
                                    NormalizeForTest(FVec3{1.0f, 0.08f, 0.0f}),
                                    layer);
    EXPECT_TRUE(stable.hit);
    EXPECT_TRUE(stable.exit <= 250000.0f);
    EXPECT_TRUE(stable.visibility > 0.0f);
    EXPECT_TRUE(stable.visibility <= 1.0f);

    const FVolumetricCloudMarchPlan horizon =
        PlanVolumetricCloudRayMarch(FVec3{0.0f, 8.0f, 0.0f},
                                    NormalizeForTest(FVec3{1.0f, 0.02f, 0.0f}),
                                    layer);
    EXPECT_TRUE(horizon.hit);
    EXPECT_TRUE(horizon.visibility > 0.0f);
    EXPECT_TRUE(horizon.visibility <= 1.0f);
    EXPECT_TRUE(horizon.fine_step * 336.0f >=
                 horizon.exit - horizon.enter);
    EXPECT_TRUE(horizon.coarse_step * 168.0f >=
                 horizon.exit - horizon.enter);
}

ACS_TEST(VolumetricClouds,
         DistanceFadeWeightsEachDensitySampleInsteadOfTheFinishedRay) {
    constexpr f32 maximumDistance = 60000.0f;
    constexpr f32 fadeFraction = 0.35f;
    constexpr f32 fadeStart = maximumDistance * (1.0f - fadeFraction);
    constexpr f32 fadeMiddle = (fadeStart + maximumDistance) * 0.5f;

    EXPECT_NEAR(EvaluateVolumetricCloudDistanceFade(0.0f, maximumDistance, fadeFraction), 1.0f, 0.0f);
    EXPECT_NEAR(EvaluateVolumetricCloudDistanceFade(fadeStart, maximumDistance, fadeFraction), 1.0f, 1e-6f);
    EXPECT_NEAR(EvaluateVolumetricCloudDistanceFade(fadeMiddle, maximumDistance, fadeFraction), 0.5f, 1e-6f);
    EXPECT_NEAR(EvaluateVolumetricCloudDistanceFade(maximumDistance, maximumDistance, fadeFraction), 0.0f, 0.0f);
    EXPECT_NEAR(EvaluateVolumetricCloudDistanceFade(maximumDistance - 1.0f, maximumDistance, 0.0f), 1.0f, 0.0f);
    EXPECT_NEAR(EvaluateVolumetricCloudDistanceFade(maximumDistance, maximumDistance, 0.0f), 0.0f, 0.0f);
    EXPECT_NEAR(EvaluateVolumetricCloudDistanceFade(std::numeric_limits<f32>::quiet_NaN(), maximumDistance, fadeFraction), 0.0f, 0.0f);
    EXPECT_NEAR(EvaluateVolumetricCloudDistanceFade(std::numeric_limits<f32>::infinity(), maximumDistance, fadeFraction), 0.0f, 0.0f);
    EXPECT_NEAR(EvaluateVolumetricCloudDistanceFade(-std::numeric_limits<f32>::infinity(), maximumDistance, fadeFraction), 0.0f, 0.0f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(shader, "floatMAX_DISTANCE=min(cloudRange.x,max(cloudRange.w,1.0));"));
    EXPECT_TRUE(Contains(shader, "floatfadeStart=MAX_DISTANCE*fadeStartRatio;"));
    EXPECT_FALSE(Contains(shader, "firstDistanceFade"));
    EXPECT_TRUE(Contains(shader, "floatcurrentDistanceFade=cloudDistanceFade(currentSampleT,fadeStart,maximumDistance);"));
    EXPECT_TRUE(Contains(
        shader,
        "detailedDistribution=cloudScaleDensityDistribution("
        "detailedDistribution,opticalDensityScale);"));
    EXPECT_TRUE(Contains(
        shader,
        "cloudLightingSourceAtPoint("
        "currentP,currentMacro,lightingContext,lowLodDensity,rayDirection);"));
    EXPECT_TRUE(Contains(
        shader,
        "macro,lowLodDensity.xxxx,context.density,ambientExtinction"));
    EXPECT_FALSE(Contains(shader, "lowLodDensity*context.density*distanceFade"));
    EXPECT_TRUE(Contains(
        shader,
        "floatfadeResult=0.0;"
        "floatfadeLength=maxDistance-fadeStart;"
        "if(sampleDistance==sampleDistance){"
        "if(fadeLength>0.001){"
        "floatblend=saturate((sampleDistance-fadeStart)/fadeLength);"
        "blend=blend*blend*(3.0-2.0*blend);"
        "fadeResult=1.0-blend;"
        "}elseif(sampleDistance<maxDistance){fadeResult=1.0;}"
        "}returnfadeResult;}"));
    EXPECT_FALSE(Contains(
        shader, "smoothstep(fadeStart,maxDistance,sampleDistance)"));
    EXPECT_FALSE(Contains(
        shader, "if(fadeLength<=0.001){return"));
    EXPECT_TRUE(Contains(shader, "floatresolvedA=baseA;"));
    EXPECT_FALSE(Contains(shader, "smoothstep(cloudRange.y,MAX_DISTANCE,t0)"));
    EXPECT_FALSE(Contains(shader, "floathFade="));
    EXPECT_FALSE(Contains(shader, "resolvedA=saturate(baseA*"));
}

ACS_TEST(VolumetricClouds,
         AdaptiveMarchBudgetCoversTheCompleteVisibleInterval) {
    const FVolumetricCloudLayer layer{96.0f, 128.0f, 0.035f};
    const FVec3 directions[]{
        FVec3{0.0f, 1.0f, 0.0f},
        NormalizeForTest(FVec3{1.0f, 0.20f, 0.0f}),
        NormalizeForTest(FVec3{1.0f, 0.02f, 0.0f}),
    };

    for (const FVec3 direction : directions) {
        const FVolumetricCloudMarchPlan plan =
            PlanVolumetricCloudRayMarch(
                FVec3{0.0f, 8.0f, 0.0f}, direction, layer);
        EXPECT_TRUE(plan.hit);
        const f32 span = plan.exit - plan.enter;
        EXPECT_TRUE(plan.fine_step * 336.0f >= span);
        EXPECT_TRUE(plan.coarse_step * 168.0f >= span);
        EXPECT_EQ(plan.max_samples, 384u);
    }

    const FVec3 horizonDirection =
        NormalizeForTest(FVec3{1.0f, 0.02f, 0.0f});
    const FVolumetricCloudMarchPlan minimum = PlanVolumetricCloudRayMarch(FVec3{0.0f, 8.0f, 0.0f}, horizonDirection, layer, kVolumetricCloudMaxDistance, FVec3{}, kVolumetricCloudMinViewSteps);
    const FVolumetricCloudMarchPlan reference = PlanVolumetricCloudRayMarch(FVec3{0.0f, 8.0f, 0.0f}, horizonDirection, layer, kVolumetricCloudMaxDistance, FVec3{}, kVolumetricCloudReferenceViewSteps);
    const f32 minimumSpan = minimum.exit - minimum.enter;
    const f32 referenceSpan = reference.exit - reference.enter;
    EXPECT_EQ(minimum.max_samples, 32u);
    EXPECT_TRUE(minimum.fine_step * 28.0f >= minimumSpan);
    EXPECT_TRUE(minimum.coarse_step * 14.0f >= minimumSpan);
    EXPECT_EQ(reference.max_samples, 512u);
    EXPECT_TRUE(reference.fine_step * 448.0f >= referenceSpan);
    EXPECT_TRUE(reference.coarse_step * 224.0f >= referenceSpan);
    EXPECT_TRUE(reference.fine_step < minimum.fine_step);

}

ACS_TEST(VolumetricClouds,
         IntegerCellBoundariesReachFarSingleAndSplitBandEnds) {
    FVolumetricCloudRange range{};
    range.StepGrowth = 0.0f;
    const FVolumetricCloudUpperLayer disabledUpper{};
    const FVolumetricCloudLayer farThinLayer{
        1448.0f, 1452.25f, 1.0f};
    const render_internal::FVolumetricCloudRayMarchPlanInternal single =
        render_internal::PlanVolumetricCloudRayMarch_Internal(
            FVec3{0.0f, 8.0f, 0.0f}, FVec3{1.0f, 0.0f, 0.0f},
            farThinLayer, disabledUpper, false, range,
            FVec3{}, kVolumetricCloudViewSteps);
    EXPECT_TRUE(single.hit);
    EXPECT_EQ(single.interval_count, 1u);
    EXPECT_TRUE(single.intervals[0].enter > 100000.0f);
    EXPECT_TRUE(single.total_fine_cell_count <=
                kVolumetricCloudViewSteps);
    EXPECT_EQ(single.intervals[0].fine_cell_count,
              single.total_fine_cell_count);

    const f32 singleSpan =
        single.intervals[0].exit - single.intervals[0].enter;
    f32 previousOffset = 0.0f;
    for (u32 cellIndex = 1u;
         cellIndex <= single.intervals[0].fine_cell_count;
         ++cellIndex) {
        const f32 offset = CloudRayCellOffsetForTest(
            singleSpan,
            cellIndex, single.intervals[0].fine_cell_count,
            single.intervals[0].fine_step);
        EXPECT_TRUE(offset > previousOffset);
        previousOffset = offset;
    }
    EXPECT_NEAR(previousOffset, singleSpan, 0.0f);

    // 同じ刻みを遠距離へ逐次加算すると、二進32bitの丸めが毎回同じ側へ寄り、
    // この監査条件では終端へ届かない。整数境界は最後を終端へ直接固定する。
    const f32 repeatedAverageStep =
        (single.intervals[0].exit - single.intervals[0].enter) /
        static_cast<f32>(single.intervals[0].fine_cell_count);
    f32 accumulatedBoundary = single.intervals[0].enter;
    for (u32 cellIndex = 0u;
         cellIndex < single.intervals[0].fine_cell_count;
         ++cellIndex) {
        accumulatedBoundary += repeatedAverageStep;
    }
    EXPECT_TRUE(accumulatedBoundary < single.intervals[0].exit);

    const FVolumetricCloudLayer lowerLayer{
        1658.0f, 1660.5f, 1.0f};
    const FVolumetricCloudUpperLayer upperLayer{
        1710.5f, 1713.0f, 0.55f, 0.30f};
    const render_internal::FVolumetricCloudRayMarchPlanInternal split =
        render_internal::PlanVolumetricCloudRayMarch_Internal(
            FVec3{0.0f, 8.0f, 0.0f}, FVec3{1.0f, 0.0f, 0.0f},
            lowerLayer, upperLayer, true, range,
            FVec3{}, kVolumetricCloudViewSteps);
    EXPECT_TRUE(split.hit);
    EXPECT_EQ(split.interval_count, 2u);
    EXPECT_EQ(split.intervals[0].physical_band_id, 0u);
    EXPECT_EQ(split.intervals[1].physical_band_id, 1u);
    EXPECT_EQ(
        split.intervals[0].fine_cell_count +
            split.intervals[1].fine_cell_count,
        split.total_fine_cell_count);
    EXPECT_TRUE(split.total_fine_cell_count <=
                kVolumetricCloudViewSteps);
    for (u32 intervalIndex = 0u;
         intervalIndex < split.interval_count;
         ++intervalIndex) {
        const render_internal::FVolumetricCloudRayMarchPlanInternal::FInterval&
            interval = split.intervals[intervalIndex];
        const f32 intervalSpan = interval.exit - interval.enter;
        f32 offset = 0.0f;
        for (u32 cellIndex = 1u;
             cellIndex <= interval.fine_cell_count;
             ++cellIndex) {
            const f32 nextOffset = CloudRayCellOffsetForTest(
                intervalSpan,
                cellIndex, interval.fine_cell_count,
                interval.fine_step);
            EXPECT_TRUE(nextOffset > offset);
            offset = nextOffset;
        }
        EXPECT_NEAR(offset, intervalSpan, 0.0f);
    }

    // 250 kmでは1 mm刻みを絶対距離へ足すと複数境界が同じfloatへ丸まる。
    // 相対距離なら各セル幅を保持でき、絶対距離への加算は標本位置で一度だけとなる。
    constexpr f32 kLargeDistance = 250000.0f;
    constexpr f32 kSmallSpan = 0.01f;
    constexpr f32 kSmallStep = 0.001f;
    f32 previousRelative = 0.0f;
    f32 previousAbsolute = kLargeDistance;
    bool absoluteBoundaryCollapsed = false;
    for (u32 cellIndex = 1u; cellIndex <= 10u; ++cellIndex) {
        const f32 relative = CloudRayCellOffsetForTest(
            kSmallSpan, cellIndex, 10u, kSmallStep);
        const f32 absolute = kLargeDistance + relative;
        EXPECT_TRUE(relative > previousRelative);
        if (absolute == previousAbsolute) absoluteBoundaryCollapsed = true;
        previousRelative = relative;
        previousAbsolute = absolute;
    }
    EXPECT_TRUE(absoluteBoundaryCollapsed);
    EXPECT_NEAR(previousRelative, kSmallSpan, 0.0f);
}

ACS_TEST(VolumetricClouds,
         CpuMarchPlanMatchesInteriorDistanceGrowthAndPhysicalBandIdentity) {
    const FVolumetricCloudLayer lowerLayer{
        96.0f, 128.0f, 0.035f};
    const FVolumetricCloudUpperLayer upperLayer{
        196.0f, 228.0f, 0.55f, 0.30f};
    FVolumetricCloudRange range{};
    range.StepGrowth = 0.0f;

    const render_internal::FVolumetricCloudRayMarchPlanInternal inside =
        render_internal::PlanVolumetricCloudRayMarch_Internal(
            FVec3{0.0f, 110.0f, 0.0f}, FVec3{1.0f, 0.0f, 0.0f},
            lowerLayer, upperLayer, false, range,
            FVec3{}, kVolumetricCloudViewSteps);
    EXPECT_TRUE(inside.hit);
    EXPECT_NEAR(inside.maximum_distance,
                kVolumetricCloudInteriorMinDistance, 1.0e-4f);
    EXPECT_NEAR(inside.intervals[0].exit,
                kVolumetricCloudInteriorMinDistance, 1.0e-3f);
    EXPECT_EQ(inside.total_fine_cell_count,
              kVolumetricCloudViewSteps);
    const f32 insideSpan =
        inside.intervals[0].exit - inside.intervals[0].enter;
    EXPECT_TRUE(
        inside.intervals[0].fine_step *
            static_cast<f32>(inside.intervals[0].fine_cell_count) >=
        insideSpan);
    EXPECT_TRUE(
        inside.intervals[0].fine_step *
            static_cast<f32>(inside.intervals[0].fine_cell_count - 1u) <
        insideSpan);

    const FVec3 horizonDirection =
        NormalizeForTest(FVec3{1.0f, 0.02f, 0.0f});
    const render_internal::FVolumetricCloudRayMarchPlanInternal noGrowth =
        render_internal::PlanVolumetricCloudRayMarch_Internal(
            FVec3{0.0f, 8.0f, 0.0f}, horizonDirection,
            lowerLayer, upperLayer, false, range,
            FVec3{}, kVolumetricCloudViewSteps);
    range.StepGrowth = 2.0f;
    const render_internal::FVolumetricCloudRayMarchPlanInternal withGrowth =
        render_internal::PlanVolumetricCloudRayMarch_Internal(
            FVec3{0.0f, 8.0f, 0.0f}, horizonDirection,
            lowerLayer, upperLayer, false, range,
            FVec3{}, kVolumetricCloudViewSteps);
    EXPECT_TRUE(noGrowth.hit);
    EXPECT_TRUE(withGrowth.hit);
    EXPECT_NEAR(noGrowth.intervals[0].enter,
                withGrowth.intervals[0].enter, 0.0f);
    EXPECT_NEAR(noGrowth.intervals[0].exit,
                withGrowth.intervals[0].exit, 0.0f);
    EXPECT_TRUE(withGrowth.requested_fine_step >
                noGrowth.requested_fine_step);
    EXPECT_TRUE(withGrowth.total_fine_cell_count <
                noGrowth.total_fine_cell_count);

    range.StepGrowth = 0.0f;
    const render_internal::FVolumetricCloudRayMarchPlanInternal beforeExit =
        render_internal::PlanVolumetricCloudRayMarch_Internal(
            FVec3{0.0f, 110.0f, 0.0f}, FVec3{0.0f, 1.0f, 0.0f},
            lowerLayer, upperLayer, true, range,
            FVec3{}, kVolumetricCloudViewSteps);
    const render_internal::FVolumetricCloudRayMarchPlanInternal afterExit =
        render_internal::PlanVolumetricCloudRayMarch_Internal(
            FVec3{0.0f, 150.0f, 0.0f}, FVec3{0.0f, 1.0f, 0.0f},
            lowerLayer, upperLayer, true, range,
            FVec3{}, kVolumetricCloudViewSteps);
    EXPECT_TRUE(beforeExit.hit);
    EXPECT_TRUE(afterExit.hit);
    EXPECT_EQ(beforeExit.interval_count, 2u);
    EXPECT_EQ(beforeExit.intervals[1].physical_band_id, 1u);
    EXPECT_EQ(afterExit.interval_count, 1u);
    EXPECT_EQ(afterExit.intervals[0].physical_band_id, 1u);
}

ACS_TEST(VolumetricClouds,
         PhysicalBandCellAllocationIsIndependentOfTraversalOrder) {
    const FVolumetricCloudLayer lowerLayer{
        10.0f, 24.5f, 0.035f};
    const FVolumetricCloudUpperLayer upperLayer{
        30.0f, 47.5f, 0.55f, 0.30f};
    FVolumetricCloudRange range{};
    range.StepGrowth = 0.0f;

    const render_internal::FVolumetricCloudRayMarchPlanInternal upward =
        render_internal::PlanVolumetricCloudRayMarch_Internal(
            FVec3{0.0f, 0.0f, 0.0f}, FVec3{0.0f, 1.0f, 0.0f},
            lowerLayer, upperLayer, true, range,
            FVec3{}, kVolumetricCloudMinViewSteps);
    const render_internal::FVolumetricCloudRayMarchPlanInternal downward =
        render_internal::PlanVolumetricCloudRayMarch_Internal(
            FVec3{0.0f, 60.0f, 0.0f}, FVec3{0.0f, -1.0f, 0.0f},
            lowerLayer, upperLayer, true, range,
            FVec3{}, kVolumetricCloudMinViewSteps);

    EXPECT_TRUE(upward.hit);
    EXPECT_TRUE(downward.hit);
    EXPECT_EQ(upward.interval_count, 2u);
    EXPECT_EQ(downward.interval_count, 2u);
    EXPECT_EQ(upward.total_fine_cell_count, 31u);
    EXPECT_EQ(downward.total_fine_cell_count, 31u);

    // 32標本では各帯へ16ずつ予約する。下層は基準1m刻みで15セル、
    // 上層は予約上限で16セルとなり、視線順が逆転しても同じ物理IDへ残る。
    EXPECT_EQ(upward.intervals[0].physical_band_id, 0u);
    EXPECT_EQ(upward.intervals[0].fine_cell_count, 15u);
    EXPECT_EQ(upward.intervals[1].physical_band_id, 1u);
    EXPECT_EQ(upward.intervals[1].fine_cell_count, 16u);
    EXPECT_EQ(downward.intervals[0].physical_band_id, 1u);
    EXPECT_EQ(downward.intervals[0].fine_cell_count, 16u);
    EXPECT_EQ(downward.intervals[1].physical_band_id, 0u);
    EXPECT_EQ(downward.intervals[1].fine_cell_count, 15u);
    EXPECT_NEAR(
        upward.intervals[0].fine_step,
        downward.intervals[1].fine_step, 1.0e-6f);
    EXPECT_NEAR(
        upward.intervals[1].fine_step,
        downward.intervals[0].fine_step, 1.0e-6f);
    EXPECT_NEAR(upward.intervals[0].fine_step, 1.0f, 1.0e-6f);
    EXPECT_NEAR(
        upward.intervals[1].fine_step, 17.5f / 16.0f, 1.0e-6f);
    EXPECT_NEAR(
        CloudRayCellOffsetForTest(
            17.5f, 1u, 16u, upward.intervals[1].fine_step),
        17.5f / 16.0f, 1.0e-6f);

    u32 lowerBudget = 0u;
    u32 upperBudget = 0u;
    render_internal::ResolveVolumetricCloudPhysicalBandBudgets_Internal(
        lowerLayer, upperLayer, true, 96u,
        lowerBudget, upperBudget);
    EXPECT_EQ(lowerBudget, 47u);
    EXPECT_EQ(upperBudget, 49u);
}

ACS_TEST(VolumetricClouds,
         PhysicalBandAllocationKeepsTheMinimumSearchBudgetForAThinBand) {
    const FVolumetricCloudLayer lowerLayer{
        100.0f, 100.25f, 0.035f};
    const FVolumetricCloudUpperLayer upperLayer{
        100.25f, 250000.0f, 0.55f, 0.30f};
    FVolumetricCloudRange range{};
    range.StepGrowth = 0.0f;

    u32 lowerBudget = 0u;
    u32 upperBudget = 0u;
    render_internal::ResolveVolumetricCloudPhysicalBandBudgets_Internal(
        lowerLayer, upperLayer, true, kVolumetricCloudViewSteps,
        lowerBudget, upperBudget);
    EXPECT_EQ(lowerBudget, kVolumetricCloudMinViewSteps);
    EXPECT_EQ(upperBudget,
              kVolumetricCloudViewSteps -
                  kVolumetricCloudMinViewSteps);

    const render_internal::FVolumetricCloudRayMarchPlanInternal plan =
        render_internal::PlanVolumetricCloudRayMarch_Internal(
            FVec3{0.0f, 100.0f, 0.0f}, FVec3{1.0f, 0.0f, 0.0f},
            lowerLayer, upperLayer, true, range,
            FVec3{}, kVolumetricCloudViewSteps);

    EXPECT_TRUE(plan.hit);
    EXPECT_EQ(plan.interval_count, 2u);
    EXPECT_EQ(plan.intervals[0].physical_band_id, 0u);
    EXPECT_EQ(plan.intervals[0].fine_cell_count,
              kVolumetricCloudMinViewSteps);
    EXPECT_TRUE(plan.intervals[1].fine_cell_count <=
                kVolumetricCloudViewSteps -
                    kVolumetricCloudMinViewSteps);
    EXPECT_TRUE(plan.total_fine_cell_count <=
                kVolumetricCloudViewSteps);
}

ACS_TEST(VolumetricClouds,
         UpperBandLatticeKeepsEarlierCellsWhenLowerBandDisappears) {
    const FVolumetricCloudLayer lowerLayer{
        99.5f, 99.75f, 0.035f};
    const FVolumetricCloudUpperLayer upperLayer{
        100.0f, 132.0f, 0.55f, 0.30f};
    FVolumetricCloudRange range{};
    range.StepGrowth = kVolumetricCloudMaxStepGrowth;

    const FVec3 beforeOrigin{0.0f, 99.49f, 0.0f};
    const FVec3 afterOrigin{0.0f, 99.76f, 0.0f};
    const render_internal::FVolumetricCloudRayMarchPlanInternal before =
        render_internal::PlanVolumetricCloudRayMarch_Internal(
            beforeOrigin, FVec3{0.0f, 1.0f, 0.0f},
            lowerLayer, upperLayer, true, range,
            FVec3{}, kVolumetricCloudMinViewSteps);
    const render_internal::FVolumetricCloudRayMarchPlanInternal after =
        render_internal::PlanVolumetricCloudRayMarch_Internal(
            afterOrigin, FVec3{0.0f, 1.0f, 0.0f},
            lowerLayer, upperLayer, true, range,
            FVec3{}, kVolumetricCloudMinViewSteps);

    EXPECT_TRUE(before.hit);
    EXPECT_TRUE(after.hit);
    EXPECT_EQ(before.interval_count, 2u);
    EXPECT_EQ(after.interval_count, 1u);
    const auto& beforeUpper = before.intervals[1];
    const auto& afterUpper = after.intervals[0];
    EXPECT_EQ(beforeUpper.physical_band_id, 1u);
    EXPECT_EQ(afterUpper.physical_band_id, 1u);
    EXPECT_EQ(beforeUpper.fine_cell_count, 16u);
    EXPECT_EQ(afterUpper.fine_cell_count, 16u);
    EXPECT_TRUE(Abs(beforeUpper.fine_step - afterUpper.fine_step) < 0.001f);
    const auto beforeQuadrature =
        render_internal::ResolveVolumetricCloudDensityQuadrature_Internal(
            beforeUpper.fine_step);
    const auto afterQuadrature =
        render_internal::ResolveVolumetricCloudDensityQuadrature_Internal(
            afterUpper.fine_step);
    EXPECT_EQ(beforeQuadrature.sample_count, afterQuadrature.sample_count);
    for (u32 sampleIndex = 0u;
         sampleIndex < beforeQuadrature.sample_count; ++sampleIndex) {
        EXPECT_NEAR(
            beforeQuadrature.fractions[sampleIndex],
            afterQuadrature.fractions[sampleIndex], 0.0f);
    }

    // 下層が消えても上層へ予約した16セルを再分配しない。上層自身の入口が
    // 0.27m近づく距離LODだけが連続して働き、全域を数m動かす旧方式を防ぐ。
    for (u32 cellIndex = 0u;
         cellIndex <= beforeUpper.fine_cell_count; ++cellIndex) {
        const f32 beforeWorldHeight = beforeOrigin.y +
            beforeUpper.enter + CloudRayCellOffsetForTest(
                beforeUpper.exit - beforeUpper.enter, cellIndex,
                beforeUpper.fine_cell_count, beforeUpper.fine_step);
        const f32 afterWorldHeight = afterOrigin.y +
            afterUpper.enter + CloudRayCellOffsetForTest(
                afterUpper.exit - afterUpper.enter, cellIndex,
                afterUpper.fine_cell_count, afterUpper.fine_step);
        const f32 expectedDistanceLodShift =
            cellIndex < beforeUpper.fine_cell_count
                ? static_cast<f32>(cellIndex) *
                    Abs(beforeUpper.fine_step - afterUpper.fine_step)
                : 0.0f;
        EXPECT_TRUE(
            Abs(beforeWorldHeight - afterWorldHeight) <=
            expectedDistanceLodShift + 1.0e-4f);
    }
}

ACS_TEST(VolumetricClouds,
         CpuGpuMirrorUsesSinglePrecisionTangentDecision) {
    const FVolumetricCloudLayer layer{
        3000.0f, 4000.0f, 0.035f};
    const FVolumetricCloudUpperLayer disabledUpper{};
    FVolumetricCloudRange range{};
    range.StepGrowth = 0.0f;
    const FVec3 origin{0.0f, 5000.0f, 0.0f};
    const f32 directionY = -0.017725509f;
    const FVec3 direction{
        Sqrt(1.0f - directionY * directionY), directionY, 0.0f};

    const render_internal::FVolumetricCloudRayMarchPlanInternal gpuMirror =
        render_internal::PlanVolumetricCloudRayMarch_Internal(
            origin, direction, layer, disabledUpper, false, range,
            FVec3{}, kVolumetricCloudViewSteps);
    const FVec3 onceNormalizedDirection = NormalizeForTest(direction);
    const FVolumetricCloudRayInterval highPrecisionReference =
        IntersectVolumetricCloudShell(
            origin, onceNormalizedDirection, layer,
            kVolumetricCloudPlanetRadius, FVec3{});

    // 地平線接線付近では単精度GPUと倍精度参照のhit判定が異なり得る。
    // GPUミラーは係数演算の共通許容差で微小な負値を接線へ丸める。
    EXPECT_TRUE(gpuMirror.hit);
    EXPECT_EQ(gpuMirror.interval_count, 1u);
    EXPECT_FALSE(highPrecisionReference.hit);
}

ACS_TEST(VolumetricClouds,
         SphereRootToleranceIsReservedForTheOuterShell) {
    constexpr f32 kCenterDot = -1000.0f;
    constexpr f32 kCenterDotSquared =
        kCenterDot * kCenterDot;
    constexpr f32 kRelativeTolerance = 9.5367431640625e-7f;
    constexpr f32 kDiscriminantTolerance =
        (kCenterDotSquared + kCenterDotSquared) * kRelativeTolerance;
    f32 nearDistance = 0.0f;
    f32 farDistance = 0.0f;

    EXPECT_TRUE(render_internal::ResolveVolumetricCloudSphereRoots_Internal(
        kCenterDot,
        kCenterDotSquared + 0.5f * kDiscriminantTolerance,
        true,
        nearDistance, farDistance));
    EXPECT_TRUE(nearDistance > 0.0f);
    EXPECT_TRUE(farDistance >= nearDistance);
    EXPECT_FALSE(render_internal::ResolveVolumetricCloudSphereRoots_Internal(
        kCenterDot,
        kCenterDotSquared + 2.0f * kDiscriminantTolerance,
        true,
        nearDistance, farDistance));
    EXPECT_FALSE(render_internal::ResolveVolumetricCloudSphereRoots_Internal(
        kCenterDot,
        kCenterDotSquared + 0.5f * kDiscriminantTolerance,
        false,
        nearDistance, farDistance));

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader,
        "staticconstfloatCLOUD_SHELL_DISCRIMINANT_RELATIVE_TOLERANCE="
        "9.5367431640625e-7;"));
    EXPECT_TRUE(Contains(
        shader,
        "boolhit=disc>=0.0||("
        "acceptRoundedOuterTangent&&disc>=-discriminantTolerance);"));
}

ACS_TEST(VolumetricClouds,
         InnerShellRoundingMissDoesNotTruncateAnInteriorCloudRay) {
    const FVolumetricCloudLayer layer{
        3000.0f, 4000.0f, 0.035f};
    const FVec3 origin{0.0f, 3500.0f, 0.0f};
    const FVec3 direction{
        0.999921441f, -0.0125355404f, 0.0f};
    const f32 centerDot =
        (kVolumetricCloudPlanetRadius + origin.y) * direction.y;
    const f32 innerC =
        (origin.y - layer.base_height) *
        (2.0f * kVolumetricCloudPlanetRadius +
         origin.y + layer.base_height);
    const f32 discriminant = centerDot * centerDot - innerC;
    EXPECT_TRUE(discriminant < 0.0f);

    f32 nearDistance = 0.0f;
    f32 farDistance = 0.0f;
    EXPECT_FALSE(render_internal::ResolveVolumetricCloudSphereRoots_Internal(
        centerDot, innerC, false, nearDistance, farDistance));
    EXPECT_TRUE(render_internal::ResolveVolumetricCloudSphereRoots_Internal(
        centerDot, innerC, true, nearDistance, farDistance));

    const FVolumetricCloudRayInterval gpuMirror =
        render_internal::ResolveVolumetricCloudShellInterval_Internal(
            origin, direction, layer);
    const FVolumetricCloudRayInterval highPrecisionReference =
        IntersectVolumetricCloudShell(
            origin, direction, layer,
            kVolumetricCloudPlanetRadius, FVec3{});
    EXPECT_TRUE(gpuMirror.hit);
    EXPECT_TRUE(highPrecisionReference.hit);
    EXPECT_NEAR(gpuMirror.enter, 0.0f, 0.0f);
    EXPECT_TRUE(gpuMirror.exit > 190000.0f);
    EXPECT_NEAR(gpuMirror.exit, highPrecisionReference.exit, 64.0f);
}

ACS_TEST(VolumetricClouds,
         PhysicalGroundHorizonKeepsVisibleDownwardCloudRay) {
    const FVolumetricCloudLayer layer{
        3000.0f, 4000.0f, 0.035f};
    const FVec3 origin{0.0f, 1000.0f, 0.0f};
    const FVec3 direction = NormalizeForTest(
        FVec3{1.0f, -0.01f, 0.0f});
    const FVolumetricCloudGroundHorizon groundHorizon =
        ResolveVolumetricCloudGroundHorizon(
            origin, layer, FVec3{});
    EXPECT_TRUE(groundHorizon.ground_cutoff < -0.017f);
    EXPECT_TRUE(direction.y > groundHorizon.ground_cutoff);

    const FVolumetricCloudMarchPlan publicPlan =
        PlanVolumetricCloudRayMarch(
            origin, direction, layer);
    const FVolumetricCloudUpperLayer disabledUpper{};
    const FVolumetricCloudRange range{};
    const render_internal::FVolumetricCloudRayMarchPlanInternal gpuPlan =
        render_internal::PlanVolumetricCloudRayMarch_Internal(
            origin, direction, layer, disabledUpper, false,
            range, FVec3{}, kVolumetricCloudViewSteps);
    EXPECT_TRUE(publicPlan.hit);
    EXPECT_TRUE(gpuPlan.hit);
    EXPECT_TRUE(publicPlan.enter > 200000.0f);
    EXPECT_TRUE(gpuPlan.intervals[0].enter > 200000.0f);
}

ACS_TEST(VolumetricClouds, CoarseOccupancyRefinementKeepsContinuousCellBoundaries) {
    constexpr f32 kIntervalStart = 100.0f;
    constexpr f32 kFineStep = 6.0f;
    constexpr f32 kCoarseStep = 12.0f;

    // 粗いセル[142,154]で支持域を検出したら、同じ始点から細密セル二つで覆う。
    constexpr f32 kCoarseCellStart = 142.0f;
    const FCloudFineSampleForTest first = ResolveCloudFineSampleForTest(
        kCoarseCellStart, kCoarseCellStart + kCoarseStep,
        kFineStep);
    const FCloudFineSampleForTest second = ResolveCloudFineSampleForTest(
        first.cell_end, kCoarseCellStart + kCoarseStep,
        kFineStep);
    EXPECT_NEAR(first.cell_start, 142.0f, 1e-6f);
    EXPECT_NEAR(first.cell_end, 148.0f, 1e-6f);
    EXPECT_NEAR(second.cell_start, first.cell_end, 0.0f);
    EXPECT_NEAR(second.cell_end, 154.0f, 1e-6f);
    const FCloudFineSampleForTest refined = ResolveCloudFineSampleForTest(
        kCoarseCellStart, kCoarseCellStart + kCoarseStep, kFineStep);
    EXPECT_NEAR(refined.occupancy_t, 145.0f, 0.0f);
    EXPECT_NEAR(refined.sample_t, 145.0f, 0.0f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    const std::size_t coarseHit = shader.find(
        "if(!nearDensity&&traversalCellCount>1){");
    const std::size_t refineFromSameStart = shader.find(
        "refineUntilCell=nextFineCellIndex;", coarseHit);
    const std::size_t enterFineMode = shader.find(
        "nearDensity=true;", refineFromSameStart);
    const std::size_t firstFineChild = shader.find(
        "nextFineCellIndex=min("
        "fineCellIndex+1,currentFineCellCount);", enterFineMode);
    const std::size_t normalAdvance = shader.find(
        "nearDensity=true;refineUntilCell=max(", firstFineChild);
    EXPECT_TRUE(coarseHit != std::string::npos);
    EXPECT_TRUE(coarseHit < refineFromSameStart);
    EXPECT_TRUE(refineFromSameStart < enterFineMode);
    EXPECT_TRUE(enterFineMode < firstFineChild);
    EXPECT_TRUE(firstFineChild < normalAdvance);
    EXPECT_TRUE(Contains(
        shader,
        "if(stepLength<=0.0){"
        "fineCellIndex=nextFineCellIndex;continue;}"));

    // 粗い親セルだけが占有、細密子セルは空という最悪分岐でも、各反復が
    // 整数セル一つ以上を進み、最小32回の上限内で全28セルへ到達する。
    constexpr u32 kMaximumSteps = 32u;
    constexpr u32 kWorstCaseCellCount = 28u;
    u32 worstCaseCellIndex = 0u;
    bool worstCaseNearDensity = false;
    u32 worstCaseRefineUntilCell = 0u;
    u32 executedSteps = 0u;
    for (; executedSteps < kMaximumSteps &&
           worstCaseCellIndex < kWorstCaseCellCount;
         ++executedSteps) {
        const u32 traversalCellCount = worstCaseNearDensity ? 1u : 2u;
        u32 nextCellIndex = worstCaseCellIndex + traversalCellCount;
        if (nextCellIndex > kWorstCaseCellCount) {
            nextCellIndex = kWorstCaseCellCount;
        }
        const bool occupied = !worstCaseNearDensity;
        if (!occupied) {
            worstCaseCellIndex = nextCellIndex;
            if (worstCaseCellIndex >= worstCaseRefineUntilCell) {
                worstCaseNearDensity = false;
            }
            continue;
        }
        worstCaseRefineUntilCell = nextCellIndex;
        worstCaseNearDensity = true;
        nextCellIndex = worstCaseCellIndex + 1u;
        if (nextCellIndex > kWorstCaseCellCount) {
            nextCellIndex = kWorstCaseCellCount;
        }
        const u32 lookAhead = nextCellIndex + 2u;
        if (lookAhead > worstCaseRefineUntilCell) {
            worstCaseRefineUntilCell = lookAhead < kWorstCaseCellCount
                ? lookAhead : kWorstCaseCellCount;
        }
        worstCaseCellIndex = nextCellIndex;
    }
    EXPECT_EQ(worstCaseCellIndex, kWorstCaseCellCount);
    EXPECT_TRUE(executedSteps <= kMaximumSteps);
    EXPECT_FALSE(Contains(shader, "cloudRefinedSampleT("));
    EXPECT_FALSE(Contains(shader, "coarseProbeT"));
}

ACS_TEST(VolumetricClouds, InteriorMarchChecksTheCameraAdjacentFineInterval) {
    constexpr f32 kInsideEnter = 0.0f;
    constexpr f32 kInsideExit = 100.0f;
    constexpr f32 kOutsideEnter = 100.0f;
    constexpr f32 kFineStep = 6.0f;
    constexpr f32 kCoarseStep = 12.0f;

    const bool startsInside = kInsideEnter <= 1e-4f;
    const FCloudFineSampleForTest insideInterval = ResolveCloudFineSampleForTest(
        kInsideEnter, kInsideExit,
        startsInside ? kFineStep : kCoarseStep);
    const bool startsOutside = kOutsideEnter <= 1e-4f;
    const FCloudFineSampleForTest outsideInterval = ResolveCloudFineSampleForTest(
        kOutsideEnter, kOutsideEnter + kCoarseStep,
        startsOutside ? kFineStep : kCoarseStep);
    EXPECT_TRUE(insideInterval.valid);
    EXPECT_NEAR(insideInterval.cell_start, kInsideEnter, 1e-6f);
    EXPECT_NEAR(insideInterval.step_length, kFineStep, 1e-6f);
    EXPECT_NEAR(insideInterval.occupancy_t, kInsideEnter + 0.5f * kFineStep, 1e-6f);
    EXPECT_NEAR(insideInterval.sample_t, insideInterval.occupancy_t, 0.0f);
    EXPECT_NEAR(outsideInterval.occupancy_t, kOutsideEnter + 0.5f * kCoarseStep, 1e-6f);
    EXPECT_NEAR(outsideInterval.sample_t, outsideInterval.occupancy_t, 0.0f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(shader, "boolstartsInsideShell=intervalStart<=1e-4;"));
    EXPECT_TRUE(Contains(shader, "intfineCellIndex=0;"));
    EXPECT_TRUE(Contains(shader, "boolnearDensity=startsInsideShell;"));
    EXPECT_TRUE(Contains(
        shader,
        "intrefineUntilCell=startsInsideShell"
        "?min(2,currentFineCellCount):0;"));
    EXPECT_FALSE(Contains(shader, "initialProbeStep"));
}

ACS_TEST(VolumetricClouds, FineCellsOwnTheCompleteIntegrationInterval) {
    constexpr f32 kIntervalStart = 100.0f;
    constexpr f32 kIntervalEnd = 110.0f;
    constexpr f32 kFineStep = 6.0f;
    const FCloudFineSampleForTest first = ResolveCloudFineSampleForTest(
        kIntervalStart, kIntervalEnd, kFineStep);
    const FCloudFineSampleForTest last = ResolveCloudFineSampleForTest(
        first.cell_end, kIntervalEnd, kFineStep);
    const FCloudFineSampleForTest finished = ResolveCloudFineSampleForTest(
        last.cell_end, kIntervalEnd, kFineStep);
    EXPECT_TRUE(first.valid);
    EXPECT_NEAR(first.cell_start, 100.0f, 1e-6f);
    EXPECT_NEAR(first.cell_end, 106.0f, 1e-6f);
    EXPECT_NEAR(first.occupancy_t, 103.0f, 1e-6f);
    EXPECT_NEAR(first.sample_t, 103.0f, 1e-6f);
    EXPECT_NEAR(first.step_length, 6.0f, 1e-6f);
    EXPECT_TRUE(last.valid);
    EXPECT_NEAR(last.cell_start, 106.0f, 1e-6f);
    EXPECT_NEAR(last.cell_end, 110.0f, 1e-6f);
    EXPECT_NEAR(last.occupancy_t, 108.0f, 1e-6f);
    EXPECT_NEAR(last.sample_t, 108.0f, 1e-6f);
    EXPECT_NEAR(last.step_length, 4.0f, 1e-6f);
    EXPECT_FALSE(finished.valid);

    // 各セルの担当区間を合計すると、位相ずらしなしで積分区間の全長になる。
    f32 cellStart = kIntervalStart;
    f32 completeLength = 0.0f;
    for (u32 sampleIndex = 0u; sampleIndex < 4u; ++sampleIndex) {
        const FCloudFineSampleForTest sample = ResolveCloudFineSampleForTest(
            cellStart, kIntervalEnd, kFineStep);
        if (!sample.valid) break;
        EXPECT_NEAR(sample.sample_t, sample.occupancy_t, 0.0f);
        EXPECT_NEAR(
            sample.occupancy_t,
            0.5f * (sample.cell_start + sample.cell_end), 0.0f);
        completeLength += sample.step_length;
        cellStart = sample.cell_end;
    }
    EXPECT_NEAR(completeLength, kIntervalEnd - kIntervalStart, 2e-5f);

    // 隣接セルの担当区間は同じ境界で接続し、探索支持域に空白を作らない。
    const FCloudFineSampleForTest adjacentFirst =
        ResolveCloudFineSampleForTest(
            kIntervalStart, kIntervalEnd, kFineStep);
    const FCloudFineSampleForTest adjacentLast =
        ResolveCloudFineSampleForTest(
            adjacentFirst.cell_end, kIntervalEnd, kFineStep);
    EXPECT_NEAR(
        adjacentFirst.cell_end,
        adjacentLast.cell_start, 0.0f);
    EXPECT_NEAR(
        adjacentFirst.step_length + adjacentLast.step_length,
        kIntervalEnd - kIntervalStart, 1.0e-6f);
    EXPECT_NEAR(
        adjacentFirst.occupancy_t,
        0.5f * (adjacentFirst.cell_start + adjacentFirst.cell_end),
        0.0f);
    EXPECT_NEAR(
        adjacentLast.occupancy_t,
        0.5f * (adjacentLast.cell_start + adjacentLast.cell_end),
        0.0f);

    // 一様密度なら区間中央採取の重み付き代表深度は、積分区間全体の中央と一致する。
    const f32 integratedLength = first.step_length + last.step_length;
    const f32 meanDepth = (first.sample_t * first.step_length + last.sample_t * last.step_length) / integratedLength;
    EXPECT_NEAR(meanDepth, 105.0f, 1e-6f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(shader, "[loop]for(inti=0;i<MAX_STEPS;i++){"));
    EXPECT_TRUE(Contains(shader, "boolintervalFinished=fineCellIndex>=currentFineCellCount;"));
    EXPECT_TRUE(Contains(shader,
        "if(intervalFinished){"
        "packedIntervalIndex++;"
        "if(packedIntervalIndex>=packedBandIntervals.count)break;"
        "intervalStart=packedBandIntervals.starts[packedIntervalIndex];"
        "intervalEnd=packedBandIntervals.ends[packedIntervalIndex];"));
    EXPECT_TRUE(Contains(shader,
        "floatcellStartOffset=cloudRayCellOffset("
        "currentIntervalSpan,fineCellIndex,currentFineCellCount,"
        "safeCurrentRequestedFineStep);"
        "floatcellEndOffset=cloudRayCellOffset("
        "currentIntervalSpan,nextFineCellIndex,currentFineCellCount,"
        "safeCurrentRequestedFineStep);"
        "floatstepLength=max(cellEndOffset-cellStartOffset,0.0);"));
    EXPECT_TRUE(Contains(shader,
        "floatoccupancySampleT=intervalStart+cellStartOffset+0.5*stepLength;"
        "float3occupancyP=camPos.xyz+dir*occupancySampleT;"));
    EXPECT_TRUE(Contains(shader,
        "floatphysicalDensityResolutionSpacing=cellLaneLength*"
        "CLOUD_DENSITY_GAUSS_MAXIMUM_GAP_FRACTION;"));
    EXPECT_TRUE(Contains(
        shader,
        "[loop]for(intdensitySampleIndex=0;"
        "densitySampleIndex<CLOUD_DENSITY_GAUSS_SAMPLE_COUNT;"));
    EXPECT_TRUE(Contains(
        shader,
        "sampleCloudPhysicalLaneDensityLightingAtFraction("
        "densitySampleFraction,laneComponentStartT,laneComponentEndT,"));
    EXPECT_TRUE(Contains(
        shader,
        "physicalDensityLongitudinalFootprint,"
        "pixelDirectionSpan,angularPixelFootprint,physicalBandId,"));
    EXPECT_TRUE(Contains(shader,
        "sample.physicalFraction=sampleFraction;"
        "floatcurrentSampleT=cloudIntervalDistanceAtFraction("
        "componentStartT,componentEndT,sampleFraction);"));
    EXPECT_FALSE(Contains(shader, "sampleSupportStartFraction"));
    EXPECT_FALSE(Contains(shader, "physicalSupport"));
    EXPECT_FALSE(Contains(shader, "requestedSampleT"));
    EXPECT_FALSE(Contains(shader, "cloudRayIntervalPhase("));
    EXPECT_TRUE(Contains(shader, "float3occupancyP=camPos.xyz+dir*occupancySampleT;"));
    EXPECT_TRUE(Contains(
        shader,
        "float3currentP=camPos.xyz+rayDirection*currentSampleT;"));
    EXPECT_TRUE(Contains(
        shader,
        "depthMoment+=firstSampleWeight*firstSampleT;"));
    EXPECT_TRUE(Contains(shader, "fineCellIndex=nextFineCellIndex;"));
    EXPECT_FALSE(Contains(shader, "cellStart=cellEnd;"));
    EXPECT_FALSE(Contains(shader, "finePhaseOffset"));
    EXPECT_FALSE(Contains(shader, "MAX_STEPS&&t<t1"));
}

ACS_TEST(VolumetricClouds, DetailBandsFollowRaySampleSpacing) {
    const FVolumetricCloudLayer layer{1500.0f, 4000.0f, 0.035f};
    const FVec3 camera{0.0f, 18.0f, 0.0f};
    const FVolumetricCloudMarchPlan vertical = PlanVolumetricCloudRayMarch(camera, FVec3{0.0f, 1.0f, 0.0f}, layer, kVolumetricCloudMaxDistance, FVec3{}, kVolumetricCloudReferenceViewSteps);
    const FVolumetricCloudMarchPlan horizon = PlanVolumetricCloudRayMarch(camera, NormalizeForTest(FVec3{1.0f, 0.02f, 0.0f}), layer, kVolumetricCloudMaxDistance, FVec3{}, kVolumetricCloudReferenceViewSteps);
    const FVolumetricCloudMarchPlan horizonNormal = PlanVolumetricCloudRayMarch(camera, NormalizeForTest(FVec3{1.0f, 0.02f, 0.0f}), layer, kVolumetricCloudMaxDistance, FVec3{}, kVolumetricCloudViewSteps);
    EXPECT_TRUE(vertical.hit);
    EXPECT_TRUE(horizon.hit);
    EXPECT_TRUE(horizonNormal.hit);
    EXPECT_TRUE(vertical.fine_step < 10.0f);
    EXPECT_TRUE(horizon.fine_step > 120.0f);
    EXPECT_TRUE(horizon.fine_step < 420.0f);
    EXPECT_TRUE(horizonNormal.fine_step > horizon.fine_step);
    // 同一区間で通常336個、参照448個の細密区間を使うため、
    // 通常描画の間隔は参照描画の4/3となる。
    EXPECT_NEAR(
        horizonNormal.fine_step * 3.0f,
        horizon.fine_step * 4.0f, 1e-3f);
    EXPECT_NEAR(CloudBillowVisibilityFromSampleSpacingForTest(vertical.fine_step), 1.0f, 0.0f);
    EXPECT_NEAR(CloudMiddleBillowVisibilityFromSampleSpacingForTest(vertical.fine_step), 1.0f, 0.0f);
    EXPECT_NEAR(CloudErosionVisibilityFromSampleSpacingForTest(vertical.fine_step), 1.0f, 0.0f);
    EXPECT_TRUE(CloudBillowVisibilityFromSampleSpacingForTest(horizon.fine_step) > 0.90f);
    EXPECT_NEAR(CloudMiddleBillowVisibilityFromSampleSpacingForTest(horizon.fine_step), 0.0f, 0.0f);
    EXPECT_NEAR(CloudErosionVisibilityFromSampleSpacingForTest(horizon.fine_step), 0.0f, 0.0f);
    EXPECT_TRUE(CloudBillowVisibilityFromSampleSpacingForTest(horizonNormal.fine_step) > 0.70f);
    EXPECT_TRUE(CloudBillowVisibilityFromSampleSpacingForTest(horizonNormal.fine_step) < CloudBillowVisibilityFromSampleSpacingForTest(horizon.fine_step));
    EXPECT_NEAR(CloudMiddleBillowVisibilityFromSampleSpacingForTest(horizonNormal.fine_step), 0.0f, 0.0f);
    EXPECT_NEAR(CloudErosionVisibilityFromSampleSpacingForTest(horizonNormal.fine_step), 0.0f, 0.0f);
    EXPECT_NEAR(CloudBillowVisibilityFromSampleSpacingForTest(270.1613f), 0.5f, 1e-5f);
    EXPECT_NEAR(CloudMiddleBillowVisibilityFromSampleSpacingForTest(50.4032258f), 0.5f, 1e-5f);
    EXPECT_NEAR(CloudErosionVisibilityFromSampleSpacingForTest(29.23387f), 0.5f, 1e-5f);
    constexpr f32 kMiddleBillowEndSpacing = 0.20f / (0.00031f * 8.0f);
    EXPECT_NEAR(CloudMiddleBillowVisibilityFromSampleSpacingForTest(kMiddleBillowEndSpacing), 0.0f, 1e-6f);
    EXPECT_TRUE(kMiddleBillowEndSpacing * 0.00031f * 16.0f < 0.401f);

    EXPECT_NEAR(CloudProjectedPixelWidthForTest(0.0f, 0.001f), 0.0f, 0.0f);
    EXPECT_NEAR(CloudProjectedPixelWidthForTest(8000.0f, 0.001f), 8.0f, 1e-6f);
    EXPECT_NEAR(CloudProjectedPixelWidthForTest(30000.0f, 0.001f), 30.0f, 1e-5f);
    EXPECT_NEAR(CloudProjectedPixelWidthForTest(60000.0f, 0.001f), 60.0f, 1e-5f);
    EXPECT_NEAR(CloudProjectedPixelWidthForTest(-2.0f, -3.0f), 0.0f, 0.0f);
    // 詳細帯域は横幅と積分幅の広い方を使う。探索側は別途ray区間全長も含める。
    constexpr f32 projectedPixelWidth = 30.0f;
    EXPECT_NEAR(CloudDetailSampleSpacingForTest(12.0f, projectedPixelWidth), 30.0f, 0.0f);
    EXPECT_NEAR(CloudDetailSampleSpacingForTest(48.0f, projectedPixelWidth), 48.0f, 0.0f);
    EXPECT_NEAR(projectedPixelWidth, CloudProjectedPixelWidthForTest(30000.0f, 0.001f), 1e-5f);
    // 同じ1000 m区間でもGauss点ごとの投影幅は異なる。区間重心の500 mを
    // 全点へ使うと、近側を約7.2倍ぼかして遠側の詳細を残す反例になる。
    constexpr f32 kFootprintIntervalLength = 1000.0f;
    constexpr f32 kFootprintAngularWidth = 0.001f;
    constexpr f32 kNearGaussFraction = 0.0694318442f;
    constexpr f32 kFarGaussFraction = 1.0f - kNearGaussFraction;
    constexpr f32 kNearGaussDistance =
        kNearGaussFraction * kFootprintIntervalLength;
    constexpr f32 kFarGaussDistance =
        kFarGaussFraction * kFootprintIntervalLength;
    const f32 nearGaussPixelWidth = CloudProjectedPixelWidthForTest(
        kNearGaussDistance, kFootprintAngularWidth);
    const f32 farGaussPixelWidth = CloudProjectedPixelWidthForTest(
        kFarGaussDistance, kFootprintAngularWidth);
    const f32 centroidPixelWidth = CloudProjectedPixelWidthForTest(
        0.5f * kFootprintIntervalLength, kFootprintAngularWidth);
    EXPECT_TRUE(nearGaussPixelWidth < centroidPixelWidth);
    EXPECT_TRUE(centroidPixelWidth < farGaussPixelWidth);
    EXPECT_NEAR(
        centroidPixelWidth / nearGaussPixelWidth,
        0.5f / kNearGaussFraction, 1.0e-5f);
    // 隣り合う粗い点標本が共に空でも、その中間の支持域を両側の掃引区間が覆う。
    constexpr f32 coarseStep = 100.0f;
    constexpr f32 supportPosition = 100.0f;
    EXPECT_TRUE(std::fabs(supportPosition - 50.0f) <= coarseStep * 0.5f);
    EXPECT_TRUE(std::fabs(supportPosition - 150.0f) <= coarseStep * 0.5f);

    f32 previousBillowVisibility = 1.0f;
    f32 previousMiddleBillowVisibility = 1.0f;
    f32 previousErosionVisibility = 1.0f;
    for (u32 spacingStep = 0u; spacingStep <= 700u; spacingStep += 5u) {
        const f32 billowVisibility = CloudBillowVisibilityFromSampleSpacingForTest(static_cast<f32>(spacingStep));
        const f32 middleBillowVisibility = CloudMiddleBillowVisibilityFromSampleSpacingForTest(static_cast<f32>(spacingStep));
        const f32 erosionVisibility = CloudErosionVisibilityFromSampleSpacingForTest(static_cast<f32>(spacingStep));
        EXPECT_TRUE(billowVisibility <= previousBillowVisibility + 1e-6f);
        EXPECT_TRUE(middleBillowVisibility <= previousMiddleBillowVisibility + 1e-6f);
        EXPECT_TRUE(erosionVisibility <= previousErosionVisibility + 1e-6f);
        EXPECT_TRUE(billowVisibility >= 0.0f && billowVisibility <= 1.0f);
        EXPECT_TRUE(middleBillowVisibility >= 0.0f && middleBillowVisibility <= 1.0f);
        EXPECT_TRUE(erosionVisibility >= 0.0f && erosionVisibility <= 1.0f);
        EXPECT_TRUE(billowVisibility + 1e-6f >= middleBillowVisibility);
        EXPECT_TRUE(middleBillowVisibility + 1e-6f >= erosionVisibility);
        previousBillowVisibility = billowVisibility;
        previousMiddleBillowVisibility = middleBillowVisibility;
        previousErosionVisibility = erosionVisibility;
    }

    // 詳細帯域の切り替えは同じ横断面密度を連続補間し、面積割合には読み替えない。
    render_internal::FVolumetricCloudShapeFrequencyBlendInternal
        completeBlend{};
    completeBlend.coarse = 0.0f;
    completeBlend.middle = 0.0f;
    completeBlend.complete = 1.0f;
    constexpr f32 kBoundaryPotential = -0.02f;
    constexpr f32 kResolvedBillowPotential = 0.10f;
    constexpr f32 kQuarterVisibility = 0.25f;
    const auto densityLanes =
        render_internal::
            ResolveVolumetricCloudDetailFilteredBandDensityLanes_Internal(
                kBoundaryPotential, kBoundaryPotential,
                kBoundaryPotential, completeBlend, 0.0f,
                kResolvedBillowPotential, kResolvedBillowPotential,
                kQuarterVisibility, 0.0f, 0.0f, 0.0f);
    const f32 densityFilteredAfterCompletion =
        render_internal::ResolveVolumetricCloudDensityLaneAverage_Internal(
            densityLanes);
    const f32 densityAfterPotentialAttenuation =
        render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
            kBoundaryPotential +
            kResolvedBillowPotential * kQuarterVisibility);
    EXPECT_NEAR(densityFilteredAfterCompletion, 0.02f, 1.0e-6f);
    EXPECT_TRUE(
        densityFilteredAfterCompletion >
        densityAfterPotentialAttenuation * 20.0f);
    for (u32 lane = 0u; lane < 4u; ++lane)
        EXPECT_NEAR(densityLanes.densities[lane], 0.02f, 1.0e-6f);
    constexpr f32 kOpticalScale = 10.0f;
    render_internal::FVolumetricCloudLaneOpticalDepthInternal opticalDepth{};
    render_internal::AccumulateVolumetricCloudLaneOpticalDepth_Internal(
        opticalDepth, densityLanes, kOpticalScale);
    const f32 laneTransmittance = render_internal::
        ResolveVolumetricCloudLaneTransmittance_Internal(opticalDepth);
    const f32 meanDensityTransmittance =
        std::exp(-densityFilteredAfterCompletion * kOpticalScale);
    EXPECT_NEAR(
        laneTransmittance, meanDensityTransmittance, 1.0e-6f);

    const f32 quietNaN = std::numeric_limits<f32>::quiet_NaN();
    const auto invalidCommon = render_internal::
        ResolveVolumetricCloudBandDensityLanes_Internal(
            0.20f, 0.20f, 0.20f, completeBlend, quietNaN, 0.0f);
    EXPECT_NEAR(
        render_internal::ResolveVolumetricCloudDensityLaneAverage_Internal(
            invalidCommon),
        0.0f, 0.0f);
    const auto invalidBillow = render_internal::
        ResolveVolumetricCloudDetailFilteredBandDensityLanes_Internal(
            0.20f, 0.20f, 0.20f, completeBlend, 0.0f,
            quietNaN, quietNaN, 1.0f, 1.0f, 0.0f, 0.0f);
    EXPECT_NEAR(
        render_internal::ResolveVolumetricCloudDensityLaneAverage_Internal(
            invalidBillow),
        0.0f, 0.0f);
    auto negativeBlend = completeBlend;
    negativeBlend.complete = -1.0f;
    const auto negativeWeight = render_internal::
        ResolveVolumetricCloudBandDensityLanes_Internal(
            0.20f, 0.20f, 0.20f, negativeBlend, 0.0f, 0.0f);
    EXPECT_NEAR(
        render_internal::ResolveVolumetricCloudDensityLaneAverage_Internal(
            negativeWeight),
        0.0f, 0.0f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudDetailFrequencyVisibility("
        "floatsampleSpacing,floatfrequency,floatfadeBegin,floatfadeEnd){"
        "floatfootprint=max(sampleSpacing,0.0)*0.00031*frequency;"
        "return1.0-smoothstep(fadeBegin,fadeEnd,footprint);}"));
    EXPECT_TRUE(Contains(
        shader,
        "returncloudDetailFrequencyVisibility("
        "sampleSpacing,4.0,0.15,0.52);"));
    EXPECT_TRUE(Contains(
        shader,
        "returncloudDetailFrequencyVisibility("
        "sampleSpacing,8.0,0.05,0.20);"));
    EXPECT_TRUE(Contains(
        shader,
        "returncloudDetailFrequencyVisibility("
        "sampleSpacing,16.0,0.05,0.24);"));
    EXPECT_TRUE(Contains(shader, "detailDomainA=rotatedPosition*0.00018;"));
    EXPECT_TRUE(Contains(shader, "detailDomainB=rotatedPosition*0.00031;"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudProjectedPixelWidth("
        "floatsampleDistance,floatangularPixelFootprint){"
        "returnmax(sampleDistance,0.0)*max(angularPixelFootprint,0.0);}"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudDetailSampleSpacing("
        "floatintegrationSpacing,floatprojectedPixelWidth){"
        "returnmax(max(integrationSpacing,0.0),max(projectedPixelWidth,0.0));}"));
    EXPECT_TRUE(Contains(
        shader,
        "floatphysicalProjectedPixelWidth=cloudProjectedPixelWidth("
        "currentSampleT,angularPixelFootprint);"
        "floatphysicalDetailSampleSpacing=cloudDetailSampleSpacing("
        "length(densityLongitudinalFootprint),"
        "physicalProjectedPixelWidth);"
        "float3densityTransverseFootprint="
        "pixelDirectionSpan*currentSampleT;"));
    EXPECT_FALSE(Contains(shader, "cellLaneCentroid"));
    EXPECT_TRUE(Contains(
        shader,
        "float3occupancyCrossSectionWidth="
        "pixelDirectionSpan*occupancySampleT;"));
    EXPECT_TRUE(Contains(
        shader,
        "float3occupancyPhysicalFootprint="
        "abs(dir)*stepLength+occupancyCrossSectionWidth;"));
    EXPECT_TRUE(Contains(
        shader,
        "float2occupancySample=cloudShapeOccupancyAtInterval("
        "occupancyP,occupancyPhysicalFootprint);"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudMacroSamplecurrentMacro=sampleCloudMacro("
        "currentP,coverageTerms,"
        "densityLongitudinalFootprint,0.5*densityTransverseFootprint);"));
    EXPECT_TRUE(Contains(
        shader,
        "float3cornerDirection0=CloudViewDirection("
        "clip.xy-0.5*pixelNdcStepX-0.5*pixelNdcStepY);"
        "float3cornerDirection1=CloudViewDirection("
        "clip.xy+0.5*pixelNdcStepX-0.5*pixelNdcStepY);"
        "float3cornerDirection2=CloudViewDirection("
        "clip.xy-0.5*pixelNdcStepX+0.5*pixelNdcStepY);"
        "float3cornerDirection3=CloudViewDirection("
        "clip.xy+0.5*pixelNdcStepX+0.5*pixelNdcStepY);"));
    EXPECT_TRUE(Contains(
        shader,
        "float3pixelDirectionSpan=max("
        "maximumPixelDirection-minimumPixelDirection,0.0.xxx);"
        "floatangularPixelFootprint=max("
        "length(cornerDirection1-cornerDirection0),"
        "length(cornerDirection2-cornerDirection0));"));
    EXPECT_TRUE(Contains(shader, "floatbillowVisibility=cloudBillowVisibilityFromSampleSpacing(physicalDetailSampleSpacing);"));
    EXPECT_TRUE(Contains(shader, "floatmiddleBillowVisibility=cloudMiddleBillowVisibilityFromSampleSpacing(physicalDetailSampleSpacing);"));
    EXPECT_TRUE(Contains(shader, "floaterosionVisibility=cloudErosionVisibilityFromSampleSpacing(physicalDetailSampleSpacing);"));
    EXPECT_FALSE(Contains(shader, "floatdetailSampleSpacing=cloudDetailSampleSpacing(densityResolutionSpacing,projectedPixelWidth);"));
    EXPECT_FALSE(Contains(shader, "floatdetailStart=12000.0;"));
    EXPECT_FALSE(Contains(shader, "floatdetailEnd=80000.0;"));
    const std::size_t detailBranch = shader.find("[branch]if(detailVisibility>0.001){");
    const std::size_t firstDetailRead = shader.find("detailNoise.SampleLevel(", detailBranch);
    const std::size_t billowBlend = shader.find(
        "billowPotentials=cloudBillowPotentialStates(", firstDetailRead);
    const std::size_t erosionBlend = shader.find(
        "erosionPotential=cloudErosionPotentialOffset(",
        firstDetailRead);
    EXPECT_TRUE(detailBranch != std::string::npos);
    EXPECT_TRUE(firstDetailRead != std::string::npos);
    EXPECT_TRUE(billowBlend != std::string::npos);
    EXPECT_TRUE(erosionBlend != std::string::npos);
    EXPECT_TRUE(detailBranch < firstDetailRead);
    EXPECT_TRUE(firstDetailRead < billowBlend);
    EXPECT_TRUE(billowBlend < erosionBlend);
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudDetailFilteredCondensationDensity("));
    EXPECT_FALSE(Contains(
        shader,
        "cloudBillowPotentialStates(ndA,ndB,h)*billowVisibility"));
    EXPECT_TRUE(Contains(shader, "floatbillowVisibility=cloudBillowVisibilityFromSampleSpacing(sampleSpacing);"));
    EXPECT_TRUE(Contains(shader, "floatmiddleBillowVisibility=cloudMiddleBillowVisibilityFromSampleSpacing(sampleSpacing);"));
    EXPECT_TRUE(Contains(shader, "floaterosionVisibility=cloudErosionVisibilityFromSampleSpacing(sampleSpacing);"));
    EXPECT_TRUE(Contains(shader,
        "cloudDensityOpticalDepthByOrderFromMacro("
        "samplePosition,macro,macro.densityWeatherMask,"
        "billowVisibility,middleBillowVisibility,erosionVisibility,"
        "max(extinctionByOrder,0.0.xxx),"
        "lightDirection,sampleSpacing,"
        "detailedFirstState,detailedSecondState,detailedThirdState);"));
    EXPECT_FALSE(Contains(shader, "lightMacro.weatherMask,0.65,1.0);"));
}

ACS_TEST(VolumetricClouds,
         DensityGaussQuadratureIsFixedAndExactThroughDegreeSeven) {
    using render_internal::ResolveVolumetricCloudDensityQuadrature_Internal;
    const auto invalid = ResolveVolumetricCloudDensityQuadrature_Internal(
        std::numeric_limits<f32>::quiet_NaN());
    const auto zero = ResolveVolumetricCloudDensityQuadrature_Internal(0.0f);
    const auto shortInterval =
        ResolveVolumetricCloudDensityQuadrature_Internal(0.01f);
    const auto longInterval =
        ResolveVolumetricCloudDensityQuadrature_Internal(250000.0f);

    const render_internal::FVolumetricCloudDensityQuadratureInternal plans[]{
        invalid, zero, shortInterval, longInterval};
    for (const auto& plan : plans) {
        EXPECT_EQ(
            plan.sample_count,
            render_internal::kVolumetricCloudDensityGaussSampleCount);
        f32 weightSum = 0.0f;
        f32 polynomialIntegrals[8]{};
        for (u32 index = 0u; index < plan.sample_count; ++index) {
            EXPECT_TRUE(plan.fractions[index] > 0.0f);
            EXPECT_TRUE(plan.fractions[index] < 1.0f);
            EXPECT_TRUE(plan.weights[index] > 0.0f);
            EXPECT_NEAR(
                plan.fractions[index] +
                    plan.fractions[plan.sample_count - 1u - index],
                1.0f, 1.0e-7f);
            EXPECT_NEAR(
                plan.weights[index],
                plan.weights[plan.sample_count - 1u - index],
                1.0e-7f);
            weightSum += plan.weights[index];
            f32 power = 1.0f;
            for (u32 degree = 0u; degree < 8u; ++degree) {
                polynomialIntegrals[degree] += plan.weights[index] * power;
                power *= plan.fractions[index];
            }
        }
        EXPECT_NEAR(weightSum, 1.0f, 1.0e-7f);
        for (u32 index = 0u; index < plan.sample_count; ++index) {
            const u32 firstBoundaryIndex = 2u * index;
            const f32 assignedTransportWidth =
                render_internal::kVolumetricCloudBeerTransportBoundaries[
                    firstBoundaryIndex + 2u] -
                render_internal::kVolumetricCloudBeerTransportBoundaries[
                    firstBoundaryIndex];
            EXPECT_NEAR(
                assignedTransportWidth, plan.weights[index], 1.0e-7f);
            EXPECT_NEAR(
                render_internal::kVolumetricCloudBeerTransportBoundaries[
                    firstBoundaryIndex + 1u],
                plan.fractions[index], 0.0f);
        }
        for (u32 degree = 0u; degree < 8u; ++degree) {
            EXPECT_NEAR(
                polynomialIntegrals[degree],
                1.0f / static_cast<f32>(degree + 1u),
                2.0e-6f);
        }

        // 雲殻・歩進セルとの実交差が任意区間でも、同じ4点をアフィン写像すれば
        // 7次までの積分次数を維持する。支持幅の中点へ置換した旧方式は満たさない。
        constexpr f32 kMappedStart = 0.125f;
        constexpr f32 kMappedEnd = 0.875f;
        constexpr f32 kMappedLength = kMappedEnd - kMappedStart;
        f32 mappedPolynomialIntegrals[8]{};
        for (u32 index = 0u; index < plan.sample_count; ++index) {
            const f32 mappedPosition =
                kMappedStart + plan.fractions[index] * kMappedLength;
            f32 mappedPower = 1.0f;
            for (u32 degree = 0u; degree < 8u; ++degree) {
                mappedPolynomialIntegrals[degree] +=
                    kMappedLength * plan.weights[index] * mappedPower;
                mappedPower *= mappedPosition;
            }
        }
        f32 startPower = kMappedStart;
        f32 endPower = kMappedEnd;
        for (u32 degree = 0u; degree < 8u; ++degree) {
            const f32 expectedIntegral =
                (endPower - startPower) /
                static_cast<f32>(degree + 1u);
            EXPECT_NEAR(
                mappedPolynomialIntegrals[degree],
                expectedIntegral, 2.0e-6f);
            startPower *= kMappedStart;
            endPower *= kMappedEnd;
        }
    }
}

ACS_TEST(VolumetricClouds,
         FarThinIntersectionKeepsGaussAndTransportInsideRealEndpoints) {
    using render_internal::ResolveVolumetricCloudIntervalDistance_Internal;
    constexpr f32 kIntervalStart = 250000.015625f;
    constexpr f32 kIntervalEnd = 250000.031250f;
    constexpr f32 kIntervalLength = kIntervalEnd - kIntervalStart;
    const auto quadrature =
        render_internal::ResolveVolumetricCloudDensityQuadrature_Internal(
            kIntervalLength);

    for (u32 index = 0u; index < quadrature.sample_count; ++index) {
        const f32 sampleDistance =
            ResolveVolumetricCloudIntervalDistance_Internal(
                kIntervalStart, kIntervalEnd,
                quadrature.fractions[index]);
        EXPECT_TRUE(sampleDistance >= kIntervalStart);
        EXPECT_TRUE(sampleDistance <= kIntervalEnd);
    }

    f32 previousBoundary = kIntervalStart;
    f32 integratedLength = 0.0f;
    for (u32 index = 0u;
         index < render_internal::kVolumetricCloudBeerTransportSegmentCount;
         ++index) {
        const f32 segmentStart =
            ResolveVolumetricCloudIntervalDistance_Internal(
                kIntervalStart, kIntervalEnd,
                render_internal::kVolumetricCloudBeerTransportBoundaries[index]);
        const f32 segmentEnd =
            index + 1u >=
                    render_internal::kVolumetricCloudBeerTransportSegmentCount
                ? kIntervalEnd
                : ResolveVolumetricCloudIntervalDistance_Internal(
                    kIntervalStart, kIntervalEnd,
                    render_internal::
                        kVolumetricCloudBeerTransportBoundaries[index + 1u]);
        EXPECT_EQ(segmentStart, previousBoundary);
        EXPECT_TRUE(segmentEnd >= segmentStart);
        EXPECT_TRUE(segmentEnd <= kIntervalEnd);
        integratedLength += segmentEnd - segmentStart;
        previousBoundary = segmentEnd;
    }
    EXPECT_EQ(previousBoundary, kIntervalEnd);
    EXPECT_NEAR(integratedLength, kIntervalLength, 0.0f);
}

ACS_TEST(VolumetricClouds,
         BeerQuadratureIntegratesSeparatedCloudsAtTheirOwnLightingPoints) {
    const auto quadrature =
        render_internal::ResolveVolumetricCloudDensityQuadrature_Internal(
            1.0f);
    const auto densityAt = [](f32 fraction) noexcept {
        const auto lobe = [](f32 fraction, f32 center) noexcept {
            const f32 distance = std::fabs(fraction - center);
            const f32 linear = std::max(
                1.0f - distance / 0.25f, 0.0f);
            return linear * linear * (3.0f - 2.0f * linear);
        };
        return 1.5f * lobe(fraction, 0.20f) +
               1.2f * lobe(fraction, 0.80f);
    };
    const auto sourceAt = [](f32 fraction) noexcept {
        return 0.20f + 0.80f * fraction;
    };
    constexpr f32 kExtinction = 2.0f;
    render_internal::FVolumetricCloudBeerIntegrationInternal integrated{};
    render_internal::FVolumetricCloudDensityLightingSampleInternal samples[4]{};
    f32 densityMean = 0.0f;
    f32 densityMoment = 0.0f;
    for (u32 index = 0u; index < quadrature.sample_count; ++index) {
        const f32 fraction = quadrature.fractions[index];
        const f32 density = densityAt(fraction);
        const f32 weight = quadrature.weights[index];
        samples[index].density = density;
        samples[index].source_radiance = sourceAt(fraction);
        densityMean += density * weight;
        densityMoment += density * weight * fraction;
    }
    render_internal::IntegrateVolumetricCloudBeerCell_Internal(
        integrated, samples, 1.0f, kExtinction,
        1.0f, 0.0f, 0.0f);

    // 独立oracleは同じ4点式を使わず、65536個の微小区間を前から積算する。
    constexpr u32 kReferenceSamples = 65536u;
    f32 referenceTransmittance = 1.0f;
    f32 referenceRadiance = 0.0f;
    f32 referenceOpacity = 0.0f;
    f32 referenceMoment = 0.0f;
    for (u32 index = 0u; index < kReferenceSamples; ++index) {
        const f32 fraction =
            (static_cast<f32>(index) + 0.5f) /
            static_cast<f32>(kReferenceSamples);
        const f32 opticalDepth = densityAt(fraction) * kExtinction /
            static_cast<f32>(kReferenceSamples);
        const f32 intervalTransmittance = std::exp(-opticalDepth);
        const f32 contribution = referenceTransmittance *
            (1.0f - intervalTransmittance);
        referenceOpacity += contribution;
        referenceRadiance += contribution * sourceAt(fraction);
        referenceMoment += contribution * fraction;
        referenceTransmittance *= intervalTransmittance;
    }

    EXPECT_NEAR(integrated.opacity, referenceOpacity, 0.03f);
    EXPECT_NEAR(integrated.radiance, referenceRadiance, 0.01f);
    EXPECT_NEAR(
        integrated.normalized_distance_moment,
        referenceMoment, 0.01f);
    EXPECT_NEAR(
        integrated.opacity,
        1.0f - integrated.transmittance, 2.0e-6f);

    // 旧方式の密度重心一点照明は、中央が空でもそこへ散乱源を移して大きく外れる。
    const f32 densityCentroid = densityMoment / densityMean;
    const f32 centroidRadiance = integrated.opacity *
        sourceAt(densityCentroid);
    EXPECT_TRUE(
        std::fabs(centroidRadiance - referenceRadiance) > 0.04f);
}

ACS_TEST(VolumetricClouds,
         PhysicalSubrayLightingKeepsAbsorptionAndSourceCorrelated) {
    /** 4本中1本だけが持つ光学的深さ。 */
    constexpr f32 opticalDepths[4]{1.0f, 0.0f, 0.0f, 0.0f};
    /** 吸収する光路だけ暗く、残りは明るい局所散乱源。 */
    constexpr f32 laneSources[4]{0.1f, 1.0f, 1.0f, 1.0f};
    /** 吸収と同じ実光路の散乱源を掛けた面積平均。 */
    f32 correlatedRadiance = 0.0f;
    /** 中心視線の明るい散乱源を全光路へ流用した旧方式。 */
    f32 sharedCenterRadiance = 0.0f;
    for (u32 laneIndex = 0u; laneIndex < 4u; ++laneIndex) {
        /** 現在光路のBeer-Lambert吸収率。 */
        const f32 absorption = 1.0f - std::exp(-opticalDepths[laneIndex]);
        correlatedRadiance += 0.25f * absorption * laneSources[laneIndex];
        sharedCenterRadiance += 0.25f * absorption;
    }

    /** 独立した解析解。 */
    const f32 expected = 0.25f * (1.0f - std::exp(-1.0f)) * 0.1f;
    EXPECT_NEAR(correlatedRadiance, expected, 1.0e-7f);
    EXPECT_TRUE(sharedCenterRadiance > correlatedRadiance * 9.9f);

}

ACS_TEST(VolumetricClouds,
         EmptyGaussSamplesCannotInjectLightingAcrossSeparatedClouds) {
    render_internal::FVolumetricCloudDensityLightingSampleInternal samples[4]{};
    samples[0].density = 2.0f;
    samples[0].source_radiance = 0.2f;
    samples[0].second_order_source_radiance = 0.3f;
    samples[0].third_order_source_radiance = 0.4f;
    samples[1].source_radiance = 60000.0f;
    samples[1].second_order_source_radiance = 50000.0f;
    samples[1].third_order_source_radiance = 40000.0f;
    samples[2].source_radiance = 30000.0f;
    samples[2].second_order_source_radiance = 20000.0f;
    samples[2].third_order_source_radiance = 10000.0f;
    samples[3].density = 2.0f;
    samples[3].source_radiance = 0.8f;
    samples[3].second_order_source_radiance = 0.7f;
    samples[3].third_order_source_radiance = 0.6f;

    render_internal::FVolumetricCloudDensityLightingSampleInternal cleanSamples[4]{
        samples[0], samples[1], samples[2], samples[3]};
    cleanSamples[1].source_radiance = 0.0f;
    cleanSamples[1].second_order_source_radiance = 0.0f;
    cleanSamples[1].third_order_source_radiance = 0.0f;
    cleanSamples[2].source_radiance = 0.0f;
    cleanSamples[2].second_order_source_radiance = 0.0f;
    cleanSamples[2].third_order_source_radiance = 0.0f;

    render_internal::FVolumetricCloudBeerIntegrationInternal withDummy{};
    render_internal::FVolumetricCloudBeerIntegrationInternal withoutDummy{};
    constexpr f32 kSecondContribution = 0.28f;
    constexpr f32 kSecondOcclusion = 0.28f;
    constexpr f32 kThirdContribution =
        kSecondContribution * kSecondContribution;
    constexpr f32 kThirdOcclusion =
        kSecondOcclusion * kSecondOcclusion;
    render_internal::IntegrateVolumetricCloudBeerCell_Internal(
        withDummy, samples, 1.0f, 2.0f,
        0.8f, 0.7f, 0.6f,
        kSecondContribution, kSecondOcclusion,
        kThirdContribution, kThirdOcclusion);
    render_internal::IntegrateVolumetricCloudBeerCell_Internal(
        withoutDummy, cleanSamples, 1.0f, 2.0f,
        0.8f, 0.7f, 0.6f,
        kSecondContribution, kSecondOcclusion,
        kThirdContribution, kThirdOcclusion);

    EXPECT_NEAR(withDummy.first_order_radiance,
                withoutDummy.first_order_radiance, 1.0e-7f);
    EXPECT_NEAR(withDummy.second_order_radiance,
                withoutDummy.second_order_radiance, 1.0e-7f);
    EXPECT_NEAR(withDummy.third_order_radiance,
                withoutDummy.third_order_radiance, 1.0e-7f);
    EXPECT_NEAR(withDummy.radiance, withoutDummy.radiance, 1.0e-7f);
}

ACS_TEST(VolumetricClouds,
         LightingSourceLimitsOuterSlopeWithoutBreakingLinearFields) {
    constexpr f32 kLeftFraction = 0.0694318442f;
    constexpr f32 kRightFraction = 0.3300094782f;
    EXPECT_NEAR(
        render_internal::ResolveVolumetricCloudLinearSourcePair_Internal(
            kLeftFraction, kRightFraction, true, true,
            kLeftFraction, kRightFraction, 0.0f, 1.0f),
        0.0f, 1.0e-7f);
    constexpr f32 kUpperLeftFraction = 0.6699905218f;
    constexpr f32 kUpperRightFraction = 0.9305681558f;
    EXPECT_NEAR(
        render_internal::ResolveVolumetricCloudLinearSourcePair_Internal(
            kUpperLeftFraction, kUpperRightFraction, true, true,
            kUpperLeftFraction, kUpperRightFraction, 1.0f, 1.0f),
        1.0f, 1.0e-7f);
    EXPECT_TRUE(
        render_internal::ResolveVolumetricCloudLinearSourcePair_Internal(
            0.1f, 1.0f, true, true,
            kLeftFraction, kRightFraction, 0.0f, 1.0f) >= 0.0f);
}

ACS_TEST(VolumetricClouds,
         BeerCentroidUsesStableSeriesAcrossThinOpticalDepths) {
    constexpr f32 kOpticalDepth = 0.001002f;
    const f32 transmittance = std::exp(-kOpticalDepth);
    const f32 lowerTransmittance = std::nextafter(
        transmittance, 0.0f);
    const f32 upperTransmittance = std::nextafter(
        transmittance, 1.0f);
    const f32 centroid =
        render_internal::ResolveVolumetricCloudBeerAbsorptionCentroidFromTransmittance_Internal(
            kOpticalDepth, transmittance);
    const f32 lowerCentroid =
        render_internal::ResolveVolumetricCloudBeerAbsorptionCentroidFromTransmittance_Internal(
            kOpticalDepth, lowerTransmittance);
    const f32 upperCentroid =
        render_internal::ResolveVolumetricCloudBeerAbsorptionCentroidFromTransmittance_Internal(
            kOpticalDepth, upperTransmittance);

    // 透過率の単精度丸めへ依存せず、解析級数の約0.4999165を保つ。
    EXPECT_NEAR(centroid, 0.4999165f, 1.0e-7f);
    EXPECT_EQ(lowerCentroid, centroid);
    EXPECT_EQ(upperCentroid, centroid);
    EXPECT_NEAR(
        render_internal::ResolveVolumetricCloudBeerAbsorptionCentroid_Internal(
            0.1f),
        0.4916680552f, 2.0e-7f);

    const f32 belowFormerBoundary = std::nextafter(0.1f, 0.0f);
    const f32 aboveFormerBoundary = std::nextafter(0.1f, 1.0f);
    const f32 belowFormerCentroid =
        render_internal::ResolveVolumetricCloudBeerAbsorptionCentroid_Internal(
            belowFormerBoundary);
    const f32 aboveFormerCentroid =
        render_internal::ResolveVolumetricCloudBeerAbsorptionCentroid_Internal(
            aboveFormerBoundary);
    // 旧0.1境界では約75 ULPの段差があった。広い高次級数で境界を消す。
    EXPECT_NEAR(belowFormerCentroid, aboveFormerCentroid, 2.0e-7f);

    const f32 belowSeriesBoundary = std::nextafter(1.0f, 0.0f);
    const f32 aboveSeriesBoundary = std::nextafter(1.0f, 2.0f);
    const f32 belowSeriesCentroid =
        render_internal::ResolveVolumetricCloudBeerAbsorptionCentroid_Internal(
            belowSeriesBoundary);
    const f32 aboveSeriesCentroid =
        render_internal::ResolveVolumetricCloudBeerAbsorptionCentroid_Internal(
            aboveSeriesBoundary);
    EXPECT_NEAR(belowSeriesCentroid, aboveSeriesCentroid, 5.0e-7f);
}

ACS_TEST(VolumetricClouds,
         FinitePathBoundCoversCancellationAndSplitFloatTransport) {
    constexpr f32 kOpticalDepth = 0.0010100000072270632f;
    constexpr u32 kSplitSegmentCount = 4u;
    constexpr f32 kFirstSource = 3.0f;
    constexpr f32 kSecondSource = 2.0f;
    constexpr f32 kThirdSource = 1.5f;
    constexpr f32 kSecondContribution = 0.28f;
    constexpr f32 kSecondOcclusion = 0.28f;
    constexpr f32 kThirdContribution =
        kSecondContribution * kSecondContribution;
    constexpr f32 kThirdOcclusion =
        kSecondOcclusion * kSecondOcclusion;
    const auto bound =
        render_internal::ResolveVolumetricCloudTransportErrorBound_Internal(
            1.0f, 1.0f, 1.0f,
            kOpticalDepth, kSplitSegmentCount,
            kFirstSource, kSecondSource, kThirdSource,
            kSecondContribution, kSecondOcclusion,
            kThirdContribution, kThirdOcclusion);
    const f64 mathematicalOpacity =
        1.0 - std::exp(-static_cast<f64>(kOpticalDepth));
    const f32 splitOpticalDepth =
        kOpticalDepth / static_cast<f32>(kSplitSegmentCount);
    const f64 exactSplitOpticalDepth =
        static_cast<f64>(splitOpticalDepth);
    const f64 exactSplitAbsorption =
        -std::expm1(-exactSplitOpticalDepth);
    const f32 expectedSplitAbsorption =
        static_cast<f32>(exactSplitAbsorption);
    const auto positiveFloatUlp = [](f32 value) noexcept {
        return std::nextafter(
            value, std::numeric_limits<f32>::infinity()) - value;
    };
    const auto exactReducedWeight = [](
        f64 opticalDepth, f64 contribution, f64 occlusion) noexcept {
        if (occlusion <= 0.0) return contribution * opticalDepth;
        const f64 reducedOpticalDepth = opticalDepth * occlusion;
        return contribution * opticalDepth *
            (-std::expm1(-reducedOpticalDepth) / reducedOpticalDepth);
    };
    f32 splitTransmittance = 1.0f;
    f32 splitSecondTransmittance = 1.0f;
    f32 splitThirdTransmittance = 1.0f;
    f32 splitOpacity = 0.0f;
    f32 splitRadiance = 0.0f;
    f32 splitFirstRadiance = 0.0f;
    f32 splitSecondRadiance = 0.0f;
    f32 splitThirdRadiance = 0.0f;
    f64 exactSplitTransmittance = 1.0;
    f64 exactSplitSecondTransmittance = 1.0;
    f64 exactSplitThirdTransmittance = 1.0;
    f64 exactSplitRadiance = 0.0;
    for (u32 segmentIndex = 0u;
         segmentIndex < kSplitSegmentCount; ++segmentIndex) {
        const f32 intervalAbsorption =
            render_internal::ResolveVolumetricCloudBeerAbsorptionFraction_Internal(
                splitOpticalDepth);
        const f32 secondOpticalDepth =
            splitOpticalDepth * kSecondOcclusion;
        const f32 thirdOpticalDepth =
            splitOpticalDepth * kThirdOcclusion;
        const f32 secondAbsorption =
            render_internal::ResolveVolumetricCloudBeerAbsorptionFraction_Internal(
                secondOpticalDepth);
        const f32 thirdAbsorption =
            render_internal::ResolveVolumetricCloudBeerAbsorptionFraction_Internal(
                thirdOpticalDepth);
        const f32 intervalTransmittance = 1.0f - intervalAbsorption;
        const f32 secondIntervalTransmittance = 1.0f - secondAbsorption;
        const f32 thirdIntervalTransmittance = 1.0f - thirdAbsorption;
        const f32 nextTransmittance =
            splitTransmittance * intervalTransmittance;
        const f32 firstWeight = splitTransmittance > nextTransmittance
            ? splitTransmittance - nextTransmittance : 0.0f;
        const f32 secondWeight = splitSecondTransmittance *
            render_internal::ResolveVolumetricCloudReducedIntervalScatteringWeight_Internal(
                splitOpticalDepth, secondIntervalTransmittance,
                kSecondContribution, kSecondOcclusion);
        const f32 thirdWeight = splitThirdTransmittance *
            render_internal::ResolveVolumetricCloudReducedIntervalScatteringWeight_Internal(
                splitOpticalDepth, thirdIntervalTransmittance,
                kThirdContribution, kThirdOcclusion);
        const f32 expectedSecondWeight = static_cast<f32>(
            exactReducedWeight(
                exactSplitOpticalDepth, kSecondContribution,
                kSecondOcclusion));
        const f32 expectedThirdWeight = static_cast<f32>(
            exactReducedWeight(
                exactSplitOpticalDepth, kThirdContribution,
                kThirdOcclusion));
        EXPECT_NEAR(
            intervalAbsorption, expectedSplitAbsorption,
            positiveFloatUlp(expectedSplitAbsorption) * 8.0f);
        EXPECT_NEAR(
            secondWeight / splitSecondTransmittance,
            expectedSecondWeight,
            positiveFloatUlp(expectedSecondWeight) * 16.0f);
        EXPECT_NEAR(
            thirdWeight / splitThirdTransmittance,
            expectedThirdWeight,
            positiveFloatUlp(expectedThirdWeight) * 16.0f);
        const f32 firstRadiance = firstWeight * kFirstSource;
        const f32 secondRadiance = secondWeight * kSecondSource;
        const f32 thirdRadiance = thirdWeight * kThirdSource;
        splitOpacity += firstWeight;
        splitRadiance +=
            (firstRadiance + secondRadiance) + thirdRadiance;
        splitFirstRadiance += firstRadiance;
        splitSecondRadiance += secondRadiance;
        splitThirdRadiance += thirdRadiance;

        const f64 exactFirstWeight =
            exactSplitTransmittance * exactSplitAbsorption;
        const f64 exactSecondWeight = exactSplitSecondTransmittance *
            exactReducedWeight(
                exactSplitOpticalDepth, kSecondContribution,
                kSecondOcclusion);
        const f64 exactThirdWeight = exactSplitThirdTransmittance *
            exactReducedWeight(
                exactSplitOpticalDepth, kThirdContribution,
                kThirdOcclusion);
        exactSplitRadiance +=
            (exactFirstWeight * kFirstSource +
             exactSecondWeight * kSecondSource) +
            exactThirdWeight * kThirdSource;
        splitTransmittance = nextTransmittance;
        splitSecondTransmittance *= secondIntervalTransmittance;
        splitThirdTransmittance *= thirdIntervalTransmittance;
        exactSplitTransmittance *= std::exp(-exactSplitOpticalDepth);
        exactSplitSecondTransmittance *=
            std::exp(-exactSplitOpticalDepth * kSecondOcclusion);
        exactSplitThirdTransmittance *=
            std::exp(-exactSplitOpticalDepth * kThirdOcclusion);
    }

    // 指数差の32 ULP拡張では届かなかった実例を、独立した倍精度の
    // Beer-Lambert解と三つの散乱次数で照合してから上から包む。
    EXPECT_TRUE(
        static_cast<f64>(bound.remaining_first_order_opacity) >=
        mathematicalOpacity);
    EXPECT_TRUE(
        bound.remaining_first_order_opacity >= splitOpacity);
    EXPECT_TRUE(bound.remaining_radiance >= splitRadiance);
    const f32 expectedSplitRadiance =
        static_cast<f32>(exactSplitRadiance);
    // 1付近の透過率差は各区間で最大1 ULPを失い得る。吸収率そのものは
    // 上の独立oracleで厳しく照合し、総和には減算の解析上限だけを許す。
    const f32 splitRadianceCancellationTolerance =
        static_cast<f32>(kSplitSegmentCount) * kFirstSource *
        render_internal::kVolumetricCloudFloatUlpAtOne;
    EXPECT_NEAR(
        splitRadiance, expectedSplitRadiance,
        splitRadianceCancellationTolerance);
    EXPECT_TRUE(splitFirstRadiance > 0.0f);
    EXPECT_TRUE(splitSecondRadiance > 0.0f);
    EXPECT_TRUE(splitThirdRadiance > 0.0f);
    EXPECT_TRUE(
        render_internal::ResolveVolumetricCloudBeerAbsorptionFraction_Internal(
            kOpticalDepth) <= kOpticalDepth);
    EXPECT_EQ(
        render_internal::ResolveVolumetricCloudReducedIntervalScatteringWeight_Internal(
            1.0f, 0.36787945f,
            std::numeric_limits<f32>::infinity(), 1.0f),
        0.0f);
}

ACS_TEST(VolumetricClouds,
         RemainingCellBudgetAndLargeDistanceStayConservative) {
    // 現帯域の最後の一セル、次帯域の最初の一セル、二セル粗探索を
    // 一セルへ細分化した直後のいずれも八輸送区間を残す。
    EXPECT_EQ(
        render_internal::ResolveVolumetricCloudRemainingTransportSegmentCount_Internal(
            8u, 7u, false, 0u),
        8u);
    EXPECT_EQ(
        render_internal::ResolveVolumetricCloudRemainingTransportSegmentCount_Internal(
            8u, 8u, true, 1u),
        8u);
    EXPECT_EQ(
        render_internal::ResolveVolumetricCloudRemainingTransportSegmentCount_Internal(
            2u, 1u, false, 0u),
        8u);
    EXPECT_EQ(
        render_internal::ResolveVolumetricCloudRemainingTransportSegmentCount_Internal(
            8u, 8u, false, 1u),
        0u);
    constexpr u32 kLargestExactCellCount =
        0xFFFFFFFFu /
        render_internal::kVolumetricCloudBeerTransportSegmentCount;
    EXPECT_EQ(
        render_internal::ResolveVolumetricCloudRemainingTransportSegmentCount_Internal(
            kLargestExactCellCount, 0u, false, 0u),
        0xFFFFFFF8u);
    EXPECT_EQ(
        render_internal::ResolveVolumetricCloudRemainingTransportSegmentCount_Internal(
            kLargestExactCellCount + 1u, 0u, false, 0u),
        0xFFFFFFFFu);
    EXPECT_EQ(
        render_internal::ResolveVolumetricCloudRemainingTransportSegmentCount_Internal(
            kLargestExactCellCount, 0u, true, 1u),
        0xFFFFFFFFu);

    constexpr f32 kTraceEnd = 250000.0f;
    const f32 currentEnd = std::nextafter(kTraceEnd, 0.0f);
    const f32 traceEndUpper =
        render_internal::InflateVolumetricCloudPositiveUpper_Internal(
            kTraceEnd);
    const f32 remainingDistance =
        render_internal::SubtractVolumetricCloudPositiveUpper_Internal(
            traceEndUpper, currentEnd);
    const f64 exactRemainingDistance =
        static_cast<f64>(kTraceEnd) - static_cast<f64>(currentEnd);
    EXPECT_TRUE(
        static_cast<f64>(remainingDistance) >= exactRemainingDistance);
    EXPECT_TRUE(remainingDistance > 0.0f);

    const auto noRemainingCell =
        render_internal::ResolveVolumetricCloudTransportErrorBound_Internal(
            0.5f, 0.0f, 0.0f, 0.0f, 0u,
            0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f);
    const auto oneRemainingCell =
        render_internal::ResolveVolumetricCloudTransportErrorBound_Internal(
            0.5f, 0.0f, 0.0f, 0.0f,
            render_internal::ResolveVolumetricCloudRemainingTransportSegmentCount_Internal(
                2u, 1u, false, 0u),
            0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_TRUE(
        render_internal::CanTerminateVolumetricCloudTransport_Internal(
            noRemainingCell, 0.5f, 500.0f, kTraceEnd));
    EXPECT_FALSE(
        render_internal::CanTerminateVolumetricCloudTransport_Internal(
            oneRemainingCell, 0.5f, 500.0f, kTraceEnd));
}

ACS_TEST(VolumetricClouds,
         ReducedScatteringUsesOneOpticalDepthAtSmallOcclusion) {
    const auto quadrature =
        render_internal::ResolveVolumetricCloudDensityQuadrature_Internal(
            1.0f);
    render_internal::FVolumetricCloudDensityLightingSampleInternal samples[4]{};
    for (u32 index = 0u; index < quadrature.sample_count; ++index) {
        samples[index].density = 10000.0f;
        samples[index].second_order_source_radiance = 1.0f;
    }
    render_internal::FVolumetricCloudBeerIntegrationInternal integrated{};
    constexpr f32 kContribution = 1.0e-4f;
    constexpr f32 kOcclusion = 1.0e-4f;
    render_internal::IntegrateVolumetricCloudBeerCell_Internal(
        integrated, samples, 1.0f, 1.0f,
        0.0f, 1.0f, 0.0f,
        kContribution, kOcclusion);

    const f32 expected = (kContribution / kOcclusion) *
        (1.0f - std::exp(-10000.0f * kOcclusion));
    EXPECT_NEAR(integrated.second_order_radiance, expected, 2.0e-6f);
    EXPECT_NEAR(integrated.second_order_transmittance,
                std::exp(-1.0f), 2.0e-6f);
}

ACS_TEST(VolumetricClouds,
         BeerCellAnalyticTransportKeepsThickLinearSourceAtTheFront) {
    const auto quadrature =
        render_internal::ResolveVolumetricCloudDensityQuadrature_Internal(
            1.0f);
    render_internal::FVolumetricCloudDensityLightingSampleInternal samples[4]{};
    for (u32 index = 0u; index < quadrature.sample_count; ++index) {
        samples[index].density = 100.0f;
        samples[index].source_radiance = quadrature.fractions[index];
    }
    render_internal::FVolumetricCloudBeerIntegrationInternal integrated{};
    render_internal::IntegrateVolumetricCloudBeerCell_Internal(
        integrated, samples, 1.0f, 1.0f,
        1.0f, 0.0f, 0.0f);

    // 均一密度と線形散乱源S(x)=xの解析解。旧逐次Gauss薄層方式は
    // 光学的深さ100で約0.069へ寄るが、連続媒質の平均吸収位置は約0.01である。
    constexpr f32 kOpticalDepth = 100.0f;
    const f32 expectedOpacity = 1.0f - std::exp(-kOpticalDepth);
    const f32 expectedCentroid =
        render_internal::ResolveVolumetricCloudBeerAbsorptionCentroid_Internal(
            kOpticalDepth);
    EXPECT_NEAR(integrated.opacity, expectedOpacity, 1.0e-6f);
    EXPECT_NEAR(integrated.transmittance, std::exp(-kOpticalDepth), 1.0e-8f);
    EXPECT_NEAR(
        integrated.normalized_distance_moment,
        expectedOpacity * expectedCentroid, 2.0e-6f);
    EXPECT_NEAR(
        integrated.radiance,
        expectedOpacity * expectedCentroid, 2.0e-6f);
    EXPECT_TRUE(integrated.radiance < 0.011f);
}

ACS_TEST(VolumetricClouds,
         BeerCellKeepsDecreasingLinearSourceAtTheFrontBoundary) {
    const auto quadrature =
        render_internal::ResolveVolumetricCloudDensityQuadrature_Internal(
            1.0f);
    render_internal::FVolumetricCloudDensityLightingSampleInternal samples[4]{};
    for (u32 index = 0u; index < quadrature.sample_count; ++index) {
        samples[index].density = 100.0f;
        samples[index].source_radiance =
            1.0f - quadrature.fractions[index];
    }
    render_internal::FVolumetricCloudBeerIntegrationInternal integrated{};
    render_internal::IntegrateVolumetricCloudBeerCell_Internal(
        integrated, samples, 1.0f, 1.0f,
        1.0f, 0.0f, 0.0f);

    constexpr f32 kOpticalDepth = 100.0f;
    const f32 expectedOpacity = 1.0f - std::exp(-kOpticalDepth);
    const f32 expectedCentroid =
        render_internal::ResolveVolumetricCloudBeerAbsorptionCentroid_Internal(
            kOpticalDepth);
    EXPECT_NEAR(
        integrated.radiance,
        expectedOpacity * (1.0f - expectedCentroid), 2.0e-6f);
    EXPECT_TRUE(integrated.radiance > 0.989f);
}

ACS_TEST(VolumetricClouds,
         R16AndDepthBoundsPreventPrematureTransportTermination) {
    const auto hdrBound =
        render_internal::ResolveVolumetricCloudTransportErrorBound_Internal(
            0.011f, 0.011f, 0.008f,
            1000.0f, 4096u,
            1500.0f, 1200.0f, 900.0f,
            1.0f, 1.0f, 1.0f, 1.0f);

    // 旧条件では一次透過率0.012未満だけで終了できたが、HDR太陽光では
    // 残りの高次散乱が二十を越える雲輝度をまだ加え得る。
    EXPECT_TRUE(hdrBound.remaining_radiance > 20.0f);
    EXPECT_FALSE(
        render_internal::CanTerminateVolumetricCloudTransport_Internal(
            hdrBound, 1.0f, 989.0f, 10000.0f));

    // 色の残差が0でも、有限経路の残り不透明度が遠方へ加わってR32F深度を
    // 変える場合は終了しない。
    const auto depthStillChanges =
        render_internal::ResolveVolumetricCloudTransportErrorBound_Internal(
            1.0e-5f, 0.0f, 0.0f,
            100.0f, 512u,
            0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f);
    const f32 depthOpacity =
        1.0f - depthStillChanges.first_order_transmittance;
    EXPECT_FALSE(
        render_internal::CanTerminateVolumetricCloudTransport_Internal(
            depthStillChanges, 1.0f,
            depthOpacity * 1000.0f, 60000.0f));

    // 一次透過率が非0でも、有限な残り経路を加えた全保存値が同じ表現なら
    // 厚い媒質の残りセルを安全に省ける。
    const auto finitePathQuantizedAway =
        render_internal::ResolveVolumetricCloudTransportErrorBound_Internal(
            1.0e-9f, 0.0f, 0.0f,
            1.0f, 8u,
            0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_TRUE(
        render_internal::CanTerminateVolumetricCloudTransport_Internal(
            finitePathQuantizedAway, 1.0f,
            0.0f, 0.0f));

    EXPECT_EQ(
        render_internal::ResolveVolumetricCloudR16Code_Internal(1.00048f),
        static_cast<u16>(0x3C00u));
    EXPECT_EQ(
        render_internal::ResolveVolumetricCloudR16Code_Internal(1.00049f),
        static_cast<u16>(0x3C01u));
    const auto crossesRoundingBoundary =
        render_internal::ResolveVolumetricCloudTransportErrorBound_Internal(
            0.0f, 1.0e-5f, 0.0f,
            100.0f, 8u,
            0.0f, 1.0f, 0.0f,
            1.0f, 1.0f, 0.0f, 0.0f);
    EXPECT_FALSE(
        render_internal::CanTerminateVolumetricCloudTransport_Internal(
            crossesRoundingBoundary, 1.00048f,
            0.0f, 0.0f));

    const auto quantizedAway =
        render_internal::ResolveVolumetricCloudTransportErrorBound_Internal(
            0.0f, 1.0e-8f, 0.0f,
            100.0f, 8u,
            0.0f, 1.0f, 0.0f,
            1.0f, 1.0f, 0.0f, 0.0f);
    EXPECT_TRUE(
        render_internal::CanTerminateVolumetricCloudTransport_Internal(
            quantizedAway, 1.0f,
            0.0f, 0.0f));

    EXPECT_TRUE(
        render_internal::InflateVolumetricCloudPositiveUpper_Internal(
            1.0e-5f) > 1.0e-5f);

    const auto invalid =
        render_internal::ResolveVolumetricCloudTransportErrorBound_Internal(
            std::numeric_limits<f32>::quiet_NaN(),
            0.0f, 0.0f, 0.0f, 0u,
            0.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f);
    EXPECT_FALSE(
        render_internal::CanTerminateVolumetricCloudTransport_Internal(
            invalid, 1.0f,
            0.0f, 0.0f));
}

ACS_TEST(VolumetricClouds,
         R32OpacityAndFiniteDepthAreRequiredForTransportTermination) {
    render_internal::FVolumetricCloudTransportErrorBoundInternal
        opacityStillChanges{};
    opacityStillChanges.first_order_transmittance = 0.5f;
    opacityStillChanges.remaining_first_order_opacity =
        std::ldexp(1.0f, -24);
    opacityStillChanges.remaining_radiance = 0.0f;
    // R16Fでは同じ0.5でも、cloudDepthOut.yのR32Fは次の表現へ進む。
    EXPECT_FALSE(
        render_internal::CanTerminateVolumetricCloudTransport_Internal(
            opacityStillChanges, 0.5f, 500.0f, 1000.0f));

    render_internal::FVolumetricCloudTransportErrorBoundInternal
        overflowingDepth{};
    overflowingDepth.first_order_transmittance = 0.9998f;
    overflowingDepth.remaining_first_order_opacity = 0.0f;
    overflowingDepth.remaining_radiance = 0.0f;
    // 入力自体が有限でも除算後の深度が無限になる場合は終了しない。
    EXPECT_FALSE(
        render_internal::CanTerminateVolumetricCloudTransport_Internal(
            overflowingDepth, 1.0f,
            std::numeric_limits<f32>::max(), 0.0f));
}

ACS_TEST(VolumetricClouds,
         BaseShapeUsesFrequencyBandsInsteadOfSpatialAverages) {
    constexpr f32 coarseShape = 0.20f;
    constexpr f32 middleShape = 0.50f;
    constexpr f32 completeShape = 0.80f;
    constexpr f32 fineTransitionStart = 0.25f / 8.0f;
    constexpr f32 fineTransitionMiddle = 0.375f / 8.0f;
    constexpr f32 fineTransitionEnd = 0.50f / 8.0f;
    constexpr f32 middleTransitionMiddle = 0.375f / 4.0f;
    constexpr f32 middleTransitionEnd = 0.50f / 4.0f;
    EXPECT_NEAR(
        CloudBandLimitedShapeForTest(
            coarseShape, middleShape, completeShape, 0.0f),
        completeShape, 0.0f);
    EXPECT_NEAR(
        CloudBandLimitedShapeForTest(
            coarseShape, middleShape, completeShape,
            fineTransitionStart),
        completeShape, 1.0e-6f);
    EXPECT_NEAR(
        CloudBandLimitedShapeForTest(
            coarseShape, middleShape, completeShape,
            fineTransitionMiddle),
        0.65f, 1.0e-6f);
    EXPECT_NEAR(
        CloudBandLimitedShapeForTest(
            coarseShape, middleShape, completeShape,
            fineTransitionEnd),
        middleShape, 1.0e-6f);
    EXPECT_NEAR(
        CloudBandLimitedShapeForTest(
            coarseShape, middleShape, completeShape,
            middleTransitionMiddle),
        0.35f, 1.0e-6f);
    EXPECT_NEAR(
        CloudBandLimitedShapeForTest(
            coarseShape, middleShape, completeShape,
            middleTransitionEnd),
        coarseShape, 1.0e-6f);
    const auto coarseTransitionMiddle =
        render_internal::ResolveVolumetricCloudShapeFrequencyBlend_Internal(
            0.375f / 2.0f);
    EXPECT_NEAR(coarseTransitionMiddle.coarse, 0.5f, 1.0e-6f);
    EXPECT_NEAR(coarseTransitionMiddle.unresolved, 0.5f, 1.0e-6f);
    const auto coarseTransitionEnd =
        render_internal::ResolveVolumetricCloudShapeFrequencyBlend_Internal(
            0.50f / 2.0f);
    EXPECT_NEAR(coarseTransitionEnd.coarse, 0.0f, 1.0e-6f);
    EXPECT_NEAR(coarseTransitionEnd.unresolved, 1.0f, 1.0e-6f);

    f32 previousCompleteWeight = 1.0f;
    for (u32 step = 0u; step <= 512u; ++step) {
        const f32 footprint = static_cast<f32>(step) / 2048.0f;
        const auto blend =
            render_internal::ResolveVolumetricCloudShapeFrequencyBlend_Internal(
                footprint);
        EXPECT_TRUE(blend.unresolved >= 0.0f && blend.unresolved <= 1.0f);
        EXPECT_TRUE(blend.coarse >= 0.0f && blend.coarse <= 1.0f);
        EXPECT_TRUE(blend.middle >= 0.0f && blend.middle <= 1.0f);
        EXPECT_TRUE(blend.complete >= 0.0f && blend.complete <= 1.0f);
        EXPECT_NEAR(
            blend.unresolved + blend.coarse + blend.middle + blend.complete,
            1.0f, 1.0e-6f);
        EXPECT_TRUE(blend.complete <= previousCompleteWeight + 1.0e-6f);

        // 実装関数を再利用せず、三つのsmoothstepから四状態の重みを独立に作る。
        auto transitionVisibility = [](f32 domainFootprint) noexcept {
            f32 transition = (domainFootprint - 0.25f) * 4.0f;
            if (transition < 0.0f) transition = 0.0f;
            if (transition > 1.0f) transition = 1.0f;
            return 1.0f - transition * transition *
                (3.0f - 2.0f * transition);
        };
        const f32 expectedComplete =
            transitionVisibility(footprint * 8.0f);
        const f32 expectedMiddleVisibility =
            transitionVisibility(footprint * 4.0f);
        const f32 expectedCoarseVisibility =
            transitionVisibility(footprint * 2.0f);
        const f32 expectedMiddle =
            (1.0f - expectedComplete) * expectedMiddleVisibility;
        const f32 expectedCoarseOwner =
            (1.0f - expectedComplete) *
            (1.0f - expectedMiddleVisibility);
        const f32 expectedCoarse =
            expectedCoarseOwner * expectedCoarseVisibility;
        const f32 expectedUnresolved =
            expectedCoarseOwner * (1.0f - expectedCoarseVisibility);
        EXPECT_NEAR(blend.complete, expectedComplete, 1.0e-6f);
        EXPECT_NEAR(blend.middle, expectedMiddle, 1.0e-6f);
        EXPECT_NEAR(blend.coarse, expectedCoarse, 1.0e-6f);
        EXPECT_NEAR(blend.unresolved, expectedUnresolved, 1.0e-6f);
        previousCompleteWeight = blend.complete;
    }

    const auto invalid =
        render_internal::ResolveVolumetricCloudShapeFrequencyBlend_Internal(
            std::numeric_limits<f32>::quiet_NaN());
    EXPECT_NEAR(invalid.unresolved, 1.0f, 0.0f);
    EXPECT_NEAR(invalid.coarse, 0.0f, 0.0f);
    EXPECT_NEAR(invalid.middle, 0.0f, 0.0f);
    EXPECT_NEAR(invalid.complete, 0.0f, 0.0f);

    // 未解像終端の4点求積は確率1、平均0、生成Perlinと同じ分散を持つ。
    const f32 unresolvedWeightSum =
        2.0f * render_internal::kVolumetricCloudUnresolvedCoarseInnerWeight +
        2.0f * render_internal::kVolumetricCloudUnresolvedCoarseOuterWeight;
    const f32 unresolvedSecondMoment = 2.0f *
        render_internal::kVolumetricCloudUnresolvedCoarseInnerWeight *
        render_internal::kVolumetricCloudUnresolvedCoarseInnerPotential *
        render_internal::kVolumetricCloudUnresolvedCoarseInnerPotential +
        2.0f * render_internal::kVolumetricCloudUnresolvedCoarseOuterWeight *
        render_internal::kVolumetricCloudUnresolvedCoarseOuterPotential *
        render_internal::kVolumetricCloudUnresolvedCoarseOuterPotential;
    EXPECT_NEAR(unresolvedWeightSum, 1.0f, 1.0e-6f);
    EXPECT_NEAR(
        unresolvedSecondMoment,
        render_internal::kVolumetricCloudUnresolvedCoarsePotentialVariance,
        1.0e-7f);

    // 周波数帯の切り替えは形状表現の連続補間であり、別々の面積には分けない。
    const auto transitionBlend =
        render_internal::ResolveVolumetricCloudShapeFrequencyBlend_Internal(
            fineTransitionMiddle);
    const f32 densityAfterBandCompletion =
        render_internal::ResolveVolumetricCloudBandDensity_Internal(
            -0.80f, -0.20f, 0.80f, transitionBlend, 0.0f, 0.0f);
    const f32 averagedPotential =
        -0.80f * transitionBlend.coarse +
        -0.20f * transitionBlend.middle +
        0.80f * transitionBlend.complete;
    const f32 densityAfterPotentialAverage =
        render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
            averagedPotential);
    EXPECT_NEAR(densityAfterBandCompletion, 0.40f, 1.0e-6f);
    EXPECT_NEAR(densityAfterPotentialAverage, 0.30f, 1.0e-6f);
    const auto transitionLanes = render_internal::
        ResolveVolumetricCloudBandDensityLanes_Internal(
            -0.80f, -0.20f, 0.80f,
            transitionBlend, 0.0f, 0.0f);
    for (u32 lane = 0u; lane < 4u; ++lane)
        EXPECT_NEAR(transitionLanes.densities[lane], 0.40f, 1.0e-6f);
    render_internal::FVolumetricCloudLaneOpticalDepthInternal
        transitionDepth{};
    render_internal::AccumulateVolumetricCloudLaneOpticalDepth_Internal(
        transitionDepth, transitionLanes, 10.0f);
    const f32 laneVisibility = render_internal::
        ResolveVolumetricCloudLaneTransmittance_Internal(transitionDepth);
    EXPECT_NEAR(laneVisibility, std::exp(-4.0f), 1.0e-6f);
    const auto zeroScaledLanes = render_internal::
        ScaleVolumetricCloudDensityLanes_Internal(transitionLanes, 0.0f);
    for (u32 lane = 0u; lane < 4u; ++lane)
        EXPECT_NEAR(zeroScaledLanes.densities[lane], 0.0f, 0.0f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader,
        "macro.shapeFrequencyWeights=cloudShapeFrequencyWeights("
        "maximumDomainFootprint);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudBandLimitedCondensationDensity("));
    EXPECT_TRUE(Contains(
        shader,
        "float4cloudShapePotentialBands(float3uvw){"));
    EXPECT_TRUE(Contains(
        shader,
        "macro.weather=cloudWeatherDataBandLimitedPoint("
        "p,safeFootprint.xz);"));
    EXPECT_FALSE(Contains(shader, "cloudStoredBaseNoise("));
    EXPECT_FALSE(Contains(shader, "periodicShapeLineSum("));
    EXPECT_FALSE(Contains(shader, "filtered*0.25"));
    EXPECT_TRUE(Contains(
        shader,
        "sampleCloudMacroLightingSegment("
        "p,saturate(params.x),sun,stepLength);"));
}

ACS_TEST(VolumetricClouds,
         PhysicalSubrayLanesPreserveSpatialIdentityAndPathPartition) {
    using namespace render_internal;
    FVolumetricCloudShapeFrequencyBlendInternal unresolvedBlend{};
    unresolvedBlend.unresolved = 1.0f;

    const auto unresolvedMeanLanes =
        ResolveVolumetricCloudBandDensityLanes_Internal(
            -1.0f, -1.0f, -1.0f,
            unresolvedBlend, 0.0f, 0.0f);
    const f32 unresolvedMean =
        ResolveVolumetricCloudUnresolvedCoarseMeanDensity_Internal(
            0.0f, 0.0f);
    for (u32 lane = 0u; lane < 4u; ++lane)
        EXPECT_NEAR(
            unresolvedMeanLanes.densities[lane],
            unresolvedMean, 1.0e-7f);

    FVolumetricCloudDensityLanesInternal physicalSamples{};
    physicalSamples.densities[0] = 0.0f;
    physicalSamples.densities[1] = 0.0f;
    physicalSamples.densities[2] =
        kVolumetricCloudUnresolvedCoarseInnerPotential;
    physicalSamples.densities[3] =
        kVolumetricCloudUnresolvedCoarseOuterPotential;
    const auto physicalLanes =
        ResolveVolumetricCloudPhysicalSubrayDensityLanes_Internal(
            unresolvedMean, physicalSamples, 1.0f);
    for (u32 lane = 0u; lane < 4u; ++lane)
        EXPECT_NEAR(
            physicalLanes.densities[lane],
            physicalSamples.densities[lane], 0.0f);

    constexpr f32 opticalScale = 8.0f;
    FVolumetricCloudLaneOpticalDepthInternal unsplitDepth{};
    AccumulateVolumetricCloudLaneOpticalDepth_Internal(
        unsplitDepth, physicalLanes, opticalScale);
    const f32 unsplitVisibility =
        ResolveVolumetricCloudLaneTransmittance_Internal(unsplitDepth);
    const f32 expectedAreaVisibility = 0.25f * (
        2.0f + std::exp(
            -kVolumetricCloudUnresolvedCoarseInnerPotential * opticalScale) +
        std::exp(
            -kVolumetricCloudUnresolvedCoarseOuterPotential * opticalScale));
    EXPECT_NEAR(unsplitVisibility, expectedAreaVisibility, 1.0e-6f);

    FVolumetricCloudLaneOpticalDepthInternal splitDepth{};
    AccumulateVolumetricCloudLaneOpticalDepth_Internal(
        splitDepth, physicalLanes, opticalScale * 0.5f);
    AccumulateVolumetricCloudLaneOpticalDepth_Internal(
        splitDepth, physicalLanes, opticalScale * 0.5f);
    EXPECT_NEAR(
        ResolveVolumetricCloudLaneTransmittance_Internal(splitDepth),
        unsplitVisibility, 1.0e-6f);

    FVolumetricCloudLaneOpticalDepthInternal halfDepth{};
    AccumulateVolumetricCloudLaneOpticalDepth_Internal(
        halfDepth, physicalLanes, opticalScale * 0.5f);
    const f32 halfVisibility =
        ResolveVolumetricCloudLaneTransmittance_Internal(halfDepth);
    // 区間ごとの面積平均を掛ける旧方式は、非一様レーンでは
    // E[X]^2 < E[X^2] となり、同じ経路を分割しただけで透過率を暗くする。
    EXPECT_TRUE(
        halfVisibility * halfVisibility < unsplitVisibility);

    // 実空間位置の密度は視線方向へ進むと変化する。同じ密度順位を固定して
    // 空レーンを永久に残さず、前半と後半で別の雲塊を通れば全レーンが暗くなる。
    FVolumetricCloudDensityLanesInternal complementarySamples{};
    complementarySamples.densities[0] = 1.0f;
    complementarySamples.densities[1] = 1.0f;
    complementarySamples.densities[2] = 0.0f;
    complementarySamples.densities[3] = 0.0f;
    constexpr f32 longPathOpticalScale = 80.0f;
    FVolumetricCloudLaneOpticalDepthInternal changingSpatialPath{};
    AccumulateVolumetricCloudLaneOpticalDepth_Internal(
        changingSpatialPath, physicalLanes, longPathOpticalScale);
    AccumulateVolumetricCloudLaneOpticalDepth_Internal(
        changingSpatialPath, complementarySamples, longPathOpticalScale);
    EXPECT_NEAR(
        ResolveVolumetricCloudLaneTransmittance_Internal(
            changingSpatialPath),
        0.25f * (
            2.0f * std::exp(-longPathOpticalScale) +
            std::exp(-kVolumetricCloudUnresolvedCoarseInnerPotential *
                     longPathOpticalScale) +
            std::exp(-kVolumetricCloudUnresolvedCoarseOuterPotential *
                     longPathOpticalScale)),
        1.0e-6f);
    EXPECT_TRUE(
        ResolveVolumetricCloudLaneTransmittance_Internal(
            changingSpatialPath) < 0.001f);

    const auto centralOnly =
        ResolveVolumetricCloudPhysicalSubrayDensityLanes_Internal(
            unresolvedMean, physicalSamples, 0.0f);
    for (u32 lane = 0u; lane < 4u; ++lane)
        EXPECT_NEAR(centralOnly.densities[lane], unresolvedMean, 1.0e-7f);

    constexpr f32 boundaryOffset =
        -kVolumetricCloudUnresolvedCoarseInnerPotential;
    constexpr f32 boundaryDelta = 1.0e-5f;
    const f32 belowBoundary =
        ResolveVolumetricCloudUnresolvedCoarseMeanDensity_Internal(
            boundaryOffset - boundaryDelta, 0.0f);
    const f32 aboveBoundary =
        ResolveVolumetricCloudUnresolvedCoarseMeanDensity_Internal(
            boundaryOffset + boundaryDelta, 0.0f);
    EXPECT_TRUE(aboveBoundary > belowBoundary);
    EXPECT_TRUE(
        std::fabs(aboveBoundary - belowBoundary) < 2.0e-5f);

    FVolumetricCloudDensityLanesInternal opaqueLanes{};
    for (u32 lane = 0u; lane < 4u; ++lane)
        opaqueLanes.densities[lane] = 1.0f;
    FVolumetricCloudLaneOpticalDepthInternal saturatedDepth{};
    AccumulateVolumetricCloudLaneOpticalDepth_Internal(
        saturatedDepth, opaqueLanes,
        std::numeric_limits<f32>::infinity());
    const f32 saturatedTransmittance =
        ResolveVolumetricCloudLaneTransmittance_Internal(saturatedDepth);
    EXPECT_TRUE(saturatedTransmittance > 0.0f);
    EXPECT_NEAR(
        saturatedTransmittance, std::exp(-80.0f), 1.0e-40f);

    FVolumetricCloudShapeFrequencyBlendInternal occupancyBlend{};
    occupancyBlend.complete = 0.75f;
    occupancyBlend.unresolved = 0.25f;
    const f32 unresolvedOccupancyUpper =
        ResolveVolumetricCloudOccupancyDensityUpperBound_Internal(
            -1.0f, occupancyBlend, 0.0f);
    EXPECT_NEAR(
        unresolvedOccupancyUpper,
        0.25f * kVolumetricCloudUnresolvedCoarseOuterPotential,
        1.0e-7f);
}

ACS_TEST(VolumetricClouds,
         UnresolvedDensityTransportPreservesFourStatesAndPathPartition) {
    using namespace render_internal;
    FVolumetricCloudShapeFrequencyBlendInternal unresolvedBlend{};
    unresolvedBlend.unresolved = 1.0f;
    const auto distribution =
        ResolveVolumetricCloudBandDensityDistribution_Internal(
            -1.0f, -1.0f, -1.0f,
            unresolvedBlend, 0.0f, 0.0f);
    EXPECT_NEAR(distribution.state_densities[0], 0.0f, 0.0f);
    EXPECT_NEAR(distribution.state_densities[1], 0.0f, 0.0f);
    EXPECT_NEAR(
        distribution.state_densities[2],
        kVolumetricCloudUnresolvedCoarseInnerPotential, 1.0e-7f);
    EXPECT_NEAR(
        distribution.state_densities[3],
        kVolumetricCloudUnresolvedCoarseOuterPotential, 1.0e-7f);

    // 焼き込み後の128角形状を軸ごとに周期自己相関し、正の相関部分を積分した値。
    // 周波数2のセル幅0.5を相関距離とみなさず、領域ゆがみ後の方向差を保持する。
    constexpr f32 shapeScale = 0.00010f;
    const f32 xCorrelationLength =
        ResolveVolumetricCloudShapeCorrelationLength_Internal(
            shapeScale, 1.0f, 0.0f, 0.0f);
    const f32 yCorrelationLength =
        ResolveVolumetricCloudShapeCorrelationLength_Internal(
            shapeScale, 0.0f, 1.0f, 0.0f);
    const f32 zCorrelationLength =
        ResolveVolumetricCloudShapeCorrelationLength_Internal(
            shapeScale, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(xCorrelationLength, 2515.07194f, 0.01f);
    EXPECT_NEAR(yCorrelationLength, 1881.47797f, 0.01f);
    EXPECT_NEAR(zCorrelationLength, 1978.09963f, 0.01f);
    EXPECT_TRUE(xCorrelationLength > yCorrelationLength * 1.30f);
    EXPECT_NEAR(
        ResolveVolumetricCloudShapeCorrelationLength_Internal(
            0.0f, 1.0f, 0.0f, 0.0f),
        0.0f, 0.0f);

    f32 distributionMean = 0.0f;
    constexpr f32 distributionWeights[4]{
        kVolumetricCloudUnresolvedCoarseOuterWeight,
        kVolumetricCloudUnresolvedCoarseInnerWeight,
        kVolumetricCloudUnresolvedCoarseInnerWeight,
        kVolumetricCloudUnresolvedCoarseOuterWeight};
    for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex) {
        distributionMean += distributionWeights[stateIndex] *
            distribution.state_densities[stateIndex];
    }

    // 相関積分距離の二倍を一セルとし、入口位相を半セルに置く。最初の
    // 一相関距離では四状態を保持するため、指数平均へ直接一致する。
    constexpr f32 correlationOpticalScale = 8.0f;
    const f32 extinctionPerDensity =
        correlationOpticalScale / xCorrelationLength;
    constexpr f32 firstFraction = 0.37f;
    f32 expectedPersistent = 0.0f;
    f32 expectedPersistentMoment = 0.0f;
    for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex) {
        const f32 opticalDepth = distribution.state_densities[stateIndex] *
            extinctionPerDensity * xCorrelationLength;
        const f32 stateTransmittance = std::exp(-opticalDepth);
        const f32 stateAbsorption = 1.0f - stateTransmittance;
        expectedPersistent +=
            distributionWeights[stateIndex] * stateTransmittance;
        expectedPersistentMoment += distributionWeights[stateIndex] *
            stateAbsorption * xCorrelationLength *
            ResolveVolumetricCloudBeerAbsorptionCentroidFromTransmittance_Internal(
                opticalDepth, stateTransmittance);
    }
    FVolumetricCloudFourStateTransportStateInternal wholeState{};
    const auto whole =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, extinctionPerDensity, xCorrelationLength,
            xCorrelationLength, wholeState);
    EXPECT_NEAR(whole.transmittance, expectedPersistent, 3.0e-6f);
    EXPECT_NEAR(
        whole.absorption_centroid_distance,
        expectedPersistentMoment / (1.0f - expectedPersistent), 0.01f);

    // 同じ光路を任意位置で分けても、条件付き四状態と相関位相を継承すれば
    // 透過率・吸収一次モーメントの双方が変わらない。
    FVolumetricCloudFourStateTransportStateInternal splitState{};
    const f32 firstLength = xCorrelationLength * firstFraction;
    const f32 secondLength = xCorrelationLength - firstLength;
    const auto first =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, extinctionPerDensity, xCorrelationLength,
            firstLength, splitState);
    const auto second =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, extinctionPerDensity, xCorrelationLength,
            secondLength, splitState);
    EXPECT_NEAR(
        first.transmittance * second.transmittance,
        whole.transmittance, 4.0e-6f);
    const f32 splitAbsorptionMoment =
        (1.0f - first.transmittance) *
            first.absorption_centroid_distance +
        first.transmittance * (1.0f - second.transmittance) *
            (firstLength + second.absorption_centroid_distance);
    EXPECT_NEAR(
        splitAbsorptionMoment / (1.0f - whole.transmittance),
        whole.absorption_centroid_distance, 0.02f);

    FVolumetricCloudFourStateTransportStateInternal resetSecondState{};
    const auto incorrectlyResetSecond =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, extinctionPerDensity, xCorrelationLength,
            secondLength, resetSecondState);
    EXPECT_TRUE(std::fabs(
        first.transmittance * incorrectlyResetSecond.transmittance -
        whole.transmittance) > 1.0e-3f);

    // 相関セルを複数跨ぐ経路は、最初の半セルと完全セルの指数平均積へ一致する。
    FVolumetricCloudFourStateTransportStateInternal cellState{};
    const auto fiveCorrelationLengths =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, extinctionPerDensity, xCorrelationLength,
            5.0f * xCorrelationLength, cellState);
    f32 fullCellTransmittance = 0.0f;
    for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex) {
        fullCellTransmittance += distributionWeights[stateIndex] *
            std::exp(-distribution.state_densities[stateIndex] *
                extinctionPerDensity * 2.0f * xCorrelationLength);
    }
    EXPECT_NEAR(
        fiveCorrelationLengths.transmittance,
        expectedPersistent * fullCellTransmittance *
            fullCellTransmittance,
        5.0e-6f);
    EXPECT_NEAR(
        cellState.remaining_boundary_distance,
        0.0f, 0.0f);
    EXPECT_NEAR(
        cellState.correlation_cell_length,
        0.0f, 0.0f);
    EXPECT_TRUE(cellState.awaiting_next_cell_length);

    // 密度分布が途中で変わっても、同じ物理区間をさらに分割した結果は一致する。
    auto shiftedDistribution = distribution;
    for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex)
        shiftedDistribution.state_densities[stateIndex] +=
            0.05f * static_cast<f32>(stateIndex + 1u);
    FVolumetricCloudFourStateTransportStateInternal varyingStateA{};
    const auto varyingA0 =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, extinctionPerDensity, xCorrelationLength,
            0.4f * xCorrelationLength, varyingStateA);
    const auto varyingA1 =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            shiftedDistribution, extinctionPerDensity, xCorrelationLength,
            0.6f * xCorrelationLength, varyingStateA);
    FVolumetricCloudFourStateTransportStateInternal varyingStateB{};
    const auto varyingB0 =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, extinctionPerDensity, xCorrelationLength,
            0.15f * xCorrelationLength, varyingStateB);
    const auto varyingB1 =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, extinctionPerDensity, xCorrelationLength,
            0.25f * xCorrelationLength, varyingStateB);
    const auto varyingB2 =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            shiftedDistribution, extinctionPerDensity, xCorrelationLength,
            0.20f * xCorrelationLength, varyingStateB);
    const auto varyingB3 =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            shiftedDistribution, extinctionPerDensity, xCorrelationLength,
            0.40f * xCorrelationLength, varyingStateB);
    EXPECT_NEAR(
        varyingA0.transmittance * varyingA1.transmittance,
        varyingB0.transmittance * varyingB1.transmittance *
            varyingB2.transmittance * varyingB3.transmittance,
        7.0e-6f);

    // 長さ0の呼び出しは状態を初期化し直さず、次の正長区間へ同じ位相を渡す。
    const auto stateBeforeZeroLength = varyingStateB;
    const auto zeroLength =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            shiftedDistribution, extinctionPerDensity, xCorrelationLength,
            0.0f, varyingStateB);
    EXPECT_NEAR(zeroLength.transmittance, 1.0f, 0.0f);
    EXPECT_NEAR(zeroLength.absorption_centroid_distance, 0.0f, 0.0f);
    EXPECT_EQ(varyingStateB.initialized, stateBeforeZeroLength.initialized);
    EXPECT_NEAR(
        varyingStateB.remaining_boundary_distance,
        stateBeforeZeroLength.remaining_boundary_distance, 0.0f);
    EXPECT_NEAR(
        varyingStateB.correlation_cell_length,
        stateBeforeZeroLength.correlation_cell_length, 0.0f);
    for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex)
        EXPECT_NEAR(
            varyingStateB.conditional_survival[stateIndex],
            stateBeforeZeroLength.conditional_survival[stateIndex], 0.0f);

    // 相関を失う極限は平均密度の均質Beerへ戻る。
    FVolumetricCloudFourStateTransportStateInternal mixedState{};
    const auto mixedLimit =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, extinctionPerDensity, 0.0f,
            xCorrelationLength, mixedState);
    EXPECT_NEAR(
        mixedLimit.transmittance,
        std::exp(-distributionMean * extinctionPerDensity *
            xCorrelationLength),
        3.0e-6f);

    // 散乱次数ごとに有限相関式を解き直し、高次深さを一次深さの固定倍率にしない。
    FVolumetricCloudFourStateTransportStateInternal halfOrderState{};
    const auto halfOrder =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, extinctionPerDensity * 0.5f,
            xCorrelationLength, xCorrelationLength, halfOrderState);
    const f32 primaryOpticalDepth = -std::log(whole.transmittance);
    const f32 halfOrderOpticalDepth = -std::log(halfOrder.transmittance);
    EXPECT_TRUE(halfOrderOpticalDepth > primaryOpticalDepth * 0.5f);
    EXPECT_TRUE(halfOrderOpticalDepth < primaryOpticalDepth);
    constexpr f32 higherOrderContribution = 0.70f;
    const f32 resolvedHalfOrderWeight =
        ResolveVolumetricCloudReducedIntervalScatteringWeightFromDepths_Internal(
            primaryOpticalDepth, halfOrderOpticalDepth,
            halfOrder.transmittance, higherOrderContribution);
    EXPECT_NEAR(
        resolvedHalfOrderWeight,
        higherOrderContribution * primaryOpticalDepth *
            (1.0f - halfOrder.transmittance) /
            halfOrderOpticalDepth,
        3.0e-6f);

    FVolumetricCloudDensityDistributionInternal uniformDistribution{};
    for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex)
        uniformDistribution.state_densities[stateIndex] = 0.30f;
    FVolumetricCloudFourStateTransportStateInternal uniformState{};
    const auto uniformTransport =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            uniformDistribution, extinctionPerDensity,
            xCorrelationLength, xCorrelationLength, uniformState);
    const f32 uniformDepth =
        0.30f * extinctionPerDensity * xCorrelationLength;
    EXPECT_NEAR(
        uniformTransport.transmittance,
        std::exp(-uniformDepth), 2.0e-6f);
    EXPECT_NEAR(
        uniformTransport.absorption_centroid_distance /
            xCorrelationLength,
        ResolveVolumetricCloudBeerAbsorptionCentroid_Internal(uniformDepth),
        2.0e-6f);

    // 全状態が濃い場合も透過率を0へ早期飽和させず、有限値を保つ。
    FVolumetricCloudDensityDistributionInternal denseDistribution{};
    denseDistribution.state_densities[0] = 1.0f;
    denseDistribution.state_densities[1] = 2.0f;
    denseDistribution.state_densities[2] = 3.0f;
    denseDistribution.state_densities[3] = 4.0f;
    FVolumetricCloudFourStateTransportStateInternal denseState{};
    const auto denseTransport =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            denseDistribution, 0.004f, 5000.0f, 5000.0f, denseState);
    EXPECT_TRUE(std::isfinite(denseTransport.transmittance));
    EXPECT_TRUE(denseTransport.transmittance > 0.0f);
    EXPECT_TRUE(denseTransport.transmittance < 1.0e-8f);

    // 一セルの吸収が小さくても、非常に多い完全セルをまとめた吸収重心を
    // 透過率のアンダーフロー位置や区間入口へ潰さない。
    constexpr f32 longCorrelationLength = 1.0f;
    constexpr f32 longCellLength = 2.0f * longCorrelationLength;
    constexpr f32 longCellCount = 10000000.0f;
    constexpr f32 longSegmentLength = longCellLength * longCellCount;
    constexpr f32 longCellOpticalDepth = 5.0e-5f;
    constexpr f32 longDensity = 0.30f;
    constexpr f32 longExtinction =
        longCellOpticalDepth / (longDensity * longCellLength);
    FVolumetricCloudDensityDistributionInternal longDistribution{};
    for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex)
        longDistribution.state_densities[stateIndex] = longDensity;
    FVolumetricCloudFourStateTransportStateInternal longState{};
    const auto longTransport =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            longDistribution, longExtinction, longCorrelationLength,
            longSegmentLength, longState);
    constexpr f32 longTotalOpticalDepth =
        longDensity * longExtinction * longSegmentLength;
    const f32 expectedLongCentroid = longSegmentLength *
        ResolveVolumetricCloudBeerAbsorptionCentroidFromTransmittance_Internal(
            longTotalOpticalDepth, 0.0f);
    EXPECT_NEAR(longTotalOpticalDepth, 500.0f, 1.0e-4f);
    EXPECT_NEAR(longTransport.transmittance, 0.0f, 0.0f);
    EXPECT_NEAR(
        longTransport.absorption_centroid_distance,
        expectedLongCentroid, 24.0f);

    // 一セルの深さがfloatの1との差より小さくても、直接保持した吸収率は消えない。
    constexpr f32 extremeThinCellOpticalDepth = 1.0e-8f;
    constexpr f32 extremeThinExtinction =
        extremeThinCellOpticalDepth / (longDensity * longCellLength);
    FVolumetricCloudFourStateTransportStateInternal extremeThinCellState{};
    const auto extremeThinCellTransport =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            longDistribution, extremeThinExtinction,
            longCorrelationLength, longCellLength, extremeThinCellState);
    EXPECT_NEAR(
        extremeThinCellTransport.absorption,
        ResolveVolumetricCloudBeerAbsorptionFraction_Internal(
            extremeThinCellOpticalDepth),
        1.0e-11f);
    EXPECT_NEAR(
        extremeThinCellTransport.absorption_centroid_distance,
        longCellLength *
            ResolveVolumetricCloudBeerAbsorptionCentroid_Internal(
                extremeThinCellOpticalDepth),
        1.0e-5f);

    // 同じ極薄セルを一千万個まとめても、全体深さ0.1の吸収へ復元できる。
    FVolumetricCloudFourStateTransportStateInternal extremeThinLongState{};
    const auto extremeThinLongTransport =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            longDistribution, extremeThinExtinction,
            longCorrelationLength, longSegmentLength,
            extremeThinLongState);
    constexpr f32 extremeThinTotalOpticalDepth =
        extremeThinCellOpticalDepth * longCellCount;
    const f32 expectedExtremeThinAbsorption =
        ResolveVolumetricCloudBeerAbsorptionFraction_Internal(
            extremeThinTotalOpticalDepth);
    EXPECT_NEAR(extremeThinTotalOpticalDepth, 0.1f, 1.0e-7f);
    EXPECT_NEAR(
        extremeThinLongTransport.absorption,
        expectedExtremeThinAbsorption, 2.0e-6f);
    EXPECT_NEAR(
        extremeThinLongTransport.transmittance,
        std::exp(-extremeThinTotalOpticalDepth), 2.0e-6f);

    // 正の相関長へ単位依存の下限を設けない。ちょうど1e-6 mでも四状態を
    // 一相関セル内で保持し、平均密度へ先に潰したBeer値とは区別する。
    FVolumetricCloudDensityDistributionInternal boundaryDistribution{};
    boundaryDistribution.state_densities[0]=0.0f;
    boundaryDistribution.state_densities[1]=0.0f;
    boundaryDistribution.state_densities[2]=2.0f;
    boundaryDistribution.state_densities[3]=2.0f;
    constexpr f32 boundaryCorrelationLength=1.0e-6f;
    constexpr f32 boundaryExtinction=1.0e6f;
    FVolumetricCloudFourStateTransportStateInternal boundaryState{};
    const auto boundaryTransport=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            boundaryDistribution,boundaryExtinction,
            boundaryCorrelationLength,boundaryCorrelationLength,
            boundaryState);
    const f32 expectedBoundaryTransmittance=
        0.5f*(1.0f+std::exp(-2.0f));
    const f32 homogeneousBoundaryTransmittance=std::exp(-1.0f);
    EXPECT_NEAR(
        boundaryTransport.transmittance,
        expectedBoundaryTransmittance,1.0e-6f);
    EXPECT_TRUE(std::fabs(
        boundaryTransport.transmittance-
        homogeneousBoundaryTransmittance)>0.19f);

    // 負またはセル長を越えた残距離は、現在の物理セル長の半分へ戻す。
    // CPUとGPUで同じ不正状態回復を行い、境界値だけで密度状態を変えない。
    FVolumetricCloudFourStateTransportStateInternal negativeDistanceState{};
    FVolumetricCloudFourStateTransportStateInternal initialDistanceState{};
    ResetVolumetricCloudFourStateTransport_Internal(negativeDistanceState);
    ResetVolumetricCloudFourStateTransport_Internal(initialDistanceState);
    negativeDistanceState.remaining_boundary_distance=-0.25f;
    negativeDistanceState.correlation_cell_length=
        2.0f*boundaryCorrelationLength;
    const auto negativeDistanceTransport=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            boundaryDistribution,boundaryExtinction,
            boundaryCorrelationLength,0.25e-6f,negativeDistanceState);
    const auto initialDistanceTransport=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            boundaryDistribution,boundaryExtinction,
            boundaryCorrelationLength,0.25e-6f,initialDistanceState);
    EXPECT_NEAR(
        negativeDistanceTransport.transmittance,
        initialDistanceTransport.transmittance,0.0f);
    EXPECT_NEAR(
        negativeDistanceState.remaining_boundary_distance,
        initialDistanceState.remaining_boundary_distance,0.0f);
    EXPECT_NEAR(
        negativeDistanceState.correlation_cell_length,
        initialDistanceState.correlation_cell_length,0.0f);

    FVolumetricCloudFourStateTransportStateInternal oversizedDistanceState{};
    FVolumetricCloudFourStateTransportStateInternal freshDistanceState{};
    ResetVolumetricCloudFourStateTransport_Internal(oversizedDistanceState);
    ResetVolumetricCloudFourStateTransport_Internal(freshDistanceState);
    oversizedDistanceState.correlation_cell_length=
        2.0f*boundaryCorrelationLength;
    oversizedDistanceState.remaining_boundary_distance=
        3.0f*boundaryCorrelationLength;
    const auto oversizedDistanceTransport=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            boundaryDistribution,boundaryExtinction,
            boundaryCorrelationLength,0.25e-6f,oversizedDistanceState);
    const auto freshDistanceTransport=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            boundaryDistribution,boundaryExtinction,
            boundaryCorrelationLength,0.25e-6f,freshDistanceState);
    EXPECT_NEAR(
        oversizedDistanceTransport.transmittance,
        freshDistanceTransport.transmittance,0.0f);
    EXPECT_NEAR(
        oversizedDistanceState.remaining_boundary_distance,
        freshDistanceState.remaining_boundary_distance,0.0f);

    // 正の極短区間と微小消散を無効扱いしない。
    FVolumetricCloudDensityDistributionInternal tinyPositiveDistribution{};
    for(u32 stateIndex=0u;stateIndex<4u;++stateIndex)
        tinyPositiveDistribution.state_densities[stateIndex]=1.0f;
    FVolumetricCloudFourStateTransportStateInternal tinyPositiveState{};
    const auto tinyPositiveTransport=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            tinyPositiveDistribution,2.0e8f,0.0f,5.0e-9f,
            tinyPositiveState);
    EXPECT_NEAR(
        tinyPositiveTransport.absorption,
        1.0f-std::exp(-1.0f),1.0e-6f);

    // 境界まで旧許容差より短い距離が残っていても、実際に届くまでは同じ
    // 相関状態を保持する。末尾長は二進単精度で再現できる値を使う。
    constexpr f32 kRepresentableTinyTail =
        0.00000095367431640625f;
    constexpr f32 kBoundaryCellCorrelation = 1.0f;
    constexpr f32 kBoundaryCellLength = 2.0f;
    FVolumetricCloudFourStateTransportStateInternal nearBoundaryState{};
    ResetVolumetricCloudFourStateTransport_Internal(nearBoundaryState);
    nearBoundaryState.correlation_cell_length = kBoundaryCellLength;
    nearBoundaryState.remaining_boundary_distance = kBoundaryCellLength;
    const auto nearBoundaryPrefix =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            boundaryDistribution, boundaryExtinction,
            kBoundaryCellCorrelation,
            kBoundaryCellLength - kRepresentableTinyTail,
            nearBoundaryState);
    EXPECT_NEAR(
        nearBoundaryState.remaining_boundary_distance,
        kRepresentableTinyTail,0.0f);
    EXPECT_NEAR(
        nearBoundaryState.correlation_cell_length,
        kBoundaryCellLength,0.0f);
    const auto nearBoundaryTail =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            boundaryDistribution, boundaryExtinction,
            kBoundaryCellCorrelation, kRepresentableTinyTail,
            nearBoundaryState);
    FVolumetricCloudFourStateTransportStateInternal wholeBoundaryState{};
    ResetVolumetricCloudFourStateTransport_Internal(wholeBoundaryState);
    wholeBoundaryState.correlation_cell_length = kBoundaryCellLength;
    wholeBoundaryState.remaining_boundary_distance = kBoundaryCellLength;
    const auto wholeBoundaryCell =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            boundaryDistribution, boundaryExtinction,
            kBoundaryCellCorrelation, kBoundaryCellLength,
            wholeBoundaryState);
    EXPECT_NEAR(
        nearBoundaryPrefix.transmittance * nearBoundaryTail.transmittance,
        wholeBoundaryCell.transmittance, 2.0e-6f);
    EXPECT_NEAR(
        nearBoundaryState.remaining_boundary_distance,
        0.0f,0.0f);
    EXPECT_NEAR(
        nearBoundaryState.correlation_cell_length,
        0.0f,0.0f);
    EXPECT_TRUE(nearBoundaryState.awaiting_next_cell_length);
    EXPECT_TRUE(wholeBoundaryState.awaiting_next_cell_length);

    // 境界を越えた直後の正の微小末尾も積分し、位相を進める。一括輸送と
    // 境界分割輸送が一致すれば、歩進セルの分割位置で雲状態が飛ばない。
    FVolumetricCloudFourStateTransportStateInternal crossedTailState{};
    const auto crossedWithTail =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            boundaryDistribution, boundaryExtinction,
            kBoundaryCellCorrelation,
            kBoundaryCellCorrelation + kRepresentableTinyTail,
            crossedTailState);
    FVolumetricCloudFourStateTransportStateInternal splitTailState{};
    const auto toBoundary =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            boundaryDistribution, boundaryExtinction,
            kBoundaryCellCorrelation, kBoundaryCellCorrelation,
            splitTailState);
    const auto afterBoundaryTail =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            boundaryDistribution, boundaryExtinction,
            kBoundaryCellCorrelation, kRepresentableTinyTail,
            splitTailState);
    EXPECT_NEAR(
        crossedWithTail.transmittance,
        toBoundary.transmittance * afterBoundaryTail.transmittance,
        2.0e-6f);
    EXPECT_NEAR(
        crossedTailState.remaining_boundary_distance,
        splitTailState.remaining_boundary_distance, 0.0f);
    EXPECT_NEAR(
        crossedTailState.correlation_cell_length,
        splitTailState.correlation_cell_length, 0.0f);
    EXPECT_TRUE(
        crossedTailState.remaining_boundary_distance > 0.0f);
    EXPECT_TRUE(
        crossedTailState.remaining_boundary_distance < kBoundaryCellLength);

    // 3 mセルの境界直前を単精度の隣接値で分けても、残距離を物理単位で
    // 減算すれば一括輸送と同じ境界へ到達する。位相率へ戻す旧方式は
    // tailを短く復元し、次セルへ余分な距離を漏らしていた。
    constexpr f32 kThreeMetreCellLength=3.0f;
    constexpr f32 kThreeMetreCorrelationLength=1.5f;
    constexpr f32 kThreeMetrePrefix=2.999999761581421f;
    constexpr f32 kThreeMetreTail=
        0.0000002384185791015625f;
    constexpr f32 kThreeMetreExtinction=0.2f;
    FVolumetricCloudFourStateTransportStateInternal threeMetreWholeState{};
    ResetVolumetricCloudFourStateTransport_Internal(threeMetreWholeState);
    threeMetreWholeState.correlation_cell_length=kThreeMetreCellLength;
    threeMetreWholeState.remaining_boundary_distance=kThreeMetreCellLength;
    const auto threeMetreWhole=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            boundaryDistribution,kThreeMetreExtinction,
            kThreeMetreCorrelationLength,kThreeMetreCellLength,
            threeMetreWholeState);
    FVolumetricCloudFourStateTransportStateInternal threeMetreSplitState{};
    ResetVolumetricCloudFourStateTransport_Internal(threeMetreSplitState);
    threeMetreSplitState.correlation_cell_length=kThreeMetreCellLength;
    threeMetreSplitState.remaining_boundary_distance=kThreeMetreCellLength;
    const auto threeMetreFirst=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            boundaryDistribution,kThreeMetreExtinction,
            kThreeMetreCorrelationLength,kThreeMetrePrefix,
            threeMetreSplitState);
    EXPECT_NEAR(
        threeMetreSplitState.remaining_boundary_distance,
        kThreeMetreTail,0.0f);
    const auto threeMetreSecond=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            boundaryDistribution,kThreeMetreExtinction,
            kThreeMetreCorrelationLength,kThreeMetreTail,
            threeMetreSplitState);
    const f32 threeMetreSplitTransmittance=
        threeMetreFirst.transmittance*threeMetreSecond.transmittance;
    const f32 threeMetreSplitAbsorption=
        threeMetreFirst.absorption+
        threeMetreFirst.transmittance*threeMetreSecond.absorption;
    const f32 threeMetreSplitMoment=
        threeMetreFirst.absorption*
            threeMetreFirst.absorption_centroid_distance+
        threeMetreFirst.transmittance*threeMetreSecond.absorption*
            (kThreeMetrePrefix+
             threeMetreSecond.absorption_centroid_distance);
    EXPECT_NEAR(
        threeMetreSplitTransmittance,
        threeMetreWhole.transmittance,2.0e-6f);
    EXPECT_NEAR(
        threeMetreSplitAbsorption,
        threeMetreWhole.absorption,2.0e-6f);
    EXPECT_NEAR(
        threeMetreSplitMoment/threeMetreSplitAbsorption,
        threeMetreWhole.absorption_centroid_distance,2.0e-6f);
    EXPECT_NEAR(
        threeMetreSplitState.remaining_boundary_distance,
        0.0f,0.0f);
    EXPECT_NEAR(
        threeMetreSplitState.correlation_cell_length,
        0.0f,0.0f);
    EXPECT_TRUE(threeMetreSplitState.awaiting_next_cell_length);
    EXPECT_TRUE(threeMetreWholeState.awaiting_next_cell_length);
    for(u32 stateIndex=0u;stateIndex<4u;++stateIndex)
        EXPECT_NEAR(
            threeMetreSplitState.conditional_survival[stateIndex],
            threeMetreWholeState.conditional_survival[stateIndex],0.0f);

    // 境界ちょうどで区間が終わった場合、次セル長は旧相関長で早期確定せず、
    // 次の正距離入力が持つ相関長から決める。境界直前で分けても同じ結果になる。
    FVolumetricCloudDensityDistributionInternal changingDistribution{};
    changingDistribution.state_densities[0]=0.0f;
    changingDistribution.state_densities[1]=0.0f;
    changingDistribution.state_densities[2]=1.0f;
    changingDistribution.state_densities[3]=1.0f;
    constexpr f32 kOldCorrelationLength=1.5f;
    constexpr f32 kOldCellLength=3.0f;
    constexpr f32 kNewCorrelationLength=5.0f;
    constexpr f32 kNewCellLength=10.0f;
    constexpr f32 kNewSegmentLength=4.0f;
    FVolumetricCloudFourStateTransportStateInternal exactChangeState{};
    ResetVolumetricCloudFourStateTransport_Internal(exactChangeState);
    exactChangeState.correlation_cell_length=kOldCellLength;
    exactChangeState.remaining_boundary_distance=kOldCellLength;
    const auto exactOldCell=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            changingDistribution,1.0f,kOldCorrelationLength,
            kOldCellLength,exactChangeState);
    EXPECT_TRUE(exactChangeState.awaiting_next_cell_length);
    EXPECT_NEAR(exactChangeState.remaining_boundary_distance,0.0f,0.0f);
    const auto exactNewSegment=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            changingDistribution,1.0f,kNewCorrelationLength,
            kNewSegmentLength,exactChangeState);
    EXPECT_NEAR(
        exactNewSegment.transmittance,
        0.5f*(1.0f+std::exp(-kNewSegmentLength)),1.0e-6f);
    EXPECT_FALSE(exactChangeState.awaiting_next_cell_length);
    EXPECT_NEAR(
        exactChangeState.correlation_cell_length,kNewCellLength,0.0f);
    EXPECT_NEAR(
        exactChangeState.remaining_boundary_distance,
        kNewCellLength-kNewSegmentLength,0.0f);

    FVolumetricCloudFourStateTransportStateInternal nearChangeState{};
    ResetVolumetricCloudFourStateTransport_Internal(nearChangeState);
    nearChangeState.correlation_cell_length=kOldCellLength;
    nearChangeState.remaining_boundary_distance=kOldCellLength;
    const auto nearOldPrefix=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            changingDistribution,1.0f,kOldCorrelationLength,
            kThreeMetrePrefix,nearChangeState);
    const auto nearCrossAndNew=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            changingDistribution,1.0f,kNewCorrelationLength,
            kThreeMetreTail+kNewSegmentLength,nearChangeState);
    EXPECT_NEAR(
        nearOldPrefix.transmittance*nearCrossAndNew.transmittance,
        exactOldCell.transmittance*exactNewSegment.transmittance,2.0e-6f);
    EXPECT_NEAR(
        nearChangeState.remaining_boundary_distance,
        exactChangeState.remaining_boundary_distance,0.0f);
    EXPECT_NEAR(
        nearChangeState.correlation_cell_length,
        exactChangeState.correlation_cell_length,0.0f);

    // 壊れた境界距離だけを修復するときは、既に観測した条件付き確率を
    // 定常分布へ戻さない。GPU側も新規進入の初期化とは別のmaskで処理する。
    FVolumetricCloudFourStateTransportStateInternal preservedState{};
    ResetVolumetricCloudFourStateTransport_Internal(preservedState);
    preservedState.conditional_survival[0]=1.0f;
    preservedState.conditional_survival[1]=0.0f;
    preservedState.conditional_survival[2]=0.0f;
    preservedState.conditional_survival[3]=0.0f;
    preservedState.remaining_boundary_distance=-1.0f;
    preservedState.correlation_cell_length=2.0f;
    const auto preservedTransport=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            changingDistribution,1.0f,1.0f,0.25f,preservedState);
    EXPECT_NEAR(preservedTransport.transmittance,1.0f,0.0f);
    EXPECT_NEAR(preservedState.conditional_survival[0],1.0f,0.0f);
    EXPECT_NEAR(preservedState.conditional_survival[1],0.0f,0.0f);
    EXPECT_NEAR(preservedState.conditional_survival[2],0.0f,0.0f);
    EXPECT_NEAR(preservedState.conditional_survival[3],0.0f,0.0f);

    const std::string transportShader=CompactShader(
        ExtractRawShader(ReadSkySource(),"const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        transportShader,
        "float4cloudPositiveMask4(float4requestedValues){"));
    EXPECT_FALSE(Contains(
        transportShader,
        "cloudFourStateTransportPhase("));
    EXPECT_FALSE(Contains(
        transportShader,"step(1e-8.xxxx,safeLengths)"));
    EXPECT_FALSE(Contains(
        transportShader,"step(1e-12,max(extinction,0.0))"));
    EXPECT_FALSE(Contains(
        transportShader,"1e-6.xxxx,safeCorrelations"));
    EXPECT_TRUE(Contains(
        transportShader,
        "float4boundaryDistances;float4cellLengths;"
        "float4boundaryPending;float4active;"));
    EXPECT_TRUE(Contains(
        transportShader,
        "float4distanceToBoundary="
        "cloudFiniteNonnegative4(state.boundaryDistances);"));
    EXPECT_TRUE(Contains(
        transportShader,
        "reachedBoundaryMask=heterogeneousMask*step("
        "distanceToBoundary,safeLengths);"));
    EXPECT_TRUE(Contains(
        transportShader,
        "advancedBoundaryDistances=max("
        "distanceToBoundary-firstLengths,0.0.xxxx);"));
    EXPECT_TRUE(Contains(
        transportShader,
        "tailMask=cloudPositiveMask4(tailLengths);"));
    EXPECT_TRUE(Contains(
        transportShader,
        "float4invalidBoundaryStateMask=heterogeneousMask"));
    EXPECT_TRUE(Contains(
        transportShader,
        "state.boundaryPending,1.0.xxxx,exactFirstBoundaryMask"));
    EXPECT_FALSE(Contains(
        transportShader,
        "cloudResetFourStateTransportLanes("
        "state,invalidBoundaryStateMask);"));
    EXPECT_FALSE(Contains(transportShader,"state.phases"));
    EXPECT_FALSE(Contains(
        transportShader,"CLOUD_CORRELATION_PHASE_BEFORE_BOUNDARY"));
    EXPECT_FALSE(Contains(transportShader,"distanceTolerances"));
}

ACS_TEST(VolumetricClouds,
         FourStateTransportGpuMatchesCpuReferenceWhenAvailable) {
    using namespace render_internal;
    // 実際の製品シェーダー末尾へ検証入口だけを追加し、輸送関数本体の複製を避ける。
    std::string shaderSource =
        ExtractRawShader(ReadSkySource(), "const char* kCloudCS");
    EXPECT_FALSE(shaderSource.empty());
    if (shaderSource.empty()) return;
    const std::string compactShader = CompactShader(shaderSource);
    EXPECT_TRUE(Contains(
        compactShader,
        "step(opticalDepth,0.125.xxxx)"));
    EXPECT_TRUE(Contains(
        compactShader,
        "step(opticalDepth,1.0.xxxx)"));
    EXPECT_TRUE(Contains(
        compactShader,
        "step(absorption,0.125.xxxx)"));
    shaderSource += R"(
[numthreads(1,1,1)]
void CSCloudTransportProbe(uint3 threadId : SV_DispatchThreadID){
    if(any(threadId!=uint3(0,0,0))) return;

    float4 densityState0=0.0.xxxx;
    float4 densityState1=0.0.xxxx;
    float4 densityState2=1.0.xxxx;
    float4 densityState3=1.0.xxxx;
    float representableTail=0.0000002384185791015625;
    float representablePrefix=3.0-representableTail;

    // xは境界ちょうど、yは直前の隣接floatで分割し、次の入力で相関長を変える。
    CloudFourStateTransportLanes boundaryState=
        cloudInitialFourStateTransportLanes();
    boundaryState.boundaryDistances=3.0.xxxx;
    boundaryState.cellLengths=3.0.xxxx;
    boundaryState.active=1.0.xxxx;
    float4 firstLengths=float4(3.0,representablePrefix,0.0,0.0);
    CloudFourStateTransportResultLanes firstTransport=
        cloudFourStateTransportLanes(
            densityState0,densityState1,densityState2,densityState3,
            1.0,1.5.xxxx,firstLengths,boundaryState);
    // 二回目を進める前の離散境界状態を保存し、隣接floatの誤分岐を隠さない。
    cloudOut[uint2(25,0)]=boundaryState.boundaryDistances;
    cloudOut[uint2(26,0)]=boundaryState.cellLengths;
    cloudOut[uint2(27,0)]=boundaryState.boundaryPending;
    cloudOut[uint2(28,0)]=boundaryState.active;
    float4 secondLengths=float4(
        2.0,2.0+representableTail,0.0,0.0);
    CloudFourStateTransportResultLanes secondTransport=
        cloudFourStateTransportLanes(
            densityState0,densityState1,densityState2,densityState3,
            1.0,5.0.xxxx,secondLengths,boundaryState);
    float4 combinedTransmittances=
        firstTransport.transmittances*secondTransport.transmittances;
    float4 combinedAbsorptions=
        firstTransport.absorptions
        +firstTransport.transmittances*secondTransport.absorptions;
    float4 combinedMoments=
        firstTransport.absorptions*firstTransport.centroidDistances
        +firstTransport.transmittances*secondTransport.absorptions
            *(firstLengths+secondTransport.centroidDistances);
    float4 combinedCentroids=combinedMoments/
        max(combinedAbsorptions,1e-30.xxxx);
    cloudOut[uint2(0,0)]=combinedTransmittances;
    cloudOut[uint2(1,0)]=combinedAbsorptions;
    cloudOut[uint2(2,0)]=combinedCentroids;
    cloudOut[uint2(3,0)]=boundaryState.boundaryDistances;
    cloudOut[uint2(4,0)]=boundaryState.cellLengths;
    cloudOut[uint2(5,0)]=boundaryState.boundaryPending;
    cloudOut[uint2(6,0)]=boundaryState.active;
    cloudOut[uint2(7,0)]=boundaryState.state0;
    cloudOut[uint2(8,0)]=boundaryState.state1;
    cloudOut[uint2(9,0)]=boundaryState.state2;
    cloudOut[uint2(10,0)]=boundaryState.state3;

    // 壊れた境界距離を修復しても、観測済みの条件付き確率を保持する。
    CloudFourStateTransportLanes invalidState=
        cloudInitialFourStateTransportLanes();
    invalidState.state0=1.0.xxxx;
    invalidState.state1=0.0.xxxx;
    invalidState.state2=0.0.xxxx;
    invalidState.state3=0.0.xxxx;
    invalidState.boundaryDistances=(-1.0).xxxx;
    invalidState.cellLengths=2.0.xxxx;
    invalidState.active=1.0.xxxx;
    CloudFourStateTransportResultLanes invalidTransport=
        cloudFourStateTransportLanes(
            densityState0,densityState1,densityState2,densityState3,
            1.0,1.0.xxxx,0.25.xxxx,invalidState);
    cloudOut[uint2(11,0)]=float4(
        invalidTransport.transmittances.x,
        invalidTransport.absorptions.x,
        invalidTransport.centroidDistances.x,
        invalidState.boundaryDistances.x);
    cloudOut[uint2(12,0)]=float4(
        invalidState.cellLengths.x,invalidState.boundaryPending.x,
        invalidState.active.x,0.0);
    cloudOut[uint2(13,0)]=float4(
        invalidState.state0.x,invalidState.state1.x,
        invalidState.state2.x,invalidState.state3.x);

    // 級数と通常式の境界値を、コンパイラ後の実比較結果として保存する。
    float4 thresholdDepths=float4(
        0.125,0.1250000149011612,1.0,1.0000001192092896);
    cloudOut[uint2(14,0)]=
        cloudStrictGreaterMask4(thresholdDepths,0.125);
    cloudOut[uint2(15,0)]=
        cloudStrictGreaterMask4(thresholdDepths,1.0);
    // 製品の級数選択に使うstep(edge,value)も、等値を含む実GPU結果で固定する。
    cloudOut[uint2(29,0)]=step(thresholdDepths,0.125.xxxx);
    cloudOut[uint2(30,0)]=step(thresholdDepths,1.0.xxxx);
    CloudFourStateTransportLanes thresholdState=
        cloudInitialFourStateTransportLanes();
    CloudFourStateTransportResultLanes thresholdTransport=
        cloudFourStateTransportLanes(
            thresholdDepths,thresholdDepths,
            thresholdDepths,thresholdDepths,
            1.0,0.0.xxxx,1.0.xxxx,thresholdState);
    cloudOut[uint2(16,0)]=thresholdTransport.transmittances;
    cloudOut[uint2(17,0)]=thresholdTransport.absorptions;
    cloudOut[uint2(18,0)]=thresholdTransport.centroidDistances;

    // 一括区間と二分区間を別状態で進め、輸送量と最終相関状態を保存する。
    CloudFourStateTransportLanes wholeState=
        cloudInitialFourStateTransportLanes();
    CloudFourStateTransportResultLanes wholeTransport=
        cloudFourStateTransportLanes(
            densityState0,densityState1,densityState2,densityState3,
            0.7,1.5.xxxx,7.25.xxxx,wholeState);
    CloudFourStateTransportLanes splitState=
        cloudInitialFourStateTransportLanes();
    CloudFourStateTransportResultLanes splitFirst=
        cloudFourStateTransportLanes(
            densityState0,densityState1,densityState2,densityState3,
            0.7,1.5.xxxx,2.25.xxxx,splitState);
    CloudFourStateTransportResultLanes splitSecond=
        cloudFourStateTransportLanes(
            densityState0,densityState1,densityState2,densityState3,
            0.7,1.5.xxxx,5.0.xxxx,splitState);
    float splitTransmittance=
        splitFirst.transmittances.x*splitSecond.transmittances.x;
    float splitAbsorption=splitFirst.absorptions.x
        +splitFirst.transmittances.x*splitSecond.absorptions.x;
    float splitMoment=
        splitFirst.absorptions.x*splitFirst.centroidDistances.x
        +splitFirst.transmittances.x*splitSecond.absorptions.x
            *(2.25+splitSecond.centroidDistances.x);
    cloudOut[uint2(19,0)]=float4(
        wholeTransport.transmittances.x,
        wholeTransport.absorptions.x,
        wholeTransport.centroidDistances.x,0.0);
    cloudOut[uint2(20,0)]=float4(
        splitTransmittance,splitAbsorption,
        splitMoment/max(splitAbsorption,1e-30),0.0);
    cloudOut[uint2(21,0)]=float4(
        wholeState.boundaryDistances.x,wholeState.cellLengths.x,
        wholeState.boundaryPending.x,wholeState.active.x);
    cloudOut[uint2(22,0)]=float4(
        splitState.boundaryDistances.x,splitState.cellLengths.x,
        splitState.boundaryPending.x,splitState.active.x);
    cloudOut[uint2(23,0)]=float4(
        wholeState.state0.x,wholeState.state1.x,
        wholeState.state2.x,wholeState.state3.x);
    cloudOut[uint2(24,0)]=float4(
        splitState.state0.x,splitState.state1.x,
        splitState.state2.x,splitState.state3.x);
}
)";

    CCloudGpuTestMemoryScope memory;
    const bool memoryReady = memory.Init();
    EXPECT_TRUE(memoryReady);
    if (!memoryReady) return;

    FDeviceConfig deviceConfiguration{};
    auto deviceResult = CreateRhiDevice(deviceConfiguration);
    if (deviceResult.IsErr()) return;
    auto device = Move(deviceResult.Value());

    FShaderDesc shaderDescription{};
    shaderDescription.stage = EShaderStage::Compute;
    shaderDescription.hlsl_source = shaderSource.c_str();
    shaderDescription.entry_point = "CSCloudTransportProbe";
    shaderDescription.target = "cs_5_1";
    shaderDescription.debug_name = "CloudTransportProbe.CS";
    auto shaderResult = CreateRhiShader(*device, shaderDescription);
    EXPECT_TRUE(shaderResult.IsOk());
    if (shaderResult.IsErr()) return;

    FComputePipelineDesc pipelineDescription{};
    pipelineDescription.cs = shaderResult.Value().Get();
    pipelineDescription.uav_slots = 1u;
    pipelineDescription.uav_names[0] = "cloudOut";
    auto pipelineResult =
        CreateRhiComputePipeline(*device, pipelineDescription);
    EXPECT_TRUE(pipelineResult.IsOk());
    if (pipelineResult.IsErr()) return;

    constexpr u32 kProbeTexelCount = 31u;
    FTextureDesc textureDescription{};
    textureDescription.width = kProbeTexelCount;
    textureDescription.height = 1u;
    textureDescription.format = EFormat::R32G32B32A32_Float;
    textureDescription.is_uav = true;
    auto textureResult = CreateRhiTexture(*device, textureDescription);
    EXPECT_TRUE(textureResult.IsOk());
    if (textureResult.IsErr()) return;

    auto commandResult = CreateRhiCommandList(*device);
    EXPECT_TRUE(commandResult.IsOk());
    if (commandResult.IsErr()) return;
    auto command = Move(commandResult.Value());
    command->Begin();
    command->SetComputePipeline(*pipelineResult.Value());
    command->BindUav(0u, *textureResult.Value());
    command->Dispatch(1u, 1u, 1u);
    command->End();
    const bool submitted = command->Submit();
    EXPECT_TRUE(submitted);
    if (!submitted) return;
    device->WaitIdle();

    f32 gpuValues[kProbeTexelCount * 4u]{};
    EXPECT_TRUE(device->ReadTexture(
        *textureResult.Value(), gpuValues,
        static_cast<u32>(sizeof(gpuValues))));
    const auto gpuValue = [&gpuValues](u32 texel, u32 component) noexcept {
        return gpuValues[texel * 4u + component];
    };

    struct FCombinedTransportForTest {
        // 二区間を合わせた透過率。
        f32 transmittance = 1.0f;
        // 二区間を合わせた吸収率。
        f32 absorption = 0.0f;
        // 全区間始点から測った平均吸収距離。
        f32 centroid = 0.0f;
    };
    const auto combineTransport = [](
        const FVolumetricCloudFourStateTransportIntervalInternal& first,
        f32 firstLength,
        const FVolumetricCloudFourStateTransportIntervalInternal& second)
        noexcept {
        FCombinedTransportForTest combined{};
        combined.transmittance =
            first.transmittance * second.transmittance;
        combined.absorption = first.absorption +
            first.transmittance * second.absorption;
        const f32 moment =
            first.absorption * first.absorption_centroid_distance +
            first.transmittance * second.absorption *
                (firstLength + second.absorption_centroid_distance);
        combined.centroid = combined.absorption > 0.0f
            ? moment / combined.absorption : 0.0f;
        return combined;
    };

    FVolumetricCloudDensityDistributionInternal distribution{};
    distribution.state_densities[0] = 0.0f;
    distribution.state_densities[1] = 0.0f;
    distribution.state_densities[2] = 1.0f;
    distribution.state_densities[3] = 1.0f;
    constexpr f32 kOldCorrelationLength = 1.5f;
    constexpr f32 kOldCellLength = 3.0f;
    constexpr f32 kNewCorrelationLength = 5.0f;
    constexpr f32 kNewSegmentLength = 2.0f;
    constexpr f32 kRepresentableTail =
        0.0000002384185791015625f;
    constexpr f32 kRepresentablePrefix =
        kOldCellLength - kRepresentableTail;
    constexpr f32 kGpuAgreementEpsilon = 3.0e-5f;

    FVolumetricCloudFourStateTransportStateInternal exactState{};
    ResetVolumetricCloudFourStateTransport_Internal(exactState);
    exactState.remaining_boundary_distance = kOldCellLength;
    exactState.correlation_cell_length = kOldCellLength;
    const auto exactFirst =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, 1.0f, kOldCorrelationLength,
            kOldCellLength, exactState);
    const auto exactStateAfterFirst = exactState;
    const auto exactSecond =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, 1.0f, kNewCorrelationLength,
            kNewSegmentLength, exactState);
    const auto exactCombined =
        combineTransport(exactFirst, kOldCellLength, exactSecond);

    FVolumetricCloudFourStateTransportStateInternal nearState{};
    ResetVolumetricCloudFourStateTransport_Internal(nearState);
    nearState.remaining_boundary_distance = kOldCellLength;
    nearState.correlation_cell_length = kOldCellLength;
    const auto nearFirst =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, 1.0f, kOldCorrelationLength,
            kRepresentablePrefix, nearState);
    const auto nearStateAfterFirst = nearState;
    const auto nearSecond =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, 1.0f, kNewCorrelationLength,
            kNewSegmentLength + kRepresentableTail, nearState);
    const auto nearCombined =
        combineTransport(nearFirst, kRepresentablePrefix, nearSecond);

    EXPECT_NEAR(gpuValue(0u, 0u), exactCombined.transmittance,
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(1u, 0u), exactCombined.absorption,
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(2u, 0u), exactCombined.centroid,
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(0u, 1u), nearCombined.transmittance,
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(1u, 1u), nearCombined.absorption,
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(2u, 1u), nearCombined.centroid,
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(3u, 0u),
                exactState.remaining_boundary_distance, 1.0e-6f);
    EXPECT_NEAR(gpuValue(3u, 1u),
                nearState.remaining_boundary_distance, 1.0e-6f);
    EXPECT_NEAR(gpuValue(4u, 0u),
                exactState.correlation_cell_length, 1.0e-6f);
    EXPECT_NEAR(gpuValue(4u, 1u),
                nearState.correlation_cell_length, 1.0e-6f);
    EXPECT_NEAR(gpuValue(5u, 0u),
                exactState.awaiting_next_cell_length ? 1.0f : 0.0f, 0.0f);
    EXPECT_NEAR(gpuValue(5u, 1u),
                nearState.awaiting_next_cell_length ? 1.0f : 0.0f, 0.0f);
    EXPECT_NEAR(gpuValue(6u, 0u), 1.0f, 0.0f);
    EXPECT_NEAR(gpuValue(6u, 1u), 1.0f, 0.0f);
    EXPECT_NEAR(gpuValue(25u, 0u),
                exactStateAfterFirst.remaining_boundary_distance, 0.0f);
    EXPECT_NEAR(gpuValue(25u, 1u),
                nearStateAfterFirst.remaining_boundary_distance, 0.0f);
    EXPECT_NEAR(gpuValue(26u, 0u),
                exactStateAfterFirst.correlation_cell_length, 0.0f);
    EXPECT_NEAR(gpuValue(26u, 1u),
                nearStateAfterFirst.correlation_cell_length, 0.0f);
    EXPECT_NEAR(gpuValue(27u, 0u),
                exactStateAfterFirst.awaiting_next_cell_length ? 1.0f : 0.0f,
                0.0f);
    EXPECT_NEAR(gpuValue(27u, 1u),
                nearStateAfterFirst.awaiting_next_cell_length ? 1.0f : 0.0f,
                0.0f);
    EXPECT_NEAR(gpuValue(28u, 0u),
                exactStateAfterFirst.initialized ? 1.0f : 0.0f, 0.0f);
    EXPECT_NEAR(gpuValue(28u, 1u),
                nearStateAfterFirst.initialized ? 1.0f : 0.0f, 0.0f);
    EXPECT_NEAR(gpuValue(25u, 0u), 0.0f, 0.0f);
    EXPECT_NEAR(gpuValue(25u, 1u), kRepresentableTail, 0.0f);
    EXPECT_NEAR(gpuValue(27u, 0u), 1.0f, 0.0f);
    EXPECT_NEAR(gpuValue(27u, 1u), 0.0f, 0.0f);
    for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex) {
        EXPECT_NEAR(gpuValue(7u + stateIndex, 0u),
                    exactState.conditional_survival[stateIndex],
                    kGpuAgreementEpsilon);
        EXPECT_NEAR(gpuValue(7u + stateIndex, 1u),
                    nearState.conditional_survival[stateIndex],
                    kGpuAgreementEpsilon);
    }

    FVolumetricCloudFourStateTransportStateInternal invalidState{};
    ResetVolumetricCloudFourStateTransport_Internal(invalidState);
    invalidState.conditional_survival[0] = 1.0f;
    invalidState.conditional_survival[1] = 0.0f;
    invalidState.conditional_survival[2] = 0.0f;
    invalidState.conditional_survival[3] = 0.0f;
    invalidState.remaining_boundary_distance = -1.0f;
    invalidState.correlation_cell_length = 2.0f;
    const auto invalidTransport =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, 1.0f, 1.0f, 0.25f, invalidState);
    EXPECT_NEAR(gpuValue(11u, 0u), invalidTransport.transmittance,
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(11u, 1u), invalidTransport.absorption,
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(11u, 2u),
                invalidTransport.absorption_centroid_distance,
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(11u, 3u),
                invalidState.remaining_boundary_distance, 1.0e-6f);
    EXPECT_NEAR(gpuValue(12u, 0u),
                invalidState.correlation_cell_length, 1.0e-6f);
    EXPECT_NEAR(gpuValue(12u, 1u),
                invalidState.awaiting_next_cell_length ? 1.0f : 0.0f, 0.0f);
    EXPECT_NEAR(gpuValue(12u, 2u), 1.0f, 0.0f);
    for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex) {
        EXPECT_NEAR(gpuValue(13u, stateIndex),
                    invalidState.conditional_survival[stateIndex],
                    kGpuAgreementEpsilon);
    }

    const f32 thresholdDepths[4]{
        0.125f, 0.1250000149011612f,
        1.0f, 1.0000001192092896f};
    const f32 expectedThreshold125[4]{0.0f, 1.0f, 1.0f, 1.0f};
    const f32 expectedThreshold1[4]{0.0f, 0.0f, 0.0f, 1.0f};
    const f32 expectedSeriesThreshold125[4]{1.0f, 0.0f, 0.0f, 0.0f};
    const f32 expectedSeriesThreshold1[4]{1.0f, 1.0f, 1.0f, 0.0f};
    for (u32 lane = 0u; lane < 4u; ++lane) {
        EXPECT_NEAR(gpuValue(14u, lane), expectedThreshold125[lane], 0.0f);
        EXPECT_NEAR(gpuValue(15u, lane), expectedThreshold1[lane], 0.0f);
        EXPECT_NEAR(gpuValue(29u, lane),
                    expectedSeriesThreshold125[lane], 0.0f);
        EXPECT_NEAR(gpuValue(30u, lane),
                    expectedSeriesThreshold1[lane], 0.0f);
        FVolumetricCloudDensityDistributionInternal thresholdDistribution{};
        for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex)
            thresholdDistribution.state_densities[stateIndex] =
                thresholdDepths[lane];
        FVolumetricCloudFourStateTransportStateInternal thresholdState{};
        const auto thresholdTransport =
            ResolveVolumetricCloudFourStateTransportInterval_Internal(
                thresholdDistribution, 1.0f, 0.0f, 1.0f,
                thresholdState);
        EXPECT_NEAR(gpuValue(16u, lane),
                    thresholdTransport.transmittance,
                    kGpuAgreementEpsilon);
        EXPECT_NEAR(gpuValue(17u, lane),
                    thresholdTransport.absorption,
                    kGpuAgreementEpsilon);
        EXPECT_NEAR(gpuValue(18u, lane),
                    thresholdTransport.absorption_centroid_distance,
                    kGpuAgreementEpsilon);
    }

    FVolumetricCloudFourStateTransportStateInternal wholeState{};
    const auto wholeTransport =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, 0.7f, 1.5f, 7.25f, wholeState);
    FVolumetricCloudFourStateTransportStateInternal splitState{};
    const auto splitFirst =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, 0.7f, 1.5f, 2.25f, splitState);
    const auto splitSecond =
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            distribution, 0.7f, 1.5f, 5.0f, splitState);
    const auto splitCombined =
        combineTransport(splitFirst, 2.25f, splitSecond);
    EXPECT_NEAR(gpuValue(19u, 0u), wholeTransport.transmittance,
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(19u, 1u), wholeTransport.absorption,
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(19u, 2u),
                wholeTransport.absorption_centroid_distance,
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(20u, 0u), splitCombined.transmittance,
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(20u, 1u), splitCombined.absorption,
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(20u, 2u), splitCombined.centroid,
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(19u, 0u), gpuValue(20u, 0u),
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(19u, 1u), gpuValue(20u, 1u),
                kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(19u, 2u), gpuValue(20u, 2u),
                kGpuAgreementEpsilon);
    EXPECT_NEAR(wholeTransport.transmittance,
                splitCombined.transmittance, kGpuAgreementEpsilon);
    EXPECT_NEAR(wholeTransport.absorption,
                splitCombined.absorption, kGpuAgreementEpsilon);
    EXPECT_NEAR(wholeTransport.absorption_centroid_distance,
                splitCombined.centroid, kGpuAgreementEpsilon);
    EXPECT_NEAR(gpuValue(21u, 0u),
                wholeState.remaining_boundary_distance, 1.0e-6f);
    EXPECT_NEAR(gpuValue(21u, 1u),
                wholeState.correlation_cell_length, 1.0e-6f);
    EXPECT_NEAR(gpuValue(21u, 2u),
                wholeState.awaiting_next_cell_length ? 1.0f : 0.0f, 0.0f);
    EXPECT_NEAR(gpuValue(21u, 3u), 1.0f, 0.0f);
    EXPECT_NEAR(gpuValue(22u, 0u),
                splitState.remaining_boundary_distance, 1.0e-6f);
    EXPECT_NEAR(gpuValue(22u, 1u),
                splitState.correlation_cell_length, 1.0e-6f);
    EXPECT_NEAR(gpuValue(22u, 2u),
                splitState.awaiting_next_cell_length ? 1.0f : 0.0f, 0.0f);
    EXPECT_NEAR(gpuValue(22u, 3u), 1.0f, 0.0f);
    EXPECT_NEAR(gpuValue(21u, 0u), gpuValue(22u, 0u), 1.0e-6f);
    EXPECT_NEAR(gpuValue(21u, 1u), gpuValue(22u, 1u), 1.0e-6f);
    EXPECT_NEAR(gpuValue(21u, 2u), gpuValue(22u, 2u), 0.0f);
    EXPECT_NEAR(gpuValue(21u, 3u), gpuValue(22u, 3u), 0.0f);
    EXPECT_NEAR(wholeState.remaining_boundary_distance,
                splitState.remaining_boundary_distance, 1.0e-6f);
    EXPECT_NEAR(wholeState.correlation_cell_length,
                splitState.correlation_cell_length, 1.0e-6f);
    EXPECT_EQ(wholeState.awaiting_next_cell_length,
              splitState.awaiting_next_cell_length);
    EXPECT_EQ(wholeState.initialized, splitState.initialized);
    for (u32 stateIndex = 0u; stateIndex < 4u; ++stateIndex) {
        EXPECT_NEAR(gpuValue(23u, stateIndex),
                    wholeState.conditional_survival[stateIndex],
                    kGpuAgreementEpsilon);
        EXPECT_NEAR(gpuValue(24u, stateIndex),
                    splitState.conditional_survival[stateIndex],
                    kGpuAgreementEpsilon);
        EXPECT_NEAR(gpuValue(23u, stateIndex),
                    gpuValue(24u, stateIndex), kGpuAgreementEpsilon);
        EXPECT_NEAR(wholeState.conditional_survival[stateIndex],
                    splitState.conditional_survival[stateIndex],
                    kGpuAgreementEpsilon);
    }
}

ACS_TEST(VolumetricClouds,
         SignedCondensationFieldKeepsThreeDimensionalCoreAndFiniteBoundary) {
    using namespace render_internal;
    const f32 clearPotential =
        ResolveVolumetricCloudCondensationPotential_Internal(
            1.0f, 0.0f, 1.0f, 0.0f);
    EXPECT_TRUE(clearPotential <= 0.0f);
    EXPECT_NEAR(
        ResolveVolumetricCloudCondensationDensity_Internal(clearPotential),
        0.0f, 0.0f);

    const f32 humidOutside =
        ResolveVolumetricCloudCondensationPotential_Internal(
            -0.20f, 1.0f, 1.0f, 0.0f);
    const f32 humidLow =
        ResolveVolumetricCloudCondensationPotential_Internal(
            0.20f, 1.0f, 1.0f, 0.0f);
    const f32 humidHigh =
        ResolveVolumetricCloudCondensationPotential_Internal(
            0.60f, 1.0f, 1.0f, 0.0f);
    EXPECT_NEAR(
        ResolveVolumetricCloudCondensationDensity_Internal(humidOutside),
        0.0f, 0.0f);
    const f32 lowDensity =
        ResolveVolumetricCloudCondensationDensity_Internal(humidLow);
    const f32 highDensity =
        ResolveVolumetricCloudCondensationDensity_Internal(humidHigh);
    EXPECT_TRUE(lowDensity > 0.0f);
    EXPECT_TRUE(highDensity > lowDensity);
    EXPECT_NEAR(highDensity - lowDensity, 0.40f, 1.0e-6f);

    constexpr f32 transitionWidth =
        kVolumetricCloudCondensationTransitionWidth;
    EXPECT_NEAR(
        ResolveVolumetricCloudCondensationDensity_Internal(-transitionWidth),
        0.0f, 0.0f);
    EXPECT_NEAR(
        ResolveVolumetricCloudCondensationDensity_Internal(0.0f),
        0.0f, 0.0f);
    EXPECT_NEAR(
        ResolveVolumetricCloudCondensationDensity_Internal(
            transitionWidth * 0.5f),
        transitionWidth * 0.375f, 1.0e-7f);
    EXPECT_NEAR(
        ResolveVolumetricCloudCondensationDensity_Internal(transitionWidth),
        transitionWidth, 1.0e-7f);
    EXPECT_NEAR(
        ResolveVolumetricCloudCondensationDensity_Internal(
            std::numeric_limits<f32>::quiet_NaN()),
        0.0f, 0.0f);

    const std::string shader = CompactShader(
        ExtractRawShader(ReadSkySource(), "const char* kCloudCS"));
    EXPECT_TRUE(Contains(shader,
        "floatcloudCondensationPotential("
        "floatshapePotential,floatverticalProfile,floatweatherMask,"
        "floatheight,floattoweringStrength){"));
    EXPECT_TRUE(Contains(shader,
        "billowPotentials=cloudBillowPotentialStates(ndA,ndB,h);"));
    EXPECT_TRUE(Contains(shader,
        "erosionPotential=cloudErosionPotentialOffset(ndA,ndB,h);"));
    EXPECT_TRUE(Contains(shader,
        "cloudDetailFilteredCondensationDistribution("
        "macro.shapePotential,macro.shapeFrequencyWeights,"
        "commonPotential,billowPotentials,"
        "billowVisibility,middleBillowVisibility,"
        "erosionPotential,erosionVisibility),"
        "densityScale"));
    EXPECT_FALSE(Contains(shader, "cloudDimensionalProfile("));
    EXPECT_FALSE(Contains(shader, "cloudColumnHeightAndSpan("));
}

ACS_TEST(VolumetricClouds,
         TransportTerminationBoundIncludesMaximumCondensationDensity) {
    using namespace render_internal;
    constexpr f32 kDensityScale = 0.72f;
    constexpr f32 kMaximumHeightPrecipitationScale = 1.10f * 1.28f;
    constexpr f32 kMaximumLayerScale = 1.35f;
    constexpr f32 kExtinction = 0.0018f;
    const f32 maximumCondensationDensity =
        ResolveVolumetricCloudMaximumCondensationDensity_Internal();
    EXPECT_NEAR(
        maximumCondensationDensity,
        kVolumetricCloudCondensationMaximumPotential, 1.0e-7f);
    EXPECT_TRUE(maximumCondensationDensity > 1.0f);

    f32 oldOpticalDensityUpper =
        MultiplyVolumetricCloudPositiveUpper_Internal(
            kDensityScale, kMaximumHeightPrecipitationScale);
    oldOpticalDensityUpper =
        MultiplyVolumetricCloudPositiveUpper_Internal(
            oldOpticalDensityUpper, kMaximumLayerScale);
    oldOpticalDensityUpper =
        MultiplyVolumetricCloudPositiveUpper_Internal(
            oldOpticalDensityUpper, kExtinction);

    f32 correctedOpticalDensityUpper =
        MultiplyVolumetricCloudPositiveUpper_Internal(
            kDensityScale, maximumCondensationDensity);
    correctedOpticalDensityUpper =
        MultiplyVolumetricCloudPositiveUpper_Internal(
            correctedOpticalDensityUpper,
            kMaximumHeightPrecipitationScale);
    correctedOpticalDensityUpper =
        MultiplyVolumetricCloudPositiveUpper_Internal(
            correctedOpticalDensityUpper, kMaximumLayerScale);
    correctedOpticalDensityUpper =
        MultiplyVolumetricCloudPositiveUpper_Internal(
            correctedOpticalDensityUpper, kExtinction);

    const f64 exactMaximumOpticalDensity =
        static_cast<f64>(kDensityScale) *
        static_cast<f64>(maximumCondensationDensity) *
        static_cast<f64>(kMaximumHeightPrecipitationScale) *
        static_cast<f64>(kMaximumLayerScale) *
        static_cast<f64>(kExtinction);
    EXPECT_TRUE(
        static_cast<f64>(oldOpticalDensityUpper) <
        exactMaximumOpticalDensity);
    EXPECT_TRUE(
        static_cast<f64>(correctedOpticalDensityUpper) >=
        exactMaximumOpticalDensity);

    const std::string shader = CompactShader(
        ExtractRawShader(ReadSkySource(), "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader,
        "floatmaximumOpticalDensityPerMeter=cloudPositiveProductUpper("
        "max(density,0.0),cloudCondensationDensity("
        "1.0+CLOUD_CONDENSATION_MAXIMUM_POSITIVE_OFFSET));"));
}

ACS_TEST(VolumetricClouds,
         PeriodicShapeMaximumKeepsTinySupportAndAxisOrder) {
    f32 deterministicLine[kShapeFilterLineLengthForTest]{};
    f32 constantLine[kShapeFilterLineLengthForTest]{};
    f32 impulseLine[kShapeFilterLineLengthForTest]{};
    for (u32 index = 0u; index < kShapeFilterLineLengthForTest; ++index) {
        deterministicLine[index] = static_cast<f32>(
            (index * 37u + 11u) % 101u) / 100.0f;
        constantLine[index] = 0.37f;
    }
    impulseLine[0] = 1.0f;

    constexpr u32 widths[] = {4u, 16u, 64u};
    constexpr u32 coveredCounts[] = {7u, 19u, 67u};
    for (u32 widthIndex = 0u; widthIndex < 3u; ++widthIndex) {
        const u32 width = widths[widthIndex];
        u32 coveredImpulseCount = 0u;
        for (u32 center = 0u;
             center < kShapeFilterLineLengthForTest; ++center) {
            EXPECT_NEAR(
                CenteredPeriodicShapeMaximumSparseForTest(
                    deterministicLine, center, width),
                CenteredPeriodicShapeMaximumReferenceForTest(
                    deterministicLine, center, width),
                0.0f);
            EXPECT_NEAR(
                CenteredPeriodicShapeMaximumSparseForTest(
                    constantLine, center, width),
                0.37f, 0.0f);
            const f32 impulseMaximum =
                CenteredPeriodicShapeMaximumSparseForTest(
                    impulseLine, center, width);
            if (impulseMaximum > 0.0f) ++coveredImpulseCount;
        }
        EXPECT_EQ(coveredImpulseCount, coveredCounts[widthIndex]);
        EXPECT_NEAR(
            CenteredPeriodicShapeMaximumSparseForTest(
                impulseLine, 0u, width),
            1.0f, 0.0f);
    }

    // yzxの循環転置は三回で元のXYZ順へ戻る。
    u32 x = 7u;
    u32 y = 63u;
    u32 z = 127u;
    for (u32 pass = 0u; pass < 3u; ++pass) {
        const u32 nextX = y;
        const u32 nextY = z;
        const u32 nextZ = x;
        x = nextX;
        y = nextY;
        z = nextZ;
    }
    EXPECT_EQ(x, 7u);
    EXPECT_EQ(y, 63u);
    EXPECT_EQ(z, 127u);
}

ACS_TEST(VolumetricClouds,
         ViewRayGaussBeerIntegrationUsesAnalyticCellTransport) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader,
        "staticconstintCLOUD_DENSITY_GAUSS_SAMPLE_COUNT=4;"));
    EXPECT_TRUE(Contains(
        shader,
        "staticconstfloat4CLOUD_DENSITY_GAUSS_FRACTIONS=float4("
        "0.0694318442,0.3300094782,0.6699905218,0.9305681558);"));
    EXPECT_TRUE(Contains(
        shader,
        "staticconstfloat4CLOUD_DENSITY_GAUSS_WEIGHTS=float4("
        "0.1739274226,0.3260725774,0.3260725774,0.1739274226);"));
    EXPECT_TRUE(Contains(
        shader,
        "elseif(boundaryIndex==2)"
        "boundary=CLOUD_DENSITY_GAUSS_WEIGHTS.x;"));
    EXPECT_TRUE(Contains(
        shader,
        "elseif(boundaryIndex==6)"
        "boundary=1.0-CLOUD_DENSITY_GAUSS_WEIGHTS.w;"));
    EXPECT_FALSE(Contains(shader, "CLOUD_BEER_TRANSPORT_BOUNDARIES"));
    EXPECT_TRUE(Contains(
        shader,
        "staticconstfloatCLOUD_DENSITY_GAUSS_MAXIMUM_GAP_FRACTION="
        "0.3399810436;"));
    EXPECT_TRUE(Contains(
        shader,
        "floatphysicalDensityResolutionSpacing=cellLaneLength*"
        "CLOUD_DENSITY_GAUSS_MAXIMUM_GAP_FRACTION;"));
    EXPECT_NEAR(
        render_internal::kVolumetricCloudDensityGaussMaximumGapFraction,
        0.6699905218f - 0.3300094782f, 1.0e-7f);
    EXPECT_TRUE(Contains(
        shader,
        "[loop]for(intdensitySampleIndex=0;"
        "densitySampleIndex<CLOUD_DENSITY_GAUSS_SAMPLE_COUNT;"));
    EXPECT_TRUE(Contains(
        shader,
        "sampleCloudPhysicalLaneDensityLightingAtFraction("
        "densitySampleFraction,laneComponentStartT,laneComponentEndT,"));
    EXPECT_TRUE(Contains(
        shader,
        "staticconstintCLOUD_BEER_TRANSPORT_SEGMENT_COUNT=8;"));
    EXPECT_TRUE(Contains(
        shader,
        "lightingSource=cloudLightingSourceAtPoint("
        "currentP,currentMacro,lightingContext,lowLodDensity,rayDirection);"));
    EXPECT_TRUE(Contains(
        shader,
        "[loop]for(inttransportSegmentIndex=0;"
        "transportSegmentIndex<CLOUD_BEER_TRANSPORT_SEGMENT_COUNT;"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudFourStateTransportResultLanesfirstTransport="
        "cloudFourStateTransportLanes("));
    EXPECT_TRUE(Contains(
        shader,
        "floatviewOpticalDepth=cloudOpticalDepthFromAbsorption("
        "firstIntervalAbsorption);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatfirstRayCentroid=overlapStart+dot("
        "physicalLaneSelector,firstTransport.centroidDistances);"));
    EXPECT_TRUE(Contains(
        shader,
        "depthMoment+=firstSampleWeight*firstSampleT;"));
    EXPECT_FALSE(Contains(shader, "weightedSegmentLength"));
    EXPECT_FALSE(Contains(shader, "densityCentroidFraction"));
    EXPECT_FALSE(Contains(shader, "normalizedDensityDistanceMoment"));
    EXPECT_EQ(
        render_internal::kVolumetricCloudDensityGaussSampleCount,
        4u);
    EXPECT_FALSE(Contains(shader, "cloudRayIntervalPhase("));
    EXPECT_FALSE(Contains(shader, "floatjit="));
    EXPECT_FALSE(Contains(shader, "currentDensity>firstDensity"));
    EXPECT_FALSE(Contains(shader, "densitySampleCount="));
    const auto quadrature =
        render_internal::ResolveVolumetricCloudDensityQuadrature_Internal(
            1.0f);
    for (u32 index = 1u; index < quadrature.sample_count; ++index) {
        EXPECT_TRUE(
            quadrature.fractions[index - 1u] <
            quadrature.fractions[index]);
    }
}

ACS_TEST(VolumetricClouds, MarchPlanSupportsFlyThroughAndRejectsPlanetFacingGroundRay) {
    const FVolumetricCloudLayer layer{96.0f, 128.0f, 0.035f};
    const FVolumetricCloudMarchPlan insideDown =
        PlanVolumetricCloudRayMarch(FVec3{0.0f, 110.0f, 0.0f},
                                    FVec3{0.0f, -1.0f, 0.0f}, layer);
    EXPECT_TRUE(insideDown.hit);
    EXPECT_NEAR(insideDown.enter, 0.0f, 1e-4f);
    EXPECT_TRUE(insideDown.exit > 13.0f);
    EXPECT_TRUE(insideDown.exit < 15.0f);

    const FVolumetricCloudMarchPlan groundDown =
        PlanVolumetricCloudRayMarch(FVec3{0.0f, 8.0f, 0.0f},
                                    FVec3{0.0f, -1.0f, 0.0f}, layer);
    EXPECT_FALSE(groundDown.hit);
}

ACS_TEST(VolumetricClouds, CurvedShellBoundsTheGeometricHorizon) {
    const FVolumetricCloudLayer layer{96.0f, 128.0f, 0.035f};
    const FVolumetricCloudRayInterval vertical =
        IntersectVolumetricCloudShell(FVec3{0.0f, 8.0f, 0.0f},
                                      FVec3{0.0f, 1.0f, 0.0f}, layer);
    EXPECT_TRUE(vertical.hit);
    EXPECT_NEAR(vertical.enter, 88.0f, 1e-3f);
    EXPECT_NEAR(vertical.exit, 120.0f, 1e-3f);

    const FVolumetricCloudRayInterval horizon =
        IntersectVolumetricCloudShell(FVec3{0.0f, 8.0f, 0.0f},
                                      FVec3{1.0f, 0.0f, 0.0f}, layer);
    EXPECT_TRUE(horizon.hit);
    EXPECT_TRUE(horizon.enter > 1000.0f);
    EXPECT_TRUE(horizon.enter > 30000.0f);
    EXPECT_TRUE(horizon.exit < 50000.0f);
    EXPECT_TRUE(horizon.exit > horizon.enter);
}

ACS_TEST(VolumetricClouds,
         DisconnectedShellOverlapsKeepTheClearGapAndResetGpuState) {
    using namespace render_internal;
    FVolumetricCloudShellIntervalSetInternal intervals{};
    intervals.interval_count = 2u;
    intervals.intervals[0].hit = true;
    intervals.intervals[0].enter = 10.0f;
    intervals.intervals[0].exit = 20.0f;
    intervals.intervals[1].hit = true;
    intervals.intervals[1].enter = 80.0f;
    intervals.intervals[1].exit = 90.0f;

    const auto nearComponent =
        ResolveVolumetricCloudShellOverlapComponent_Internal(
            intervals, 0u, 0.0f, 100.0f);
    const auto farComponent =
        ResolveVolumetricCloudShellOverlapComponent_Internal(
            intervals, 1u, 0.0f, 100.0f);
    EXPECT_TRUE(nearComponent.hit);
    EXPECT_TRUE(farComponent.hit);
    EXPECT_NEAR(nearComponent.enter, 10.0f, 0.0f);
    EXPECT_NEAR(nearComponent.exit, 20.0f, 0.0f);
    EXPECT_NEAR(farComponent.enter, 80.0f, 0.0f);
    EXPECT_NEAR(farComponent.exit, 90.0f, 0.0f);
    EXPECT_NEAR(farComponent.enter - nearComponent.exit, 60.0f, 0.0f);

    const auto nearOnly =
        ResolveVolumetricCloudShellOverlapComponent_Internal(
            intervals, 0u, 50.0f, 100.0f);
    const auto farOnly =
        ResolveVolumetricCloudShellOverlapComponent_Internal(
            intervals, 1u, 0.0f, 50.0f);
    EXPECT_FALSE(nearOnly.hit);
    EXPECT_FALSE(farOnly.hit);

    // 支持上界が0の晴天域では相関状態を切る。近側で濃い二状態が消えた
    // 条件付き生存率を別の雲塊へ持ち越す反例と比較する。
    FVolumetricCloudDensityDistributionInternal gapDistribution{};
    gapDistribution.state_densities[0]=0.0f;
    gapDistribution.state_densities[1]=0.0f;
    gapDistribution.state_densities[2]=1.0f;
    gapDistribution.state_densities[3]=1.0f;
    FVolumetricCloudFourStateTransportStateInternal nearState{};
    const auto nearTransport=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            gapDistribution,20.0f,100.0f,1.0f,nearState);
    EXPECT_TRUE(nearTransport.transmittance>0.49f);
    EXPECT_TRUE(nearTransport.transmittance<0.51f);

    auto resetAtFarEntryState=nearState;
    auto staleAcrossGapState=nearState;
    ResetVolumetricCloudFourStateTransport_Internal(resetAtFarEntryState);
    const auto resetFarTransport=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            gapDistribution,1.0f,100.0f,1.0f,resetAtFarEntryState);
    const auto staleFarTransport=
        ResolveVolumetricCloudFourStateTransportInterval_Internal(
            gapDistribution,1.0f,100.0f,1.0f,staleAcrossGapState);
    EXPECT_NEAR(
        resetFarTransport.transmittance,
        0.5f*(1.0f+std::exp(-1.0f)),1.0e-6f);
    EXPECT_TRUE(staleFarTransport.transmittance>0.99f);
    EXPECT_TRUE(
        staleFarTransport.transmittance-
        resetFarTransport.transmittance>0.30f);

    const std::string shader = CompactShader(
        ExtractRawShader(ReadSkySource(), "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader,
        "float2cloudBandIntervalComponentOverlap("
        "float4intervals,intcomponentIndex,"));
    EXPECT_FALSE(Contains(shader, "cloudBandIntervalOverlapMoment("));
    EXPECT_TRUE(Contains(
        shader,
        "[loop]for(intshellComponentIndex=0;"
        "shellComponentIndex<CLOUD_SHELL_COMPONENT_COUNT;"
        "++shellComponentIndex){"));
    EXPECT_TRUE(Contains(
        shader,
        "float4componentEntryMasks="
        "cloudPhysicalSubraySecondComponentEntryMasks("));
    EXPECT_TRUE(Contains(
        shader,
        "firstOrderTransportState.active*="
        "1.0.xxxx-componentEntryMasks;"));
    const std::size_t emptySkipBegin=shader.find(
        "if(occupancyDensityUpperBound<=0.0){");
    const std::size_t emptySkipEnd=shader.find(
        "if(!nearDensity&&traversalCellCount>1){",emptySkipBegin);
    EXPECT_TRUE(emptySkipBegin!=std::string::npos);
    EXPECT_TRUE(emptySkipEnd!=std::string::npos);
    if(emptySkipBegin!=std::string::npos&&
       emptySkipEnd!=std::string::npos){
        const std::string emptySkip=shader.substr(
            emptySkipBegin,emptySkipEnd-emptySkipBegin);
        EXPECT_TRUE(Contains(
            emptySkip,
            "firstOrderTransportState.active=0.0.xxxx;"
            "secondOrderTransportState.active=0.0.xxxx;"
            "thirdOrderTransportState.active=0.0.xxxx;"));
        EXPECT_FALSE(Contains(
            emptySkip,"emptyComponentEntryMasks"));
        EXPECT_TRUE(Contains(emptySkip,"continue;}"));
    }
}

ACS_TEST(VolumetricClouds,
         PhysicalSubrayShellIntervalsPreserveTangentAndInCloudCoverage) {
    using render_internal::ResolveVolumetricCloudShellInterval_Internal;

    constexpr f32 pixelGaussAngle = 0.000279907f;
    const auto directionFromElevation = [](f32 elevation) noexcept {
        return FVec3{
            static_cast<f32>(std::cos(elevation)),
            static_cast<f32>(std::sin(elevation)),
            0.0f};
    };
    const auto containsDistance = [](
        const FVolumetricCloudRayInterval& interval,
        f32 distance) noexcept {
        return interval.hit &&
            distance >= interval.enter && distance <= interval.exit;
    };

    // 上空の雲頂接線では中心が38.6m外れても、画素下側の二レーンは実際に雲を通る。
    const FVolumetricCloudLayer tangentLayer{
        2600.0f, 4000.0f, 0.035f};
    const FVec3 aboveCloud{0.0f, 10000.0f, 0.0f};
    const f32 tangentRadius =
        kVolumetricCloudPlanetRadius + tangentLayer.top_height + 38.6f;
    const f32 cameraRadius =
        kVolumetricCloudPlanetRadius + aboveCloud.y;
    const f32 centerElevation = -static_cast<f32>(std::acos(
        tangentRadius / cameraRadius));
    const FVolumetricCloudRayInterval tangentCenter =
        ResolveVolumetricCloudShellInterval_Internal(
            aboveCloud, directionFromElevation(centerElevation),
            tangentLayer);
    const FVolumetricCloudRayInterval tangentLower =
        ResolveVolumetricCloudShellInterval_Internal(
            aboveCloud,
            directionFromElevation(centerElevation - pixelGaussAngle),
            tangentLayer);
    const FVolumetricCloudRayInterval tangentUpper =
        ResolveVolumetricCloudShellInterval_Internal(
            aboveCloud,
            directionFromElevation(centerElevation + pixelGaussAngle),
            tangentLayer);
    EXPECT_FALSE(tangentCenter.hit);
    EXPECT_TRUE(tangentLower.hit);
    EXPECT_FALSE(tangentUpper.hit);
    const f32 tangentProbe =
        0.5f * (tangentLower.enter + tangentLower.exit);
    const f32 tangentPhysicalCoverage = 0.25f *
        (2.0f * (containsDistance(tangentLower, tangentProbe) ? 1.0f : 0.0f) +
         2.0f * (containsDistance(tangentUpper, tangentProbe) ? 1.0f : 0.0f));
    EXPECT_NEAR(tangentPhysicalCoverage, 0.5f, 0.0f);

    // 雲層内の水平視線でも上下サブレイの出口は一致しない。中心出口で打ち切らず、
    // 最遠出口まで歩進しながら各レーン固有の区間マスクを保つ必要がある。
    const FVolumetricCloudLayer inCloudLayer{
        1000.0f, 4000.0f, 0.035f};
    const FVec3 insideCloud{0.0f, 2000.0f, 0.0f};
    const FVolumetricCloudRayInterval downwardLane =
        ResolveVolumetricCloudShellInterval_Internal(
            insideCloud, directionFromElevation(-pixelGaussAngle),
            inCloudLayer);
    const FVolumetricCloudRayInterval upwardLane =
        ResolveVolumetricCloudShellInterval_Internal(
            insideCloud, directionFromElevation(pixelGaussAngle),
            inCloudLayer);
    EXPECT_TRUE(downwardLane.hit);
    EXPECT_TRUE(upwardLane.hit);
    EXPECT_TRUE(downwardLane.exit > upwardLane.exit + 3000.0f);
    const f32 inCloudProbe = 0.5f *
        (downwardLane.exit + upwardLane.exit);
    const f32 inCloudPhysicalCoverage = 0.25f *
        (2.0f * (containsDistance(downwardLane, inCloudProbe) ? 1.0f : 0.0f) +
         2.0f * (containsDistance(upwardLane, inCloudProbe) ? 1.0f : 0.0f));
    EXPECT_NEAR(inCloudPhysicalCoverage, 0.5f, 0.0f);
}

ACS_TEST(VolumetricClouds,
         CurvedShellKeepsBothVisibleIntervalsAndClipsAtThePlanet) {
    using render_internal::ResolveVolumetricCloudShellInterval_Internal;
    using render_internal::ResolveVolumetricCloudShellIntervals_Internal;

    // 雲頂直上から雲底より100mだけ低い最近接点を通るレイは、近側と遠側の
    // 二区間を作る。最大距離内の遠側を、最近接区間だけの旧契約で失わない。
    const FVolumetricCloudLayer aboveLayer{
        1500.0f, 4000.0f, 0.035f};
    const FVec3 aboveOrigin{0.0f, 4100.0f, 0.0f};
    const FVec3 aboveDirection{
        0.999575745f, -0.029126099f, 0.0f};
    const auto aboveIntervals =
        ResolveVolumetricCloudShellIntervals_Internal(
            aboveOrigin, aboveDirection, aboveLayer, 250000.0f);
    EXPECT_EQ(aboveIntervals.interval_count, 2u);
    EXPECT_NEAR(aboveIntervals.intervals[0].enter, 3465.719f, 2.0f);
    EXPECT_NEAR(aboveIntervals.intervals[0].exit, 149692.229f, 3.0f);
    EXPECT_NEAR(aboveIntervals.intervals[1].enter, 221030.578f, 3.0f);
    EXPECT_NEAR(aboveIntervals.intervals[1].exit, 250000.0f, 0.01f);
    constexpr f64 planOriginRadius =
        static_cast<f64>(kVolumetricCloudPlanetRadius) + 5300.0;
    constexpr f64 planClosestRadius =
        static_cast<f64>(kVolumetricCloudPlanetRadius) + 1499.0;
    const f64 planDirectionY = -std::sqrt(
        planOriginRadius * planOriginRadius -
        planClosestRadius * planClosestRadius) / planOriginRadius;
    const FVec3 planDirection{
        static_cast<f32>(std::sqrt(
            1.0 - planDirectionY * planDirectionY)),
        static_cast<f32>(planDirectionY), 0.0f};
    const FVec3 planOrigin{0.0f, 5300.0f, 0.0f};
    const auto expectedPlanIntervals =
        ResolveVolumetricCloudShellIntervals_Internal(
            planOrigin, planDirection, aboveLayer, 250000.0f);
    EXPECT_EQ(expectedPlanIntervals.interval_count, 2u);
    FVolumetricCloudRange aboveRange{};
    aboveRange.MaxDistance = 250000.0f;
    aboveRange.StepGrowth = 0.0f;
    const FVolumetricCloudUpperLayer disabledUpper{};
    const auto abovePlan =
        render_internal::PlanVolumetricCloudRayMarch_Internal(
            planOrigin, planDirection, aboveLayer,
            disabledUpper, false, aboveRange,
            FVec3{}, kVolumetricCloudViewSteps);
    EXPECT_TRUE(abovePlan.hit);
    EXPECT_EQ(abovePlan.interval_count, 2u);
    EXPECT_EQ(abovePlan.intervals[0].physical_band_id, 0u);
    EXPECT_EQ(abovePlan.intervals[1].physical_band_id, 0u);
    EXPECT_NEAR(
        abovePlan.intervals[0].enter,
        expectedPlanIntervals.intervals[0].enter, 0.01f);
    EXPECT_NEAR(
        abovePlan.intervals[1].enter,
        expectedPlanIntervals.intervals[1].enter, 0.01f);
    EXPECT_TRUE(
        abovePlan.total_fine_cell_count <=
        kVolumetricCloudViewSteps);

    // 雲底から1mの雲中でも、最近接点が雲底を2mだけ下回ると短い晴天域を挟む。
    // 局所視程16kmに入る遠側区間まで保持する。
    constexpr f64 insideOriginRadius =
        static_cast<f64>(kVolumetricCloudPlanetRadius) + 1501.0;
    constexpr f64 insideClosestRadius =
        static_cast<f64>(kVolumetricCloudPlanetRadius) + 1499.0;
    const f64 insideDirectionY = -std::sqrt(
        insideOriginRadius * insideOriginRadius -
        insideClosestRadius * insideClosestRadius) / insideOriginRadius;
    const FVec3 insideDirection{
        static_cast<f32>(std::sqrt(
            1.0 - insideDirectionY * insideDirectionY)),
        static_cast<f32>(insideDirectionY), 0.0f};
    const auto insideIntervals =
        ResolveVolumetricCloudShellIntervals_Internal(
            FVec3{0.0f, 1501.0f, 0.0f},
            insideDirection, aboveLayer, 16000.0f);
    EXPECT_EQ(insideIntervals.interval_count, 2u);
    EXPECT_NEAR(insideIntervals.intervals[0].enter, 0.0f, 0.01f);
    EXPECT_TRUE(insideIntervals.intervals[0].exit > 1400.0f);
    EXPECT_TRUE(insideIntervals.intervals[0].exit < 1550.0f);
    EXPECT_TRUE(insideIntervals.intervals[1].enter > 8500.0f);
    EXPECT_TRUE(insideIntervals.intervals[1].enter < 8750.0f);
    EXPECT_NEAR(insideIntervals.intervals[1].exit, 16000.0f, 0.01f);

    // 惑星表面より1m内側を通るレイでは、遠側上層は幾何上の球殻交差であっても
    // 地面の裏にある。全レーン共通の物理終端として除外する。
    constexpr f64 groundOriginRadius =
        static_cast<f64>(kVolumetricCloudPlanetRadius) + 1000.0;
    constexpr f64 groundClosestRadius =
        static_cast<f64>(kVolumetricCloudPlanetRadius) - 1.0;
    const f64 groundDirectionY = -std::sqrt(
        groundOriginRadius * groundOriginRadius -
        groundClosestRadius * groundClosestRadius) / groundOriginRadius;
    const FVec3 groundDirection{
        static_cast<f32>(std::sqrt(
            1.0 - groundDirectionY * groundDirectionY)),
        static_cast<f32>(groundDirectionY), 0.0f};
    const FVolumetricCloudLayer hiddenUpperLayer{
        1100.0f, 1200.0f, 0.035f};
    const FVolumetricCloudRayInterval unoccludedUpper =
        ResolveVolumetricCloudShellInterval_Internal(
            FVec3{0.0f, 1000.0f, 0.0f},
            groundDirection, hiddenUpperLayer);
    const auto clippedUpper =
        ResolveVolumetricCloudShellIntervals_Internal(
            FVec3{0.0f, 1000.0f, 0.0f},
            groundDirection, hiddenUpperLayer, 250000.0f);
    EXPECT_TRUE(unoccludedUpper.hit);
    EXPECT_TRUE(unoccludedUpper.enter > 200000.0f);
    EXPECT_EQ(clippedUpper.interval_count, 0u);

    // Gauss点を境界が跨いでも担当幅を丸ごと有無へ切り替えず、実際の境界移動量だけ
    // 光路長を変える。旧二値maskでは同じ反例が画素平均で約8.15%跳躍していた。
    constexpr f32 gaussBoundary = 0.3300094782f;
    constexpr f32 boundaryDelta = 1.0e-6f;
    render_internal::FVolumetricCloudShellIntervalSetInternal before{};
    before.interval_count = 1u;
    before.intervals[0] = FVolumetricCloudRayInterval{
        0.0f, gaussBoundary - boundaryDelta, true};
    render_internal::FVolumetricCloudShellIntervalSetInternal after = before;
    after.intervals[0].exit = gaussBoundary + boundaryDelta;
    const f32 beforeOverlap =
        render_internal::ResolveVolumetricCloudShellOverlapLength_Internal(
            before, 0.0f, 0.5f);
    const f32 afterOverlap =
        render_internal::ResolveVolumetricCloudShellOverlapLength_Internal(
            after, 0.0f, 0.5f);
    const f32 representableBoundaryMotion =
        after.intervals[0].exit - before.intervals[0].exit;
    EXPECT_NEAR(
        afterOverlap - beforeOverlap,
        representableBoundaryMotion, 1.0e-8f);
    EXPECT_NEAR(
        representableBoundaryMotion,
        2.0f * boundaryDelta, 3.0e-8f);
    EXPECT_TRUE(afterOverlap - beforeOverlap < 4.0e-6f);
}

ACS_TEST(VolumetricClouds,
         SubrayIntervalEnvelopeIsPermutationInvariantAndKeepsLargestGap) {
    using render_internal::ResolveVolumetricCloudIntervalEnvelopePair_Internal;

    // 旧逐次結合が入力順で異なる結果を返した四連結成分を、4通りの順序で照合する。
    const FVolumetricCloudRayInterval permutations[4][8]{
        {
            {0.0f, 1.0f, true}, {10.0f, 11.0f, true},
            {20.0f, 21.0f, true}, {12.0f, 13.0f, true},
        },
        {
            {20.0f, 21.0f, true}, {12.0f, 13.0f, true},
            {0.0f, 1.0f, true}, {10.0f, 11.0f, true},
        },
        {
            {10.0f, 11.0f, true}, {0.0f, 1.0f, true},
            {12.0f, 13.0f, true}, {20.0f, 21.0f, true},
        },
        {
            {12.0f, 13.0f, true}, {20.0f, 21.0f, true},
            {10.0f, 11.0f, true}, {0.0f, 1.0f, true},
        },
    };
    for (u32 permutationIndex = 0u;
         permutationIndex < 4u; ++permutationIndex)
    {
        const auto result =
            ResolveVolumetricCloudIntervalEnvelopePair_Internal(
                permutations[permutationIndex], 8u);
        EXPECT_EQ(result.interval_count, 2u);
        EXPECT_NEAR(result.intervals[0].enter, 0.0f, 0.0f);
        EXPECT_NEAR(result.intervals[0].exit, 1.0f, 0.0f);
        EXPECT_NEAR(result.intervals[1].enter, 10.0f, 0.0f);
        EXPECT_NEAR(result.intervals[1].exit, 21.0f, 0.0f);
    }

    // 同じ最大空隙では近い空隙を分割境界にして結果を一意にし、
    // 無効候補は余分な切れ目を作らない。
    const FVolumetricCloudRayInterval equalGaps[8]{
        {6.0f, 7.0f, true}, {3.0f, 4.0f, true},
        {0.0f, 1.0f, true}, {8.0f, 2.0f, true},
        {100.0f, 101.0f, false},
    };
    const auto equalGapResult =
        ResolveVolumetricCloudIntervalEnvelopePair_Internal(
            equalGaps, 8u);
    EXPECT_EQ(equalGapResult.interval_count, 2u);
    EXPECT_NEAR(equalGapResult.intervals[0].enter, 0.0f, 0.0f);
    EXPECT_NEAR(equalGapResult.intervals[0].exit, 1.0f, 0.0f);
    EXPECT_NEAR(equalGapResult.intervals[1].enter, 3.0f, 0.0f);
    EXPECT_NEAR(equalGapResult.intervals[1].exit, 7.0f, 0.0f);

    FVolumetricCloudRayInterval tooManyCandidates[9]{};
    tooManyCandidates[0] = FVolumetricCloudRayInterval{
        0.0f, 1.0f, true};
    const auto rejected =
        ResolveVolumetricCloudIntervalEnvelopePair_Internal(
            tooManyCandidates, 9u);
    EXPECT_EQ(rejected.interval_count, 0u);
}

ACS_TEST(VolumetricClouds, CurvedShellBoundaryDoesNotCrossIntoTheWrongSide) {
    const FVolumetricCloudLayer layer{96.0f, 128.0f, 0.035f};

    // 雲底上の境界では、下向きレイは雲へ入らず、上向きレイだけが層を通る。
    const FVolumetricCloudRayInterval baseDown =
        IntersectVolumetricCloudShell(
            FVec3{0.0f, 96.0f, 0.0f}, FVec3{0.0f, -1.0f, 0.0f}, layer);
    const FVolumetricCloudRayInterval baseUp =
        IntersectVolumetricCloudShell(
            FVec3{0.0f, 96.0f, 0.0f}, FVec3{0.0f, 1.0f, 0.0f}, layer);
    EXPECT_FALSE(baseDown.hit);
    EXPECT_TRUE(baseUp.hit);
    EXPECT_NEAR(baseUp.enter, 0.0f, 1e-3f);
    EXPECT_NEAR(baseUp.exit, 32.0f, 1e-3f);

    // 雲頂上の境界では、上向きレイは雲へ入らず、下向きレイだけが層を通る。
    const FVolumetricCloudRayInterval topUp =
        IntersectVolumetricCloudShell(
            FVec3{0.0f, 128.0f, 0.0f}, FVec3{0.0f, 1.0f, 0.0f}, layer);
    const FVolumetricCloudRayInterval topDown =
        IntersectVolumetricCloudShell(
            FVec3{0.0f, 128.0f, 0.0f}, FVec3{0.0f, -1.0f, 0.0f}, layer);
    EXPECT_FALSE(topUp.hit);
    EXPECT_TRUE(topDown.hit);
    EXPECT_NEAR(topDown.enter, 0.0f, 1e-3f);
    EXPECT_NEAR(topDown.exit, 32.0f, 1e-3f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader,
        "t1=(headingInward&&hitsInner&&innerNear>=0.0)?innerNear:outerFar;"));
    EXPECT_TRUE(Contains(
        source,
        "out.exit = (centreDot < 0.0 && hitsInner && innerNear >= 0.0f)"));
}

ACS_TEST(VolumetricClouds,
         CloudDepthRangeCoversThePhysicalGeometricHorizon) {
    const FVolumetricCloudLayer layer{};
    const FVolumetricCloudRayInterval horizon =
        IntersectVolumetricCloudShell(
            FVec3{0.0f, 1.5f, 0.0f}, FVec3{1.0f, 0.0f, 0.0f}, layer);
    EXPECT_TRUE(horizon.hit);
    EXPECT_TRUE(horizon.enter > 100000.0f);
    EXPECT_TRUE(horizon.enter < kVolumetricCloudMaxDistance);

    const std::string source = ReadSkySource();
    EXPECT_TRUE(Contains(
        source, "RWTexture2D<float2> historyDepthOut : register(u1)"));
    EXPECT_TRUE(Contains(
        source,
        "make_texture(hw, hh, EFormat::R32G32_Float, true, false, lowDepth)"));
    EXPECT_TRUE(Contains(
        source,
        "make_texture(fw, fh, EFormat::R32G32_Float, true, false, historyDepth[0])"));
    EXPECT_TRUE(Contains(source, "float2(250001.0,0.0)"));
    EXPECT_TRUE(!Contains(source, "cloudHit.x > 65000.0"));
    EXPECT_TRUE(!Contains(source, "refD.x<65000.0"));
}

ACS_TEST(VolumetricClouds,
         CurvedShellEntryRemainsWorldAnchoredAcrossCameraTranslation) {
    const FVolumetricCloudLayer layer{96.0f, 128.0f, 0.035f};
    const f64 innerRadius =
        static_cast<f64>(kVolumetricCloudPlanetRadius) +
        static_cast<f64>(layer.base_height);
    const f32 anchorX = 180.0f;
    const f32 anchorZ = -270.0f;
    const f32 anchorY = static_cast<f32>(
        std::sqrt(
            innerRadius * innerRadius -
            static_cast<f64>(anchorX) * anchorX -
            static_cast<f64>(anchorZ) * anchorZ) -
        static_cast<f64>(kVolumetricCloudPlanetRadius));
    const FVec3 worldAnchor{anchorX, anchorY, anchorZ};

    const FVec3 cameras[]{
        FVec3{-24.0f, 8.0f, 17.0f},
        FVec3{53.0f, 31.0f, -41.0f},
    };
    for (const FVec3 camera : cameras) {
        const FVec3 direction = NormalizeForTest(
            FVec3{worldAnchor.x - camera.x,
                  worldAnchor.y - camera.y,
                  worldAnchor.z - camera.z});
        const FVolumetricCloudRayInterval interval =
            IntersectVolumetricCloudShell(camera, direction, layer);
        EXPECT_TRUE(interval.hit);
        ExpectVec3Near(
            PointOnRay(camera, direction, interval.enter),
            worldAnchor, 2e-2f);
    }
}

ACS_TEST(VolumetricClouds,
         CurvedShellRebasesForLongDistanceFlightWithoutCameraFollowing) {
    const FVolumetricCloudLayer layer{96.0f, 128.0f, 0.035f};
    const FVec3 nearCamera{0.0f, 8.0f, 0.0f};
    const FVec3 farCamera{13107200.0f, 8.0f, -6553600.0f};
    const FVec3 nearOrigin =
        RebaseVolumetricCloudWorldOrigin(nearCamera);
    const FVec3 farOrigin =
        RebaseVolumetricCloudWorldOrigin(farCamera);

    EXPECT_NEAR(nearOrigin.x, 0.0f, 1e-6f);
    EXPECT_NEAR(nearOrigin.z, 0.0f, 1e-6f);
    EXPECT_NEAR(farOrigin.x, farCamera.x, 1e-6f);
    EXPECT_NEAR(farOrigin.z, farCamera.z, 1e-6f);
    EXPECT_NEAR(farOrigin.y, 0.0f, 1e-6f);

    const FVolumetricCloudRayInterval nearInterval =
        IntersectVolumetricCloudShell(
            nearCamera, FVec3{0.0f, 1.0f, 0.0f}, layer,
            kVolumetricCloudPlanetRadius, nearOrigin);
    const FVolumetricCloudRayInterval farInterval =
        IntersectVolumetricCloudShell(
            farCamera, FVec3{0.0f, 1.0f, 0.0f}, layer,
            kVolumetricCloudPlanetRadius, farOrigin);
    EXPECT_TRUE(nearInterval.hit);
    EXPECT_TRUE(farInterval.hit);
    EXPECT_NEAR(farInterval.enter, nearInterval.enter, 1e-3f);
    EXPECT_NEAR(farInterval.exit, nearInterval.exit, 1e-3f);

    // The tangent origin remains fixed through the central half of a cell, so
    // an ordinary editor orbit does not drag the shell with the camera.
    const FVec3 withinCellA =
        RebaseVolumetricCloudWorldOrigin(FVec3{3.0f, 8.0f, -7.0f});
    const FVec3 withinCellB =
        RebaseVolumetricCloudWorldOrigin(FVec3{15.0f, 8.0f, 15.0f});
    ExpectVec3Near(withinCellA, withinCellB, 1e-6f);

    // Crossing the old hard-round boundary at half a cell must not move the
    // local planet centre by 64 units in one step.
    const FVec3 beforeBoundary =
        RebaseVolumetricCloudWorldOrigin(FVec3{31.999f, 8.0f, -32.001f});
    const FVec3 afterBoundary =
        RebaseVolumetricCloudWorldOrigin(FVec3{32.001f, 8.0f, -31.999f});
    EXPECT_NEAR(beforeBoundary.x, afterBoundary.x, 0.01f);
    EXPECT_NEAR(beforeBoundary.z, afterBoundary.z, 0.01f);

    const FVec3 transitionStart =
        RebaseVolumetricCloudWorldOrigin(FVec3{16.0f, 8.0f, 0.0f});
    const FVec3 transitionMiddle =
        RebaseVolumetricCloudWorldOrigin(FVec3{32.0f, 8.0f, 0.0f});
    const FVec3 transitionEnd =
        RebaseVolumetricCloudWorldOrigin(FVec3{48.0f, 8.0f, 0.0f});
    EXPECT_NEAR(transitionStart.x, 0.0f, 1e-6f);
    EXPECT_NEAR(transitionMiddle.x, 32.0f, 1e-5f);
    EXPECT_NEAR(transitionEnd.x, 64.0f, 1e-5f);

    // The old fixed global centre cannot represent a local tangent patch after
    // this flight distance and misses the upward cloud layer entirely.
    const FVolumetricCloudRayInterval fixedOriginInterval =
        IntersectVolumetricCloudShell(
            farCamera, FVec3{0.0f, 1.0f, 0.0f}, layer);
    EXPECT_FALSE(fixedOriginInterval.hit);
}

ACS_TEST(VolumetricClouds,
         TemporalLightingDifferenceUsesContinuousMismatch) {
    const FVec3 sunDirection{0.0f, 1.0f, 0.0f};
    const FVec3 sunColor{1.0f, 0.95f, 0.85f};
    const FVec3 skyColor{0.2f, 0.4f, 0.8f};
    EXPECT_FALSE(VolumetricCloudLightingChanged(
        sunDirection, sunColor, skyColor,
        sunDirection, sunColor, skyColor));
    EXPECT_TRUE(VolumetricCloudLightingChanged(
        sunDirection, sunColor, skyColor,
        FVec3{0.001f, 0.9999995f, 0.0f}, sunColor, skyColor));
    EXPECT_TRUE(VolumetricCloudLightingChanged(
        sunDirection, sunColor, skyColor,
        sunDirection, FVec3{1.0f, 0.94f, 0.85f}, skyColor));
    EXPECT_TRUE(VolumetricCloudLightingChanged(
        sunDirection, sunColor, skyColor,
        sunDirection, sunColor, FVec3{0.2f, 0.41f, 0.8f}));

    const FVolumetricCloudLighting lighting{};
    EXPECT_NEAR(
        VolumetricCloudLightingTemporalMismatch(
            lighting, sunDirection, sunColor, skyColor,
            lighting, sunDirection, sunColor, skyColor),
        0.0f, 1.0e-6f);
    const f32 small_direction_mismatch =
        VolumetricCloudLightingTemporalMismatch(
            lighting, sunDirection, sunColor, skyColor,
            lighting, FVec3{0.01f, 0.99995f, 0.0f}, sunColor, skyColor);
    EXPECT_TRUE(small_direction_mismatch > 0.0f);
    EXPECT_TRUE(small_direction_mismatch < 0.01f);
    EXPECT_NEAR(
        VolumetricCloudLightingTemporalMismatch(
            lighting, sunDirection, sunColor, skyColor,
            lighting, sunDirection, FVec3{1.1f, 0.95f, 0.85f}, skyColor),
        1.0f - 1.0f / 1.1f, 1.0e-5f);
    EXPECT_NEAR(
        VolumetricCloudLightingTemporalMismatch(
            lighting, sunDirection, sunColor, skyColor,
            lighting, FVec3{0.0f, -1.0f, 0.0f}, sunColor, skyColor),
        1.0f, 1.0e-6f);
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_NEAR(
        VolumetricCloudLightingTemporalMismatch(
            lighting, sunDirection, sunColor, skyColor,
            lighting, sunDirection, FVec3{nan, 0.0f, 0.0f}, skyColor),
        1.0f, 0.0f);

    const std::string source = ReadSkySource();
    EXPECT_FALSE(Contains(
        source, "if (VolumetricCloudLightingChanged("));
    EXPECT_TRUE(Contains(
        source, "VolumetricCloudLightingTemporalMismatch("));
    EXPECT_TRUE(Contains(
        source, "if (historyTransportChanged) {"));
    EXPECT_TRUE(Contains(
        source, "cb.cloudLightingHistory = FVec4{lightingMismatch"));
    EXPECT_TRUE(Contains(
        source, "recorded.previous_sun_direction = safeSun;"));
    EXPECT_TRUE(Contains(
        source, "recorded.previous_sun_color = safeSunColor;"));
    EXPECT_TRUE(Contains(
        source, "recorded.previous_sky_color = safeSkyColor;"));
    EXPECT_TRUE(Contains(
        source, "m_PrevSunDir = recorded.previous_sun_direction;"));
    EXPECT_TRUE(Contains(
        source, "m_PrevSunColor = recorded.previous_sun_color;"));
    EXPECT_TRUE(Contains(
        source, "m_PrevSkyColor = recorded.previous_sky_color;"));
}

ACS_TEST(VolumetricClouds,
         ShaderDensityDomainsStayWorldAnchored) {
    const std::string source = ReadSkySource();
    const std::string shader =
        ExtractRawShader(source, "const char* kCloudCS");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!shader.empty());

    // レイ始点はカメラに依存するが、密度の標本位置は絶対ワールド座標である。
    // 雑音と天候の採取では、カメラ位置をもう一度引かずにこの座標を直接使う。
    EXPECT_TRUE(Contains(
        shader, "float3 currentP=camPos.xyz+rayDirection*currentSampleT;"));
    EXPECT_TRUE(Contains(shader, "float MAX_DISTANCE=min(cloudRange.x,max(cloudRange.w,1.0));"));
    EXPECT_TRUE(Contains(
        shader, "float3 local=p-worldOrigin.xyz;"));
    EXPECT_TRUE(Contains(
        shader,
        "macro.weather=cloudWeatherDataBandLimitedPoint(\n"
        "        p,safeFootprint.xz);"));
    EXPECT_TRUE(Contains(
        CompactShader(shader),
        "macro.shapePotential=cloudShapePotentialBands("
        "cloudUVW(p,macro.layerHeight,upperBand));"));
    EXPECT_TRUE(Contains(
        CompactShader(shader),
        "floatmaximumDomainFootprint="
        "cloudShapeMaximumDomainFootprint("
        "physicalFootprint,upperBand);"));
    EXPECT_TRUE(Contains(
        CompactShader(shader),
        "floatmaximumShapePotential=cloudShapeOccupancyMaximum("
        "cloudUVW(p,layerHeight,upperBand),maximumDomainFootprint);"));
    EXPECT_TRUE(!Contains(
        shader, "cloudWeatherData(p-camPos"));
    EXPECT_TRUE(!Contains(
        shader, "cloudUVW(p-camPos"));
    EXPECT_TRUE(!Contains(shader, "p.x-camPos.x"));
    EXPECT_TRUE(!Contains(shader, "p.y-camPos.y"));
    EXPECT_TRUE(!Contains(shader, "p.z-camPos.z"));
    EXPECT_TRUE(!Contains(shader, "cloudWeatherData(p-worldOrigin"));
    EXPECT_TRUE(!Contains(shader, "cloudUVW(p-worldOrigin"));
}

ACS_TEST(VolumetricClouds,
         WeatherShapeDetailAndCurlUseIndependentResources) {
    const std::string source = ReadSkySource();
    const std::string shader =
        ExtractRawShader(source, "const char* kCloudCS");
    const std::string noiseShader = CompactShader(
        ExtractRawShader(source, "const char* kNoiseGenCS"));
    const std::string noiseFilterShader = CompactShader(
        ExtractRawShader(source, "const char* kNoiseFilterCS"));
    const std::string weatherShader = CompactShader(
        ExtractRawShader(source, "const char* kWeatherGenCS"));
    const std::string detailShader = CompactShader(
        ExtractRawShader(source, "const char* kDetailGenCS"));
    const std::string curlShader = CompactShader(
        ExtractRawShader(source, "const char* kCurlGenCS"));
    const std::string compactSource = CompactShader(source);
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(!noiseShader.empty());
    EXPECT_TRUE(!noiseFilterShader.empty());
    EXPECT_TRUE(!weatherShader.empty());
    EXPECT_TRUE(!detailShader.empty());
    EXPECT_TRUE(!curlShader.empty());

    // 雲塊配置、基本形状、縁の侵食、渦は別の領域を使う。
    // 一つの形状体積を全用途へ流用すると、同じ繰り返し模様が露出する。
    EXPECT_TRUE(Contains(
        shader, "Texture3D<float4> shapeNoise     : register(t0)"));
    EXPECT_TRUE(Contains(
        shader, "Texture3D<float4> shapeOccupancy : register(t1)"));
    EXPECT_TRUE(Contains(
        shader, "Texture2D    weatherMap          : register(t2)"));
    EXPECT_TRUE(Contains(
        shader, "Texture3D<float2> detailNoise    : register(t3)"));
    EXPECT_TRUE(Contains(
        shader, "Texture2D    curlNoise           : register(t4)"));
    EXPECT_TRUE(Contains(
        shader, "weatherMap.SampleLevel("));
    EXPECT_TRUE(Contains(
        shader, "weatherMap_sampler"));
    EXPECT_TRUE(Contains(
        shader, "detailNoise.SampleLevel("));
    EXPECT_TRUE(Contains(
        CompactShader(shader),
        "curlNoise.SampleLevel(curlNoise_sampler"));

    // 2D被覆は雲塊の配置だけを決め、約315 mの細部を雲層全高へ押し出さない。
    const std::size_t coverageFbmBegin = weatherShader.find(
        "floatweatherCoverageFbm(");
    const std::size_t coverageFbmEnd = weatherShader.find(
        "returnsaturate(n);}", coverageFbmBegin);
    EXPECT_TRUE(coverageFbmBegin != std::string::npos);
    EXPECT_TRUE(coverageFbmEnd != std::string::npos);
    if (coverageFbmBegin != std::string::npos &&
        coverageFbmEnd != std::string::npos) {
        const std::string coverageFbm = weatherShader.substr(
            coverageFbmBegin, coverageFbmEnd - coverageFbmBegin);
        EXPECT_TRUE(Contains(
            coverageFbm,
            "periodicValue(uv*3.0,3.0,seed)*0.68;"));
        EXPECT_TRUE(Contains(
            coverageFbm,
            "periodicValue(uv*7.0,7.0,seed+17.0)*0.32;"));
        EXPECT_FALSE(Contains(coverageFbm, "uv*13.0"));
        EXPECT_FALSE(Contains(coverageFbm, "uv*29.0"));
    }
    EXPECT_TRUE(Contains(
        weatherShader,
        "floatcoverage=weatherCoverageFbm("
        "uv,float2(11.7,29.3));"));
    EXPECT_TRUE(Contains(weatherShader, "floatstorm=weatherCoverageFbm(" "float2(1.0-uv.y,uv.x)+float2(0.31,0.07)," "float2(103.7,47.2));"));
    EXPECT_TRUE(Contains(weatherShader, "floatwarp=weatherCoverageFbm(" "uv+float2(0.53,0.23),float2(151.9,73.4));"));
    EXPECT_FALSE(Contains(weatherShader, "floatstorm=weatherFbm("));
    EXPECT_FALSE(Contains(weatherShader, "floatwarp=weatherFbm("));

    // 最初の体積では符号付き湿度場を周波数別に保存し、
    // 次の体積では各帯域の最大ポテンシャルだけを広げる。
    EXPECT_TRUE(Contains(
        noiseShader,
        "RWTexture3D<float4>shapeSourceOut:register(u0);"));
    EXPECT_TRUE(Contains(
        noiseShader,
        "RWTexture3D<float4>shapeOccupancySourceOut:register(u1);"));
    EXPECT_TRUE(Contains(
        noiseShader,
        "float3wrapPeriodicCell(float3cell,floatfreq){"
        "returncell-floor(cell/freq)*freq;}"));
    EXPECT_FALSE(Contains(noiseShader, "fmod("));
    EXPECT_TRUE(Contains(noiseShader, "floatperlin4=gnoise(warpedUvw*4.0,4.0);"));
    EXPECT_TRUE(Contains(noiseShader, "floatperlin8=gnoise(warpedUvw*8.0,8.0);"));
    EXPECT_TRUE(Contains(noiseShader, "floatwarpX=gnoise((uvw+float3(0.173,0.417,0.619))*2.0,2.0);"));
    EXPECT_TRUE(Contains(noiseShader, "floatwarpZ=gnoise((uvw+float3(0.731,0.251,0.847))*2.0,2.0);"));
    EXPECT_FALSE(Contains(noiseShader, "floatwarpX=gnoise(uvw+float3(0.173,0.417,0.619),1.0);"));
    EXPECT_FALSE(Contains(noiseShader, "floatwarpZ=gnoise(uvw+float3(0.731,0.251,0.847),1.0);"));
    EXPECT_TRUE(Contains(noiseShader, "returnuvw+domainWarp;"));
    EXPECT_TRUE(Contains(noiseShader, "floatperlin2=gnoise(warpedUvw*2.0,2.0);"));
    EXPECT_FALSE(Contains(noiseShader, "floatperlin16="));
    EXPECT_TRUE(Contains(noiseShader, "floatworley4A=worley(warpedUvw,4.0);"));
    EXPECT_TRUE(Contains(
        noiseShader,
        "floatworley4B=worley("
        "warpedUvw+float3(0.50,0.25,0.75),4.0);"));
    EXPECT_TRUE(Contains(
        noiseShader,
        "floatboundaryDisplacement="
        "signedCloudNoise(middlePerlin)*0.50+"
        "signedCloudNoise(finePerlin)*0.25+"
        "(saturate(worleyA)-saturate(worleyB))*0.25;"));
    EXPECT_TRUE(Contains(
        noiseShader,
        "floatboundaryWeight=1.0-abs(macroPotential);"));
    const std::size_t completedComposition = noiseShader.find(
        "floatcompletedPotential=completedHierarchicalCloudPotential("
        "perlin2,perlin4,perlin8,worley4A,worley4B);");
    const std::size_t coarseComposition = noiseShader.find(
        "floatcoarsePotential=completedHierarchicalCloudPotential("
        "perlin2,0.5,0.5,0.5,0.5);");
    const std::size_t middleComposition = noiseShader.find(
        "floatmiddlePotential=completedHierarchicalCloudPotential("
        "perlin2,perlin4,0.5,0.5,0.5);");
    const std::size_t completedWrite = noiseShader.find(
        "shapeSourceOut[id]=float4("
        "storedSignedCloudPotential(coarsePotential),"
        "storedSignedCloudPotential(middlePotential),"
        "storedSignedCloudPotential(completedPotential),0.5);");
    const std::size_t occupancyMaximum = noiseShader.find(
        "floatmaximumPotential=max("
        "coarsePotential,max(middlePotential,completedPotential));");
    const std::size_t occupancyInflation = noiseShader.find(
        "floatconservativeStoredPotential=min("
        "storedSignedCloudPotential(maximumPotential)+1.0/255.0,1.0);");
    const std::size_t occupancyWrite = noiseShader.find(
        "shapeOccupancySourceOut[id]=conservativeStoredPotential.xxxx;");
    EXPECT_TRUE(completedComposition != std::string::npos);
    EXPECT_TRUE(coarseComposition != std::string::npos);
    EXPECT_TRUE(middleComposition != std::string::npos);
    EXPECT_TRUE(completedWrite != std::string::npos);
    EXPECT_TRUE(occupancyMaximum != std::string::npos);
    EXPECT_TRUE(occupancyInflation != std::string::npos);
    EXPECT_TRUE(occupancyWrite != std::string::npos);
    EXPECT_TRUE(completedComposition < completedWrite);
    EXPECT_TRUE(coarseComposition < completedWrite);
    EXPECT_TRUE(middleComposition < completedWrite);
    EXPECT_TRUE(completedWrite < occupancyMaximum);
    EXPECT_TRUE(occupancyMaximum < occupancyInflation);
    EXPECT_TRUE(occupancyInflation < occupancyWrite);
    EXPECT_FALSE(Contains(noiseShader, "shapeLevelsAt("));
    EXPECT_FALSE(Contains(noiseShader, "shapeLevelsColumnAt("));
    EXPECT_FALSE(Contains(noiseShader, "cloudMacroDensity("));
    EXPECT_FALSE(Contains(noiseShader, "storedShapeLevel("));
    EXPECT_FALSE(Contains(noiseShader, "erosionPerlin"));
    EXPECT_FALSE(Contains(noiseShader, "erosionWorley"));
    EXPECT_FALSE(Contains(source, "const char* kNoiseDownsampleCS"));
    EXPECT_TRUE(Contains(
        noiseFilterShader,
        "Texture3D<float4>shapeFilterSource:register(t0);"));
    EXPECT_TRUE(Contains(
        noiseFilterShader,
        "RWTexture3D<float4>shapeFilterOut:register(u0);"));
    EXPECT_TRUE(Contains(
        noiseFilterShader,
        "groupsharedfloat3shapeMaximumA[128];"));
    EXPECT_TRUE(Contains(
        noiseFilterShader,
        "groupsharedfloat3shapeMaximumB[128];"));
    EXPECT_TRUE(Contains(
        noiseFilterShader,
        "[numthreads(128,1,1)]"));
    EXPECT_TRUE(Contains(
        noiseFilterShader,
        "uint3sourceCoord=uint3(lineIndex,groupId.x,groupId.y);"));
    EXPECT_TRUE(Contains(
        noiseFilterShader,
        "float4sourceValue=shapeFilterSource.Load(int4(sourceCoord,0));"));
    EXPECT_TRUE(Contains(
        noiseFilterShader,
        "shapeMaximumB[lineIndex]=max("
        "shapeMaximumA[lineIndex],"
        "shapeMaximumA[wrapShapeLineIndex(int(lineIndex)+1)]);"));
    EXPECT_TRUE(Contains(
        noiseFilterShader,
        "float3maximum32=shapeMaximumB[lineIndex];"
        "float3extension32=shapeMaximumB["
        "wrapShapeLineIndex(int(lineIndex)+32)];"));
    EXPECT_TRUE(Contains(
        noiseFilterShader,
        "float3conservativeMaximum=float3("
        "max(shapeMaximumA["
        "wrapShapeLineIndex(int(lineIndex)-3)].r,"
        "shapeMaximumA[lineIndex].r),"
        "max(shapeMaximumA["
        "wrapShapeLineIndex(int(lineIndex)-9)].g,"
        "shapeMaximumA["
        "wrapShapeLineIndex(int(lineIndex)-6)].g),"
        "max(shapeMaximumA["
        "wrapShapeLineIndex(int(lineIndex)-33)].b,"
        "shapeMaximumA["
        "wrapShapeLineIndex(int(lineIndex)-30)].b));"));
    EXPECT_TRUE(Contains(
        noiseFilterShader,
        "shapeFilterOut[sourceCoord.yzx]=saturate(float4("
        "conservativeMaximum,sourceValue.a));"));
    EXPECT_EQ(
        CountOccurrences(
            noiseFilterShader, "GroupMemoryBarrierWithGroupSync();"),
        static_cast<std::size_t>(7));
    EXPECT_TRUE(Contains(noiseFilterShader, "shapeMaximum"));
    EXPECT_FALSE(Contains(noiseFilterShader, "shapePrefix"));
    EXPECT_FALSE(Contains(noiseFilterShader, "SampleLevel("));
    EXPECT_FALSE(Contains(noiseFilterShader, "shapeLevel"));
    EXPECT_FALSE(Contains(noiseFilterShader, "gnoise("));
    EXPECT_FALSE(Contains(noiseFilterShader, "worley("));
    EXPECT_FALSE(Contains(noiseFilterShader, "completedPerlinWorleyShape("));
    EXPECT_FALSE(Contains(noiseShader, "completedPerlinWorleyShape("));
    EXPECT_FALSE(Contains(noiseShader, "remapShape("));
    EXPECT_FALSE(Contains(noiseFilterShader, "averageCompletedShapeEightPoint("));
    EXPECT_FALSE(Contains(noiseFilterShader, "averageCompletedShapeWide("));

    // 高周波の全成分が最大でも、低周波核の深い外部へ孤立粒を作らない。
    EXPECT_NEAR(
        CloudHierarchicalShapeForTest(0.25f, 1.0f, 1.0f, 1.0f, 0.0f),
        0.0f, 0.0f);
    // 深い内部は高周波の全成分が最小でも消えず、雲塊の核を分断しない。
    EXPECT_TRUE(
        CloudHierarchicalShapeForTest(0.80f, 0.0f, 0.0f, 0.0f, 1.0f) >
        0.0f);
    // 同じ分布を平行移動したWorley対が同値なら、細胞成分は形状へ偏りを加えない。
    EXPECT_NEAR(
        CloudHierarchicalShapeForTest(0.60f, 0.50f, 0.50f, 0.20f, 0.20f),
        CloudHierarchicalShapeForTest(0.60f, 0.50f, 0.50f, 0.80f, 0.80f),
        0.0f);
    // 細部を固定したまま湿度核を増やしても、完成形状が逆に減らない。
    for (u32 detailCase = 0u; detailCase < 4u; ++detailCase) {
        const f32 middle = (detailCase & 1u) != 0u ? 1.0f : 0.0f;
        const f32 fine = (detailCase & 2u) != 0u ? 1.0f : 0.0f;
        f32 previous = -1.0f;
        for (u32 macroStep = 0u; macroStep <= 64u; ++macroStep) {
            const f32 macro = static_cast<f32>(macroStep) / 64.0f;
            const f32 shape = CloudHierarchicalShapeForTest(
                macro, middle, fine, 1.0f - middle, middle);
            EXPECT_TRUE(shape + 1.0e-6f >= previous);
            previous = shape;
        }
    }

    // すべての周期生成器で、負座標を壊す符号付きfmodを使わない。
    EXPECT_FALSE(Contains(weatherShader, "fmod("));
    EXPECT_FALSE(Contains(detailShader, "fmod("));
    EXPECT_FALSE(Contains(curlShader, "fmod("));
    EXPECT_TRUE(Contains(
        detailShader,
        "returncell-floor(cell/frequency)*frequency;"));
    EXPECT_TRUE(Contains(
        curlShader,
        "returncell-floor(cell/frequency)*frequency;"));
    EXPECT_TRUE(Contains(
        detailShader, "RWTexture3D<float2>detailOut:register(u0);"));
    EXPECT_TRUE(Contains(detailShader, "detailOut[id]=float2(a,d);"));
    EXPECT_EQ(
        CountOccurrences(
            compactSource,
            "td.width=128;td.height=128;td.depth=128;"
            "td.format=EFormat::R16G16B16A16_Float;td.is_uav=true;"),
        static_cast<std::size_t>(1));
    EXPECT_EQ(
        CountOccurrences(
            compactSource,
            "td.width=128;td.height=128;td.depth=128;"
            "td.format=EFormat::R8G8B8A8_UNorm;td.is_uav=true;"),
        static_cast<std::size_t>(2));
    EXPECT_FALSE(Contains(compactSource, "m_ShapeLevel4Tex"));
    EXPECT_FALSE(Contains(compactSource, "m_ShapeLevel16Tex"));
    EXPECT_FALSE(Contains(compactSource, "m_ShapeLevel64Tex"));
    EXPECT_TRUE(Contains(
        compactSource,
        "td.width=64;td.height=64;td.depth=64;"
        "td.format=EFormat::R16G16_Float;td.is_uav=true;"));
    const std::string compactMarch = CompactShader(shader);
    EXPECT_TRUE(Contains(
        compactMarch,
        "float4cloudShapePotentialBands(float3uvw){"));
    EXPECT_TRUE(Contains(
        compactMarch,
        "float4densityBands=shapeNoise.SampleLevel("
        "shapeNoise_sampler,uvw,0);"));
    EXPECT_TRUE(Contains(
        compactMarch,
        "floatcloudShapeOccupancyMaximum("
        "float3uvw,floatmaximumDomainFootprint){"
        "float4occupancyBands=shapeOccupancy.SampleLevel("
        "shapeOccupancy_sampler,uvw,0);"));
    EXPECT_TRUE(Contains(
        compactMarch,
        "macro.shapeFrequencyWeights=cloudShapeFrequencyWeights("
        "maximumDomainFootprint);"));
    EXPECT_TRUE(Contains(
        compactMarch,
        "floatcloudBandLimitedCondensationDensity("));
    EXPECT_FALSE(Contains(compactMarch, "cloudStoredBaseNoise("));
    EXPECT_FALSE(Contains(compactMarch, "cloudPerlinWorleyShape("));
    EXPECT_FALSE(Contains(compactMarch, "floatperlin2="));
    EXPECT_FALSE(Contains(compactMarch, "cloudBaseShapeBand("));
    EXPECT_FALSE(Contains(compactMarch, "cloudShapeErosionBand("));
    EXPECT_FALSE(Contains(compactMarch, "cloudDensityFromShapeErosion("));
    EXPECT_FALSE(Contains(compactMarch, "floatboundarySupport="));
    EXPECT_FALSE(Contains(compactMarch, "floatshapedMacro="));
    EXPECT_TRUE(Contains(compactMarch,
        "floatsignedDetail=detailA.g-detailB.g;"));
    EXPECT_TRUE(Contains(compactMarch,
        "erosionPotential=cloudErosionPotentialOffset("));
    EXPECT_FALSE(Contains(compactMarch, "floatdetailNear="));
    EXPECT_FALSE(Contains(compactMarch, "floatdetailFar="));

    // 生DX12と通常バックエンドの両経路が、周波数別形状と占有最大値を含む
    // 同じシェーダー群をコンパイルし、所有側がその結果を受け取る。
    EXPECT_FALSE(Contains(compactSource, "noise_downsample"));
    EXPECT_TRUE(Contains(
        compactSource,
        "ACS_COMPILE_CLOUD_SHADER(noise_filter,EShaderStage::Compute,"
        "kNoiseFilterCS,\"CSNoiseFilter\",\"Clouds.NoiseFilterCS\");"));
    EXPECT_TRUE(Contains(
        compactSource,
        "ACS_COMPILE_CLOUD_SHADER(weather,EShaderStage::Compute,"
        "kWeatherGenCS,\"CSWeather\",\"Clouds.WeatherCS\");"));
    EXPECT_TRUE(Contains(
        compactSource,
        "ACS_COMPILE_CLOUD_SHADER(detail,EShaderStage::Compute,"
        "kDetailGenCS,\"CSDetail\",\"Clouds.DetailCS\");"));
    EXPECT_TRUE(Contains(
        compactSource,
        "ACS_COMPILE_CLOUD_SHADER(curl,EShaderStage::Compute,"
        "kCurlGenCS,\"CSCurl\",\"Clouds.CurlCS\");"));
    EXPECT_TRUE(Contains(
        compactSource,
        "ACS_CREATE_CLOUD_SHADER(noise_filter,EShaderStage::Compute,"
        "kNoiseFilterCS,\"CSNoiseFilter\",\"Clouds.NoiseFilterCS\");"));
    EXPECT_TRUE(Contains(
        compactSource,
        "ACS_CREATE_CLOUD_SHADER(weather,EShaderStage::Compute,"
        "kWeatherGenCS,\"CSWeather\",\"Clouds.WeatherCS\");"));
    EXPECT_TRUE(Contains(
        compactSource,
        "ACS_CREATE_CLOUD_SHADER(detail,EShaderStage::Compute,"
        "kDetailGenCS,\"CSDetail\",\"Clouds.DetailCS\");"));
    EXPECT_TRUE(Contains(
        compactSource,
        "ACS_CREATE_CLOUD_SHADER(curl,EShaderStage::Compute,"
        "kCurlGenCS,\"CSCurl\",\"Clouds.CurlCS\");"));
    EXPECT_TRUE(Contains(
        compactSource,
        "m_NoiseFilterResources->shader=Move(shaders.noise_filter);"));
    EXPECT_TRUE(Contains(
        compactSource, "m_WeatherCs=Move(shaders.weather);"));
    EXPECT_TRUE(Contains(
        compactSource, "m_DetailCs=Move(shaders.detail);"));
    EXPECT_TRUE(Contains(
        compactSource, "m_CurlCs=Move(shaders.curl);"));
    EXPECT_TRUE(Contains(source, "pd.srv_slots = 6"));
    EXPECT_TRUE(Contains(source, "pd.srv_names[0] = \"shapeNoise\""));
    EXPECT_TRUE(Contains(source, "pd.srv_names[1] = \"shapeOccupancy\""));
    EXPECT_TRUE(Contains(source, "pd.srv_names[2] = \"weatherMap\""));
    EXPECT_TRUE(Contains(source, "pd.srv_names[3] = \"detailNoise\""));
    EXPECT_TRUE(Contains(source, "pd.srv_names[4] = \"curlNoise\""));
    EXPECT_TRUE(Contains(source, "pd.srv_names[5] = \"cloudShadowCache\""));
    EXPECT_TRUE(Contains(source, "pd.static_sampler_count = 6"));
    EXPECT_TRUE(Contains(
        shader,
        "SamplerState shapeOccupancy_sampler  : register(s1);"));
    EXPECT_TRUE(Contains(
        shader,
        "SamplerState cloudShadowCache_sampler : register(s5);"));

    EXPECT_TRUE(Contains(
        source, "cl.SetTexture(0, *m_ShapeTex)"));
    const std::size_t sourceBake = compactSource.find(
        "cl.SetComputePipeline(*m_NoisePipe);"
        "cl.BindUav(0,*m_ShapeTex);"
        "cl.BindUav(1,*m_NoiseFilterResources->source_texture);"
        "cl.Dispatch(32,32,32);");
    const std::size_t xAxisBake = compactSource.find(
        "if(bakeStage==1u){"
        "cl.SetTexture(0,*m_NoiseFilterResources->source_texture);"
        "cl.BindUav(0,*m_NoiseFilterResources->filtered_texture);",
        sourceBake);
    const std::size_t yAxisBake = compactSource.find(
        "elseif(bakeStage==2u){"
        "cl.SetTexture(0,*m_NoiseFilterResources->filtered_texture);"
        "cl.BindUav(0,*m_NoiseFilterResources->source_texture);",
        xAxisBake);
    const std::size_t zAxisBake = compactSource.find(
        "}else{"
        "cl.SetTexture(0,*m_NoiseFilterResources->source_texture);"
        "cl.BindUav(0,*m_NoiseFilterResources->filtered_texture);",
        yAxisBake);
    EXPECT_TRUE(sourceBake != std::string::npos);
    EXPECT_TRUE(xAxisBake != std::string::npos);
    EXPECT_TRUE(yAxisBake != std::string::npos);
    EXPECT_TRUE(zAxisBake != std::string::npos);
    EXPECT_TRUE(sourceBake < xAxisBake);
    EXPECT_TRUE(xAxisBake < yAxisBake);
    EXPECT_TRUE(yAxisBake < zAxisBake);
    EXPECT_EQ(
        CountOccurrences(compactSource, "cl.Dispatch(128,128,1);"),
        static_cast<std::size_t>(1));
    EXPECT_TRUE(Contains(
        compactSource,
        "if(bakeStage>=3u){"
        "recorded.density_bake_stage=4u;"
        "recorded.noise_baked=true;}else{"
        "recorded.density_bake_stage="
        "static_cast<u8>(bakeStage+1u);}"));
    EXPECT_TRUE(Contains(
        compactSource,
        "m_NoiseBaked=recorded.noise_baked;"));
    EXPECT_TRUE(Contains(
        compactSource,
        "m_NoiseFilterResources->density_bake_stage="
        "recorded.density_bake_stage;"));
    EXPECT_TRUE(Contains(
        source, "cl.SetTexture(1, *m_NoiseFilterResources->filtered_texture)"));
    EXPECT_TRUE(Contains(
        source, "cl.SetTexture(2, *m_WeatherTex)"));
    EXPECT_TRUE(Contains(
        source, "cl.SetTexture(3, *m_DetailTex)"));
    EXPECT_TRUE(Contains(
        source, "cl.SetTexture(4, *m_CurlTex)"));
}

ACS_TEST(VolumetricClouds,
         HierarchicalShapeKeepsTheDeterministicVolumeInCloudBanks) {
    constexpr u32 gridSize = 32u;
    constexpr u32 voxelCount = gridSize * gridSize * gridSize;
    static f32 legacyField[voxelCount]{};
    static f32 hierarchicalField[voxelCount]{};
    for (u32 z = 0u; z < gridSize; ++z) {
        for (u32 y = 0u; y < gridSize; ++y) {
            for (u32 x = 0u; x < gridSize; ++x) {
                const u32 index = (z * gridSize + y) * gridSize + x;
                ResolveCloudTopologyShapesForTest(
                    FCloudNoisePointForTest{
                        (static_cast<f32>(x) + 0.5f) /
                            static_cast<f32>(gridSize),
                        (static_cast<f32>(y) + 0.5f) /
                            static_cast<f32>(gridSize),
                        (static_cast<f32>(z) + 0.5f) /
                            static_cast<f32>(gridSize)},
                    legacyField[index], hierarchicalField[index]);
            }
        }
    }

    // 0.18は旧実装が正の雲体として扱っていた境界で比較し、同じ決定的な
    // Perlin/Worley標本に対して細胞粒から連続した雲塊へ変わることを検査する。
    const FCloudTopologyMetricsForTest legacy =
        ResolveCloudTopologyMetricsForTest<gridSize>(legacyField, 0.18f);
    const FCloudTopologyMetricsForTest hierarchical =
        ResolveCloudTopologyMetricsForTest<gridSize>(
            hierarchicalField, 0.18f);
    EXPECT_TRUE(hierarchical.occupied_count > voxelCount / 8u);
    EXPECT_TRUE(hierarchical.occupied_count < voxelCount / 2u);
    EXPECT_TRUE(hierarchical.component_count < legacy.component_count);
    EXPECT_TRUE(
        static_cast<u64>(hierarchical.largest_component_count) * 100u >=
        static_cast<u64>(hierarchical.occupied_count) * 97u);
    const f32 legacyLargestShare = static_cast<f32>(
        legacy.largest_component_count) /
        static_cast<f32>(legacy.occupied_count);
    const f32 hierarchicalLargestShare = static_cast<f32>(
        hierarchical.largest_component_count) /
        static_cast<f32>(hierarchical.occupied_count);
    EXPECT_TRUE(
        hierarchicalLargestShare > legacyLargestShare + 0.05f);
}

ACS_TEST(VolumetricClouds,
         WeatherEnvelopeRemainsTwoDimensionalWhileShapeUsesPhysicalHeight) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    const std::size_t weatherBegin =
        shader.find(
            "float4cloudWeatherDataBandLimitedPoint("
            "float3p,float2resolutionFootprint){");
    const std::size_t weatherEnd =
        shader.find("float3rotateNoise(", weatherBegin);
    EXPECT_TRUE(weatherBegin != std::string::npos);
    EXPECT_TRUE(weatherEnd != std::string::npos);
    if (weatherBegin != std::string::npos &&
        weatherEnd != std::string::npos) {
        const std::string weatherFunction = shader.substr(
            weatherBegin, weatherEnd - weatherBegin);
        EXPECT_FALSE(Contains(weatherFunction, "p.y"));
        EXPECT_FALSE(Contains(weatherFunction, "layerHeight"));
        EXPECT_FALSE(Contains(weatherFunction, "upperBand"));
        EXPECT_FALSE(Contains(weatherFunction, "cloudHeightShapeShear"));
    }
    EXPECT_TRUE(Contains(
        shader,
        "float2xz=p.xz-cloudWindWorld();"));
    EXPECT_EQ(
        CountOccurrences(
            shader,
            "cloudWeatherDataBandLimitedPoint(p,safeFootprint.xz);"),
        static_cast<std::size_t>(1));
    EXPECT_TRUE(Contains(shader, "cloudWeatherDataBandLimitedPoint(p,safeFootprint.xz);"));
    EXPECT_FALSE(Contains(shader, "cloudWeatherVerticalBend"));
    EXPECT_FALSE(Contains(
        shader, "float4cloudWeatherData(float3p,floatlayerHeight"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcanonicalY=saturate(normalizedLayerHeight)"
        "*cloudShapeVerticalSpan(upperBand)+0.07;"));
    EXPECT_TRUE(Contains(shader, "float2cloudHeightShapeShear(floatlayerHeight,boolupperBand){" "floatbandScale=upperBand?0.25:1.0;" "returnfloat2(0.9284767,0.3713907)" "*(850.0*saturate(layerHeight)*bandScale);}"));
    EXPECT_TRUE(Contains(shader, "float2xz=p.xz-cloudWindWorld()" "+cloudHeightShapeShear(normalizedLayerHeight,upperBand);"));
    EXPECT_EQ(CountOccurrences(shader, "cloudHeightShapeShear("), static_cast<std::size_t>(3));
    EXPECT_FALSE(Contains(shader, "weatherWarp"));
    EXPECT_FALSE(Contains(shader, "curlWarp"));
    EXPECT_FALSE(Contains(shader, "convectionWarp"));
    EXPECT_FALSE(Contains(shader, "localCanonicalY"));
    EXPECT_FALSE(Contains(shader, "cloudShapeVerticalVariation"));
    EXPECT_TRUE(Contains(
        shader,
        "float3canonicalPosition=float3("));
    EXPECT_TRUE(Contains(
        shader,
        "returnrotateNoise(canonicalPosition);"));
    EXPECT_TRUE(Contains(
        shader,
        "float3vertical=altitude*float3("
        "0.8000000,-0.4242641,-0.4242641);"));

    // 公開式の850 mずれを独立計算し、方向の長さ、高度端、上層倍率を検査する。
    constexpr f32 kShearDirectionX = 0.9284767f;
    constexpr f32 kShearDirectionY = 0.3713907f;
    constexpr f32 kLowerLayerTopShear = 850.0f;
    constexpr f32 kUpperLayerTopShear = kLowerLayerTopShear * 0.25f;
    EXPECT_NEAR(kShearDirectionX * kShearDirectionX + kShearDirectionY * kShearDirectionY, 1.0f, 1.0e-6f);
    EXPECT_NEAR(kLowerLayerTopShear * 0.0f, 0.0f, 0.0f);
    EXPECT_NEAR(kLowerLayerTopShear * 0.5f, 425.0f, 0.0f);
    EXPECT_NEAR(kLowerLayerTopShear, 850.0f, 0.0f);
    EXPECT_NEAR(kUpperLayerTopShear, 212.5f, 0.0f);
}

ACS_TEST(VolumetricClouds,
         ComputeNoiseSamplersBindTheirDeclaredHlslNames) {
    const std::string source = ReadDiligentPipelineSource();
    EXPECT_TRUE(!source.empty());

    // Compute shaders disable combined samplers to preserve UAV reflection.
    // The immutable sampler must therefore target the actual `foo_sampler`
    // declaration, while the SRV remains bound by its independent `foo` name.
    EXPECT_TRUE(Contains(source, "constexpr char suffix[] = \"_sampler\";"));
    EXPECT_TRUE(Contains(
        source, "samplers[i].SamplerOrTextureName = samplerNames[i];"));
    EXPECT_TRUE(!Contains(
        source,
        "samplers[i].SamplerOrTextureName = desc.srv_names[i] ? "
        "desc.srv_names[i]"));
}

ACS_TEST(VolumetricClouds,
         NativeResolutionResolveKeepsTheCenterColorAndDepthSamples) {
    const std::string source = ReadSkySource();
    const std::string resolveShader = CompactShader(
        ExtractRawShader(source, "const char* kCloudResolveCS"));
    EXPECT_TRUE(!resolveShader.empty());
    EXPECT_FALSE(Contains(source, "kCloudDepthResolvePS"));
    EXPECT_FALSE(Contains(source, "m_DepthResolvePipe"));

    // Keep the generic native trace path correct for future explicit policies.
    // A 3x3 reconstruction at that scale turns one native texel into a soft dot
    // and mixes cloud depth across silhouettes, so color and depth must use the
    // same refC/refD center sample.
    EXPECT_TRUE(Contains(resolveShader, "boolnativeMarch="));
    EXPECT_TRUE(Contains(resolveShader, "dims.x+0.5>=dims.z"));
    EXPECT_TRUE(Contains(resolveShader, "dims.y+0.5>=dims.w"));

    EXPECT_TRUE(Contains(
        resolveShader, "boolexactCurrent=nativeMarch||scheduled;"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "floatcurA=exactCurrent?saturate(refC.a):"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "float3curPremul=exactCurrent?refC.rgb*refC.a:"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "floatcurDepth=exactCurrent?"
        "((refC.a>0.003&&refD.x<=250000.0)?refD.x:250001.0):"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "float2nativeDepth=float2("
        "(refC.a>0.003&&refD.x<=250000.0)?refD.x:250001.0,"
        "saturate(refC.a));"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "float2resolvedDepth=float2(250001.0,0.0);"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "resolvedDepth=nativeMarch?nativeDepth:float2(curDepth,curA);"));
    EXPECT_TRUE(Contains(
        resolveShader, "historyDepthOut[tid.xy]=resolvedDepth;"));
}

ACS_TEST(VolumetricClouds,
         ResolveUsesOneComputeDispatchAndNeverReadsStalePhases) {
    const std::string source = ReadSkySource();
    const std::string compactSource = CompactShader(source);
    const std::string resolveShader = CompactShader(
        ExtractRawShader(source, "const char* kCloudResolveCS"));
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!resolveShader.empty());

    EXPECT_TRUE(Contains(
        resolveShader,
        "RWTexture2D<float4>historyColorOut:register(u0);"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "RWTexture2D<float2>historyDepthOut:register(u1);"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "[numthreads(8,8,1)]voidCSResolve(uint3tid:SV_DispatchThreadID){"
        "uintfullW=(uint)dims.z;uintfullH=(uint)dims.w;"
        "if(tid.x>=fullW||tid.y>=fullH)return;"
        "float2uv=(float2(tid.xy)+0.5)/dims.zw;"));
    EXPECT_TRUE(Contains(
        resolveShader, "historyColorOut[tid.xy]=float4("));
    EXPECT_TRUE(Contains(
        resolveShader, "historyDepthOut[tid.xy]=resolvedDepth;"));
    EXPECT_TRUE(Contains(
        compactSource,
        "ACS_COMPILE_CLOUD_SHADER(resolve,EShaderStage::Compute,"
        "kCloudResolveCS,\"CSResolve\",\"Clouds.ResolveCS\");"));
    EXPECT_TRUE(Contains(
        compactSource,
        "ACS_CREATE_CLOUD_SHADER(resolve,EShaderStage::Compute,"
        "kCloudResolveCS,\"CSResolve\",\"Clouds.ResolveCS\");"));
    EXPECT_TRUE(Contains(
        compactSource, "m_ResolveCs=Move(shaders.resolve);"));
    EXPECT_TRUE(Contains(compactSource, "pd.cs=m_ResolveCs.Get();"));
    EXPECT_TRUE(Contains(
        compactSource,
        "pd.uav_slots=2;pd.uav_names[0]=\"historyColorOut\";"
        "pd.uav_names[1]=\"historyDepthOut\";"));
    EXPECT_FALSE(Contains(source, "kCloudDepthResolvePS"));
    EXPECT_FALSE(Contains(source, "m_DepthResolvePs"));
    EXPECT_FALSE(Contains(source, "m_DepthResolvePipe"));
    EXPECT_FALSE(Contains(source, "m_ResolvePs"));

    const std::size_t renderBegin = compactSource.find(
        "voidCVolumetricClouds::RenderCompute(");
    const std::size_t renderEnd = compactSource.find(
        "voidCVolumetricClouds::Composite(", renderBegin);
    EXPECT_TRUE(renderBegin != std::string::npos);
    EXPECT_TRUE(renderEnd != std::string::npos);
    if (renderBegin != std::string::npos &&
        renderEnd != std::string::npos) {
        const std::string render = compactSource.substr(
            renderBegin, renderEnd - renderBegin);
        EXPECT_TRUE(Contains(
            render,
            "cl.SetComputePipeline(*m_ResolvePipe);"));
        EXPECT_TRUE(Contains(
            render,
            "cl.SetTexture(2,*m_HistoryColor[prev]);"
            "cl.SetTexture(3,*m_HistoryDepth[prev]);"
            "cl.BindUav(0,*m_HistoryColor[cur]);"
            "cl.BindUav(1,*m_HistoryDepth[cur]);"
            "cl.Dispatch((m_FullW+7u)/8u,(m_FullH+7u)/8u,1);"));
        const std::size_t resolveBegin = render.find(
            "constu32cur=recorded.frame_index&1u;");
        const std::size_t resolveEnd = render.find(
            "recorded.resolved_index=cur;", resolveBegin);
        EXPECT_TRUE(resolveBegin != std::string::npos);
        EXPECT_TRUE(resolveEnd != std::string::npos);
        if (resolveBegin != std::string::npos &&
            resolveEnd != std::string::npos) {
            const std::string resolveRuntime = render.substr(
                resolveBegin, resolveEnd - resolveBegin);
            EXPECT_EQ(
                CountOccurrences(resolveRuntime, "cl.Dispatch("),
                static_cast<std::size_t>(1u));
            EXPECT_FALSE(Contains(resolveRuntime, "cl.Draw("));
            EXPECT_FALSE(Contains(
                resolveRuntime, "BeginRenderToTexture"));
            EXPECT_FALSE(Contains(
                resolveRuntime, "EndRenderToTexture"));
        }
        EXPECT_EQ(
            CountOccurrences(render, "cl.Draw(3,0);"),
            static_cast<std::size_t>(0u));
    }

    EXPECT_TRUE(Contains(
        compactSource,
        "make_texture(fw,fh,EFormat::R16G16B16A16_Float,true,false,historyColor[0])"));
    EXPECT_TRUE(Contains(
        compactSource,
        "make_texture(fw,fh,EFormat::R32G32_Float,true,false,historyDepth[0])"));

    // The reduced trace is spatially complete. Phase filtering happens only
    // in full-resolution output space, so every low texture read is current.
    EXPECT_TRUE(Contains(
        resolveShader,
        "booltemporalSuperRes=IsTemporalSuperResolution();"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "boolscheduled=temporalSuperRes&&"
        "IsScheduledFullPixel(tid.xy,phaseOffset);"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "float4refC=cloudLow.SampleLevel(cloudLow_sampler,nearestUv,0);"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "float4c=cloudLow.SampleLevel(cloudLow_sampler,suv,0);"));
    EXPECT_FALSE(Contains(resolveShader, "IsScheduledQ("));
    EXPECT_FALSE(Contains(resolveShader, "InterleaveReferenceQ("));

    // The 8x8 launch rounds odd sizes up, while the shader guard writes every
    // valid full-resolution pixel exactly once and never writes padding lanes.
    u32 coverageFailures = 0u;
    u32 paddingWrites = 0u;
    for (u32 width = 1u; width <= 17u; ++width) {
        for (u32 height = 1u; height <= 17u; ++height) {
            u32 visits[17 * 17]{};
            const u32 launchWidth = ((width + 7u) / 8u) * 8u;
            const u32 launchHeight = ((height + 7u) / 8u) * 8u;
            for (u32 y = 0u; y < launchHeight; ++y) {
                for (u32 x = 0u; x < launchWidth; ++x) {
                    if (x >= width || y >= height) continue;
                    if (x >= 17u || y >= 17u) {
                        ++paddingWrites;
                        continue;
                    }
                    ++visits[y * 17u + x];
                }
            }
            for (u32 y = 0u; y < height; ++y) {
                for (u32 x = 0u; x < width; ++x) {
                    if (visits[y * 17u + x] != 1u) {
                        ++coverageFailures;
                    }
                }
            }
        }
    }
    EXPECT_EQ(coverageFailures, static_cast<u32>(0u));
    EXPECT_EQ(paddingWrites, static_cast<u32>(0u));
}

#if 0  // Superseded by the spatially complete 4x4 temporal-super-resolution contract below.
ACS_TEST(VolumetricClouds,
         DirectInterleaveReferenceMatchesLegacyThreeByThreeSearch) {
    u32 compared = 0u;
    u32 mismatched = 0u;
    u32 invalid = 0u;
    u32 phaseContractFailures = 0u;
    for (i32 width = 2; width <= 17; ++width) {
        for (i32 height = 2; height <= 17; ++height) {
            for (u32 scheduledPhase = 0u;
                 scheduledPhase < 4u;
                 ++scheduledPhase) {
                for (i32 y = 0; y < height; ++y) {
                    for (i32 x = 0; x < width; ++x) {
                        const f32 xf = static_cast<f32>(x);
                        const f32 yf = static_cast<f32>(y);
                        const f32 lowXs[]{
                            xf - 0.49f,
                            xf - 0.25f,
                            std::nextafter(
                                xf,
                                -std::numeric_limits<f32>::infinity()),
                            xf,
                            std::nextafter(
                                xf,
                                std::numeric_limits<f32>::infinity()),
                            xf + 0.25f,
                            xf + 0.49f};
                        const f32 lowYs[]{
                            yf - 0.49f,
                            yf - 0.25f,
                            std::nextafter(
                                yf,
                                -std::numeric_limits<f32>::infinity()),
                            yf,
                            std::nextafter(
                                yf,
                                std::numeric_limits<f32>::infinity()),
                            yf + 0.25f,
                            yf + 0.49f};
                        for (const f32 lowY : lowYs) {
                            for (const f32 lowX : lowXs) {
                                const FCloudReferenceCoordForTest legacy =
                                    LegacyCloudReferenceForTest(
                                        x, y, lowX, lowY,
                                        width, height,
                                        scheduledPhase);
                                const FCloudReferenceCoordForTest direct =
                                    DirectCloudReferenceForTest(
                                        x, y, lowX, lowY,
                                        width, height,
                                        scheduledPhase);
                                if (direct.x != legacy.x ||
                                    direct.y != legacy.y) {
                                    ++mismatched;
                                }
                                if (direct.x < 0 ||
                                    direct.x >= width ||
                                    direct.y < 0 ||
                                    direct.y >= height ||
                                    CloudReferenceConfidenceForTest(
                                        direct.x, direct.y,
                                        scheduledPhase) < 0.0f) {
                                    ++invalid;
                                }
                                ++compared;
                            }
                        }
                        const bool centerSkipped =
                            CloudReferenceConfidenceForTest(
                                x, y, scheduledPhase) < 0.0f;
                        if (centerSkipped !=
                            (CloudPixelPhaseForTest(x, y) !=
                             scheduledPhase)) {
                            ++phaseContractFailures;
                        }
                    }
                }
            }
        }
    }
    EXPECT_EQ(compared, static_cast<u32>(4528384u));
    EXPECT_EQ(mismatched, static_cast<u32>(0u));
    EXPECT_EQ(invalid, static_cast<u32>(0u));
    EXPECT_EQ(phaseContractFailures, static_cast<u32>(0u));

    // Bind the exhaustive CPU proof above to the single compute resolve shader.
    // A call-site-only assertion would still pass if the helper drifted away
    // from the parity/boundary rule exercised by the CPU model.
    const std::string source = ReadSkySource();
    const std::string resolveShader = CompactShader(
        ExtractRawShader(source, "const char* kCloudResolveCS"));
    const char* exactCandidate =
        "voidConsiderInterleaveReference("
        "int2candidateQ,float2lowPos,"
        "inoutint2bestQ,inoutfloatbestDistance){"
        "float2candidateDelta=float2(candidateQ)-lowPos;"
        "floatcandidateDistance=dot(candidateDelta,candidateDelta);"
        "if(candidateDistance<bestDistance){"
        "bestDistance=candidateDistance;"
        "bestQ=candidateQ;}}";
    const char* exactHelper =
        "int2InterleaveReferenceQ(int2centerQ,float2lowPos){"
        "uintscheduledPhase=(uint)temporal.z&3u;"
        "intscheduledX=(int)(scheduledPhase&1u);"
        "intscheduledY=(int)((scheduledPhase>>1u)&1u);"
        "int2maxQ=int2(dims.xy)-1;"
        "int2q0=centerQ;"
        "int2q1=centerQ;"
        "if((centerQ.x&1)!=scheduledX){"
        "q0.x=centerQ.x>0?centerQ.x-1:centerQ.x+1;"
        "q1.x=centerQ.x<maxQ.x?centerQ.x+1:q0.x;}"
        "if((centerQ.y&1)!=scheduledY){"
        "q0.y=centerQ.y>0?centerQ.y-1:centerQ.y+1;"
        "q1.y=centerQ.y<maxQ.y?centerQ.y+1:q0.y;}"
        "int2bestQ=centerQ;"
        "floatbestDistance=1e9;"
        "ConsiderInterleaveReference(q0,lowPos,bestQ,bestDistance);"
        "ConsiderInterleaveReference("
        "int2(q1.x,q0.y),lowPos,bestQ,bestDistance);"
        "ConsiderInterleaveReference("
        "int2(q0.x,q1.y),lowPos,bestQ,bestDistance);"
        "ConsiderInterleaveReference(q1,lowPos,bestQ,bestDistance);"
        "returnclamp(bestQ,int2(0,0),maxQ);}";
    EXPECT_TRUE(Contains(resolveShader, exactCandidate));
    EXPECT_TRUE(Contains(resolveShader, exactHelper));
    EXPECT_EQ(
        CountOccurrences(resolveShader, exactCandidate),
        static_cast<std::size_t>(1u));
    EXPECT_EQ(
        CountOccurrences(resolveShader, exactHelper),
        static_cast<std::size_t>(1u));
}

ACS_TEST(VolumetricClouds,
         CompactInterleaveDispatchExactlyCoversEveryScheduledPhase) {
    u32 outOfBounds = 0u;
    u32 coverageFailures = 0u;
    u32 unionFailures = 0u;
    u32 cases = 0u;
    for (i32 width = 2; width <= 17; ++width) {
        for (i32 height = 2; height <= 17; ++height) {
            u32 unionVisits[17 * 17]{};
            for (u32 phase = 0u; phase < 4u; ++phase) {
                u32 phaseVisits[17 * 17]{};
                const i32 phaseX = static_cast<i32>(phase & 1u);
                const i32 phaseY =
                    static_cast<i32>((phase >> 1u) & 1u);
                const i32 compactWidth =
                    (width + 1 - phaseX) / 2;
                const i32 compactHeight =
                    (height + 1 - phaseY) / 2;
                for (i32 compactY = 0;
                     compactY < compactHeight;
                     ++compactY) {
                    for (i32 compactX = 0;
                         compactX < compactWidth;
                         ++compactX) {
                        const i32 x = compactX * 2 + phaseX;
                        const i32 y = compactY * 2 + phaseY;
                        if (x < 0 || x >= width ||
                            y < 0 || y >= height) {
                            ++outOfBounds;
                            continue;
                        }
                        ++phaseVisits[y * 17 + x];
                        ++unionVisits[y * 17 + x];
                    }
                }
                for (i32 y = 0; y < height; ++y) {
                    for (i32 x = 0; x < width; ++x) {
                        const u32 expected =
                            CloudPixelPhaseForTest(x, y) == phase ? 1u : 0u;
                        if (phaseVisits[y * 17 + x] != expected) {
                            ++coverageFailures;
                        }
                    }
                }
                ++cases;
            }
            for (i32 y = 0; y < height; ++y) {
                for (i32 x = 0; x < width; ++x) {
                    if (unionVisits[y * 17 + x] != 1u) {
                        ++unionFailures;
                    }
                }
            }
        }
    }
    EXPECT_EQ(cases, static_cast<u32>(1024u));
    EXPECT_EQ(outOfBounds, static_cast<u32>(0u));
    EXPECT_EQ(coverageFailures, static_cast<u32>(0u));
    EXPECT_EQ(unionFailures, static_cast<u32>(0u));
}

ACS_TEST(VolumetricClouds,
         PhaseFilteredGatherPreservesLegacyClampDuplicatesAndFallback) {
    u32 sequenceFailures = 0u;
    u32 fallbackFailures = 0u;
    u32 unscheduledReads = 0u;
    for (i32 width = 2; width <= 17; ++width) {
        for (i32 height = 2; height <= 17; ++height) {
            for (u32 phase = 0u; phase < 4u; ++phase) {
                const i32 invalidX = static_cast<i32>(phase & 1u);
                const i32 invalidY =
                    static_cast<i32>((phase >> 1u) & 1u);
                for (i32 centerY = 0; centerY < height; ++centerY) {
                    for (i32 centerX = 0; centerX < width; ++centerX) {
                        i32 legacySequence[9]{};
                        i32 filteredSequence[9]{};
                        u32 legacyCount = 0u;
                        u32 filteredCount = 0u;
                        FCloudReferenceCoordForTest legacyFallback{};
                        FCloudReferenceCoordForTest filteredFallback{};
                        f32 legacyDistance =
                            std::numeric_limits<f32>::max();
                        f32 filteredDistance =
                            std::numeric_limits<f32>::max();
                        for (i32 oy = -1; oy <= 1; ++oy) {
                            for (i32 ox = -1; ox <= 1; ++ox) {
                                const i32 x = ClampCloudCoordForTest(
                                    centerX + ox, width);
                                const i32 y = ClampCloudCoordForTest(
                                    centerY + oy, height);
                                const bool scheduled =
                                    CloudPixelPhaseForTest(x, y) == phase;
                                const i32 packed = y * 17 + x;
                                // Legacy compact march wrote -1 sentinels to
                                // every other phase before either texture read.
                                if (scheduled) {
                                    legacySequence[legacyCount++] = packed;
                                }
                                // New persistent UAV texels may contain valid
                                // stale data, so the coordinate phase must be
                                // rejected before inspecting confidence.
                                if (scheduled) {
                                    filteredSequence[filteredCount++] = packed;
                                }

                                const bool intentionallyInvalid =
                                    x == invalidX && y == invalidY;
                                const f32 dx = static_cast<f32>(x - centerX);
                                const f32 dy = static_cast<f32>(y - centerY);
                                const f32 distance = dx * dx + dy * dy;
                                const bool legacyValid =
                                    scheduled && !intentionallyInvalid;
                                if (legacyValid && distance < legacyDistance) {
                                    legacyDistance = distance;
                                    legacyFallback =
                                        FCloudReferenceCoordForTest{x, y};
                                }
                                if (!scheduled) {
                                    // This is where an implementation that
                                    // sampled first would observe stale +valid
                                    // confidence. The model intentionally does
                                    // not perform that read.
                                    continue;
                                }
                                if (!intentionallyInvalid &&
                                    distance < filteredDistance) {
                                    filteredDistance = distance;
                                    filteredFallback =
                                        FCloudReferenceCoordForTest{x, y};
                                }
                            }
                        }
                        if (legacyCount != filteredCount) {
                            ++sequenceFailures;
                        } else {
                            for (u32 i = 0u; i < legacyCount; ++i) {
                                if (legacySequence[i] != filteredSequence[i]) {
                                    ++sequenceFailures;
                                    break;
                                }
                            }
                        }
                        if (legacyFallback.x != filteredFallback.x ||
                            legacyFallback.y != filteredFallback.y) {
                            ++fallbackFailures;
                        }
                    }
                }
            }
        }
    }
    EXPECT_EQ(sequenceFailures, static_cast<u32>(0u));
    EXPECT_EQ(fallbackFailures, static_cast<u32>(0u));
    EXPECT_EQ(unscheduledReads, static_cast<u32>(0u));
}
#endif

ACS_TEST(VolumetricClouds,
         TemporalSuperResolutionPhasesExactlyCoverEveryFullPixel) {
    constexpr i32 kOffsets[16][2]{
        {0,0},{2,2},{2,0},{0,2},{1,1},{3,3},{3,1},{1,3},
        {1,0},{3,2},{3,0},{1,2},{0,1},{2,3},{2,1},{0,3}};
    const auto scrambledPhase = [](u32 blockX, u32 blockY,
                                   u32 phaseIndex) noexcept {
        u32 blockHash = blockX * 0x8da6b343u ^ blockY * 0xd8163841u;
        blockHash ^= blockHash >> 16u;
        blockHash *= 0x7feb352du;
        blockHash ^= blockHash >> 15u;
        return (phaseIndex + (blockHash & 15u)) & 15u;
    };
    u32 coverageFailures = 0u;
    u32 mappingFailures = 0u;
    for (i32 width = 1; width <= 33; ++width) {
        for (i32 height = 1; height <= 33; ++height) {
            const i32 lowWidth = (width + 3) / 4;
            const i32 lowHeight = (height + 3) / 4;
            u32 visits[33 * 33]{};
            for (u32 phase = 0u; phase < 16u; ++phase) {
                for (i32 qy = 0; qy < lowHeight; ++qy) {
                    for (i32 qx = 0; qx < lowWidth; ++qx) {
                        const u32 blockPhase = scrambledPhase(
                            static_cast<u32>(qx), static_cast<u32>(qy), phase);
                        const i32 x = qx * 4 + kOffsets[blockPhase][0];
                        const i32 y = qy * 4 + kOffsets[blockPhase][1];
                        if (x < width && y < height) {
                            ++visits[y * 33 + x];
                            if (x / 4 != qx || y / 4 != qy) {
                                ++mappingFailures;
                            }
                        }
                    }
                }
            }
            for (i32 y = 0; y < height; ++y) {
                for (i32 x = 0; x < width; ++x) {
                    if (visits[y * 33 + x] != 1u) ++coverageFailures;
                    u32 matchingPhases = 0u;
                    for (u32 phase = 0u; phase < 16u; ++phase) {
                        const u32 blockPhase = scrambledPhase(
                            static_cast<u32>(x / 4),
                            static_cast<u32>(y / 4), phase);
                        if (kOffsets[blockPhase][0] == (x & 3) &&
                            kOffsets[blockPhase][1] == (y & 3)) {
                            ++matchingPhases;
                        }
                    }
                    if (matchingPhases != 1u) ++mappingFailures;
                }
            }
        }
    }
    EXPECT_EQ(coverageFailures, static_cast<u32>(0u));
    EXPECT_EQ(mappingFailures, static_cast<u32>(0u));

    // One global phase must not select the same offset in every block. The
    // deterministic scramble distributes all history ages while each block's
    // sixteen-frame schedule remains a bijection over the same sample set.
    u32 phaseUse[16]{};
    for (u32 blockY = 0u; blockY < 32u; ++blockY) {
        for (u32 blockX = 0u; blockX < 32u; ++blockX) {
            ++phaseUse[scrambledPhase(blockX, blockY, 0u)];
        }
    }
    for (const u32 uses : phaseUse) EXPECT_TRUE(uses > 0u);
}

ACS_TEST(VolumetricClouds,
         TemporalSuperResolutionUsesBayerPhasesAndCompleteLowDispatch) {
    const std::string source = ReadSkySource();
    const std::string compactSource = CompactShader(source);
    const std::string marchShader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    const std::string resolveShader = CompactShader(
        ExtractRawShader(source, "const char* kCloudResolveCS"));
    const char* bayer =
        "uint2(0,0),uint2(2,2),uint2(2,0),uint2(0,2),"
        "uint2(1,1),uint2(3,3),uint2(3,1),uint2(1,3),"
        "uint2(1,0),uint2(3,2),uint2(3,0),uint2(1,2),"
        "uint2(0,1),uint2(2,3),uint2(2,1),uint2(0,3)";
    EXPECT_TRUE(Contains(marchShader, bayer));
    EXPECT_TRUE(Contains(resolveShader, bayer));
    EXPECT_TRUE(Contains(
        marchShader,
        "uint2rayPixel=pixelQ;float2rayDimensions=dims.xy;"
        "if(temporal.w>3.5)"));
    EXPECT_TRUE(Contains(
        marchShader,
        "uint2phaseOffset=CloudTemporalPhaseOffset4(pixelQ,scheduledPhase);"
        "rayPixel=min(pixelQ*4u+phaseOffset,uint2(dims.zw)-1u);"));
    const char* blockScramble =
        "uintblockHash=blockQ.x*0x8da6b343u^blockQ.y*0xd8163841u;"
        "blockHash^=blockHash>>16u;blockHash*=0x7feb352du;"
        "blockHash^=blockHash>>15u;"
        "return(phaseIndex+(blockHash&15u))&15u;";
    EXPECT_TRUE(Contains(marchShader, blockScramble));
    EXPECT_TRUE(Contains(resolveShader, blockScramble));
    EXPECT_TRUE(Contains(
        resolveShader,
        "uint2pixelBlock=min(tid.xy>>2u,uint2(dims.xy)-1u);"
        "uint2phaseOffset=CloudTemporalPhaseOffset4(pixelBlock,phaseIndex);"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "uint2phaseOffset=CloudTemporalPhaseOffset4(uint2(q),phaseIndex);"));
    EXPECT_FALSE(Contains(
        marchShader, "CloudTemporalPhaseOffset4(scheduledPhase)"));
    EXPECT_FALSE(Contains(resolveShader, "uint2TemporalPhaseOffset4()"));
    EXPECT_TRUE(Contains(
        compactSource,
        "cl.Dispatch((m_W+7u)/8u,(m_H+7u)/8u,1);"));
    EXPECT_TRUE(Contains(
        compactSource, "if(!historyValid)recorded.temporal_phase=0u;"));
    EXPECT_TRUE(Contains(
        compactSource,
        "recorded.temporal_phase=(recorded.temporal_phase+1u)&15u;"));
    EXPECT_TRUE(Contains(
        compactSource,
        "m_TemporalPhase=recorded.temporal_phase;"));
}

ACS_TEST(VolumetricClouds,
         UltraCurrentTraceWorkIsConstantAcrossHistoryAndCameraMotion) {
    const std::string source = ReadSkySource();
    const std::string compactSource = CompactShader(source);
    const std::string marchShader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    const std::string resolveShader = CompactShader(
        ExtractRawShader(source, "const char* kCloudResolveCS"));
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!marchShader.empty());
    EXPECT_TRUE(!resolveShader.empty());

    // Ultra is an output-quality policy. Its expensive current ray march is
    // quarter-dimension on camera cuts, while moving, and while stationary;
    // temporal history may improve reconstruction but never changes dispatch
    // dimensions or requests a native/full seed.
    EXPECT_TRUE(Contains(
        source,
        "ResolveVolumetricCloudTraceResolution(fw, fh, "
        "render_scale, reference_mode)"));
    EXPECT_TRUE(Contains(
        compactSource,
        "constu32hw=traceResolution.width;"
        "constu32hh=traceResolution.height;"));
    EXPECT_TRUE(Contains(
        compactSource,
        "constu32temporalFrame=(recorded.frame_index&4080u)|"
        "(recorded.temporal_phase&15u);"));
    EXPECT_TRUE(Contains(
        compactSource,
        "cb.temporal=FVec4{"
        "(historyValid&&!m_ReferenceMode)?1.0f:0.0f,m_PrevWindOffset,"
        "static_cast<f32>(temporalFrame),"));
    EXPECT_TRUE(Contains(
        compactSource,
        "cl.Dispatch((m_W+7u)/8u,(m_H+7u)/8u,1);"));
    EXPECT_FALSE(Contains(source, "const bool interleaveStable"));
    EXPECT_FALSE(Contains(source, "const bool temporalInterleave"));
    EXPECT_FALSE(Contains(compactSource, "u32marchW=m_W;"));
    EXPECT_FALSE(Contains(compactSource, "u32marchH=m_H;"));
    EXPECT_FALSE(Contains(compactSource, "if(temporalInterleave){"));

    // Camera cuts can still invalidate color history, but invalidation no
    // longer selects a different current-trace workload.
    EXPECT_TRUE(Contains(
        source, "if (VolumetricCloudViewCutDetected("));
    EXPECT_TRUE(Contains(marchShader, "uint2pixelQ=tid.xy;"));
    EXPECT_TRUE(Contains(marchShader, "cloudOut[pixelQ]="));
    EXPECT_TRUE(Contains(marchShader, "cloudDepthOut[pixelQ]="));
    EXPECT_TRUE(Contains(resolveShader, "floatgatheredA=saturate(alphaSum/max(weightSum,1e-5));"));
    EXPECT_TRUE(Contains(resolveShader, "floatcurA=exactCurrent?saturate(refC.a):gatheredA;"));
}

ACS_TEST(VolumetricClouds,
         DetailErosionShapesTheProfileAndWeatherSurface) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    const std::size_t profile = shader.find(
        "float4cloudDensityDistributionFromPositiveWeatherMacro(");
    const std::size_t commonPotential = shader.find(
        "floatcommonPotential=cloudCondensationCommonPotential("
        "macro.heightProfile,weatherMask,"
        "h,macro.toweringStrength);", profile);
    const std::size_t maximumPotential = shader.find(
        "floatmaximumBasePotential=maximumShapePotential+commonPotential;",
        commonPotential);
    const std::size_t maximumDensity = shader.find(
        "floatmaximumDetailedDensity=cloudDetailFilteredDensityUpperBound("
        "maximumBasePotential,billowVisibility,erosionVisibility);",
        maximumPotential);
    const std::size_t densityScale = shader.find(
        "floatdensityScale=cloudHeightPrecipitationDensityScale("
        "h,macro.weather.b);", maximumDensity);
    const std::size_t conservativeEnvelope = shader.find(
        "if(maximumDetailedDensity*densityScale>0.0){", densityScale);
    const std::size_t billow = shader.find(
        "billowPotentials=cloudBillowPotentialStates(ndA,ndB,h);",
        conservativeEnvelope);
    const std::size_t erosion = shader.find(
        "erosionPotential=cloudErosionPotentialOffset(ndA,ndB,h);",
        billow);
    const std::size_t finalDensity = shader.find(
        "densityDistribution=cloudScaleDensityDistribution("
        "cloudDetailFilteredCondensationDistribution("
        "macro.shapePotential,macro.shapeFrequencyWeights,"
        "commonPotential,billowPotentials,"
        "billowVisibility,middleBillowVisibility,"
        "erosionPotential,erosionVisibility),densityScale);", erosion);
    EXPECT_TRUE(profile != std::string::npos);
    EXPECT_TRUE(commonPotential != std::string::npos);
    EXPECT_TRUE(maximumPotential != std::string::npos);
    EXPECT_TRUE(maximumDensity != std::string::npos);
    EXPECT_TRUE(densityScale != std::string::npos);
    EXPECT_TRUE(conservativeEnvelope != std::string::npos);
    EXPECT_TRUE(billow != std::string::npos);
    EXPECT_TRUE(erosion != std::string::npos);
    EXPECT_TRUE(finalDensity != std::string::npos);
    EXPECT_TRUE(profile < commonPotential);
    EXPECT_TRUE(commonPotential < maximumPotential);
    EXPECT_TRUE(maximumPotential < maximumDensity);
    EXPECT_TRUE(maximumDensity < densityScale);
    EXPECT_TRUE(densityScale < conservativeEnvelope);
    EXPECT_TRUE(conservativeEnvelope < billow);
    EXPECT_TRUE(billow < erosion);
    EXPECT_TRUE(erosion < finalDensity);
    const std::string densityFunction = shader.substr(
        profile, finalDensity - profile);
    EXPECT_FALSE(Contains(densityFunction, "remapc("));
    EXPECT_FALSE(Contains(densityFunction, "saturate(commonPotential)"));

    // 交換した二領域の差は符号だけが反転する。詳細をLODで0へ戻しても
    // 深い凝結域の平均ポテンシャルと平均密度は変わらない。
    constexpr f32 basePotential = 0.40f;
    constexpr f32 positiveOffset = 0.10f;
    const f32 positiveDensity =
        render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
            basePotential + positiveOffset);
    const f32 negativeDensity =
        render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
            basePotential - positiveOffset);
    const f32 baseDensity =
        render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
            basePotential);
    EXPECT_NEAR(
        (positiveDensity + negativeDensity) * 0.5f,
        baseDensity, 1.0e-6f);
    EXPECT_TRUE(positiveDensity > baseDensity);
    EXPECT_TRUE(negativeDensity < baseDensity);
}

ACS_TEST(VolumetricClouds,
         BaseShapeKeepsPointDensitySeparateFromOccupancyMaximum) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    const std::size_t pointShapeBegin = shader.find(
        "float4cloudShapePotentialBands(float3uvw){");
    const std::size_t occupancyMaximumBegin = shader.find(
        "floatcloudShapeOccupancyMaximum(", pointShapeBegin);
    const std::size_t occupancyBegin = shader.find(
        "float2cloudShapeOccupancyAtInterval(", occupancyMaximumBegin);
    const std::size_t macroBegin = shader.find(
        "structCloudMacroSample", occupancyBegin);
    EXPECT_TRUE(pointShapeBegin != std::string::npos);
    EXPECT_TRUE(occupancyMaximumBegin != std::string::npos);
    EXPECT_TRUE(occupancyBegin != std::string::npos);
    EXPECT_TRUE(macroBegin != std::string::npos);
    EXPECT_TRUE(pointShapeBegin < occupancyMaximumBegin);
    EXPECT_TRUE(occupancyMaximumBegin < occupancyBegin);
    EXPECT_TRUE(occupancyBegin < macroBegin);
    if (pointShapeBegin != std::string::npos &&
        occupancyMaximumBegin != std::string::npos &&
        occupancyBegin != std::string::npos &&
        macroBegin != std::string::npos) {
        const std::string pointShape = shader.substr(
            pointShapeBegin, occupancyMaximumBegin - pointShapeBegin);
        const std::string occupancyMaximum = shader.substr(
            occupancyMaximumBegin, occupancyBegin - occupancyMaximumBegin);
        const std::string occupancyShape = shader.substr(
            occupancyBegin, macroBegin - occupancyBegin);
        EXPECT_TRUE(Contains(
            pointShape,
            "float4densityBands=shapeNoise.SampleLevel("
            "shapeNoise_sampler,uvw,0);"));
        EXPECT_TRUE(Contains(
            occupancyMaximum,
            "float4occupancyBands=shapeOccupancy.SampleLevel("
            "shapeOccupancy_sampler,uvw,0);"));
        EXPECT_TRUE(Contains(
            pointShape,
            "cloudSignedPotentialFromStored(densityBands.r)"));
        EXPECT_FALSE(Contains(pointShape, "shapeOccupancy.SampleLevel("));
        EXPECT_FALSE(Contains(occupancyMaximum, "shapeNoise.SampleLevel("));
        EXPECT_TRUE(Contains(
            occupancyShape,
            "floatmaximumShapePotential=cloudShapeOccupancyMaximum("
            "cloudUVW(p,layerHeight,upperBand),maximumDomainFootprint);"));
        EXPECT_TRUE(Contains(
            occupancyShape,
            "floatunresolvedDensityUpper=cloudCondensationDensity("
            "CLOUD_UNRESOLVED_COARSE_OUTER_POTENTIAL"
            "+maximumPositiveOffset);"));
        EXPECT_TRUE(Contains(
            occupancyShape,
            "floatdensityUpper=pointOwner*pointDensityUpper"
            "+max(frequencyWeights.w,0.0)*unresolvedDensityUpper;"));
        EXPECT_FALSE(Contains(occupancyShape, "shapeNoise.SampleLevel("));
    }
    EXPECT_FALSE(Contains(shader, "cloudBaseShapeBand("));
    EXPECT_FALSE(Contains(shader, "cloudShapeErosionBand("));
    EXPECT_FALSE(Contains(shader, "cloudDensityFromShapeErosion("));
    EXPECT_FALSE(Contains(shader, "floatboundarySupport="));
    EXPECT_FALSE(Contains(shader, "rejectionThreshold"));
    EXPECT_FALSE(Contains(shader, "pointCanReachDensity"));
    EXPECT_FALSE(Contains(shader, "footprintContainsSupport"));
    EXPECT_FALSE(Contains(shader, "cloudStoredBaseNoise("));

    // 階層形状は未凝結側の負値を含めて一度だけ確定する。
    const f32 positivePotential =
        CloudHierarchicalShapeForTest(
            0.72f, 0.60f, 0.55f, 0.60f, 0.40f);
    const f32 negativePotential =
        CloudHierarchicalShapeForTest(
            0.20f, 0.50f, 0.50f, 0.50f, 0.50f);
    EXPECT_TRUE(positivePotential > 0.0f);
    EXPECT_TRUE(negativePotential < 0.0f);
    EXPECT_TRUE(negativePotential >= -1.0f);

    // 最大3D湿度でも天候包絡が晴天なら、加算場の正値化後は空のままになる。
    const f32 clearPotential =
        render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
            1.0f, 0.0f, 1.0f, 0.0f);
    EXPECT_NEAR(
        render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
            clearPotential),
        0.0f, 0.0f);
}

ACS_TEST(VolumetricClouds,
         WeatherCoverageParticipatesInDimensionalProfileOnce) {
    const f32 clearGate =
        render_internal::ResolveVolumetricCloudCondensationEnvelopeGate_Internal(
            0.0f);
    const f32 middleGate =
        render_internal::ResolveVolumetricCloudCondensationEnvelopeGate_Internal(
            0.5f);
    const f32 saturatedGate =
        render_internal::ResolveVolumetricCloudCondensationEnvelopeGate_Internal(
            1.0f);
    EXPECT_TRUE(clearGate < middleGate);
    EXPECT_TRUE(middleGate < saturatedGate);
    EXPECT_NEAR(saturatedGate, 0.0f, 0.0f);
    EXPECT_NEAR(
        middleGate,
        -0.25f *
            render_internal::kVolumetricCloudCondensationEnvelopeRejection,
        1.0e-6f);

    const f32 lowWeather =
        render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
            0.1f, 0.25f, 1.0f, 0.0f);
    const f32 highWeather =
        render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
            0.1f, 0.75f, 1.0f, 0.0f);
    EXPECT_TRUE(highWeather > lowWeather);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudCondensationEnvelopeGate(floatenvelope){"));
    EXPECT_TRUE(Contains(
        shader,
        "cloudCondensationEnvelopeGate(weatherMask)"
        "+cloudCondensationEnvelopeGate(verticalProfile)"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcommonPotential=cloudCondensationCommonPotential("
        "macro.heightProfile,weatherMask,"));
    EXPECT_TRUE(Contains(shader, "floatdensityScale=cloudHeightPrecipitationDensityScale(" "h,macro.weather.b);"));
    EXPECT_TRUE(Contains(
        shader,
        "cloudBandLimitedCondensationDistribution("
        "macro.shapePotential,macro.shapeFrequencyWeights,"));
    EXPECT_FALSE(Contains(shader, "cloudDimensionalProfile("));
    EXPECT_FALSE(Contains(shader, "cloudWeatherShapeErosion("));
    EXPECT_FALSE(Contains(shader, "cloudWeatheredBaseNoise("));
    EXPECT_FALSE(Contains(shader, "baseDensity*weatherMask"));
}

ACS_TEST(VolumetricClouds,
         VerticallyCoherentDetailBillowPreservesOccupancyBounds) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    EXPECT_TRUE(Contains(
        shader, "floatcloudBillowMaximumOffset(floatheight){"));
    EXPECT_TRUE(Contains(
        shader,
        "returnlerp(0.024,0.130,smoothstep(0.18,0.92,saturate(height)));"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudDetailMiddleBand(float2detailBands){"
        "returnsaturate((detailBands.g-detailBands.r*0.55)*(1.0/0.45));}"));
    EXPECT_TRUE(Contains(
        shader,
        "float2cloudBillowPotentialStates("
        "float2detailA,float2detailB,floatheight){"));
    EXPECT_TRUE(Contains(
        shader,
        "floattopMiddleWeight=0.48*"
        "smoothstep(0.38,0.90,saturate(height));"));
    EXPECT_TRUE(Contains(
        shader,
        "returnfloat2(coarseDifference,"
        "lerp(coarseDifference,middleDifference,topMiddleWeight))"
        "*maximumOffset;"));
    EXPECT_TRUE(Contains(
        shader,
        "float3rotatedPosition=horizontal+vertical;"
        "detailDomainA=rotatedPosition*0.00018;"
        "detailDomainB=rotatedPosition*0.00031;"));
    EXPECT_FALSE(Contains(
        shader, "detailDomainA=horizontal*0.0011+vertical*0.00055;"));
    EXPECT_FALSE(Contains(
        shader, "detailDomainB=horizontal*0.0023+vertical*0.00115;"));
    EXPECT_FALSE(Contains(shader, "cloudBillowBoundaryWeight("));
    EXPECT_FALSE(Contains(shader, "cloudBillowedBaseDensity("));
    EXPECT_TRUE(Contains(
        shader,
        "float2occupancySample=cloudShapeOccupancyAtInterval("
        "occupancyP,occupancyPhysicalFootprint);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatmaximumDetailedDensity=cloudDetailFilteredDensityUpperBound("
        "maximumBasePotential,billowVisibility,erosionVisibility);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatdensityScale=cloudHeightPrecipitationDensityScale("
        "h,macro.weather.b);"));
    EXPECT_TRUE(Contains(
        shader,
        "if(maximumDetailedDensity*densityScale>0.0){"));
    EXPECT_TRUE(Contains(
        shader,
        "billowPotentials=cloudBillowPotentialStates(ndA,ndB,h);"));

    const f32 topLimit = CloudBillowMaximumOffsetForTest(1.0f);
    const f32 baseLimit = CloudBillowMaximumOffsetForTest(0.0f);
    EXPECT_NEAR(baseLimit, 0.024f, 1.0e-6f);
    EXPECT_NEAR(topLimit, 0.130f, 1.0e-6f);
    EXPECT_TRUE(topLimit > baseLimit);

    // 正の密度は長い接線経路で可視になり得るため、局所しきい値で落とさない。
    constexpr f32 kThinEnvelopeDensity = 0.0008f;
    constexpr f32 kMaximumDensityScale = 1.10f * 1.28f;
    EXPECT_TRUE(kThinEnvelopeDensity < 0.001f);
    EXPECT_TRUE(kThinEnvelopeDensity * kMaximumDensityScale > 0.001f);

    const f32 expandedOffset = CloudBillowOffsetForTest(FVec2{1.0f, 1.0f}, FVec2{0.0f, 0.0f}, 1.0f, 1.0f);
    const f32 erodedOffset = CloudBillowOffsetForTest(FVec2{0.0f, 0.0f}, FVec2{1.0f, 1.0f}, 1.0f, 1.0f);
    const f32 unchangedOffset = CloudBillowOffsetForTest(FVec2{0.42f, 0.42f}, FVec2{0.42f, 0.42f}, 1.0f, 1.0f);
    EXPECT_NEAR(expandedOffset, topLimit, 1.0e-6f);
    EXPECT_NEAR(erodedOffset, -topLimit, 1.0e-6f);
    EXPECT_NEAR(unchangedOffset, 0.0f, 0.0f);
    EXPECT_NEAR(
        expandedOffset + erodedOffset, 0.0f, 1.0e-6f);

    EXPECT_NEAR(CloudDetailMiddleBandForTest(FVec2{0.40f, 0.5575f}), 0.75f, 1.0e-6f);
    const FVec2 risingTopA{0.80f, 0.95f};
    const FVec2 risingTopB{0.20f, 0.11f};
    const f32 coarseOnlyTop = CloudBillowOffsetForTest(risingTopA, risingTopB, 1.0f, 0.0f);
    const f32 detailedTop = CloudBillowOffsetForTest(risingTopA, risingTopB, 1.0f, 1.0f);
    EXPECT_TRUE(detailedTop > coarseOnlyTop);
    EXPECT_NEAR(CloudBillowOffsetForTest(risingTopA, risingTopB, 0.0f, 0.0f), CloudBillowOffsetForTest(risingTopA, risingTopB, 0.0f, 1.0f), 0.0f);

    // 房変位は符号付き場へ直接加わり、A/Bを交換すると厳密に逆方向へ動く。
    constexpr f32 boundaryPotential = 0.0f;
    EXPECT_NEAR(
        boundaryPotential + expandedOffset,
        topLimit, 1.0e-6f);
    EXPECT_NEAR(
        boundaryPotential + erodedOffset,
        -topLimit, 1.0e-6f);

    constexpr FVec2 kDetailSamples[]{FVec2{0.0f, 0.0f}, FVec2{1.0f, 0.55f}, FVec2{0.0f, 0.45f}, FVec2{1.0f, 1.0f}, FVec2{0.42f, 0.42f}, FVec2{0.70f, 0.65f}};
    constexpr f32 kMiddleVisibilities[]{0.0f, 0.5f, 1.0f};
    for (u32 heightStep = 0u; heightStep <= 10u; ++heightStep) {
        const f32 height = static_cast<f32>(heightStep) * 0.1f;
        const f32 limit = CloudBillowMaximumOffsetForTest(height);
        for (const f32 middleVisibility : kMiddleVisibilities) {
            for (const FVec2 detailA : kDetailSamples) {
                for (const FVec2 detailB : kDetailSamples) {
                    const f32 offset = CloudBillowOffsetForTest(detailA, detailB, height, middleVisibility);
                    EXPECT_TRUE(offset >= -limit - 1.0e-6f);
                    EXPECT_TRUE(offset <= limit + 1.0e-6f);
                    EXPECT_NEAR(offset + CloudBillowOffsetForTest(detailB, detailA, height, middleVisibility), 0.0f, 1.0e-6f);
                }
            }
        }
    }
}

ACS_TEST(VolumetricClouds,
         SignedCondensationPotentialUsesFixedRangeAndPreservesCoreVariation) {
    const f32 verticalProfiles[]{0.0f, 0.01f, 0.25f, 0.75f, 1.0f};
    const f32 weatherMasks[]{0.0f, 0.15f, 0.50f, 1.0f};
    for (const f32 verticalProfile : verticalProfiles) {
        for (const f32 weatherMask : weatherMasks) {
            f32 previousPotential = -100.0f;
            f32 previousDensity = 0.0f;
            for (u32 shapeStep = 0u; shapeStep <= 100u; ++shapeStep) {
                const f32 shapePotential =
                    static_cast<f32>(shapeStep) * 0.02f - 1.0f;
                const f32 potential =
                    render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
                        shapePotential, weatherMask,
                        verticalProfile, 0.0f);
                const f32 density =
                    render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
                        potential);
                EXPECT_TRUE(potential + 1.0e-6f >= previousPotential);
                EXPECT_TRUE(density + 1.0e-6f >= previousDensity);
                EXPECT_TRUE(density >= 0.0f);
                previousPotential = potential;
                previousDensity = density;
            }
        }
    }

    // 深い凝結域では正値化が線形なので、3D湿度差を1で平坦化しない。
    const f32 lowPotential =
        render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
            0.20f, 1.0f, 1.0f, 0.0f);
    const f32 highPotential =
        render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
            1.00f, 1.0f, 1.0f, 0.0f);
    const f32 lowDensity =
        render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
            lowPotential);
    const f32 highDensity =
        render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
            highPotential);
    EXPECT_NEAR(highDensity - lowDensity, 0.80f, 1.0e-6f);
    EXPECT_NEAR(highDensity, 1.0f, 1.0e-6f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_FALSE(Contains(shader, "cloudPositiveDensityNoiseThreshold("));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudSignedPotentialFromStored(floatstoredPotential){"));
    EXPECT_TRUE(Contains(
        shader,
        "signedPotential=saturate(storedPotential)*2.0-1.0;"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudCondensationPotential("
        "floatshapePotential,floatverticalProfile,floatweatherMask,"));
    EXPECT_TRUE(Contains(shader,
        "returnsafeShapePotential+cloudCondensationCommonPotential("
        "verticalProfile,weatherMask,height,toweringStrength);"));
    EXPECT_TRUE(Contains(shader, "floatcloudCondensationDensity(floatpotential){"));
    EXPECT_FALSE(Contains(shader, "cloudProfileCarvedDensity("));
    EXPECT_FALSE(Contains(shader, "cloudDimensionalProfile("));
    EXPECT_FALSE(Contains(shader, "cloudHeightThresholdFromTarget("));
    EXPECT_FALSE(Contains(shader, "cloudVerticalProfileShape("));
    EXPECT_FALSE(Contains(shader, "cloudProfileTailClosure"));
    EXPECT_FALSE(Contains(shader, "macro.profileWeight"));
    EXPECT_FALSE(Contains(shader, "*slowWeatherMask*profileThresholdWeight"));
}

ACS_TEST(VolumetricClouds,
         PointDensityDoesNotUseFootprintRejectionThreshold) {
    const f32 clearPotential =
        render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
            1.0f, 0.0f, 1.0f, 0.0f);
    EXPECT_NEAR(
        render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
            clearPotential),
        0.0f, 0.0f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_FALSE(Contains(shader, "cloudPositiveDensityNoiseThreshold("));
    EXPECT_FALSE(Contains(shader, "rejectionThreshold"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudMacroSamplesampleCloudMacroLighting("
        "float3p,floatweatherCoverage){"
        "returnsampleCloudMacroLightingBandLimited("
        "p,weatherCoverage,0.0.xxx,0.0.xxx);}"));
    EXPECT_TRUE(Contains(
        shader,
        "floatmaximumShapePotential=cloudShapeOccupancyMaximum("
        "cloudUVW(p,layerHeight,upperBand),maximumDomainFootprint);"));
    EXPECT_FALSE(Contains(
        shader,
        "cloudCondensationDensity("
        "macro.shapePotential,rejectionThreshold)"));
    EXPECT_FALSE(Contains(shader, "profile*sqrt(profile)"));
    EXPECT_FALSE(Contains(shader, "smoothstep(0.02,0.32,sampledProfile)"));
}

ACS_TEST(VolumetricClouds, HeightFractionUsesInitializedReturnWithoutChangingLayerSelection) {
    constexpr f32 kLowerBase = 1500.0f;
    constexpr f32 kLowerInverseThickness = 1.0f / 2500.0f;
    constexpr f32 kUpperBase = 6500.0f;
    constexpr f32 kUpperInverseThickness = 1.0f / 1800.0f;
    const f32 altitudes[]{0.0f, 1500.0f, 2750.0f, 4000.0f, 6500.0f, 7400.0f, 8300.0f, 12000.0f};

    for (const bool upperBand : {false, true}) {
        for (const f32 altitude : altitudes) {
            const f32 selectedExpression = upperBand
                ? (altitude - kUpperBase) * kUpperInverseThickness
                : (altitude - kLowerBase) * kLowerInverseThickness;
            EXPECT_NEAR(CloudHeightFractionFromAltitudeForTest(altitude, kLowerBase, kLowerInverseThickness, kUpperBase, kUpperInverseThickness, upperBand), SaturateForTest(selectedExpression), 0.0f);
        }
    }
}

ACS_TEST(VolumetricClouds,
         StormProfileUsesRawCloudTypeWithoutDoubleClassification) {
    constexpr f32 cloudType = 0.88f;
    constexpr f32 precipitation = 0.62f;
    const f32 rawTower =
        CloudToweringStrengthForTest(cloudType, precipitation);
    const f32 classifiedType =
        SmoothStepForTest(0.50f, 0.84f, cloudType);
    const f32 doubleClassifiedTower =
        CloudToweringStrengthForTest(classifiedType, precipitation);

    // 検証用の積乱雲設定でも段階的な成長を残し、雲種0.98で初めて上限へ達する。
    EXPECT_NEAR(rawTower, 0.671824f, 1.0e-5f);
    EXPECT_NEAR(doubleClassifiedTower, 1.0f, 0.0f);
    EXPECT_TRUE(doubleClassifiedTower > rawTower + 0.30f);
    EXPECT_NEAR(CloudStormProfileMixForTest(rawTower), 0.618078f, 1.0e-5f);
    EXPECT_NEAR(CloudStormProfileMixForTest(CloudToweringStrengthForTest(0.98f, precipitation)), 0.9082785f, 1.0e-6f);

    // 作者が全域を積乱雲へ寄せても、手続き天候場の下限と被覆中心だけが成熟する。
    const f32 weakPotentialTower = CloudLocalToweringStrengthForTest(1.0f, 0.85f, 0.0f, 1.0f);
    const f32 edgeTower = CloudLocalToweringStrengthForTest(1.0f, 0.85f, 1.0f, 0.0f);
    const f32 developingTower = CloudLocalToweringStrengthForTest(1.0f, 0.85f, 0.76f, 1.0f);
    const f32 matureTower = CloudLocalToweringStrengthForTest(1.0f, 0.85f, 1.0f, 1.0f);
    const f32 ordinaryTower = CloudLocalToweringStrengthForTest(0.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(weakPotentialTower, 0.4571895f, 1.0e-5f);
    EXPECT_NEAR(edgeTower, 0.0f, 0.0f);
    EXPECT_NEAR(developingTower, 0.4571895f, 1.0e-5f);
    EXPECT_NEAR(matureTower, 1.0f, 0.0f);
    EXPECT_NEAR(ordinaryTower, 0.0f, 0.0f);
    EXPECT_TRUE(developingTower > 0.10f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(Contains(shader, "floatcloudProfileFromTypeWeights(" "floath,float2typeWeights,floatcloudType,floattoweringStrength," "floatcolumnSpan,boolupperBand){"));
    EXPECT_TRUE(Contains(shader, "floatcloudLocalToweringStrength(float4weather,floatcloudInterior){"));
    EXPECT_TRUE(Contains(shader, "floatauthoredFloor=0.45*authoredTower;"));
    EXPECT_TRUE(Contains(shader, "floatbroadPotential=max(smoothstep(0.66,0.92,saturate(weather.a)),authoredFloor);"));
    EXPECT_TRUE(Contains(shader, "floatinteriorPotential=smoothstep(0.50,0.96,saturate(cloudInterior));"));
    EXPECT_FALSE(Contains(
        shader,
        "cloudToweringStrength(typeWeights.y,precipitation)*0.92"));
    EXPECT_TRUE(Contains(shader, "h,cloudProfileTypeWeights(cloudType),cloudType,toweringStrength," "columnSpan,upperBand);"));
}

ACS_TEST(VolumetricClouds,
         SignedThreeDimensionalFieldReplacesLocalColumnTopExtrusion) {
    // 同じ2D天候と高さでも3D湿度の符号が異なれば、高層の支持は局所的に分かれる。
    const f32 highLayerDry =
        render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
            -0.40f, 1.0f, 0.65f, 0.0f);
    const f32 highLayerHumid =
        render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
            0.40f, 1.0f, 0.65f, 0.0f);
    EXPECT_NEAR(
        render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
            highLayerDry),
        0.0f, 0.0f);
    EXPECT_TRUE(
        render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
            highLayerHumid) > 0.0f);

    // 湿潤包絡は3D場を正方向へ押し上げず、負の形状を柱として復活させない。
    const f32 lowLayerDry =
        render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
            -0.40f, 1.0f, 1.0f, 0.0f);
    const f32 lowLayerHumid =
        render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
            0.40f, 1.0f, 1.0f, 0.0f);
    EXPECT_NEAR(
        render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
            lowLayerDry),
        0.0f, 0.0f);
    EXPECT_TRUE(lowLayerHumid > lowLayerDry);

    const std::string shader = CompactShader(
        ExtractRawShader(ReadSkySource(), "const char* kCloudCS"));
    EXPECT_TRUE(Contains(shader,
        "macro.height=layerHeight;macro.columnSpan=1.0;"));
    EXPECT_TRUE(Contains(shader,
        "macro.shapePotential=cloudShapePotentialBands("));
    EXPECT_TRUE(Contains(shader,
        "macro.shapePotential,macro.shapeFrequencyWeights,"
        "commonPotential,billowPotentials,"
        "billowVisibility,middleBillowVisibility,"
        "erosionPotential,erosionVisibility"));
    EXPECT_FALSE(Contains(shader, "cloudColumnTopShift("));
    EXPECT_FALSE(Contains(shader, "cloudColumnTop("));
    EXPECT_FALSE(Contains(shader, "cloudColumnHeightAndSpan("));
    EXPECT_TRUE(Contains(shader, "floatcloudAnvilCoverageExpansion("));
    EXPECT_TRUE(Contains(shader,
        "floatcloudConvectiveWaistThresholdOffset("));
}

ACS_TEST(VolumetricClouds,
         CondensationBaseBiasReplacesColumnBaseLift) {
    constexpr f32 shapePotential = -0.50f;
    constexpr f32 weatherEnvelope = 1.0f;
    constexpr f32 heightEnvelope = 1.0f;
    const f32 withoutBase =
        render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
            shapePotential, weatherEnvelope, heightEnvelope, 0.0f);
    const f32 withBase =
        render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
            shapePotential, weatherEnvelope, heightEnvelope, 0.42f);
    EXPECT_NEAR(
        withBase - withoutBase,
        0.42f *
            render_internal::kVolumetricCloudCondensationBaseSupportScale,
        1.0e-6f);
    EXPECT_TRUE(withBase > withoutBase);

    const std::string shader = CompactShader(
        ExtractRawShader(ReadSkySource(), "const char* kCloudCS"));
    EXPECT_TRUE(Contains(shader,
        "floatbaseSupport=cloudCondensationBaseSupport("
        "height,weatherMask,toweringStrength);"));
    EXPECT_TRUE(Contains(shader,
        "+baseSupport*CLOUD_CONDENSATION_BASE_SUPPORT_SCALE;"));
    EXPECT_FALSE(Contains(shader, "cloudColumnBaseLift("));
    EXPECT_FALSE(Contains(shader, "floaterosion=lerp(0.10,0.24,"));
}

ACS_TEST(VolumetricClouds,
         WeatherCoverageGateDoesNotRescaleSignedShapePotential) {
    using namespace render_internal;
    constexpr f32 lowShapePotential = -0.30f;
    constexpr f32 highShapePotential = 0.30f;
    f32 previousLowPotential = -4.0f;
    for (u32 weatherStep = 0u; weatherStep <= 100u; ++weatherStep) {
        const f32 weather = static_cast<f32>(weatherStep) / 100.0f;
        const f32 lowPotential =
            ResolveVolumetricCloudCondensationPotential_Internal(
                lowShapePotential, weather, 1.0f, 0.0f);
        const f32 highPotential =
            ResolveVolumetricCloudCondensationPotential_Internal(
                highShapePotential, weather, 1.0f, 0.0f);
        EXPECT_TRUE(std::isfinite(lowPotential));
        EXPECT_TRUE(lowPotential >= previousLowPotential);
        EXPECT_NEAR(
            highPotential - lowPotential,
            highShapePotential - lowShapePotential, 1.0e-6f);
        previousLowPotential = lowPotential;
    }

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader,
        "returnsafeShapePotential+cloudCondensationCommonPotential("
        "verticalProfile,weatherMask,height,toweringStrength);"));
    EXPECT_FALSE(Contains(shader, "cloudCoverage.z"));
    EXPECT_FALSE(Contains(shader, "cloudCoverage.w"));
    EXPECT_FALSE(Contains(shader, "cloudNormalizedBaseDensity("));
    EXPECT_FALSE(Contains(shader, "remapc(baseNoise"));
}

ACS_TEST(VolumetricClouds,
         WeatherCoverageEvolutionCreatesBoundedLocalGrowthAndDissipation) {
    const FVec2 maximumShapePhase{0.18f, 0.16f};
    const FVec2 maximumFinePhase{0.11f, 0.09f};
    const f32 growth = CloudWeatherCoverageEvolutionForTest(
        0.5f, 1.0f, 1.0f,
        maximumShapePhase, maximumFinePhase);
    const f32 dissipation = CloudWeatherCoverageEvolutionForTest(
        0.5f, 0.0f, 0.0f,
        maximumShapePhase, maximumFinePhase);
    EXPECT_NEAR(growth, 0.14f, 1e-6f);
    EXPECT_NEAR(dissipation, -0.14f, 1e-6f);

    // 完全な空と濃い中心は動かさず、全許容値で変化量と被覆を有界に保つ。
    for (u32 coverageStep = 0u; coverageStep <= 100u; ++coverageStep) {
        const f32 coverage =
            static_cast<f32>(coverageStep) / 100.0f;
        for (u32 typeStep = 0u; typeStep <= 20u; ++typeStep) {
            const f32 cloudType =
                static_cast<f32>(typeStep) / 20.0f;
            for (u32 warpStep = 0u; warpStep <= 20u; ++warpStep) {
                const f32 warp =
                    static_cast<f32>(warpStep) / 20.0f;
                for (const f32 phaseSign : {-1.0f, 1.0f}) {
                    const FVec2 shapePhase{
                        0.18f * phaseSign, 0.16f * phaseSign};
                    const FVec2 finePhase{
                        0.11f * phaseSign, 0.09f * phaseSign};
                    const f32 change =
                        CloudWeatherCoverageEvolutionForTest(
                            coverage, cloudType, warp,
                            shapePhase, finePhase);
                    const f32 evolved =
                        SaturateForTest(coverage + change);
                    EXPECT_TRUE(std::isfinite(change));
                    EXPECT_TRUE(std::fabs(change) <= 0.140001f);
                    EXPECT_TRUE(evolved >= 0.0f && evolved <= 1.0f);
                    if (coverageStep == 0u || coverageStep == 100u) {
                        EXPECT_NEAR(change, 0.0f, 0.0f);
                        EXPECT_NEAR(evolved, coverage, 0.0f);
                    }
                }
            }
        }
    }

    // 時刻0では従来と一致し、5秒後は同時に成長地点と消散地点を作る。
    const auto zeroTerms =
        ResolveVolumetricCloudEvolutionFrameTerms(0.0f, 1.0f);
    EXPECT_NEAR(
        CloudWeatherCoverageEvolutionForTest(
            0.5f, 1.0f, 1.0f,
            zeroTerms.shape_phase, zeroTerms.fine_phase),
        0.0f, 0.0f);
    const auto fiveSecondTerms =
        ResolveVolumetricCloudEvolutionFrameTerms(5.0f, 1.0f);
    const f32 fiveSecondGrowth =
        CloudWeatherCoverageEvolutionForTest(
            0.5f, 1.0f, 1.0f,
            fiveSecondTerms.shape_phase,
            fiveSecondTerms.fine_phase);
    const f32 fiveSecondDissipation =
        CloudWeatherCoverageEvolutionForTest(
            0.5f, 0.0f, 0.0f,
            fiveSecondTerms.shape_phase,
            fiveSecondTerms.fine_phase);
    EXPECT_TRUE(fiveSecondGrowth > 0.02f);
    EXPECT_NEAR(
        fiveSecondGrowth, -fiveSecondDissipation, 1e-6f);

    // 60 Hzの隣接フレームでは被覆を急変させず、時間再構成が追従できる。
    const auto nextFrameTerms =
        ResolveVolumetricCloudEvolutionFrameTerms(
            5.0f + 1.0f / 60.0f, 1.0f);
    const f32 nextFrameGrowth =
        CloudWeatherCoverageEvolutionForTest(
            0.5f, 1.0f, 1.0f,
            nextFrameTerms.shape_phase,
            nextFrameTerms.fine_phase);
    EXPECT_TRUE(
        std::fabs(nextFrameGrowth - fiveSecondGrowth) < 0.0002f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudWeatherCoverageEvolution(float4weather){"
        "float2localPattern=float2(weather.a,weather.g)*2.0-1.0;"
        "floatslowPhase=dot(cloudEvolution.xy,localPattern);"
        "floatfinePhase=dot(cloudEvolution.zw,localPattern.yx);"
        "floatedgeBase=weather.r*(1.0-weather.r);"
        "floatedgeResponse=16.0*edgeBase*edgeBase;"
        "returnclamp(slowPhase*0.38+finePhase*0.32,-0.14,0.14)"
        "*edgeResponse;}"));
    const std::size_t typeExpansion = shader.find("weather.g=smoothstep(0.42,0.66,weather.g);");
    const std::size_t typeControl = shader.find(
        "weather.g=lerp(weather.g,cloudWeatherControl.x,cloudWeatherControl.y);");
    const std::size_t precipitationControl = shader.find(
        "weather.b=lerp(weather.b,cloudWeatherControl.z,cloudWeatherControl.w);");
    const std::size_t coverageEvolution = shader.find(
        "weather.r=saturate("
        "weather.r+cloudWeatherCoverageEvolution(weather));");
    const std::size_t weatherReturn = shader.find(
        "returnweather;}", coverageEvolution);
    EXPECT_TRUE(typeExpansion < typeControl);
    EXPECT_TRUE(typeControl < precipitationControl);
    EXPECT_TRUE(precipitationControl < coverageEvolution);
    EXPECT_TRUE(coverageEvolution < weatherReturn);
    const std::size_t helperBegin = shader.find(
        "floatcloudWeatherCoverageEvolution(");
    const std::size_t helperEnd = shader.find(
        "*edgeResponse;}", helperBegin);
    EXPECT_TRUE(helperBegin != std::string::npos);
    EXPECT_TRUE(helperEnd != std::string::npos);
    if (helperBegin != std::string::npos &&
        helperEnd != std::string::npos) {
        const std::string helper = shader.substr(
            helperBegin, helperEnd - helperBegin);
        EXPECT_FALSE(Contains(helper, "SampleLevel("));
    }
}

ACS_TEST(VolumetricClouds, WeatherTypeExpansionPreservesStratusTransitionAndCumulusRanges) {
    // 決定的な天候場の 10～90 パーセンタイルを含む代表値で、中央域を飽和させない。
    EXPECT_NEAR(CloudExpandedTypeForTest(0.40f), 0.0f, 0.0f);
    EXPECT_NEAR(CloudExpandedTypeForTest(0.48f), 0.15625f, 1e-6f);
    EXPECT_NEAR(CloudExpandedTypeForTest(0.54f), 0.5f, 1e-6f);
    EXPECT_NEAR(CloudExpandedTypeForTest(0.60f), 0.84375f, 1e-6f);
    EXPECT_NEAR(CloudExpandedTypeForTest(0.68f), 1.0f, 0.0f);

    f32 previous = -1.0f;
    for (u32 step = 0u; step <= 1000u; ++step) {
        const f32 input = static_cast<f32>(step) / 1000.0f;
        const f32 expanded = CloudExpandedTypeForTest(input);
        EXPECT_TRUE(std::isfinite(expanded));
        EXPECT_TRUE(expanded >= previous);
        EXPECT_TRUE(expanded >= 0.0f && expanded <= 1.0f);
        previous = expanded;
    }

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(shader, "weather.g=smoothstep(0.42,0.66,weather.g);"));
    EXPECT_FALSE(Contains(shader, "weather.g=smoothstep(0.34,0.58,weather.g);"));
}

ACS_TEST(VolumetricClouds,
         MacroSamplingSeparatesPointMediumFromConservativeOccupancy) {
    for (u32 coverageSample = 0u;
         coverageSample <= 1000u;
         ++coverageSample) {
        const f32 coverage =
            static_cast<f32>(coverageSample) / 1000.0f;
        const f32 occupancyCoverage =
            SaturateForTest(coverage + 0.08f);
        for (u32 weatherSample = 0u;
             weatherSample <= 1000u;
             ++weatherSample) {
            const f32 weather =
                static_cast<f32>(weatherSample) / 1000.0f;
            const f32 occupancyMask =
                CloudWeatherMaskForTest(
                    weather, occupancyCoverage);
            if (occupancyMask <= 0.001f) {
                EXPECT_TRUE(
                    CloudWeatherMaskForTest(
                        weather, coverage) <= 0.001f);
            }
        }
    }

    struct FWeatherCoverageCalibration {
        f32 authored_coverage;
        f32 measured_boundary;
    };
    // 二領域を混ぜた決定的な天候場の逆百分位。しきい値がこの境界に沿うことで、
    // 0.42の入力が旧式の12%ではなく約42%の正領域を選ぶ。
    const FWeatherCoverageCalibration calibrations[]{
        {0.10f, 0.684051f},
        {0.25f, 0.623031f},
        {0.50f, 0.549406f},
        {0.75f, 0.472031f},
        {0.90f, 0.399657f}};
    for (const FWeatherCoverageCalibration& calibration : calibrations) {
        const f32 threshold =
            0.72f + (0.36f - 0.72f) * calibration.authored_coverage;
        EXPECT_NEAR(threshold, calibration.measured_boundary, 0.025f);
    }
    EXPECT_NEAR(0.72f + (0.36f - 0.72f) * 0.42f, 0.5688f, 1.0e-6f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    const std::string compactSource = CompactShader(source);
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudWeatherThreshold(floatcoverage){"
        "returnlerp(0.72,0.36,saturate(coverage));}"));
    EXPECT_TRUE(Contains(shader, "float4cloudCoverage;"));
    EXPECT_TRUE(Contains(
        compactSource,
        "constf32occupancyCoverage="
        "safeCoverage+0.08f<1.0f?safeCoverage+0.08f:1.0f;"));
    EXPECT_TRUE(Contains(
        compactSource,
        "out.coverage=FVec4{"
        "0.72f-0.36f*occupancyCoverage,"
        "0.72f-0.36f*safeCoverage,"
        "0.0f,0.0f};"));
    EXPECT_FALSE(Contains(
        compactSource, "kVolumetricCloudBaseNoise"));
    const char* declaration =
        "CloudMacroSamplesampleCloudMacroFromThreshold(";
    const char* initializers[]{
        "macro.weather=float4(0,0,0,0);",
        "macro.curl=float2(0,0);",
        "macro.shapePotential=-1.0.xxxx;",
        "macro.shapeFrequencyWeights=float4(0.0,0.0,0.0,1.0);",
        "macro.toweringStrength=0.0;",
        "macro.densityWeatherMask=0.0;",
        "macro.heightProfile=0.0;",
        "macro.columnInterior=0.0;",
        "macro.layerHeight=0.0;",
        "macro.altitude=0.0;",
        "macro.height=0.0;",
        "macro.columnSpan=1.0;",
        "macro.upperBand=0.0;"};
    EXPECT_FALSE(Contains(shader, "physicalSubrayBlend"));
    EXPECT_EQ(
        CountOccurrences(shader, "macro.altitude=altitude;"),
        static_cast<std::size_t>(1));
    const std::size_t begin = shader.find(declaration);
    const std::size_t end = shader.find("returnmacro;}", begin);
    EXPECT_TRUE(begin != std::string::npos);
    EXPECT_TRUE(end != std::string::npos);
    if (begin != std::string::npos && end != std::string::npos) {
        const std::string function = shader.substr(begin, end - begin);
        const std::size_t altitude =
            function.find("floataltitude=cloudAltitude(p);");
        const std::size_t altitudeStore =
            function.find("macro.altitude=altitude;");
        const std::size_t layerBand =
            function.find("boolupperBand=inUpperCloudBandFromAltitude(altitude);");
        const std::size_t layerHeight =
            function.find("floatlayerHeight=heightFractionFromAltitude("
                          "altitude,upperBand);");
        const std::size_t layerHeightStore =
            function.find("macro.layerHeight=layerHeight;");
        const std::size_t weather =
            function.find(
                "macro.weather=cloudWeatherDataBandLimitedPoint("
                "p,safeFootprint.xz);");
        const std::size_t columnInterior =
            function.find("macro.columnInterior=cloudWeatherMask");
        const std::size_t toweringStrength =
            function.find("macro.toweringStrength=cloudLocalToweringStrength(");
        const std::size_t height =
            function.find("macro.height=layerHeight;");
        const usize macroUpperBand =
            function.find("macro.upperBand=upperBand?1.0:0.0;", weather);
        const std::size_t densityWeather =
            function.find("macro.densityWeatherMask=cloudWeatherMask");
        const std::size_t heightProfile =
            function.find("macro.heightProfile=saturate(cloudProfile(");
        const std::size_t curl =
            function.find(
                "macro.curl=cloudCurlOffset(p,safeFootprint.xz);");
        const std::size_t shape = function.find(
            "macro.shapePotential=cloudShapePotentialBands(");
        for (const char* initializer : initializers) {
            const std::size_t initialized =
                function.find(initializer);
            EXPECT_TRUE(initialized != std::string::npos);
            EXPECT_TRUE(initialized < altitude);
        }
        EXPECT_TRUE(altitude < altitudeStore);
        EXPECT_TRUE(altitudeStore < layerBand);
        EXPECT_TRUE(layerBand < layerHeight);
        EXPECT_TRUE(layerHeight < layerHeightStore);
        EXPECT_TRUE(layerHeightStore < weather);
        EXPECT_TRUE(weather < macroUpperBand);
        EXPECT_TRUE(macroUpperBand < columnInterior);
        EXPECT_TRUE(columnInterior < toweringStrength);
        EXPECT_TRUE(toweringStrength < height);
        EXPECT_TRUE(height < densityWeather);
        EXPECT_TRUE(densityWeather < heightProfile);
        EXPECT_TRUE(heightProfile < curl);
        EXPECT_TRUE(curl < shape);
        EXPECT_FALSE(Contains(function, "macro.weatherMask"));
        EXPECT_FALSE(Contains(function, "cloudColumnHeightAndSpan("));
        EXPECT_FALSE(Contains(function, "cloudColumnTop("));
        EXPECT_FALSE(Contains(function, "rejectionThreshold"));
        EXPECT_FALSE(Contains(function, "if(macro.heightProfile"));
    }

    EXPECT_TRUE(Contains(
        shader,
        "CloudMacroSamplesampleCloudMacro("
        "float3p,float4coverageTerms,"
        "float3longitudinalFootprint,float3transverseFootprint){"
        "returnsampleCloudMacroFromThreshold("
        "p,coverageTerms.y,cloudCoverageReciprocals.y,"
        "longitudinalFootprint,transverseFootprint);}"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudMacroSamplesampleCloudMacroLightingBandLimited("
        "float3p,floatweatherCoverage,"
        "float3longitudinalFootprint,float3transverseFootprint){"));
    EXPECT_TRUE(Contains(
        shader,
        "returnsampleCloudMacroFromThreshold("
        "p,coverageThreshold,inverseTransitionWidth,"
        "longitudinalFootprint,transverseFootprint);"));

}

ACS_TEST(VolumetricClouds,
         MacroDensityAndLightingShareEveryBeerQuadraturePoint) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    // 担当セルの占有は保守的最大値で判定し、実密度と散乱源は各Gauss点で揃える。
    EXPECT_TRUE(Contains(
        shader,
        "float2occupancySample=cloudShapeOccupancyAtInterval("
        "occupancyP,occupancyPhysicalFootprint);"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudMacroSamplecurrentMacro=sampleCloudMacro("
        "currentP,coverageTerms,"
        "densityLongitudinalFootprint,0.5*densityTransverseFootprint);"));
    EXPECT_FALSE(Contains(shader, "viewMacroUvw"));
    EXPECT_FALSE(Contains(shader, "cloudOccupancyFromMacro("));
    EXPECT_FALSE(Contains(shader, "occupancyBaseNoise"));
    EXPECT_TRUE(Contains(
        shader,
        "currentP,currentMacro,currentMacro.densityWeatherMask,"));
    EXPECT_TRUE(Contains(
        shader,
        "detailedDistribution*=cloudOpticalDepthScaleFromBand("
        "currentMacro.upperBand>0.5);"));
    EXPECT_TRUE(Contains(
        shader,
        "sample.densityStates=detailedDistribution;"
        "sample.correlationLength="
        "cloudUnresolvedDensityCorrelationLengthAtDirection("
        "currentP,rayDirection,physicalBandId>0);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcurrentSampleT=cloudIntervalDistanceAtFraction("
        "componentStartT,componentEndT,sampleFraction);"));
    EXPECT_FALSE(Contains(shader, "physicalSupport"));
    EXPECT_TRUE(Contains(
        shader,
        "cloudLightingSourceAtPoint("
        "currentP,currentMacro,lightingContext,lowLodDensity,rayDirection);"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudLightingContextphysicalLaneLightingContext="
        "cloudLightingContextForRayDirection("
        "lightingContext,physicalRayDirection);"));
    EXPECT_TRUE(Contains(
        shader,
        "depthMoment+=firstSampleWeight*firstSampleT;"));
    EXPECT_TRUE(Contains(
        shader,
        "sample.sourceValidity=1.0;"));
    EXPECT_TRUE(Contains(
        shader,
        "sample.secondOrderSource=lightingSource.secondOrder*"
        "lightingSource.higherOrderInScatterFactor;"));
    EXPECT_FALSE(Contains(shader, "weightedSegmentLength"));
    EXPECT_FALSE(Contains(shader, "filtered*0.25"));
    EXPECT_TRUE(Contains(
        shader,
        "macro.curl=cloudCurlOffset(p,safeFootprint.xz);"));
    EXPECT_TRUE(Contains(shader, "float2detailXz=p.xz-cloudWindWorld()+" "cloudHeightShapeShear(macro.layerHeight,macro.upperBand>0.5)+" "macro.curl*35.0;"));
    EXPECT_TRUE(Contains(
        shader,
        "cloudDetailDomains("
        "detailXz,macro.altitude,detailDomainA,detailDomainB);"));
    EXPECT_FALSE(Contains(shader, "rotateNoise(detailPosA)"));
    EXPECT_FALSE(Contains(shader, "rotateNoise(detailPosB)"));
    EXPECT_FALSE(Contains(
        shader,
        "float2detailXz=p.xz-cloudWindWorld()+"
        "cloudCurlOffset(p)*35.0;"));

    // 基本形状は一つの3D領域を使い、点ポテンシャルと担当区間の最大値を分離する。
    EXPECT_FALSE(Contains(shader, "float3uvwB=float3("));
    EXPECT_FALSE(Contains(shader, "float3uvwC=float3("));
    EXPECT_FALSE(Contains(shader, "float3uvwD=float3("));
    EXPECT_TRUE(Contains(
        shader,
        "floatcommonPotential=cloudCondensationCommonPotential("
        "macro.heightProfile,weatherMask,"));
    EXPECT_FALSE(Contains(shader, "cloudNormalizedBaseDensity("));
    EXPECT_FALSE(Contains(shader, "macro.baseNoise"));
    EXPECT_FALSE(Contains(shader, "cloudShapeDomainVisibility("));
    EXPECT_FALSE(Contains(shader, "cloudGovernedShapeErosion("));
    EXPECT_FALSE(Contains(shader, "cloudCenteredShape"));
    const std::size_t pointShapeBegin = shader.find(
        "float4cloudShapePotentialBands(float3uvw){");
    const std::size_t occupancyShapeBegin = shader.find(
        "floatcloudShapeOccupancyMaximum(", pointShapeBegin);
    const std::size_t macroBegin =
        shader.find("structCloudMacroSample", occupancyShapeBegin);
    EXPECT_TRUE(pointShapeBegin != std::string::npos);
    EXPECT_TRUE(occupancyShapeBegin != std::string::npos);
    EXPECT_TRUE(macroBegin != std::string::npos);
    EXPECT_TRUE(pointShapeBegin < occupancyShapeBegin);
    EXPECT_TRUE(occupancyShapeBegin < macroBegin);
    if (pointShapeBegin != std::string::npos &&
        occupancyShapeBegin != std::string::npos &&
        macroBegin != std::string::npos) {
        const std::string pointShape = shader.substr(
            pointShapeBegin, occupancyShapeBegin - pointShapeBegin);
        const std::string occupancyShape = shader.substr(
            occupancyShapeBegin, macroBegin - occupancyShapeBegin);
        EXPECT_EQ(CountOccurrences(
            pointShape, "shapeNoise.SampleLevel("),
            static_cast<std::size_t>(1));
        EXPECT_EQ(CountOccurrences(
            pointShape, "shapeOccupancy.SampleLevel("),
            static_cast<std::size_t>(0));
        EXPECT_EQ(
            CountOccurrences(occupancyShape, "shapeNoise.SampleLevel("),
            static_cast<std::size_t>(0));
        EXPECT_EQ(
            CountOccurrences(occupancyShape, "shapeOccupancy.SampleLevel("),
            static_cast<std::size_t>(1));
        EXPECT_TRUE(Contains(
            pointShape,
            "returnfloat4("
            "cloudSignedPotentialFromStored(densityBands.r),"
            "cloudSignedPotentialFromStored(densityBands.g),"
            "cloudSignedPotentialFromStored(densityBands.b),-1.0);"));
        EXPECT_TRUE(Contains(
            occupancyShape,
            "returncloudSignedPotentialFromStored("
            "occupancyStoredPotential);"));
    }
    EXPECT_FALSE(Contains(shader, "sampleCloudMacro(p-camPos"));
}

ACS_TEST(VolumetricClouds, SignedPrimaryShapePreservesSupportAndExplicitBranchOutputs) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    // 点密度と探索最大値は戻り値で明示し、out引数や早期returnの未定義値を作らない。
    EXPECT_FALSE(Contains(shader, "cloudGovernedShapeErosion("));
    EXPECT_FALSE(Contains(shader, "bErosionShape"));
    EXPECT_FALSE(Contains(shader, "cErosionShape"));
    EXPECT_FALSE(Contains(shader, "dErosionShape"));

    const std::size_t pointShapeBegin = shader.find(
        "float4cloudShapePotentialBands(float3uvw){");
    const std::size_t occupancyShapeBegin = shader.find(
        "floatcloudShapeOccupancyMaximum(", pointShapeBegin);
    const std::size_t macroBegin =
        shader.find("structCloudMacroSample", occupancyShapeBegin);
    EXPECT_TRUE(pointShapeBegin != std::string::npos);
    EXPECT_TRUE(occupancyShapeBegin != std::string::npos);
    EXPECT_TRUE(macroBegin != std::string::npos);
    if (pointShapeBegin != std::string::npos &&
        occupancyShapeBegin != std::string::npos &&
        macroBegin != std::string::npos) {
        const std::string pointShape = shader.substr(
            pointShapeBegin, occupancyShapeBegin - pointShapeBegin);
        const std::string occupancyShape = shader.substr(
            occupancyShapeBegin, macroBegin - occupancyShapeBegin);
        EXPECT_TRUE(Contains(
            pointShape,
            "returnfloat4("
            "cloudSignedPotentialFromStored(densityBands.r),"
            "cloudSignedPotentialFromStored(densityBands.g),"
            "cloudSignedPotentialFromStored(densityBands.b),-1.0);"));
        EXPECT_TRUE(Contains(
            occupancyShape,
            "returncloudSignedPotentialFromStored("
            "occupancyStoredPotential);"));
        EXPECT_FALSE(Contains(pointShape, "outfloat"));
        EXPECT_FALSE(Contains(occupancyShape, "outfloat"));
    }

    // 全ての高周波端点で、低周波核に対する単調性と符号付き値域を保つ。
    for (u32 detailBits = 0u; detailBits < 16u; ++detailBits) {
        const f32 middle = (detailBits & 1u) != 0u ? 1.0f : 0.0f;
        const f32 fine = (detailBits & 2u) != 0u ? 1.0f : 0.0f;
        const f32 worleyA = (detailBits & 4u) != 0u ? 1.0f : 0.0f;
        const f32 worleyB = (detailBits & 8u) != 0u ? 1.0f : 0.0f;
        f32 previousShape = -1.0f;
        for (u32 macroStep = 0u; macroStep <= 40u; ++macroStep) {
            const f32 macro = static_cast<f32>(macroStep) / 40.0f;
            const f32 shape = CloudHierarchicalShapeForTest(
                macro, middle, fine, worleyA, worleyB);
            EXPECT_TRUE(shape >= -1.0f);
            EXPECT_TRUE(shape <= 1.0f);
            EXPECT_TRUE(shape + 1.0e-6f >= previousShape);
            previousShape = shape;
        }
        EXPECT_NEAR(
            CloudHierarchicalShapeForTest(
                0.0f, middle, fine, worleyA, worleyB),
            -1.0f, 1.0e-6f);
        EXPECT_NEAR(
            CloudHierarchicalShapeForTest(
                1.0f, middle, fine, worleyA, worleyB),
            1.0f, 0.0f);
    }
    EXPECT_FALSE(Contains(shader, "cloudGovernedShapeErosion("));
    EXPECT_FALSE(Contains(shader, "returngovernedShape*lerp("));
}

ACS_TEST(VolumetricClouds,
         MacroDerivedTermsAreComputedOnceAndLightBasisIsFrameHoisted) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    const std::size_t densityBegin =
        shader.find("float4cloudDensityDistributionFromMacro(");
    const std::size_t densityEnd =
        shader.find("returndensityDistribution;}", densityBegin);
    EXPECT_TRUE(densityBegin != std::string::npos);
    EXPECT_TRUE(densityEnd != std::string::npos);
    if (densityBegin != std::string::npos &&
        densityEnd != std::string::npos) {
        EXPECT_FALSE(Contains(
            shader.substr(
                densityBegin, densityEnd - densityBegin),
            "smoothstep(0.02,0.32,macro.profile)"));
    }
    EXPECT_TRUE(Contains(
        shader, "floatheight=macro.height;"));
    EXPECT_FALSE(Contains(
        shader, "floath=heightFraction(p);"));

    const std::size_t mainEntry =
        shader.find("[numthreads(8,8,1)]voidCSCloud(");
    const std::size_t viewLoop =
        shader.find("[loop]for(inti=0;i<MAX_STEPS", mainEntry);
    const std::size_t shadowEntry =
        shader.find("[numthreads(4,1,4)]voidCSCloudShadow(");
    const std::size_t basisUse = shader.find(
        "float3finiteSunDirection=cloudSunDiskDirection("
        "sun,cloudLightTangent.xyz,cloudLightBitangent.xyz,groupIndex);",
        shadowEntry);
    const std::size_t shadowHeightLoop = shader.find(
        "[loop]for(uintsunHeightIndex=0u;", shadowEntry);
    EXPECT_TRUE(mainEntry != std::string::npos);
    EXPECT_TRUE(viewLoop != std::string::npos);
    EXPECT_TRUE(shadowEntry != std::string::npos);
    EXPECT_TRUE(basisUse != std::string::npos);
    EXPECT_TRUE(shadowHeightLoop != std::string::npos);
    EXPECT_TRUE(basisUse < shadowHeightLoop);
    EXPECT_EQ(CountOccurrences(
        shader, "cloudLightTangent.xyz"),
        static_cast<std::size_t>(7));
    EXPECT_EQ(CountOccurrences(
        shader, "cloudLightBitangent.xyz"),
        static_cast<std::size_t>(7));
    EXPECT_FALSE(Contains(shader,"normalize(sunDir.xyz)"));
    EXPECT_FALSE(Contains(shader,"cloudLightBasis("));
    EXPECT_FALSE(Contains(shader,"cloudLightStepFromBand("));
    EXPECT_FALSE(Contains(shader,"cloudCoverageReciprocals.w"));

    const std::size_t fixedIntegrator = shader.find(
        "float3traceCloudMainLightDepths("
        "float3rayOrigin,floatcoverage,float3lightDirection,"
        "float3extinctionByOrder){");
    const std::size_t bandIntersection = shader.find(
        "CloudPackedBandIntervalsintervals="
        "intersectCloudBandsFromPosition("
        "rayOrigin,lightDirection);",fixedIntegrator);
    const std::size_t fixedLoop = shader.find(
        "[loop]for(intsampleIndex=0;"
        "sampleIndex<CLOUD_LIGHT_MARCH_SAMPLE_COUNT;"
        "++sampleIndex){",bandIntersection);
    const std::size_t distanceMapping = shader.find(
        "if(!cloudAdaptiveLightSampleTerms("
        "intervals,sampleCounts,sampleIndex,"
        "rayDistance,sampleSpacing))continue;",fixedLoop);
    const std::size_t samplePosition = shader.find(
        "float3samplePosition=rayOrigin+lightDirection*rayDistance;",
        distanceMapping);
    const std::size_t lightMacro = shader.find(
        "CloudMacroSamplemacro=sampleCloudMacroLightingSegment("
        "samplePosition,coverage,lightDirection,sampleSpacing);",
        samplePosition);
    const std::size_t lightingSource = shader.find(
        "CloudLightingSourcecloudLightingSourceAtPoint(");
    const std::size_t cacheRead = shader.find(
        "sampleCloudSunTransmittance("
        "p,cacheBlendWeight,"
        "cachedFirstVisibility,cachedSecondVisibility,"
        "cachedThirdVisibility);",lightingSource);
    const std::size_t exactFallback = shader.find(
        "traceCloudMainLightDepths("
        "p,context.coverage,finiteSunDirection,"
        "exactExtinctionByOrder);",cacheRead);
    EXPECT_TRUE(Contains(shader, "staticconstboolCLOUD_MAIN_SHADOW_CACHE_ENABLED=true;"));
    EXPECT_TRUE(fixedIntegrator != std::string::npos);
    EXPECT_TRUE(bandIntersection != std::string::npos);
    EXPECT_TRUE(fixedLoop != std::string::npos);
    EXPECT_TRUE(distanceMapping != std::string::npos);
    EXPECT_TRUE(samplePosition != std::string::npos);
    EXPECT_TRUE(lightMacro != std::string::npos);
    EXPECT_TRUE(lightingSource != std::string::npos);
    EXPECT_TRUE(cacheRead != std::string::npos);
    EXPECT_TRUE(exactFallback != std::string::npos);
    EXPECT_TRUE(fixedIntegrator < bandIntersection);
    EXPECT_TRUE(bandIntersection < fixedLoop);
    EXPECT_TRUE(fixedLoop < distanceMapping);
    EXPECT_TRUE(distanceMapping < samplePosition);
    EXPECT_TRUE(samplePosition < lightMacro);
    EXPECT_TRUE(lightingSource < cacheRead);
    EXPECT_TRUE(cacheRead < exactFallback);
    EXPECT_TRUE(exactFallback < viewLoop);
    EXPECT_TRUE(Contains(
        shader,
        "staticconstintCLOUD_LIGHT_MARCH_SAMPLE_COUNT=16;"
        "staticconstintCLOUD_LIGHT_DETAIL_SAMPLE_COUNT=3;"));
    EXPECT_TRUE(Contains(
        shader,
        "if(lightDepths.z>18.0)break;"));
    EXPECT_TRUE(Contains(
        shader,
        "float4firstDetailOpticalDepthResiduals=0.0.xxxx;"
        "float4secondDetailOpticalDepthResiduals=0.0.xxxx;"
        "float4thirdDetailOpticalDepthResiduals=0.0.xxxx;"
        "if(cacheBlendWeight>0.0){"));
    EXPECT_TRUE(Contains(
        shader,
        "[loop]for(uintresidualDirectionIndex=0u;"
        "residualDirectionIndex<CLOUD_SUN_DISK_DIRECTION_COUNT;"
        "++residualDirectionIndex){"));
    EXPECT_TRUE(Contains(
        shader,
        "float3residualSunDirection=cloudSunDiskDirection("
        "context.sun,cloudLightTangent.xyz,cloudLightBitangent.xyz,"
        "residualDirectionIndex);"
        "float3directionalResiduals=cloudNearLightOpticalDepthResiduals("
        "p,context.coverage,residualSunDirection,"
        "cacheExtinctionByOrder);"));
    /** 次数別消散を作る位置。 */
    const std::size_t residualExtinctionPosition = shader.find(
        "float3cacheExtinctionByOrder=float3(");
    /** 方向別残差の初期化位置。 */
    const std::size_t residualInitializationPosition = shader.find(
        "float4firstDetailOpticalDepthResiduals=0.0.xxxx;",
        residualExtinctionPosition);
    /** 太陽円盤方向ループの位置。 */
    const std::size_t residualLoopPosition = shader.find(
        "[loop]for(uintresidualDirectionIndex=0u;",
        residualInitializationPosition);
    /** 動的添字を使わず方向を選ぶ位置。 */
    const std::size_t residualSelectorPosition = shader.find(
        "float4residualDirectionSelector=float4(", residualLoopPosition);
    /** 3次数の方向別残差を埋め終える位置。 */
    const std::size_t residualThirdAccumulationPosition = shader.find(
        "thirdDetailOpticalDepthResiduals+="
        "residualDirectionSelector*directionalResiduals.z;",
        residualSelectorPosition);
    /** 12値からキャッシュ信頼度を判定する位置。 */
    const std::size_t residualReliabilityPosition = shader.find(
        "floatcacheReliability=cloudSunDepthResidualCacheReliability(",
        residualThirdAccumulationPosition);
    /** 信頼度をキャッシュ混合率へ反映する位置。 */
    const std::size_t residualBlendPosition = shader.find(
        "cacheBlendWeight*=cacheReliability;",
        residualReliabilityPosition);
    EXPECT_TRUE(residualExtinctionPosition < residualInitializationPosition);
    EXPECT_TRUE(residualInitializationPosition < residualLoopPosition);
    EXPECT_TRUE(residualLoopPosition < residualSelectorPosition);
    EXPECT_TRUE(residualSelectorPosition < residualThirdAccumulationPosition);
    EXPECT_TRUE(residualThirdAccumulationPosition < residualReliabilityPosition);
    EXPECT_TRUE(residualReliabilityPosition < residualBlendPosition);
    EXPECT_FALSE(Contains(
        shader,
        "p,context.coverage,context.sun,cacheExtinctionByOrder);"));
    EXPECT_TRUE(Contains(
        shader,
        "float3lightSegmentDepth="
        "cloudDensityOpticalDepthByOrderFromMacro("
        "samplePosition,macro,macro.densityWeatherMask,"));
    EXPECT_TRUE(Contains(
        shader,
        "lightDirection,sampleSpacing,"
        "firstOrderState,secondOrderState,thirdOrderState);"
        "lightDepths+=lightSegmentDepth;"));
    EXPECT_TRUE(Contains(
        shader,
        "int4sampleCounts=cloudAdaptiveLightSampleCounts("
        "intervals,CLOUD_LIGHT_MARCH_SAMPLE_COUNT,"
        "rayOrigin,lightDirection);"));
    EXPECT_FALSE(Contains(shader,"floatlightJitter="));
    EXPECT_FALSE(Contains(shader,"lightStep*=1.8"));
    EXPECT_FALSE(Contains(shader, "cloudConeDirection("));
    EXPECT_FALSE(Contains(shader, "coneSin"));
    EXPECT_FALSE(Contains(shader,"sampleCloudFarLightingDensityAndScale("));
    EXPECT_FALSE(Contains(shader,"sampleCloudMacroLightingFromSlowFields("));
    EXPECT_FALSE(Contains(shader,"sampleCloudLightingDensityFromSlowFields("));
}

ACS_TEST(VolumetricClouds, LightMarchUsesCorrelationAwareIntervalsAndMidpoints) {
    constexpr f32 kStepLength = 8.0f;
    const auto decreasingLinearDensity = [kStepLength](f32 distance) noexcept { return 1.0f - distance / kStepLength; };
    const f32 exactDepth = 0.5f * kStepLength;
    const f32 rightEndpointDepth = decreasingLinearDensity(kStepLength) * kStepLength;
    const f32 midpointDepth = decreasingLinearDensity(0.5f * kStepLength) * kStepLength;

    // 終点法は区間内で濃度が下がる雲縁を見落とすが、中央法は一次変化の積分値と一致する。
    EXPECT_NEAR(rightEndpointDepth, 0.0f, 0.0f);
    EXPECT_NEAR(midpointDepth, exactDepth, 1e-6f);
    EXPECT_TRUE(std::fabs(rightEndpointDepth - exactDepth) > 0.49f * kStepLength);

    // 均質な代表区間では、相関長による配分後も担当距離の積分を保つ。
    constexpr f32 kOccupiedLength = 2500.0f;
    constexpr f32 kSampleSpacing =
        kOccupiedLength/static_cast<f32>(kVolumetricCloudMaxLightMarchSamples);
    f32 integratedUniformDepth = 0.0f;
    f32 previousMidpoint = -1.0f;
    for (u32 lightSample = 0u;
         lightSample < kVolumetricCloudMaxLightMarchSamples;
         ++lightSample) {
        const f32 midpoint =
            (static_cast<f32>(lightSample)+0.5f)*kSampleSpacing;
        EXPECT_TRUE(midpoint>previousMidpoint);
        EXPECT_TRUE(midpoint>0.0f&&midpoint<kOccupiedLength);
        integratedUniformDepth += 0.42f*kSampleSpacing;
        previousMidpoint=midpoint;
    }
    EXPECT_NEAR(
        previousMidpoint,
        kOccupiedLength-0.5f*kSampleSpacing,1.0e-4f);
    EXPECT_NEAR(integratedUniformDepth,0.42f*kOccupiedLength,1.0e-4f);

    // 長い下層と短い上層へ同じ総距離の中央標本を置くと、短い上層を一度も採取できない。
    // 距離比で7対1へ分け、密度が大きく異なる短い上層も積分へ必ず含める。
    constexpr f32 kFirstStart=0.0f;
    constexpr f32 kFirstEnd=7900.0f;
    constexpr f32 kSecondStart=12000.0f;
    constexpr f32 kSecondEnd=12100.0f;
    constexpr f32 kTwoBandLength=
        (kFirstEnd-kFirstStart)+(kSecondEnd-kSecondStart);
    constexpr u32 kFirstSampleCount=7u;
    constexpr u32 kSecondSampleCount=
        kVolumetricCloudMaxLightMarchSamples-kFirstSampleCount;
    constexpr f32 kFirstDensity=0.05f;
    constexpr f32 kSecondDensity=0.95f;
    f32 twoBandDepth=0.0f;
    for(u32 sampleIndex=0u;
        sampleIndex<kVolumetricCloudMaxLightMarchSamples;
        ++sampleIndex){
        const bool secondBand=sampleIndex>=kFirstSampleCount;
        const u32 bandSampleIndex=secondBand
            ?sampleIndex-kFirstSampleCount:sampleIndex;
        const u32 bandSampleCount=secondBand
            ?kSecondSampleCount:kFirstSampleCount;
        const f32 bandStart=secondBand?kSecondStart:kFirstStart;
        const f32 bandLength=secondBand
            ?kSecondEnd-kSecondStart:kFirstEnd-kFirstStart;
        const f32 bandSpacing=
            bandLength/static_cast<f32>(bandSampleCount);
        const f32 rayDistance=bandStart+
            (static_cast<f32>(bandSampleIndex)+0.5f)*bandSpacing;
        EXPECT_TRUE(rayDistance>=bandStart&&rayDistance<bandStart+bandLength);
        twoBandDepth+=(secondBand?kSecondDensity:kFirstDensity)*bandSpacing;
    }
    const f32 expectedTwoBandDepth=
        kFirstDensity*(kFirstEnd-kFirstStart)+
        kSecondDensity*(kSecondEnd-kSecondStart);
    const f32 concatenatedDepth=kFirstDensity*kTwoBandLength;
    EXPECT_NEAR(
        twoBandDepth,
        expectedTwoBandDepth,
        1.0e-6f*expectedTwoBandDepth);
    EXPECT_TRUE(std::fabs(concatenatedDepth-expectedTwoBandDepth)>80.0f);

    // 近距離の高周波差分は符号を保ち、低LOD深さへ足すと詳細密度へ正確に戻る。
    constexpr f32 kLowLodDensity=0.62f;
    constexpr f32 kDetailedDensity=0.27f;
    constexpr f32 kOpticalScale=kVolumetricCloudReferenceExtinctionPerMeter;
    const f32 cachedDepth=kLowLodDensity*kSampleSpacing*kOpticalScale;
    const f32 detailResidual=
        (kDetailedDensity-kLowLodDensity)*kSampleSpacing*kOpticalScale;
    EXPECT_NEAR(
        cachedDepth+detailResidual,
        kDetailedDensity*kSampleSpacing*kOpticalScale,1.0e-7f);
    EXPECT_TRUE(detailResidual<0.0f);

    // 負の詳細差分は、保存済みの4光路透過率へ個別に指数補正してから1へ制限する。
    // 平均深さへ先に足すと、開いた光路と厚い光路を同じ透過率へ潰してしまう。
    constexpr f32 kDepths[4]={0.2f,0.4f,1.0f,2.0f};
    constexpr f32 kNegativeResidual=-0.5f;
    f32 separatePathTransmittance=0.0f;
    f32 meanDepth=0.0f;
    for(const f32 pathDepth:kDepths){
        const f32 cachedTransmittance=std::exp(-pathDepth);
        const f32 unboundedCorrectedTransmittance=
            cachedTransmittance*std::exp(-kNegativeResidual);
        const f32 correctedTransmittance=
            unboundedCorrectedTransmittance<1.0f
                ?unboundedCorrectedTransmittance:1.0f;
        separatePathTransmittance+=correctedTransmittance;
        meanDepth+=pathDepth;
    }
    separatePathTransmittance*=0.25f;
    meanDepth*=0.25f;
    const f32 correctedMeanDepth=meanDepth+kNegativeResidual>0.0f
        ?meanDepth+kNegativeResidual:0.0f;
    const f32 meanFirstTransmittance=std::exp(-correctedMeanDepth);
    EXPECT_TRUE(separatePathTransmittance>=0.0f&&
                separatePathTransmittance<=1.0f);
    EXPECT_TRUE(std::fabs(
        separatePathTransmittance-meanFirstTransmittance)>0.03f);

    // 太陽円盤の一方向だけが晴れている反例では、中心方向の残差を4方向へ
    // 流用すると可視率1になる。方向別に補正してから面積平均した値を保つ。
    constexpr f32 kDirectionalCachedTransmittances[4]={
        1.0f,1.0f,1.0f,1.0f};
    constexpr f32 kDirectionalResiduals[4]={
        0.0f,5.0f,5.0f,5.0f};
    const f32 directionalResidualAverage=render_internal::
        ResolveVolumetricCloudDirectionalResidualAverage_Internal(
            kDirectionalCachedTransmittances,kDirectionalResiduals);
    const f32 expectedDirectionalResidualAverage=
        0.25f*(1.0f+3.0f*std::exp(-5.0f));
    EXPECT_NEAR(
        directionalResidualAverage,expectedDirectionalResidualAverage,
        1.0e-7f);
    EXPECT_TRUE(std::fabs(directionalResidualAverage-1.0f)>0.7f);

    constexpr f32 kUniformDirectionalResiduals[4]={
        5.0f,5.0f,5.0f,5.0f};
    EXPECT_NEAR(
        render_internal::
            ResolveVolumetricCloudDirectionalResidualAverage_Internal(
                kDirectionalCachedTransmittances,
                kUniformDirectionalResiduals),
        std::exp(-5.0f),1.0e-7f);

    // 晴天列と濃雲列の補間は、各列を透過率へ変換してから行う。
    // 深さ8の中点を深さとして補間する旧方式は、面積の半分に届く直達光を失う。
    constexpr f32 kClearDepth=0.0f;
    constexpr f32 kDenseDepth=8.0f;
    constexpr f32 kCellBlend=0.5f;
    const f32 visibilityFirstInterpolation=
        (1.0f-kCellBlend)*std::exp(-kClearDepth)
        +kCellBlend*std::exp(-kDenseDepth);
    const f32 depthFirstInterpolation=std::exp(
        -((1.0f-kCellBlend)*kClearDepth+kCellBlend*kDenseDepth));
    EXPECT_NEAR(
        visibilityFirstInterpolation,
        0.5f*(1.0f+std::exp(-kDenseDepth)),1.0e-7f);
    EXPECT_TRUE(visibilityFirstInterpolation>0.50f);
    EXPECT_TRUE(depthFirstInterpolation<0.02f);
    EXPECT_TRUE(
        visibilityFirstInterpolation>depthFirstInterpolation+0.48f);

    // R16Fの非正規化領域では負残差による保存時半ULPが増幅されるため、
    // 補正後の値を直接R16Fへ保存した場合の半ULPを越える分だけ正確積分へ移す。
    constexpr f32 kDeepOpticalDepth = 20.0f;
    constexpr f32 kDeepNegativeResidual = -16.0f;
    const f32 deepStoredTransmittance = QuantizeR16FloatForTest(
        std::exp(-kDeepOpticalDepth));
    const f32 deepExpectedTransmittance = std::exp(
        -(kDeepOpticalDepth + kDeepNegativeResidual));
    const f32 deepQuantizedCorrection = deepStoredTransmittance *
        std::exp(-kDeepNegativeResidual);
    EXPECT_NEAR(deepStoredTransmittance, 0.0f, 0.0f);
    EXPECT_NEAR(deepExpectedTransmittance, 0.0183156f, 1.0e-6f);
    EXPECT_NEAR(deepQuantizedCorrection, 0.0f, 0.0f);
    const f32 deepReliability =
        CloudSunDepthResidualCacheReliabilityForTest(
            deepStoredTransmittance, kDeepNegativeResidual);
    EXPECT_TRUE(deepReliability > 0.0f);
    EXPECT_TRUE(deepReliability < 1.0e-6f);

    constexpr f32 kSubnormalOpticalDepth = 17.0f;
    const f32 subnormalStoredTransmittance = QuantizeR16FloatForTest(
        std::exp(-kSubnormalOpticalDepth));
    const f32 subnormalExpectedTransmittance = std::exp(-1.0f);
    const f32 subnormalQuantizedCorrection =
        subnormalStoredTransmittance * std::exp(16.0f);
    EXPECT_TRUE(
        subnormalStoredTransmittance <
        kVolumetricCloudSunCacheMinimumReliableTransmittance);
    EXPECT_TRUE(std::fabs(
        subnormalQuantizedCorrection -
        subnormalExpectedTransmittance) > 0.15f);
    const f32 subnormalReliability =
        CloudSunDepthResidualCacheReliabilityForTest(
            subnormalStoredTransmittance, -16.0f);
    EXPECT_TRUE(subnormalReliability > 0.0f);
    EXPECT_TRUE(subnormalReliability > 0.0008f);
    EXPECT_TRUE(subnormalReliability < 0.0011f);

    const f32 normalStoredTransmittance = QuantizeR16FloatForTest(
        std::exp(-9.0f));
    EXPECT_TRUE(
        normalStoredTransmittance >=
        kVolumetricCloudSunCacheMinimumReliableTransmittance);
    const f32 normalReliability =
        CloudSunDepthResidualCacheReliabilityForTest(
            normalStoredTransmittance, -1.0f);
    EXPECT_TRUE(normalReliability > 0.5f);
    EXPECT_TRUE(normalReliability <= 1.0f);
    EXPECT_NEAR(
        CloudSunDepthResidualCacheReliabilityForTest(
            deepStoredTransmittance, 1.0f),
        1.0f, 0.0f);
    EXPECT_NEAR(
        CloudSunDepthResidualCacheReliabilityForTest(
            1.0f, std::numeric_limits<f32>::quiet_NaN()),
        0.0f, 0.0f);

    // 残差が0を跨いでも、符号だけで信頼度が切り替わらない。消散0なら
    // 呼び出し前に有効光学的深さの残差が0となるため、完全にキャッシュを使える。
    const f32 minimumSubnormal =
        DirectX::PackedVector::XMConvertHalfToFloat(
            static_cast<DirectX::PackedVector::HALF>(0x0001u));
    const f32 reliabilityBelowZero =
        CloudSunDepthResidualCacheReliabilityForTest(
            minimumSubnormal, -1.0e-6f);
    const f32 reliabilityAtZero =
        CloudSunDepthResidualCacheReliabilityForTest(
            minimumSubnormal, 0.0f);
    const f32 reliabilityAboveZero =
        CloudSunDepthResidualCacheReliabilityForTest(
            minimumSubnormal, 1.0e-6f);
    EXPECT_NEAR(reliabilityAtZero, 1.0f, 0.0f);
    EXPECT_NEAR(reliabilityAboveZero, 1.0f, 0.0f);
    EXPECT_TRUE(
        std::fabs(reliabilityBelowZero - reliabilityAtZero) < 2.0e-6f);
    EXPECT_NEAR(
        CloudSunDepthResidualCacheReliabilityForTest(
            minimumSubnormal, 0.0f),
        1.0f, 0.0f);

    // binary16の最大非正規化数(0x03ff)と最小正規化数(0x0400)の間でも、
    // 同じ半ULPモデルが連続し、形式境界だけで大きな輝度差を作らない。
    const f32 maximumSubnormal =
        DirectX::PackedVector::XMConvertHalfToFloat(
            static_cast<DirectX::PackedVector::HALF>(0x03ffu));
    const f32 minimumNormal =
        DirectX::PackedVector::XMConvertHalfToFloat(
            static_cast<DirectX::PackedVector::HALF>(0x0400u));
    const f32 maximumSubnormalReliability =
        CloudSunDepthResidualCacheReliabilityForTest(
            maximumSubnormal, -1.0f);
    const f32 minimumNormalReliability =
        CloudSunDepthResidualCacheReliabilityForTest(
            minimumNormal, -1.0f);
    EXPECT_TRUE(maximumSubnormalReliability > 0.5f);
    EXPECT_TRUE(minimumNormalReliability > 0.5f);
    EXPECT_TRUE(
        std::fabs(
            minimumNormalReliability - maximumSubnormalReliability) <
        0.0011f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    const std::string compactSource = CompactShader(source);
    EXPECT_TRUE(Contains(
        shader,
        "int4cloudPackedIntervalSampleCounts("
        "CloudPackedBandIntervalsintervals,"
        "intrequestedSampleCount){"));
    EXPECT_TRUE(Contains(
        shader,
        "intreservedPerInterval="
        "safeSampleCount>=intervalCount?1:0;"));
    EXPECT_TRUE(Contains(
        shader,
        "sampleCounts[intervalIndex]=reservedPerInterval"
        "+max(target-previousTarget,0);"));
    EXPECT_TRUE(Contains(
        shader,
        "boolcloudLightSampleTerms("
        "CloudPackedBandIntervalsintervals,"
        "intrequestedSampleCount,intsampleIndex,"));
    EXPECT_TRUE(Contains(
        shader,
        "int4sampleCounts=cloudPackedIntervalSampleCounts("
        "intervals,requestedSampleCount);"));
    EXPECT_TRUE(Contains(shader, "returnvalidSample;}"));
    EXPECT_TRUE(Contains(
        shader,
        "float3cloudNearLightOpticalDepthResiduals("
        "float3rayOrigin,floatcoverage,float3lightDirection,"
        "float3extinctionByOrder){"));
    EXPECT_TRUE(Contains(
        shader,
        "[loop]for(intsampleIndex=0;"
        "sampleIndex<CLOUD_LIGHT_DETAIL_SAMPLE_COUNT;"
        "++sampleIndex){"));
    EXPECT_TRUE(Contains(
        shader,
        "float3traceCloudMainLightDepths("
        "float3rayOrigin,floatcoverage,float3lightDirection,"
        "float3extinctionByOrder){"));
    EXPECT_TRUE(Contains(
        shader,
        "float3traceCloudShadowDepths("
        "float3rayOrigin,floatcoverage,float3lightDirection,"
        "float3extinctionByOrder){"));
    EXPECT_TRUE(Contains(
        shader,
        "sampleSpacing=intervalLength"
        "/float(max(intervalSampleCount,1));"
        "rayDistance=intervals.starts[intervalIndex]"
        "+(float(intervalSampleIndex)+0.5)*sampleSpacing;"));
    EXPECT_TRUE(Contains(
        shader,
        "float3samplePosition=rayOrigin+lightDirection*rayDistance;"));
    EXPECT_TRUE(Contains(
        shader,
        "sampleCloudSunTransmittance("
        "p,cacheBlendWeight,"
        "cachedFirstVisibility,cachedSecondVisibility,"
        "cachedThirdVisibility);"));
    EXPECT_TRUE(Contains(
        shader,
        "float3lightScatteringByOrder=lerp("
        "exactAverageScattering,correctedCachedAverageScattering,"
        "cacheBlendWeight);"));
    EXPECT_FALSE(Contains(shader,"floatlightJitter="));
    EXPECT_FALSE(Contains(shader,"lightStep*=1.8"));
    EXPECT_FALSE(Contains(shader,"cloudCoverageReciprocals.w"));
    EXPECT_FALSE(Contains(compactSource,"0.0075f/"));
}

ACS_TEST(VolumetricClouds, EnvironmentCubemapSharesViewSamplingTermsIncludingUpperLayer)
{
    /** 空白を除去し、式と呼び出し順を検査できる描画実装。 */
    const auto compactSource = CompactShader(ReadSkySource());

    // 共有関数は定義1回と画面・環境キューブマップからの呼び出し2回だけにする。
    EXPECT_EQ(CountOccurrences(compactSource, "ResolveVolumetricCloudSamplingTerms_Internal("), static_cast<usize>(3));
    EXPECT_TRUE(Contains(compactSource, "out.coverage=FVec4{0.72f-0.36f*occupancyCoverage,0.72f-0.36f*safeCoverage,0.0f,0.0f};"));
    EXPECT_TRUE(Contains(compactSource, "constf32unclampedFineStep=0.035f/(horizontalNoiseScale>0.001f?horizontalNoiseScale:0.001f);"));
    EXPECT_TRUE(Contains(
        compactSource,
        "out.coverageReciprocals=FVec4{"
        "1.0f/(occupancyWeatherUpper-out.coverage.x),"
        "1.0f/(densityWeatherUpper-out.coverage.y),fineStep,0.0f};"));
    EXPECT_TRUE(Contains(
        compactSource,
        "out.upperTerms=FVec4{upperLayer.coverage_scale,"
        "upperLayer.density_scale,"
        "kVolumetricCloudReferenceExtinctionPerMeter,0.0f};"));
    EXPECT_FALSE(Contains(compactSource,"layerSamplingScale"));
    EXPECT_FALSE(Contains(compactSource,"upperLayerLightStep"));

    /** 画面描画本体の開始位置。 */
    const auto renderBegin = compactSource.find("voidCVolumetricClouds::RenderComputeCameraRelative(");
    /** 続く環境キューブマップ生成の開始位置。 */
    const auto environmentBegin = compactSource.find("TResult<TUniquePtr<IRhiTexture>>CVolumetricClouds::BuildEnvironmentCubemap(", renderBegin);
    /** 環境キューブマップ生成に続く公開取得関数の開始位置。 */
    const auto environmentEnd = compactSource.find("FVolumetricCloudWorldShadowMapCVolumetricClouds::WorldShadowMap()constnoexcept{", environmentBegin);
    EXPECT_TRUE(renderBegin < environmentBegin);
    EXPECT_TRUE(environmentBegin < environmentEnd);

    /** 画面描画だけに限定した実装断片。 */
    auto renderBody = compactSource.substr(0u, 0u);
    /** 環境キューブマップだけに限定した実装断片。 */
    auto environmentBody = compactSource.substr(0u, 0u);
    if (renderBegin < environmentBegin && environmentBegin <= compactSource.size()) {
        renderBody = compactSource.substr(renderBegin, environmentBegin - renderBegin);
    }
    if (environmentBegin < environmentEnd && environmentEnd <= compactSource.size()) {
        environmentBody = compactSource.substr(environmentBegin, environmentEnd - environmentBegin);
    }

    /** 一つの描画経路が共有項をすべて定数バッファーへ渡すことを検査する。 */
    const auto expectSharedTerms = [](const auto& body) {
        EXPECT_TRUE(Contains(body, "ResolveVolumetricCloudSamplingTerms_Internal("));
        EXPECT_TRUE(Contains(body, "cb.cloudCoverage=samplingTerms.coverage;"));
        EXPECT_TRUE(Contains(body, "cb.cloudCoverageReciprocals=samplingTerms.coverageReciprocals;"));
        EXPECT_TRUE(Contains(body, "cb.cloudUpperTerms=samplingTerms.upperTerms;"));
        EXPECT_TRUE(Contains(body, "cb.cloudWeatherControl=FVec4{m_Weather.CloudType,m_Weather.CloudTypeInfluence,m_Weather.Precipitation,m_Weather.PrecipitationInfluence};"));
    };
    expectSharedTerms(renderBody);
    expectSharedTerms(environmentBody);

    // 旧GI経路だけにあった別形状、粗い光採取、上層無効化を戻さない。
    EXPECT_FALSE(Contains(environmentBody, "0.90f-0.55f"));
    EXPECT_FALSE(Contains(environmentBody, "0.72f-0.22f"));
    EXPECT_FALSE(Contains(environmentBody, "constf32light_step=0.012f/"));
    EXPECT_FALSE(Contains(environmentBody, "cb.cloudUpperTerms=FVec4{"));
    EXPECT_EQ(CountOccurrences(compactSource, "cb.cloudWeatherControl=FVec4{"), static_cast<usize>(2));

    /** 通常積雲の代表被覆。 */
    constexpr f32 authoredCoverage = 0.42f;
    /** 空領域の早期棄却だけを保守的に広げた被覆。 */
    constexpr f32 occupancyCoverage = authoredCoverage + 0.08f;
    /** 空領域判定が使う天候しきい値。 */
    constexpr f32 occupancyWeatherThreshold = 0.72f - 0.36f * occupancyCoverage;
    /** 密度積分が使う天候しきい値。 */
    constexpr f32 densityWeatherThreshold = 0.72f - 0.36f * authoredCoverage;
    EXPECT_NEAR(occupancyWeatherThreshold, 0.54f, 1.0e-6f);
    EXPECT_NEAR(densityWeatherThreshold, 0.5688f, 1.0e-6f);
    EXPECT_FALSE(Contains(compactSource, "kVolumetricCloudBaseNoise"));
    EXPECT_TRUE(Contains(
        compactSource,
        "out.coverage=FVec4{"
        "0.72f-0.36f*occupancyCoverage,"
        "0.72f-0.36f*safeCoverage,0.0f,0.0f};"));

    // 光積分の距離は層厚から作る基準刻みではなく、各光線の雲殻交差で決める。
    EXPECT_TRUE(Contains(
        compactSource,
        "CloudPackedBandIntervalsintervals="
        "intersectCloudBandsFromPosition("
        "rayOrigin,lightDirection);"));
    EXPECT_TRUE(Contains(
        compactSource,
        "if(!cloudAdaptiveLightSampleTerms("
        "intervals,sampleCounts,sampleIndex,"
        "rayDistance,sampleSpacing))continue;"));
}

ACS_TEST(VolumetricClouds, LightDensityAndPhysicalOpticalScaleStayCorrectAcrossEveryPath) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    // 上層の被覆倍率は天候域の面積へ適用し、濃さ倍率だけを完成密度へ掛ける。
    const std::size_t densityBegin =
        shader.find("float4cloudDensityDistributionFromMacro(");
    const std::size_t densityEnd =
        shader.find("returndensityDistribution;}", densityBegin);
    EXPECT_TRUE(densityBegin != std::string::npos);
    EXPECT_TRUE(densityEnd != std::string::npos);
    if (densityBegin != std::string::npos && densityEnd != std::string::npos) {
        const std::string body = shader.substr(densityBegin, densityEnd - densityBegin);
        const std::size_t coverage = body.find("if(upperBand)weatherMask*=cloudUpperTerms.x;");
        const std::size_t densityValue = body.find(
            "float4densityDistribution="
            "cloudDensityDistributionFromPositiveWeatherMacro(");
        const std::size_t densityScale = body.find(
            "if(macro.upperBand>0.5)"
            "densityDistribution=cloudScaleDensityDistribution(");
        EXPECT_TRUE(coverage == std::string::npos);
        EXPECT_TRUE(densityValue < densityScale);
    }
    const std::size_t lowLodBegin =
        shader.find("float4cloudLowLodDensityDistributionFromMacro(");
    const std::size_t lowLodEnd =
        shader.find("returndensityDistribution;}", lowLodBegin);
    EXPECT_TRUE(lowLodBegin != std::string::npos);
    EXPECT_TRUE(lowLodEnd != std::string::npos);
    if (lowLodBegin != std::string::npos && lowLodEnd != std::string::npos) {
        const std::string body = shader.substr(lowLodBegin, lowLodEnd - lowLodBegin);
        const std::size_t coverage = body.find("if(upperBand)weatherMask*=cloudUpperTerms.x;");
        const std::size_t densityValue = body.find(
            "float4densityDistribution="
            "cloudLowLodDensityDistributionFromPositiveWeatherMacro(");
        const std::size_t densityScale = body.find(
            "if(macro.upperBand>0.5)"
            "densityDistribution=cloudScaleDensityDistribution(");
        EXPECT_TRUE(coverage == std::string::npos);
        EXPECT_TRUE(densityValue < densityScale);
    }
    EXPECT_FALSE(Contains(shader, "macro.weatherMask"));
    EXPECT_TRUE(Contains(
        shader,
        "layerCoverageThreshold=cloudWeatherThreshold("
        "lowerCoverage*cloudUpperTerms.x);"));
    EXPECT_FALSE(Contains(
        shader, "macro.densityWeatherMask*=cloudUpperTerms.x"));
    EXPECT_TRUE(Contains(
        shader,
        "detailedDistribution=cloudScaleDensityDistribution("
        "detailedDistribution,opticalDensityScale);"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudFourStateTransportResultLanesfirstTransport="
        "cloudFourStateTransportLanes("));
    EXPECT_TRUE(Contains(
        shader,
        "densityLightingSample.correlationLength.xxxx,"
        "laneSegmentLengths,firstOrderTransportState);"));
    EXPECT_FALSE(Contains(shader, "densityStatePathLengths"));
    EXPECT_FALSE(Contains(shader, "densityCentroidFraction"));
    EXPECT_FALSE(Contains(shader, "currentDensity>firstDensity"));
    EXPECT_TRUE(Contains(
        shader,
        "float4distribution=cloudDensityDistributionFromMacro("
        "p,macro,weatherMask,billowVisibility,"
        "middleBillowVisibility,erosionVisibility);"));
    EXPECT_TRUE(Contains(
        shader,
        "float4distribution=cloudLowLodDensityDistributionFromMacro("
        "macro,weatherMask);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatsafeLowLodDensity="));
    EXPECT_FALSE(Contains(shader, "lowLodDensityAndProfile"));
    EXPECT_FALSE(Contains(shader, "boolinUpperCloudBand(float3p)"));
    EXPECT_TRUE(Contains(shader, "floatupperBand;"));
    EXPECT_TRUE(Contains(shader, "macro.upperBand=upperBand?1.0:0.0;"));

    // 消散は上下層とも固定m^-1尺度で、光積分距離は実際の雲殻交差から求める。
    EXPECT_TRUE(Contains(shader, "floatcloudOpticalDepthScaleFromBand(boolupperBand){floatscale=layer.w;if(upperBand)scale=cloudUpperTerms.z;returnscale;}"));
    EXPECT_FALSE(Contains(shader,"cloudLightStepFromBand("));
    const auto compactSource = CompactShader(source);
    EXPECT_TRUE(Contains(compactSource, "out.upperTerms=FVec4{upperLayer.coverage_scale,upperLayer.density_scale,kVolumetricCloudReferenceExtinctionPerMeter,0.0f};"));
    EXPECT_EQ(
        CountOccurrences(
            compactSource,
            "m_Layer.horizontal_noise_scale,kVolumetricCloudReferenceExtinctionPerMeter};"),
        static_cast<usize>(2));
    EXPECT_EQ(CountOccurrences(compactSource, "cb.cloudUpperTerms=samplingTerms.upperTerms;"), static_cast<usize>(2));

    // 高度と降水の補正は詳細密度、低詳細度密度、空間棄却、および
    // 有限な残り光路の上限で共有し、遠距離や終了判定だけ式を落とさない。
    EXPECT_EQ(CountOccurrences(shader, "cloudHeightPrecipitationDensityScale("), static_cast<std::size_t>(5));
    EXPECT_TRUE(Contains(shader, "floatcloudHeightPrecipitationDensityScale(" "floatheight,floatprecipitation){" "returnlerp(1.10,0.92,height)*" "lerp(1.0,1.28,precipitation);}"));
    EXPECT_FALSE(Contains(shader, "sampleCloudLightingDensityFromSlowFields("));
    EXPECT_FALSE(Contains(shader, "sampleCloudLightingShapeFromSlowFields("));
    const std::size_t mainLightBegin =
        shader.find("float3traceCloudMainLightDepths(");
    const std::size_t mainLightEnd =
        shader.find("returnlightDepths;}",mainLightBegin);
    EXPECT_TRUE(mainLightBegin != std::string::npos);
    EXPECT_TRUE(mainLightEnd != std::string::npos);
    if(mainLightBegin != std::string::npos&&
       mainLightEnd != std::string::npos){
        const std::string mainLight=shader.substr(
            mainLightBegin,mainLightEnd-mainLightBegin);
        EXPECT_TRUE(Contains(
            mainLight,
            "CloudMacroSamplemacro=sampleCloudMacroLightingSegment("
            "samplePosition,coverage,lightDirection,sampleSpacing);"));
        EXPECT_TRUE(Contains(
            mainLight,
            "float3lightSegmentDepth="
            "cloudDensityOpticalDepthByOrderFromMacro("
            "samplePosition,macro,macro.densityWeatherMask,"));
        EXPECT_FALSE(Contains(
            mainLight,
            "cloudLowLodDensityFromMacro("));
        EXPECT_TRUE(Contains(
            mainLight,
            "lightDirection,sampleSpacing,"
            "firstOrderState,secondOrderState,thirdOrderState);"
            "lightDepths+=lightSegmentDepth;"));
        EXPECT_FALSE(Contains(mainLight,"sharedLightCurl"));
    }

    const std::size_t cacheBegin = shader.find("float3traceCloudShadowDepths(");
    const std::size_t cacheEnd = shader.find("returnlightDepths;}", cacheBegin);
    EXPECT_TRUE(cacheBegin != std::string::npos);
    EXPECT_TRUE(cacheEnd != std::string::npos);
    if (cacheBegin != std::string::npos && cacheEnd != std::string::npos) {
        const std::string cacheBody = shader.substr(cacheBegin, cacheEnd - cacheBegin);
        EXPECT_TRUE(Contains(
            cacheBody,
            "float3lightSegmentDepth="
            "cloudLowLodOpticalDepthByOrderFromMacro("
            "samplePosition,lightMacro,lightMacro.densityWeatherMask,"));
        EXPECT_TRUE(Contains(
            cacheBody,
            "lightDirection,sampleSpacing,"
            "firstOrderState,secondOrderState,thirdOrderState);"
            "lightDepths+=lightSegmentDepth;"));
        EXPECT_FALSE(Contains(cacheBody, "cloudShapeFromMacro(lightMacro)"));
    }

    // 代表的な凝結場を正値化した後で物理補正を掛け、上層の濃さを一度だけ適用する。
    constexpr f32 kShapePotential = 0.60f;
    constexpr f32 kWeatherMask = 0.80f;
    constexpr f32 kVerticalProfile = 0.75f;
    constexpr f32 kHeight = 0.0f;
    constexpr f32 kPrecipitation = 1.0f;
    const FVolumetricCloudUpperLayer upper{};
    const f32 heightPrecipitationScale =
        (1.10f + (0.92f - 1.10f) * kHeight) *
        (1.0f + (1.28f - 1.0f) * kPrecipitation);
    const f32 condensationPotential =
        render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
            kShapePotential, kWeatherMask, kVerticalProfile, 0.0f);
    const f32 condensationDensity =
        render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
            condensationPotential);
    const f32 lowerDensity = condensationDensity * heightPrecipitationScale;
    const f32 upperDensity = lowerDensity * upper.density_scale;
    EXPECT_TRUE(condensationPotential > 0.0f);
    EXPECT_TRUE(lowerDensity > 0.0f);
    EXPECT_NEAR(
        upperDensity / lowerDensity, upper.density_scale, 1.0e-6f);
    EXPECT_FALSE(Contains(
        shader, "densityResult*=cloudUpperTerms.x"));

    // Beer-Lambert則の光学的深さは、m^-1の消散係数を実距離で積分する。
    // 基準2500mの従来値だけを維持し、薄層と厚層を同じ1.6へ正規化しない。
    constexpr f32 kThinLayerThickness = 900.0f;
    constexpr f32 kReferenceLayerThickness = 2500.0f;
    constexpr f32 kCumulonimbusThickness = 9400.0f;
    constexpr f32 kReferenceDepth = 1.6f;
    constexpr f32 kExtinctionPerMeter =
        kVolumetricCloudReferenceExtinctionPerMeter;
    const auto opticalDepth = [](f32 distance) noexcept {
        return distance * kVolumetricCloudReferenceExtinctionPerMeter;
    };
    const f32 thinDepth = opticalDepth(kThinLayerThickness);
    const f32 referenceDepth = opticalDepth(kReferenceLayerThickness);
    const f32 cumulonimbusDepth = opticalDepth(kCumulonimbusThickness);
    EXPECT_NEAR(kExtinctionPerMeter, 0.00064f, 1.0e-8f);
    EXPECT_NEAR(thinDepth, 0.576f, 1.0e-6f);
    EXPECT_NEAR(referenceDepth, kReferenceDepth, 1.0e-6f);
    EXPECT_NEAR(cumulonimbusDepth, 6.016f, 1.0e-6f);
    EXPECT_TRUE(thinDepth < referenceDepth);
    EXPECT_TRUE(referenceDepth < cumulonimbusDepth);
    EXPECT_NEAR(
        cumulonimbusDepth / referenceDepth,
        kCumulonimbusThickness / kReferenceLayerThickness,
        1.0e-6f);
    EXPECT_TRUE(
        std::exp(-cumulonimbusDepth) < std::exp(-referenceDepth));

    // 視線、中心光、太陽面キャッシュ、ワールド影の全積分で標本側の尺度を使う。
    EXPECT_TRUE(Contains(
        shader,
        "lightDepths+=lightSegmentDepth;"));
    EXPECT_TRUE(Contains(
        shader,
        "opticalDepth+=sampleDepth;"));
    EXPECT_TRUE(Contains(shader, "floatsegmentDepth=columnDensity*lowerCellWorldStep*cloudOpticalDepthScaleFromBand(false)*ambientExtinction;"));
    EXPECT_TRUE(Contains(
        shader,
        "float3lightSegmentDepth="
        "cloudLowLodOpticalDepthByOrderFromMacro("));
    EXPECT_TRUE(Contains(
        shader,
        "floatviewOpticalDepth=cloudOpticalDepthFromAbsorption("
        "firstIntervalAbsorption);"));
    EXPECT_FALSE(Contains(
        shader,
        "lightDepths+=lightDensity*segmentScale"));
    EXPECT_FALSE(Contains(
        shader,
        "float4viewOpticalDepthLanes=densityLanes*"));
    EXPECT_FALSE(Contains(shader, "lightDepth+=lightDensity*lightStep*layer.w;"));
    EXPECT_FALSE(Contains(shader, "opticalDepth+=sampleDensity*stepLength*layer.w;"));
    EXPECT_FALSE(Contains(shader, "dens*stepLength*layer.w"));
}

ACS_TEST(VolumetricClouds, LightProbePhaseCoversEveryDepthBandWithoutRepeatingOnePlane)
{
    /** 通常の視線採取を16区画へ数えた分布。 */
    u32 phaseBins[16]{};
    /** 各区画へ入る理想個数。 */
    constexpr u32 kExpectedPerBin = kVolumetricCloudViewSteps / 16u;
    /** 黄金比の小数部。隣接する採取点の位相を一定量ずらす。 */
    constexpr f32 kGoldenFraction = 0.61803398875f;
    for (u32 sample = 0u; sample < kVolumetricCloudViewSteps; ++sample) {
        /** 参照描画の区間中央から進めた未折り返し位相。 */
        const f32 unfoldedPhase =
            0.5f + static_cast<f32>(sample) * kGoldenFraction;
        /** HLSL の frac と同じ 0 以上 1 未満の位相。 */
        const f32 phase = unfoldedPhase - std::floor(unfoldedPhase);
        /** 位相の均一性を検査する区画番号。 */
        const u32 bin = static_cast<u32>(phase * 16.0f);
        EXPECT_TRUE(bin < 16u);
        if (bin < 16u) ++phaseBins[bin];
    }

    for (u32 bin = 0u; bin < 16u; ++bin) {
        EXPECT_TRUE(phaseBins[bin] + 1u >= kExpectedPerBin);
        EXPECT_TRUE(phaseBins[bin] <= kExpectedPerBin + 1u);
    }
}

ACS_TEST(VolumetricClouds,
         BandLimitedViewAndLightDensityRemainExplicit) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    const auto functionBody =
        [&shader](const char* declaration) {
            const std::size_t begin = shader.find(declaration);
            const std::size_t end =
                shader.find("returnmacro;}", begin);
            if (begin == std::string::npos ||
                end == std::string::npos) {
                return std::string{};
            }
            return shader.substr(begin, end + 12u - begin);
        };
    const std::string commonMacro = functionBody(
        "CloudMacroSamplesampleCloudMacroFromThreshold("
        "float3p,floatcoverageThreshold,floatinverseTransitionWidth,"
        "float3longitudinalFootprint,float3transverseFootprint){");
    EXPECT_TRUE(!commonMacro.empty());
    EXPECT_FALSE(Contains(
        shader, "sampleCloudMacroLightingFromSlowFields("));

    // 視線・光線とも担当する物理幅で周波数を制限し、経路ごとに別の密度場を作らない。
    EXPECT_FALSE(Contains(shader, "cloudPositiveDensityNoiseThreshold("));
    EXPECT_FALSE(Contains(shader, "rejectionThreshold"));
    EXPECT_FALSE(Contains(commonMacro, "macro.heightThreshold"));
    EXPECT_FALSE(Contains(commonMacro, "densityHeightThreshold"));
    EXPECT_TRUE(Contains(
        commonMacro,
        "macro.weather=cloudWeatherDataBandLimitedPoint(p,safeFootprint.xz);"));
    EXPECT_TRUE(Contains(commonMacro, "macro.curl=cloudCurlOffset(p,safeFootprint.xz);"));
    EXPECT_TRUE(Contains(
        commonMacro,
        "macro.shapePotential=cloudShapePotentialBands("
        "cloudUVW(p,macro.layerHeight,upperBand));"));
    EXPECT_TRUE(Contains(
        commonMacro,
        "macro.shapeFrequencyWeights=cloudShapeFrequencyWeights("
        "maximumDomainFootprint);"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudMacroSamplesampleCloudMacroLighting("
        "float3p,floatweatherCoverage){"
        "returnsampleCloudMacroLightingBandLimited("
        "p,weatherCoverage,0.0.xxx,0.0.xxx);}"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudMacroSamplesampleCloudMacroLightingSegment("
        "float3p,floatweatherCoverage,float3rayDirection,"
        "floatsampleSpacing){"
        "float3physicalFootprint="
        "abs(rayDirection)*max(sampleSpacing,0.0);"
        "returnsampleCloudMacroLightingBandLimited("
        "p,weatherCoverage,physicalFootprint,0.0.xxx);}"));
    EXPECT_TRUE(Contains(
        shader,
        "float3physicalFootprint=float3("
        "max(horizontalFootprint.x,0.0),max(verticalStep,0.0),"
        "max(horizontalFootprint.y,0.0));"
        "CloudMacroSamplemacro=sampleCloudMacroLightingBandLimited("
        "p,coverage,physicalFootprint,0.0.xxx);"));

    const std::size_t occupancyBegin =
        shader.find("float2cloudShapeOccupancyAtInterval(");
    const std::size_t occupancyEnd =
        shader.find("structCloudMacroSample", occupancyBegin);
    const std::size_t densityBegin = shader.find(
        "float4cloudDensityDistributionFromPositiveWeatherMacro(",
        occupancyEnd);
    const std::size_t densityEnd =
        shader.find("returndensityDistribution;}", densityBegin);
    EXPECT_TRUE(occupancyBegin != std::string::npos);
    EXPECT_TRUE(occupancyEnd != std::string::npos);
    EXPECT_TRUE(densityBegin != std::string::npos);
    EXPECT_TRUE(densityEnd != std::string::npos);
    if (occupancyBegin != std::string::npos &&
        occupancyEnd != std::string::npos) {
        const std::string occupancyBody =
            shader.substr(occupancyBegin, occupancyEnd - occupancyBegin);
        EXPECT_TRUE(Contains(
            occupancyBody,
            "floatmaximumDomainFootprint="
            "cloudShapeMaximumDomainFootprint(physicalFootprint,upperBand);"));
        EXPECT_TRUE(Contains(
            occupancyBody,
            "floatunresolvedDensityUpper=cloudCondensationDensity("
            "CLOUD_UNRESOLVED_COARSE_OUTER_POTENTIAL"
            "+maximumPositiveOffset);"));
        EXPECT_TRUE(Contains(
            occupancyBody,
            "floatdensityUpper=pointOwner*pointDensityUpper"
            "+max(frequencyWeights.w,0.0)*unresolvedDensityUpper;"));
        EXPECT_FALSE(Contains(occupancyBody, "cloudWeather"));
        EXPECT_FALSE(Contains(occupancyBody, "cloudProfile"));
        EXPECT_FALSE(Contains(occupancyBody, "cloudCurl"));
        EXPECT_FALSE(Contains(occupancyBody, "cloudDetail"));
    }
    if (densityBegin != std::string::npos &&
        densityEnd != std::string::npos) {
        const std::string densityBody =
            shader.substr(
                densityBegin, densityEnd - densityBegin);
        EXPECT_TRUE(Contains(
            densityBody,
            "floatcommonPotential=cloudCondensationCommonPotential("
            "macro.heightProfile,weatherMask,"));
        EXPECT_TRUE(Contains(
            densityBody,
            "cloudDetailFilteredCondensationDistribution("
            "macro.shapePotential,macro.shapeFrequencyWeights,"
            "commonPotential,billowPotentials,"));
        EXPECT_FALSE(Contains(densityBody, "cloudShapeOccupancyAtInterval("));
        EXPECT_FALSE(Contains(densityBody, "occupancyBaseNoise"));
        EXPECT_FALSE(Contains(densityBody, "remapc("));
    }

    // 雲量と縦分布は符号付き形状を再正規化せず、加算する偏りだけを変える。
    const f32 coverages[] = {0.0f, 0.08f, 0.37f, 0.72f, 1.0f};
    const f32 profileShapes[] = {0.0f, 0.01f, 0.5f, 0.99f, 1.0f};
    for (const f32 coverage : coverages) {
        for (const f32 profileShape : profileShapes) {
            const f32 lowShape =
                render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
                    -0.25f, coverage, profileShape, 0.0f);
            const f32 highShape =
                render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
                    0.25f, coverage, profileShape, 0.0f);
            EXPECT_NEAR(highShape - lowShape, 0.50f, 1.0e-6f);
        }
    }

    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 384u);
    EXPECT_EQ(kVolumetricCloudMaxLightMarchSamples, 16u);
    EXPECT_TRUE(Contains(shader, "staticconstintCLOUD_LIGHT_MARCH_SAMPLE_COUNT=16;"));
    EXPECT_TRUE(Contains(shader, "staticconstintCLOUD_LIGHT_DETAIL_SAMPLE_COUNT=3;"));
    EXPECT_TRUE(Contains(shader, "sampleIndex<CLOUD_LIGHT_MARCH_SAMPLE_COUNT;"));
    EXPECT_TRUE(Contains(shader, "sampleIndex<CLOUD_LIGHT_DETAIL_SAMPLE_COUNT;"));
}

ACS_TEST(VolumetricClouds,
         DenseRaySamplesRebuildLightFieldsAndPreservePhysicalDomains) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    const std::size_t coverageTerms = shader.find(
        "float4coverageTerms=cloudCoverage;");
    const std::size_t viewLoop =
        shader.find("[loop]for(inti=0;i<MAX_STEPS", coverageTerms);
    const std::size_t mainLightPattern =
        shader.find("float3traceCloudMainLightDepths(");
    const std::size_t shadowLightPattern =
        shader.find("float3traceCloudShadowDepths(");
    EXPECT_TRUE(coverageTerms != std::string::npos);
    EXPECT_TRUE(viewLoop != std::string::npos);
    EXPECT_TRUE(mainLightPattern != std::string::npos);
    EXPECT_TRUE(shadowLightPattern != std::string::npos);
    EXPECT_TRUE(coverageTerms < viewLoop);
    EXPECT_FALSE(Contains(shader, "sharedLightProfileTerms"));
    EXPECT_FALSE(Contains(shader, "sharedLightColumnTerms"));
    EXPECT_FALSE(Contains(shader, "sharedLightWeatherTerms"));
    EXPECT_FALSE(Contains(shader, "sharedLightCurl"));
    if (viewLoop != std::string::npos) {
        const std::string denseRay = shader.substr(viewLoop);
        EXPECT_FALSE(Contains(denseRay, "cloudWeatherThreshold("));
        EXPECT_FALSE(Contains(denseRay, "cloudHeightThresholdTarget("));
    }
    EXPECT_TRUE(Contains(
        shader,
        "CloudMacroSamplemacro=sampleCloudMacroLightingSegment("
        "samplePosition,coverage,lightDirection,sampleSpacing);"));
    EXPECT_FALSE(Contains(shader,"sampleCloudFarLightingDensityAndScale("));

    EXPECT_FALSE(Contains(
        shader, "sampleCloudMacroLightingFromSlowFields("));

    EXPECT_FALSE(Contains(
        shader, "floatsampleCloudLightingDensityFromSlowFields("));

    const std::size_t mainLightEnd=
        shader.find("returnlightDepths;}",mainLightPattern);
    EXPECT_TRUE(mainLightEnd!=std::string::npos);
    if(mainLightPattern!=std::string::npos&&
       mainLightEnd!=std::string::npos){
        const std::string helper=shader.substr(
            mainLightPattern,mainLightEnd-mainLightPattern);
        EXPECT_TRUE(Contains(helper,"intersectCloudBandsFromPosition("));
        EXPECT_TRUE(Contains(helper,"cloudAdaptiveLightSampleTerms("));
        EXPECT_FALSE(Contains(
            helper,
            "cloudLowLodOpticalDepthByOrderFromMacro("));
        EXPECT_TRUE(Contains(
            helper,
            "cloudDensityOpticalDepthByOrderFromMacro("));
        EXPECT_TRUE(Contains(
            helper,
            "lightDepths+=lightSegmentDepth;"));
        EXPECT_FALSE(Contains(helper,"sharedLightCurl"));
    }
    const std::size_t residualBegin=shader.find(
        "float3cloudNearLightOpticalDepthResiduals(");
    const std::size_t residualEnd=shader.find(
        "returnresiduals;}",residualBegin);
    EXPECT_TRUE(residualBegin!=std::string::npos);
    EXPECT_TRUE(residualEnd!=std::string::npos);
    if(residualBegin!=std::string::npos&&
       residualEnd!=std::string::npos){
        const std::string residual=shader.substr(
            residualBegin,residualEnd-residualBegin);
        EXPECT_TRUE(Contains(residual,"cloudLightSampleTerms("));
        EXPECT_TRUE(Contains(
            residual,
            "cloudDensityOpticalDepthByOrderFromMacro("));
        EXPECT_TRUE(Contains(
            residual,
            "cloudLowLodOpticalDepthByOrderFromMacro("));
    }

    // 基本形状の座標を非線形に曲げると、最大値階層が担当区間を包めなくなる。
    EXPECT_FALSE(Contains(shader, "weatherWarp"));
    EXPECT_FALSE(Contains(shader, "warpAngle"));
    EXPECT_FALSE(Contains(
        shader,
        "float2weatherWarp=float2(cos(warpAngle),sin(warpAngle))"));
    EXPECT_TRUE(Contains(
        shader,
        "voidcloudDetailDomains("
        "float2detailXz,floataltitude,"
        "outfloat3detailDomainA,outfloat3detailDomainB)"));
    EXPECT_TRUE(Contains(
        shader,
        "cloudDetailDomains("
        "detailXz,macro.altitude,detailDomainA,detailDomainB);"));
    EXPECT_FALSE(Contains(shader, "rotateNoise(detailPosA)"));
    EXPECT_FALSE(Contains(shader, "rotateNoise(detailPosB)"));

    const std::size_t densityBegin = shader.find(
        "float4cloudDensityDistributionFromPositiveWeatherMacro(");
    const std::size_t densityEnd =
        shader.find("returndensityDistribution;}", densityBegin);
    EXPECT_TRUE(densityBegin != std::string::npos);
    EXPECT_TRUE(densityEnd != std::string::npos);
    if (densityBegin != std::string::npos &&
        densityEnd != std::string::npos) {
        EXPECT_EQ(
            CountOccurrences(
                shader.substr(
                    densityBegin, densityEnd - densityBegin),
                "detailNoise.SampleLevel("),
            static_cast<std::size_t>(2));
    }

    const auto lowCloudProfile = [](f32 h, f32 typeWeightA,
                                    f32 typeWeightB) noexcept {
        const f32 stratus =
            SmoothStepForTest(0.0f, 0.055f, h) *
            (1.0f - SmoothStepForTest(0.38f, 0.50f, h));
        const f32 stratocumulus =
            SmoothStepForTest(0.0f, 0.09f, h) *
            (1.0f - SmoothStepForTest(0.72f, 0.90f, h)) *
            (0.78f + 0.22f *
                SmoothStepForTest(0.08f, 0.42f, h));
        const f32 cumulus =
            SmoothStepForTest(0.0f, 0.13f, h) *
            (1.0f - SmoothStepForTest(0.62f, 0.94f, h)) *
            (0.64f + 0.36f *
                SmoothStepForTest(0.12f, 0.52f, h));
        f32 value =
            stratus + (stratocumulus - stratus) * typeWeightA;
        value = value + (cumulus - value) * typeWeightB;
        return SaturateForTest(value);
    };
    const f32 heights[] = {0.0f, 0.03f, 0.19f, 0.51f, 0.83f, 1.0f};
    const f32 cloudTypes[] = {0.0f, 0.23f, 0.51f, 0.77f, 1.0f};
    for (const f32 cloudType : cloudTypes) {
        const f32 cachedA =
            SmoothStepForTest(0.18f, 0.52f, cloudType);
        const f32 cachedB =
            SmoothStepForTest(0.50f, 0.84f, cloudType);
        for (const f32 h : heights) {
            const f32 former = lowCloudProfile(
                h,
                SmoothStepForTest(0.18f, 0.52f, cloudType),
                SmoothStepForTest(0.50f, 0.84f, cloudType));
            const f32 hoisted =
                lowCloudProfile(h, cachedA, cachedB);
            EXPECT_NEAR(hoisted, former, 0.0f);
        }
    }

    const auto rotateDetail = [](FVec3 q) noexcept {
        return FVec3{
            q.y * 0.8000000f + q.z * 0.6000000f,
            q.x * -0.7071068f + q.y * -0.4242641f +
                q.z * 0.5656854f,
            q.x * 0.7071068f + q.y * -0.4242641f +
                q.z * 0.5656854f};
    };
    const auto distance = [](FVec3 a, FVec3 b) noexcept {
        return std::sqrt(
            (a.x - b.x) * (a.x - b.x) +
            (a.y - b.y) * (a.y - b.y) +
            (a.z - b.z) * (a.z - b.z));
    };
    const FVec3 detailOrigin = rotateDetail(FVec3{0.0f, 5000.0f, 0.0f});
    const FVec3 detailHorizontal = rotateDetail(FVec3{1000.0f, 5000.0f, 0.0f});
    const FVec3 detailVertical = rotateDetail(FVec3{0.0f, 6000.0f, 0.0f});
    EXPECT_NEAR(
        distance(detailOrigin, detailHorizontal),
        distance(detailOrigin, detailVertical), 2.0e-3f);
    EXPECT_NEAR(
        distance(detailOrigin, detailHorizontal) * 0.00018f,
        0.18f, 2.0e-6f);
    EXPECT_NEAR(
        distance(detailOrigin, detailVertical) * 0.00031f,
        0.31f, 2.0e-6f);

    // 同じ球殻高度5000 mでも、接平面原点から100 km離れると世界Yは約786 m下がる。
    // 詳細位相へ世界Yを使う旧式はこの差を雲形状へ混入するが、球殻高度なら差は0である。
    constexpr f64 kPlanetRadius = 6360000.0;
    constexpr f64 kShellAltitude = 5000.0;
    constexpr f64 kHorizontalDistance = 100000.0;
    const f64 radialY = std::sqrt(
        (kPlanetRadius + kShellAltitude) *
        (kPlanetRadius + kShellAltitude) -
        kHorizontalDistance * kHorizontalDistance) - kPlanetRadius;
    const f64 recoveredAltitude = std::sqrt(
        kHorizontalDistance * kHorizontalDistance +
        (kPlanetRadius + radialY) * (kPlanetRadius + radialY)) -
        kPlanetRadius;
    EXPECT_TRUE(kShellAltitude - radialY > 780.0);
    EXPECT_NEAR(recoveredAltitude, kShellAltitude, 1.0e-6);
    EXPECT_TRUE((kShellAltitude - radialY) * 0.00018 > 0.14);

    EXPECT_TRUE(Contains(
        shader,
        "intMAX_STEPS=(int)cloudLightingAmbient.z;"
        "if(MAX_STEPS<CLOUD_MIN_VIEW_MARCH_SAMPLE_COUNT)MAX_STEPS=CLOUD_MIN_VIEW_MARCH_SAMPLE_COUNT;"));
    EXPECT_TRUE(Contains(shader, "sampleIndex<CLOUD_LIGHT_DETAIL_SAMPLE_COUNT;"));
    EXPECT_TRUE(Contains(shader, "sampleIndex<CLOUD_LIGHT_MARCH_SAMPLE_COUNT;"));
    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 384u);
    EXPECT_EQ(kVolumetricCloudMaxLightMarchSamples, 16u);
    EXPECT_EQ(kVolumetricCloudUltraTraceDivisor, 4u);
}

ACS_TEST(VolumetricClouds,
         LightSamplesUseBandLimitedSegmentFieldsAndPreserveWorldDomain) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    // 空間棄却は担当区間の支持域を二値包絡で判断し、点密度や天候で薄い縁を捨てない。
    const std::size_t shapeEnvelope = shader.find(
        "float2occupancySample=cloudShapeOccupancyAtInterval("
        "occupancyP,occupancyPhysicalFootprint);");
    const std::size_t completedUpperBound = shader.find(
        "floatoccupancyDensityUpperBound=occupancySample.x"
        "*cloudHeightPrecipitationDensityScale(0.0,1.0)"
        "*density;",
        shapeEnvelope);
    const std::size_t upperLayerScale = shader.find(
        "if(occupancySample.y>0.5)"
        "occupancyDensityUpperBound*=cloudUpperTerms.y;",
        completedUpperBound);
    const std::size_t emptySpaceReject = shader.find(
        "if(occupancyDensityUpperBound<=0.0){",
        upperLayerScale);
    EXPECT_TRUE(shapeEnvelope != std::string::npos);
    EXPECT_TRUE(shapeEnvelope < completedUpperBound);
    EXPECT_TRUE(completedUpperBound < upperLayerScale);
    EXPECT_TRUE(emptySpaceReject != std::string::npos);
    EXPECT_TRUE(upperLayerScale < emptySpaceReject);
    EXPECT_TRUE(Contains(
        shader,
        "firstOrderTransportState.active=0.0.xxxx;"
        "secondOrderTransportState.active=0.0.xxxx;"
        "thirdOrderTransportState.active=0.0.xxxx;"));
    EXPECT_FALSE(Contains(
        shader,
        "if(occupancyDensityUpperBound<=0.0015){"));
    EXPECT_FALSE(Contains(shader, "if(shape<=0.006){"));
    EXPECT_FALSE(Contains(shader, "if(macro.weatherMask*macro.heightProfile>0.006){"));
    constexpr f32 kOccupancyEnvelope = 1.0f;
    constexpr f32 kMaximumHeightPrecipitationScale = 1.10f * 1.28f;
    constexpr f32 kAuthoredDensity = 1.8f;
    const f32 visibleUpperBound =
        kOccupancyEnvelope * kMaximumHeightPrecipitationScale * kAuthoredDensity;
    EXPECT_TRUE(visibleUpperBound > 0.0f);

    EXPECT_FALSE(Contains(
        shader, "sampleCloudMacroLightingFromSlowFields("));
    const std::size_t helperBegin = shader.find(
        "CloudMacroSamplesampleCloudMacroFromThreshold("
        "float3p,floatcoverageThreshold,floatinverseTransitionWidth,"
        "float3longitudinalFootprint,float3transverseFootprint){");
    const std::size_t helperEnd =
        shader.find("returnmacro;}", helperBegin);
    EXPECT_TRUE(helperBegin != std::string::npos);
    EXPECT_TRUE(helperEnd != std::string::npos);
    if (helperBegin != std::string::npos &&
        helperEnd != std::string::npos) {
        const std::string helper =
            shader.substr(helperBegin, helperEnd - helperBegin);
        EXPECT_TRUE(Contains(
            helper,
            "macro.weather=cloudWeatherDataBandLimitedPoint("
            "p,safeFootprint.xz);"));
        EXPECT_TRUE(Contains(
            helper,
            "macro.curl=cloudCurlOffset("
            "p,safeFootprint.xz);"));
        EXPECT_TRUE(Contains(helper, "macro.height=layerHeight;"));
        EXPECT_TRUE(Contains(
            helper, "macro.shapePotential=cloudShapePotentialBands("));
        EXPECT_TRUE(Contains(
            helper,
            "macro.shapeFrequencyWeights=cloudShapeFrequencyWeights("));
        EXPECT_FALSE(Contains(helper, "cloudColumnTopShift("));
        EXPECT_FALSE(Contains(helper, "cloudColumnHeightAndSpan("));
        EXPECT_FALSE(Contains(helper, "camPos"));
        EXPECT_TRUE(Contains(helper, "cloudUVW("));
    }
    EXPECT_TRUE(Contains(
        shader,
        "CloudMacroSamplemacro=sampleCloudMacroLightingSegment("
        "samplePosition,coverage,lightDirection,sampleSpacing);"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudMacroSamplesampleCloudMacroLighting("
        "float3p,floatweatherCoverage){"
        "returnsampleCloudMacroLightingBandLimited("
        "p,weatherCoverage,0.0.xxx,0.0.xxx);}"));

    // 同じ未加工天候でも、積乱雲のくびれとかなとこを跨ぐと被覆は高さごとに変わる。
    constexpr f32 kWeatherCoverage = 0.50f;
    constexpr f32 kWeatherThreshold = 0.54f;
    constexpr f32 kInverseTransition = 1.0f / 0.14f;
    const f32 waistMask = CloudWeatherMaskForLayerForTest(kWeatherCoverage, kWeatherThreshold, kInverseTransition, 0.20f, 1.0f);
    const f32 anvilMask = CloudWeatherMaskForLayerForTest(kWeatherCoverage, kWeatherThreshold, kInverseTransition, 0.66f, 1.0f);
    EXPECT_NEAR(waistMask, 0.0f, 0.0f);
    EXPECT_TRUE(anvilMask > 0.10f);
    EXPECT_TRUE(anvilMask > waistMask);

    // 主3D座標は、風移流と高度せん断だけを持つ物理距離基準の写像にする。
    // 天候・渦・局所的な列の高さを座標へ混ぜず、直交回転の前後で距離を保存する。
    const FVec2 wind{183.25f, -91.75f};
    constexpr f32 kShapeScale = 0.000105f;
    constexpr f32 kLayerThickness = 9400.0f;
    constexpr f32 kInverseLayerHeight = 1.0f / kLayerThickness;
    const f32 kVerticalSpan = CloudShapeVerticalSpanForTest(
        kShapeScale, kInverseLayerHeight);
    const auto rotateCanonical =
        [](FVec3 value) noexcept {
            return FVec3{
                value.y * 0.8000000f + value.z * 0.6000000f,
                value.x * -0.7071068f + value.y * -0.4242641f +
                    value.z * 0.5656854f,
                value.x * 0.7071068f + value.y * -0.4242641f +
                    value.z * 0.5656854f};
        };
    const auto unrotatedUvw =
        [&](FVec3 point, f32 height, f32 verticalSpan,
            bool upperBand) noexcept {
            const f32 boundedHeight = SaturateForTest(height);
            const f32 bandScale = upperBand ? 0.25f : 1.0f;
            const FVec2 shear{
                0.9284767f * 850.0f * boundedHeight * bandScale,
                0.3713907f * 850.0f * boundedHeight * bandScale};
            return FVec3{
                (point.x - wind.x + shear.x) * kShapeScale,
                boundedHeight * verticalSpan + 0.07f,
                (point.z - wind.y + shear.y) * kShapeScale};
        };
    const auto absoluteUvw =
        [&](FVec3 point, f32 height, f32 verticalSpan,
            bool upperBand) noexcept {
            return rotateCanonical(
                unrotatedUvw(point, height, verticalSpan, upperBand));
        };
    EXPECT_NEAR(kVerticalSpan, 0.987f, 1e-6f);
    EXPECT_NEAR(CloudShapeVerticalSpanForTest(
        kShapeScale, 1.0f / 2600.0f), 0.273f, 1e-6f);
    EXPECT_NEAR(CloudShapeVerticalSpanForTest(
        0.00020f, 1.0f / 12000.0f), 2.40f, 1e-6f);
    struct FUvwCase {
        FVec3 reference;
        f32 reference_height;
        f32 reference_vertical_span;
        FVec3 probe;
        f32 probe_height;
        f32 probe_vertical_span;
        bool upper_band;
    };
    const FUvwCase uvwCases[]{
        {{0.0f, 1500.0f, 0.0f}, 0.0f, kVerticalSpan,
         {12.0f, 1600.0f, -8.0f}, 0.04f, kVerticalSpan, false},
        {{-48000.0f, 2300.0f, 62000.0f}, 0.32f, kVerticalSpan,
         {-47912.0f, 2650.0f, 62141.0f}, 0.46f, kVerticalSpan, false},
        {{249500.0f, 15000.0f, -249200.0f}, 0.08f, 0.21f,
         {249740.0f, 16500.0f, -248910.0f}, 0.74f, 0.21f, true}};
    for (const FUvwCase& uvwCase : uvwCases) {
        const FVec3 unrotatedReference = unrotatedUvw(
            uvwCase.reference, uvwCase.reference_height,
            uvwCase.reference_vertical_span, uvwCase.upper_band);
        const FVec3 unrotatedProbe = unrotatedUvw(
            uvwCase.probe, uvwCase.probe_height,
            uvwCase.probe_vertical_span, uvwCase.upper_band);
        const FVec3 rotatedReference = absoluteUvw(
            uvwCase.reference, uvwCase.reference_height,
            uvwCase.reference_vertical_span, uvwCase.upper_band);
        const FVec3 rotatedProbe = absoluteUvw(
            uvwCase.probe, uvwCase.probe_height,
            uvwCase.probe_vertical_span, uvwCase.upper_band);
        const auto distance = [](FVec3 a, FVec3 b) noexcept {
            const f32 x = a.x - b.x;
            const f32 y = a.y - b.y;
            const f32 z = a.z - b.z;
            return std::sqrt(x * x + y * y + z * z);
        };
        EXPECT_NEAR(
            distance(unrotatedReference, unrotatedProbe),
            distance(rotatedReference, rotatedProbe), 2.0e-3f);
    }

    // 1000 mの高さ差は正規化層厚に依存せず、形状領域でも1000 m分だけ進む。
    const FVec3 verticalStart = unrotatedUvw(
        FVec3{}, 0.0f, kVerticalSpan, false);
    const FVec3 verticalKilometer = unrotatedUvw(
        FVec3{}, 1000.0f / kLayerThickness, kVerticalSpan, false);
    EXPECT_NEAR(
        verticalKilometer.y - verticalStart.y,
        1000.0f * kShapeScale, 1.0e-6f);
    EXPECT_NEAR(
        verticalKilometer.x - verticalStart.x,
        0.9284767f * 850.0f * (1000.0f / kLayerThickness) *
            kShapeScale,
        1.0e-6f);
    EXPECT_NEAR(
        verticalKilometer.z - verticalStart.z,
        0.3713907f * 850.0f * (1000.0f / kLayerThickness) *
            kShapeScale,
        1.0e-6f);

    // 共有場から作った構造体も、同じ加算順とC1正値化を使う。
    // 形状差は天候と縦分布を変えても再正規化されず、そのまま残る。
    struct FScalarCase {
        f32 shape_potential;
        f32 weather_mask;
        f32 vertical_profile;
    };
    const FScalarCase scalarCases[]{
        {-1.0f, 0.0f, 0.0f},
        {-0.31f, 0.35f, 0.42f},
        {0.04f, 0.81f, 0.67f},
        {0.60f, 1.0f, 1.0f},
        {1.0f, 0.13f, 0.88f}};
    for (const FScalarCase& scalarCase : scalarCases) {
        const f32 potential =
            render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
                scalarCase.shape_potential, scalarCase.weather_mask,
                scalarCase.vertical_profile, 0.0f);
        const f32 denserPotential =
            render_internal::ResolveVolumetricCloudCondensationPotential_Internal(
                scalarCase.shape_potential + 0.10f,
                scalarCase.weather_mask,
                scalarCase.vertical_profile, 0.0f);
        const f32 density =
            render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
                potential);
        const f32 denserDensity =
            render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
                denserPotential);
        EXPECT_TRUE(std::isfinite(density));
        EXPECT_TRUE(density >= 0.0f);
        EXPECT_TRUE(denserDensity >= density);
    }

    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 384u);
    EXPECT_EQ(kVolumetricCloudMaxLightMarchSamples, 16u);
    EXPECT_EQ(kVolumetricCloudUltraTraceDivisor, 4u);
}

ACS_TEST(VolumetricClouds, CurvedBandRayInvariantQuadraticTermsAreCpuHoisted) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    const std::string compactSource = CompactShader(source);
    EXPECT_TRUE(!shader.empty());

    EXPECT_TRUE(Contains(shader, "float4cloudShellRayOrigin;"));
    EXPECT_TRUE(Contains(shader, "float4cloudShellTerms;"));
    EXPECT_TRUE(Contains(
        shader,
        "boolsphereRootsFromTerms(floatb,floatc,"
        "boolacceptRoundedOuterTangent,"
        "outfloatnearT,outfloatfarT){"));
    EXPECT_FALSE(Contains(
        shader,
        "boolsphereRoots(float3localOrigin,float3rd,floataltitude,"));
    EXPECT_TRUE(Contains(
        shader,
        "intintersectCloudShellTermsAll("
        "floatb,floatinnerC,floatouterC,floatrayEnd,"
        "outfloat4intervals){"));
    EXPECT_TRUE(Contains(
        shader,
        "boolhitsOuter=sphereRootsFromTerms("
        "b,outerC,true,outerNear,outerFar);"));
    EXPECT_TRUE(Contains(
        shader,
        "boolhitsInner=sphereRootsFromTerms("
        "b,innerC,false,innerNear,innerFar);"));
    EXPECT_TRUE(Contains(shader, "intintersectCloudBands(float3rayDir,outfloat4intervals,outint2bandIds){" "floatb=dot(cloudShellRayOrigin.xyz,rayDir);"));
    EXPECT_TRUE(Contains(
        shader,
        "intersectCloudShellTermsAll("
        "b,cloudShellRayOrigin.w,cloudShellTerms.x,"
        "rayEnd,result.lower);"));
    EXPECT_TRUE(Contains(
        shader,
        "intersectCloudShellTermsAll("
        "b,cloudShellTerms.y,cloudShellTerms.z,"
        "rayEnd,result.upper);"));
    EXPECT_TRUE(Contains(
        shader,
        "structCloudBandIntervalSet{float4lower;float4upper;};"));
    EXPECT_TRUE(Contains(
        shader,
        "structCloudSubrayBandIntervals{"
        "CloudBandIntervalSetlane0;"
        "CloudBandIntervalSetlane1;"
        "CloudBandIntervalSetlane2;"
        "CloudBandIntervalSetlane3;};"));
    EXPECT_TRUE(Contains(
        shader,
        "structCloudPackedBandIntervals{"
        "float4starts;float4ends;int4bandIds;intcount;};"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudPackedBandIntervalsintersectCloudSubrayBandUnion("
        "CloudPhysicalSubrayDirectionssubrayDirections,"
        "floatmaximumDistance,"));
    EXPECT_FALSE(Contains(shader, "subrayIntervals.center"));
    EXPECT_TRUE(Contains(
        shader,
        "float4lowerPair=cloudBandIntervalPairFromCandidates("
        "subrayIntervals.lane0.lower,subrayIntervals.lane1.lower,"
        "subrayIntervals.lane2.lower,subrayIntervals.lane3.lower);"));
    EXPECT_TRUE(Contains(
        shader,
        "if(gap>largestGap){largestGap=gap;"
        "splitEnd=currentEnd;splitStart=candidate.x;}"));
    EXPECT_FALSE(Contains(shader, "addCloudBandIntervalToPair"));
    EXPECT_FALSE(Contains(shader, "addCloudBandIntervalSetToPair"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudPackedBandIntervalspackedBandIntervals="
        "intersectCloudSubrayBandUnion("
        "subrayDirections,MAX_DISTANCE,subrayIntervals);"
        "if(packedBandIntervals.count<=0){"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudPhysicalSubraySegmentOverlapscellComponentOverlaps="
        "cloudPhysicalSubrayBandOverlaps("
        "subrayIntervals,physicalBandId,shellComponentIndex,"
        "cellStartT,cellEndT);"));
    EXPECT_TRUE(Contains(
        shader,
        "float4physicalLaneSelector="
        "cloudPhysicalSubraySelector(physicalLaneIndex);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatsegmentRayStart=cloudIntervalDistanceAtFraction("
        "laneComponentStartT,laneComponentEndT,segmentStart);"
        "floatsegmentRayEnd=transportSegmentIndex+1"
        ">=CLOUD_BEER_TRANSPORT_SEGMENT_COUNT"
        "?laneComponentEndT"
        ":cloudIntervalDistanceAtFraction("
        "laneComponentStartT,laneComponentEndT,segmentEnd);"
        "floatsegmentRayLength=max("
        "segmentRayEnd-segmentRayStart,0.0);"));
    EXPECT_TRUE(Contains(
        shader,
        "structCloudPhysicalSubraySegmentOverlaps{"
        "float4starts;float4ends;};"));
    EXPECT_FALSE(Contains(shader, "cellComponentOverlaps.centroids"));
    EXPECT_FALSE(Contains(
        shader,
        "cellLaneCentroid-0.5*cellLaneLength"));
    EXPECT_FALSE(Contains(shader, "segmentOverlap"));
    EXPECT_FALSE(Contains(shader, "physicalSubrayBlend"));
    EXPECT_FALSE(Contains(
        shader,
        "float3localOrigin=rayOrigin-worldOrigin.xyz;"));
    EXPECT_FALSE(Contains(
        shader,
        "floatcentreDot=dot(localOrigin,rayDir)"));

    EXPECT_TRUE(Contains(
        compactSource,
        "offsetof(FCloudCb,cloudShellRayOrigin)==416u"));
    EXPECT_TRUE(Contains(
        compactSource,
        "offsetof(FCloudCb,cloudShellTerms)==432u"));
    EXPECT_TRUE(Contains(compactSource, "sizeof(FCloudCb)==736"));
    EXPECT_TRUE(Contains(
        compactSource,
        "constFVec3shellLocalOrigin{"
        "cam_pos.x-worldOrigin.x,"
        "cam_pos.y-worldOrigin.y,"
        "cam_pos.z-worldOrigin.z};"));
    EXPECT_TRUE(Contains(
        compactSource,
        "constf32shellRadialXzSquared="
        "shellLocalOrigin.x*shellLocalOrigin.x+"
        "shellLocalOrigin.z*shellLocalOrigin.z;"));
    EXPECT_TRUE(Contains(
        compactSource,
        "cb.cloudShellRayOrigin=FVec4{"
        "shellLocalOrigin.x,"
        "shellLocalOrigin.y+kVolumetricCloudPlanetRadius,"
        "shellLocalOrigin.z,"
        "shellC(m_Layer.base_height)};"));
    EXPECT_TRUE(Contains(compactSource, "constf32upperBaseShellC=hasUpperLayer?" "shellC(m_UpperLayer.base_height):0.0f;" "constf32upperTopShellC=hasUpperLayer?" "shellC(m_UpperLayer.top_height):0.0f;"));
    EXPECT_TRUE(Contains(compactSource, "cb.cloudShellTerms=FVec4{" "shellC(m_Layer.top_height)," "upperBaseShellC,upperTopShellC,0.0f};"));
    EXPECT_TRUE(Contains(compactSource, "cb.cloudShellTerms=FVec4{" "shell_c(m_Layer.top_height)," "upper_base_shell_c,upper_top_shell_c,0.0f};"));

    struct FShellTerms {
        FVec3 from_planet_center;
        f32 inner_c;
        f32 outer_c;
    };
    const auto buildTerms =
        [](FVec3 camera, FVec3 worldOrigin,
           const FVolumetricCloudLayer& layer) noexcept {
            const FVec3 local{
                camera.x - worldOrigin.x,
                camera.y - worldOrigin.y,
                camera.z - worldOrigin.z};
            const f32 radialXzSquared =
                local.x * local.x + local.z * local.z;
            const auto c =
                [local, radialXzSquared](f32 altitude) noexcept {
                    return radialXzSquared +
                        (local.y - altitude) *
                        (2.0f * kVolumetricCloudPlanetRadius +
                         local.y + altitude);
                };
            return FShellTerms{
                FVec3{local.x,
                      local.y + kVolumetricCloudPlanetRadius,
                      local.z},
                c(layer.base_height),
                c(layer.top_height)};
        };
    const auto formerC =
        [](FVec3 camera, FVec3 worldOrigin, f32 altitude) noexcept {
            const FVec3 local{
                camera.x - worldOrigin.x,
                camera.y - worldOrigin.y,
                camera.z - worldOrigin.z};
            return local.x * local.x + local.z * local.z +
                (local.y - altitude) *
                (2.0f * kVolumetricCloudPlanetRadius +
                 local.y + altitude);
        };
    const FVolumetricCloudLayer layer{1500.0f, 4000.0f, 0.035f};
    struct FCameraCase {
        FVec3 camera;
        FVec3 world_origin;
    };
    const FCameraCase cameraCases[]{
        {{0.0f, 8.0f, 0.0f}, {0.0f, 0.0f, 0.0f}},
        {{63.5f, 2100.0f, -31.25f}, {48.0f, 0.0f, -16.0f}},
        {{250015.0f, 6200.0f, -179983.0f},
         {250000.0f, 0.0f, -180000.0f}}};
    const FVec3 directions[]{
        NormalizeForTest(FVec3{0.0f, 0.02f, 1.0f}),
        NormalizeForTest(FVec3{0.4f, 0.8f, -0.2f}),
        NormalizeForTest(FVec3{-0.7f, -0.3f, 0.5f})};
    for (const FCameraCase& cameraCase : cameraCases) {
        const FShellTerms terms =
            buildTerms(
                cameraCase.camera, cameraCase.world_origin, layer);
        EXPECT_NEAR(
            terms.inner_c,
            formerC(
                cameraCase.camera, cameraCase.world_origin,
                layer.base_height),
            0.0f);
        EXPECT_NEAR(
            terms.outer_c,
            formerC(
                cameraCase.camera, cameraCase.world_origin,
                layer.top_height),
            0.0f);
        const FVec3 local{
            cameraCase.camera.x - cameraCase.world_origin.x,
            cameraCase.camera.y - cameraCase.world_origin.y,
            cameraCase.camera.z - cameraCase.world_origin.z};
        for (const FVec3 direction : directions) {
            const f32 formerB =
                local.x * direction.x +
                local.y * direction.y +
                local.z * direction.z +
                kVolumetricCloudPlanetRadius * direction.y;
            const f32 hoistedB =
                terms.from_planet_center.x * direction.x +
                terms.from_planet_center.y * direction.y +
                terms.from_planet_center.z * direction.z;
            EXPECT_NEAR(
                hoistedB, formerB,
                std::fabs(formerB) * 2.0e-7f + 0.25f);
            EXPECT_NEAR(
                hoistedB * hoistedB - terms.inner_c,
                formerB * formerB -
                    formerC(
                        cameraCase.camera, cameraCase.world_origin,
                        layer.base_height),
                std::fabs(formerB * formerB) * 5.0e-7f + 8.0f);
            EXPECT_NEAR(
                hoistedB * hoistedB - terms.outer_c,
                formerB * formerB -
                    formerC(
                        cameraCase.camera, cameraCase.world_origin,
                        layer.top_height),
                std::fabs(formerB * formerB) * 5.0e-7f + 8.0f);
        }
    }

    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 384u);
    EXPECT_EQ(kVolumetricCloudMaxLightMarchSamples, 16u);
    EXPECT_EQ(kVolumetricCloudUltraTraceDivisor, 4u);
}

ACS_TEST(VolumetricClouds, SplitCloudBandsExcludeClearGapFromFixedSampleBudgets) {
    const FVec3 camera{0.0f, 0.0f, 0.0f};
    const FVec3 up{0.0f, 1.0f, 0.0f};
    const FVolumetricCloudLayer lowerLayer{2600.0f, 12000.0f, 0.035f};
    const FVolumetricCloudLayer upperLayer{15000.0f, 17000.0f, 0.035f};
    const FVolumetricCloudRayInterval lower = IntersectVolumetricCloudShell(camera, up, lowerLayer);
    const FVolumetricCloudRayInterval upper = IntersectVolumetricCloudShell(camera, up, upperLayer);
    EXPECT_TRUE(lower.hit);
    EXPECT_TRUE(upper.hit);
    EXPECT_NEAR(lower.enter, 2600.0f, 0.5f);
    EXPECT_NEAR(lower.exit, 12000.0f, 0.5f);
    EXPECT_NEAR(upper.enter, 15000.0f, 0.5f);
    EXPECT_NEAR(upper.exit, 17000.0f, 0.5f);

    const f32 lowerLength = lower.exit - lower.enter;
    const f32 upperLength = upper.exit - upper.enter;
    const f32 clearGap = upper.enter - lower.exit;
    const f32 occupiedLength = lowerLength + upperLength;
    const f32 enclosingLength = upper.exit - lower.enter;
    EXPECT_NEAR(clearGap, 3000.0f, 1.0f);
    EXPECT_NEAR(occupiedLength, 11400.0f, 1.0f);
    EXPECT_NEAR(enclosingLength, occupiedLength + clearGap, 1.0f);

    constexpr u32 kSampleCount = 32u;
    u32 lowerSampleCount = static_cast<u32>(Floor(static_cast<f32>(kSampleCount) * lowerLength / occupiedLength + 0.5f));
    if (lowerSampleCount < 1u) lowerSampleCount = 1u;
    if (lowerSampleCount >= kSampleCount) lowerSampleCount = kSampleCount - 1u;
    const u32 upperSampleCount = kSampleCount - lowerSampleCount;
    EXPECT_EQ(lowerSampleCount, 26u);
    EXPECT_EQ(upperSampleCount, 6u);
    const f32 lowerStep = lowerLength / static_cast<f32>(lowerSampleCount);
    const f32 upperStep = upperLength / static_cast<f32>(upperSampleCount);
    for (u32 sampleIndex = 0u; sampleIndex < lowerSampleCount; ++sampleIndex) {
        const f32 sampleDistance = lower.enter + (static_cast<f32>(sampleIndex) + 0.5f) * lowerStep;
        EXPECT_TRUE(sampleDistance >= lower.enter);
        EXPECT_TRUE(sampleDistance <= lower.exit);
        EXPECT_FALSE(sampleDistance > lower.exit && sampleDistance < upper.enter);
    }
    for (u32 sampleIndex = 0u; sampleIndex < upperSampleCount; ++sampleIndex) {
        const f32 sampleDistance = upper.enter + (static_cast<f32>(sampleIndex) + 0.5f) * upperStep;
        EXPECT_TRUE(sampleDistance >= upper.enter);
        EXPECT_TRUE(sampleDistance <= upper.exit);
        EXPECT_FALSE(sampleDistance > lower.exit && sampleDistance < upper.enter);
    }

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(shader, "intpackCloudBandIntervals("));
    EXPECT_TRUE(Contains(shader, "boollowerFirst=lowerInterval.x<=upperInterval.x;"));
    EXPECT_TRUE(Contains(shader, "int2cloudPhysicalBandSampleBudgets("));
    EXPECT_TRUE(Contains(
        shader,
        "int2physicalBandBudgets=cloudPhysicalBandSampleBudgets(MAX_STEPS);"));
    EXPECT_FALSE(Contains(shader, "floatoccupiedSpan=intervalEnd-intervalStart;"));
    EXPECT_FALSE(Contains(shader, "floatspan=bandIntervals.w-bandIntervals.x;"));
    EXPECT_TRUE(Contains(
        shader,
        "packedIntervalIndex++;"
        "if(packedIntervalIndex>=packedBandIntervals.count)break;"
        "intervalStart=packedBandIntervals.starts[packedIntervalIndex];"
        "intervalEnd=packedBandIntervals.ends[packedIntervalIndex];"
        "physicalBandId=packedBandIntervals.bandIds[packedIntervalIndex];"));
    EXPECT_TRUE(Contains(
        shader,
        "fineCellIndex=0;"
        "nearDensity=true;"
        "refineUntilCell=min(2,currentFineCellCount);"));
}

ACS_TEST(VolumetricClouds,
         FrameInvariantDensityTermsAndLightBasisAreCpuHoisted) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    const std::string compactSource = CompactShader(source);
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(Contains(
        compactSource,
        "f32shapeScale=authoredScale*0.0030f;"
        "if(shapeScale<0.00004f)shapeScale=0.00004f;"
        "if(shapeScale>0.00020f)shapeScale=0.00020f;"));

    // 視線と光の各採取で繰り返していた値は、フレームごとの定数へ移してある。
    // 高度からの層内位置も、同じ採取点で高度を再計算しない形を保つ。
    EXPECT_TRUE(Contains(shader, "float4cloudFrameTerms;"));
    EXPECT_TRUE(Contains(shader, "float4cloudEvolution;"));
    EXPECT_TRUE(Contains(shader, "float4cloudWeatherControl;"));
    EXPECT_TRUE(Contains(shader, "float4cloudLightTangent;"));
    EXPECT_TRUE(Contains(shader, "float4cloudLightBitangent;"));
    EXPECT_TRUE(Contains(shader, "floatheightFractionFromAltitude(" "floataltitude,boolupperBand){" "//FXCが分岐内の即時returnを未初期化扱いすることがあるため、" "下層の値で先に初期化し、" "//上層だけを上書きして一つの経路から返す。" "高さの式と飽和処理の順序は変えない。" "floatheight=(altitude-layer.x)*cloudFrameTerms.w;" "if(upperBand)" "height=(altitude-cloudUpperLayer.x)*cloudUpperLayer.z;" "returnsaturate(height);}"));
    EXPECT_TRUE(Contains(
        shader,
        "floatheightFraction(float3p){"
        "floataltitude=cloudAltitude(p);"
        "returnheightFractionFromAltitude("
        "altitude,inUpperCloudBandFromAltitude(altitude));}"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudShapeScale(){"
        "//CPU側でlayer.z*0.0030を0.00004～0.00020に収め、1フレームに一度だけ求めた倍率を使う。"
        "returncloudFrameTerms.z;}"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudShapeVerticalSpan(boolupperBand){"
        "floatinverseThickness=upperBand?cloudUpperLayer.z:cloudFrameTerms.w;"
        "returncloudShapeScale()/max(inverseThickness,1e-6);}"));
    EXPECT_FALSE(Contains(
        shader, "returnclamp(physicalSpan,1.20,2.20);"));
    EXPECT_TRUE(Contains(
        shader,
        "float2cloudWindWorld(){"
        "//CPU側で風の移動距離を同じ方向へ射影し、"
        "フレームごとに一度だけ求める。"
        "returncloudFrameTerms.xy;}"));
    EXPECT_FALSE(Contains(
        shader,
        "(cloudAltitude(p)-layer.x)/max(layer.y-layer.x,1e-4)"));
    EXPECT_FALSE(Contains(
        shader,
        "returnparams.z*float2(0.9284767,0.3713907);"));
    EXPECT_FALSE(Contains(
        shader,
        "returnclamp(layer.z*0.006,0.00012,0.00045);"));
    // 主レイ、内部照明キャッシュ、立体物用雲影が同じ正規化済み太陽方向を使う。
    EXPECT_EQ(CountOccurrences(shader, "float3sun=sunDir.xyz;"), static_cast<std::size_t>(3));
    EXPECT_FALSE(Contains(shader, "normalize(sunDir.xyz)"));
    EXPECT_FALSE(Contains(shader, "cloudLightBasis("));

    EXPECT_TRUE(Contains(compactSource, "sizeof(FCloudCb)==736"));
    EXPECT_TRUE(Contains(
        compactSource,
        "offsetof(FCloudCb,cloudFrameTerms)==336u"));
    EXPECT_TRUE(Contains(
        compactSource,
        "offsetof(FCloudCb,cloudLightTangent)==352u&&"
        "offsetof(FCloudCb,cloudLightBitangent)==368u"));
    EXPECT_TRUE(Contains(
        compactSource,
        "offsetof(FCloudCb,cloudEvolution)==624u"));
    EXPECT_TRUE(Contains(compactSource, "offsetof(FCloudCb,cloudWeatherControl)==640u"));
    EXPECT_TRUE(Contains(compactSource, "offsetof(FCloudCb,cloudShadowUpdate)==656u"));
    EXPECT_TRUE(Contains(compactSource, "offsetof(FCloudCb,cloudWorldShadowMap)==672u"));
    EXPECT_TRUE(Contains(compactSource, "offsetof(FCloudCb,cloudPreviousEvolution)==688u"));
    EXPECT_TRUE(Contains(compactSource, "offsetof(FCloudCb,cloudWorldShadowUpdate)==704u"));
    EXPECT_TRUE(Contains(
        compactSource,
        "cb.cloudFrameTerms=FVec4{"
        "densityFrameTerms.wind_world.x,"
        "densityFrameTerms.wind_world.y,"
        "densityFrameTerms.shape_scale,"
        "densityFrameTerms.inverse_layer_height};"));
    EXPECT_TRUE(Contains(
        compactSource,
        "cb.cloudEvolution=FVec4{"
        "evolutionFrameTerms.shape_phase.x,"
        "evolutionFrameTerms.shape_phase.y,"
        "evolutionFrameTerms.fine_phase.x,"
        "evolutionFrameTerms.fine_phase.y};"));
    EXPECT_TRUE(Contains(
        compactSource,
        "cb.cloudWeatherControl=FVec4{"
        "m_Weather.CloudType,"
        "m_Weather.CloudTypeInfluence,"
        "m_Weather.Precipitation,"
        "m_Weather.PrecipitationInfluence};"));
    EXPECT_TRUE(Contains(compactSource, "cb.cloudShadowUpdate=FVec4{" "static_cast<f32>(shadowUpdateOffsetX)," "static_cast<f32>(shadowUpdateOffsetY)," "static_cast<f32>(shadowUpdateDivisor)," "refreshAllSelfShadows?1.0f:0.0f};"));
    EXPECT_TRUE(Contains(compactSource, "cb.cloudWorldShadowUpdate=FVec4{" "static_cast<f32>(worldShadowUpdateOffsetX)," "static_cast<f32>(worldShadowUpdateOffsetY)," "static_cast<f32>(worldShadowUpdateDivisor)," "refreshAllWorldShadows?1.0f:0.0f};"));
    EXPECT_TRUE(Contains(
        compactSource,
        "cb.cloudLightTangent=FVec4{"
        "lightBasis.tangent.x,lightBasis.tangent.y,"
        "lightBasis.tangent.z,0.0f};"));
    EXPECT_TRUE(Contains(
        compactSource,
        "cb.cloudLightBitangent=FVec4{"
        "lightBasis.bitangent.x,lightBasis.bitangent.y,"
        "lightBasis.bitangent.z,0.0f};"));

    const FVolumetricCloudLayer layers[] = {
        FVolumetricCloudLayer{2500.0f, 8500.0f, 0.02f},
        FVolumetricCloudLayer{-1200.0f, -1199.5f, 0.001f},
        FVolumetricCloudLayer{100.0f, 18000.0f, 0.10f},
        FVolumetricCloudLayer{0.0f, 0.00001f, 0.075f},
    };
    const f32 windOffsets[] = {
        -500000000.0f, -1234.5f, 0.0f, 98.25f, 500000000.0f};
    for (const FVolumetricCloudLayer& layer : layers) {
        for (const f32 windOffset : windOffsets) {
            const auto expected =
                DensityFrameTermsHlslReferenceForTest(layer, windOffset);
            const auto actual =
                ResolveVolumetricCloudDensityFrameTerms(layer, windOffset);
            EXPECT_NEAR(
                actual.wind_world.x, expected.wind_world.x,
                std::fabs(expected.wind_world.x) * 1.0e-6f + 1.0e-7f);
            EXPECT_NEAR(
                actual.wind_world.y, expected.wind_world.y,
                std::fabs(expected.wind_world.y) * 1.0e-6f + 1.0e-7f);
            EXPECT_NEAR(actual.shape_scale, expected.shape_scale, 5.0e-5f);
            EXPECT_NEAR(
                actual.inverse_layer_height,
                expected.inverse_layer_height,
                std::fabs(expected.inverse_layer_height) * 1.0e-6f +
                    1.0e-7f);

            const f32 oldHeightSamples[] = {
                layer.base_height - 100.0f,
                layer.base_height,
                (layer.base_height + layer.top_height) * 0.5f,
                layer.top_height,
                layer.top_height + 100.0f};
            const f32 span =
                (layer.top_height - layer.base_height) > 1.0e-4f
                    ? layer.top_height - layer.base_height
                    : 1.0e-4f;
            for (const f32 altitude : oldHeightSamples) {
                f32 oldFraction =
                    (altitude - layer.base_height) / span;
                if (oldFraction < 0.0f) oldFraction = 0.0f;
                if (oldFraction > 1.0f) oldFraction = 1.0f;
                f32 hoistedFraction =
                    (altitude - layer.base_height) *
                    actual.inverse_layer_height;
                if (hoistedFraction < 0.0f) hoistedFraction = 0.0f;
                if (hoistedFraction > 1.0f) hoistedFraction = 1.0f;
                EXPECT_NEAR(oldFraction, hoistedFraction, 2.0e-6f);
            }
        }
    }
    const auto standardLayerTerms =
        ResolveVolumetricCloudDensityFrameTerms(
            FVolumetricCloudLayer{2600.0f, 12000.0f, 0.035f}, 0.0f);
    EXPECT_NEAR(standardLayerTerms.shape_scale, 0.000105f, 1.0e-8f);
    FVolumetricCloudLayer hostileLayer{};
    hostileLayer.top_height = std::numeric_limits<f32>::infinity();
    hostileLayer.horizontal_noise_scale =
        std::numeric_limits<f32>::quiet_NaN();
    const auto hostileTerms =
        ResolveVolumetricCloudDensityFrameTerms(
            hostileLayer, std::numeric_limits<f32>::infinity());
    EXPECT_NEAR(hostileTerms.wind_world.x, 0.0f, 0.0f);
    EXPECT_NEAR(hostileTerms.wind_world.y, 0.0f, 0.0f);
    EXPECT_TRUE(std::isfinite(hostileTerms.shape_scale));
    EXPECT_TRUE(std::isfinite(hostileTerms.inverse_layer_height));
    EXPECT_TRUE(hostileTerms.shape_scale >= 0.00004f);
    EXPECT_TRUE(hostileTerms.shape_scale <= 0.00020f);
    EXPECT_TRUE(hostileTerms.inverse_layer_height > 0.0f);

    const FVec3 sunDirections[] = {
        FVec3{0.0f, 1.0f, 0.0f},
        FVec3{0.0f, -1.0f, 0.0f},
        FVec3{0.45f, 0.82f, -0.38f},
        FVec3{-0.71f, 0.001f, 0.42f},
        FVec3{0.0001f, -0.9999f, -0.0002f},
    };
    for (const FVec3 direction : sunDirections) {
        const auto expected =
            LightBasisHlslReferenceForTest(direction);
        const auto actual =
            ResolveVolumetricCloudLightBasis(direction);
        ExpectVec3Near(actual.direction, expected.direction, 1.0e-6f);
        ExpectVec3Near(actual.tangent, expected.tangent, 1.0e-6f);
        ExpectVec3Near(actual.bitangent, expected.bitangent, 1.0e-6f);

        const auto dot = [](FVec3 a, FVec3 b) noexcept {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        };
        EXPECT_NEAR(dot(actual.direction, actual.tangent), 0.0f, 2.0e-6f);
        EXPECT_NEAR(
            dot(actual.direction, actual.bitangent), 0.0f, 2.0e-6f);
        EXPECT_NEAR(dot(actual.tangent, actual.bitangent), 0.0f, 2.0e-6f);
        EXPECT_NEAR(dot(actual.tangent, actual.tangent), 1.0f, 2.0e-6f);
        EXPECT_NEAR(
            dot(actual.bitangent, actual.bitangent), 1.0f, 2.0e-6f);

        const auto sunDiskDirection =
            [](const FVolumetricCloudLightBasis& basis,
               u32 sampleIndex) noexcept {
                constexpr f32 kOffset =
                    kSkyPhysicalSunAngularRadiusRadians * 0.5f;
                constexpr f32 kSigns[4][2]{
                    {1.0f, 1.0f}, {-1.0f, -1.0f},
                    {-1.0f, 1.0f}, {1.0f, -1.0f}};
                const f32 tangentOffset =
                    kSigns[sampleIndex & 3u][0] * kOffset;
                const f32 bitangentOffset =
                    kSigns[sampleIndex & 3u][1] * kOffset;
                const f32 inverseLength = 1.0f / Sqrt(
                    1.0f + tangentOffset * tangentOffset +
                    bitangentOffset * bitangentOffset);
                return FVec3{
                    (basis.direction.x +
                     basis.tangent.x * tangentOffset +
                     basis.bitangent.x * bitangentOffset) * inverseLength,
                    (basis.direction.y +
                     basis.tangent.y * tangentOffset +
                     basis.bitangent.y * bitangentOffset) * inverseLength,
                    (basis.direction.z +
                     basis.tangent.z * tangentOffset +
                     basis.bitangent.z * bitangentOffset) * inverseLength};
            };
        FVec3 meanDirection{};
        f32 meanTangentMoment = 0.0f;
        f32 meanBitangentMoment = 0.0f;
        constexpr u32 kSunDiskDirectionCount = 4u;
        for (u32 sampleIndex = 0u;
             sampleIndex < kSunDiskDirectionCount;
             ++sampleIndex) {
            const FVec3 actualDirection =
                sunDiskDirection(actual, sampleIndex);
            ExpectVec3Near(
                actualDirection,
                sunDiskDirection(expected, sampleIndex), 3.0e-6f);
            EXPECT_NEAR(dot(actualDirection, actualDirection), 1.0f, 3.0e-6f);
            meanDirection.x += actualDirection.x;
            meanDirection.y += actualDirection.y;
            meanDirection.z += actualDirection.z;
            const f32 tangentProjection =
                dot(actualDirection, actual.tangent);
            const f32 bitangentProjection =
                dot(actualDirection, actual.bitangent);
            meanTangentMoment += tangentProjection*tangentProjection;
            meanBitangentMoment += bitangentProjection*bitangentProjection;
        }
        meanTangentMoment /= static_cast<f32>(kSunDiskDirectionCount);
        meanBitangentMoment /= static_cast<f32>(kSunDiskDirectionCount);
        constexpr f32 kAxisOffset =
            kSkyPhysicalSunAngularRadiusRadians*0.5f;
        constexpr f32 kExpectedAxisMoment =
            kAxisOffset*kAxisOffset/(1.0f+2.0f*kAxisOffset*kAxisOffset);
        EXPECT_NEAR(
            meanTangentMoment, kExpectedAxisMoment, 1.0e-9f);
        EXPECT_NEAR(
            meanBitangentMoment, kExpectedAxisMoment, 1.0e-9f);
        EXPECT_NEAR(dot(meanDirection, actual.tangent), 0.0f, 3.0e-6f);
        EXPECT_NEAR(dot(meanDirection, actual.bitangent), 0.0f, 3.0e-6f);
    }
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const auto hostileBasis =
        ResolveVolumetricCloudLightBasis(FVec3{nan, 0.0f, 0.0f});
    ExpectVec3Near(
        hostileBasis.direction, FVec3{0.0f, 1.0f, 0.0f}, 0.0f);
    EXPECT_TRUE(std::isfinite(hostileBasis.tangent.x));
    EXPECT_TRUE(std::isfinite(hostileBasis.tangent.y));
    EXPECT_TRUE(std::isfinite(hostileBasis.tangent.z));
    EXPECT_TRUE(std::isfinite(hostileBasis.bitangent.x));
    EXPECT_TRUE(std::isfinite(hostileBasis.bitangent.y));
    EXPECT_TRUE(std::isfinite(hostileBasis.bitangent.z));

    // 不変量の移動後も、描画投入数とCPU側の標本上限を明示したままにする。
    EXPECT_TRUE(Contains(
        shader,
        "intMAX_STEPS=(int)cloudLightingAmbient.z;"
        "if(MAX_STEPS<CLOUD_MIN_VIEW_MARCH_SAMPLE_COUNT)MAX_STEPS=CLOUD_MIN_VIEW_MARCH_SAMPLE_COUNT;"));
    EXPECT_TRUE(Contains(
        shader,
        "staticconstintCLOUD_LIGHT_MARCH_SAMPLE_COUNT=16;"
        "staticconstintCLOUD_LIGHT_DETAIL_SAMPLE_COUNT=3;"));
    EXPECT_EQ(
        CountOccurrences(
            shader,
            "sampleIndex<CLOUD_LIGHT_MARCH_SAMPLE_COUNT;"),
        static_cast<std::size_t>(2));
    EXPECT_TRUE(Contains(
        shader,
        "if(!cloudAdaptiveLightSampleTerms("
        "intervals,sampleCounts,sampleIndex,"
        "rayDistance,sampleSpacing))continue;"));
    EXPECT_FALSE(Contains(shader, "lightHalfStep"));
    EXPECT_FALSE(Contains(shader, "lightStep*=1.8"));
    EXPECT_TRUE(Contains(
        compactSource,
        "cl.Dispatch((m_W+7u)/8u,(m_H+7u)/8u,1);"));
    EXPECT_TRUE(Contains(
        compactSource,
        "cl.Dispatch((m_FullW+7u)/8u,(m_FullH+7u)/8u,1);"));
    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 384u);
    EXPECT_EQ(kVolumetricCloudMaxLightMarchSamples, 16u);
    EXPECT_EQ(kVolumetricCloudUltraTraceDivisor, 4u);
}

ACS_TEST(VolumetricClouds,
         EvolutionPhasesMorphIndependentDomainsWithoutAdditionalTextureReads) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    const std::string compactSource = CompactShader(source);
    EXPECT_TRUE(!shader.empty());

    // 基本形状は一回の採取から帯域を選び、天候だけを広域4点で面積ろ過する。
    // 追加採取は担当面積が周波数を解像できない場合だけ使い、独立位相の責務は維持する。
    EXPECT_EQ(
        CountOccurrences(shader, "shapeNoise.SampleLevel("),
        static_cast<std::size_t>(1));
    EXPECT_EQ(
        CountOccurrences(shader, "weatherMap.SampleLevel("),
        static_cast<std::size_t>(2));
    EXPECT_EQ(
        CountOccurrences(shader, "curlNoise.SampleLevel("),
        static_cast<std::size_t>(2));
    // 実際の侵食2回に加え、二つの任意影パスが登録枠を保持する到達不能参照を持つ。
    EXPECT_EQ(CountOccurrences(shader, "detailNoise.SampleLevel("), static_cast<std::size_t>(4));
    EXPECT_FALSE(Contains(
        shader,
        "+float3(cloudEvolution.x,cloudEvolution.y,-cloudEvolution.x);"));
    EXPECT_FALSE(Contains(
        shader,
        "+float3(-cloudEvolution.y,cloudEvolution.x,cloudEvolution.y);"));
    EXPECT_TRUE(Contains(
        shader,
        "curlUv+=float4(0.0,0.0,cloudEvolution.z,cloudEvolution.w);"));
    // 主3D形状は最大値包絡が成立するアフィン写像に限定し、動きは天候、渦、
    // 詳細領域の独立した時間位相で作る。
    EXPECT_FALSE(Contains(shader, "convectionWarp"));
    EXPECT_FALSE(Contains(shader, "canonicalY+="));
    EXPECT_TRUE(Contains(
        shader,
        "floatcanonicalY=saturate(normalizedLayerHeight)"
        "*cloudShapeVerticalSpan(upperBand)+0.07;"));
    EXPECT_TRUE(Contains(
        shader,
        "detailDomainA+float3(0.19,0.67,0.41)"
        "+float3(cloudEvolution.z,cloudEvolution.w,-cloudEvolution.z)"));
    EXPECT_TRUE(Contains(
        shader,
        "detailDomainB+float3(0.73,0.23,0.59)"
        "+float3(-cloudEvolution.w,cloudEvolution.z,cloudEvolution.w)"));
    EXPECT_TRUE(Contains(
        compactSource,
        "ResolveVolumetricCloudEvolutionFrameTerms(safeTime,safeWind)"));
    EXPECT_EQ(
        CountOccurrences(
            compactSource,
            "ResolveVolumetricCloudAdvectionDistance("),
        static_cast<std::size_t>(3));
    EXPECT_FALSE(Contains(
        compactSource,
        "safeTime*safeWind*2.5f"));
    EXPECT_FALSE(Contains(
        compactSource,
        "safe_time*safe_wind*2.5f"));

    // CloudWind=1 は 10 秒で 120 ワールド単位を移動し、既定の雲高度でも
    // 数十秒待たずに画面上の移流を識別できる。負の値では同じ速さで逆向きへ流れる。
    EXPECT_NEAR(
        ResolveVolumetricCloudAdvectionDistance(1.0f, 1.0f),
        12.0f, 0.0f);
    EXPECT_NEAR(
        ResolveVolumetricCloudAdvectionDistance(10.0f, 1.0f),
        120.0f, 0.0f);
    EXPECT_NEAR(
        ResolveVolumetricCloudAdvectionDistance(10.0f, -1.0f),
        -120.0f, 0.0f);
    const FVec2 tenSecondWind = VolumetricCloudWindOffsetXZ(
        ResolveVolumetricCloudAdvectionDistance(10.0f, 1.0f));
    EXPECT_NEAR(
        std::sqrt(
            tenSecondWind.x * tenSecondWind.x +
            tenSecondWind.y * tenSecondWind.y),
        120.0f, 1.0e-3f);
    EXPECT_NEAR(
        ResolveVolumetricCloudAdvectionDistance(
            std::numeric_limits<f32>::quiet_NaN(), 1.0f),
        0.0f, 0.0f);
    EXPECT_NEAR(
        ResolveVolumetricCloudAdvectionDistance(
            1.0f, std::numeric_limits<f32>::infinity()),
        0.0f, 0.0f);
    EXPECT_NEAR(
        ResolveVolumetricCloudAdvectionDistance(20000000.0f, 100.0f),
        2400000000.0f, 0.0f);

    // 時刻 0 は従来の密度場と一致し、無風でも時間が進めば形状だけが変化する。
    const auto zero = ResolveVolumetricCloudEvolutionFrameTerms(0.0f, 20.0f);
    EXPECT_NEAR(zero.shape_phase.x, 0.0f, 0.0f);
    EXPECT_NEAR(zero.shape_phase.y, 0.0f, 0.0f);
    EXPECT_NEAR(zero.fine_phase.x, 0.0f, 0.0f);
    EXPECT_NEAR(zero.fine_phase.y, 0.0f, 0.0f);
    const auto still =
        ResolveVolumetricCloudEvolutionFrameTerms(60.0f, 0.0f);
    const f32 stillChange =
        std::fabs(still.shape_phase.x) +
        std::fabs(still.shape_phase.y) +
        std::fabs(still.fine_phase.x) +
        std::fabs(still.fine_phase.y);
    EXPECT_TRUE(stillChange > 0.05f);

    // 既定風速では10秒以内に独立領域の変化を判別できるが、画素単位のちらつきになる速度にはしない。
    const auto tenSeconds =
        ResolveVolumetricCloudEvolutionFrameTerms(10.0f, 1.0f);
    const f32 tenSecondChange =
        std::fabs(tenSeconds.shape_phase.x) +
        std::fabs(tenSeconds.shape_phase.y) +
        std::fabs(tenSeconds.fine_phase.x) +
        std::fabs(tenSeconds.fine_phase.y);
    EXPECT_TRUE(tenSecondChange > 0.16f);

    // 風向きの符号は移流だけへ反映し、変形速度は風速の大きさで決める。
    const auto forward =
        ResolveVolumetricCloudEvolutionFrameTerms(60.0f, 1.0f);
    const auto reverse =
        ResolveVolumetricCloudEvolutionFrameTerms(60.0f, -1.0f);
    EXPECT_NEAR(forward.shape_phase.x, reverse.shape_phase.x, 0.0f);
    EXPECT_NEAR(forward.shape_phase.y, reverse.shape_phase.y, 0.0f);
    EXPECT_NEAR(forward.fine_phase.x, reverse.fine_phase.x, 0.0f);
    EXPECT_NEAR(forward.fine_phase.y, reverse.fine_phase.y, 0.0f);

    // 60 Hz の隣接フレームでは位相が急変せず、時間再構成へ過大な差を渡さない。
    const auto nextFrame = ResolveVolumetricCloudEvolutionFrameTerms(
        60.0f + 1.0f / 60.0f, 1.0f);
    EXPECT_TRUE(std::fabs(nextFrame.shape_phase.x - forward.shape_phase.x) < 0.001f);
    EXPECT_TRUE(std::fabs(nextFrame.shape_phase.y - forward.shape_phase.y) < 0.001f);
    EXPECT_TRUE(std::fabs(nextFrame.fine_phase.x - forward.fine_phase.x) < 0.001f);
    EXPECT_TRUE(std::fabs(nextFrame.fine_phase.y - forward.fine_phase.y) < 0.001f);

    // 非有限値や極端な有限値でも GPU へ渡す範囲を超えない。
    const f32 hostileTimes[] = {
        std::numeric_limits<f32>::quiet_NaN(),
        -std::numeric_limits<f32>::infinity(),
        std::numeric_limits<f32>::infinity(),
        std::numeric_limits<f32>::lowest(),
        std::numeric_limits<f32>::max()};
    const f32 hostileWinds[] = {
        std::numeric_limits<f32>::quiet_NaN(),
        -std::numeric_limits<f32>::infinity(),
        std::numeric_limits<f32>::infinity(),
        std::numeric_limits<f32>::max()};
    for (const f32 hostileTime : hostileTimes) {
        for (const f32 hostileWind : hostileWinds) {
            const auto terms = ResolveVolumetricCloudEvolutionFrameTerms(
                hostileTime, hostileWind);
            EXPECT_TRUE(std::isfinite(terms.shape_phase.x));
            EXPECT_TRUE(std::isfinite(terms.shape_phase.y));
            EXPECT_TRUE(std::isfinite(terms.fine_phase.x));
            EXPECT_TRUE(std::isfinite(terms.fine_phase.y));
            EXPECT_TRUE(std::fabs(terms.shape_phase.x) <= 0.180001f);
            EXPECT_TRUE(std::fabs(terms.shape_phase.y) <= 0.160001f);
            EXPECT_TRUE(std::fabs(terms.fine_phase.x) <= 0.110001f);
            EXPECT_TRUE(std::fabs(terms.fine_phase.y) <= 0.090001f);
        }
    }
}

ACS_TEST(VolumetricClouds, TemporalSuperResolutionRejectsDisocclusionGhostsWithoutPeriodicSnaps) {
    const std::string source = ReadSkySource();
    const std::string resolveShader = CompactShader(
        ExtractRawShader(source, "const char* kCloudResolveCS"));
    EXPECT_TRUE(!resolveShader.empty());

    EXPECT_TRUE(Contains(
        resolveShader,
        "float2seedDepth=float2(curDepth,curA);"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "if(temporalSuperRes&&!scheduled&&worldOrigin.w>0.5)"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "float2sameScreenDepth=historyDepth.Load(int3(tid.xy,0));"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "boolsameScreenSeedValid=sameScreenDepth.x<=250000.0&&"
        "sameScreenDepth.y>0.001;"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "if(sameScreenSeedValid){seedDepth=sameScreenDepth;"
        "reprojectionDepth=sameScreenDepth.x;}"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "floatdepthTolerance=max(0.30,expectedDepth*0.01);"));
    EXPECT_TRUE(Contains(resolveShader, "booloccupancyMismatch=(!temporalSuperRes||scheduled)&&((curA<0.02&&hist.a>0.08)||(curA>0.08&&hist.a<0.02));"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "boolalphaOk=!occupancyMismatch&&"
        "abs(hist.a-seedDepth.y)<0.42;"));
    const std::size_t scheduledBegin = resolveShader.find("if(scheduled){");
    const std::size_t scheduledEnd = resolveShader.find("}else{", scheduledBegin);
    EXPECT_TRUE(scheduledBegin != std::string::npos);
    EXPECT_TRUE(scheduledEnd != std::string::npos);
    const std::string scheduledPath = scheduledBegin != std::string::npos &&
        scheduledEnd != std::string::npos
        ? resolveShader.substr(scheduledBegin, scheduledEnd - scheduledBegin)
        : std::string{};
    EXPECT_TRUE(Contains(
        scheduledPath,
        "resolved=lerp(histPacked,current,scheduledCurrentWeight);"
        "resolvedDepth=float2(curDepth,curA);"));
    EXPECT_TRUE(Contains(resolveShader, "if(!temporalSuperRes||scheduled){" "histPacked=CloudTemporalClipHistory("));
    EXPECT_TRUE(Contains(
        resolveShader,
        "resolved=lerp(histPacked,current,"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "resolvedDepth=float2(curDepth,curA);"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "resolved=lerp(histPacked,current,temporalCurrentWeight);"));
    EXPECT_TRUE(Contains(resolveShader, "floattemporalCurrentWeight=temporalMismatch;"));
    EXPECT_TRUE(Contains(resolveShader, "floatscheduledCurrentWeight=CloudTemporalScheduledCurrentWeight(temporalMismatch);"));
    EXPECT_TRUE(Contains(resolveShader, "floatscaledCurrentWeight=CloudTemporalScaledCurrentWeight(temporalMismatch);"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "resolved=lerp(histPacked,current,temporalCurrentWeight);"));
    EXPECT_TRUE(Contains(resolveShader, "float4spatialCurrent=float4(gatheredPremul,gatheredA);float2spatialDepth=float2(gatheredDepth,gatheredA);"));
    EXPECT_TRUE(Contains(resolveShader, "if(temporalSuperRes&&scheduled&&!historyAccepted){resolved=lerp(spatialCurrent,current,scheduledCurrentWeight);"));
    EXPECT_TRUE(Contains(resolveShader, "floatfallbackDepth=curDepth<=250000.0?curDepth:spatialDepth.x;resolvedDepth=float2(fallbackDepth,resolved.a);"));
    EXPECT_TRUE(CloudTemporalUsesInvalidHistoryBlendForTest(true, true, false));
    EXPECT_FALSE(CloudTemporalUsesInvalidHistoryBlendForTest(true, true, true));
    EXPECT_FALSE(CloudTemporalUsesInvalidHistoryBlendForTest(true, false, false));
    EXPECT_FALSE(CloudTemporalUsesInvalidHistoryBlendForTest(false, true, false));
    EXPECT_TRUE(Contains(
        resolveShader,
        "float4hist=historyColor.Load(int3(historyPixel,0));"
        "float2histD=historyDepth.Load(int3(historyPixel,0));"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "if(refEmpty!=tapEmpty){"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "bilateral=exp(-max(refC.a,c.a)*5.0);"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "boolstableUnscheduled=temporal.x>0.5&&temporalSuperRes&&"
        "!scheduled&&worldOrigin.w>0.5&&temporalMismatch<0.08;"));
    EXPECT_FALSE(Contains(resolveShader, "currentTracePixel"));
    EXPECT_FALSE(Contains(resolveShader, "currentTraceHistory"));
    EXPECT_FALSE(Contains(resolveShader, "blockResponse"));
    EXPECT_FALSE(Contains(resolveShader, "CloudTemporalUnscheduledWeight"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "if(currentDefinitelyEmpty&&sameScreenColor.a<=0.003)"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "historyColor.Load(int3(emptyPixel,0)).a"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "if(maximumHistoryAlpha<=0.003){"
        "historyColorOut[tid.xy]=float4(0,0,0,0);"));
    const std::size_t stableHistoryFirst = resolveShader.find(
        "boolstableUnscheduled=temporal.x>0.5&&temporalSuperRes&&");
    const std::size_t bilateralFallback = resolveShader.find(
        "float3premulSum=0.0;");
    EXPECT_TRUE(stableHistoryFirst != std::string::npos);
    EXPECT_TRUE(bilateralFallback != std::string::npos);
    EXPECT_TRUE(stableHistoryFirst < bilateralFallback);
    EXPECT_TRUE(std::exp(-0.05f * 5.0f) > 0.75f);
    EXPECT_TRUE(std::exp(-0.95f * 5.0f) < 0.01f);

    // The combined compute pass consumes the same previous full-resolution depth
    // history as color, so a rescued edge remains valid in the later
    // depth-aware composite.
    EXPECT_TRUE(Contains(
        source, "pd.srv_names[3] = \"historyDepth\";"));
    EXPECT_TRUE(Contains(
        source, "cl.SetTexture(3, *m_HistoryDepth[prev]);"));
}

ACS_TEST(VolumetricClouds, TemporalSuperResolutionBlendsScheduledExactSampleWithoutPeriodicSnap) {
    // 採取画素の同一雲体履歴は段階的に更新し、未採取15画素は各自の履歴を保つ。
    const f32 currentAlpha = 0.80f;
    const f32 historyAlpha = 0.20f;
    const f32 scheduledWeight = CloudTemporalScheduledCurrentWeightForTest(0.0f);
    const f32 scheduledResult = historyAlpha +
        (currentAlpha - historyAlpha) * scheduledWeight;
    const f32 unscheduledResult = historyAlpha;
    EXPECT_NEAR(scheduledWeight, 0.28f, 0.0f);
    EXPECT_NEAR(scheduledResult, 0.368f, 1e-6f);
    EXPECT_NEAR(unscheduledResult, 0.20f, 0.0f);
    EXPECT_NEAR(scheduledResult - unscheduledResult, 0.168f, 1e-6f);
    EXPECT_TRUE(scheduledResult - unscheduledResult < (currentAlpha - historyAlpha) * 0.30f);

    // 静止形状でも10周期後の残差を4%未満にし、旧20%の長い尾を残さない。
    f32 retainedError = 1.0f;
    for (u32 update = 0u; update < 10u; ++update) {
        retainedError *= 1.0f - CloudTemporalScheduledCurrentWeightForTest(0.0f);
    }
    EXPECT_TRUE(retainedError < 0.04f);
    EXPECT_NEAR(CloudTemporalScheduledCurrentWeightForTest(0.022f), 0.324f, 1e-6f);
    EXPECT_NEAR(CloudTemporalScheduledCurrentWeightForTest(0.36f), 1.0f, 0.0f);
    EXPECT_NEAR(CloudTemporalScaledCurrentWeightForTest(0.0f), 0.18f, 0.0f);
    EXPECT_NEAR(CloudTemporalScaledCurrentWeightForTest(0.35f), 0.35f, 0.0f);

    // 未採取画素の空間再構成は別レイなので、正確な画素別履歴を棄却する根拠にしない。
    EXPECT_FALSE(CloudTemporalOccupancyMismatchForTest(0.0f, 0.8f, true, false));
    EXPECT_FALSE(CloudTemporalOccupancyMismatchForTest(0.8f, 0.0f, true, false));
    EXPECT_TRUE(CloudTemporalOccupancyMismatchForTest(0.0f, 0.8f, true, true));
    EXPECT_TRUE(CloudTemporalOccupancyMismatchForTest(0.8f, 0.0f, false, false));

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudResolveCS"));
    EXPECT_FALSE(Contains(shader, "CloudTemporalSampleResponse"));
    EXPECT_TRUE(Contains(shader, "floattemporalCurrentWeight=temporalMismatch;"));
    EXPECT_EQ(CountOccurrences(shader, "resolved=current;"), 2u);
    EXPECT_TRUE(Contains(shader, "resolved=lerp(histPacked,current,"));
    EXPECT_TRUE(Contains(shader, "if(!temporalSuperRes||scheduled){histPacked=CloudTemporalClipHistory(histPacked,neighborhoodMin,neighborhoodMax);}"));
    EXPECT_TRUE(Contains(shader, "booloccupancyMismatch=(!temporalSuperRes||scheduled)&&((curA<0.02&&hist.a>0.08)||(curA>0.08&&hist.a<0.02));"));
    EXPECT_FALSE(Contains(shader, "CloudTemporalBlockResponse"));
    EXPECT_FALSE(Contains(shader, "CloudTemporalUnscheduledWeight"));
    EXPECT_TRUE(Contains(shader, "resolved=current;resolvedDepth=nativeDepth;"));
    EXPECT_TRUE(Contains(shader, "}elseif(nativeMarch){"));
    EXPECT_TRUE(Contains(shader, "resolved=lerp(histPacked,current,scaledCurrentWeight);resolvedDepth=float2(curDepth,resolved.a);"));
    EXPECT_FALSE(Contains(shader, "floatfeedback=max(lerp(0.42,0.62,edgeConfidence),evolutionMismatch);"));
    EXPECT_TRUE(Contains(shader, "floatCloudTemporalEvolutionMismatch(){"));
    EXPECT_TRUE(Contains(shader, "floatCloudTemporalScheduledCurrentWeight(floatevolutionMismatch){returnsaturate(0.28+evolutionMismatch*2.0);}"));
    EXPECT_TRUE(Contains(shader, "floatCloudTemporalScaledCurrentWeight(floatevolutionMismatch){returnmax(0.18,saturate(evolutionMismatch));}"));
    EXPECT_TRUE(Contains(shader, "float2slowDelta=abs(cloudEvolution.xy-cloudPreviousEvolution.xy);"));
    EXPECT_TRUE(Contains(shader, "floatevolutionMismatch=CloudTemporalEvolutionMismatch();"));
    EXPECT_TRUE(Contains(shader, "!scheduled&&worldOrigin.w>0.5&&temporalMismatch<0.08"));
    EXPECT_TRUE(Contains(shader, "resolved=lerp(histPacked,current,temporalCurrentWeight);"));
    EXPECT_NEAR(
        CloudTemporalEvolutionMismatchForTest(
            FVec4{0.010f, 0.0f, 0.0f, 0.0f},
            FVec4{0.0f, 0.0f, 0.0f, 0.0f}),
        1.0f,
        1e-6f);
    EXPECT_NEAR(
        CloudTemporalEvolutionMismatchForTest(
            FVec4{0.0001f, -0.0001f, 0.0001f, -0.0001f},
            FVec4{0.0f, 0.0f, 0.0f, 0.0f}),
        0.022f,
        1e-6f);
    EXPECT_NEAR(CloudTemporalCurrentWeightForTest(0.0f), 0.0f, 0.0f);
    EXPECT_NEAR(CloudTemporalCurrentWeightForTest(0.022f), 0.022f, 1e-6f);
    EXPECT_NEAR(CloudTemporalCurrentWeightForTest(0.35f), 0.35f, 1e-6f);
    // 固定8%では、静止した正確な履歴の約71%が次の等倍採取までに別レイへ置き換わる。
    f32 fixedFloorHistory = 1.0f;
    for (u32 unscheduledFrame = 0u; unscheduledFrame < 15u; ++unscheduledFrame) {
        fixedFloorHistory *= 0.92f;
    }
    EXPECT_TRUE(fixedFloorHistory < 0.30f);
    f32 correctedHistory = 1.0f;
    for (u32 unscheduledFrame = 0u; unscheduledFrame < 15u; ++unscheduledFrame) {
        correctedHistory *= 1.0f - CloudTemporalCurrentWeightForTest(0.0f);
    }
    EXPECT_NEAR(correctedHistory, 1.0f, 0.0f);
}

ACS_TEST(VolumetricClouds, ContinuousCloudTimeUsesReprojectionInsteadOfWholeFrameInvalidation) {
    constexpr f32 evolutionFullResponseDelta = render_internal::kCloudEvolutionFullResponseDelta;
    EXPECT_NEAR(render_internal::kCloudEvolutionResponseScale, 220.0f, 0.0f);
    EXPECT_NEAR(evolutionFullResponseDelta * render_internal::kCloudEvolutionResponseScale, 1.0f, 1e-6f);
    EXPECT_EQ(render_internal::kCloudShadowTemporalPhaseCount, 4u);
    const u32 expectedPhases[] = {0u, 1u, 2u, 3u, 0u, 1u, 2u, 3u};
    const u32 expectedOffsetsX[] = {0u, 1u, 0u, 1u, 0u, 1u, 0u, 1u};
    const u32 expectedOffsetsY[] = {0u, 0u, 1u, 1u, 0u, 0u, 1u, 1u};
    const FVolumetricCloudEvolutionFrameTerms zeroEvolution{};
    for (u32 frameIndex = 0u; frameIndex < 8u; ++frameIndex) {
        const render_internal::FVolumetricCloudShadowTemporalDecision phaseDecision = render_internal::ResolveVolumetricCloudShadowTemporalDecision(frameIndex, zeroEvolution, zeroEvolution, 0.0f, 0.0f);
        EXPECT_EQ(phaseDecision.phase, expectedPhases[frameIndex]);
        EXPECT_EQ(phaseDecision.partial_update_offset_x, expectedOffsetsX[frameIndex]);
        EXPECT_EQ(phaseDecision.partial_update_offset_y, expectedOffsetsY[frameIndex]);
        EXPECT_FALSE(phaseDecision.self_shadow_requires_full_refresh);
        EXPECT_FALSE(phaseDecision.world_shadow_requires_full_refresh);
    }
    EXPECT_EQ(
        render_internal::kCloudShadowTemporalCompleteMask,
        static_cast<u8>(0x0fu));
    for (u32 firstPhase = 0u;
         firstPhase < render_internal::kCloudShadowTemporalPhaseCount;
         ++firstPhase) {
        u8 warmupMask = 0u;
        for (u32 step = 0u;
             step < render_internal::kCloudShadowTemporalPhaseCount;
             ++step) {
            warmupMask =
                render_internal::ResolveVolumetricCloudShadowWarmupMask_Internal(
                    warmupMask, firstPhase + step, false);
            if (step + 1u <
                render_internal::kCloudShadowTemporalPhaseCount) {
                EXPECT_TRUE(
                    warmupMask !=
                    render_internal::kCloudShadowTemporalCompleteMask);
            }
        }
        EXPECT_EQ(
            warmupMask,
            render_internal::kCloudShadowTemporalCompleteMask);
    }
    u8 repeatedPhaseMask = 0u;
    for (u32 step = 0u;
         step < render_internal::kCloudShadowTemporalPhaseCount;
         ++step) {
        repeatedPhaseMask =
            render_internal::ResolveVolumetricCloudShadowWarmupMask_Internal(
                repeatedPhaseMask, 2u, false);
    }
    EXPECT_TRUE(
        repeatedPhaseMask !=
        render_internal::kCloudShadowTemporalCompleteMask);
    EXPECT_EQ(
        render_internal::ResolveVolumetricCloudShadowWarmupMask_Internal(
            0u, 2u, true),
        render_internal::kCloudShadowTemporalCompleteMask);

    FVolumetricCloudEvolutionFrameTerms reverseEvolution{};
    reverseEvolution.shape_phase.x = -0.001f;
    const render_internal::FVolumetricCloudShadowTemporalDecision reverseDecision = render_internal::ResolveVolumetricCloudShadowTemporalDecision(7u, reverseEvolution, zeroEvolution, -48.0f, 0.0f);
    EXPECT_EQ(reverseDecision.phase, 3u);
    EXPECT_FALSE(reverseDecision.self_shadow_requires_full_refresh);
    EXPECT_TRUE(reverseDecision.world_shadow_requires_full_refresh);

    FVolumetricCloudEvolutionFrameTerms thresholdEvolution{};
    thresholdEvolution.shape_phase.x = evolutionFullResponseDelta / 3.0f;
    const render_internal::FVolumetricCloudShadowTemporalDecision thresholdDecision = render_internal::ResolveVolumetricCloudShadowTemporalDecision(0u, thresholdEvolution, zeroEvolution, kVolumetricCloudWorldShadowMapTexelSize / 3.0f, 0.0f);
    EXPECT_TRUE(thresholdDecision.self_shadow_requires_full_refresh);
    EXPECT_TRUE(thresholdDecision.world_shadow_requires_full_refresh);
    thresholdEvolution.shape_phase.x *= 0.999f;
    const render_internal::FVolumetricCloudShadowTemporalDecision belowThresholdDecision = render_internal::ResolveVolumetricCloudShadowTemporalDecision(0u, thresholdEvolution, zeroEvolution, (kVolumetricCloudWorldShadowMapTexelSize / 3.0f) * 0.999f, 0.0f);
    EXPECT_FALSE(belowThresholdDecision.self_shadow_requires_full_refresh);
    EXPECT_FALSE(belowThresholdDecision.world_shadow_requires_full_refresh);

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();
    FVolumetricCloudEvolutionFrameTerms invalidEvolution{};
    invalidEvolution.fine_phase.y = nan;
    const render_internal::FVolumetricCloudShadowTemporalDecision nanDecision = render_internal::ResolveVolumetricCloudShadowTemporalDecision(0u, invalidEvolution, zeroEvolution, nan, 0.0f);
    EXPECT_TRUE(nanDecision.self_shadow_requires_full_refresh);
    EXPECT_TRUE(nanDecision.world_shadow_requires_full_refresh);
    invalidEvolution.fine_phase.y = infinity;
    const render_internal::FVolumetricCloudShadowTemporalDecision positiveInfinityDecision = render_internal::ResolveVolumetricCloudShadowTemporalDecision(0u, invalidEvolution, zeroEvolution, infinity, 0.0f);
    EXPECT_TRUE(positiveInfinityDecision.self_shadow_requires_full_refresh);
    EXPECT_TRUE(positiveInfinityDecision.world_shadow_requires_full_refresh);
    invalidEvolution.fine_phase.y = -infinity;
    const render_internal::FVolumetricCloudShadowTemporalDecision negativeInfinityDecision = render_internal::ResolveVolumetricCloudShadowTemporalDecision(0u, invalidEvolution, zeroEvolution, -infinity, 0.0f);
    EXPECT_TRUE(negativeInfinityDecision.self_shadow_requires_full_refresh);
    EXPECT_TRUE(negativeInfinityDecision.world_shadow_requires_full_refresh);

    // 可変刻みでも、部分更新を許した各一段差の和は最古位相までの許容差未満になる。
    const f32 variableEvolutionSteps[] = {evolutionFullResponseDelta * 0.31f, evolutionFullResponseDelta * 0.08f, evolutionFullResponseDelta * 0.32f, evolutionFullResponseDelta * 0.29f, evolutionFullResponseDelta * 0.30f, evolutionFullResponseDelta * 0.10f, evolutionFullResponseDelta * 0.32f, evolutionFullResponseDelta * 0.31f};
    f32 phaseEvolutionPositions[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    f32 previousEvolutionPosition = 0.0f;
    for (u32 stepIndex = 0u; stepIndex < 8u; ++stepIndex) {
        const f32 currentEvolutionPosition = previousEvolutionPosition + variableEvolutionSteps[stepIndex];
        FVolumetricCloudEvolutionFrameTerms previousVariableEvolution{};
        FVolumetricCloudEvolutionFrameTerms currentVariableEvolution{};
        previousVariableEvolution.shape_phase.x = previousEvolutionPosition;
        currentVariableEvolution.shape_phase.x = currentEvolutionPosition;
        const render_internal::FVolumetricCloudShadowTemporalDecision variableDecision = render_internal::ResolveVolumetricCloudShadowTemporalDecision(stepIndex + 1u, currentVariableEvolution, previousVariableEvolution, 0.0f, 0.0f);
        EXPECT_FALSE(variableDecision.self_shadow_requires_full_refresh);
        phaseEvolutionPositions[variableDecision.phase] = currentEvolutionPosition;
        for (const f32 phaseEvolutionPosition : phaseEvolutionPositions) {
            f32 actualAgeDelta = currentEvolutionPosition - phaseEvolutionPosition;
            if (actualAgeDelta < 0.0f) actualAgeDelta = -actualAgeDelta;
            EXPECT_TRUE(actualAgeDelta < evolutionFullResponseDelta);
        }
        previousEvolutionPosition = currentEvolutionPosition;
    }

    constexpr f32 previousTime = 10.0f;
    constexpr f32 currentTime = 10.30f;
    constexpr f32 windSpeed = 1.0f;
    const f32 previousWind = ResolveVolumetricCloudAdvectionDistance(previousTime, windSpeed);
    const f32 currentWind = ResolveVolumetricCloudAdvectionDistance(currentTime, windSpeed);
    const FVec2 previousOffset = VolumetricCloudWindOffsetXZ(previousWind);
    const FVec2 currentOffset = VolumetricCloudWindOffsetXZ(currentWind);
    const FVec3 previousPoint{4100.0f, 3200.0f, 9800.0f};
    const FVec3 advectedPoint{previousPoint.x + currentOffset.x - previousOffset.x, previousPoint.y, previousPoint.z + currentOffset.y - previousOffset.y};
    const FVec2 previousMaterial = VolumetricCloudMaterialXZ(previousPoint, previousWind);
    const FVec2 currentMaterial = VolumetricCloudMaterialXZ(advectedPoint, currentWind);

    // 遅いフレームでも風移流は前フレームの同じ物質位置へ厳密に戻せる。
    EXPECT_NEAR(previousMaterial.x, currentMaterial.x, 1e-3f);
    EXPECT_NEAR(previousMaterial.y, currentMaterial.y, 1e-3f);

    const FVolumetricCloudEvolutionFrameTerms previousEvolution = ResolveVolumetricCloudEvolutionFrameTerms(previousTime, windSpeed);
    const FVolumetricCloudEvolutionFrameTerms currentEvolution = ResolveVolumetricCloudEvolutionFrameTerms(currentTime, windSpeed);
    const f32 evolutionMismatch = CloudTemporalEvolutionMismatchForTest(FVec4{currentEvolution.shape_phase.x, currentEvolution.shape_phase.y, currentEvolution.fine_phase.x, currentEvolution.fine_phase.y}, FVec4{previousEvolution.shape_phase.x, previousEvolution.shape_phase.y, previousEvolution.fine_phase.x, previousEvolution.fine_phase.y});
    EXPECT_TRUE(evolutionMismatch > 0.0f);
    EXPECT_TRUE(evolutionMismatch < 1.0f);
    const f32 shortAdvectionDistance = currentWind - previousWind;
    EXPECT_TRUE(shortAdvectionDistance < kVolumetricCloudWorldShadowMapTexelSize);

    // 直前フレームとの差は小さくても、4位相の最古状態との差は完全追従境界を越える。
    constexpr f32 oldestPhaseTime = 0.0f;
    constexpr f32 previousPhaseTime = 0.60f;
    constexpr f32 currentPhaseTime = 0.90f;
    const FVolumetricCloudEvolutionFrameTerms oldestPhaseEvolution = ResolveVolumetricCloudEvolutionFrameTerms(oldestPhaseTime, windSpeed);
    const FVolumetricCloudEvolutionFrameTerms previousPhaseEvolution = ResolveVolumetricCloudEvolutionFrameTerms(previousPhaseTime, windSpeed);
    const FVolumetricCloudEvolutionFrameTerms currentPhaseEvolution = ResolveVolumetricCloudEvolutionFrameTerms(currentPhaseTime, windSpeed);
    const f32 previousPhaseMismatch = CloudTemporalEvolutionMismatchForTest(FVec4{currentPhaseEvolution.shape_phase.x, currentPhaseEvolution.shape_phase.y, currentPhaseEvolution.fine_phase.x, currentPhaseEvolution.fine_phase.y}, FVec4{previousPhaseEvolution.shape_phase.x, previousPhaseEvolution.shape_phase.y, previousPhaseEvolution.fine_phase.x, previousPhaseEvolution.fine_phase.y});
    const f32 oldestPhaseMismatch = CloudTemporalEvolutionMismatchForTest(FVec4{currentPhaseEvolution.shape_phase.x, currentPhaseEvolution.shape_phase.y, currentPhaseEvolution.fine_phase.x, currentPhaseEvolution.fine_phase.y}, FVec4{oldestPhaseEvolution.shape_phase.x, oldestPhaseEvolution.shape_phase.y, oldestPhaseEvolution.fine_phase.x, oldestPhaseEvolution.fine_phase.y});
    EXPECT_TRUE(previousPhaseMismatch < 1.0f);
    EXPECT_TRUE(previousPhaseMismatch * static_cast<f32>(render_internal::kCloudShadowTemporalPhaseCount - 1u) >= 1.0f);
    EXPECT_NEAR(oldestPhaseMismatch, 1.0f, 0.0f);
    const render_internal::FVolumetricCloudShadowTemporalDecision evolutionJumpDecision = render_internal::ResolveVolumetricCloudShadowTemporalDecision(3u, currentPhaseEvolution, previousPhaseEvolution, 0.0f, 0.0f);
    EXPECT_TRUE(evolutionJumpDecision.self_shadow_requires_full_refresh);

    // 固定ワールド影では、各フレームが1画素未満でも最古位相との差は累積する。
    constexpr f32 fastWindSpeed = 20.0f;
    constexpr f32 fastFrameSeconds = 0.20f;
    const f32 fastFrameAdvection = ResolveVolumetricCloudAdvectionDistance(fastFrameSeconds, fastWindSpeed);
    const f32 oldestWorldShadowAdvection = ResolveVolumetricCloudAdvectionDistance(fastFrameSeconds * 3.0f, fastWindSpeed);
    EXPECT_TRUE(fastFrameAdvection < kVolumetricCloudWorldShadowMapTexelSize);
    EXPECT_TRUE(fastFrameAdvection * static_cast<f32>(render_internal::kCloudShadowTemporalPhaseCount - 1u) >= kVolumetricCloudWorldShadowMapTexelSize);
    EXPECT_NEAR(oldestWorldShadowAdvection, fastFrameAdvection * 3.0f, 1e-5f);
    const render_internal::FVolumetricCloudShadowTemporalDecision fastWindDecision = render_internal::ResolveVolumetricCloudShadowTemporalDecision(1u, zeroEvolution, zeroEvolution, fastFrameAdvection, 0.0f);
    EXPECT_FALSE(fastWindDecision.self_shadow_requires_full_refresh);
    EXPECT_TRUE(fastWindDecision.world_shadow_requires_full_refresh);
    const f32 longRunningPreviousWind = ResolveVolumetricCloudAdvectionDistance(10000.0f, 1.0f);
    const f32 longRunningEditedWind = ResolveVolumetricCloudAdvectionDistance(10000.0f, 1.0005f);
    const render_internal::FVolumetricCloudShadowTemporalDecision speedEditDecision = render_internal::ResolveVolumetricCloudShadowTemporalDecision(2u, zeroEvolution, zeroEvolution, longRunningEditedWind, longRunningPreviousWind);
    EXPECT_TRUE(speedEditDecision.world_shadow_requires_full_refresh);

    // 地平線付近では方向ベクトル差が小さくても xz/y の投影差が急増する。
    // 自己影は雲層の厚さ、地表影は参照面から雲頂までを使い、同じ太陽変化でも
    // 四位相の間に地表影だけが一画素を超える幾何条件を固定する。
    const FVec3 previousLowSun = NormalizeForTest(
        FVec3{1.0f, 0.1f, 0.0f});
    const FVec3 currentLowSun = NormalizeForTest(
        FVec3{1.0f, 0.1001f, 0.0f});
    constexpr f32 selfShadowVerticalSpan = 2900.0f;
    constexpr f32 worldShadowVerticalSpan = 4600.0f;
    f32 selfLowSunProjectionDelta = 0.0f;
    f32 worldLowSunProjectionDelta = 0.0f;
    EXPECT_TRUE(
        render_internal::ResolveVolumetricCloudSunProjectionDelta_Internal(
            currentLowSun, previousLowSun, selfShadowVerticalSpan,
            selfLowSunProjectionDelta));
    EXPECT_TRUE(
        render_internal::ResolveVolumetricCloudSunProjectionDelta_Internal(
            currentLowSun, previousLowSun, worldShadowVerticalSpan,
            worldLowSunProjectionDelta));
    constexpr f32 maximumPartialShadowAge = static_cast<f32>(
        render_internal::kCloudShadowTemporalPhaseCount - 1u);
    EXPECT_TRUE(
        selfLowSunProjectionDelta * maximumPartialShadowAge <
        kVolumetricCloudWorldShadowMapTexelSize);
    EXPECT_TRUE(
        worldLowSunProjectionDelta * maximumPartialShadowAge >=
        kVolumetricCloudWorldShadowMapTexelSize);
    const f32 projectionRatioDelta = Abs(
        currentLowSun.x / currentLowSun.y -
        previousLowSun.x / previousLowSun.y);
    EXPECT_NEAR(
        selfLowSunProjectionDelta,
        selfShadowVerticalSpan * projectionRatioDelta, 1e-3f);
    EXPECT_NEAR(
        worldLowSunProjectionDelta,
        worldShadowVerticalSpan * projectionRatioDelta, 1e-3f);
    f32 rejectedProjectionDelta = 1.0f;
    EXPECT_FALSE(
        render_internal::ResolveVolumetricCloudSunProjectionDelta_Internal(
            FVec3{1.0f, kVolumetricCloudWorldShadowMinimumSunY, 0.0f},
            previousLowSun, 3000.0f, rejectedProjectionDelta));
    EXPECT_NEAR(rejectedProjectionDelta, 0.0f, 0.0f);

    const std::string source = CompactShader(ReadSkySource());
    EXPECT_TRUE(!source.empty());
    EXPECT_FALSE(Contains(source, "timeDelta>0.25f"));
    EXPECT_FALSE(Contains(source, "windDelta>2.0f"));
    EXPECT_TRUE(Contains(source, "if(coverageDelta>0.001f||densityDelta>0.001f||" "windSpeedDelta>0.001f)historyValid=false;"));
    EXPECT_TRUE(Contains(source, "constrender_internal::FVolumetricCloudShadowTemporalDecision" "shadowTemporalDecision=render_internal::ResolveVolumetricCloudShadowTemporalDecision(" "recorded.frame_index,evolutionFrameTerms,previousEvolutionFrameTerms," "windOffset,m_PrevWindOffset);"));
    EXPECT_TRUE(Contains(source, "constf32selfShadowVerticalSpan=" "highestCloudAltitude-m_Layer.base_height;"));
    EXPECT_TRUE(Contains(source, "constf32worldShadowVerticalSpan=" "highestCloudAltitude>recorded.world_shadow_reference_height" "?highestCloudAltitude-recorded.world_shadow_reference_height" ":0.0f;"));
    EXPECT_TRUE(Contains(source, "ResolveVolumetricCloudSunProjectionDelta_Internal(" "safeSun,m_PrevSunDir,selfShadowVerticalSpan," "selfSunDirectionStepDistance);"));
    EXPECT_TRUE(Contains(source, "ResolveVolumetricCloudSunProjectionDelta_Internal(" "safeSun,m_PrevSunDir,worldShadowVerticalSpan," "worldSunDirectionStepDistance);"));
    EXPECT_TRUE(Contains(source, "constboolselfShadowTemporalDiscontinuity=" "rebuildShadowCacheThisFrame&&" "(shadowTemporalDecision.self_shadow_requires_full_refresh||" "cloudMediumChanged||selfSunDirectionDiscontinuity);"));
    EXPECT_TRUE(Contains(source, "constboolworldShadowTemporalDiscontinuity=" "rebuildWorldShadowThisFrame&&" "(shadowTemporalDecision.world_shadow_requires_full_refresh||" "cloudMediumChanged||worldSunDirectionDiscontinuity||" "worldShadowMappingChanged);"));
    EXPECT_TRUE(Contains(source, "constu32shadowUpdateOffsetX=shadowUpdateDivisor==1u" "?0u:shadowTemporalDecision.partial_update_offset_x;"));
    EXPECT_TRUE(Contains(source, "constu32shadowUpdateOffsetY=shadowUpdateDivisor==1u" "?0u:shadowTemporalDecision.partial_update_offset_y;"));
    EXPECT_TRUE(Contains(
        source,
        "constboolrefreshAllSelfShadows="
        "m_ReferenceMode||selfShadowTemporalDiscontinuity;"));
    EXPECT_TRUE(Contains(
        source,
        "constboolrefreshAllWorldShadows="
        "m_ReferenceMode||worldShadowTemporalDiscontinuity;"));
    EXPECT_FALSE(Contains(
        source,
        "constboolrefreshAllShadows=m_ReferenceMode||!historyValid"));
    EXPECT_TRUE(Contains(
        source,
        "cb.shadowState=FVec4{"
        "shadowCacheReadyAfterUpdate?1.0f:0.0f,"));
    const std::string resolveShader = CompactShader(ExtractRawShader(ReadSkySource(), "const char* kCloudResolveCS"));
    EXPECT_TRUE(Contains(resolveShader, "returnsaturate(delta*220.0);"));
    EXPECT_TRUE(Contains(resolveShader, "floatCloudTemporalLightingMismatch(){"));
    EXPECT_TRUE(Contains(resolveShader, "floatlightingMismatch=CloudTemporalLightingMismatch();"));
    EXPECT_TRUE(Contains(resolveShader, "floattemporalMismatch=max(evolutionMismatch,lightingMismatch);"));
}

ACS_TEST(VolumetricClouds, StableUnscheduledHistoryClipsOnlyCurrentNeighborhoodOutliers) {
    EXPECT_NEAR(CloudTemporalClipChannelForTest(0.70f, 0.50f, 0.50f, 0.025f), 0.50875f, 1e-6f);
    EXPECT_NEAR(CloudTemporalClipChannelForTest(0.30f, 0.50f, 0.50f, 0.025f), 0.49125f, 1e-6f);
    EXPECT_NEAR(CloudTemporalClipChannelForTest(0.505f, 0.50f, 0.50f, 0.025f), 0.505f, 0.0f);
    EXPECT_NEAR(CloudTemporalClipChannelForTest(0.10f, 0.20f, 0.80f, 0.025f), 0.10f, 0.0f);
    EXPECT_FALSE(CloudTemporalNeedsNeighborhoodClipForTest(0.505f, 0.50f, 0.025f));
    EXPECT_FALSE(CloudTemporalNeedsNeighborhoodClipForTest(0.520f, 0.50f, 0.025f));
    EXPECT_TRUE(CloudTemporalNeedsNeighborhoodClipForTest(0.530f, 0.50f, 0.025f));
    EXPECT_TRUE(CloudTemporalNeedsNeighborhoodClipForTest(0.520f, 0.50f, 0.015f));

    constexpr u32 pixelCoordinates[] = {0u, 1u, 2u, 37u, 91u, 4095u};
    for (const u32 pixelX : pixelCoordinates) {
        for (const u32 pixelY : pixelCoordinates) {
            u32 scheduledCount = 0u;
            u32 maximumGap = 0u;
            u32 firstPhase = 0u;
            u32 previousPhase = 0u;
            bool foundPrevious = false;
            for (u32 phase = 0u; phase < 16u; ++phase) {
                if (!CloudTemporalNeighborhoodClipScheduledForTest(pixelX, pixelY, phase)) continue;
                if (!foundPrevious) firstPhase = phase;
                if (foundPrevious) {
                    const u32 gap = phase - previousPhase;
                    if (gap > maximumGap) maximumGap = gap;
                }
                previousPhase = phase;
                foundPrevious = true;
                ++scheduledCount;
            }
            const u32 wrappedGap = firstPhase + 16u - previousPhase;
            if (wrappedGap > maximumGap) maximumGap = wrappedGap;
            EXPECT_EQ(scheduledCount, 2u);
            EXPECT_TRUE(maximumGap <= 8u);
        }
    }

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudResolveCS"));
    EXPECT_TRUE(Contains(shader, "float4CloudTemporalPackedCurrent(int2q){"));
    EXPECT_TRUE(Contains(shader, "float4CloudTemporalClipHistory(float4historyPacked,float4currentMin,float4currentMax){"));
    EXPECT_TRUE(Contains(shader, "boolCloudTemporalNeedsNeighborhoodClip(float4historyPacked,float4currentPacked){"));
    EXPECT_TRUE(Contains(shader, "boolCloudTemporalNeighborhoodClipScheduled(uint2pixel,uintphaseIndex){"));
    EXPECT_TRUE(Contains(shader, "returnalphaDifference>CLOUD_TEMPORAL_MIN_RANGE.a||luminanceDifference>CLOUD_TEMPORAL_MIN_RANGE.r;"));
    EXPECT_TRUE(Contains(shader, "return(CloudTemporalBlockPhase4(pixel,phaseIndex)&7u)==0u;"));
    EXPECT_TRUE(Contains(shader, "constint2stableOffsets[4]={int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)};"));
    EXPECT_TRUE(Contains(shader, "if(CloudTemporalNeighborhoodClipScheduled(tid.xy,phaseIndex)&&CloudTemporalNeedsNeighborhoodClip(stableHistPacked,stableReferencePacked)){"));
    EXPECT_TRUE(Contains(shader, "stableHistPacked=CloudTemporalClipHistory(stableHistPacked,stableCurrentMin,stableCurrentMax);}" "resolved=lerp(stableHistPacked,stableReferencePacked,temporalCurrentWeight);"));
    EXPECT_TRUE(Contains(shader, "resolvedDepth=float2(lerp(sameScreenDepth.x,stableCurrentDepth,temporalCurrentWeight),resolved.a);"));
    EXPECT_FALSE(Contains(shader, "resolved=float4(stableHist.rgb*stableHist.a,stableHist.a);"));
}

ACS_TEST(VolumetricClouds,
         SaturatedLightMarchAccountsForHighestActiveOrder) {
    constexpr f32 kCutoffOpticalDepth = 18.0f;
    constexpr f32 kOcclusion = 0.28f;
    constexpr f32 kThirdOcclusion = kOcclusion * kOcclusion;
    // 一次散乱用の旧打ち切り点では、三次散乱の光がまだ約24%残る。
    EXPECT_TRUE(
        std::exp(-kCutoffOpticalDepth * kThirdOcclusion) > 0.20f);
    // 最高次数の消散で18へ達した点なら、全次数の光が知覚限界より十分小さい。
    const f32 correctedOpticalDepth =
        kCutoffOpticalDepth / kThirdOcclusion;
    const f32 remaining =
        std::exp(-correctedOpticalDepth) +
        std::exp(-correctedOpticalDepth * kOcclusion) +
        std::exp(-correctedOpticalDepth * kThirdOcclusion);
    EXPECT_TRUE(remaining < 2.0e-7f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_EQ(
        CountOccurrences(
            shader,
            "if(lightDepths.z>18.0)break;"),
        static_cast<std::size_t>(1));
    EXPECT_TRUE(Contains(shader, "boolradianceValid="));
    EXPECT_TRUE(Contains(shader, "all(abs(col)<=65504.0)"));
    EXPECT_TRUE(Contains(shader, "col=max(col,0.0);"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudMacroSamplemacro=sampleCloudMacroLighting("
        "p,coverage);"));
    EXPECT_FALSE(Contains(shader, "cloudPositiveDensityNoiseThreshold("));
    EXPECT_TRUE(Contains(
        shader,
        "macro.shapePotential=cloudShapePotentialBands("
        "cloudUVW(p,macro.layerHeight,upperBand));"));
    EXPECT_TRUE(Contains(
        shader,
        "macro.shapeFrequencyWeights=cloudShapeFrequencyWeights("
        "maximumDomainFootprint);"));
}

ACS_TEST(VolumetricClouds,
         ShaderBranchOutputsAreExplicitlyInitialized) {
    const std::string source = ReadSkySource();
    const std::string shader =
        ExtractRawShader(source, "const char* kCloudCS");
    const std::string compactShader = CompactShader(shader);
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!shader.empty());

    // FXCの制御経路解析は、早期returnを持つ補助関数のout引数を完全には追跡しない。
    // 分岐を通る出力は入口で初期化し、密度関数は単一の戻り値で未定義値を防ぐ。
    EXPECT_TRUE(Contains(shader,
        "out float nearT,out float farT){\n"
        "    nearT=0.0;\n"
        "    farT=0.0;"));
    EXPECT_TRUE(Contains(shader,
        "out float t0,out float t1){\n"
        "    t0=0.0;\n"
        "    t1=0.0;"));
    EXPECT_TRUE(Contains(shader, "float outerNear=0.0,outerFar=0.0;"));
    EXPECT_TRUE(Contains(shader, "float height=(altitude-layer.x)*cloudFrameTerms.w;\n" "    if(upperBand)\n" "        height=(altitude-cloudUpperLayer.x)*cloudUpperLayer.z;\n" "    return saturate(height);"));
    EXPECT_TRUE(!Contains(shader, "if(upperBand)\n" "        return saturate((altitude-cloudUpperLayer.x)*"));
    EXPECT_TRUE(Contains(
        compactShader,
        "CloudMacroSamplesampleCloudMacroLighting("
        "float3p,floatweatherCoverage){"
        "returnsampleCloudMacroLightingBandLimited("
        "p,weatherCoverage,0.0.xxx,0.0.xxx);}"));
    EXPECT_TRUE(Contains(
        compactShader,
        "returnfloat2("
        "densityUpper,"
        "upperBand?1.0:0.0);"));
    EXPECT_FALSE(Contains(compactShader, "outfloatshapeResult"));
    EXPECT_TRUE(Contains(shader, "float densityResult=0.0;"));
    EXPECT_TRUE(Contains(shader, "return densityResult;"));
    EXPECT_TRUE(!Contains(shader, "if(disc<0.0){ nearT="));
    EXPECT_TRUE(!Contains(shader, "if(weatherMask<=0.001) return 0.0;"));
    EXPECT_TRUE(!Contains(shader, "if(profile<=0.001) return 0.0;"));
}

ACS_TEST(VolumetricClouds, ViewIntegrationDoesNotAnimateUnaveragedSamplingError) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    const std::string resolveShader = CompactShader(ExtractRawShader(source, "const char* kCloudResolveCS"));
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(!resolveShader.empty());

    // 視線積分はフレーム位相を使わず、固定Gauss点を前からBeer-Lambert合成する。
    // 16位相の採取画素は現在近傍へ制限した同一雲体履歴へ段階反映する。
    EXPECT_TRUE(Contains(
        shader,
        "floatphysicalDensityResolutionSpacing=cellLaneLength*"
        "CLOUD_DENSITY_GAUSS_MAXIMUM_GAP_FRACTION;"));
    EXPECT_TRUE(Contains(
        shader,
        "sample.physicalFraction=sampleFraction;"
        "floatcurrentSampleT=cloudIntervalDistanceAtFraction("
        "componentStartT,componentEndT,sampleFraction);"));
    EXPECT_FALSE(Contains(shader, "requestedSampleT"));
    EXPECT_TRUE(Contains(
        shader,
        "cloudLightingSourceAtPoint("
        "currentP,currentMacro,lightingContext,lowLodDensity,rayDirection)"));
    EXPECT_FALSE(Contains(shader, "densityCentroidFraction"));
    EXPECT_EQ(CountOccurrences(resolveShader, "resolved=current;"), static_cast<std::size_t>(2));
    EXPECT_TRUE(Contains(resolveShader, "resolved=lerp(histPacked,current,scheduledCurrentWeight);"));
    EXPECT_TRUE(Contains(resolveShader, "resolvedDepth=float2(curDepth,curA);"));
    EXPECT_TRUE(Contains(resolveShader, "resolvedDepth=nativeDepth;"));
    EXPECT_FALSE(Contains(shader, "jitterFrame"));
    EXPECT_FALSE(Contains(shader, "jitterSequence"));
    EXPECT_FALSE(Contains(shader, "CloudJitterHash2D"));
    EXPECT_FALSE(Contains(shader, "cloudRayIntervalPhase("));
    EXPECT_FALSE(Contains(shader, "CloudJitter01"));

    constexpr f32 kIntervalStart = 100.0f;
    constexpr f32 kIntervalEnd = 110.0f;
    const f32 midpoint = kIntervalStart + (kIntervalEnd - kIntervalStart) * 0.5f;
    EXPECT_NEAR(midpoint, 105.0f, 0.0f);
}

ACS_TEST(VolumetricClouds,
         HorizonCloudRadianceIsNotFlattenedIntoTheSkyColor) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!shader.empty());

    // Atmospheric perspective already applies distance-terminated haze during
    // composite. Replacing ray-marched radiance with skyColor at low elevation
    // destroys self-shadowing and makes every horizon cloud one flat color.
    EXPECT_FALSE(Contains(
        shader, "col=lerp(skyCol.rgb,col,smoothstep("));
    EXPECT_TRUE(Contains(
        shader, "cloudOut[pixelQ]=float4(col,resolvedA);"));
}

ACS_TEST(VolumetricClouds, CameraRelativeInverseViewProjectionIgnoresFarWorldTranslation) {
    constexpr f32 kAspect = 16.0f / 9.0f;
    constexpr f32 kNear = 0.05f;
    constexpr f32 kFar = 250000.0f;
    const FVec3 viewDirection{0.25f, -0.125f, 1.0f};

    CCamera originCamera;
    originCamera.SetPerspective(60.0f * kDeg2Rad, kAspect, kNear, kFar);
    originCamera.SetLookDirection(FVec3{}, viewDirection);

    const FVec3 farEye{64000.0f, 1.0f, 96000.0f};
    CCamera farCamera;
    farCamera.SetPerspective(60.0f * kDeg2Rad, kAspect, kNear, kFar);
    farCamera.SetLookDirection(farEye, viewDirection);

    const FMat4 originRelative = BuildCameraRelativeInverseViewProjection(originCamera.View(), originCamera.Projection());
    const FMat4 farRelative = BuildCameraRelativeInverseViewProjection(farCamera.View(), farCamera.Projection());
    ExpectMat4Near(farRelative, originRelative, 1.0e-5f);
    const FMat4 originRelativeViewProjection = BuildCameraRelativeViewProjection(originCamera.View(), originCamera.Projection());
    const FMat4 farRelativeViewProjection = BuildCameraRelativeViewProjection(farCamera.View(), farCamera.Projection());
    ExpectMat4Near(farRelativeViewProjection, originRelativeViewProjection, 1.0e-5f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    const std::string resolveShader = CompactShader(ExtractRawShader(source, "const char* kCloudResolveCS"));
    const std::string compositeShader = CompactShader(ExtractRawShader(source, "const char* kCloudCompPS"));
    const std::string atmosphereCompositeShader = CompactShader(ExtractRawShader(source, "const char* kCloudCompAtmosPS"));
    const std::string skyShader = CompactShader(ExtractRawShader(source, "const char* kSkyHLSL"));
    const std::string compactSource = CompactShader(source);
    EXPECT_TRUE(Contains(compactSource, "cb.invViewProj=camera_relative_inv_view_proj;"));
    EXPECT_TRUE(Contains(compactSource, "voidCVolumetricClouds::RenderComputeCameraRelative("));
    EXPECT_TRUE(Contains(compactSource, "cb.inv_view_proj=BuildCameraRelativeInverseViewProjection(camera.View(),camera.Projection());"));
    EXPECT_TRUE(Contains(skyShader, "float3dir=CameraRelativeViewDirection(float2(v.ndc.x,-v.ndc.y));"));
    EXPECT_FALSE(Contains(skyShader, "farHomogeneous.xyz/farHomogeneous.w"));
    EXPECT_TRUE(Contains(shader, "float3dir=CloudViewDirection(clip.xy);"));
    EXPECT_TRUE(Contains(shader, "floatxElevation=dot(CloudViewDirection(xClip.xy),localUp);floatyElevation=dot(CloudViewDirection(yClip.xy),localUp);"));
    EXPECT_FALSE(Contains(shader, "farHomogeneous.xyz/farHomogeneous.w"));
    EXPECT_TRUE(Contains(resolveShader, "float3stableRay=ResolveViewDirection(uv);"));
    EXPECT_TRUE(Contains(resolveShader, "float3ray=ResolveViewDirection(uv);"));
    EXPECT_FALSE(Contains(resolveShader, "farHomogeneous.xyz/farHomogeneous.w"));
    EXPECT_TRUE(Contains(resolveShader, "float3stablePrevCameraRelativeP=stableRay*sameScreenDepth.x+(camPos.xyz-prevCamPos.xyz)-float3(stableWindDelta*0.9284767,0.0,stableWindDelta*0.3713907);"));
    EXPECT_TRUE(Contains(resolveShader, "float3prevCameraRelativeP=ray*reprojectionDepth+(camPos.xyz-prevCamPos.xyz)-float3(windDelta*0.9284767,0.0,windDelta*0.3713907);"));
    EXPECT_TRUE(Contains(resolveShader, "float4(prevCameraRelativeP,1.0),prevCameraRelativeViewProj"));
    EXPECT_TRUE(Contains(resolveShader, "floatexpectedDepth=length(prevCameraRelativeP);"));
    EXPECT_FALSE(Contains(resolveShader, "float3worldP="));
    EXPECT_FALSE(Contains(resolveShader, "prevWorldP"));
    const std::string compositeShaders[]{
        compositeShader, atmosphereCompositeShader};
    for (const std::string& composite : compositeShaders) {
        EXPECT_TRUE(Contains(composite, "floatcenterElevation=dot(CloudCompositeViewDirection(v.farPoint),groundHorizon.xyz);"));
        EXPECT_TRUE(Contains(composite, "floatsceneDistance=length(world.xyz);"));
        EXPECT_FALSE(Contains(composite, "world.xyz-camPos.xyz"));
    }

    const std::string compactEditor = CompactShader(ReadEditorAbiSource());
    const std::string compactLegacy = CompactShader(ReadLegacyScene3DAdapterSource());
    EXPECT_TRUE(Contains(compactEditor, "BuildCameraRelativeViewProjection(cam.View(),cam.Projection())"));
    EXPECT_TRUE(Contains(compactEditor, "VolumetricCloudViewCutDetected(h.prev_camera_relative_inv_vp,h.prev_temporal_camera_eye,camera_relative_inv_vp,eye)"));
    EXPECT_TRUE(Contains(compactEditor, "vclouds3d.RenderComputeCameraRelative("));
    EXPECT_TRUE(Contains(compactEditor, "ibl3d.DrawEnvSkyboxCameraRelative("));
    EXPECT_TRUE(Contains(compactEditor, "sk.inv_view_proj=camera_relative_inv_vp;"));
    EXPECT_TRUE(Contains(compactEditor, "h.prev_camera_relative_inv_vp=camera_relative_inv_vp;"));
    EXPECT_TRUE(Contains(compactLegacy, "BuildCameraRelativeInverseViewProjection(m_Camera.View(),m_Camera.Projection())"));
    EXPECT_TRUE(Contains(compactLegacy, "m_Clouds.RenderComputeCameraRelative("));
    EXPECT_TRUE(Contains(compactLegacy, "m_Ibl.DrawEnvSkyboxCameraRelative("));
}

ACS_TEST(VolumetricClouds, EditorAndCppAdaptersUploadThePhysicalGroundLighting)
{
    /** Editor経路の雲照明更新を含む実装。 */
    const std::string editor = CompactShader(ReadEditorAbiSource());
    /** 通常C++経路の雲照明更新を含む実装。 */
    const std::string legacy = CompactShader(ReadLegacyScene3DAdapterSource());
    EXPECT_TRUE(!editor.empty());
    EXPECT_TRUE(!legacy.empty());
    EXPECT_TRUE(Contains(editor, "if(host.q_sky_mode==1){FAtmosphereParamsatmosphere{};"));
    EXPECT_TRUE(Contains(editor, "lighting.SkyZenithColor=CAtmosphere::EvaluateSkyRadiance("));
    EXPECT_TRUE(Contains(editor, "lighting.GroundColor=CAtmosphere::EvaluateSkyRadiance("));
    EXPECT_TRUE(Contains(editor, "lighting.SkyZenithColor=host.sky_zenith;"));
    EXPECT_TRUE(Contains(editor, "lighting.GroundColor=host.sky_ground;"));
    EXPECT_TRUE(Contains(legacy, "FAtmosphereParamsatmosphere=m_AtmosphereParams;"));
    EXPECT_TRUE(Contains(legacy, "lighting.SkyZenithColor=CAtmosphere::EvaluateSkyRadiance("));
    EXPECT_TRUE(Contains(legacy, "lighting.GroundColor=CAtmosphere::EvaluateSkyRadiance("));
    EXPECT_TRUE(Contains(legacy, "FVec3ALegacyScene3DAdapter::SunColorForClouds()constnoexcept{"));
    EXPECT_TRUE(Contains(legacy, "if(m_Lights.DirectionalCount()==0u)returnkDefaultCloudSunColor;"));
    const auto sunRadiance = legacy.find("constFVec3sun_radiance=PhysicalSunIntensity(SunColorForClouds());");
    const auto renderClouds = legacy.find("m_Clouds.RenderComputeCameraRelative(", sunRadiance);
    EXPECT_TRUE(sunRadiance != legacy.npos);
    EXPECT_TRUE(renderClouds != legacy.npos);
    EXPECT_TRUE(Contains(legacy, "constFVec3physicalHorizon=CAtmosphere::EvaluateSkyRadiance("));
    EXPECT_TRUE(Contains(legacy, "SunDirection(),sun_radiance,physicalHorizon"));
}

ACS_TEST(VolumetricClouds, EnvironmentSkyBakeTracksObserverAltitude)
{
    const std::string legacy = ReadLegacyScene3DAdapterSource();
    EXPECT_TRUE(!legacy.empty());
    EXPECT_TRUE(Contains(
        legacy,
        "const f32 observer_altitude =\n"
        "        m_Camera.Eye().y > 0.0f ? m_Camera.Eye().y : 0.0f;"));
    EXPECT_TRUE(Contains(legacy, "m_IblBakedCloudSignature"));
    EXPECT_TRUE(Contains(legacy, "BakeEquirectAtAltitude("));
    EXPECT_TRUE(Contains(
        legacy,
        "const u16 observer_altitude_bucket =\n"
        "        QuantizedIblObserverAltitude("));
}

ACS_TEST(VolumetricClouds,
         GroundHorizonUsesProjectionAwareAnalyticPixelCoverage) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(Contains(
        shader,
        "float3localUp=groundHorizon.xyz;"
        "floatsignedElevation=dot(dir,localUp);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatgroundCutoff=groundHorizon.w;"
        "if(groundCutoff>=-1.0&&cameraBelowCloudBase){"));
    EXPECT_TRUE(Contains(
        shader, "boolcloudCameraBelowCloudBase(){"));
    EXPECT_TRUE(Contains(
        shader, "floatcameraAltitude=cloudAltitude(camPos.xyz);"));
    EXPECT_TRUE(Contains(
        shader, "if(groundCutoff>=-1.0&&cameraBelowCloudBase){"));
    EXPECT_TRUE(Contains(
        shader, "if(groundHorizonCoverage<=0.001&&cameraBelowCloudBase){"));
    EXPECT_FALSE(Contains(
        shader, "floatgroundRadiusRatio=CLOUD_PLANET_RADIUS/"));
    EXPECT_TRUE(Contains(
        shader,
        "float2pixelCenter=float2(rayPixel)+0.5;"
        "floatxOffset=rayPixel.x+1u<(uint)rayDimensions.x?1.0:-1.0;"
        "floatyOffset=rayPixel.y+1u<(uint)rayDimensions.y?1.0:-1.0;"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcoverageHalfWidth=max("
        "0.5*(abs(xElevation-signedElevation)+"
        "abs(yElevation-signedElevation)),1e-6);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatxElevation=dot(CloudViewDirection(xClip.xy),localUp);"
        "floatyElevation=dot(CloudViewDirection(yClip.xy),localUp);"));
    EXPECT_FALSE(Contains(shader, "xWp/="));
    EXPECT_FALSE(Contains(shader, "yWp/="));
    EXPECT_TRUE(Contains(
        shader,
        "groundHorizonCoverage=smoothstep("
        "groundCutoff-coverageHalfWidth,"
        "groundCutoff+coverageHalfWidth,signedElevation);"));
    EXPECT_FALSE(Contains(shader, "floatverticalOffset="));
    EXPECT_FALSE(Contains(shader, "groundCutoff+coverageWidth"));
    EXPECT_FALSE(Contains(
        shader,
        "cameraAltitude<layer.x&&signedElevation<-0.002"));
    EXPECT_EQ(
        CountOccurrences(shader, "float4groundHorizon;"),
        static_cast<std::size_t>(1));

    const std::string compositeVertexShader = CompactShader(ExtractRawShader(source, "const char* kCloudCompVS"));
    const std::string resolveShader = CompactShader(ExtractRawShader(source, "const char* kCloudResolveCS"));
    const std::string compositeShader = CompactShader(ExtractRawShader(source, "const char* kCloudCompPS"));
    const std::string atmosphereCompositeShader = CompactShader(ExtractRawShader(source, "const char* kCloudCompAtmosPS"));
    const std::string compositeShaders[]{compositeShader, atmosphereCompositeShader};
    const std::string compactSource = CompactShader(source);
    EXPECT_EQ(CountOccurrences(resolveShader, "float4groundHorizon;"), static_cast<std::size_t>(1));
    EXPECT_TRUE(Contains(compactSource, "offsetof(FCloudCb,groundHorizon)==320u"));
    EXPECT_TRUE(Contains(compactSource, "sizeof(FCloudCb)==736"));
    EXPECT_TRUE(Contains(compactSource, "offsetof(FCloudCb,cloudFrameTerms)==336u"));
    EXPECT_TRUE(Contains(compactSource, "CBSize<FCloudCb>()==768u"));
    EXPECT_TRUE(Contains(resolveShader, "floatoutA=saturate(resolved.a);resolvedDepth.y=outA;"));
    EXPECT_FALSE(Contains(resolveShader, "CloudGroundCoverage"));
    EXPECT_FALSE(Contains(resolveShader, "outA*=outputGroundCoverage"));
    EXPECT_FALSE(Contains(resolveShader, "resolved.rgb*=outputGroundCoverage"));
    EXPECT_TRUE(Contains(compositeVertexShader, "structVSOut{float4pos:SV_POSITION;float2uv:TEXCOORD0;float4farPoint:TEXCOORD1;};"));
    EXPECT_TRUE(Contains(compositeVertexShader, "float4CloudFarPoint(float2uv){float4clip=float4(uv.x*2.0-1.0,-(uv.y*2.0-1.0),1.0,1.0);returnmul(clip,invViewProj);}"));
    EXPECT_TRUE(Contains(compositeVertexShader, "o.farPoint=CloudFarPoint(uv);"));
    for (const std::string& composite : compositeShaders) {
        EXPECT_EQ(CountOccurrences(composite, "float4groundHorizon;"), static_cast<std::size_t>(1));
        EXPECT_EQ(CountOccurrences(composite, "float4cloudUpperLayer;"), static_cast<std::size_t>(1));
        EXPECT_TRUE(Contains(composite, "float4worldOrigin;float4shadowGrid;float4shadowState;float4groundHorizon;"));
        EXPECT_TRUE(Contains(composite, "structVSOut{float4pos:SV_POSITION;float2uv:TEXCOORD0;float4farPoint:TEXCOORD1;};"));
        EXPECT_TRUE(Contains(composite, "boolCloudCameraBelowCloudBase(){"));
        EXPECT_TRUE(Contains(composite, "floatCloudGroundCoverage(VSOutv){floatresult=1.0;boolcameraBelowCloudBase=CloudCameraBelowCloudBase();if(groundHorizon.w>=-1.0&&cameraBelowCloudBase){"));
        EXPECT_TRUE(Contains(composite, "uint2pixel=min(uint2(v.pos.xy),fullSize-1u);floatcenterElevation=dot(CloudCompositeViewDirection(v.farPoint),groundHorizon.xyz);"));
        EXPECT_TRUE(Contains(composite, "float3CloudCompositeViewDirection(float4farHomogeneous)"));
        EXPECT_TRUE(Contains(composite, "float4xFarP=v.farPoint+xOffset*(2.0/dims.z)*invViewProj[0];float4yFarP=v.farPoint-yOffset*(2.0/dims.w)*invViewProj[1];"));
        EXPECT_TRUE(Contains(composite, "floatcoverageHalfWidth=max(0.5*(abs(xElevation-centerElevation)+abs(yElevation-centerElevation)),1e-6);"));
        EXPECT_TRUE(Contains(composite, "result=smoothstep(groundHorizon.w-coverageHalfWidth,groundHorizon.w+coverageHalfWidth,centerElevation);"));
        EXPECT_TRUE(Contains(composite, "floatgroundCoverage=CloudGroundCoverage(v);cloud.a*=groundCoverage;cloudHit.y*=groundCoverage;"));
        EXPECT_FALSE(Contains(composite, "centerFarP=mul("));
        EXPECT_FALSE(Contains(composite, "xFarP=mul("));
        EXPECT_FALSE(Contains(composite, "yFarP=mul("));
        EXPECT_FALSE(Contains(composite, "xFarP/=xFarP.w"));
        EXPECT_FALSE(Contains(composite, "yFarP/=yFarP.w"));
        EXPECT_FALSE(Contains(composite, "cloud.rgb*=groundCoverage"));
        const std::size_t coverageCall = composite.find("floatgroundCoverage=CloudGroundCoverage(v);");
        const std::size_t sceneDepthRead = composite.find("floatdepth=sceneDepth.SampleLevel", coverageCall);
        EXPECT_TRUE(coverageCall != std::string::npos);
        EXPECT_TRUE(sceneDepthRead != std::string::npos);
        EXPECT_TRUE(coverageCall < sceneDepthRead);
    }
    // 旧順序では16位相中15回の履歴表示だけで、被覆0.5がほぼ透明まで減衰する。
    constexpr f64 partialCoverage = 0.5;
    f64 repeatedlyMaskedHistory = 1.0;
    f64 compositeOnlyVisibleAlpha = 0.0;
    for (u32 phase = 1u; phase < 16u; ++phase) {
        repeatedlyMaskedHistory *= partialCoverage;
        compositeOnlyVisibleAlpha = 1.0 * partialCoverage;
    }
    EXPECT_TRUE(repeatedlyMaskedHistory < 0.0001);
    EXPECT_NEAR(compositeOnlyVisibleAlpha, partialCoverage, 0.0);
    // 以前の境界修復が使った無関係な履歴画素の借用は戻さない。
    EXPECT_FALSE(Contains(resolveShader, "outputCoveragePixels"));
    EXPECT_FALSE(Contains(resolveShader, "edgePremul"));
    EXPECT_FALSE(Contains(resolveShader, "edgeColor=historyColor.Load"));

    const FVolumetricCloudLayer layer{};
    const auto seaLevel = ResolveVolumetricCloudGroundHorizon(
        FVec3{0.0f, 0.0f, 0.0f}, layer, FVec3{});
    ExpectVec3Near(
        seaLevel.local_up, FVec3{0.0f, 1.0f, 0.0f}, 1.0e-6f);
    EXPECT_NEAR(seaLevel.ground_cutoff, 0.0f, 1.0e-6f);

    constexpr f32 observerAltitude = 1000.0f;
    const auto airborne = ResolveVolumetricCloudGroundHorizon(
        FVec3{0.0f, observerAltitude, 0.0f}, layer, FVec3{});
    const f32 radiusRatio =
        kVolumetricCloudPlanetRadius /
        (kVolumetricCloudPlanetRadius + observerAltitude);
    const f32 expectedCutoff =
        -Sqrt(1.0f - radiusRatio * radiusRatio);
    EXPECT_NEAR(airborne.ground_cutoff, expectedCutoff, 1.0e-6f);

    const FVec3 translatedOrigin{8192.0f, 0.0f, -4096.0f};
    const auto translated = ResolveVolumetricCloudGroundHorizon(
        FVec3{translatedOrigin.x,
              translatedOrigin.y + observerAltitude,
              translatedOrigin.z},
        layer, translatedOrigin);
    ExpectVec3Near(
        translated.local_up, airborne.local_up, 1.0e-6f);
    EXPECT_NEAR(
        translated.ground_cutoff,
        airborne.ground_cutoff, 1.0e-6f);

    const auto insideLayer = ResolveVolumetricCloudGroundHorizon(
        FVec3{0.0f, layer.base_height, 0.0f}, layer, FVec3{});
    EXPECT_TRUE(insideLayer.ground_cutoff < -1.0f);
    const auto hostile = ResolveVolumetricCloudGroundHorizon(
        FVec3{std::numeric_limits<f32>::quiet_NaN(), 0.0f, 0.0f},
        layer, FVec3{});
    ExpectVec3Near(
        hostile.local_up, FVec3{0.0f, 1.0f, 0.0f}, 0.0f);
    EXPECT_TRUE(hostile.ground_cutoff < -1.0f);
    const auto hostileOrigin = ResolveVolumetricCloudGroundHorizon(
        FVec3{}, layer,
        FVec3{0.0f, std::numeric_limits<f32>::infinity(), 0.0f});
    ExpectVec3Near(
        hostileOrigin.local_up, FVec3{0.0f, 1.0f, 0.0f}, 0.0f);
    EXPECT_TRUE(hostileOrigin.ground_cutoff < -1.0f);
    const auto overflow = ResolveVolumetricCloudGroundHorizon(
        FVec3{std::numeric_limits<f32>::max(), 0.0f,
              std::numeric_limits<f32>::max()},
        layer, FVec3{});
    ExpectVec3Near(
        overflow.local_up, FVec3{0.0f, 1.0f, 0.0f}, 0.0f);
    EXPECT_TRUE(overflow.ground_cutoff < -1.0f);
    const auto planetCenter = ResolveVolumetricCloudGroundHorizon(
        FVec3{0.0f, -kVolumetricCloudPlanetRadius, 0.0f},
        layer, FVec3{});
    ExpectVec3Near(
        planetCenter.local_up, FVec3{0.0f, 1.0f, 0.0f}, 0.0f);
    EXPECT_TRUE(planetCenter.ground_cutoff < -1.0f);

    // Compare the CPU-hoisted result against the former per-pixel HLSL
    // float equations at representative altitudes, curvature offsets and the
    // exact layer boundary. This guards the one-pixel physical tangent.
    const FVec3 representativeCameras[]{
        FVec3{0.0f, -250.0f, 0.0f},
        FVec3{0.0f, 0.0f, 0.0f},
        FVec3{10000.0f, 250.0f, -12000.0f},
        FVec3{30000.0f, 1000.0f, 40000.0f},
        FVec3{0.0f, layer.base_height - 0.001f, 0.0f},
        FVec3{0.0f, layer.base_height, 0.0f}};
    for (const FVec3 camera : representativeCameras) {
        const auto expected =
            GroundHorizonHlslReferenceForTest(
                camera, layer, FVec3{});
        const auto actual =
            ResolveVolumetricCloudGroundHorizon(
                camera, layer, FVec3{});
        ExpectVec3Near(actual.local_up, expected.local_up, 1.0e-6f);
        EXPECT_NEAR(
            actual.ground_cutoff,
            expected.ground_cutoff, 1.0e-6f);
        if (actual.ground_cutoff >= -1.0f) {
            constexpr f32 tangentDx = 0.003f;
            constexpr f32 tangentDy = -0.002f;
            constexpr f32 tangentOffsets[]{
                -0.0025f, 0.0f, 0.0025f};
            for (const f32 offset : tangentOffsets) {
                const f32 signedElevation =
                    expected.ground_cutoff + offset;
                const f32 oldCoverage =
                    ResolveVolumetricCloudHorizonCoverage(
                        signedElevation,
                        expected.ground_cutoff,
                        tangentDx, tangentDy);
                const f32 hoistedCoverage =
                    ResolveVolumetricCloudHorizonCoverage(
                        signedElevation,
                        actual.ground_cutoff,
                        tangentDx, tangentDy);
                EXPECT_NEAR(
                    hoistedCoverage, oldCoverage, 1.0e-4f);
            }
        }
    }

    FVolumetricCloudLayer extremeLayer = layer;
    extremeLayer.base_height = 50000000.0f;
    extremeLayer.top_height = 50004000.0f;
    const FVec3 extremeCamera{
        10000000.0f, 10000000.0f, -10000000.0f};
    const auto extremeExpected =
        GroundHorizonHlslReferenceForTest(
            extremeCamera, extremeLayer, FVec3{});
    const auto extremeActual =
        ResolveVolumetricCloudGroundHorizon(
            extremeCamera, extremeLayer, FVec3{});
    ExpectVec3Near(
        extremeActual.local_up,
        extremeExpected.local_up, 1.0e-6f);
    EXPECT_NEAR(
        extremeActual.ground_cutoff,
        extremeExpected.ground_cutoff, 1.0e-6f);

    constexpr f32 cutoff = -0.002f;
    constexpr f32 deltaX = 0.003f;
    constexpr f32 deltaY = -0.002f;
    constexpr f32 footprint = 0.0025f;
    EXPECT_NEAR(
        ResolveVolumetricCloudHorizonCoverage(
            cutoff, cutoff, deltaX, deltaY),
        0.5f, 1e-6f);
    EXPECT_NEAR(
        ResolveVolumetricCloudHorizonCoverage(
            cutoff - footprint, cutoff, deltaX, deltaY),
        0.0f, 1e-6f);
    EXPECT_NEAR(
        ResolveVolumetricCloudHorizonCoverage(
            cutoff + footprint, cutoff, deltaX, deltaY),
        1.0f, 1e-6f);

    f32 previousCoverage = -1.0f;
    for (i32 i = -200; i <= 200; ++i) {
        const f32 elevation = cutoff + static_cast<f32>(i) * 0.000025f;
        const f32 coverage = ResolveVolumetricCloudHorizonCoverage(
            elevation, cutoff, deltaX, deltaY);
        EXPECT_TRUE(std::isfinite(coverage));
        EXPECT_TRUE(coverage >= previousCoverage);
        EXPECT_TRUE(coverage >= 0.0f);
        EXPECT_TRUE(coverage <= 1.0f);
        previousCoverage = coverage;
    }
    constexpr f32 symmetricOffset = 0.0007f;
    const f32 below = ResolveVolumetricCloudHorizonCoverage(
        cutoff - symmetricOffset, cutoff, deltaX, deltaY);
    const f32 above = ResolveVolumetricCloudHorizonCoverage(
        cutoff + symmetricOffset, cutoff, deltaX, deltaY);
    EXPECT_NEAR(below + above, 1.0f, 1e-5f);
    const f32 before = ResolveVolumetricCloudHorizonCoverage(
        cutoff - 1.0e-7f, cutoff, deltaX, deltaY);
    const f32 after = ResolveVolumetricCloudHorizonCoverage(
        cutoff + 1.0e-7f, cutoff, deltaX, deltaY);
    EXPECT_TRUE(std::fabs(after - before) < 0.001f);
    EXPECT_NEAR(
        ResolveVolumetricCloudHorizonCoverage(
            std::numeric_limits<f32>::infinity(),
            cutoff, deltaX, deltaY),
        0.0f, 0.0f);
    EXPECT_NEAR(
        ResolveVolumetricCloudHorizonCoverage(
            cutoff, cutoff,
            std::numeric_limits<f32>::quiet_NaN(), deltaY),
        0.0f, 0.0f);
}

ACS_TEST(VolumetricClouds, StableHistoryStoresUnmaskedCloudUntilFinalComposite) {
    const std::string source = ReadSkySource();
    const std::string compactSource = CompactShader(source);
    const std::string resolveShader = CompactShader(ExtractRawShader(source, "const char* kCloudResolveCS"));
    const std::string compositeShader = CompactShader(ExtractRawShader(source, "const char* kCloudCompPS"));
    const std::string atmosphereCompositeShader = CompactShader(ExtractRawShader(source, "const char* kCloudCompAtmosPS"));
    EXPECT_TRUE(!resolveShader.empty());

    // 小さなカメラ移動で保持した履歴も、被覆前の値として一度だけ公開する。
    EXPECT_TRUE(Contains(compactSource, "constbooltemporalHistoryStationary=historyValid&&cameraDeltaSquared<=0.0025f&&matrixDelta<=0.002f;"));
    EXPECT_TRUE(Contains(resolveShader, "boolstableUnscheduled=temporal.x>0.5&&temporalSuperRes&&!scheduled&&worldOrigin.w>0.5&&temporalMismatch<0.08;"));

    const std::size_t stableAccept = resolveShader.find("if(stableDepthOk&&stableAlphaOk){");
    const std::size_t fallbackGate = resolveShader.find("if(!stableHistoryResolved){", stableAccept);
    const std::size_t finalColorWrite = resolveShader.find("historyColorOut[tid.xy]=float4(", fallbackGate);
    EXPECT_TRUE(stableAccept != std::string::npos);
    EXPECT_TRUE(fallbackGate != std::string::npos);
    EXPECT_TRUE(finalColorWrite != std::string::npos);
    EXPECT_TRUE(stableAccept < fallbackGate);
    EXPECT_TRUE(fallbackGate < finalColorWrite);
    EXPECT_FALSE(Contains(resolveShader, "possibleGroundEdge"));
    EXPECT_FALSE(Contains(resolveShader, "outputGroundCoverage"));
    EXPECT_TRUE(Contains(resolveShader, "resolvedDepth.y=outA;if(outA<=0.001){resolved.rgb=0.0;outA=0.0;resolvedDepth=float2(250001.0,0.0);}"));
    EXPECT_TRUE(Contains(compositeShader, "cloud.a*=groundCoverage;cloudHit.y*=groundCoverage;"));
    EXPECT_TRUE(Contains(atmosphereCompositeShader, "cloud.a*=groundCoverage;cloudHit.y*=groundCoverage;"));

    if (stableAccept != std::string::npos && fallbackGate != std::string::npos && stableAccept < fallbackGate) {
        const std::string stableAcceptPath = resolveShader.substr(stableAccept, fallbackGate - stableAccept);
        EXPECT_TRUE(Contains(stableAcceptPath, "stableHistPacked=CloudTemporalClipHistory(stableHistPacked,stableCurrentMin,stableCurrentMax);}" "resolved=lerp(stableHistPacked,stableReferencePacked,temporalCurrentWeight);"));
        EXPECT_TRUE(Contains(stableAcceptPath, "resolvedDepth=float2(lerp(sameScreenDepth.x,stableCurrentDepth,temporalCurrentWeight),resolved.a);"));
        EXPECT_TRUE(Contains(stableAcceptPath, "stableHistoryResolved=true;"));
        EXPECT_FALSE(Contains(stableAcceptPath, "historyColorOut[tid.xy]=stableHist;"));
        EXPECT_FALSE(Contains(stableAcceptPath, "return;"));
    }
}

ACS_TEST(VolumetricClouds,
         RawDx12ShaderCompilationAcceptsHoistedCloudCbLayout) {
#if !WITH_RENDER_DILIGENT
    auto compiled = CVolumetricClouds::CompileShadersCpu();
    EXPECT_TRUE(compiled.IsOk());
    if (compiled.IsOk()) {
        EXPECT_TRUE(compiled.Value().noise_filter.Get() != nullptr);
        EXPECT_TRUE(compiled.Value().shadow.Get() != nullptr);
        EXPECT_TRUE(compiled.Value().world_shadow.Get() != nullptr);
        EXPECT_TRUE(compiled.Value().shadow_finalize.Get() == nullptr);
        EXPECT_EQ(
            compiled.Value().Status(),
            EShaderStatus::Ready);
    }
#else
    // Diligent構成は描画初期化時に同じソースを実バックエンドでコンパイルする。
    // CPUだけの直接コンパイルは生DX12構成で検証する。
    EXPECT_TRUE(true);
#endif
}

ACS_TEST(VolumetricClouds,
         CloudPhaseAndHigherOrderTransportAreNormalizedAndMatchTheShader) {
    const FVolumetricCloudLighting lighting{};
    f32 minimumPhase = 1000.0f;
    f32 maximumPhase = -1000.0f;
    for (u32 sample = 0; sample <= 256u; ++sample) {
        const f32 cosine =
            -1.0f + 2.0f * static_cast<f32>(sample) / 256.0f;
        const f32 phase = DefaultCloudPhaseForTest(cosine);
        EXPECT_TRUE(std::isfinite(phase));
        EXPECT_TRUE(phase >= lighting.PhaseMin);
        EXPECT_TRUE(phase <= lighting.PhaseMax);
        if (phase < minimumPhase) minimumPhase = phase;
        if (phase > maximumPhase) maximumPhase = phase;
    }

    // HG位相と既定の二ローブ混合を全立体角で数値積分し、確率密度の総和が1になることを確認する。
    constexpr u32 kSphereSampleCount = 16384u;
    constexpr f64 kTwoPi = 6.28318530717958647692;
    const f64 cosineWidth = 2.0 / static_cast<f64>(kSphereSampleCount);
    f64 forwardIntegral = 0.0;
    f64 backwardIntegral = 0.0;
    f64 defaultIntegral = 0.0;
    for (u32 sample = 0u; sample < kSphereSampleCount; ++sample) {
        const f32 cosine = static_cast<f32>(
            -1.0 + (static_cast<f64>(sample) + 0.5) * cosineWidth);
        const f64 forwardPhase = static_cast<f64>(
            CloudHenyeyGreensteinForTest(cosine, lighting.PhaseForward));
        const f64 backwardPhase = static_cast<f64>(
            CloudHenyeyGreensteinForTest(cosine, lighting.PhaseBackward));
        forwardIntegral += forwardPhase * kTwoPi * cosineWidth;
        backwardIntegral += backwardPhase * kTwoPi * cosineWidth;
        defaultIntegral +=
            (forwardPhase * static_cast<f64>(lighting.PhaseBlend) +
             backwardPhase * static_cast<f64>(1.0f - lighting.PhaseBlend)) *
            kTwoPi * cosineWidth;
    }
    EXPECT_NEAR(forwardIntegral, 1.0, 2.0e-4);
    EXPECT_NEAR(backwardIntegral, 1.0, 2.0e-4);
    EXPECT_NEAR(defaultIntegral, 1.0, 2.0e-4);

    // 許容上限のgでも分母を見た目用に丸めず、HGの閉形式と同じ前方ピークを保つ。
    constexpr f32 kMaximumEccentricity =
        kVolumetricCloudMaxPhaseEccentricity;
    constexpr f32 kFourPi = 12.56637061435917295385f;
    const f32 peakDenominator =
        (1.0f - kMaximumEccentricity) *
        (1.0f - kMaximumEccentricity);
    const f32 expectedMaximumForwardPhase =
        (1.0f + kMaximumEccentricity) /
        (kFourPi * peakDenominator);
    EXPECT_NEAR(
        CloudHenyeyGreensteinForTest(
            1.0f, kMaximumEccentricity),
        expectedMaximumForwardPhase,
        expectedMaximumForwardPhase * 1.0e-4f);

    // カメラから雲への視線と、雲から太陽への方向が一致すると、太陽光子の
    // 入射方向とカメラへの出射方向も一致し、前方散乱のピークになる。
    const f32 backward = DefaultCloudPhaseForTest(-1.0f);
    const f32 side = DefaultCloudPhaseForTest(0.0f);
    const f32 forward = DefaultCloudPhaseForTest(1.0f);
    EXPECT_TRUE(backward >= 0.030f && backward <= 0.036f);
    EXPECT_TRUE(side >= 0.036f && side <= 0.041f);
    EXPECT_TRUE(forward >= 0.67f && forward <= 0.70f);
    EXPECT_TRUE(forward > side * 15.0f);
    EXPECT_TRUE(maximumPhase <= lighting.PhaseMax);
    EXPECT_TRUE(minimumPhase >= lighting.PhaseMin);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(Contains(
        shader,
        "CloudLightingContextcloudLightingContextForRayDirection("
        "CloudLightingContextcontext,float3rayDirection){"));
    EXPECT_TRUE(Contains(
        shader,
        "voidcloudScatteringPhasesForDirections("
        "float3viewDirection,float3sunDirection,"
        "outfloatphase,outfloatphaseMulti){"
        "floatcosA=clamp(dot(viewDirection,sunDirection),-1.0,1.0);"));
    EXPECT_TRUE(Contains(
        shader,
        "floathg(floatc,floatg){floata=abs(g);"
        "floatoneMinusA=1.0-a;floatalignedC=g>=0.0?c:-c;"
        "floatd=oneMinusA*oneMinusA+2.0*a*"
        "max(1.0-alignedC,0.0);return(oneMinusA*(1.0+a))/"
        "(12.566370*pow(max(d,1e-6),1.5));}"));
    EXPECT_FALSE(Contains(shader, "cloudForwardPhaseWeight"));
    EXPECT_TRUE(Contains(
        shader,
        "floatforwardPhase=hg(cosA,cloudLightingPhase.x);"
        "floatbackwardPhase=hg(cosA,cloudLightingPhase.y);"
        "phase=clamp("
        "lerp(backwardPhase,forwardPhase,saturate(cloudLightingPhase.z)),"
        "cloudLightingMulti.y,cloudLightingMulti.z);"));
    EXPECT_FALSE(Contains(shader, "4.0*hg("));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudReducedIntervalScatteringWeight("
        "floatopticalDepth,floatreducedOpticalDepth,"
        "floatintervalTransmittance,floatcontribution){"));
    EXPECT_TRUE(Contains(
        shader,
        "floatsafeReducedOpticalDepth="
        "max(reducedOpticalDepth,0.0);"));
    EXPECT_TRUE(Contains(
        shader,
        "normalizedAbsorption=1.0-safeReducedOpticalDepth*0.5+"
        "reducedOpticalDepthSquared/6.0-"
        "reducedOpticalDepthSquared*safeReducedOpticalDepth/24.0;"));
    EXPECT_TRUE(Contains(
        shader,
        "floatmaximumWeight=safeContribution*safeOpticalDepth;"
        "floatweight=maximumWeight*normalizedAbsorption;"));
    EXPECT_TRUE(Contains(
        shader,
        "resolvedWeight=min(weight,maximumWeight);"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudFourStateTransportResultLanesfirstTransport="
        "cloudFourStateTransportLanes("
        "densityLightingSample.densityStates.xxxx,"
        "densityLightingSample.densityStates.yyyy,"
        "densityLightingSample.densityStates.zzzz,"
        "densityLightingSample.densityStates.wwww,"
        "firstOrderExtinction,"
        "densityLightingSample.correlationLength.xxxx,"
        "laneSegmentLengths,firstOrderTransportState);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatviewOpticalDepth=cloudOpticalDepthFromAbsorption("
        "firstIntervalAbsorption);"));
    EXPECT_EQ(
        CountOccurrences(shader, "exp(-viewOpticalDepthLanes"),
        static_cast<std::size_t>(0));
    EXPECT_EQ(
        CountOccurrences(shader, "exp(-secondOpticalDepthLanes"),
        static_cast<std::size_t>(0));
    EXPECT_EQ(
        CountOccurrences(shader, "exp(-thirdOpticalDepthLanes"),
        static_cast<std::size_t>(0));
    EXPECT_EQ(
        CountOccurrences(shader, "floatphase=lerp("),
        static_cast<std::size_t>(0));
    EXPECT_TRUE(Contains(
        shader,
        "phaseMulti=clamp("
        "hg(cosA,cloudMultiPhase.x),"
        "cloudLightingMulti.y,cloudLightingMulti.z);"));
    EXPECT_TRUE(Contains(
        shader,
        "lightingContext.lightExtinction="
        "density*cloudLightingExtinction.y;"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudLightingContextphysicalLaneLightingContext="
        "cloudLightingContextForRayDirection("
        "lightingContext,physicalRayDirection);"));
    EXPECT_TRUE(Contains(
        shader,
        "source.higherOrderInScatterFactor=lerp("
        "1.0,inScatterDepth,"
        "cloudCondensationFiniteSaturate(cloudLightingExtinction.w));"));
    EXPECT_FALSE(Contains(shader, "inScatterVertical"));
    EXPECT_TRUE(Contains(
        shader,
        "floatsafeLowLodDensity="));
    EXPECT_FALSE(Contains(shader, "lowLodDensityAndProfile"));
    EXPECT_FALSE(Contains(shader, "pow(saturate(shape),inScatterDepthExponent)"));
    EXPECT_FALSE(Contains(shader, "detailedLightDepth"));
    EXPECT_FALSE(Contains(shader, "detailedTauL"));
    EXPECT_FALSE(Contains(shader, "farTauL"));
    EXPECT_FALSE(Contains(shader, "secondDetailedOcclusion"));
    EXPECT_FALSE(Contains(shader, "lightTerminationOcclusion"));
    EXPECT_EQ(
        CountOccurrences(
            shader,
            "if(lightDepths.z>18.0)break;"),
        static_cast<std::size_t>(1));
    EXPECT_TRUE(Contains(
        shader,
        "floatfirstLightScattering=lightScatteringByOrder.x;"
        "floatsecondLightScattering=lightScatteringByOrder.y;"
        "floatthirdLightScattering=lightScatteringByOrder.z;"));
    EXPECT_TRUE(Contains(
        shader,
        "source.firstOrder=context.sunAtCloud*"
        "context.directionalScatteringScale*"
        "firstLightScattering;"
        "source.secondOrder=context.sunAtCloud*"
        "context.directionalScatteringScale*"
        "secondLightScattering;"
        "source.thirdOrder=context.sunAtCloud*"
        "context.directionalScatteringScale*"
        "thirdLightScattering;"));
    EXPECT_TRUE(Contains(shader, "floatskyAmbientZenithWeight=lerp("));
    EXPECT_TRUE(Contains(shader, "*skyAmbientVisibility*cloudLightingExtinction.z;"));
    EXPECT_TRUE(Contains(shader, "*bottomWeight*groundAmbientVisibility*cloudLightingExtinction.z;"));
    EXPECT_FALSE(Contains(shader, "topSurfaceScatter"));
    EXPECT_FALSE(Contains(shader, "singleScatter"));
    EXPECT_FALSE(Contains(shader, "multipleScatter"));
    EXPECT_FALSE(Contains(shader, "beer*(1.0-multiWeight)*phase"));
    EXPECT_FALSE(Contains(shader, "nearLightDensity"));
    EXPECT_FALSE(Contains(shader, "edgeBoost"));
    EXPECT_TRUE(Contains(
        shader,
        "floatfirstSampleWeight=0.25*firstLaneTransmit*"
        "firstIntervalAbsorption;"
        "floatsecondSampleWeight=0.25*secondLaneTransmit*"
        "cloudReducedIntervalScatteringWeight("
        "viewOpticalDepth,secondOpticalDepth,"
        "secondIntervalTransmittance,multiContribution);"
        "floatthirdSampleWeight=0.25*thirdLaneTransmit*"
        "cloudReducedIntervalScatteringWeight("
        "viewOpticalDepth,thirdOpticalDepth,"
        "thirdIntervalTransmittance,thirdContribution);"));
    EXPECT_TRUE(Contains(
        shader,
        "sample.secondOrderSource=lightingSource.secondOrder*"
        "lightingSource.higherOrderInScatterFactor;"
        "sample.thirdOrderSource=lightingSource.thirdOrder*"
        "lightingSource.higherOrderInScatterFactor;"));
    EXPECT_FALSE(Contains(
        shader,
        "secondLaneWeights*higherOrderInScatterFactors"));
    EXPECT_TRUE(Contains(
        shader,
        "scatter+=firstSampleWeight*firstOrderSource+"
        "secondSampleWeight*secondOrderSource+"
        "thirdSampleWeight*thirdOrderSource;"));
    EXPECT_TRUE(Contains(
        shader,
        "floatfirstRayCentroid=overlapStart+dot("
        "physicalLaneSelector,firstTransport.centroidDistances);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatfirstSampleT=laneComponentStartT+"
        "firstCentroidFraction*cellLaneLength;"
        "depthMoment+=firstSampleWeight*firstSampleT;"
        "accumulatedOpticalDepthLanes=min("
        "accumulatedOpticalDepthLanes+"
        "physicalLaneSelector*viewOpticalDepth,"
        "80.0.xxxx);"));
    EXPECT_TRUE(Contains(
        shader,
        "transmitLanes=exp(-accumulatedOpticalDepthLanes);"
        "secondOrderTransmitLanes=exp("
        "-secondOrderAccumulatedOpticalDepthLanes);"
        "thirdOrderTransmitLanes=exp("
        "-thirdOrderAccumulatedOpticalDepthLanes);"));
    EXPECT_FALSE(Contains(
        shader,
        "transmitLanes*=intervalTransmittanceLanes;"));
    EXPECT_TRUE(Contains(
        shader,
        "transmit=saturate(dot("
        "CLOUD_SUBRAY_AREA_WEIGHTS,transmitLanes));"
        "secondOrderTransmit=saturate(dot("
        "CLOUD_SUBRAY_AREA_WEIGHTS,secondOrderTransmitLanes));"
        "thirdOrderTransmit=saturate(dot("
        "CLOUD_SUBRAY_AREA_WEIGHTS,thirdOrderTransmitLanes));"));
    EXPECT_TRUE(Contains(
        shader,
        "floatremainingDistance=cloudPositiveDifferenceUpper("
        "traceEndUpper,intervalStart+cellEndOffset);"
        "floatmaximumOpticalDensityPerMeter=cloudPositiveProductUpper("));
    EXPECT_TRUE(Contains(
        shader,
        "uintremainingTransportSegmentCount="
        "cloudRemainingTransportSegmentCount("
        "currentFineCellCount,nextFineCellIndex,"
        "remainingPackedCellBudget>0,remainingPackedCellBudget);"));
    EXPECT_TRUE(Contains(
        shader,
        "uintsegmentCountPerCell="
        "uint(CLOUD_BEER_TRANSPORT_SEGMENT_COUNT)*"
        "uint(CLOUD_SHELL_COMPONENT_COUNT);"
        "uintmaximumCellCount=0xffffffffu/segmentCountPerCell;"
        "uintsegmentCount=0xffffffffu;"
        "uintremainingCellCapacity=0u;"
        "boolcurrentCountFits="
        "currentRemainingCellCount<=maximumCellCount;"
        "if(currentCountFits)remainingCellCapacity="
        "maximumCellCount-currentRemainingCellCount;"
        "boolnextCountFits=currentCountFits&&"
        "nextRemainingCellCount<=remainingCellCapacity;"
        "if(nextCountFits)segmentCount="
        "(currentRemainingCellCount+nextRemainingCellCount)*"
        "segmentCountPerCell;"
        "returnsegmentCount;"));
    EXPECT_TRUE(Contains(
        shader,
        "float3remainingCloudRadianceUpper="
        "cloudPositiveSumUpper3(cloudPositiveSumUpper3("));
    EXPECT_FALSE(Contains(
        shader,
        "if(cloudDensityLaneAverage(sampleMeanDensityLanes)<=0.0)"
        "continue;"));
    EXPECT_TRUE(Contains(
        shader,
        "state.active=max(state.active,activeMask);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatremainingFirstOpacityUpper=cloudTransportWeightUpper("
        "transmit,remainingPrimaryOpticalDepth,1.0,1.0,"
        "remainingTransportSegmentCount);"));
    EXPECT_TRUE(Contains(
        shader,
        "if(colorCodeStable&&opacityCodeStable&&opacityFloatCodeStable&&"
        "opacityDepthStable)break;"));
    EXPECT_TRUE(Contains(shader, "cloudR16ValueRangeKeepsCode("));
    EXPECT_TRUE(Contains(shader, "cloudR32PositiveRangeKeepsCode("));
    EXPECT_TRUE(Contains(
        shader,
        "cloudDepthOut[pixelQ]=float2(meanDepth,resolvedA);"));
    EXPECT_FALSE(Contains(shader, "opacityStorageHalfUlp"));
    EXPECT_FALSE(Contains(shader, "transmit<0.012"));

    // 太陽面の異なる光路はBeer-Lambert変換後に平均する。平均光学的厚さを
    // 先に指数変換する旧順序は、雲縁の開けた方向から届く光を失う。
    constexpr f32 kSunPathDepths[]{0.2f, 0.8f, 2.4f, 5.0f};
    f32 meanTransmittance = 0.0f;
    f32 meanOpticalDepth = 0.0f;
    for (const f32 pathDepth : kSunPathDepths) {
        meanTransmittance += std::exp(-pathDepth);
        meanOpticalDepth += pathDepth;
    }
    meanTransmittance *= 0.25f;
    meanOpticalDepth *= 0.25f;
    EXPECT_TRUE(meanTransmittance > std::exp(-meanOpticalDepth));

    // 縮小係数を適用した均質層は、刻み数に依存せず解析解へ一致する。
    constexpr f32 kContribution = 0.28f;
    constexpr f32 kOcclusion = 0.28f;
    constexpr f32 kOpticalDepth = 5.0f;
    constexpr f32 kThirdContribution = kContribution * kContribution;
    constexpr f32 kThirdOcclusion = kOcclusion * kOcclusion;
    const f32 expectedSecond = (kContribution / kOcclusion) *
        (1.0f - std::exp(-kOpticalDepth * kOcclusion));
    const f32 expectedThird = (kThirdContribution / kThirdOcclusion) *
        (1.0f - std::exp(-kOpticalDepth * kThirdOcclusion));
    for (const u32 intervalCount : {1u, 2u, 7u, 64u, 257u}) {
        EXPECT_NEAR(
            AccumulateReducedCloudOrderForTest(
                kOpticalDepth, intervalCount,
                kContribution, kOcclusion),
            expectedSecond, 2.0e-5f);
        EXPECT_NEAR(
            AccumulateReducedCloudOrderForTest(
                kOpticalDepth, intervalCount,
                kThirdContribution, kThirdOcclusion),
            expectedThird, 2.0e-5f);
    }

    // 旧式は一次の視線不透明度へ高次係数を後掛けするため、厚い層ほど奥からの光を失う。
    const f32 formerSecond = kContribution *
        (1.0f - std::exp(-kOpticalDepth));
    const f32 formerThird = kThirdContribution *
        (1.0f - std::exp(-kOpticalDepth));
    EXPECT_TRUE(expectedSecond > formerSecond * 2.5f);
    EXPECT_TRUE(expectedThird > formerThird * 4.0f);

    // 薄い区間では解析式が contribution*tau の極限へ収束し、旧式との一次精度を保つ。
    constexpr f32 kThinDepth = 0.001f;
    EXPECT_NEAR(
        CloudReducedIntervalScatteringWeightForTest(
            kThinDepth, kContribution, kOcclusion),
        kContribution * (1.0f - std::exp(-kThinDepth)), 2.0e-7f);
    EXPECT_NEAR(
        CloudReducedIntervalScatteringWeightForTest(
            2.0f, 0.0f, 0.0f),
        0.0f, 0.0f);
    EXPECT_TRUE(std::isfinite(
        CloudReducedIntervalScatteringWeightForTest(
            2.0f, 0.0f, 0.0f)));

    // 散乱縮小率が消散縮小率を越える入力も、区間内で増幅しない上限へ収める。
    EXPECT_NEAR(
        CloudReducedIntervalScatteringWeightForTest(
            5.0f, 0.8f, 0.4f),
        1.0f - std::exp(-2.0f), 1.0e-6f);
}
ACS_TEST(VolumetricClouds,
         SkyAmbientVisibilityUsesColumnDepthWithoutDoubleAttenuation) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());
    const std::size_t ambientBegin =
        shader.find(
            "floatambientExtinction=max("
            "cloudLightingExtinction.y,0.0);");
    const std::size_t ambientEnd = shader.find(
        "source.firstOrder+=ambientLight+groundLight;",
        ambientBegin);
    EXPECT_TRUE(ambientBegin != std::string::npos);
    EXPECT_TRUE(ambientEnd != std::string::npos);
    const std::string ambient =
        ambientBegin != std::string::npos &&
        ambientEnd != std::string::npos
        ? shader.substr(ambientBegin, ambientEnd - ambientBegin)
        : std::string{};

    EXPECT_FALSE(Contains(ambient, "cloudDensityLaneAverage("));
    EXPECT_FALSE(Contains(ambient, "distanceFade"));
    EXPECT_TRUE(Contains(
        ambient,
        "floatambientExtinction=max("
        "cloudLightingExtinction.y,0.0);"));
    EXPECT_TRUE(Contains(
        ambient,
        "float2fallbackAmbientDepth="
        "cloudAmbientFallbackOpticalDepth("
        "macro,lowLodDensity.xxxx,context.density,ambientExtinction);"
        "float4fallbackAmbientVisibility=cloudHemisphericVisibility("
        "float4(fallbackAmbientDepth,0.0,0.0));"));
    EXPECT_TRUE(Contains(
        ambient,
        "float3cachedAmbientVisibility="
        "sampleCloudAmbientVisibility(p);"));
    EXPECT_TRUE(Contains(
        ambient,
        "floatskyAmbientVisibility=lerp("
        "fallbackAmbientVisibility.x,"
        "cachedAmbientVisibility.y,"
        "cachedAmbientVisibility.x);"));
    EXPECT_TRUE(Contains(
        ambient,
        "floatgroundAmbientVisibility=lerp("
        "fallbackAmbientVisibility.y,"
        "cachedAmbientVisibility.z,"
        "cachedAmbientVisibility.x);"));
    EXPECT_FALSE(Contains(ambient, "multiOcclusion"));
    EXPECT_FALSE(Contains(ambient, "cachedAmbientVisibility.y*"));
    EXPECT_FALSE(Contains(ambient, "cachedAmbientVisibility.z*"));
    EXPECT_FALSE(Contains(ambient, "sampleCloudAmbientDepth"));
    EXPECT_FALSE(Contains(ambient, "reducedAmbientExtinction"));
    EXPECT_TRUE(Contains(
        ambient,
        "floatskyAmbientZenithWeight=lerp("
        "0.3333333,0.6666667,saturate(height));"));
    EXPECT_TRUE(Contains(
        ambient,
        "*skyAmbientVisibility*cloudLightingExtinction.z;"));
    EXPECT_TRUE(Contains(
        ambient,
        "*bottomWeight*groundAmbientVisibility"
        "*cloudLightingExtinction.z;"));

    // キャッシュ値は既に半球積分済みの可視率なので、局所密度や消散を再度掛けない。
    const auto resolvedVisibility = [](f32 fallbackDepth,
                                       f32 cachedVisibility,
                                       f32 cacheWeight,
                                       f32 extinction) noexcept {
        const f32 fallbackVisibility =
            render_internal::ResolveVolumetricCloudHemisphericVisibility_Internal(
                fallbackDepth * extinction);
        const f32 boundedWeight =
            cacheWeight < 0.0f ? 0.0f :
            (cacheWeight > 1.0f ? 1.0f : cacheWeight);
        return fallbackVisibility +
            (cachedVisibility - fallbackVisibility) * boundedWeight;
    };
    const f32 sparseFallback =
        resolvedVisibility(0.10f, 0.35f, 0.0f, 5.0f);
    const f32 denseFallback =
        resolvedVisibility(1.20f, 0.35f, 0.0f, 5.0f);
    EXPECT_TRUE(denseFallback < sparseFallback);
    EXPECT_NEAR(
        resolvedVisibility(0.10f, 0.35f, 1.0f, 5.0f),
        0.35f, 1e-7f);
    EXPECT_NEAR(
        resolvedVisibility(1.20f, 0.35f, 1.0f, 5.0f),
        0.35f, 1e-7f);
    EXPECT_TRUE(
        resolvedVisibility(0.10f, 0.35f, 0.5f, 5.0f) >
        resolvedVisibility(1.20f, 0.35f, 0.5f, 5.0f));
}

ACS_TEST(VolumetricClouds,
         CompositeTreatsOnlyExactClearDepthAsSky) {
    const std::string source = ReadSkySource();
    const std::string shader =
        ExtractRawShader(source, "const char* kCloudCompPS");
    const std::string atmosphereShader =
        ExtractRawShader(source, "const char* kCloudCompAtmosPS");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(!atmosphereShader.empty());

    // IRhiCommandList's conventional-depth render passes clear to exactly 1.
    // Every smaller representable value is finite scene geometry and must
    // participate in cloud-vs-scene ray-distance occlusion.
    EXPECT_TRUE(Contains(shader, "if (depth < 1.0) {"));
    EXPECT_TRUE(Contains(atmosphereShader, "if (depth < 1.0) {"));
    EXPECT_TRUE(!Contains(shader, "depth < 0.99999"));
    EXPECT_TRUE(!Contains(shader, "depth <= 0.99999"));
    EXPECT_TRUE(!Contains(atmosphereShader, "depth < 0.99999"));
    EXPECT_TRUE(!Contains(atmosphereShader, "depth <= 0.99999"));
}

ACS_TEST(VolumetricClouds,
         RuntimeInitializationSerializesLargeCloudEntryPoints) {
    const std::string source = CompactShader(ReadSkySource());
    EXPECT_FALSE(source.empty());

    const std::size_t begin = source.find(
        "CVolumetricClouds::BeginInitializationAsync(");
    const std::size_t advance = source.find(
        "CVolumetricClouds::AdvanceInitialization(", begin);
    EXPECT_TRUE(begin != std::string::npos);
    EXPECT_TRUE(advance != std::string::npos);
    if (begin == std::string::npos || advance == std::string::npos) return;

    const std::string begin_body = source.substr(begin, advance - begin);
    EXPECT_TRUE(Contains(
        begin_body, "CreateCloudShaderSet(device,true,false)"));
    EXPECT_FALSE(Contains(begin_body, "\"CSCloudShadow\""));
    EXPECT_FALSE(Contains(begin_body, "\"CSCloudWorldShadow\""));
    const std::size_t ownerAllocation = begin_body.find(
        "MakeUnique<FNoiseFilterResources>()");
    const std::size_t mandatorySubmission = begin_body.find(
        "CreateCloudShaderSet(device,true,false)");
    EXPECT_TRUE(ownerAllocation != std::string::npos);
    EXPECT_TRUE(mandatorySubmission != std::string::npos);
    EXPECT_TRUE(ownerAllocation < mandatorySubmission);

    const std::size_t mandatory_status = source.find(
        "PendingMandatoryShaderStatus_Internal();", advance);
    const std::size_t shadow_submit = source.find(
        "CreateCloudShaderHandle(device,EShaderStage::Compute,kCloudCS,"
        "\"CSCloudShadow\",\"Clouds.ShadowCacheCS\",true);",
        mandatory_status);
    const std::size_t shadow_status = source.find(
        "constEShaderStatusstatus=m_ShadowCs->Status();",
        shadow_submit);
    const std::size_t world_shadow_submit = source.find(
        "CreateCloudShaderHandle(device,EShaderStage::Compute,kCloudCS,"
        "\"CSCloudWorldShadow\",\"Clouds.WorldShadowCS\",true);",
        shadow_status);
    const std::size_t world_shadow_status = source.find(
        "constEShaderStatusstatus=m_WorldShadowCs->Status();",
        world_shadow_submit);
    const std::size_t publish = source.find(
        "FCompiledShaderscompleted=TakePendingShaders_Internal();",
        world_shadow_status);
    EXPECT_TRUE(mandatory_status != std::string::npos);
    EXPECT_TRUE(shadow_submit != std::string::npos);
    EXPECT_TRUE(shadow_status != std::string::npos);
    EXPECT_TRUE(world_shadow_submit != std::string::npos);
    EXPECT_TRUE(world_shadow_status != std::string::npos);
    EXPECT_TRUE(publish != std::string::npos);
    EXPECT_TRUE(mandatory_status < shadow_submit);
    EXPECT_TRUE(shadow_submit < shadow_status);
    EXPECT_TRUE(shadow_status < world_shadow_submit);
    EXPECT_TRUE(world_shadow_submit < world_shadow_status);
    EXPECT_TRUE(world_shadow_status < publish);

    const std::string advance_body = source.substr(
        advance, publish - advance);
    EXPECT_TRUE(Contains(
        advance_body,
        "m_NoiseFilterResources->resource_device!=&device"));
    EXPECT_TRUE(Contains(
        advance_body,
        "if(status==EShaderStatus::Compiling){"
        "returnTResult<bool>(OkInit,false);}"));

    const std::size_t compiledInit = source.find(
        "CVolumetricClouds::InitWithCompiledShaders(", publish);
    const std::size_t candidateInit = source.find(
        "CVolumetricClouds::InitCandidateWithCompiledShaders(", compiledInit);
    EXPECT_TRUE(compiledInit != std::string::npos);
    EXPECT_TRUE(candidateInit != std::string::npos);
    if (compiledInit != std::string::npos &&
        candidateInit != std::string::npos) {
        const std::string compiledInitBody = source.substr(
            compiledInit, candidateInit - compiledInit);
        EXPECT_TRUE(Contains(
            compiledInitBody,
            "if(InitializationPending()){returnACS_ERR("));
    }

    const std::size_t shutdown = source.find(
        "voidCVolumetricClouds::Shutdown()noexcept{");
    EXPECT_TRUE(shutdown != std::string::npos);
    if (shutdown != std::string::npos) {
        const std::string shutdownBody = source.substr(shutdown);
        const std::size_t firstRelease = shutdownBody.find(
            "m_ShapeTex.Reset()");
        EXPECT_TRUE(firstRelease != std::string::npos);
        EXPECT_FALSE(Contains(
            shutdownBody, "PendingOwnedShaderStatus_Internal"));
        EXPECT_FALSE(Contains(shutdownBody, "SleepMs("));
    }
}

ACS_TEST(VolumetricClouds,
         GpuPipelinesAndRg32DistanceTargetsInitializeWhenAvailable) {
    FDeviceConfig config{};
    auto deviceResult = CreateRhiDevice(config);
    if (deviceResult.IsErr()) return;

    CVolumetricClouds clouds;
    clouds.SetLayer(FVolumetricCloudLayer{1700.0f, 4600.0f, 0.041f});
    clouds.SetReferenceMode(true);
    FVolumetricCloudLighting lighting = clouds.Lighting();
    lighting.ViewExtinction = 1.7f;
    lighting.SkyZenithColor = FVec3{0.13f, 0.27f, 0.41f};
    clouds.SetLighting(lighting);
    clouds.SetWeather(FVolumetricCloudWeather{0.64f, 0.75f, 0.22f, 0.35f});
    clouds.SetRange(FVolumetricCloudRange{93000.0f, 0.31f, 0.6f, 224u});
    clouds.SetUpperLayer(
        FVolumetricCloudUpperLayer{7200.0f, 9400.0f, 0.37f, 0.21f});
    IRhiDevice& device = *deviceResult.Value();
    bool initialized = false;
    if (device.SupportsAsyncShaderCompilation()) {
        const auto begin = clouds.BeginInitializationAsync(
            device, EFormat::R16G16B16A16_Float);
        EXPECT_TRUE(begin.IsOk());
        if (begin.IsOk()) {
            EXPECT_TRUE(clouds.InitializationPending());
            const auto duplicateBegin = clouds.BeginInitializationAsync(
                device, EFormat::R16G16B16A16_Float);
            EXPECT_TRUE(duplicateBegin.IsErr());
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::minutes(5);
            while (!initialized &&
                   std::chrono::steady_clock::now() < deadline) {
                const auto advance = clouds.AdvanceInitialization(device);
                EXPECT_TRUE(advance.IsOk());
                if (advance.IsErr()) break;
                initialized = advance.Value();
                if (!initialized) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1));
                }
            }
        }
    } else {
        const auto init = clouds.Init(
            device, EFormat::R16G16B16A16_Float);
        EXPECT_TRUE(init.IsOk());
        initialized = init.IsOk();
    }
    EXPECT_TRUE(initialized);
    if (initialized) {
        EXPECT_FALSE(clouds.InitializationPending());
        const auto alreadyReady = clouds.BeginInitializationAsync(
            device, EFormat::R16G16B16A16_Float);
        EXPECT_TRUE(alreadyReady.IsErr());
        const auto idempotentAdvance = clouds.AdvanceInitialization(device);
        EXPECT_TRUE(idempotentAdvance.IsOk());
        if (idempotentAdvance.IsOk()) {
            EXPECT_TRUE(idempotentAdvance.Value());
        }
        // 通常C++で自然な「設定してから初期化」の順序でも、候補構築が
        // 公開設定を既定値へ戻さないことを実GPU初期化で固定する。
        EXPECT_NEAR(clouds.Layer().base_height, 1700.0f, 0.0f);
        EXPECT_NEAR(clouds.Layer().top_height, 4600.0f, 0.0f);
        EXPECT_NEAR(clouds.Layer().horizontal_noise_scale, 0.041f, 0.0f);
        EXPECT_TRUE(clouds.ReferenceMode());
        EXPECT_NEAR(clouds.Lighting().ViewExtinction, 1.7f, 0.0f);
        EXPECT_NEAR(clouds.Lighting().SkyZenithColor.y, 0.27f, 0.0f);
        EXPECT_NEAR(clouds.Weather().CloudType, 0.64f, 0.0f);
        EXPECT_NEAR(clouds.Weather().PrecipitationInfluence, 0.35f, 0.0f);
        EXPECT_NEAR(clouds.Range().MaxDistance, 93000.0f, 0.0f);
        EXPECT_EQ(clouds.Range().ViewSteps, 224u);
        EXPECT_NEAR(clouds.UpperLayer().base_height, 7200.0f, 0.0f);
        EXPECT_NEAR(clouds.UpperLayer().density_scale, 0.21f, 0.0f);
        EXPECT_TRUE(clouds.WorldShadowAvailable());
        EXPECT_TRUE(clouds.EnsureSize(
            device, 32u, 24u, 1.0f));
    }
    clouds.Shutdown();
}

ACS_TEST(VolumetricClouds,
         CompositeTerminatesAtmosphereAtResolvedCloudDistance) {
    const std::string source = ReadSkySource();
    const std::string shader =
        ExtractRawShader(source, "const char* kCloudCompAtmosPS");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!shader.empty());

    // 雲外では時間・空間再構成後の代表距離を使い、雲中では雲の積分と重なる前景大気区間を作らない。
    EXPECT_TRUE(Contains(
        shader, "float2 cloudHit = cloudDepth.SampleLevel("));
    EXPECT_FALSE(Contains(
        shader, "sqrt(saturate(cloudHit.x / maxDistance))"));
    EXPECT_TRUE(Contains(
        shader, "float CloudCompositeAtmosphereDistance(float cloudDistance){"));
    EXPECT_TRUE(Contains(
        shader, "return CloudCameraInsideCloudLayer()?0.0:max(cloudDistance,0.0);"));
    EXPECT_TRUE(Contains(
        shader, "float atmosphereDistance=CloudCompositeAtmosphereDistance(cloudHit.x);"));
    EXPECT_FALSE(Contains(
        shader, "float slice = sqrt(saturate(cloudHit.x / maxDistance));"));
    EXPECT_TRUE(Contains(
        shader, "float slice = sqrt(saturate(atmosphereDistance / maxDistance));"));
    EXPECT_TRUE(Contains(
        shader, "atmosphereVolume.SampleLevel("));
    EXPECT_TRUE(Contains(
        shader, "atmosphereTransmittance.SampleLevel("));

    // Camera-volume L and wavelength-dependent T transform straight cloud
    // radiance before alpha blending. This preserves the same foreground
    // medium for both the cloud and the transmitted background. The T volume
    // alpha is also a validity sentinel: an unwritten/unbound u1 must fail open
    // to identity instead of turning every cloud black.
    EXPECT_TRUE(Contains(
        shader,
        "transmittanceSample.a >= 0.5"));
    EXPECT_TRUE(Contains(
        shader,
        "all(transmittanceSample.rgb == transmittanceSample.rgb)"));
    EXPECT_TRUE(Contains(
        shader,
        "all(transmittanceSample.rgb <= 1.001)"));
    EXPECT_TRUE(Contains(
        shader,
        "? saturate(transmittanceSample.rgb) : float3(1.0,1.0,1.0);"));
    EXPECT_TRUE(Contains(
        shader,
        "all(inScatterSample.rgb == inScatterSample.rgb)"));
    EXPECT_TRUE(Contains(
        shader,
        "? max(inScatterSample.rgb, 0.0) : float3(0.0,0.0,0.0);"));
    EXPECT_TRUE(Contains(
        shader,
        "cloud.rgb = inScatter + transmittance * cloud.rgb;"));
    EXPECT_TRUE(!Contains(shader, "float opacity ="));
    EXPECT_TRUE(Contains(source, "m_CompAtmosPipe"));
    EXPECT_TRUE(Contains(source, "cl.SetTexture(3, *atmosphere_volume)"));
    EXPECT_TRUE(Contains(
        source,
        "cl.SetTexture(4, *atmosphere_transmittance)"));

    // The atmosphere-aware cloud PSO must expose the complete five-texture,
    // five-sampler binding contract on both RHIs.
    EXPECT_TRUE(Contains(
        shader,
        "Texture3D<float4> atmosphereTransmittance : register(t4);"));
    EXPECT_TRUE(Contains(
        shader,
        "SamplerState atmosphereTransmittance_sampler : register(s4);"));
    EXPECT_TRUE(Contains(source, "pd.texture_slots = 5;"));
    EXPECT_TRUE(Contains(source, "pd.static_sampler_count = 5;"));
    EXPECT_TRUE(Contains(
        source,
        "pd.texture_names[4] = \"atmosphereTransmittance\";"));
    EXPECT_TRUE(Contains(
        source,
        "atmosphere_transmittance != nullptr &&"));

    // Verify the straight-radiance transform algebra per RGB channel:
    //   a*(L+T*C) + (1-a)*(L+T*B) == L+T*(a*C+(1-a)*B).
    const FVec3 background{0.30f, 0.50f, 0.80f};
    const FVec3 cloud{2.00f, 1.20f, 0.60f};
    const FVec3 inScatter{0.10f, 0.20f, 0.35f};
    const FVec3 transmittance{0.25f, 0.50f, 0.75f};
    constexpr f32 alpha = 0.40f;
    const FVec3 transformedCloud{
        inScatter.x + transmittance.x * cloud.x,
        inScatter.y + transmittance.y * cloud.y,
        inScatter.z + transmittance.z * cloud.z};
    const FVec3 transformedBackground{
        inScatter.x + transmittance.x * background.x,
        inScatter.y + transmittance.y * background.y,
        inScatter.z + transmittance.z * background.z};
    const FVec3 blended{
        alpha * transformedCloud.x +
            (1.0f - alpha) * transformedBackground.x,
        alpha * transformedCloud.y +
            (1.0f - alpha) * transformedBackground.y,
        alpha * transformedCloud.z +
            (1.0f - alpha) * transformedBackground.z};
    const FVec3 expected{
        inScatter.x + transmittance.x *
            (alpha * cloud.x + (1.0f - alpha) * background.x),
        inScatter.y + transmittance.y *
            (alpha * cloud.y + (1.0f - alpha) * background.y),
        inScatter.z + transmittance.z *
            (alpha * cloud.z + (1.0f - alpha) * background.z)};
    ExpectVec3Near(blended, expected, 1e-6f);
}

ACS_TEST(VolumetricClouds,
         ResizeWaitsOnlyAfterTransactionalReplacementSucceeds) {
    const std::string source = ReadSkySource();
    EXPECT_TRUE(!source.empty());

    const std::size_t ensure = source.find(
        "bool CVolumetricClouds::EnsureSize");
    const std::size_t sameSize = source.find(
        "m_W == hw && m_H == hh && m_FullW == fw && m_FullH == fh) return true;",
        ensure);
    const std::size_t allocationFailure = source.find(
        "return false; // 旧サイズを保持", ensure);
    const std::size_t replacementCheck = source.find(
        "const bool replacingExistingTextures =", ensure);
    const std::size_t waitIdle = source.find(
        "if (replacingExistingTextures) device.WaitIdle();", ensure);
    const std::size_t swap = source.find(
        "m_CloudTex = Move(lowColor);", ensure);

    EXPECT_TRUE(ensure != std::string::npos);
    EXPECT_TRUE(sameSize != std::string::npos);
    EXPECT_TRUE(allocationFailure != std::string::npos);
    EXPECT_TRUE(replacementCheck != std::string::npos);
    EXPECT_TRUE(waitIdle != std::string::npos);
    EXPECT_TRUE(swap != std::string::npos);
    EXPECT_TRUE(sameSize < allocationFailure);
    EXPECT_TRUE(allocationFailure < replacementCheck);
    EXPECT_TRUE(replacementCheck < waitIdle);
    EXPECT_TRUE(waitIdle < swap);
}

ACS_TEST(VolumetricClouds,
         ShadowCacheMaterialMappingIsWindInvariantAndLatticeSnapped) {
    EXPECT_EQ(kVolumetricCloudShadowCacheWidth, 96u);
    EXPECT_EQ(kVolumetricCloudShadowCacheHeight, 32u);
    EXPECT_EQ(kVolumetricCloudShadowCacheDepth, 96u);
    EXPECT_NEAR(kVolumetricCloudShadowCacheExtent, 48000.0f, 1e-6f);
    EXPECT_NEAR(kVolumetricCloudShadowCacheCellSize, 500.0f, 1e-6f);
    EXPECT_EQ(kVolumetricCloudAmbientCacheQuadratureAxis, 4u);
    EXPECT_EQ(kVolumetricCloudAmbientCacheQuadratureSamples, 16u);
    // 周囲光1領域と太陽透過率3領域を、合計128高度のRGBA16Fへ保持する。
    constexpr u32 cacheTextureHeight =
        kVolumetricCloudShadowCacheHeight * 4u;
    EXPECT_EQ(
        kVolumetricCloudShadowCacheWidth * cacheTextureHeight *
            kVolumetricCloudShadowCacheDepth * 8u,
        9437184u);

    // 生成時の96画素中心を48 km格子へ写し、読取UVから同じ画素へ戻ることを
    // 全ての水平画素で検証する。風は物質座標化で相殺される。
    constexpr f32 gridMinimum = -17321.0f;
    constexpr f32 windOffset = 731.0f;
    constexpr f32 inverseExtent =
        1.0f / kVolumetricCloudShadowCacheExtent;
    for (u32 cellIndex = 0u;
         cellIndex < kVolumetricCloudShadowCacheWidth;
         ++cellIndex) {
        const f32 textureUv =
            (static_cast<f32>(cellIndex) + 0.5f) /
            static_cast<f32>(kVolumetricCloudShadowCacheWidth);
        const f32 generatedWorld =
            gridMinimum + textureUv / inverseExtent + windOffset;
        const f32 readTextureUv =
            ((generatedWorld - windOffset) - gridMinimum) * inverseExtent;
        const f32 reconstructedCell =
            readTextureUv *
                static_cast<f32>(kVolumetricCloudShadowCacheWidth) -
            0.5f;
        EXPECT_NEAR(reconstructedCell, static_cast<f32>(cellIndex), 1.0e-4f);
    }

    const std::string shadowSource = CompactShader(
        ExtractRawShader(ReadSkySource(), "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shadowSource,
        "float2ambientTextureUv=(float2(outputColumn)+0.5)"
        "/float2(width,depth);"));
    EXPECT_TRUE(Contains(
        shadowSource,
        "float2sunColumnWorldXz=shadowGrid.xy+"
        "ambientTextureUv/max(shadowGrid.zw,1e-8.xx)+"
        "cloudWindWorld();"));
    EXPECT_FALSE(Contains(
        shadowSource,
        "(float(outputColumn.x)+0.5)/max(shadowGrid.z,1e-8)"));

    const FVec3 worldPoint{12345.0f, 2600.0f, -6789.0f};
    constexpr f32 windA = 91.0f;
    constexpr f32 windB = 413.0f;
    const FVec2 offsetA = VolumetricCloudWindOffsetXZ(windA);
    const FVec2 offsetB = VolumetricCloudWindOffsetXZ(windB);
    const FVec2 materialA =
        VolumetricCloudMaterialXZ(worldPoint, windA);
    const FVec3 advectedPoint{
        worldPoint.x + offsetB.x - offsetA.x,
        worldPoint.y,
        worldPoint.z + offsetB.y - offsetA.y};
    const FVec2 materialB =
        VolumetricCloudMaterialXZ(advectedPoint, windB);
    EXPECT_NEAR(materialA.x, materialB.x, 1e-3f);
    EXPECT_NEAR(materialA.y, materialB.y, 1e-3f);

    const auto mapping = CenterVolumetricCloudShadowCache(
        FVec2{12345.0f, 6789.0f});
    EXPECT_NEAR(
        mapping.center_material_xz.x /
            kVolumetricCloudShadowCacheCellSize,
        std::floor(mapping.center_material_xz.x /
                   kVolumetricCloudShadowCacheCellSize),
        1e-6f);
    EXPECT_NEAR(
        mapping.center_material_xz.y /
            kVolumetricCloudShadowCacheCellSize,
        std::floor(mapping.center_material_xz.y /
                   kVolumetricCloudShadowCacheCellSize),
        1e-6f);
    EXPECT_NEAR(
        mapping.center_material_xz.x - mapping.min_material_xz.x,
        kVolumetricCloudShadowCacheExtent * 0.5f,
        1e-6f);
    EXPECT_NEAR(
        mapping.center_material_xz.y - mapping.min_material_xz.y,
        kVolumetricCloudShadowCacheExtent * 0.5f,
        1e-6f);

    const f32 halfDiagonal =
        kVolumetricCloudShadowCacheExtent * 0.5f * std::sqrt(2.0f);
    const f32 maximumCenterAnchorOffset =
        kVolumetricCloudShadowCacheSafeRadius * std::sqrt(2.0f);
    EXPECT_NEAR(
        halfDiagonal + maximumCenterAnchorOffset,
        (kVolumetricCloudShadowCacheExtent * 0.5f +
         kVolumetricCloudShadowCacheSafeRadius) * std::sqrt(2.0f),
        1e-3f);
}

ACS_TEST(VolumetricClouds, AmbientShadowGuardMappingPreservesCenterAndCoversViewRange) {
    constexpr f32 viewDistances[] = {kVolumetricCloudMinDistance, 60000.0f, 100000.0f, kVolumetricCloudMaxDistance};
    constexpr f32 fullWeightTextureAxis = 1.0f - kVolumetricCloudShadowCacheFilterFullCells / static_cast<f32>(kVolumetricCloudShadowCacheWidth);
    constexpr f32 halfTexel = 0.5f / static_cast<f32>(kVolumetricCloudShadowCacheWidth);

    // 実装が使う純粋計算を最小距離から上限まで直接検査し、試験側に別の逆式を持たない。
    for (const f32 viewDistance : viewDistances) {
        const render_internal::FVolumetricCloudAmbientCacheMapTerms terms = render_internal::ResolveVolumetricCloudAmbientCacheMapTerms_Internal(viewDistance);
        const f32 expectedGuardedDistance = viewDistance + kVolumetricCloudShadowCacheSafeRadius > terms.half_extent ? viewDistance + kVolumetricCloudShadowCacheSafeRadius : terms.half_extent;
        EXPECT_NEAR(terms.uniform_radius, kVolumetricCloudShadowCacheSafeRadius, 1e-6f);
        EXPECT_NEAR(terms.guarded_distance, expectedGuardedDistance, 1e-3f);
        EXPECT_TRUE(terms.guard_coefficient >= 0.0f);

        f32 previousOffset = render_internal::VolumetricCloudAmbientCacheMaterialOffset_Internal(terms, halfTexel);
        for (u32 cellIndex = 0u; cellIndex < kVolumetricCloudShadowCacheWidth; ++cellIndex) {
            const f32 textureAxis = (static_cast<f32>(cellIndex) + 0.5f) / static_cast<f32>(kVolumetricCloudShadowCacheWidth);
            const f32 worldOffset = render_internal::VolumetricCloudAmbientCacheMaterialOffset_Internal(terms, textureAxis);
            EXPECT_NEAR(render_internal::VolumetricCloudAmbientCacheTextureAxis_Internal(terms, worldOffset), textureAxis, 2e-5f);
            if (cellIndex > 0u) EXPECT_TRUE(worldOffset > previousOffset);
            previousOffset = worldOffset;

            const f32 lowerBoundary = render_internal::VolumetricCloudAmbientCacheMaterialOffset_Internal(terms, textureAxis - halfTexel);
            const f32 upperBoundary = render_internal::VolumetricCloudAmbientCacheMaterialOffset_Internal(terms, textureAxis + halfTexel);
            f32 summedWidth = 0.0f;
            f32 previousSample = lowerBoundary;
            for (u32 sampleIndex = 0u;
                 sampleIndex < kVolumetricCloudAmbientCacheQuadratureAxis;
                 ++sampleIndex) {
                f32 sampleOffset = 0.0f;
                f32 sampleWidth = 0.0f;
                render_internal::ResolveVolumetricCloudAmbientCacheAxisSample_Internal(
                    terms, textureAxis, sampleIndex,
                    sampleOffset, sampleWidth);
                EXPECT_TRUE(sampleWidth > 0.0f);
                EXPECT_TRUE(sampleOffset > lowerBoundary);
                EXPECT_TRUE(sampleOffset < upperBoundary);
                EXPECT_TRUE(sampleOffset > previousSample);
                previousSample = sampleOffset;
                summedWidth += sampleWidth;
            }
            EXPECT_NEAR(summedWidth, upperBoundary - lowerBoundary, 3e-2f);
        }

        const f32 positiveGuard = render_internal::VolumetricCloudAmbientCacheMaterialOffset_Internal(terms, fullWeightTextureAxis);
        const f32 negativeGuard = render_internal::VolumetricCloudAmbientCacheMaterialOffset_Internal(terms, 1.0f - fullWeightTextureAxis);
        EXPECT_NEAR(positiveGuard, terms.guarded_distance, 2e-1f);
        EXPECT_NEAR(negativeGuard, -terms.guarded_distance, 2e-1f);
        EXPECT_NEAR(render_internal::VolumetricCloudAmbientCacheTextureAxis_Internal(terms, terms.guarded_distance), fullWeightTextureAxis, 2e-6f);
        EXPECT_NEAR(render_internal::VolumetricCloudAmbientCacheTextureAxis_Internal(terms, -terms.guarded_distance), 1.0f - fullWeightTextureAxis, 2e-6f);
        EXPECT_TRUE(render_internal::VolumetricCloudAmbientCacheMaterialOffset_Internal(terms, 1.0f) > terms.guarded_distance);
        EXPECT_TRUE(render_internal::VolumetricCloudAmbientCacheMaterialOffset_Internal(terms, 0.0f) < -terms.guarded_distance);

        const f32 centralBoundary = 0.5f + 0.5f * terms.central_fraction;
        EXPECT_NEAR(render_internal::VolumetricCloudAmbientCacheCellWidth_Internal(terms, centralBoundary), kVolumetricCloudShadowCacheCellSize, 1e-3f);
        EXPECT_NEAR(render_internal::VolumetricCloudAmbientCacheCellWidth_Internal(terms, centralBoundary + 1e-6f), kVolumetricCloudShadowCacheCellSize, 6e-2f);
    }

    const render_internal::FVolumetricCloudAmbientCacheMapTerms defaultTerms = render_internal::ResolveVolumetricCloudAmbientCacheMapTerms_Internal(60000.0f);
    for (i32 cellOffset = -16; cellOffset <= 16; ++cellOffset) {
        const f32 worldOffset = static_cast<f32>(cellOffset) * kVolumetricCloudShadowCacheCellSize;
        const f32 expectedTextureAxis = 0.5f + 0.5f * worldOffset / defaultTerms.half_extent;
        EXPECT_NEAR(render_internal::VolumetricCloudAmbientCacheMaterialOffset_Internal(defaultTerms, expectedTextureAxis), worldOffset, 2e-3f);
        EXPECT_NEAR(render_internal::VolumetricCloudAmbientCacheTextureAxis_Internal(defaultTerms, worldOffset), expectedTextureAxis, 2e-6f);
    }
}

ACS_TEST(VolumetricClouds, AmbientVisibilityIntegratesTheHemisphereBeforeAreaAveraging) {
    const auto referenceVisibility = [](f64 opticalDepth) noexcept {
        if (opticalDepth <= 0.0) return 1.0;
        constexpr u32 integrationSamples = 65536u;
        f64 integral = 0.0;
        for (u32 sampleIndex = 0u; sampleIndex < integrationSamples; ++sampleIndex) {
            const f64 directionCosine =
                (static_cast<f64>(sampleIndex) + 0.5) /
                static_cast<f64>(integrationSamples);
            integral += 2.0 * directionCosine *
                std::exp(-opticalDepth / directionCosine);
        }
        return integral / static_cast<f64>(integrationSamples);
    };

    constexpr f32 opticalDepths[] = {
        0.0f, 0.01f, 0.05f, 0.10f, 0.25f,
        0.50f, 1.0f, 2.0f, 5.0f, 10.0f,
    };
    f32 previousVisibility = 1.0f;
    for (const f32 opticalDepth : opticalDepths) {
        const f32 visibility =
            render_internal::ResolveVolumetricCloudHemisphericVisibility_Internal(
                opticalDepth);
        EXPECT_TRUE(visibility >= 0.0f);
        EXPECT_TRUE(visibility <= 1.0f);
        EXPECT_TRUE(visibility <= previousVisibility);
        EXPECT_NEAR(
            visibility,
            static_cast<f32>(referenceVisibility(opticalDepth)),
            1.2e-3f);
        previousVisibility = visibility;
    }

    // 疎密が混じる面積では各列を指数変換してから平均する。平均深さを先に
    // 指数変換すると、晴れた半分まで厚い雲として消してしまう。
    const f32 clearVisibility =
        render_internal::ResolveVolumetricCloudHemisphericVisibility_Internal(0.0f);
    const f32 denseVisibility =
        render_internal::ResolveVolumetricCloudHemisphericVisibility_Internal(10.0f);
    const f32 visibilityBeforeAreaAverage =
        0.5f * (clearVisibility + denseVisibility);
    const f32 areaAverageBeforeVisibility =
        render_internal::ResolveVolumetricCloudHemisphericVisibility_Internal(5.0f);
    EXPECT_TRUE(visibilityBeforeAreaAverage > 0.49f);
    EXPECT_TRUE(areaAverageBeforeVisibility < 0.01f);
    EXPECT_TRUE(
        visibilityBeforeAreaAverage > areaAverageBeforeVisibility + 0.49f);
}
ACS_TEST(VolumetricClouds, AmbientVisibilityAndShapeFrequencyBlendRemainBounded) {
    constexpr f32 inverseQuantizationScale = 1.0f / 65535.0f;
    for (u32 sampleIndex = 0u; sampleIndex <= 1024u; ++sampleIndex) {
        const f32 visibility =
            static_cast<f32>(sampleIndex) / 1024.0f;
        const u32 quantized =
            render_internal::QuantizeVolumetricCloudAmbientVisibility_Internal(
                visibility);
        const f32 restored =
            static_cast<f32>(quantized) * inverseQuantizationScale;
        EXPECT_NEAR(
            restored, visibility,
            0.5f * inverseQuantizationScale + 1e-7f);
    }
    EXPECT_EQ(
        render_internal::QuantizeVolumetricCloudAmbientVisibility_Internal(-1.0f),
        0u);
    EXPECT_EQ(
        render_internal::QuantizeVolumetricCloudAmbientVisibility_Internal(2.0f),
        65535u);
    constexpr u64 maximumGroupVisibilitySum =
        65535ull * kVolumetricCloudAmbientCacheQuadratureSamples;
    EXPECT_TRUE(
        maximumGroupVisibilitySum <
        static_cast<u64>(std::numeric_limits<u32>::max()));

    constexpr f32 shapeScales[] = {
        0.00004f, 0.00012f, 0.00020f,
    };
    constexpr f32 viewDistances[] = {
        kVolumetricCloudMinDistance,
        60000.0f,
        kVolumetricCloudMaxDistance,
    };
    constexpr f32 halfTexel =
        0.5f / static_cast<f32>(kVolumetricCloudShadowCacheWidth);
    for (const f32 viewDistance : viewDistances) {
        const render_internal::FVolumetricCloudAmbientCacheMapTerms terms =
            render_internal::ResolveVolumetricCloudAmbientCacheMapTerms_Internal(
                viewDistance);
        for (u32 cellIndex = 0u;
             cellIndex < kVolumetricCloudShadowCacheWidth;
             ++cellIndex) {
            const f32 textureAxis =
                (static_cast<f32>(cellIndex) + 0.5f) /
                static_cast<f32>(kVolumetricCloudShadowCacheWidth);
            const f32 lowerBoundary =
                render_internal::VolumetricCloudAmbientCacheMaterialOffset_Internal(
                    terms, textureAxis - halfTexel);
            const f32 upperBoundary =
                render_internal::VolumetricCloudAmbientCacheMaterialOffset_Internal(
                    terms, textureAxis + halfTexel);
            const f32 sampleWidth =
                (upperBoundary - lowerBoundary) /
                static_cast<f32>(kVolumetricCloudAmbientCacheQuadratureAxis);
            for (const f32 shapeScale : shapeScales) {
                const f32 maximumDomainFootprint =
                    render_internal::ResolveVolumetricCloudShapeMaximumDomainFootprint_Internal(
                        sampleWidth, 300.0f, sampleWidth,
                        shapeScale, 1.0f / 9400.0f, false);
                const auto blend =
                    render_internal::ResolveVolumetricCloudShapeFrequencyBlend_Internal(
                        maximumDomainFootprint);
                EXPECT_NEAR(
                    blend.unresolved + blend.coarse +
                        blend.middle + blend.complete,
                    1.0f, 1.0e-6f);
            }
        }
    }

    const render_internal::FVolumetricCloudAmbientCacheMapTerms maximumTerms =
        render_internal::ResolveVolumetricCloudAmbientCacheMapTerms_Internal(
            kVolumetricCloudMaxDistance);
    f32 outerSampleOffset = 0.0f;
    f32 outerSampleWidth = 0.0f;
    render_internal::ResolveVolumetricCloudAmbientCacheAxisSample_Internal(
        maximumTerms,
        1.0f - halfTexel,
        kVolumetricCloudAmbientCacheQuadratureAxis - 1u,
        outerSampleOffset,
        outerSampleWidth);
    const f32 outerDomainFootprint =
        render_internal::ResolveVolumetricCloudShapeMaximumDomainFootprint_Internal(
            outerSampleWidth, 300.0f, outerSampleWidth,
            0.00020f, 1.0f / 9400.0f, false);
    EXPECT_TRUE(outerSampleOffset > 0.0f);
    EXPECT_TRUE(outerSampleWidth > 4000.0f);
    const auto outerBlend =
        render_internal::ResolveVolumetricCloudShapeFrequencyBlend_Internal(
            outerDomainFootprint);
    EXPECT_NEAR(outerBlend.unresolved, 1.0f, 0.0f);
    EXPECT_NEAR(outerBlend.coarse, 0.0f, 0.0f);
    EXPECT_NEAR(outerBlend.middle, 0.0f, 0.0f);
    EXPECT_NEAR(outerBlend.complete, 0.0f, 0.0f);
    /** 下層の300 m高度幅を、物理距離と高度せん断から求めた担当幅。 */
    const f32 lowerVerticalFootprint =
        render_internal::ResolveVolumetricCloudShapeMaximumDomainFootprint_Internal(
            0.0f, 300.0f, 0.0f,
            0.00020f, 1.0f / 9400.0f, false);
    /** 同じ高度幅を上層の弱いせん断で評価した担当幅。 */
    const f32 upperVerticalFootprint =
        render_internal::ResolveVolumetricCloudShapeMaximumDomainFootprint_Internal(
            0.0f, 300.0f, 0.0f,
            0.00020f, 1.0f / 2000.0f, true);
    EXPECT_TRUE(lowerVerticalFootprint > 0.0f);
    EXPECT_TRUE(upperVerticalFootprint > 0.0f);
    // 局所雲頂の正規化幅ではなく、有限な物理層厚だけが結果を決める。
    EXPECT_TRUE(lowerVerticalFootprint < 0.08f);
    EXPECT_TRUE(upperVerticalFootprint < 0.08f);
}

ACS_TEST(VolumetricClouds,
         CompletedDensityPrecedesQuadratureAndBeerVisibilityAverage) {
    // 符号付き形状を密度へ変換する正値化は非線形なので、入力を平均してから
    // 正値化すると晴天列と濃い雲列の面積割合を失う。
    const f32 clearPotential = CloudHierarchicalShapeForTest(
        0.2f, 0.2f, 0.2f, 0.2f, 0.8f);
    const f32 densePotential = CloudHierarchicalShapeForTest(
        0.9f, 0.8f, 0.7f, 0.8f, 0.2f);
    const f32 averagedInputsPotential = CloudHierarchicalShapeForTest(
        0.5f * (0.2f + 0.9f),
        0.5f * (0.2f + 0.8f),
        0.5f * (0.2f + 0.7f),
        0.5f * (0.2f + 0.8f),
        0.5f * (0.8f + 0.2f));
    const f32 clearDensity =
        render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
            clearPotential);
    const f32 denseDensity =
        render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
            densePotential);
    const f32 completedDensityAverage =
        0.5f * (clearDensity + denseDensity);
    const f32 averagedInputsDensity =
        render_internal::ResolveVolumetricCloudCondensationDensity_Internal(
            averagedInputsPotential);
    EXPECT_NEAR(clearPotential, -0.84f, 1.0e-6f);
    EXPECT_NEAR(densePotential, 0.91f, 1.0e-6f);
    EXPECT_NEAR(clearDensity, 0.0f, 0.0f);
    EXPECT_NEAR(completedDensityAverage, 0.455f, 1.0e-6f);
    EXPECT_NEAR(averagedInputsPotential, 0.0775f, 1.0e-6f);
    EXPECT_NEAR(averagedInputsDensity, 0.0775f, 1.0e-6f);
    EXPECT_TRUE(completedDensityAverage > averagedInputsDensity + 0.3f);

    // 最粗形状を解像できる範囲では、同じ物質点の低周波湿度核を所有者として残す。
    constexpr f32 coarseShape = 0.72f;
    constexpr f32 middleShape = 0.82f;
    constexpr f32 completeShape = 0.91f;
    f32 previousShape = completeShape;
    for (u32 step = 0u; step <= 128u; ++step) {
        const f32 footprint = static_cast<f32>(step) / 1024.0f;
        const f32 selectedShape = CloudBandLimitedShapeForTest(
            coarseShape, middleShape, completeShape, footprint);
        EXPECT_TRUE(selectedShape >= coarseShape - 1.0e-6f);
        EXPECT_TRUE(selectedShape <= completeShape + 1.0e-6f);
        EXPECT_TRUE(selectedShape <= previousShape + 1.0e-6f);
        previousShape = selectedShape;
    }
    EXPECT_NEAR(previousShape, coarseShape, 1.0e-6f);

    // 最粗周期も一つのサブレイ面積へ収まった後は、密度順位を光路へ固定せず
    // Gauss-Hermite求積した局所平均へ完全移行する。
    const auto terminalBlend =
        render_internal::ResolveVolumetricCloudShapeFrequencyBlend_Internal(
            0.25f);
    EXPECT_NEAR(terminalBlend.unresolved, 1.0f, 0.0f);
    EXPECT_NEAR(terminalBlend.coarse, 0.0f, 0.0f);
    EXPECT_NEAR(terminalBlend.middle, 0.0f, 0.0f);
    EXPECT_NEAR(terminalBlend.complete, 0.0f, 0.0f);
    const auto terminalLanes = render_internal::
        ResolveVolumetricCloudBandDensityLanes_Internal(
            -1.0f, -1.0f, -1.0f,
            terminalBlend, 0.0f, 0.0f);
    const f32 terminalMean = render_internal::
        ResolveVolumetricCloudUnresolvedCoarseMeanDensity_Internal(
            0.0f, 0.0f);
    EXPECT_TRUE(terminalMean > 0.08f);
    for (u32 lane = 0u; lane < 4u; ++lane)
        EXPECT_NEAR(terminalLanes.densities[lane], terminalMean, 1.0e-7f);

    // 半分が晴天、半分が厚い雲のセルを、各列でBeer-Lambert変換してから平均する。
    // 平均深さを先に指数変換する均質霧近似とは一致してはならない。
    constexpr u32 columnCount = 16u;
    constexpr f32 denseOpticalDepth = 8.0f;
    f32 visibilitySum = 0.0f;
    f32 opticalDepthSum = 0.0f;
    for (u32 columnIndex = 0u; columnIndex < columnCount; ++columnIndex) {
        const f32 opticalDepth = columnIndex < columnCount / 2u
            ? 0.0f : denseOpticalDepth;
        visibilitySum += std::exp(-opticalDepth);
        opticalDepthSum += opticalDepth;
    }
    const f32 visibilityBeforeAreaAverage =
        visibilitySum / static_cast<f32>(columnCount);
    const f32 areaAverageBeforeVisibility = std::exp(
        -opticalDepthSum / static_cast<f32>(columnCount));
    EXPECT_TRUE(visibilityBeforeAreaAverage > 0.50f);
    EXPECT_TRUE(areaAverageBeforeVisibility < 0.02f);
    EXPECT_TRUE(
        visibilityBeforeAreaAverage > areaAverageBeforeVisibility + 0.48f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader,
        "floatsegmentDepth=columnDensity*lowerCellWorldStep*"
        "cloudOpticalDepthScaleFromBand(false)*ambientExtinction;"));
    EXPECT_TRUE(Contains(
        shader,
        "cloudAmbientQuantizedVisibilitySums[profileIndex]="
        "cloudQuantizeAmbientVisibility("
        "cloudHemisphericVisibility(pathDepth));"));
    EXPECT_FALSE(Contains(
        shader, "cloudHemisphericVisibility(pathDepth*ambientExtinction)"));
    EXPECT_TRUE(Contains(
        shader,
        "resolvedVisibilitySum+="
        "cloudAmbientQuantizedVisibilitySums[profileIndex];"));
    const std::size_t shadowBegin = shader.find("voidCSCloudShadow(");
    const std::size_t shadowEnd = shader.find(
        "uintCloudTemporalBlockPhase4(", shadowBegin);
    EXPECT_TRUE(shadowBegin != std::string::npos);
    EXPECT_TRUE(shadowEnd != std::string::npos);
    if (shadowBegin != std::string::npos &&
        shadowEnd != std::string::npos) {
        EXPECT_FALSE(Contains(
            shader.substr(shadowBegin, shadowEnd - shadowBegin),
            "floatmeanDepth="));
    }
    EXPECT_TRUE(Contains(
        shader,
        "float4densityBands=shapeNoise.SampleLevel("
        "shapeNoise_sampler,uvw,0);"));
    EXPECT_TRUE(Contains(
        shader,
        "float4occupancyBands=shapeOccupancy.SampleLevel("
        "shapeOccupancy_sampler,uvw,0);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudBandLimitedCondensationDensity("));
    EXPECT_TRUE(Contains(
        shader,
        "cloudCondensationDensity(shapePotentials.x+sharedOffset)"));
    EXPECT_FALSE(Contains(shader, "cloudFilteredStoredBaseNoise("));
    EXPECT_FALSE(Contains(shader, "floatperlin2=components.r;"));
    EXPECT_FALSE(Contains(shader, "unresolvedPerlinMean"));
    EXPECT_FALSE(Contains(shader, "unresolvedWorleyMean"));
}

ACS_TEST(VolumetricClouds, WorldShadowProjectionPreservesSunRaysAndSnapsToTexels) {
    EXPECT_TRUE(kVolumetricCloudWorldShadowEnabled);
    EXPECT_EQ(kVolumetricCloudWorldShadowMapResolution, 256u);
    EXPECT_NEAR(kVolumetricCloudWorldShadowMapExtent, 32768.0f, 1e-6f);
    EXPECT_NEAR(kVolumetricCloudWorldShadowMapTexelSize, 128.0f, 1e-6f);
    EXPECT_EQ(kVolumetricCloudWorldShadowSamples, 32u);
    EXPECT_NEAR(kVolumetricCloudWorldShadowMinimumSunY, 0.03f, 1e-6f);

    const FVec3 sun = NormalizeForTest(FVec3{0.42f, 0.75f, -0.33f});
    const FVec3 receiver{1452.0f, 310.0f, -702.0f};
    constexpr f32 referenceHeight = 25.0f;
    const FVec2 projected = ProjectVolumetricCloudWorldShadowReferenceXZ(receiver, sun, referenceHeight);
    const FVec2 projectedAlongSameRay = ProjectVolumetricCloudWorldShadowReferenceXZ(PointOnRay(receiver, sun, 8192.0f), sun, referenceHeight);
    // 同じ太陽光線上の受光点は、標高が違っても同じ地図画素を参照する。
    EXPECT_NEAR(projected.x, projectedAlongSameRay.x, 2e-3f);
    EXPECT_NEAR(projected.y, projectedAlongSameRay.y, 2e-3f);

    const FVec2 lowSun = ProjectVolumetricCloudWorldShadowReferenceXZ(receiver, FVec3{1.0f, 0.03f, 0.0f}, referenceHeight);
    EXPECT_NEAR(lowSun.x, receiver.x, 1e-6f);
    EXPECT_NEAR(lowSun.y, receiver.z, 1e-6f);

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const FVec2 sanitized = ProjectVolumetricCloudWorldShadowReferenceXZ(FVec3{4.0f, nan, 8.0f}, FVec3{0.0f, 1.0f, 0.0f}, nan);
    EXPECT_NEAR(sanitized.x, 4.0f, 1e-6f);
    EXPECT_NEAR(sanitized.y, 8.0f, 1e-6f);

    const FVec2 requestedCenter{12345.25f, -6789.75f};
    const FVec2 minimum = VolumetricCloudWorldShadowMapMinimum(requestedCenter);
    const f32 halfExtent =
        kVolumetricCloudWorldShadowMapExtent * 0.5f;
    const FVec2 snappedCenter{
        minimum.x + halfExtent,
        minimum.y + halfExtent};
    const FVec2 expectedCenter{std::floor(requestedCenter.x / kVolumetricCloudWorldShadowMapTexelSize + 0.5f) * kVolumetricCloudWorldShadowMapTexelSize, std::floor(requestedCenter.y / kVolumetricCloudWorldShadowMapTexelSize + 0.5f) * kVolumetricCloudWorldShadowMapTexelSize};
    EXPECT_NEAR(snappedCenter.x, expectedCenter.x, 1e-6f);
    EXPECT_NEAR(snappedCenter.y, expectedCenter.y, 1e-6f);
    EXPECT_TRUE(std::abs(snappedCenter.x - requestedCenter.x) <= kVolumetricCloudWorldShadowMapTexelSize * 0.5f);
    EXPECT_TRUE(std::abs(snappedCenter.y - requestedCenter.y) <= kVolumetricCloudWorldShadowMapTexelSize * 0.5f);
}

ACS_TEST(VolumetricClouds, WorldShadowIntegratesFullCurvedCloudPathInPhysicalOrder) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    EXPECT_TRUE(Contains(
        shader,
        "CloudPackedBandIntervalsintersectCloudBandsFromPosition("
        "float3rayOrigin,float3rayDir){"));
    EXPECT_TRUE(Contains(
        shader,
        "floatrayEnd=cloudPlanetRayEnd(b,groundC);"));
    EXPECT_TRUE(Contains(
        shader,
        "returnpackCloudBandIntervalPairs("
        "lowerIntervals,upperIntervals);"));
    EXPECT_TRUE(Contains(shader, "floatcloudShellCFromLocalPosition(float3local,floataltitude){returndot(local.xz,local.xz)+(local.y-altitude)*(2.0*CLOUD_PLANET_RADIUS+local.y+altitude);}"));
    EXPECT_TRUE(Contains(shader, "[numthreads(8,8,1)]voidCSCloudWorldShadow(uint3tid:SV_DispatchThreadID){"));
    EXPECT_TRUE(Contains(shader, "uintupdateStride=max((uint)cloudWorldShadowUpdate.z,1u);"));
    EXPECT_TRUE(Contains(shader, "uint2outputPixel=tid.xy*updateStride+" "(uint2)cloudWorldShadowUpdate.xy;"));
    EXPECT_TRUE(Contains(shader, "if(any(outputPixel>=uint2(width,height)))return;"));
    EXPECT_TRUE(Contains(shader, "float4worldShadowValue=float4(saturate(transmittance),max(opticalDepth,0.0),0.0,1.0);"));
    EXPECT_FALSE(Contains(shader, "CLOUD_SHADOW_PARTIAL_BLEND"));
    EXPECT_FALSE(Contains(shader, "float4previousValue=cloudOut[outputPixel];"));
    EXPECT_FALSE(Contains(shader, "worldShadowValue=lerp("));
    EXPECT_TRUE(Contains(shader, "cloudOut[outputPixel]=worldShadowValue;"));
    EXPECT_TRUE(Contains(shader, "constintSAMPLE_COUNT=32;"));
    EXPECT_TRUE(Contains(
        shader,
        "if(!cloudLightSampleTerms("
        "bandIntervals,SAMPLE_COUNT,sampleIndex,"
        "sampleDistance,stepLength))continue;"));
    EXPECT_TRUE(Contains(
        shader,
        "floatextinction=max("
        "params.y*cloudLightingExtinction.y,0.0);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatsampleDepth=cloudLowLodOpticalDepthByOrderFromMacro("
        "p,macro,macro.densityWeatherMask,extinction.xxx,"
        "sun,stepLength,"
        "firstOrderState,secondOrderState,thirdOrderState).x;"));
    EXPECT_TRUE(Contains(
        shader,
        "opticalDepth+=sampleDepth;"));
    EXPECT_TRUE(Contains(
        shader,
        "transmittance=exp(-max(opticalDepth,0.0));"));
    // 8点の主光路だけに頼らず、晴天域を除く上下層を合計32点で積分する。
    const std::size_t worldShadowBegin = shader.find("voidCSCloudWorldShadow(");
    const std::size_t worldShadowEnd = shader.find("uintCloudTemporalBlockPhase4(", worldShadowBegin);
    EXPECT_TRUE(worldShadowBegin != std::string::npos);
    EXPECT_TRUE(worldShadowEnd != std::string::npos);
    if (worldShadowBegin != std::string::npos && worldShadowEnd != std::string::npos) {
        const std::string worldShadow = shader.substr(worldShadowBegin, worldShadowEnd - worldShadowBegin);
        EXPECT_EQ(CountOccurrences(worldShadow, "cloudOut["), static_cast<std::size_t>(1));
        EXPECT_FALSE(Contains(worldShadow, "sampleCloudShadowTail("));
        EXPECT_FALSE(Contains(worldShadow, "traceCloudShadowDepth("));
    }
}

ACS_TEST(VolumetricClouds, WorldShadowResourceIsOptionalAndRebuiltBeforeCloudTracing) {
    const std::string source = ReadSkySource();
    const std::string compact = CompactShader(source);
    EXPECT_TRUE(!compact.empty());

    // 同期・一括非同期に加え、通常実行時の段階投入経路も同じ入口を使う。
    EXPECT_EQ(CountOccurrences(compact, "\"CSCloudWorldShadow\",\"Clouds.WorldShadowCS\""), static_cast<std::size_t>(3));
    EXPECT_TRUE(Contains(compact, "td.width=kVolumetricCloudWorldShadowMapResolution;td.height=kVolumetricCloudWorldShadowMapResolution;td.format=EFormat::R16G16B16A16_Float;td.is_uav=true;"));
    EXPECT_TRUE(Contains(compact, "pd.uav_slots=1;pd.uav_names[0]=\"cloudOut\";"));
    EXPECT_TRUE(Contains(compact, "cl.BindUav(0,*m_WorldShadowTex);constu32updateWidth=CloudCeilDivisor(kVolumetricCloudWorldShadowMapResolution-worldShadowUpdateOffsetX,worldShadowUpdateDivisor);constu32updateHeight=CloudCeilDivisor(kVolumetricCloudWorldShadowMapResolution-worldShadowUpdateOffsetY,worldShadowUpdateDivisor);cl.Dispatch((updateWidth+7u)/8u,(updateHeight+7u)/8u,1u);"));
    const std::size_t worldShadowDispatch =
        compact.find("if(rebuildWorldShadowThisFrame){");
    const std::size_t mainCloudDispatch = compact.find("cl.SetComputePipeline(*m_CloudPipe);", worldShadowDispatch);
    EXPECT_TRUE(worldShadowDispatch != std::string::npos);
    EXPECT_TRUE(mainCloudDispatch != std::string::npos);
    EXPECT_TRUE(worldShadowDispatch < mainCloudDispatch);
    EXPECT_TRUE(Contains(
        compact,
        "m_WorldShadowValid?m_WorldShadowTex.Get():nullptr;"));
    EXPECT_TRUE(Contains(
        compact,
        "m_NoiseFilterResources->recorded_frame.command_list"
        "!=&command_list||"
        "m_NoiseFilterResources->recorded_frame.settings_revision"
        "!=m_NoiseFilterResources->settings_revision)"
        "{returnWorldShadowMap();}"));
    EXPECT_TRUE(Contains(
        compact,
        "recorded.world_shadow_valid?m_WorldShadowTex.Get():nullptr;"));
}

ACS_TEST(VolumetricClouds, LayeredAmbientDepthPreservesBothBandOpticalDepth) {
    const auto resolveDepths = [](f32 lowerColumnDepth, f32 lowerGroundDepth, f32 lowerSegmentDepth, f32 upperColumnDepth, f32 upperGroundDepth, f32 upperSegmentDepth, f32 segmentFraction) noexcept {
        const f32 lowerSampleGroundDepth = lowerGroundDepth + lowerSegmentDepth * segmentFraction;
        const f32 upperLocalGroundDepth = upperGroundDepth + upperSegmentDepth * segmentFraction;
        return FVec4{upperColumnDepth - upperLocalGroundDepth, upperLocalGroundDepth + lowerColumnDepth, lowerColumnDepth - lowerSampleGroundDepth + upperColumnDepth, lowerSampleGroundDepth};
    };

    constexpr f32 lowerColumnDepth = 2.4f;
    constexpr f32 upperColumnDepth = 0.8f;
    const FVec4 layeredDepth = resolveDepths(lowerColumnDepth, 0.9f, 0.2f, upperColumnDepth, 0.3f, 0.1f, 0.5f);
    const f32 totalColumnDepth = lowerColumnDepth + upperColumnDepth;
    EXPECT_NEAR(layeredDepth.x, 0.45f, 1e-6f);
    EXPECT_NEAR(layeredDepth.y, 2.75f, 1e-6f);
    EXPECT_NEAR(layeredDepth.z, 2.20f, 1e-6f);
    EXPECT_NEAR(layeredDepth.w, 1.00f, 1e-6f);
    EXPECT_NEAR(layeredDepth.x + layeredDepth.y, totalColumnDepth, 1e-6f);
    EXPECT_NEAR(layeredDepth.z + layeredDepth.w, totalColumnDepth, 1e-6f);
    // 上層の地面側は下層全体を、下層の空側は上層全体を必ず含む。
    EXPECT_TRUE(layeredDepth.y > lowerColumnDepth);
    EXPECT_TRUE(layeredDepth.z > upperColumnDepth);

    const FVec4 lowerOnlyDepth = resolveDepths(lowerColumnDepth, 0.9f, 0.2f, 0.0f, 0.0f, 0.0f, 0.5f);
    EXPECT_NEAR(lowerOnlyDepth.x, 0.0f, 0.0f);
    EXPECT_NEAR(lowerOnlyDepth.y, lowerColumnDepth, 1e-6f);
    EXPECT_NEAR(lowerOnlyDepth.z + lowerOnlyDepth.w, lowerColumnDepth, 1e-6f);

    // 接触する上下層では、下層頂と上層底が同じ方向別深さへ連続する。
    const FVec4 lowerTopDepth = resolveDepths(lowerColumnDepth, 2.2f, 0.2f, upperColumnDepth, 0.7f, 0.1f, 1.0f);
    const FVec4 upperBottomDepth = resolveDepths(lowerColumnDepth, 0.0f, 0.2f, upperColumnDepth, 0.0f, 0.1f, 0.0f);
    EXPECT_NEAR(lowerTopDepth.z, upperBottomDepth.x, 1e-6f);
    EXPECT_NEAR(lowerTopDepth.w, upperBottomDepth.y, 1e-6f);
    EXPECT_NEAR(lowerTopDepth.z, upperColumnDepth, 1e-6f);
    EXPECT_NEAR(lowerTopDepth.w, lowerColumnDepth, 1e-6f);

    // 非一様な32区間を使い、各行の端点位置を区分一定積分から独立に再計算する。
    // これによりj/31、出力前の接頭和、上下層の方向別合計を全行で固定する。
    constexpr u32 segmentCount = 32u;
    f32 lowerSegments[segmentCount]{};
    f32 upperSegments[segmentCount]{};
    f32 lowerTotalDepth = 0.0f;
    f32 upperTotalDepth = 0.0f;
    for (u32 segmentIndex = 0u; segmentIndex < segmentCount; ++segmentIndex) {
        lowerSegments[segmentIndex] = 0.01f * static_cast<f32>(segmentIndex + 1u) + 0.003f * static_cast<f32>(segmentIndex % 5u);
        upperSegments[segmentIndex] = 0.007f * static_cast<f32>(segmentCount - segmentIndex) + 0.002f * static_cast<f32>(segmentIndex % 3u);
        lowerTotalDepth += lowerSegments[segmentIndex];
        upperTotalDepth += upperSegments[segmentIndex];
    }

    f32 lowerPrefixDepth = 0.0f;
    f32 upperPrefixDepth = 0.0f;
    for (u32 outputIndex = 0u; outputIndex < segmentCount; ++outputIndex) {
        const f32 rowHeight = static_cast<f32>(outputIndex) / static_cast<f32>(segmentCount - 1u);
        const f32 cellPosition = rowHeight * static_cast<f32>(segmentCount);
        u32 expectedCellIndex = static_cast<u32>(cellPosition);
        f32 expectedCellFraction = cellPosition - static_cast<f32>(expectedCellIndex);
        if (expectedCellIndex >= segmentCount) {
            expectedCellIndex = segmentCount - 1u;
            expectedCellFraction = 1.0f;
        }
        f32 expectedLowerGroundDepth = 0.0f;
        f32 expectedUpperGroundDepth = 0.0f;
        for (u32 segmentIndex = 0u; segmentIndex < expectedCellIndex; ++segmentIndex) {
            expectedLowerGroundDepth += lowerSegments[segmentIndex];
            expectedUpperGroundDepth += upperSegments[segmentIndex];
        }
        expectedLowerGroundDepth += lowerSegments[expectedCellIndex] * expectedCellFraction;
        expectedUpperGroundDepth += upperSegments[expectedCellIndex] * expectedCellFraction;

        const FVec4 rowDepth = resolveDepths(lowerTotalDepth, lowerPrefixDepth, lowerSegments[outputIndex], upperTotalDepth, upperPrefixDepth, upperSegments[outputIndex], rowHeight);
        EXPECT_NEAR(rowDepth.w, expectedLowerGroundDepth, 2e-6f);
        EXPECT_NEAR(rowDepth.y, expectedUpperGroundDepth + lowerTotalDepth, 2e-6f);
        EXPECT_NEAR(rowDepth.z, lowerTotalDepth - expectedLowerGroundDepth + upperTotalDepth, 2e-6f);
        EXPECT_NEAR(rowDepth.x, upperTotalDepth - expectedUpperGroundDepth, 2e-6f);
        EXPECT_NEAR(rowDepth.x + rowDepth.y, lowerTotalDepth + upperTotalDepth, 2e-6f);
        EXPECT_NEAR(rowDepth.z + rowDepth.w, lowerTotalDepth + upperTotalDepth, 2e-6f);
        lowerPrefixDepth += lowerSegments[outputIndex];
        upperPrefixDepth += upperSegments[outputIndex];
    }

    // 4x1x4の16スレッドが、共有配列512要素を重複も欠落もなく所有する。
    constexpr u32 groupThreadCount = 16u;
    constexpr u32 sharedElementCount = groupThreadCount * segmentCount;
    bool visitedSharedElement[sharedElementCount]{};
    for (u32 groupIndex = 0u; groupIndex < groupThreadCount; ++groupIndex) {
        for (u32 segmentIndex = 0u; segmentIndex < segmentCount; ++segmentIndex) {
            const u32 sharedIndex = groupIndex * segmentCount + segmentIndex;
            EXPECT_TRUE(sharedIndex < sharedElementCount);
            EXPECT_FALSE(visitedSharedElement[sharedIndex]);
            visitedSharedElement[sharedIndex] = true;
        }
    }
    for (u32 sharedIndex = 0u; sharedIndex < sharedElementCount; ++sharedIndex) EXPECT_TRUE(visitedSharedElement[sharedIndex]);
}

ACS_TEST(VolumetricClouds,
         ShadowCacheIntegratesFullFiniteSunPathsAndKeepsDetailResidual) {
    const std::string source = ReadSkySource();
    const std::string compactSource = CompactShader(source);
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());
    const auto sliceBetween = [](
        const std::string& text, const char* beginMarker,
        const char* endMarker) {
        const std::size_t begin = text.find(beginMarker);
        if (begin == std::string::npos) return std::string{};
        const std::size_t end = text.find(endMarker, begin);
        if (end == std::string::npos || end <= begin) return std::string{};
        return text.substr(begin, end - begin);
    };

    EXPECT_EQ(
        CountOccurrences(
            compactSource,
            "ResolveVolumetricCloudAmbientCacheMapTerms_Internal("
            "m_Range.MaxDistance);"),
        static_cast<std::size_t>(2));
    EXPECT_FALSE(Contains(compactSource, "kCloudAmbientResolveCS"));
    EXPECT_FALSE(Contains(shader, "cloudAmbientRaw"));
    EXPECT_FALSE(Contains(shader, "CLOUD_AMBIENT_CACHE_SUPERSAMPLE"));
    EXPECT_TRUE(Contains(
        shader,
        "RWTexture3D<float4>cloudShadowOut:register(u2);"));
    EXPECT_TRUE(Contains(
        shader,
        "staticconstuintCLOUD_SHADOW_CACHE_TEXTURE_HEIGHT="
        "4u*CLOUD_SHADOW_CACHE_HEIGHT;"));
    EXPECT_TRUE(Contains(
        shader,
        "staticconstuintCLOUD_SHADOW_CACHE_GROUP_THREAD_COUNT=16u;"));
    EXPECT_TRUE(Contains(
        shader,
        "groupsharedfloat2cloudShadowColumnSegmentDepths["
        "CLOUD_SHADOW_CACHE_GROUP_THREAD_COUNT*"
        "CLOUD_SHADOW_CACHE_HEIGHT];"));
    EXPECT_TRUE(Contains(
        shader,
        "groupshareduint4cloudAmbientQuantizedVisibilitySums["
        "CLOUD_SHADOW_CACHE_GROUP_THREAD_COUNT*"
        "CLOUD_SHADOW_CACHE_HEIGHT];"));
    EXPECT_TRUE(Contains(
        shader,
        "groupsharedfloat3cloudSunVisibilityProfiles["
        "CLOUD_SUN_CACHE_GROUP_THREAD_COUNT*"
        "CLOUD_SHADOW_CACHE_HEIGHT];"));

    const std::size_t shadowEntry =
        shader.find("[numthreads(4,1,4)]voidCSCloudShadow(");
    const std::size_t shadowEnd =
        shader.find(
            "[numthreads(8,8,1)]voidCSCloudWorldShadow(",
            shadowEntry);
    EXPECT_TRUE(shadowEntry != std::string::npos);
    EXPECT_TRUE(shadowEnd != std::string::npos);
    std::string shadowKernel;
    if (shadowEntry != std::string::npos &&
        shadowEnd != std::string::npos) {
        shadowKernel =
            shader.substr(shadowEntry, shadowEnd - shadowEntry);
    }
    EXPECT_EQ(
        CountOccurrences(
            shadowKernel,
            "GroupMemoryBarrierWithGroupSync();"),
        static_cast<std::size_t>(1));
    EXPECT_TRUE(Contains(
        shadowKernel,
        "uint2quadratureIndex=uint2("
        "groupIndex%CLOUD_AMBIENT_CACHE_QUADRATURE_AXIS,"
        "groupIndex/CLOUD_AMBIENT_CACHE_QUADRATURE_AXIS);"));
    EXPECT_TRUE(Contains(
        shadowKernel,
        "float2ambientFootprint=float2("
        "ambientWidthX,ambientWidthZ);"));
    EXPECT_TRUE(Contains(
        shadowKernel,
        "float2ambientWorldXz=float2("
        "ambientSampleX,ambientSampleZ)+cloudWindWorld();"));
    EXPECT_TRUE(Contains(
        shadowKernel,
        "cloudAmbientQuadratureDensity("
        "ambientWorldXz,ambientFootprint,"));
    EXPECT_FALSE(Contains(shadowKernel, "ambientSubsample"));
    EXPECT_FALSE(Contains(shadowKernel, "maximumColumnSpacing"));
    EXPECT_TRUE(Contains(
        shadowKernel,
        "sampleIndex<CLOUD_SHADOW_CACHE_GROUP_THREAD_COUNT;"
        "++sampleIndex){"));

    // 一次環境光には密度倍率と光側消散だけを使い、高次散乱の縮小率を混ぜない。
    EXPECT_TRUE(Contains(
        shadowKernel,
        "floatambientExtinction=max(params.y,0.0)"
        "*max(cloudLightingExtinction.y,0.0);"));
    const std::string ambientGeneration = sliceBetween(
        shadowKernel,
        "floatambientExtinction=max(params.y,0.0)",
        "if(groupIndex<CLOUD_SUN_CACHE_GROUP_THREAD_COUNT){");
    EXPECT_TRUE(!ambientGeneration.empty());
    EXPECT_FALSE(Contains(ambientGeneration, "cloudLightingMulti.x"));
    EXPECT_TRUE(Contains(
        shadowKernel,
        "cloudAmbientQuantizedVisibilitySums[profileIndex]="
        "cloudQuantizeAmbientVisibility("
        "cloudHemisphericVisibility("
        "pathDepth));"));
    EXPECT_FALSE(Contains(
        shadowKernel,
        "cloudHemisphericVisibility(pathDepth*ambientExtinction)"));
    EXPECT_FALSE(Contains(
        shadowKernel,
        "cloudAmbientQuantizedVisibilitySums[profileIndex]+="));
    EXPECT_TRUE(Contains(
        shadowKernel,
        "=saturate(float4(resolvedVisibilitySum)"
        "/(65535.0"
        "*float(CLOUD_SHADOW_CACHE_GROUP_THREAD_COUNT)));"));

    // 各列の光学的深さを半球透過率へ変換してから空間平均する。
    const std::size_t layeredDepth =
        shadowKernel.find("float4pathDepth=cloudLayeredAmbientDepth(");
    const std::size_t visibilitySum =
        shadowKernel.find(
            "cloudAmbientQuantizedVisibilitySums[profileIndex]=",
            layeredDepth);
    const std::size_t visibility =
        shadowKernel.find(
            "cloudHemisphericVisibility(pathDepth)",
            visibilitySum);
    const std::size_t barrier =
        shadowKernel.find(
            "GroupMemoryBarrierWithGroupSync();",
            visibilitySum);
    const std::size_t resolvedWrite =
        shadowKernel.find(
            "cloudShadowOut[uint3("
            "outputColumn.x,outputHeightIndex,outputColumn.y)]",
            barrier);
    EXPECT_TRUE(layeredDepth != std::string::npos);
    EXPECT_TRUE(visibility != std::string::npos);
    EXPECT_TRUE(visibilitySum != std::string::npos);
    EXPECT_TRUE(barrier != std::string::npos);
    EXPECT_TRUE(resolvedWrite != std::string::npos);
    EXPECT_TRUE(layeredDepth < visibilitySum);
    EXPECT_TRUE(visibilitySum < visibility);
    EXPECT_TRUE(visibilitySum < barrier);
    EXPECT_TRUE(barrier < resolvedWrite);

    // 先頭4スレッドが同一点の太陽円盤4方向を担当し、同期後にRGBAへまとめる。
    const std::size_t sunOwner =
        shadowKernel.find(
            "if(groupIndex<CLOUD_SUN_CACHE_GROUP_THREAD_COUNT){");
    const std::size_t sunDirection =
        shadowKernel.find(
            "cloudSunDiskDirection("
            "sun,cloudLightTangent.xyz,"
            "cloudLightBitangent.xyz,groupIndex)",
            sunOwner);
    const std::size_t sunTrace =
        shadowKernel.find(
            "float3sunDepths=traceCloudShadowDepths("
            "sunP,coverage,finiteSunDirection,"
            "float3(firstExtinction,secondExtinction,thirdExtinction));",
            sunDirection);
    const std::size_t sunConversion =
        shadowKernel.find(
            "cloudSunVisibilityProfiles["
            "groupIndex*CLOUD_SHADOW_CACHE_HEIGHT+sunHeightIndex]="
            "exp(-max(sunDepths,0.0.xxx));",
            sunTrace);
    const std::size_t firstSunPublish = shadowKernel.find(
        "cloudShadowOut[uint3("
        "outputColumn.x,"
        "outputHeightIndex+CLOUD_SHADOW_CACHE_HEIGHT,"
        "outputColumn.y)]=saturate(firstVisibility);",
        barrier);
    const std::size_t secondSunPublish = shadowKernel.find(
        "cloudShadowOut[uint3("
        "outputColumn.x,"
        "outputHeightIndex+2u*CLOUD_SHADOW_CACHE_HEIGHT,"
        "outputColumn.y)]=saturate(secondVisibility);",
        firstSunPublish);
    const std::size_t thirdSunPublish = shadowKernel.find(
        "cloudShadowOut[uint3("
        "outputColumn.x,"
        "outputHeightIndex+3u*CLOUD_SHADOW_CACHE_HEIGHT,"
        "outputColumn.y)]=saturate(thirdVisibility);",
        secondSunPublish);
    EXPECT_TRUE(sunOwner != std::string::npos);
    EXPECT_TRUE(sunDirection != std::string::npos);
    EXPECT_TRUE(sunTrace != std::string::npos);
    EXPECT_TRUE(sunConversion != std::string::npos);
    EXPECT_TRUE(firstSunPublish != std::string::npos);
    EXPECT_TRUE(secondSunPublish != std::string::npos);
    EXPECT_TRUE(thirdSunPublish != std::string::npos);
    EXPECT_TRUE(sunOwner < sunDirection);
    EXPECT_TRUE(sunDirection < sunTrace);
    EXPECT_TRUE(sunTrace < sunConversion);
    EXPECT_TRUE(sunConversion < barrier);
    EXPECT_TRUE(barrier < firstSunPublish);
    EXPECT_TRUE(firstSunPublish < secondSunPublish);
    EXPECT_TRUE(secondSunPublish < thirdSunPublish);
    EXPECT_FALSE(Contains(shadowKernel, "floatmeanDepth="));
    EXPECT_FALSE(Contains(shadowKernel, "previousSunDepth"));
    EXPECT_FALSE(Contains(shadowKernel, "previousValue"));

    const std::string hemisphericVisibility = sliceBetween(
        shader,
        "float4cloudHemisphericVisibility(",
        "uint4cloudQuantizeAmbientVisibility(");
    EXPECT_TRUE(Contains(
        hemisphericVisibility,
        "0.0694318442029737,0.3300094782075719,"
        "0.6699905217924281,0.9305681557970262"));
    EXPECT_TRUE(Contains(
        hemisphericVisibility,
        "0.0241522034128332,0.2152140822717850,"
        "0.4369310725907611,0.3237026417246206"));

    // キャッシュ外も同じ一次環境光契約を使う。
    const std::size_t fallbackDepth =
        shader.find(
            "float2fallbackAmbientDepth="
            "cloudAmbientFallbackOpticalDepth(");
    const std::size_t fallbackVisibility =
        shader.find(
            "float4fallbackAmbientVisibility="
            "cloudHemisphericVisibility(",
            fallbackDepth);
    const std::size_t cachedVisibility =
        shader.find(
            "float3cachedAmbientVisibility="
            "sampleCloudAmbientVisibility(p);",
            fallbackVisibility);
    EXPECT_TRUE(fallbackDepth != std::string::npos);
    EXPECT_TRUE(fallbackVisibility != std::string::npos);
    EXPECT_TRUE(cachedVisibility != std::string::npos);
    EXPECT_TRUE(fallbackDepth < fallbackVisibility);
    EXPECT_TRUE(fallbackVisibility < cachedVisibility);

    // 参照描画は500 mキャッシュを迂回し、完成密度を直接積分する。
    const std::size_t cacheCondition = shader.find(
        "if(CLOUD_MAIN_SHADOW_CACHE_ENABLED&&"
        "cloudLightingAmbient.w<0.5){");
    const std::size_t cacheRead = shader.find(
        "sampleCloudSunTransmittance("
        "p,cacheBlendWeight,"
        "cachedFirstVisibility,cachedSecondVisibility,"
        "cachedThirdVisibility);",
        cacheCondition);
    const std::size_t detailResidual = shader.find(
        "float4firstDetailOpticalDepthResiduals=0.0.xxxx;", cacheRead);
    const std::size_t detailGuard = shader.find(
        "if(cacheBlendWeight>0.0){", detailResidual);
    const std::size_t detailDirectionLoop = shader.find(
        "[loop]for(uintresidualDirectionIndex=0u;"
        "residualDirectionIndex<CLOUD_SUN_DISK_DIRECTION_COUNT;"
        "++residualDirectionIndex){",
        detailGuard);
    const std::size_t detailEvaluation = shader.find(
        "float3directionalResiduals=cloudNearLightOpticalDepthResiduals("
        "p,context.coverage,residualSunDirection,"
        "cacheExtinctionByOrder);",
        detailDirectionLoop);
    const std::size_t reliabilityEvaluation = shader.find(
        "floatcacheReliability=cloudSunDepthResidualCacheReliability("
        "cachedFirstVisibility,cachedSecondVisibility,"
        "cachedThirdVisibility,"
        "firstDetailOpticalDepthResiduals,"
        "secondDetailOpticalDepthResiduals,"
        "thirdDetailOpticalDepthResiduals);",
        detailEvaluation);
    const std::size_t reliabilityApply = shader.find(
        "cacheBlendWeight*=cacheReliability;", reliabilityEvaluation);
    const std::size_t exactFallback =
        shader.find("if(cacheBlendWeight<1.0){", reliabilityApply);
    const std::size_t exactPath = shader.find(
        "float3exactDepths=traceCloudMainLightDepths("
        "p,context.coverage,finiteSunDirection,"
        "exactExtinctionByOrder);",
        exactFallback);
    const std::size_t exactConversion = shader.find(
        "exactScatteringSum+=exactVisibility*float3("
        "directionalPhase,directionalPhaseMulti,directionalPhaseMulti);",
        exactPath);
    const std::size_t cachedCorrection = shader.find(
        "float4correctedCachedFirst=cloudApplySunOpticalDepthResidual("
        "cachedFirstVisibility,firstDetailOpticalDepthResiduals);",
        exactConversion);
    const std::size_t visibilityBlend = shader.find(
        "float3lightScatteringByOrder=lerp("
        "exactAverageScattering,correctedCachedAverageScattering,"
        "cacheBlendWeight);",
        cachedCorrection);
    const std::size_t firstLight = shader.find(
        "floatfirstLightScattering=lightScatteringByOrder.x;",
        visibilityBlend);
    EXPECT_TRUE(cacheCondition != std::string::npos);
    EXPECT_TRUE(detailResidual != std::string::npos);
    EXPECT_TRUE(detailGuard != std::string::npos);
    EXPECT_TRUE(detailDirectionLoop != std::string::npos);
    EXPECT_TRUE(detailEvaluation != std::string::npos);
    EXPECT_TRUE(reliabilityEvaluation != std::string::npos);
    EXPECT_TRUE(reliabilityApply != std::string::npos);
    EXPECT_TRUE(cacheRead != std::string::npos);
    EXPECT_TRUE(exactFallback != std::string::npos);
    EXPECT_TRUE(exactPath != std::string::npos);
    EXPECT_TRUE(exactConversion != std::string::npos);
    EXPECT_TRUE(cachedCorrection != std::string::npos);
    EXPECT_TRUE(visibilityBlend != std::string::npos);
    EXPECT_TRUE(firstLight != std::string::npos);
    EXPECT_TRUE(cacheCondition < cacheRead);
    EXPECT_TRUE(cacheRead < detailResidual);
    EXPECT_TRUE(detailResidual < detailGuard);
    EXPECT_TRUE(detailGuard < detailDirectionLoop);
    EXPECT_TRUE(detailDirectionLoop < detailEvaluation);
    EXPECT_TRUE(detailEvaluation < reliabilityEvaluation);
    EXPECT_TRUE(reliabilityEvaluation < reliabilityApply);
    EXPECT_TRUE(reliabilityApply < exactFallback);
    EXPECT_TRUE(exactFallback < exactPath);
    EXPECT_TRUE(exactPath < exactConversion);
    EXPECT_TRUE(exactConversion < cachedCorrection);
    EXPECT_TRUE(cachedCorrection < visibilityBlend);
    EXPECT_TRUE(visibilityBlend < firstLight);
    EXPECT_FALSE(Contains(shader, "lightDepths=lerp("));
    EXPECT_FALSE(Contains(shader, "sampleCloudSunDepths("));

    const std::string residualReliability = sliceBetween(
        shader,
        "floatcloudR16TransmittanceHalfUlp(",
        "float3cloudSunDiskDirection(");
    EXPECT_TRUE(Contains(
        residualReliability,
        "constfloatminimumNormal=0.00006103515625;"
        "constfloatmaximumBelowOne=0.99951171875;"
        "constfloatsubnormalHalfUlp=0.0000000298023223876953125;"));
    EXPECT_TRUE(Contains(
        residualReliability,
        "floatamplification=exp(clamp("
        "-opticalDepthResidual,0.0,16.0));"));
    EXPECT_TRUE(Contains(
        residualReliability,
        "floatcorrectedHalfUlp="
        "cloudR16TransmittanceHalfUlp(correctedVisibility);"));
    EXPECT_TRUE(Contains(
        residualReliability,
        "reliability=saturate("
        "correctedHalfUlp/max(amplifiedHalfUlp,1e-30));"));
    EXPECT_TRUE(Contains(residualReliability, "returnreliability;}"));
    EXPECT_TRUE(Contains(
        residualReliability,
        "floatcloudSunDepthResidualCacheReliability("));
    EXPECT_TRUE(Contains(
        residualReliability,
        "float4thirdVisibility,"
        "float4firstOpticalDepthResiduals,"
        "float4secondOpticalDepthResiduals,"
        "float4thirdOpticalDepthResiduals){"));
    EXPECT_TRUE(Contains(
        residualReliability,
        "boolvalidInput=!any("
        "firstOpticalDepthResiduals!=firstOpticalDepthResiduals)"
        "&&!any(secondOpticalDepthResiduals!=secondOpticalDepthResiduals)"
        "&&!any(thirdOpticalDepthResiduals!=thirdOpticalDepthResiduals)"
        "&&all(abs(firstOpticalDepthResiduals)<=3.0e38.xxxx)"
        "&&all(abs(secondOpticalDepthResiduals)<=3.0e38.xxxx)"
        "&&all(abs(thirdOpticalDepthResiduals)<=3.0e38.xxxx)"
        "&&!any(firstVisibility!=firstVisibility)"
        "&&!any(secondVisibility!=secondVisibility)"
        "&&!any(thirdVisibility!=thirdVisibility);"
        "floatreliability=0.0;"
        "if(validInput){"));
    EXPECT_TRUE(Contains(
        residualReliability,
        "[unroll]for(uintdirectionIndex=0u;directionIndex<4u;"
        "++directionIndex){"));
    EXPECT_TRUE(Contains(
        residualReliability,
        "firstVisibility[directionIndex],"
        "firstOpticalDepthResiduals[directionIndex]));"));
    EXPECT_TRUE(Contains(
        residualReliability,
        "secondVisibility[directionIndex],"
        "secondOpticalDepthResiduals[directionIndex]));"));
    EXPECT_TRUE(Contains(
        residualReliability,
        "thirdVisibility[directionIndex],"
        "thirdOpticalDepthResiduals[directionIndex]));"));
    EXPECT_FALSE(Contains(
        residualReliability,
        "if(opticalDepthResidual>=0.0)return1.0;"));
    EXPECT_FALSE(Contains(
        shader, "cloudSunDepthResidualUsesReliableCache("));

    const std::string directLight = sliceBetween(
        shader,
        "float3traceCloudMainLightDepths(",
        "float3cloudShadowWorldPositionAtAltitude(");
    EXPECT_TRUE(Contains(
        directLight,
        "float3lightSegmentDepth="
        "cloudDensityOpticalDepthByOrderFromMacro("));
    EXPECT_FALSE(Contains(
        directLight,
        "cloudLowLodOpticalDepthByOrderFromMacro("));

    // 視線・光・環境光は同じ一点値を使い、担当幅で未解像周波数だけを除く。
    EXPECT_TRUE(Contains(
        shader,
        "float4cloudWeatherDataBandLimitedPoint("
        "float3p,float2resolutionFootprint){"));
    EXPECT_TRUE(Contains(
        shader,
        "constfloatregionalShortestPeriod=9127.0/29.0;"));
    EXPECT_FALSE(Contains(shader, "constfloatglobalShortestPeriod="));
    EXPECT_EQ(
        CountOccurrences(
            sliceBetween(
                shader,
                "float4cloudWeatherDataBandLimitedPoint("
                "float3p,float2resolutionFootprint){",
                "float3rotateNoise("),
            "cloudWeatherDataAtMaterialXz("),
        static_cast<std::size_t>(1));
    EXPECT_TRUE(Contains(
        shader,
        "float3physicalFootprint="
        "abs(rayDirection)*max(sampleSpacing,0.0);"
        "returnsampleCloudMacroLightingBandLimited("
        "p,weatherCoverage,physicalFootprint,0.0.xxx);"));
    EXPECT_FALSE(Contains(shader, "float2offset=0.2886751345948129"));
    EXPECT_TRUE(Contains(
        shader,
        "constfloatshortestCurlPeriod=947.0/17.0;"));
    EXPECT_EQ(
        CountOccurrences(shader, "cloudWeatherData(p,0.0.xx);"),
        static_cast<std::size_t>(0));
    EXPECT_EQ(
        CountOccurrences(shader, "cloudCurlOffset(p,0.0.xx);"),
        static_cast<std::size_t>(0));
    EXPECT_TRUE(Contains(shader, "cloudWeatherDataBandLimitedPoint(p,safeFootprint.xz);"));
    EXPECT_TRUE(Contains(shader, "cloudCurlOffset(p,safeFootprint.xz);"));
    EXPECT_TRUE(Contains(
        shader,
        "float4cloudShapePotentialBands(float3uvw){"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudShapeOccupancyMaximum("
        "float3uvw,floatmaximumDomainFootprint){"));
    const std::string filteredShape = sliceBetween(
        shader,
        "float4cloudShapePotentialBands(",
        "floatcloudShapeOccupancyMaximum(");
    const std::string filteredOccupancy = sliceBetween(
        shader,
        "floatcloudShapeOccupancyMaximum(",
        "floatcloudWeatherThreshold(");
    EXPECT_EQ(
        CountOccurrences(
            filteredShape,
            "shapeNoise.SampleLevel("),
        static_cast<std::size_t>(1));
    EXPECT_EQ(
        CountOccurrences(
            filteredShape,
            "shapeOccupancy.SampleLevel("),
        static_cast<std::size_t>(0));
    EXPECT_EQ(
        CountOccurrences(
            filteredOccupancy,
            "shapeNoise.SampleLevel("),
        static_cast<std::size_t>(0));
    EXPECT_EQ(
        CountOccurrences(
            filteredOccupancy,
            "shapeOccupancy.SampleLevel("),
        static_cast<std::size_t>(1));
    const std::size_t completedRead = filteredShape.find(
        "float4densityBands=shapeNoise.SampleLevel("
        "shapeNoise_sampler,uvw,0);");
    const std::size_t occupancyRead = filteredOccupancy.find(
        "float4occupancyBands=shapeOccupancy.SampleLevel("
        "shapeOccupancy_sampler,uvw,0);",
        0u);
    const std::size_t footprint = filteredOccupancy.find(
        "floatfootprintVoxels=max(maximumDomainFootprint,0.0)*128.0;",
        occupancyRead);
    const std::size_t storedShape = filteredShape.find(
        "cloudSignedPotentialFromStored(densityBands.b)",
        completedRead);
    const std::size_t storedOccupancy = filteredOccupancy.find(
        "returncloudSignedPotentialFromStored(occupancyStoredPotential);",
        footprint);
    EXPECT_TRUE(completedRead != std::string::npos);
    EXPECT_TRUE(occupancyRead != std::string::npos);
    EXPECT_TRUE(footprint != std::string::npos);
    EXPECT_TRUE(storedShape != std::string::npos);
    EXPECT_TRUE(storedOccupancy != std::string::npos);
    EXPECT_TRUE(occupancyRead < footprint);
    EXPECT_TRUE(completedRead < storedShape);
    EXPECT_TRUE(footprint < storedOccupancy);
    EXPECT_FALSE(Contains(filteredShape, "cloudShapeFrequencyVisibility("));
    EXPECT_FALSE(Contains(filteredOccupancy, "cloudShapeFrequencyWeights("));
    EXPECT_FALSE(Contains(filteredShape, "cloudPerlinWorleyShape("));
    EXPECT_FALSE(Contains(filteredShape, "unresolvedPerlinMean"));
    EXPECT_FALSE(Contains(filteredShape, "unresolvedWorleyMean"));
}

ACS_TEST(VolumetricClouds, ShadowCacheRhiBindingIsOptionalOrderedAndUpdatedEveryFrame) {
    const std::string source = ReadSkySource();
    const std::string compact = CompactShader(source);
    EXPECT_TRUE(!compact.empty());
    EXPECT_TRUE(kVolumetricCloudShadowCacheEnabled);

    // 同期・一括非同期では同じ一式を作り、通常実行時は同じ入口を後段で一度だけ投入する。
    EXPECT_EQ(
        CountOccurrences(
            compact,
            "autoshadow_result=compile("
            "EShaderStage::Compute,kCloudCS,"
            "\"CSCloudShadow\",\"Clouds.ShadowCacheCS\");"),
        static_cast<std::size_t>(2));
    EXPECT_EQ(
        CountOccurrences(
            compact,
            "\"CSCloudShadow\",\"Clouds.ShadowCacheCS\""),
        static_cast<std::size_t>(3));
    EXPECT_FALSE(Contains(compact, "kCloudAmbientResolveCS"));
    EXPECT_FALSE(Contains(compact, "CSCloudAmbientResolve"));
    EXPECT_FALSE(Contains(compact, "m_ShadowAmbient"));
    EXPECT_FALSE(Contains(compact, "m_ShadowRaw"));
    EXPECT_FALSE(Contains(compact, "CLOUD_AMBIENT_CACHE_SUPERSAMPLE"));

    // 任意シェーダーが無ければ雲本体は失敗させず、キャッシュだけを縮退させる。
    EXPECT_TRUE(Contains(
        compact,
        "boolshadowOk=kVolumetricCloudShadowCacheEnabled&&"
        "shaders.shadow;"
        "if(shadowOk){m_ShadowCs=Move(shaders.shadow);}"));
    EXPECT_TRUE(Contains(
        compact,
        "if(!shadowOk){"
        "m_ShadowTex.Reset();"
        "m_ShadowPipe.Reset();"
        "m_ShadowCs.Reset();"));
    const std::size_t optionalBegin = compact.find(
        "boolshadowOk=kVolumetricCloudShadowCacheEnabled&&"
        "shaders.shadow;");
    const std::size_t optionalEnd =
        compact.find("m_ShadowCacheDispatchCount=0;}", optionalBegin);
    EXPECT_TRUE(optionalBegin != std::string::npos);
    EXPECT_TRUE(optionalEnd != std::string::npos);
    if (optionalBegin != std::string::npos &&
        optionalEnd != std::string::npos) {
        EXPECT_FALSE(Contains(
            compact.substr(optionalBegin, optionalEnd - optionalBegin),
            "returnErr"));
    }

    // 一つのパイプラインと一つの3Dテクスチャだけを所有し、既存ABIへ型を足さない。
    EXPECT_TRUE(Contains(
        compact,
        "pd.srv_slots=5;"
        "pd.srv_names[0]=\"shapeNoise\";"
        "pd.srv_names[1]=\"shapeOccupancy\";"
        "pd.srv_names[2]=\"weatherMap\";"
        "pd.srv_names[3]=\"detailNoise\";"
        "pd.srv_names[4]=\"curlNoise\";"));
    EXPECT_TRUE(Contains(
        compact,
        "pd.uav_slots=3;"
        "pd.uav_names[0]=\"cloudOut\";"
        "pd.uav_names[1]=\"cloudDepthOut\";"
        "pd.uav_names[2]=\"cloudShadowOut\";"));
    EXPECT_TRUE(Contains(
        compact,
        "td.width=kVolumetricCloudShadowCacheWidth;"
        "td.height=4u*kVolumetricCloudShadowCacheHeight;"
        "td.depth=kVolumetricCloudShadowCacheDepth;"
        "td.format=EFormat::R16G16B16A16_Float;"
        "td.is_uav=true;"));
    EXPECT_EQ(
        CountOccurrences(
            compact,
            "td.width=kVolumetricCloudShadowCacheWidth;"
            "td.height=4u*kVolumetricCloudShadowCacheHeight;"
            "td.depth=kVolumetricCloudShadowCacheDepth;"
            "td.format=EFormat::R16G16B16A16_Float;"
            "td.is_uav=true;"),
        static_cast<std::size_t>(1));

    // 1グループを1セルへ投入し、周囲光と太陽光を一度のDispatchで更新する。
    const std::size_t shadowBuild =
        compact.find("if(rebuildShadowCacheThisFrame){");
    const std::size_t shadowBuildEnd =
        compact.find(
            "++recorded.shadow_cache_dispatch_count;",
            shadowBuild);
    EXPECT_TRUE(shadowBuild != std::string::npos);
    EXPECT_TRUE(shadowBuildEnd != std::string::npos);
    if (shadowBuild != std::string::npos &&
        shadowBuildEnd != std::string::npos) {
        const std::string build = compact.substr(
            shadowBuild, shadowBuildEnd - shadowBuild);
        EXPECT_EQ(
            CountOccurrences(build, "cl.SetComputePipeline(*m_ShadowPipe);"),
            static_cast<std::size_t>(1));
        EXPECT_EQ(
            CountOccurrences(build, "cl.Dispatch("),
            static_cast<std::size_t>(1));
        EXPECT_TRUE(Contains(
            build,
            "cl.BindUav(2,*m_ShadowTex);"
            "cl.Dispatch(updateWidth,1u,updateDepth);"));
        EXPECT_FALSE(Contains(build, "ambientRaw"));
        EXPECT_FALSE(Contains(build, "resolve"));
    }
    EXPECT_TRUE(Contains(
        compact,
        "constboolshadowResourcesReady="
        "m_ShadowCacheAvailable&&"
        "m_ShadowCs&&m_ShadowPipe&&m_ShadowTex;"));
    EXPECT_FALSE(Contains(
        compact,
        "false&&m_ShadowCacheAvailable"));
    EXPECT_TRUE(Contains(
        compact,
        "m_ShadowTex.Reset();"
        "m_ShadowPipe.Reset();m_ShadowCs.Reset();"));

    // 旧二段APIの欄は空のままでも状態集約を妨げない。
    EXPECT_TRUE(Contains(
        compact,
        "if(shadow_finalize){"
        "constEShaderStatusambientStatus="
        "shadow_finalize->Status();"));
    EXPECT_FALSE(Contains(
        compact,
        "shaders.shadow_finalize=Move("));
}

ACS_TEST(VolumetricClouds,
         ViewCutDetectionRetainsHistoryAcrossOrdinaryEditorTranslation) {
    CCamera previousCamera;
    previousCamera.SetPerspective(60.0f * kDeg2Rad, 16.0f / 9.0f,
                                  0.05f, 250000.0f);
    const FVec3 previousEye{64012.0f, 24.0f, 95964.0f};
    previousCamera.SetLookAt(previousEye, FVec3{64012.0f, 24.0f, 95965.0f});

    CCamera translatedCamera;
    translatedCamera.SetPerspective(60.0f * kDeg2Rad, 16.0f / 9.0f,
                                    0.05f, 250000.0f);
    const FVec3 translatedEye{64092.0f, 40.0f, 96004.0f};
    translatedCamera.SetLookAt(translatedEye, FVec3{64092.0f, 40.0f, 96005.0f});

    EXPECT_FALSE(VolumetricCloudViewCutDetected(BuildCameraRelativeInverseViewProjection(previousCamera.View(), previousCamera.Projection()), previousEye, BuildCameraRelativeInverseViewProjection(translatedCamera.View(), translatedCamera.Projection()), translatedEye));
}

ACS_TEST(VolumetricClouds,
         ViewCutDetectionRejectsTeleportsAndAbruptOrientationChanges) {
    CCamera previousCamera;
    previousCamera.SetPerspective(60.0f * kDeg2Rad, 16.0f / 9.0f,
                                  0.05f, 250000.0f);
    const FVec3 previousEye{0.0f, 8.0f, 0.0f};
    previousCamera.SetLookAt(
        previousEye, FVec3{0.0f, 8.0f, 1.0f});
    const FMat4 previousInverse = BuildCameraRelativeInverseViewProjection(previousCamera.View(), previousCamera.Projection());

    CCamera smallRotation;
    smallRotation.SetPerspective(60.0f * kDeg2Rad, 16.0f / 9.0f,
                                 0.05f, 250000.0f);
    smallRotation.SetLookAt(
        previousEye,
        FVec3{Sin(5.0f * kDeg2Rad), 8.0f,
              Cos(5.0f * kDeg2Rad)});
    EXPECT_FALSE(VolumetricCloudViewCutDetected(previousInverse, previousEye, BuildCameraRelativeInverseViewProjection(smallRotation.View(), smallRotation.Projection()), previousEye));

    CCamera abruptRotation;
    abruptRotation.SetPerspective(60.0f * kDeg2Rad, 16.0f / 9.0f,
                                  0.05f, 250000.0f);
    abruptRotation.SetLookAt(
        previousEye,
        FVec3{Sin(35.0f * kDeg2Rad), 8.0f,
              Cos(35.0f * kDeg2Rad)});
    EXPECT_TRUE(VolumetricCloudViewCutDetected(previousInverse, previousEye, BuildCameraRelativeInverseViewProjection(abruptRotation.View(), abruptRotation.Projection()), previousEye));

    const FVec3 teleportedEye{400.0f, 8.0f, 0.0f};
    CCamera teleportedCamera;
    teleportedCamera.SetPerspective(60.0f * kDeg2Rad, 16.0f / 9.0f,
                                    0.05f, 250000.0f);
    teleportedCamera.SetLookAt(
        teleportedEye, FVec3{400.0f, 8.0f, 1.0f});
    EXPECT_TRUE(VolumetricCloudViewCutDetected(previousInverse, previousEye, BuildCameraRelativeInverseViewProjection(teleportedCamera.View(), teleportedCamera.Projection()), teleportedEye));
}

ACS_TEST(VolumetricClouds,
         ViewCutAndSunDirectionRejectNonFiniteInputs) {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    FMat4 invalidMatrix = FMat4::Identity();
    invalidMatrix.m[1][2] = nan;
    EXPECT_TRUE(VolumetricCloudViewCutDetected(
        FMat4::Identity(), FVec3{}, invalidMatrix, FVec3{}));
    EXPECT_TRUE(VolumetricCloudViewCutDetected(
        FMat4::Identity(), FVec3{}, FMat4::Identity(),
        FVec3{nan, 0.0f, 0.0f}));
    FMat4 singularMatrix = FMat4::Identity();
    for (u32 row = 0u; row < 4u; ++row) {
        for (u32 column = 0u; column < 4u; ++column) {
            singularMatrix.m[row][column] = 0.0f;
        }
    }
    EXPECT_TRUE(VolumetricCloudViewCutDetected(
        FMat4::Identity(), FVec3{}, singularMatrix, FVec3{}));

    CSky sky;
    sky.SetSunDirection(FVec3{nan, 1.0f, 0.0f});
    ExpectVec3Near(sky.SunDirection(), FVec3{0.0f, 1.0f, 0.0f}, 0.0f);
}

ACS_TEST(VolumetricClouds,
         RuntimeSanitizesFrameInputsWithoutMatrixTranslationInvalidation) {
    const std::string source = ReadSkySource();
    const std::string compact = CompactShader(source);

    EXPECT_TRUE(Contains(compact, "if(!IsFiniteCloudMatrix(camera_relative_inv_view_proj)||!IsFiniteCloudVector(cam_pos)){"));
    EXPECT_TRUE(Contains(compact, "constFMat4cameraRelativeViewProj=Inverse(camera_relative_inv_view_proj);if(!IsFiniteCloudMatrix(cameraRelativeViewProj)){"));
    EXPECT_TRUE(Contains(compact, "constf32finiteTime=std::isfinite(time)?time:fallbackTime;"));
    EXPECT_TRUE(Contains(compact, "constf32safeTime=finiteTime<-10000000.0f?-10000000.0f:"));
    EXPECT_TRUE(Contains(compact, "VolumetricCloudViewCutDetected(m_PrevCameraRelativeInvViewProj,m_PrevCamPos,camera_relative_inv_view_proj,cam_pos)"));
    EXPECT_FALSE(Contains(compact, "matrixDelta>0.35f"));
    EXPECT_TRUE(Contains(
        compact,
        "recorded.previous_camera_relative_inverse_view_projection="
        "camera_relative_inv_view_proj;"));
    EXPECT_TRUE(Contains(
        compact,
        "m_PrevCameraRelativeInvViewProj="
        "recorded.previous_camera_relative_inverse_view_projection;"));
    EXPECT_TRUE(Contains(
        compact, "recorded.previous_sun_color=safeSunColor;"));
    EXPECT_TRUE(Contains(
        compact, "recorded.previous_sky_color=safeSkyColor;"));
    EXPECT_TRUE(Contains(
        compact, "m_PrevSunColor=recorded.previous_sun_color;"));
    EXPECT_TRUE(Contains(
        compact, "m_PrevSkyColor=recorded.previous_sky_color;"));
}
