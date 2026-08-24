// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "render/IRhiDevice.h"
#include "render/Sky.h"
#include "editor_abi/EditorRenderPolicy.h"
#include "math/Camera.h"
#include "math/Math.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

using namespace acs;

namespace {

FVec3 NormalizeForTest(FVec3 v) noexcept {
    const f32 invLen = 1.0f / Sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return FVec3{v.x * invLen, v.y * invLen, v.z * invLen};
}

FVec3 PointOnRay(FVec3 origin, FVec3 direction, f32 distance) noexcept {
    return FVec3{origin.x + direction.x * distance,
                 origin.y + direction.y * distance,
                 origin.z + direction.z * distance};
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
        layer.horizontal_noise_scale * 0.006f;
    out.shape_scale =
        authoredShapeScale < 0.00012f
            ? 0.00012f
            : (authoredShapeScale > 0.00045f
                   ? 0.00045f
                   : authoredShapeScale);
    const f32 layerHeight =
        (layer.top_height - layer.base_height) > 1.0e-4f
            ? layer.top_height - layer.base_height
            : 1.0e-4f;
    out.inverse_layer_height = 1.0f / layerHeight;
    return out;
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

u32 CloudJitterHash2DForTest(u32 pixelX, u32 pixelY) noexcept {
    u32 state = pixelX * 747796405u + pixelY * 2891336453u + 277803737u;
    const u32 word =
        ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

f64 CloudJitter01ForTest(u32 pixelX, u32 pixelY) noexcept {
    return static_cast<f64>(
               CloudJitterHash2DForTest(pixelX, pixelY) >> 8u) /
           16777216.0;
}

f64 CloudJitterForTest(
    u32 pixelX, u32 pixelY, u32 frame, bool temporalSuperResolution) noexcept {
    const u32 sequence = temporalSuperResolution ? (frame >> 4u) : frame;
    const u32 jitterX = pixelX + (sequence * 47u) % 131u;
    const u32 jitterY = pixelY + (sequence * 17u) % 127u;
    const f64 unwrapped =
        CloudJitter01ForTest(jitterX, jitterY) +
        static_cast<f64>(sequence) * 0.754877666;
    return unwrapped - std::floor(unwrapped);
}

f32 CloudRefinedSampleTForTest(f32 intervalStart, f32 coarseProbeT, f32 fineStep, f32 coarseStep, f32 jitter) noexcept {
    const f32 candidateStart = coarseProbeT - coarseStep;
    const f32 coarseCellStart =
        candidateStart > intervalStart ? candidateStart : intervalStart;
    return coarseCellStart + jitter * fineStep;
}

// 細密標本が代表する区間と、その区間内で密度を採取する位置。
struct FCloudFineSampleForTest {
    // 担当区間の始点。
    f32 cell_start = 0.0f;
    // 担当区間内の標本位置。
    f32 sample_t = 0.0f;
    // 光学的深さへ掛ける担当区間の長さ。
    f32 step_length = 0.0f;
    // 雲層の積分範囲内に担当区間が存在するか。
    bool valid = false;
};

// 乱数位相付きの標本位置から担当区間を復元し、末尾の端数区間へ位相を収める。
FCloudFineSampleForTest ResolveCloudFineSampleForTest(f32 intervalStart, f32 intervalEnd, f32 phaseSampleT, f32 fineStep, f32 jitter) noexcept {
    const f32 finePhaseOffset = jitter * fineStep;
    const f32 candidateStart = phaseSampleT - finePhaseOffset;
    const f32 cellStart =
        candidateStart > intervalStart ? candidateStart : intervalStart;
    if (cellStart >= intervalEnd) return {};
    const f32 remainingLength = intervalEnd - cellStart;
    const f32 stepLength =
        remainingLength < fineStep ? remainingLength : fineStep;
    return FCloudFineSampleForTest{cellStart, cellStart + jitter * stepLength, stepLength, true};
}

f32 CloudHenyeyGreensteinForTest(f32 cosine, f32 anisotropy) noexcept {
    const f32 anisotropySquared = anisotropy * anisotropy;
    const f32 denominator = std::pow(
        (1.0f + anisotropySquared -
         2.0f * anisotropy * cosine) > 0.001f
            ? (1.0f + anisotropySquared -
               2.0f * anisotropy * cosine)
            : 0.001f,
        1.5f);
    return (1.0f - anisotropySquared) /
           (12.566370f * denominator);
}

f32 DefaultCloudPhaseForTest(f32 cosine) noexcept {
    const FVolumetricCloudLighting lighting{};
    const f32 clampedCosine =
        cosine < -1.0f ? -1.0f : (cosine > 1.0f ? 1.0f : cosine);
    const f32 phase =
        4.0f *
        (CloudHenyeyGreensteinForTest(
             clampedCosine, lighting.PhaseForward) *
             lighting.PhaseBlend +
         CloudHenyeyGreensteinForTest(
             clampedCosine, lighting.PhaseBackward) *
             (1.0f - lighting.PhaseBlend));
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

// 作者指定の前方散乱率を、光源までと現在区間の透過率で雲芯側へ弱める。
f32 CloudForwardPhaseWeightForTest(f32 authoredForwardWeight, f32 lightTransmittance, f32 intervalTransmittance) noexcept {
    return SaturateForTest(authoredForwardWeight) *
           SaturateForTest(lightTransmittance) *
           SaturateForTest(intervalTransmittance);
}

// 前方・後方の位相をレイごとに一度だけ求め、標本の深さに対応する混合を再現する。
f32 DepthAwareCloudPhaseForTest(f32 cosine, f32 lightTransmittance, f32 intervalTransmittance) noexcept {
    const FVolumetricCloudLighting lighting{};
    const f32 clampedCosine = cosine < -1.0f ? -1.0f : (cosine > 1.0f ? 1.0f : cosine);
    const f32 forwardPhase = 4.0f * CloudHenyeyGreensteinForTest(clampedCosine, lighting.PhaseForward);
    const f32 backwardPhase = 4.0f * CloudHenyeyGreensteinForTest(clampedCosine, lighting.PhaseBackward);
    const f32 forwardWeight = CloudForwardPhaseWeightForTest(
        lighting.PhaseBlend, lightTransmittance, intervalTransmittance);
    const f32 phase = backwardPhase + (forwardPhase - backwardPhase) * forwardWeight;
    return phase < lighting.PhaseMin
        ? lighting.PhaseMin
        : (phase > lighting.PhaseMax ? lighting.PhaseMax : phase);
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

// 雲頂を上空から見るときだけ加える未解像の拡散散乱を再現する。
f32 CloudTopSurfaceScatterForTest(
    f32 cosine, f32 sunHeight, f32 normalizedHeight,
    f32 surfaceProbability) noexcept {
    return 0.22f * SaturateForTest(sunHeight) *
        SmoothStepForTest(0.35f, 0.90f, normalizedHeight) *
        SaturateForTest(surfaceProbability) *
        SaturateForTest(-cosine);
}

// 詳細体積が基本形状を膨張または侵食できる最大量を求める。
f32 CloudBillowMaximumOffsetForTest(f32 height) noexcept {
    return 0.018f + (0.130f - 0.018f) *
        SmoothStepForTest(0.18f, 0.92f, SaturateForTest(height));
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

// 履歴を使えない採取画素だけを、現在フレームの両側再構成へ戻す条件を表す。
bool CloudTemporalUsesSpatialFallbackForTest(bool temporalSuperResolution, bool scheduled, bool historyAccepted) noexcept {
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

f32 CloudColumnHeightShiftForTest(
    f32 cloudInterior, f32 cloudType, f32 precipitation,
    f32 warp, f32 shapePhaseX, f32 shapePhaseY) noexcept {
    const f32 core = SmoothStepForTest(0.08f, 0.92f, SaturateForTest(cloudInterior));
    const f32 verticalType = SaturateForTest(
        cloudType > precipitation ? cloudType : precipitation);
    const f32 typeTower = SmoothStepForTest(0.72f, 0.98f, cloudType);
    const f32 precipitationTower = SmoothStepForTest(0.25f, 0.85f, precipitation);
    const f32 toweringStrength = typeTower > precipitationTower ? typeTower : precipitationTower;
    f32 amplitude = 0.018f + (0.11f - 0.018f) * verticalType;
    amplitude *= 1.0f + 0.35f * toweringStrength;
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

// 低周波天候模様から、物理層内に収まる柱ごとの雲底持ち上げ量を求める。
f32 CloudColumnBaseLiftForTest(f32 cloudInterior, f32 cloudType, f32 precipitation, f32 warp) noexcept {
    const f32 verticalType = SaturateForTest(cloudType > precipitation ? cloudType : precipitation);
    const f32 broadPattern = SmoothStepForTest(0.18f, 0.82f, warp);
    const f32 edgePattern = 1.0f - SmoothStepForTest(0.08f, 0.86f, SaturateForTest(cloudInterior));
    const f32 typeTower = SmoothStepForTest(0.72f, 0.98f, cloudType);
    const f32 precipitationTower = SmoothStepForTest(0.25f, 0.85f, precipitation);
    const f32 toweringStrength = typeTower > precipitationTower ? typeTower : precipitationTower;
    f32 amplitude = 0.025f + (0.075f - 0.025f) * verticalType;
    amplitude *= 1.0f + 0.40f * toweringStrength;
    const f32 signal = SaturateForTest(0.08f + broadPattern * 0.62f + edgePattern * 0.30f);
    return amplitude * signal;
}

f32 CloudAnvilCoverageExpansionForTest(
    f32 layerHeight, f32 cloudType, f32 precipitation) noexcept {
    const f32 typeTower = SmoothStepForTest(0.72f, 0.98f, cloudType);
    const f32 precipitationTower = SmoothStepForTest(0.25f, 0.85f, precipitation);
    const f32 toweringStrength = typeTower > precipitationTower ? typeTower : precipitationTower;
    const f32 anvilBand = SmoothStepForTest(0.50f, 0.66f, SaturateForTest(layerHeight)) *
        (1.0f - SmoothStepForTest(0.80f, 0.97f, SaturateForTest(layerHeight)));
    return 0.16f * toweringStrength * anvilBand;
}

// 積乱雲の本体・肩・かなとこを同じ密度で埋めず、上部だけを横へ広げる形状を再現する。
f32 CloudStormProfileForTest(f32 height, f32 cloudType, f32 precipitation) noexcept {
    const f32 typeWeight = SmoothStepForTest(0.50f, 0.84f, cloudType);
    const f32 precipitationTower = SmoothStepForTest(0.25f, 0.85f, precipitation);
    const f32 typeTower = SmoothStepForTest(0.72f, 0.98f, cloudType);
    const f32 toweringStrength = typeTower > precipitationTower ? typeTower : precipitationTower;
    const f32 stormBody = SmoothStepForTest(0.045f, 0.16f, height) *
        (1.0f - SmoothStepForTest(0.40f, 0.70f, height));
    const f32 stormShoulder = SmoothStepForTest(0.20f, 0.34f, height) *
        (1.0f - SmoothStepForTest(0.46f, 0.66f, height)) * 0.62f;
    const f32 anvil = SmoothStepForTest(0.56f, 0.70f, height) *
        (1.0f - SmoothStepForTest(0.85f, 0.995f, height)) * 0.82f;
    const f32 storm = stormBody > stormShoulder ? stormBody :
        (stormShoulder > anvil ? stormShoulder : anvil);
    const f32 stormMix = precipitation * 0.72f > toweringStrength * 0.92f ?
        precipitation * 0.72f : toweringStrength * 0.92f;
    const f32 baseProfile = typeWeight * SmoothStepForTest(0.13f, 0.62f, height);
    return (1.0f - stormMix) * baseProfile + stormMix * storm;
}

// 積乱雲の中層被覆だけを細くし、かなとこの横張り出しは維持する。
f32 CloudConvectiveWaistScaleForTest(
    f32 height, f32 cloudType, f32 precipitation) noexcept {
    const f32 precipitationTower = SmoothStepForTest(0.25f, 0.85f, precipitation);
    const f32 typeTower = SmoothStepForTest(0.72f, 0.98f, cloudType);
    const f32 toweringStrength = typeTower > precipitationTower ? typeTower : precipitationTower;
    const f32 waist = SmoothStepForTest(0.28f, 0.44f, height) *
        (1.0f - SmoothStepForTest(0.58f, 0.74f, height));
    return SaturateForTest(1.0f - 0.34f * toweringStrength * waist);
}

// 積乱雲の中層だけ低密度の尾を締め、かなとこの薄い縁は残す。
f32 CloudConvectiveBodyDensityForTest(
    f32 baseDensity, f32 height, f32 cloudType, f32 precipitation) noexcept {
    const f32 density = SaturateForTest(baseDensity);
    const f32 precipitationTower = SmoothStepForTest(0.25f, 0.85f, precipitation);
    const f32 typeTower = SmoothStepForTest(0.72f, 0.98f, cloudType);
    const f32 toweringStrength = typeTower > precipitationTower ? typeTower : precipitationTower;
    const f32 bodyBand = SmoothStepForTest(0.08f, 0.28f, SaturateForTest(height)) *
        (1.0f - SmoothStepForTest(0.56f, 0.76f, SaturateForTest(height)));
    const f32 tightening = 0.58f * toweringStrength * bodyBand;
    return density + (density * density - density) * tightening;
}

f32 CloudWeatherMaskForLayerForTest(
    f32 weatherCoverage, f32 threshold, f32 inverseTransitionWidth,
    f32 layerHeight, f32 cloudType, f32 precipitation) noexcept {
    const f32 baseT = SaturateForTest(
        (weatherCoverage - threshold) * inverseTransitionWidth);
    const f32 baseMask = baseT * baseT * (3.0f - 2.0f * baseT);
    const f32 anvilThreshold = threshold - CloudAnvilCoverageExpansionForTest(
        layerHeight, cloudType, precipitation);
    const f32 anvilT = SaturateForTest(
        (weatherCoverage - anvilThreshold) * inverseTransitionWidth);
    const f32 anvilMask = anvilT * anvilT * (3.0f - 2.0f * anvilT);
    return baseMask > anvilMask ? baseMask : anvilMask;
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

f32 CloudConvectiveHeightForTest(f32 height, f32 columnShift, bool upperBand) noexcept {
    const f32 boundedHeight = SaturateForTest(height);
    constexpr f32 kCondensationHeight = 0.14f;
    const f32 upperHeight = SaturateForTest((boundedHeight - kCondensationHeight) / (1.0f - kCondensationHeight));
    const f32 upperRemainder = 1.0f - upperHeight;
    const f32 upperHeightSquared = upperHeight * upperHeight;
    const f32 upperInterior = 45.5625f * upperHeightSquared * upperHeightSquared * upperRemainder * upperRemainder;
    const f32 bandScale = upperBand ? 0.30f : 1.0f;
    const f32 shiftedUpper = SaturateForTest(upperHeight - columnShift * upperInterior * bandScale);
    return boundedHeight <= kCondensationHeight ? boundedHeight : kCondensationHeight + (1.0f - kCondensationHeight) * shiftedUpper;
}

// 低周波形状を雲体側へ満たし、高さ形状で支えた密度を求める。
f32 CloudDimensionalDensityForTest(f32 baseDensity, f32 heightProfile) noexcept {
    return Sqrt(SaturateForTest(baseDensity)) *
        SaturateForTest(heightProfile);
}

// 局所雲底より上の高さだけを再配置し、既存の対流形状を適用する。
f32 CloudColumnHeightForTest(f32 height, f32 columnShift, f32 baseLift, bool upperBand) noexcept {
    const f32 boundedHeight = SaturateForTest(height);
    const f32 bandScale = upperBand ? 0.35f : 1.0f;
    const f32 localBase = SaturateForTest(baseLift * bandScale);
    const f32 remainingHeight =
        1.0f - localBase > 0.001f ? 1.0f - localBase : 0.001f;
    const f32 localHeight = SaturateForTest((boundedHeight - localBase) / remainingHeight);
    return CloudConvectiveHeightForTest(localHeight, columnShift, upperBand);
}

// 高さ分布を途中で飽和させず、上下端を中心より強く絞る形状重みを求める。
f32 CloudVerticalProfileShapeForTest(f32 sampledProfile) noexcept {
    const f32 profile = SaturateForTest(sampledProfile);
    return profile * Sqrt(profile);
}

// 2D 天候場の境界から 3D 基本形状を削る量を求める。
f32 CloudWeatherShapeErosionForTest(f32 weatherMask) noexcept {
    const f32 edge = 1.0f - SaturateForTest(weatherMask);
    const f32 edgeSquared = edge * edge;
    return edgeSquared * edge * 0.65f;
}

// 2D天候境界を層の高さごとにずらすS字変形を、シェーダーと同じ式で求める。
f32 CloudWeatherVerticalBendForTest(
    f32 layerHeight, bool upperBand) noexcept {
    const f32 centeredHeight = SaturateForTest(layerHeight) * 2.0f - 1.0f;
    const f32 bend = centeredHeight *
        (0.35f + (1.0f - 0.35f) * Abs(centeredHeight));
    return bend * (upperBand ? 0.25f : 1.0f);
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

// レイの採取間隔から、高周波の侵食形状を安全に採取できる割合を求める。
f32 CloudErosionVisibilityFromSampleSpacingForTest(f32 sampleSpacing) noexcept {
    return CloudDetailFrequencyVisibilityForTest(sampleSpacing, 16.0f, 0.05f, 0.24f);
}

// 積分間隔と距離に応じた投影画素幅から、形状を安全に採取できる実幅を求める。
f32 CloudProjectedSampleSpacingForTest(f32 integrationSpacing, f32 sampleDistance, f32 angularPixelFootprint) noexcept {
    const f32 boundedSpacing = integrationSpacing > 0.0f ? integrationSpacing : 0.0f;
    const f32 boundedDistance = sampleDistance > 0.0f ? sampleDistance : 0.0f;
    const f32 boundedFootprint = angularPixelFootprint > 0.0f ? angularPixelFootprint : 0.0f;
    const f32 projectedPixelWidth = boundedDistance * boundedFootprint;
    return boundedSpacing > projectedPixelWidth ? boundedSpacing : projectedPixelWidth;
}

// 基本形状の一周期に対する採取間隔から、その帯域を安全に残せる割合を求める。
f32 CloudShapeFrequencyVisibilityForTest(
    f32 sampleSpacing, f32 shapeScale,
    f32 domainScale, f32 frequency) noexcept {
    const f32 boundedSpacing = sampleSpacing > 0.0f ? sampleSpacing : 0.0f;
    const f32 footprint =
        boundedSpacing * shapeScale * domainScale * frequency;
    return 1.0f - SmoothStepForTest(0.22f, 0.52f, footprint);
}

// 天候中心による雲体の広がりをシェーダーと同じ式で求める。
f32 CloudWeatherCoreShapeOffsetForTest(
    f32 weatherMask, f32 height, bool upperBand) noexcept {
    const f32 core = SmoothStepForTest(
        0.18f, 0.82f, SaturateForTest(weatherMask));
    const f32 boundedHeight = SaturateForTest(height);
    const f32 body = SmoothStepForTest(0.10f, 0.30f, boundedHeight) *
        (1.0f - SmoothStepForTest(0.82f, 0.98f, boundedHeight));
    return core * body * (upperBand ? 0.015f : 0.030f);
}

// 視線区間ごとの採取位相を求め、参照描画だけは区間中央へ固定する。
f32 CloudRayIntervalPhaseForTest(f32 basePhase, u32 intervalIndex, bool referenceMode) noexcept {
    if (referenceMode) return 0.5f;
    const f32 unfolded = basePhase + static_cast<f32>(intervalIndex) * 0.41421356237f;
    return unfolded - std::floor(unfolded);
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
    EXPECT_EQ(steady.shadow_cache_logical_invocations, 73728u);
    EXPECT_EQ(steady.shadow_cache_launched_threads, 2304u);
    EXPECT_EQ(steady.world_shadow_logical_invocations, 16384u);
    EXPECT_EQ(steady.world_shadow_launched_threads, 16384u);
    EXPECT_EQ(steady.total_logical_invocations, 2293312u);
    EXPECT_EQ(steady.total_launched_threads, 2222848u);
    EXPECT_EQ(steady.maximum_view_samples, 49766400u);
    EXPECT_EQ(steady.maximum_light_samples, 398131200u);
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
    EXPECT_EQ(reference.maximum_view_samples, 66355200u);
    EXPECT_EQ(reference.maximum_light_samples, 530841600u);
    EXPECT_EQ(reference.shadow_cache_logical_invocations, 294912u);
    EXPECT_EQ(reference.shadow_cache_launched_threads, 9216u);
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
    EXPECT_EQ(cold.one_time_bake_dispatches, 4u);
    EXPECT_EQ(cold.shadow_cache_dispatches, 1u);
    EXPECT_EQ(cold.world_shadow_dispatches, 1u);
    EXPECT_EQ(cold.total_compute_dispatches, 8u);
    EXPECT_EQ(cold.one_time_bake_logical_invocations, 2637824u);
    EXPECT_EQ(cold.one_time_bake_launched_threads, 2637824u);
    EXPECT_EQ(cold.shadow_cache_logical_invocations, 294912u);
    EXPECT_EQ(cold.shadow_cache_launched_threads, 9216u);
    EXPECT_EQ(cold.world_shadow_logical_invocations, 65536u);
    EXPECT_EQ(cold.world_shadow_launched_threads, 65536u);
    EXPECT_EQ(cold.total_logical_invocations, 5201472u);
    EXPECT_EQ(cold.total_launched_threads, 4916736u);
    EXPECT_EQ(cold.maximum_view_samples, steady.maximum_view_samples);
    EXPECT_EQ(cold.maximum_light_samples, steady.maximum_light_samples);
    EXPECT_EQ(cold.maximum_world_shadow_samples, 2097152u);
}

ACS_TEST(VolumetricClouds,
         WorkloadDiagnosticsAccountForPaddingAndSaturateHostileSizes) {
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
    EXPECT_EQ(unsupportedShadow.shadow_cache_logical_invocations, 294912u);
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
    EXPECT_TRUE(Contains(compact, "m_ShadowCacheValid=false;m_WorldShadowValid=false;m_LastFrameWorkload.skip_reason=EVolumetricCloudFrameSkipReason::ResourcesNotReady;return;"));
    EXPECT_TRUE(CountOccurrences(compact, "m_ShadowCacheValid=false;m_WorldShadowValid=false;") >= static_cast<std::size_t>(3));
    EXPECT_TRUE(Contains(
        compact,
        "m_LastFrameWorkload="
        "PlanVolumetricCloudFrameWorkload(workloadPlan);"));
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
    EXPECT_TRUE(Contains(
        compact,
        "m_LastFrameWorkload.submitted=true;"));
    EXPECT_TRUE(Contains(
        compact,
        "if(m_LastFrameWorkload.submitted&&"
        "m_LastFrameWorkload.composite_draws!=~u32{0}){"));
    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 384u);
    EXPECT_EQ(kVolumetricCloudMaxLightMarchSamples, 8u);
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
    EXPECT_NEAR(production.fine_step * 2.0f, former.fine_step, 1e-4f);
    EXPECT_NEAR(production.coarse_step * 2.0f, former.coarse_step, 1e-4f);
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
    EXPECT_TRUE(Contains(shader, "floatdistanceFade=cloudDistanceFade(sampleT,fadeStart,MAX_DISTANCE);"));
    EXPECT_TRUE(Contains(shader, "*density*distanceFade;"));
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

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(Contains(
        shader,
        "intMAX_STEPS=(int)cloudLightingAmbient.z;"
        "if(MAX_STEPS<32)MAX_STEPS=32;"));
    EXPECT_TRUE(Contains(shader, "floatspan=t1-t0;"));
    EXPECT_TRUE(Contains(
        shader, "floatbaseFineStep=cloudCoverageReciprocals.z;"));
    EXPECT_TRUE(Contains(shader, "intfineSampleBudget=MAX_STEPS-(MAX_STEPS>>3);"));
    EXPECT_TRUE(Contains(shader, "intcoarseSampleBudget=max(fineSampleBudget>>1,1);"));
    EXPECT_TRUE(Contains(shader, "floatfineStep=max(baseFineStep,span/float(fineSampleBudget));"));
    EXPECT_TRUE(Contains(shader, "floatcoarseStep=max(fineStep*2.0,span/float(coarseSampleBudget));"));
    EXPECT_FALSE(Contains(shader, "span/168.0"));
    EXPECT_FALSE(Contains(shader, "span/84.0"));
    EXPECT_FALSE(Contains(shader, "constintMAX_STEPS=128;"));
}

ACS_TEST(VolumetricClouds, CoarseOccupancyRewindPreservesTheFineSamplePhase) {
    constexpr f32 kIntervalStart = 100.0f;
    constexpr f32 kFineStep = 6.0f;
    constexpr f32 kCoarseStep = 12.0f;

    // 最初の粗い区間で検出した場合も、参照描画は細密区間の中央から始める。
    EXPECT_NEAR(CloudRefinedSampleTForTest(kIntervalStart, 106.0f, kFineStep, kCoarseStep, 0.5f), 103.0f, 1e-6f);
    // 後続区間では一つ前の粗い区間へ戻し、同じ細密位相を保つ。
    EXPECT_NEAR(CloudRefinedSampleTForTest(kIntervalStart, 154.0f, kFineStep, kCoarseStep, 0.5f), 145.0f, 1e-6f);
    for (u32 phaseStep = 0u; phaseStep < 100u; ++phaseStep) {
        const f32 jitter = static_cast<f32>(phaseStep) / 100.0f;
        const f32 refined = CloudRefinedSampleTForTest(kIntervalStart, 154.0f, kFineStep, kCoarseStep, jitter);
        EXPECT_TRUE(refined >= 142.0f);
        EXPECT_TRUE(refined <= 148.0f);
        EXPECT_NEAR(refined, 142.0f + jitter * kFineStep, 2e-5f);
    }

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(shader, "floatcloudRefinedSampleT(" "floatintervalStart,floatcoarseProbeT,floatfineStep," "floatcoarseStep,floatjitter){" "floatcoarseCellStart=max(coarseProbeT-coarseStep,intervalStart);" "returncoarseCellStart+jitter*fineStep;}"));
    EXPECT_TRUE(Contains(shader, "floatcoarseProbeT=t;" "refineUntil=coarseProbeT+coarseStep;" "t=cloudRefinedSampleT(" "t0,coarseProbeT,fineStep,coarseStep,jit);"));
    EXPECT_FALSE(Contains(shader, "t=max(t-coarseStep,t0);"));
}

ACS_TEST(VolumetricClouds, InteriorMarchChecksTheCameraAdjacentFineInterval) {
    constexpr f32 kInsideEnter = 0.0f;
    constexpr f32 kInsideExit = 100.0f;
    constexpr f32 kOutsideEnter = 100.0f;
    constexpr f32 kFineStep = 6.0f;
    constexpr f32 kCoarseStep = 12.0f;

    for (u32 phaseStep = 0u; phaseStep <= 100u; ++phaseStep) {
        const f32 jitter = static_cast<f32>(phaseStep) / 100.0f;
        const bool startsInside = kInsideEnter <= 1e-4f;
        const f32 insidePhaseSample =
            kInsideEnter + jitter * (startsInside ? kFineStep : kCoarseStep);
        const FCloudFineSampleForTest insideInterval = ResolveCloudFineSampleForTest(kInsideEnter, kInsideExit, insidePhaseSample, kFineStep, jitter);
        const bool startsOutside = kOutsideEnter <= 1e-4f;
        const f32 outsideSample = kOutsideEnter + jitter * (startsOutside ? kFineStep : kCoarseStep);
        EXPECT_TRUE(insideInterval.valid);
        EXPECT_NEAR(insideInterval.cell_start, kInsideEnter, 1e-6f);
        EXPECT_NEAR(insideInterval.step_length, kFineStep, 1e-6f);
        EXPECT_TRUE(insideInterval.sample_t >= kInsideEnter);
        EXPECT_TRUE(insideInterval.sample_t <= kInsideEnter + kFineStep);
        EXPECT_TRUE(outsideSample >= kOutsideEnter);
        EXPECT_TRUE(outsideSample <= kOutsideEnter + kCoarseStep);
    }

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(shader, "boolstartsInsideShell=t0<=1e-4;"));
    EXPECT_TRUE(Contains(shader, "floatt=t0+jit*(startsInsideShell?fineStep:coarseStep);"));
    EXPECT_TRUE(Contains(shader, "boolnearDensity=startsInsideShell;"));
    EXPECT_TRUE(Contains(shader, "floatrefineUntil=startsInsideShell?min(t0+coarseStep,t1):t0;"));
    EXPECT_FALSE(Contains(shader, "floatt=t0+jit*coarseStep;boolnearDensity=false;"));
}

ACS_TEST(VolumetricClouds, FineSamplePhaseOwnsTheCompleteIntegrationInterval) {
    constexpr f32 kIntervalStart = 100.0f;
    constexpr f32 kIntervalEnd = 110.0f;
    constexpr f32 kFineStep = 6.0f;
    constexpr f32 kReferencePhase = 0.5f;

    const FCloudFineSampleForTest first = ResolveCloudFineSampleForTest(kIntervalStart, kIntervalEnd, 103.0f, kFineStep, kReferencePhase);
    const FCloudFineSampleForTest last = ResolveCloudFineSampleForTest(kIntervalStart, kIntervalEnd, 109.0f, kFineStep, kReferencePhase);
    const FCloudFineSampleForTest finished = ResolveCloudFineSampleForTest(kIntervalStart, kIntervalEnd, 115.0f, kFineStep, kReferencePhase);
    EXPECT_TRUE(first.valid);
    EXPECT_NEAR(first.cell_start, 100.0f, 1e-6f);
    EXPECT_NEAR(first.sample_t, 103.0f, 1e-6f);
    EXPECT_NEAR(first.step_length, 6.0f, 1e-6f);
    EXPECT_TRUE(last.valid);
    EXPECT_NEAR(last.cell_start, 106.0f, 1e-6f);
    EXPECT_NEAR(last.sample_t, 108.0f, 1e-6f);
    EXPECT_NEAR(last.step_length, 4.0f, 1e-6f);
    EXPECT_FALSE(finished.valid);

    // どの位相でも各標本の担当区間を合計すると、積分区間の全長になる。
    constexpr f32 kPhases[]{0.0f, 0.25f, 0.5f, 0.9f};
    for (const f32 phase : kPhases) {
        f32 phaseSampleT = kIntervalStart + phase * kFineStep;
        f32 integratedLength = 0.0f;
        for (u32 sampleIndex = 0u; sampleIndex < 4u; ++sampleIndex) {
            const FCloudFineSampleForTest sample = ResolveCloudFineSampleForTest(kIntervalStart, kIntervalEnd, phaseSampleT, kFineStep, phase);
            if (!sample.valid) break;
            EXPECT_TRUE(sample.sample_t >= sample.cell_start);
            EXPECT_TRUE(sample.sample_t <= sample.cell_start + sample.step_length);
            integratedLength += sample.step_length;
            phaseSampleT += kFineStep;
        }
        EXPECT_NEAR(integratedLength, kIntervalEnd - kIntervalStart, 2e-5f);
    }

    // 一様密度なら区間中央採取の重み付き代表深度は、積分区間全体の中央と一致する。
    const f32 integratedLength = first.step_length + last.step_length;
    const f32 meanDepth = (first.sample_t * first.step_length + last.sample_t * last.step_length) / integratedLength;
    EXPECT_NEAR(meanDepth, 105.0f, 1e-6f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(shader, "floatfinePhaseOffset=jit*fineStep;"));
    EXPECT_TRUE(Contains(shader, "[loop]for(inti=0;i<MAX_STEPS;i++){"));
    EXPECT_TRUE(Contains(shader, "elseif(t>=t1){break;}"));
    EXPECT_TRUE(Contains(shader, "floatfineCellStart=max(t-finePhaseOffset,t0);" "if(fineCellStart>=t1)break;" "stepLength=min(fineStep,t1-fineCellStart);" "floatintervalPhase=cloudRayIntervalPhase(jit,i);" "sampleT=fineCellStart+intervalPhase*stepLength;"));
    EXPECT_TRUE(Contains(shader, "float3p=camPos.xyz+dir*sampleT;"));
    EXPECT_TRUE(Contains(shader, "depthMoment+=sampleWeight*sampleT;"));
    EXPECT_TRUE(Contains(shader, "t+=fineStep;"));
    EXPECT_FALSE(Contains(shader, "MAX_STEPS&&t<t1"));
    EXPECT_FALSE(Contains(shader, "t+stepLength*0.5"));
    EXPECT_FALSE(Contains(shader, "stepLength=min(fineStep,t1-t)"));
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
    EXPECT_NEAR(CloudErosionVisibilityFromSampleSpacingForTest(vertical.fine_step), 1.0f, 0.0f);
    EXPECT_TRUE(CloudBillowVisibilityFromSampleSpacingForTest(horizon.fine_step) > 0.90f);
    EXPECT_NEAR(CloudErosionVisibilityFromSampleSpacingForTest(horizon.fine_step), 0.0f, 0.0f);
    EXPECT_TRUE(CloudBillowVisibilityFromSampleSpacingForTest(horizonNormal.fine_step) > 0.70f);
    EXPECT_TRUE(CloudBillowVisibilityFromSampleSpacingForTest(horizonNormal.fine_step) < CloudBillowVisibilityFromSampleSpacingForTest(horizon.fine_step));
    EXPECT_NEAR(CloudErosionVisibilityFromSampleSpacingForTest(horizonNormal.fine_step), 0.0f, 0.0f);
    EXPECT_NEAR(CloudBillowVisibilityFromSampleSpacingForTest(270.1613f), 0.5f, 1e-5f);
    EXPECT_NEAR(CloudErosionVisibilityFromSampleSpacingForTest(29.23387f), 0.5f, 1e-5f);

    EXPECT_NEAR(CloudProjectedSampleSpacingForTest(12.0f, 0.0f, 0.001f), 12.0f, 0.0f);
    EXPECT_NEAR(CloudProjectedSampleSpacingForTest(12.0f, 8000.0f, 0.001f), 12.0f, 0.0f);
    EXPECT_NEAR(CloudProjectedSampleSpacingForTest(12.0f, 30000.0f, 0.001f), 30.0f, 1e-5f);
    EXPECT_NEAR(CloudProjectedSampleSpacingForTest(12.0f, 60000.0f, 0.001f), 60.0f, 1e-5f);
    EXPECT_NEAR(CloudProjectedSampleSpacingForTest(-1.0f, -2.0f, -3.0f), 0.0f, 0.0f);

    f32 previousBillowVisibility = 1.0f;
    f32 previousErosionVisibility = 1.0f;
    for (u32 spacingStep = 0u; spacingStep <= 700u; spacingStep += 5u) {
        const f32 billowVisibility = CloudBillowVisibilityFromSampleSpacingForTest(static_cast<f32>(spacingStep));
        const f32 erosionVisibility = CloudErosionVisibilityFromSampleSpacingForTest(static_cast<f32>(spacingStep));
        EXPECT_TRUE(billowVisibility <= previousBillowVisibility + 1e-6f);
        EXPECT_TRUE(erosionVisibility <= previousErosionVisibility + 1e-6f);
        EXPECT_TRUE(billowVisibility >= 0.0f && billowVisibility <= 1.0f);
        EXPECT_TRUE(erosionVisibility >= 0.0f && erosionVisibility <= 1.0f);
        EXPECT_TRUE(billowVisibility + 1e-6f >= erosionVisibility);
        previousBillowVisibility = billowVisibility;
        previousErosionVisibility = erosionVisibility;
    }

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
        "sampleSpacing,16.0,0.05,0.24);"));
    EXPECT_TRUE(Contains(shader, "detailDomainA=horizontal*0.00018+vertical*0.00014;"));
    EXPECT_TRUE(Contains(shader, "detailDomainB=horizontal*0.00031+vertical*0.00024;"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudProjectedSampleSpacing("
        "floatintegrationSpacing,floatsampleDistance,floatangularPixelFootprint){"
        "floatprojectedPixelWidth=max(sampleDistance,0.0)*"
        "max(angularPixelFootprint,0.0);"
        "returnmax(max(integrationSpacing,0.0),projectedPixelWidth);}"));
    EXPECT_TRUE(Contains(
        shader,
        "floatprojectedSampleSpacing=cloudProjectedSampleSpacing("
        "fineStep,sampleT,angularPixelFootprint);"));
    EXPECT_TRUE(Contains(
        shader,
        "float3xPixelDirection=CloudViewDirection("
        "float2(xUv.x*2.0-1.0,-(xUv.y*2.0-1.0)));"
        "float3yPixelDirection=CloudViewDirection("
        "float2(yUv.x*2.0-1.0,-(yUv.y*2.0-1.0)));"
        "floatangularPixelFootprint=max("
        "length(xPixelDirection-dir),length(yPixelDirection-dir));"));
    EXPECT_TRUE(Contains(shader, "floatbillowVisibility=cloudBillowVisibilityFromSampleSpacing(projectedSampleSpacing);"));
    EXPECT_TRUE(Contains(shader, "floaterosionVisibility=cloudErosionVisibilityFromSampleSpacing(projectedSampleSpacing);"));
    EXPECT_FALSE(Contains(shader, "cloudBillowVisibilityFromSampleSpacing(stepLength)"));
    EXPECT_FALSE(Contains(shader, "cloudErosionVisibilityFromSampleSpacing(stepLength)"));
    EXPECT_FALSE(Contains(shader, "floatdetailStart=12000.0;"));
    EXPECT_FALSE(Contains(shader, "floatdetailEnd=80000.0;"));
    const std::size_t detailBranch = shader.find("[branch]if(detailVisibility>0.001){");
    const std::size_t firstDetailRead = shader.find("detailNoise.SampleLevel(", detailBranch);
    const std::size_t billowBlend = shader.find("floatbillowedDensity=lerp(coarseDensity,billowedCoarseDensity,billowVisibility);", firstDetailRead);
    const std::size_t erosionBlend = shader.find("d=lerp(billowedDensity,eroded,erosionVisibility);", firstDetailRead);
    EXPECT_TRUE(detailBranch != std::string::npos);
    EXPECT_TRUE(firstDetailRead != std::string::npos);
    EXPECT_TRUE(billowBlend != std::string::npos);
    EXPECT_TRUE(erosionBlend != std::string::npos);
    EXPECT_TRUE(detailBranch < firstDetailRead);
    EXPECT_TRUE(firstDetailRead < billowBlend);
    EXPECT_TRUE(billowBlend < erosionBlend);
    EXPECT_TRUE(Contains(shader, "floatlightBillowVisibility=cloudBillowVisibilityFromSampleSpacing(lightStep);"));
    EXPECT_TRUE(Contains(shader, "floatlightErosionVisibility=cloudErosionVisibilityFromSampleSpacing(lightStep);"));
    EXPECT_TRUE(Contains(shader, "cloudDensityFromMacro(lp,lightMacro,lightMacro.heightThreshold,lightMacro.weatherMask,lightBillowVisibility,lightErosionVisibility);"));
    EXPECT_FALSE(Contains(shader, "lightMacro.weatherMask,0.65,1.0);"));
}

ACS_TEST(VolumetricClouds, BaseShapeLodRejectsUnresolvableFrequencies) {
    constexpr f32 kShapeScale = 0.035f * 0.006f;
    constexpr f32 kDomainScales[]{1.0f, 1.83f, 3.17f, 4.73f};
    f32 previousFine[4]{1.0f, 1.0f, 1.0f, 1.0f};
    f32 previousDomain[4]{1.0f, 1.0f, 1.0f, 1.0f};
    for (u32 spacingStep = 0u; spacingStep <= 700u; spacingStep += 5u) {
        for (u32 domain = 0u; domain < 4u; ++domain) {
            const f32 fineVisibility =
                CloudShapeFrequencyVisibilityForTest(
                    static_cast<f32>(spacingStep), kShapeScale,
                    kDomainScales[domain], 32.0f);
            const f32 domainVisibility =
                CloudShapeFrequencyVisibilityForTest(
                    static_cast<f32>(spacingStep), kShapeScale,
                    kDomainScales[domain], 12.0f);
            EXPECT_TRUE(fineVisibility <= previousFine[domain] + 1.0e-6f);
            EXPECT_TRUE(domainVisibility <= previousDomain[domain] + 1.0e-6f);
            EXPECT_TRUE(domainVisibility + 1.0e-6f >= fineVisibility);
            previousFine[domain] = fineVisibility;
            previousDomain[domain] = domainVisibility;
        }
    }
    EXPECT_NEAR(
        CloudShapeFrequencyVisibilityForTest(
            0.0f, kShapeScale, 4.73f, 32.0f),
        1.0f, 0.0f);
    EXPECT_NEAR(
        CloudShapeFrequencyVisibilityForTest(
            500.0f, kShapeScale, 1.83f, 12.0f),
        0.0f, 0.0f);
    EXPECT_TRUE(
        CloudShapeFrequencyVisibilityForTest(
            80.0f, kShapeScale, 1.83f, 12.0f) >
        CloudShapeFrequencyVisibilityForTest(
            80.0f, kShapeScale, 3.17f, 12.0f));

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader,
        "floatfootprint=max(sampleSpacing,0.0)*cloudShapeScale()*"
        "domainScale*frequency;"
        "return1.0-smoothstep(0.22,0.52,footprint);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatfineVisibility=cloudShapeFrequencyVisibility("
        "sampleSpacing,domainScale,32.0);"));
    EXPECT_TRUE(Contains(
        shader,
        "returncloudShapeFrequencyVisibility("
        "sampleSpacing,domainScale,12.0);"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudMacroSamplemacro=sampleCloudMacro("
        "p,coverageTerms,projectedSampleSpacing,"
        "viewMacroUvw,densityHeightThreshold);"));
    EXPECT_TRUE(Contains(
        shader,
        "sampleCloudMacroLighting("
        "p,saturate(params.x),stepLength);"));
}

ACS_TEST(VolumetricClouds, ViewRayIntervalPhasesBreakPeriodicShapeResonance) {
    constexpr u32 kIntervalCount = kVolumetricCloudViewSteps;
    constexpr u32 kPhaseBinCount = 16u;
    constexpr u32 kExpectedPerBin = kIntervalCount / kPhaseBinCount;
    constexpr f32 kBasePhase = 0.37f;
    constexpr f64 kTwoPi = 6.28318530717958647692;
    u32 phaseBins[kPhaseBinCount]{};
    f64 fixedSignalSum = 0.0;
    f64 dispersedSignalSum = 0.0;

    for (u32 interval = 0u; interval < kIntervalCount; ++interval) {
        const f32 phase = CloudRayIntervalPhaseForTest(kBasePhase, interval, false);
        EXPECT_TRUE(phase >= 0.0f && phase < 1.0f);
        const u32 bin = static_cast<u32>(phase * static_cast<f32>(kPhaseBinCount));
        EXPECT_TRUE(bin < kPhaseBinCount);
        if (bin < kPhaseBinCount) ++phaseBins[bin];

        // 区間周期と同じ形状成分では、固定位相は全標本が同じ値となる。
        fixedSignalSum += 0.5 + 0.5 * std::cos(kTwoPi * static_cast<f64>(kBasePhase));
        dispersedSignalSum += 0.5 + 0.5 * std::cos(kTwoPi * static_cast<f64>(phase));
        EXPECT_NEAR(CloudRayIntervalPhaseForTest(kBasePhase, interval, true), 0.5f, 0.0f);
    }

    const f64 fixedAverage = fixedSignalSum / static_cast<f64>(kIntervalCount);
    const f64 dispersedAverage = dispersedSignalSum / static_cast<f64>(kIntervalCount);
    EXPECT_TRUE(std::fabs(fixedAverage - 0.5) > 0.30);
    EXPECT_NEAR(dispersedAverage, 0.5, 0.01);
    for (u32 bin = 0u; bin < kPhaseBinCount; ++bin) {
        EXPECT_TRUE(phaseBins[bin] + 1u >= kExpectedPerBin);
        EXPECT_TRUE(phaseBins[bin] <= kExpectedPerBin + 1u);
    }

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(shader, "floatcloudRayIntervalPhase(floatbasePhase,intintervalIndex){floatsamplePhase=0.5;if(cloudLightingAmbient.w<0.5){samplePhase=frac(basePhase+float(intervalIndex)*0.41421356237);}returnsamplePhase;}"));
    EXPECT_TRUE(Contains(shader, "floatintervalPhase=cloudRayIntervalPhase(jit,i);sampleT=fineCellStart+intervalPhase*stepLength;"));
    EXPECT_FALSE(Contains(shader, "sampleT=fineCellStart+jit*stepLength;"));
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
         TemporalHistoryInvalidatesForEveryLightingInput) {
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

    const std::string source = ReadSkySource();
    EXPECT_TRUE(Contains(
        source, "if (VolumetricCloudLightingChanged("));
    EXPECT_TRUE(Contains(
        source, "m_PrevSunDir = safeSun;"));
    EXPECT_TRUE(Contains(
        source, "m_PrevSunColor = safeSunColor;"));
    EXPECT_TRUE(Contains(
        source, "m_PrevSkyColor = safeSkyColor;"));
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
        shader, "float3 p=camPos.xyz+dir*sampleT;"));
    EXPECT_TRUE(Contains(shader, "float MAX_DISTANCE=min(cloudRange.x,max(cloudRange.w,1.0));"));
    EXPECT_TRUE(Contains(
        shader, "float3 local=p-worldOrigin.xyz;"));
    EXPECT_TRUE(Contains(
        shader,
        "macro.weather=cloudWeatherData(p,layerHeight,upperBand);"));
    EXPECT_TRUE(Contains(
        CompactShader(shader),
        "sampleUvw=cloudUVW("
        "p,macro.weather,macro.curl,macro.height);"));
    EXPECT_TRUE(Contains(
        CompactShader(shader),
        "cloudBaseShape("
        "sampleUvw,"
        "macro.heightThreshold-cloudBillowMaximumOffset(macro.height),"
        "sampleSpacing,"
        "macro.height,"
        "macro.baseNoise);"));
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
    const std::string weatherShader = CompactShader(
        ExtractRawShader(source, "const char* kWeatherGenCS"));
    const std::string detailShader = CompactShader(
        ExtractRawShader(source, "const char* kDetailGenCS"));
    const std::string compactSource = CompactShader(source);
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(!noiseShader.empty());
    EXPECT_TRUE(!weatherShader.empty());
    EXPECT_TRUE(!detailShader.empty());

    // 雲塊配置、基本形状、縁の侵食、渦は別の領域を使う。
    // 一つの形状体積を全用途へ流用すると、同じ繰り返し模様が露出する。
    EXPECT_TRUE(Contains(
        shader, "Texture3D<float2> shapeNoise     : register(t0)"));
    EXPECT_TRUE(Contains(
        shader, "Texture2D    weatherMap          : register(t1)"));
    EXPECT_TRUE(Contains(
        shader, "Texture3D<float2> detailNoise    : register(t2)"));
    EXPECT_TRUE(Contains(
        shader, "Texture2D    curlNoise           : register(t3)"));
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

    // R は低周波を主形状とした中周波混合の Perlin 雲体、G は輪郭用 Perlin-Worley とする。
    // 責務の異なる二値だけを RG16F に保持し、RGBA16Fへ戻さない。
    EXPECT_TRUE(Contains(
        noiseShader, "RWTexture3D<float2>noiseOut:register(u0);"));
    EXPECT_TRUE(Contains(noiseShader, "floatperlin2=gnoise(uvw*2.0,2.0);"));
    EXPECT_TRUE(Contains(noiseShader, "floatperlin4=gnoise(uvw*4.0,4.0);"));
    EXPECT_TRUE(Contains(noiseShader, "floatperlin32=gnoise(uvw*32.0,32.0);"));
    EXPECT_TRUE(Contains(noiseShader, "floatperlinMacro=perlin2*0.58+perlin4*0.30+perlin8*0.12;"));
    EXPECT_TRUE(Contains(noiseShader, "floatremap(floatv,floata,floatb,floatc,floatd){" "floatspan=max(b-a,1e-4);" "returnc+saturate((v-a)/span)*(d-c);}"));
    EXPECT_TRUE(Contains(noiseShader, "floatfullShape=remap(" "perlinFull,1.0-worleyFull,1.0,0.0,1.0);"));
    EXPECT_FALSE(Contains(noiseShader, "worleyFull-1.0"));
    const auto perlinWorleyForTest = [](f32 perlin, f32 invertedWorley) noexcept {
        const f32 lower = 1.0f - invertedWorley;
        const f32 rawSpan = 1.0f - lower;
        const f32 span = rawSpan > 1.0e-4f ? rawSpan : 1.0e-4f;
        return SaturateForTest((perlin - lower) / span);
    };
    // Worley は既に反転済みなので、値が高いセル中心ほど Perlin を膨張させる。
    // Worley が0の領域は安全に0へ閉じ、分母0を生成しない。
    EXPECT_NEAR(perlinWorleyForTest(0.55f, 0.65f), 0.3076923f, 1.0e-6f);
    EXPECT_NEAR(perlinWorleyForTest(0.55f, 1.0f), 0.55f, 1.0e-6f);
    EXPECT_NEAR(perlinWorleyForTest(0.90f, 0.0f), 0.0f, 0.0f);
    EXPECT_TRUE(Contains(noiseShader, "noiseOut[id]=float2(perlinMacro,fullShape);"));
    EXPECT_TRUE(Contains(
        detailShader, "RWTexture3D<float2>detailOut:register(u0);"));
    EXPECT_TRUE(Contains(detailShader, "detailOut[id]=float2(a,d);"));
    EXPECT_TRUE(Contains(
        compactSource,
        "td.width=128;td.height=128;td.depth=128;"
        "td.format=EFormat::R16G16_Float;td.is_uav=true;"));
    EXPECT_TRUE(Contains(
        compactSource,
        "td.width=64;td.height=64;td.depth=64;"
        "td.format=EFormat::R16G16_Float;td.is_uav=true;"));
    const std::string compactMarch = CompactShader(shader);
    EXPECT_TRUE(Contains(compactMarch, "floatcloudBaseShapeBand(" "float2shapeBands,floatsampleSpacing,floatdomainScale,floatheight)"));
    EXPECT_TRUE(Contains(compactMarch, "floatfineSignal=smoothstep(0.18,0.70,saturate(shapeBands.g));" "floattopDetail=smoothstep(0.35,0.90,saturate(height));" "floatmaximumErosion=fineVisibility*lerp(0.14,0.26,topDetail);" "returnshapeBands.r*lerp(" "1.0-maximumErosion,1.0,fineSignal);"));
    EXPECT_TRUE(Contains(compactMarch, "floatdetailNear=ndA.g*0.62+ndB.g*0.38;"));
    EXPECT_TRUE(Contains(compactMarch, "floatdetailFar=ndA.r*0.62+ndB.r*0.38;"));

    // Both the raw-DX12 CPU worker and the synchronous backend path compile
    // the three independent generators, then the owner-resource candidate
    // consumes those exact staged shaders. This replaces the old inline
    // FShaderDesc shape without weakening the resource-separation contract.
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
        compactSource, "m_WeatherCs=Move(shaders.weather);"));
    EXPECT_TRUE(Contains(
        compactSource, "m_DetailCs=Move(shaders.detail);"));
    EXPECT_TRUE(Contains(
        compactSource, "m_CurlCs=Move(shaders.curl);"));
    EXPECT_TRUE(Contains(source, "pd.srv_slots = 5"));
    EXPECT_TRUE(Contains(source, "pd.srv_names[0] = \"shapeNoise\""));
    EXPECT_TRUE(Contains(source, "pd.srv_names[1] = \"weatherMap\""));
    EXPECT_TRUE(Contains(source, "pd.srv_names[2] = \"detailNoise\""));
    EXPECT_TRUE(Contains(source, "pd.srv_names[3] = \"curlNoise\""));
    EXPECT_TRUE(Contains(source, "pd.srv_names[4] = \"cloudShadowCache\""));

    EXPECT_TRUE(Contains(
        source, "cl.SetTexture(0, *m_ShapeTex)"));
    EXPECT_TRUE(Contains(
        source, "cl.SetTexture(1, *m_WeatherTex)"));
    EXPECT_TRUE(Contains(
        source, "cl.SetTexture(2, *m_DetailTex)"));
    EXPECT_TRUE(Contains(
        source, "cl.SetTexture(3, *m_CurlTex)"));
}

ACS_TEST(VolumetricClouds,
         WeatherCoverageBendsThroughHeightWithoutMovingLayerBounds) {
    EXPECT_NEAR(CloudWeatherVerticalBendForTest(0.0f, false), -1.0f, 0.0f);
    EXPECT_NEAR(CloudWeatherVerticalBendForTest(0.5f, false), 0.0f, 0.0f);
    EXPECT_NEAR(CloudWeatherVerticalBendForTest(1.0f, false), 1.0f, 0.0f);
    EXPECT_NEAR(CloudWeatherVerticalBendForTest(0.0f, true), -0.25f, 0.0f);
    EXPECT_NEAR(CloudWeatherVerticalBendForTest(1.0f, true), 0.25f, 0.0f);
    f32 previousBend = -1.0f;
    for (u32 heightStep = 0u; heightStep <= 100u; ++heightStep) {
        const f32 bend = CloudWeatherVerticalBendForTest(
            static_cast<f32>(heightStep) / 100.0f, false);
        EXPECT_TRUE(bend + 1.0e-6f >= previousBend);
        EXPECT_TRUE(bend >= -1.0f && bend <= 1.0f);
        previousBend = bend;
    }

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudWeatherVerticalBend("
        "floatlayerHeight,boolupperBand){"
        "floatcenteredHeight=saturate(layerHeight)*2.0-1.0;"
        "floatbend=centeredHeight*lerp(0.35,1.0,abs(centeredHeight));"
        "returnbend*(upperBand?0.25:1.0);}"));
    EXPECT_TRUE(Contains(
        shader,
        "floatverticalBendScale=cloudWeatherVerticalBendScale();"
        "weatherUv+=verticalBend*verticalBendScale*"
        "float4(0.012,-0.009,-0.090,0.130);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudWeatherVerticalBendScale(){floatlayerThickness=max(layer.y-layer.x,0.0);"
        "returnlerp(0.45,1.0,smoothstep(2600.0,9400.0,layerThickness));}"));
    EXPECT_TRUE(Contains(
        shader,
        "floatlayerHeight=heightFractionFromAltitude("
        "altitude,upperBand);"
        "macro.weather=cloudWeatherData(p,layerHeight,upperBand);"));
    EXPECT_FALSE(Contains(shader, "float4cloudWeatherData(float3p){"));
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
            "constu32cur=m_FrameIndex&1u;");
        const std::size_t resolveEnd = render.find(
            "m_ResolvedIndex=cur;", resolveBegin);
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
        compactSource, "if(!historyValid)m_TemporalPhase=0u;"));
    EXPECT_TRUE(Contains(
        compactSource,
        "m_TemporalPhase=(m_TemporalPhase+1u)&15u;"));
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
        "constu32temporalFrame=(m_FrameIndex&4080u)|"
        "(m_TemporalPhase&15u);"));
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

    const std::size_t profile =
        shader.find("floatsampledProfile=cloudProfile(");
    const std::size_t threshold = shader.find(
        "macro.heightThreshold=cloudHeightThresholdFromTarget(", profile);
    const std::size_t base = shader.find("floatbaseDensity=remapc(", threshold);
    const std::size_t billow = shader.find(
        "floatbillowOffset=cloudBillowOffset("
        "ndA,ndB,h,erosionVisibility);", base);
    const std::size_t billowedCoarse = shader.find(
        "floatbillowedCoarseDensity=saturate("
        "cloudDimensionalDensity("
        "billowedBaseDensity,macro.heightProfile)*densityScale);",
        billow);
    const std::size_t coarse = shader.find(
        "floatcoarseDensity=saturate("
        "cloudDimensionalDensity("
        "baseDensity,macro.heightProfile)*densityScale);",
        base);
    const std::size_t billowedDensity = shader.find(
        "floatbillowedDensity=lerp("
        "coarseDensity,billowedCoarseDensity,billowVisibility);",
        billowedCoarse);
    const std::size_t erosion = shader.find(
        "remapc(billowedDensity,detail*erosion,1.0,0.0,1.0)",
        billowedDensity);
    const std::size_t finalDensity = shader.find(
        "densityResult=saturate(d);", erosion);
    EXPECT_TRUE(profile != std::string::npos);
    EXPECT_TRUE(threshold != std::string::npos);
    EXPECT_TRUE(base != std::string::npos);
    EXPECT_TRUE(billow != std::string::npos);
    EXPECT_TRUE(billowedCoarse != std::string::npos);
    EXPECT_TRUE(coarse != std::string::npos);
    EXPECT_TRUE(billowedDensity != std::string::npos);
    EXPECT_TRUE(erosion != std::string::npos);
    EXPECT_TRUE(finalDensity != std::string::npos);
    EXPECT_TRUE(profile < threshold);
    EXPECT_TRUE(threshold < base);
    EXPECT_TRUE(base < coarse);
    EXPECT_TRUE(coarse < billow);
    EXPECT_TRUE(billow < billowedCoarse);
    EXPECT_TRUE(billowedCoarse < billowedDensity);
    EXPECT_TRUE(billowedDensity < erosion);
    EXPECT_TRUE(erosion < finalDensity);
    EXPECT_FALSE(Contains(
        shader,
        "remapc(baseDensity,detail*erosion,1.0,0.0,1.0)"));
    EXPECT_FALSE(Contains(shader, "d*weatherMask*macro.heightProfile"));

    // 基本密度が1でも、高さ形状で薄くなった実表面は詳細侵食の対象になる。
    constexpr f32 kBaseDensity = 1.0f;
    constexpr f32 kWeatherMask = 1.0f;
    constexpr f32 kHeightProfile = 0.15f;
    constexpr f32 kPrecipitationScale = 1.0f;
    constexpr f32 kDetail = 0.60f;
    constexpr f32 kErosion = 0.24f;
    const f32 coarseDensity = SaturateForTest(CloudDimensionalDensityForTest(kBaseDensity, kHeightProfile) * kPrecipitationScale);
    const f32 correctedDensity = RemapUnitRangeForTest(
        coarseDensity, kDetail * kErosion, 1.0f);
    const f32 formerDensity = RemapUnitRangeForTest(
        kBaseDensity, kDetail * kErosion, 1.0f) *
        kHeightProfile * kPrecipitationScale;
    EXPECT_NEAR(coarseDensity, 0.15f, 1.0e-6f);
    EXPECT_TRUE(correctedDensity < formerDensity * 0.10f);
    EXPECT_NEAR(formerDensity, coarseDensity, 1.0e-6f);
}

ACS_TEST(VolumetricClouds,
         WeatherCoverageErodesThreeDimensionalShapeInsteadOfScalingColumns) {
    EXPECT_NEAR(CloudWeatherShapeErosionForTest(0.0f), 0.65f, 0.0f);
    EXPECT_NEAR(CloudWeatherShapeErosionForTest(0.50f), 0.08125f, 1.0e-6f);
    EXPECT_NEAR(CloudWeatherShapeErosionForTest(1.0f), 0.0f, 0.0f);

    f32 previousErosion = 0.65f;
    for (u32 maskStep = 0u; maskStep <= 100u; ++maskStep) {
        const f32 weatherMask = static_cast<f32>(maskStep) / 100.0f;
        const f32 erosion = CloudWeatherShapeErosionForTest(weatherMask);
        EXPECT_TRUE(erosion >= 0.0f && erosion <= 0.65f);
        EXPECT_TRUE(erosion <= previousErosion + 1.0e-6f);
        previousErosion = erosion;
    }

    constexpr f32 kThreshold = 0.60f;
    constexpr f32 kUpper = 0.82f;
    constexpr f32 kBoundaryMask = 0.50f;
    const f32 boundaryErosion =
        CloudWeatherShapeErosionForTest(kBoundaryMask);
    const f32 weakLobe = RemapUnitRangeForTest(
        0.63f - boundaryErosion, kThreshold, kUpper);
    const f32 strongLobe = RemapUnitRangeForTest(
        0.95f - boundaryErosion, kThreshold, kUpper);
    const f32 formerlyScaledWeakLobe = RemapUnitRangeForTest(
        0.63f, kThreshold, kUpper) * kBoundaryMask;
    EXPECT_NEAR(weakLobe, 0.0f, 0.0f);
    EXPECT_TRUE(strongLobe > 0.90f);
    EXPECT_TRUE(formerlyScaledWeakLobe > 0.05f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader, "floatcloudWeatherShapeErosion(floatweatherMask){"));
    EXPECT_TRUE(Contains(shader, "floatedge=1.0-saturate(weatherMask);"));
    EXPECT_TRUE(Contains(shader, "floatedgeSquared=edge*edge;"));
    EXPECT_TRUE(Contains(
        shader, "returnedgeSquared*edge*0.65;}"));
    EXPECT_TRUE(Contains(
        shader,
        "floatweatheredBaseNoise=cloudWeatheredBaseNoise("
        "macro.baseNoise,weatherMask);"));
    EXPECT_TRUE(Contains(shader, "floatdensityScale=cloudHeightPrecipitationDensityScale(" "h,macro.weather.b);"));
    EXPECT_TRUE(Contains(shader, "cloudDimensionalDensity(" "baseDensity,macro.heightProfile)*densityScale"));
    EXPECT_FALSE(Contains(shader, "*weatherMask*macro.heightProfile"));
}

ACS_TEST(VolumetricClouds,
         VerticallyCoherentDetailBillowPreservesOccupancyBounds) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    EXPECT_TRUE(Contains(
        shader,
        "floatcloudBillowMaximumOffset(floatheight){"
        "returnlerp(0.018,0.130,smoothstep(0.18,0.92,saturate(height)));}"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudDetailMiddleBand(float2detailBands){"
        "returnsaturate((detailBands.g-detailBands.r*0.55)*(1.0/0.45));}"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudBillowOffset("
        "float2detailA,float2detailB,floatheight,floatmiddleVisibility){"));
    EXPECT_TRUE(Contains(
        shader,
        "floattopMiddleWeight=0.48*"
        "smoothstep(0.38,0.90,saturate(height))*"
        "saturate(middleVisibility);"));
    EXPECT_TRUE(Contains(
        shader,
        "returnlerp(coarseDifference,middleDifference,topMiddleWeight)"
        "*cloudBillowMaximumOffset(height);"));
    EXPECT_TRUE(Contains(
        shader,
        "detailDomainA=horizontal*0.00018+vertical*0.00014;"
        "detailDomainB=horizontal*0.00031+vertical*0.00024;"));
    EXPECT_FALSE(Contains(
        shader, "detailDomainA=horizontal*0.0011+vertical*0.00055;"));
    EXPECT_FALSE(Contains(
        shader, "detailDomainB=horizontal*0.0023+vertical*0.00115;"));
    EXPECT_TRUE(Contains(
        shader,
        "floatenvelopeNoise=cloudWeatheredBaseNoise("
        "macro.baseNoise+cloudBillowMaximumOffset(macro.height),"
        "macro.weatherMask);"));
    EXPECT_TRUE(Contains(
        shader,
        "macro.heightThreshold-cloudBillowMaximumOffset(macro.height),"
        "sampleSpacing,"
        "macro.height,"
        "macro.baseNoise);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatenvelopeBaseDensity=remapc("
        "cloudWeatheredBaseNoise("
        "macro.baseNoise+cloudBillowMaximumOffset(h),weatherMask),"
        "heightThreshold,"));
    EXPECT_TRUE(Contains(shader, "floatenvelopeDensity=cloudDimensionalDensity(" "envelopeBaseDensity,macro.heightProfile);" "if(envelopeDensity>0.001){"));
    EXPECT_TRUE(Contains(
        shader,
        "floatbillowOffset=cloudBillowOffset("
        "ndA,ndB,h,erosionVisibility);"));

    constexpr f32 kThreshold = 0.60f;
    constexpr f32 kUpper = 0.82f;
    constexpr f32 kEdgeNoise = 0.57f;
    const f32 topLimit = CloudBillowMaximumOffsetForTest(1.0f);
    const f32 baseLimit = CloudBillowMaximumOffsetForTest(0.0f);
    EXPECT_NEAR(baseLimit, 0.018f, 1.0e-6f);
    EXPECT_NEAR(topLimit, 0.130f, 1.0e-6f);
    EXPECT_TRUE(topLimit > baseLimit);

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

    const f32 original = RemapUnitRangeForTest(
        kEdgeNoise, kThreshold, kUpper);
    const f32 expanded = RemapUnitRangeForTest(
        kEdgeNoise + expandedOffset, kThreshold, kUpper);
    const f32 eroded = RemapUnitRangeForTest(
        kEdgeNoise + erodedOffset, kThreshold, kUpper);
    const f32 envelope = RemapUnitRangeForTest(
        kEdgeNoise + topLimit, kThreshold, kUpper);
    EXPECT_NEAR(original, 0.0f, 0.0f);
    EXPECT_TRUE(expanded > 0.0f);
    EXPECT_NEAR(eroded, 0.0f, 0.0f);
    EXPECT_TRUE(envelope + 1.0e-6f >= expanded);

    // 値1の雲芯はしきい値移動と侵食のどちらでも密度1を保つ。
    const f32 denseCore = RemapUnitRangeForTest(
        1.0f - topLimit, kThreshold, kUpper);
    EXPECT_NEAR(denseCore, 1.0f, 0.0f);

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

ACS_TEST(VolumetricClouds, DimensionalDensityFillsCloudBodyWithoutCreatingMatter) {
    f32 previousDensity = 0.0f;
    for (u32 step = 0u; step <= 100u; ++step) {
        const f32 baseDensity = static_cast<f32>(step) * 0.01f;
        const f32 density = CloudDimensionalDensityForTest(baseDensity, 1.0f);
        EXPECT_TRUE(density + 1.0e-6f >= previousDensity);
        EXPECT_TRUE(density >= baseDensity - 1.0e-6f);
        EXPECT_TRUE(density >= 0.0f && density <= 1.0f);
        previousDensity = density;
    }
    EXPECT_NEAR(CloudDimensionalDensityForTest(0.0f, 1.0f), 0.0f, 0.0f);
    EXPECT_NEAR(CloudDimensionalDensityForTest(0.25f, 1.0f), 0.5f, 1.0e-6f);
    EXPECT_NEAR(CloudDimensionalDensityForTest(1.0f, 0.25f), 0.25f, 1.0e-6f);
    EXPECT_NEAR(CloudDimensionalDensityForTest(1.0f, 1.0f), 1.0f, 0.0f);
    EXPECT_NEAR(CloudDimensionalDensityForTest(0.25f, 0.0f), 0.0f, 0.0f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(shader, "floatcloudDimensionalDensity(" "floatbaseDensity,floatheightProfile){" "returnsqrt(saturate(baseDensity))*saturate(heightProfile);}"));
    EXPECT_TRUE(Contains(shader, "macro.heightProfile=saturate(sampledProfile);"));
    EXPECT_TRUE(Contains(shader, "cloudDimensionalDensity(" "baseDensity,macro.heightProfile)*densityScale"));
    EXPECT_FALSE(Contains(shader, "*macro.weatherMask*macro.heightProfile"));
    EXPECT_FALSE(Contains(shader, "*weatherMask*macro.heightProfile"));
    EXPECT_FALSE(Contains(shader, "cloudProfileTailClosure"));
    EXPECT_FALSE(Contains(shader, "macro.profileWeight"));
    EXPECT_FALSE(Contains(shader, "*slowWeatherMask*profileThresholdWeight"));
}

ACS_TEST(VolumetricClouds, HeightProfileShapesTheWholeColumnWithoutEarlySaturation) {
    f32 previousShape = 0.0f;
    for (u32 step = 0u; step <= 100u; ++step) {
        const f32 profile = static_cast<f32>(step) * 0.01f;
        const f32 shape = CloudVerticalProfileShapeForTest(profile);
        EXPECT_TRUE(shape + 1.0e-6f >= previousShape);
        EXPECT_TRUE(shape >= 0.0f && shape <= profile + 1.0e-6f);
        previousShape = shape;
    }
    EXPECT_NEAR(CloudVerticalProfileShapeForTest(0.0f), 0.0f, 0.0f);
    EXPECT_NEAR(CloudVerticalProfileShapeForTest(0.25f), 0.125f, 1.0e-6f);
    EXPECT_NEAR(CloudVerticalProfileShapeForTest(1.0f), 1.0f, 0.0f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudVerticalProfileShape(floatsampledProfile){"
        "floatprofile=saturate(sampledProfile);"
        "returnprofile*sqrt(profile);}"));
    EXPECT_EQ(
        CountOccurrences(
            shader,
            "floatprofileShape=cloudVerticalProfileShape(sampledProfile);"),
        static_cast<std::size_t>(4));
    EXPECT_FALSE(Contains(
        shader,
        "smoothstep(0.02,0.32,sampledProfile)"));
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
         ConvectiveHeightWarpAnchorsCloudBaseAndIsSharedWithLighting) {
    const f32 tallCore = CloudColumnHeightShiftForTest(
        1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    const f32 compressedEdge = CloudColumnHeightShiftForTest(
        0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    const f32 stratusCore = CloudColumnHeightShiftForTest(
        1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    EXPECT_NEAR(tallCore, 0.1485f, 1e-6f);
    EXPECT_NEAR(compressedEdge, -0.14515875f, 1e-6f);
    EXPECT_NEAR(stratusCore, 0.018f, 1e-6f);

    // 既定雲量で実際に見える天候値を、被覆境界から中心までの位置へ直す。
    // 生の天候値はどちらも高いが、雲の縁と中心は逆向きへ変形しなければならない。
    constexpr f32 defaultCoverage = 0.50f;
    const f32 visibleEdgeInterior = CloudWeatherMaskForTest(0.565f, defaultCoverage);
    const f32 visibleCoreInterior = CloudWeatherMaskForTest(0.665f, defaultCoverage);
    const f32 visibleEdgeShift = CloudColumnHeightShiftForTest(visibleEdgeInterior, 1.0f, 0.0f, 0.50f, 0.0f, 0.0f);
    const f32 visibleCoreShift = CloudColumnHeightShiftForTest(visibleCoreInterior, 1.0f, 0.0f, 0.50f, 0.0f, 0.0f);
    EXPECT_TRUE(visibleEdgeInterior < 0.10f);
    EXPECT_TRUE(visibleCoreInterior > 0.90f);
    EXPECT_TRUE(visibleEdgeShift < -0.08f);
    EXPECT_TRUE(visibleCoreShift > 0.10f);

    // 同じ時刻でも低周波の天候模様が異なる地点は、逆向きへ変形する。
    const f32 lowWarpAtRest = CloudColumnHeightShiftForTest(
        0.60f, 0.50f, 0.0f, 0.40f, 0.0f, 0.0f);
    const f32 lowWarpEvolved = CloudColumnHeightShiftForTest(
        0.60f, 0.50f, 0.0f, 0.40f, 0.18f, 0.0f);
    const f32 highWarpAtRest = CloudColumnHeightShiftForTest(
        0.60f, 0.50f, 0.0f, 0.60f, 0.0f, 0.0f);
    const f32 highWarpEvolved = CloudColumnHeightShiftForTest(
        0.60f, 0.50f, 0.0f, 0.60f, 0.18f, 0.0f);
    EXPECT_TRUE(lowWarpEvolved < lowWarpAtRest);
    EXPECT_TRUE(highWarpEvolved > highWarpAtRest);
    EXPECT_TRUE(lowWarpAtRest - lowWarpEvolved > 0.002f);
    EXPECT_TRUE(highWarpEvolved - highWarpAtRest > 0.002f);

    // 第2位相も雲種模様を通じて独立に寄与し、全許容入力でも変形量を越えない。
    const f32 lowTypeAtRest = CloudColumnHeightShiftForTest(
        0.60f, 0.25f, 0.0f, 0.50f, 0.0f, 0.0f);
    const f32 lowTypeEvolved = CloudColumnHeightShiftForTest(
        0.60f, 0.25f, 0.0f, 0.50f, 0.0f, 0.16f);
    const f32 highTypeAtRest = CloudColumnHeightShiftForTest(
        0.60f, 0.75f, 0.0f, 0.50f, 0.0f, 0.0f);
    const f32 highTypeEvolved = CloudColumnHeightShiftForTest(
        0.60f, 0.75f, 0.0f, 0.50f, 0.0f, 0.16f);
    EXPECT_TRUE(lowTypeEvolved < lowTypeAtRest);
    EXPECT_TRUE(highTypeEvolved > highTypeAtRest);
    for (u32 warpStep = 0u; warpStep <= 20u; ++warpStep) {
        for (u32 typeStep = 0u; typeStep <= 20u; ++typeStep) {
            const f32 warp = static_cast<f32>(warpStep) / 20.0f;
            const f32 cloudType = static_cast<f32>(typeStep) / 20.0f;
            for (const f32 shapePhaseX : {-0.18f, 0.18f}) {
                for (const f32 shapePhaseY : {-0.16f, 0.16f}) {
                    EXPECT_TRUE(std::fabs(CloudColumnHeightShiftForTest(
                        0.60f, cloudType, 1.0f, warp,
                        shapePhaseX, shapePhaseY)) <= 0.148501f);
                }
            }
        }
    }

    // 積乱雲のかなとこは上部だけを横へ広げ、雲底と雲頂では広がらない。
    const f32 stormAnvilCore = CloudAnvilCoverageExpansionForTest(
        0.66f, 1.0f, 1.0f);
    const f32 stormAnvilEdge = CloudAnvilCoverageExpansionForTest(
        0.90f, 1.0f, 1.0f);
    const f32 normalAnvil = CloudAnvilCoverageExpansionForTest(
        0.66f, 0.5f, 0.0f);
    EXPECT_NEAR(stormAnvilCore, 0.16f, 1e-6f);
    EXPECT_TRUE(stormAnvilEdge > 0.0f);
    EXPECT_TRUE(stormAnvilCore > stormAnvilEdge);
    EXPECT_NEAR(normalAnvil, 0.0f, 1e-6f);
    EXPECT_TRUE(
        CloudWeatherMaskForLayerForTest(
            0.50f, 0.54f, 1.0f / 0.14f, 0.66f, 1.0f, 1.0f) >
        CloudWeatherMaskForLayerForTest(
            0.50f, 0.54f, 1.0f / 0.14f, 0.20f, 1.0f, 1.0f));

    // 本体の上端は絞り、かなとこ帯で再び広がる。全高が同じ密度にならないことを確認する。
    const f32 stormBody = CloudStormProfileForTest(0.36f, 0.88f, 0.62f);
    const f32 stormWaist = CloudStormProfileForTest(0.58f, 0.88f, 0.62f);
    const f32 stormAnvil = CloudStormProfileForTest(0.72f, 0.88f, 0.62f);
    const f32 stormTop = CloudStormProfileForTest(0.94f, 0.88f, 0.62f);
    EXPECT_TRUE(stormBody > stormWaist);
    EXPECT_TRUE(stormAnvil > stormWaist);
    EXPECT_TRUE(stormTop < stormAnvil);
    EXPECT_TRUE(stormBody - stormWaist > 0.07f);
    EXPECT_TRUE(stormAnvil - stormWaist > 0.05f);
    EXPECT_TRUE(CloudConvectiveWaistScaleForTest(0.50f, 0.88f, 0.62f) < 0.80f);
    EXPECT_NEAR(CloudConvectiveWaistScaleForTest(0.50f, 0.50f, 0.0f), 1.0f, 1e-6f);

    // 凝結高度までは柱ごとの変形を加えず、雲底を煙のように上下させない。
    const f32 liftedLowerBody = CloudConvectiveHeightForTest(0.10f, tallCore, false);
    const f32 loweredLowerBody = CloudConvectiveHeightForTest(0.10f, compressedEdge, false);
    EXPECT_NEAR(liftedLowerBody, 0.10f, 0.0f);
    EXPECT_NEAR(loweredLowerBody, 0.10f, 0.0f);
    EXPECT_NEAR(CloudConvectiveHeightForTest(0.14f, tallCore, false), 0.14f, 1e-6f);
    EXPECT_TRUE(CloudConvectiveHeightForTest(0.50f, tallCore, false) < 0.46f);
    EXPECT_TRUE(CloudConvectiveHeightForTest(0.50f, compressedEdge, false) > 0.54f);

    // 積雲の高さ形状は0.94で閉じる。圧縮した縁は物理高度0.84で既に上端を越え、
    // 成長した中心は0.94でも内部に残るため、全列が同じ層上端へ揃わない。
    EXPECT_TRUE(CloudConvectiveHeightForTest(0.84f, compressedEdge, false) > 0.90f);
    EXPECT_TRUE(CloudConvectiveHeightForTest(0.94f, tallCore, false) < 0.94f);

    // 高さ変形は凝結高度と層上端を固定し、全許容変形量で折り返さない。
    for (u32 shiftStep = 0u; shiftStep <= 36u; ++shiftStep) {
        const f32 shift =
            -0.18f + static_cast<f32>(shiftStep) * 0.01f;
        f32 previousLower = 0.0f;
        f32 previousUpper = 0.0f;
        EXPECT_NEAR(
            CloudConvectiveHeightForTest(0.0f, shift, false),
            0.0f, 1e-6f);
        EXPECT_NEAR(
            CloudConvectiveHeightForTest(1.0f, shift, false),
            1.0f, 1e-6f);
        for (u32 heightStep = 1u; heightStep <= 1000u; ++heightStep) {
            const f32 height =
                static_cast<f32>(heightStep) / 1000.0f;
            const f32 lower = CloudConvectiveHeightForTest(
                height, shift, false);
            const f32 upper = CloudConvectiveHeightForTest(
                height, shift, true);
            EXPECT_TRUE(lower + 1e-6f >= previousLower);
            EXPECT_TRUE(upper + 1e-6f >= previousUpper);
            EXPECT_TRUE(lower >= 0.0f && lower <= 1.0f);
            EXPECT_TRUE(upper >= 0.0f && upper <= 1.0f);
            if (height <= 0.14f) {
                EXPECT_NEAR(lower, height, 1e-6f);
                EXPECT_NEAR(upper, height, 1e-6f);
            }
            EXPECT_TRUE(
                std::fabs(upper - height) <=
                std::fabs(lower - height) + 1e-6f);
            previousLower = lower;
            previousUpper = upper;
        }
    }

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudLocalConvectionPhase(float4weather){"
        "floatwarpPattern=smoothstep(0.36,0.64,weather.a)*2.0-1.0;"
        "floattypePattern=weather.g*2.0-1.0;"
        "returndot(cloudEvolution.xy,float2(warpPattern,typePattern));}"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudColumnHeightShift(float4weather,floatcloudInterior){"
        "floatcore=smoothstep(0.08,0.92,saturate(cloudInterior));"
        "floatverticalType=saturate(max(weather.g,weather.b));"
        "floatamplitude=lerp(0.018,0.11,verticalType);"
        "amplitude*=lerp(1.0,1.35,cloudToweringStrength(weather.g,weather.b));"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudToweringStrength(floatcloudType,floatprecipitation){"
        "floattypeTower=smoothstep(0.72,0.98,saturate(cloudType));"
        "floatprecipitationTower=smoothstep(0.25,0.85,saturate(precipitation));"
        "returnmax(typeTower,precipitationTower);}"));
    EXPECT_TRUE(Contains(
        shader,
        "floatstormBody=smoothstep(0.045,0.16,h)"
        "*(1.0-smoothstep(0.40,0.70,h));"
        "floatstormShoulder=smoothstep(0.20,0.34,h)"
        "*(1.0-smoothstep(0.46,0.66,h))*0.62;"
        "floatanvil=smoothstep(0.56,0.70,h)"
        "*(1.0-smoothstep(0.85,0.995,h))*0.82;"
        "floatstorm=max(stormBody,max(stormShoulder,anvil));"
        "floatstormMix=max(precipitation*0.72,"
        "cloudToweringStrength(typeWeights.y,precipitation)*0.92);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudConvectiveWaistScale(floatlayerHeight,floatcloudType,floatprecipitation){"
        "floattower=cloudToweringStrength(cloudType,precipitation);"
        "floatwaist=smoothstep(0.28,0.44,saturate(layerHeight))"
        "*(1.0-smoothstep(0.58,0.74,saturate(layerHeight)));"
        "returnsaturate(1.0-0.34*tower*waist);}"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudConvectiveBodyDensity("
        "floatbaseDensity,floatheight,floatcloudType,floatprecipitation){"
        "floatdensity=saturate(baseDensity);"
        "floattower=cloudToweringStrength(cloudType,precipitation);"
        "floatbodyBand=smoothstep(0.08,0.28,saturate(height))"
        "*(1.0-smoothstep(0.56,0.76,saturate(height)));"
        "floattightening=0.58*tower*bodyBand;"
        "returnlerp(density,density*density,tightening);}"));
    EXPECT_TRUE(Contains(
        shader,
        "baseDensity=cloudConvectiveBodyDensity(baseDensity,macro.height,macro.weather.g,macro.weather.b);"));
    EXPECT_TRUE(Contains(
        shader,
        "envelopeBaseDensity=cloudConvectiveBodyDensity(envelopeBaseDensity,h,macro.weather.g,macro.weather.b);"));
    EXPECT_TRUE(Contains(
        shader,
        "billowedBaseDensity=cloudConvectiveBodyDensity(billowedBaseDensity,h,macro.weather.g,macro.weather.b);"));
    EXPECT_TRUE(Contains(
        shader,
        "envelopeBaseDensity=cloudConvectiveBodyDensity(envelopeBaseDensity,macro.height,macro.weather.g,macro.weather.b);"));
    EXPECT_TRUE(Contains(
        shader,
        "baseDensity=cloudConvectiveBodyDensity(baseDensity,sampleHeight,weather.g,weather.b);"));
    EXPECT_TRUE(Contains(
        shader,
        "baseMask*=cloudConvectiveWaistScale(layerHeight,weather.g,weather.b);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudAnvilCoverageExpansion(floatlayerHeight,floatcloudType,floatprecipitation){"
        "floattower=cloudToweringStrength(cloudType,precipitation);"
        "floatanvilBand=smoothstep(0.50,0.66,saturate(layerHeight))"
        "*(1.0-smoothstep(0.80,0.97,saturate(layerHeight)));"
        "return0.16*tower*anvilBand;}"));
    EXPECT_TRUE(Contains(
        shader,
        "weather.a-0.5+cloudLocalConvectionPhase(weather)*0.45,"));
    EXPECT_FALSE(Contains(
        shader,
        "weather.a-0.5+cloudEvolution.x*0.45"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudConvectiveHeight("
        "floath,floatcolumnShift,boolupperBand){"
        "h=saturate(h);"
        "constfloatcondensationHeight=0.14;"
        "floatupperHeight=saturate((h-condensationHeight)/(1.0-condensationHeight));"
        "floatupperRemainder=1.0-upperHeight;"
        "floatupperHeightSquared=upperHeight*upperHeight;"
        "floatupperInterior=45.5625*upperHeightSquared*upperHeightSquared*upperRemainder*upperRemainder;"
        "floatbandScale=upperBand?0.30:1.0;"
        "floatshiftedUpper=saturate(upperHeight-columnShift*upperInterior*bandScale);"
        "returnh<=condensationHeight?h:lerp(condensationHeight,1.0,shiftedUpper);}"));
    EXPECT_FALSE(Contains(shader, "floatinterior=4.0*h*(1.0-h);"));
    EXPECT_TRUE(Contains(shader, "float4(0.56,0.84,0.94,0.98),h.xxxx);"));
    EXPECT_TRUE(Contains(shader, "float4sharedLightProfileTerms=float4(cloudProfileTypeWeights(macro.weather.g),macro.weather.b,cloudColumnHeightShift(macro.weather,macro.densityWeatherMask));"));
    EXPECT_TRUE(Contains(shader, "floatsharedLightBaseLift=cloudColumnBaseLift(macro.weather,macro.densityWeatherMask);"));
    EXPECT_TRUE(Contains(shader, "floatviewWeatherMask=macro.densityWeatherMask;"));
    EXPECT_FALSE(Contains(shader, "floatviewWeatherMask=cloudWeatherMaskFromTerms("));
    EXPECT_TRUE(Contains(
        shader,
        "macro.height=cloudColumnHeight("
        "heightFractionFromAltitude(altitude,upperBand),"
        "slowProfileTerms.w,slowBaseLift,upperBand);"));
}

ACS_TEST(VolumetricClouds,
         ColumnBaseLiftBreaksCommonUndersideWithinPhysicalShell) {
    const f32 lowPatternLift = CloudColumnBaseLiftForTest(1.0f, 1.0f, 0.0f, 0.0f);
    const f32 highPatternLift = CloudColumnBaseLiftForTest(1.0f, 1.0f, 0.0f, 1.0f);
    const f32 visibleEdgeLift = CloudColumnBaseLiftForTest(0.0f, 1.0f, 0.0f, 0.0f);
    const f32 stratusLift = CloudColumnBaseLiftForTest(1.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(lowPatternLift, 0.0084f, 1e-6f);
    EXPECT_NEAR(highPatternLift, 0.0735f, 1e-6f);
    EXPECT_NEAR(visibleEdgeLift, 0.0399f, 1e-6f);
    EXPECT_NEAR(stratusLift, 0.0175f, 1e-6f);
    EXPECT_TRUE(highPatternLift > lowPatternLift + 0.04f);
    EXPECT_TRUE(visibleEdgeLift > lowPatternLift);
    EXPECT_TRUE(stratusLift < highPatternLift);

    // 同じ物理高度でも低周波模様の異なる柱は同時に立ち上がらず、共通の平面を作らない。
    constexpr f32 lowerLayerHeight = 0.04f;
    EXPECT_TRUE(CloudColumnHeightForTest(lowerLayerHeight, 0.0f, lowPatternLift, false) > 0.0f);
    EXPECT_NEAR(CloudColumnHeightForTest(lowerLayerHeight, 0.0f, highPatternLift, false), 0.0f, 0.0f);

    // 局所雲底より下は常に空で、上端は固定し、全許容値で折り返さない。
    for (u32 liftStep = 0u; liftStep <= 12u; ++liftStep) {
        const f32 lift = static_cast<f32>(liftStep) * 0.01f;
        for (const bool upperBand : {false, true}) {
            const f32 scaledLift = lift * (upperBand ? 0.35f : 1.0f);
            EXPECT_NEAR(CloudColumnHeightForTest(scaledLift, 0.0f, lift, upperBand), 0.0f, 0.0f);
            EXPECT_NEAR(CloudColumnHeightForTest(1.0f, 0.18f, lift, upperBand), 1.0f, 1e-6f);
            f32 previous = 0.0f;
            for (u32 heightStep = 0u; heightStep <= 1000u; ++heightStep) {
                const f32 height =
                    static_cast<f32>(heightStep) / 1000.0f;
                const f32 localHeight = CloudColumnHeightForTest(height, 0.18f, lift, upperBand);
                EXPECT_TRUE(localHeight + 1e-6f >= previous);
                EXPECT_TRUE(localHeight >= 0.0f && localHeight <= 1.0f);
                previous = localHeight;
            }
        }
    }

    // 上層雲は同じ入力でも局所雲底差を抑え、薄い層を過度に削らない。
    EXPECT_TRUE(CloudColumnHeightForTest(0.07f, 0.0f, highPatternLift, true) > 0.0f);
    EXPECT_NEAR(CloudColumnHeightForTest(0.05f, 0.0f, highPatternLift, false), 0.0f, 0.0f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudColumnBaseLift(float4weather,floatcloudInterior){"
        "floatverticalType=saturate(max(weather.g,weather.b));"
        "floatbroadPattern=smoothstep(0.18,0.82,weather.a);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatamplitude=lerp(0.025,0.075,verticalType);"
        "amplitude*=lerp(1.0,1.40,cloudToweringStrength(weather.g,weather.b));"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudColumnHeight("
        "floath,floatcolumnShift,floatbaseLift,boolupperBand){"
        "h=saturate(h);"
        "floatbandScale=upperBand?0.35:1.0;"
        "floatlocalBase=saturate(baseLift*bandScale);"));
    EXPECT_EQ(CountOccurrences(shader, "cloudColumnBaseLift("), static_cast<std::size_t>(5));
    EXPECT_EQ(CountOccurrences(shader, "cloudColumnHeight("), static_cast<std::size_t>(5));
    EXPECT_TRUE(Contains(
        shader,
        "floaterosion=lerp(0.10,0.24,"
        "smoothstep(0.18,0.92,h));"));
    EXPECT_FALSE(Contains(shader, "floaterosion=lerp(0.17,0.24,"));
}

ACS_TEST(VolumetricClouds,
         WeatherCoreBuildsBodyWithoutMovingLayerBoundaries) {
    EXPECT_NEAR(
        CloudWeatherCoreShapeOffsetForTest(1.0f, 0.0f, false),
        0.0f, 0.0f);
    EXPECT_NEAR(
        CloudWeatherCoreShapeOffsetForTest(1.0f, 1.0f, false),
        0.0f, 0.0f);
    EXPECT_NEAR(
        CloudWeatherCoreShapeOffsetForTest(0.0f, 0.5f, false),
        0.0f, 0.0f);
    EXPECT_NEAR(CloudWeatherCoreShapeOffsetForTest(1.0f, 0.5f, false), 0.030f, 1.0e-6f);
    EXPECT_NEAR(CloudWeatherCoreShapeOffsetForTest(1.0f, 0.5f, true), 0.015f, 1.0e-6f);

    for (u32 heightStep = 0u; heightStep <= 100u; ++heightStep) {
        const f32 height = static_cast<f32>(heightStep) / 100.0f;
        f32 previousLower = 0.0f;
        for (u32 maskStep = 0u; maskStep <= 100u; ++maskStep) {
            const f32 mask = static_cast<f32>(maskStep) / 100.0f;
            const f32 lower = CloudWeatherCoreShapeOffsetForTest(
                mask, height, false);
            const f32 upper = CloudWeatherCoreShapeOffsetForTest(
                mask, height, true);
            EXPECT_TRUE(lower + 1.0e-6f >= previousLower);
            EXPECT_TRUE(lower >= 0.0f && lower <= 0.030001f);
            EXPECT_TRUE(upper >= 0.0f && upper <= 0.015001f);
            EXPECT_TRUE(upper <= lower + 1.0e-6f);
            previousLower = lower;
        }
    }

    // 広い占有被覆は密度被覆以上の中心度を持つため、雲体拡張後もしきい値は必ず保守的である。
    for (u32 coverageStep = 0u; coverageStep <= 100u; ++coverageStep) {
        const f32 coverage = static_cast<f32>(coverageStep) / 100.0f;
        const f32 occupancyCoverage = SaturateForTest(coverage + 0.08f);
        for (u32 profileStep = 0u; profileStep <= 20u; ++profileStep) {
            const f32 profile = static_cast<f32>(profileStep) / 20.0f;
            const auto thresholdFor = [profile](f32 value) noexcept {
                const f32 bounded = value < 0.72f ? value : 0.72f;
                const f32 target = 0.50f + (0.34f - 0.50f) * bounded;
                return 0.62f + (target - 0.62f) * profile;
            };
            for (u32 densityMaskStep = 0u;
                 densityMaskStep <= 20u; ++densityMaskStep) {
                const f32 densityMask =
                    static_cast<f32>(densityMaskStep) / 20.0f;
                const f32 occupancyMask = SaturateForTest(
                    densityMask + 0.12f);
                const f32 occupancyThreshold =
                    thresholdFor(occupancyCoverage) -
                    CloudWeatherCoreShapeOffsetForTest(
                        occupancyMask, 0.5f, false);
                const f32 densityThreshold = thresholdFor(coverage) -
                    CloudWeatherCoreShapeOffsetForTest(
                        densityMask, 0.5f, false);
                EXPECT_TRUE(
                    occupancyThreshold <= densityThreshold + 1.0e-6f);
            }
        }
    }

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(shader, "floatcloudWeatherCoreShapeOffset(" "floatweatherMask,floatheight,boolupperBand){" "floatcore=smoothstep(0.18,0.82,saturate(weatherMask));" "floatbody=smoothstep(0.10,0.30,saturate(height))*" "(1.0-smoothstep(0.82,0.98,saturate(height)));" "returncore*body*(upperBand?0.015:0.030);}"));
    EXPECT_EQ(
        CountOccurrences(shader, "cloudWeatherCoreShapeOffset("),
        static_cast<std::size_t>(6));
    EXPECT_TRUE(Contains(
        shader,
        "macro.heightThreshold=cloudHeightThresholdFromTarget("
        "coverageTerms.z,profileShape)-cloudWeatherCoreShapeOffset("
        "macro.weatherMask,macro.height,upperBand);"
        "densityHeightThreshold=cloudHeightThresholdFromTarget("
        "coverageTerms.w,profileShape)-cloudWeatherCoreShapeOffset("
        "macro.densityWeatherMask,macro.height,upperBand);"));
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
         WeatherFirstRejectIsConservativeAndPrecedesVolumeFetches) {
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
        "returnlerp(0.72,0.36,saturate(coverage));}"
        "floatcloudWeatherMaskFromThreshold(float4weather,floatthreshold){"
        "returnsmoothstep("
        "threshold,min(threshold+0.14,0.98),weather.r);}"));
    EXPECT_TRUE(Contains(shader, "float4cloudCoverage;"));
    EXPECT_TRUE(Contains(
        compactSource,
        "constf32occupancyCoverage="
        "safeCoverage+0.08f<1.0f?safeCoverage+0.08f:1.0f;"));
    EXPECT_TRUE(Contains(compactSource, "out.coverage=FVec4{" "0.72f-0.36f*occupancyCoverage," "0.72f-0.36f*safeCoverage," "0.50f-0.16f*occupancyHeightCoverage," "0.50f-0.16f*densityHeightCoverage};"));
    const char* declarations[]{
        "CloudMacroSamplesampleCloudMacro(",
        "CloudMacroSamplesampleCloudMacroLighting("};
    const char* initializers[]{
        "macro.weather=float4(0,0,0,0);",
        "macro.curl=float2(0,0);",
        "macro.baseNoise=0.0;",
        "macro.weatherMask=0.0;",
        "macro.densityWeatherMask=0.0;",
        "macro.heightProfile=0.0;",
        "macro.heightThreshold=0.62;",
        "macro.height=0.0;",
        "macro.upperBand=0.0;"};
    for (u32 functionIndex = 0u;
         functionIndex < 2u;
         ++functionIndex) {
        const std::size_t begin =
            shader.find(declarations[functionIndex]);
        const std::size_t end =
            shader.find("returnmacro;}", begin);
        EXPECT_TRUE(begin != std::string::npos);
        EXPECT_TRUE(end != std::string::npos);
        if (begin == std::string::npos ||
            end == std::string::npos) {
            continue;
        }
        const std::string function =
            shader.substr(begin, end - begin);
        const std::size_t altitude =
            function.find("floataltitude=cloudAltitude(p);");
        const std::size_t layerBand =
            function.find("boolupperBand=inUpperCloudBandFromAltitude(altitude);");
        const std::size_t layerHeight =
            function.find("floatlayerHeight=heightFractionFromAltitude("
                          "altitude,upperBand);");
        const std::size_t weather =
            function.find(
                "macro.weather=cloudWeatherData(p,layerHeight,upperBand);");
        const std::size_t mask =
            function.find("macro.weatherMask=cloudWeatherMask");
        const std::size_t maskBranch =
            function.find("if(macro.weatherMask>0.001){");
        const std::size_t height =
            function.find("macro.height=cloudColumnHeight(");
        const usize macroUpperBand =
            function.find("macro.upperBand=upperBand?1.0:0.0;");
        const std::size_t profile =
            function.find("floatsampledProfile=cloudProfile(");
        const std::size_t profileBranch =
            function.find("if(sampledProfile>0.0){");
        const std::size_t profileShape =
            function.find(
                "floatprofileShape=cloudVerticalProfileShape(");
        const std::size_t heightThreshold =
            function.find(
                "macro.heightThreshold=cloudHeightThreshold");
        const std::size_t curl =
            function.find("macro.curl=cloudCurlOffset(p);");
        const std::size_t heightProfile = function.find("macro.heightProfile=saturate(sampledProfile);");
        const std::size_t shape =
            function.find("cloudBaseShape");
        for (const char* initializer : initializers) {
            const std::size_t initialized =
                function.find(initializer);
            EXPECT_TRUE(initialized != std::string::npos);
            EXPECT_TRUE(initialized < altitude);
        }
        EXPECT_TRUE(altitude < layerBand);
        EXPECT_TRUE(layerBand < layerHeight);
        EXPECT_TRUE(layerHeight < weather);
        EXPECT_TRUE(weather < mask);
        EXPECT_TRUE(mask < maskBranch);
        EXPECT_TRUE(maskBranch < height);
        EXPECT_TRUE(mask < macroUpperBand);
        EXPECT_TRUE(macroUpperBand < height);
        EXPECT_TRUE(height < profile);
        EXPECT_TRUE(profile < profileBranch);
        EXPECT_TRUE(profileBranch < profileShape);
        EXPECT_TRUE(profileShape < heightThreshold);
        EXPECT_TRUE(heightThreshold < heightProfile);
        EXPECT_TRUE(heightProfile < curl);
        EXPECT_TRUE(curl < shape);
    }

}

ACS_TEST(VolumetricClouds,
         MacroDensityIsReusedAndShapeLodsFourWorldSpaceDomains) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    // 視線は一つの大域標本を、広い空領域判定と詳細密度の両方へ再利用する。
    // 占有された各標本で天候、渦、基本体積を二重に評価しない。
    EXPECT_TRUE(Contains(
        shader,
        "float3viewMacroUvw;"
        "floatdensityHeightThreshold;"
        "CloudMacroSamplemacro=sampleCloudMacro("
        "p,coverageTerms,projectedSampleSpacing,"
        "viewMacroUvw,densityHeightThreshold);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatshape=cloudShapeFromMacro(macro);"));
    EXPECT_TRUE(Contains(shader, "floatviewWeatherMask=macro.densityWeatherMask;"));
    EXPECT_TRUE(Contains(shader, "floatdens=cloudDensityFromMacro(p,macro,densityHeightThreshold,viewWeatherMask,billowVisibility,erosionVisibility)*density*distanceFade;"));
    EXPECT_TRUE(Contains(
        shader, "macro.curl=cloudCurlOffset(p);"));
    EXPECT_TRUE(Contains(
        shader,
        "float2detailXz=p.xz-cloudWindWorld()+macro.curl*35.0;"));
    EXPECT_TRUE(Contains(
        shader,
        "cloudDetailDomains(detailXz,p.y,detailDomainA,detailDomainB);"));
    EXPECT_FALSE(Contains(shader, "rotateNoise(detailPosA)"));
    EXPECT_FALSE(Contains(shader, "rotateNoise(detailPosB)"));
    EXPECT_FALSE(Contains(
        shader,
        "float2detailXz=p.xz-cloudWindWorld()+"
        "cloudCurlOffset(p)*35.0;"));

    // 第1領域だけで雲体を作り、三つの独立領域は乗算侵食で輪郭と繰り返しだけを崩す。
    // 採取不能な領域は重み0となり、雲体を増減させない。
    EXPECT_TRUE(Contains(shader, "float3uvwD=float3("));
    EXPECT_TRUE(Contains(
        shader,
        ")*4.73+float3(0.263,0.887,0.491)"
        "+float3(cloudEvolution.y,-cloudEvolution.x,cloudEvolution.x);"));
    EXPECT_TRUE(Contains(shader, "floatshape=cloudBaseShapeBand(a,sampleSpacing,1.0,height);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatbWeight=0.04*cloudShapeDomainVisibility("
        "sampleSpacing,1.83);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcWeight=0.02*cloudShapeDomainVisibility("
        "sampleSpacing,3.17);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatdWeight=0.01*cloudShapeDomainVisibility("
        "sampleSpacing,4.73);"));
    EXPECT_TRUE(Contains(shader, "floatcloudGovernedShapeErosion(" "floatgovernedShape,floaterosionShape,floatmaximumErosion){" "floaterosionSignal=smoothstep(0.01,0.28,saturate(erosionShape));" "returngovernedShape*lerp(" "1.0-saturate(maximumErosion),1.0,erosionSignal);}"));
    EXPECT_FALSE(Contains(shader, "cloudCenteredShape"));
    EXPECT_TRUE(Contains(shader, "[branch]if(dWeight>0.0){"));
    EXPECT_TRUE(Contains(shader, "shapeResult=saturate(shape);"));
    const std::size_t viewShapeBegin =
        shader.find(
            "voidcloudBaseShape("
            "float3uvw,floatrejectionThreshold,floatsampleSpacing,"
            "floatheight,"
            "outfloatshapeResult){");
    const std::size_t lightShapeBegin =
        shader.find(
            "voidcloudBaseShapeLighting("
            "float3uvw,floatrejectionThreshold,floatsampleSpacing,"
            "floatheight,"
            "outfloatshapeResult){");
    const std::size_t macroBegin =
        shader.find("structCloudMacroSample", lightShapeBegin);
    EXPECT_TRUE(viewShapeBegin != std::string::npos);
    EXPECT_TRUE(lightShapeBegin != std::string::npos);
    EXPECT_TRUE(macroBegin != std::string::npos);
    EXPECT_TRUE(viewShapeBegin < lightShapeBegin);
    EXPECT_TRUE(lightShapeBegin < macroBegin);
    if (viewShapeBegin != std::string::npos &&
        lightShapeBegin != std::string::npos &&
        macroBegin != std::string::npos) {
        EXPECT_EQ(
            CountOccurrences(
                shader.substr(viewShapeBegin,
                              lightShapeBegin - viewShapeBegin),
                "shapeNoise.SampleLevel"),
            static_cast<std::size_t>(4));
        EXPECT_EQ(
            CountOccurrences(
                shader.substr(lightShapeBegin,
                              macroBegin - lightShapeBegin),
                "shapeNoise.SampleLevel"),
            static_cast<std::size_t>(3));
    }
    EXPECT_FALSE(Contains(shader, "sampleCloudMacro(p-camPos"));
}

ACS_TEST(VolumetricClouds, GovernedShapeErosionSkipsOnlyProvablyEmptyDensity) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    // 追加領域は乗算侵食だけを行うため、現在値がしきい値未満なら後段で復活しない。
    // 視線側3か所と光側2か所の棄却は、各追加採取を省略できる位置に置く。
    EXPECT_EQ(CountOccurrences(shader, "[branch]if(shape<rejectionThreshold-1e-5)return;"), static_cast<std::size_t>(5));
    EXPECT_TRUE(Contains(shader, "shape=cloudGovernedShapeErosion(" "shape,b.g,bWeight);"));
    EXPECT_TRUE(Contains(shader, "shape=cloudGovernedShapeErosion(" "shape,c.g,cWeight);"));
    EXPECT_TRUE(Contains(shader, "shape=cloudGovernedShapeErosion(" "shape,d.g,dWeight);"));

    // FXCの経路解析が出力値を未定義と扱わないよう、双方とも棄却前に0で初期化する。
    const std::size_t viewShapeBegin =
        shader.find(
            "voidcloudBaseShape("
            "float3uvw,floatrejectionThreshold,floatsampleSpacing,"
            "floatheight,"
            "outfloatshapeResult){");
    const std::size_t lightShapeBegin =
        shader.find(
            "voidcloudBaseShapeLighting("
            "float3uvw,floatrejectionThreshold,floatsampleSpacing,"
            "floatheight,"
            "outfloatshapeResult){");
    const std::size_t macroBegin =
        shader.find("structCloudMacroSample", lightShapeBegin);
    EXPECT_TRUE(viewShapeBegin != std::string::npos);
    EXPECT_TRUE(lightShapeBegin != std::string::npos);
    EXPECT_TRUE(macroBegin != std::string::npos);
    if (viewShapeBegin != std::string::npos &&
        lightShapeBegin != std::string::npos &&
        macroBegin != std::string::npos) {
        const std::string viewShape = shader.substr(
            viewShapeBegin, lightShapeBegin - viewShapeBegin);
        const std::string lightShape = shader.substr(
            lightShapeBegin, macroBegin - lightShapeBegin);
        EXPECT_TRUE(Contains(viewShape, "shapeResult=0.0;"));
        EXPECT_TRUE(Contains(lightShape, "shapeResult=0.0;"));
        EXPECT_EQ(
            CountOccurrences(viewShape, "return;"),
            static_cast<std::size_t>(3));
        EXPECT_EQ(
            CountOccurrences(lightShape, "return;"),
            static_cast<std::size_t>(2));
        EXPECT_FALSE(Contains(viewShape, "return0.0;"));
        EXPECT_FALSE(Contains(lightShape, "return0.0;"));
    }

    const auto governedErosion = [](f32 governedShape, f32 erosionShape, f32 maximumErosion) noexcept {
        const f32 erosionSignal = SmoothStepForTest(0.01f, 0.28f, SaturateForTest(erosionShape));
        return governedShape *
            ((1.0f - maximumErosion) + erosionSignal * maximumErosion);
    };
    u32 fourLobeRejects = 0u;
    u32 fourLobeViolations = 0u;
    u32 threeLobeRejects = 0u;
    u32 threeLobeViolations = 0u;
    u32 supportExpansionViolations = 0u;
    u32 emptySupportViolations = 0u;
    for (u32 state = 0u; state < 65536u; ++state) {
        const f32 a = static_cast<f32>(state & 15u) / 15.0f;
        const f32 b = static_cast<f32>((state >> 4u) & 15u) / 15.0f;
        const f32 c = static_cast<f32>((state >> 8u) & 15u) / 15.0f;
        const f32 d = static_cast<f32>((state >> 12u) & 15u) / 15.0f;
        const f32 threshold =
            0.34f + 0.28f *
                static_cast<f32>((state * 37u) & 255u) / 255.0f;

        const f32 fullFour = governedErosion(governedErosion(governedErosion(a, b, 0.04f), c, 0.02f), d, 0.01f);
        f32 partialFour = a;
        bool rejectedFour = partialFour < threshold - 1.0e-5f;
        if (!rejectedFour) {
            partialFour = governedErosion(partialFour, b, 0.04f);
            rejectedFour = partialFour < threshold - 1.0e-5f;
        }
        if (!rejectedFour) {
            partialFour = governedErosion(partialFour, c, 0.02f);
            rejectedFour = partialFour < threshold - 1.0e-5f;
        }
        if (rejectedFour) {
            ++fourLobeRejects;
            if (fullFour > threshold) ++fourLobeViolations;
        }

        const f32 fullThree = governedErosion(governedErosion(a, b, 0.04f), c, 0.02f);
        f32 partialThree = a;
        bool rejectedThree = partialThree < threshold - 1.0e-5f;
        if (!rejectedThree) {
            partialThree = governedErosion(partialThree, b, 0.04f);
            rejectedThree = partialThree < threshold - 1.0e-5f;
        }
        if (rejectedThree) {
            ++threeLobeRejects;
            if (fullThree > threshold) ++threeLobeViolations;
        }
        if (fullFour > a + 1.0e-6f || fullThree > a + 1.0e-6f) {
            ++supportExpansionViolations;
        }
        if (a == 0.0f && (fullFour != 0.0f || fullThree != 0.0f)) {
            ++emptySupportViolations;
        }
    }
    EXPECT_TRUE(fourLobeRejects > 0u);
    EXPECT_TRUE(threeLobeRejects > 0u);
    EXPECT_EQ(fourLobeViolations, 0u);
    EXPECT_EQ(threeLobeViolations, 0u);
    EXPECT_EQ(supportExpansionViolations, 0u);
    EXPECT_EQ(emptySupportViolations, 0u);
    // 独立三領域は最大侵食でも主形状の93%以上を残す。雲頂の主領域侵食26%を含めても
    // 雲芯は約69%残り、細部だけで連結した支持範囲を点群へ分断しない。
    EXPECT_TRUE((1.0f - 0.04f) * (1.0f - 0.02f) * (1.0f - 0.01f) > 0.93f);
    EXPECT_TRUE((1.0f - 0.26f) * (1.0f - 0.04f) * (1.0f - 0.02f) * (1.0f - 0.01f) > 0.68f);
    EXPECT_FALSE(Contains(shader, "cloudBaseShapeBand(a,sampleSpacing,1.0,height)*0.45"));

    // 占有判定は雲量を0.08広げるため、同じ大域標本を使う詳細密度より厳しくならない。
    for (u32 coverageStep = 0u; coverageStep <= 100u; ++coverageStep) {
        const f32 coverage =
            static_cast<f32>(coverageStep) / 100.0f;
        const f32 occupancyCoverage =
            SaturateForTest(coverage + 0.08f);
        for (u32 profileStep = 0u; profileStep <= 100u; ++profileStep) {
            const f32 profile =
                static_cast<f32>(profileStep) / 100.0f;
            const auto thresholdFor = [profile](f32 value) noexcept {
                const f32 clampedCoverage =
                    value < 0.72f ? value : 0.72f;
                const f32 cloudThreshold =
                    0.50f + (0.34f - 0.50f) * clampedCoverage;
                return 0.62f +
                       (cloudThreshold - 0.62f) * profile;
            };
            EXPECT_TRUE(
                thresholdFor(occupancyCoverage) <=
                thresholdFor(coverage) + 1.0e-6f);
        }
    }
}

ACS_TEST(VolumetricClouds,
         MacroDerivedTermsAreComputedOnceAndLightBasisIsFrameHoisted) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    const std::size_t shapeBegin =
        shader.find("floatcloudShapeFromMacro(");
    const std::size_t densityBegin =
        shader.find("floatcloudDensityFromMacro(");
    const std::size_t densityEnd =
        shader.find("returndensityResult;}", densityBegin);
    EXPECT_TRUE(shapeBegin != std::string::npos);
    EXPECT_TRUE(densityBegin != std::string::npos);
    EXPECT_TRUE(densityEnd != std::string::npos);
    if (shapeBegin != std::string::npos &&
        densityBegin != std::string::npos) {
        EXPECT_FALSE(Contains(
            shader.substr(
                shapeBegin, densityBegin - shapeBegin),
            "smoothstep(0.02,0.32,macro.profile)"));
    }
    if (densityBegin != std::string::npos &&
        densityEnd != std::string::npos) {
        EXPECT_FALSE(Contains(
            shader.substr(
                densityBegin, densityEnd - densityBegin),
            "smoothstep(0.02,0.32,macro.profile)"));
    }
    EXPECT_TRUE(Contains(
        shader, "floath=macro.height;"));
    EXPECT_FALSE(Contains(
        shader, "floath=heightFraction(p);"));

    const std::size_t mainEntry =
        shader.find("[numthreads(8,8,1)]voidCSCloud(");
    const std::size_t basis =
        shader.find(
            "float3lightTangent=cloudLightTangent.xyz;"
            "float3lightBitangent=cloudLightBitangent.xyz;",
            mainEntry);
    const std::size_t viewLoop =
        shader.find("[loop]for(inti=0;i<MAX_STEPS", mainEntry);
    EXPECT_TRUE(mainEntry != std::string::npos);
    EXPECT_TRUE(basis != std::string::npos);
    EXPECT_TRUE(viewLoop != std::string::npos);
    EXPECT_TRUE(basis < viewLoop);
    if (mainEntry != std::string::npos) {
        EXPECT_EQ(
            CountOccurrences(
                shader.substr(mainEntry),
                "float3lightTangent=cloudLightTangent.xyz;"),
            static_cast<std::size_t>(1));
        EXPECT_FALSE(Contains(
            shader.substr(mainEntry), "normalize(sunDir.xyz)"));
        EXPECT_FALSE(Contains(
            shader.substr(mainEntry), "cloudLightBasis("));
    }
    EXPECT_FALSE(Contains(
        shader, "floatbaseLightStep=cloudCoverageReciprocals.w;"));
    EXPECT_TRUE(Contains(
        shader, "floatlightStep=cloudLightStepFromBand(sampleUpperBand);"));

    const std::size_t lightPhase = shader.find("floatlightJitter=frac(jit+float(i)*0.61803398875);", viewLoop);
    const std::size_t pairedTrig = shader.find("floatconeSin,coneCos;" "sincos(6.2831853*lightJitter,coneSin,coneCos);", lightPhase);
    const std::size_t nearLightLoop =
        shader.find("[loop]for(intl=0;l<3;l++)", pairedTrig);
    const auto billowResidualCall = shader.find(
        "lightDepth+=cloudBillowLightDepthResidual("
        "lp,lightStep,coverage,sun,"
        "lightTangent,lightBitangent,"
        "coneSin,coneCos);",
        nearLightLoop);
    const std::size_t cacheCompileOut = shader.find(
        "if(!lightTerminated&&CLOUD_MAIN_SHADOW_CACHE_ENABLED){",
        nearLightLoop);
    const std::size_t farLightLoop =
        shader.find("[loop]for(intl=3;l<8;l++)", cacheCompileOut);
    const std::size_t coneDirection = shader.find(
        "float3coneDir=cloudConeDirection("
        "sun,lightTangent,lightBitangent,"
        "coneSin,coneCos,coneGeometry);",
        nearLightLoop);
    const std::size_t lightHalfStep = shader.find("float3lightHalfStep=coneDir*(0.5*lightStep);", coneDirection);
    const std::size_t lightAdvance =
        shader.find("lp+=lightHalfStep;", lightHalfStep);
    const std::size_t lightMacro = shader.find(
        "CloudMacroSamplelightMacro="
        "sampleCloudMacroLightingFromSlowFields("
        "lp,viewWeatherMask,coverageTerms.w,"
        "sharedLightProfileTerms,sharedLightBaseLift,sharedLightCurl,"
        "p,viewMacroUvw,macro.height,sharedShapeScale,lightStep);",
        lightAdvance);
    const std::size_t nearBillowVisibility = shader.find(
        "floatlightBillowVisibility="
        "cloudBillowVisibilityFromSampleSpacing(lightStep);",
        lightMacro);
    const std::size_t nearErosionVisibility = shader.find(
        "floatlightErosionVisibility="
        "cloudErosionVisibilityFromSampleSpacing(lightStep);",
        nearBillowVisibility);
    const std::size_t nearDensity = shader.find(
        "floatlightDensity=cloudDensityFromMacro("
        "lp,lightMacro,lightMacro.heightThreshold,"
        "lightMacro.weatherMask,lightBillowVisibility,"
        "lightErosionVisibility);",
        nearErosionVisibility);
    const std::size_t lightAccumulate = shader.find(
        "lightDepth+=lightDensity*lightStep*"
        "cloudOpticalDepthScaleFromBand("
        "lightMacro.upperBand>0.5);",
        nearDensity);
    const std::size_t lightFinish =
        shader.find("lp+=lightHalfStep;", lightAccumulate);
    const std::size_t recurrence = shader.find(
        "floatpreviousConeCos=coneCos;"
        "coneCos=previousConeCos*(-0.737368878)"
        "-coneSin*(-0.675490294);"
        "coneSin=coneSin*(-0.737368878)"
        "+previousConeCos*(-0.675490294);",
        lightAccumulate);
    EXPECT_TRUE(Contains(shader, "staticconstboolCLOUD_MAIN_SHADOW_CACHE_ENABLED=true;"));
    EXPECT_TRUE(nearLightLoop != std::string::npos);
    EXPECT_TRUE(farLightLoop != std::string::npos);
    EXPECT_TRUE(lightPhase != std::string::npos);
    EXPECT_TRUE(pairedTrig != std::string::npos);
    EXPECT_TRUE(cacheCompileOut != std::string::npos);
    EXPECT_TRUE(coneDirection != std::string::npos);
    EXPECT_TRUE(lightHalfStep != std::string::npos);
    EXPECT_TRUE(lightAdvance != std::string::npos);
    EXPECT_TRUE(lightMacro != std::string::npos);
    EXPECT_TRUE(nearBillowVisibility != std::string::npos);
    EXPECT_TRUE(nearErosionVisibility != std::string::npos);
    EXPECT_TRUE(nearDensity != std::string::npos);
    EXPECT_TRUE(lightAccumulate != std::string::npos);
    EXPECT_TRUE(lightFinish != std::string::npos);
    EXPECT_TRUE(recurrence != std::string::npos);
    EXPECT_TRUE(viewLoop < lightPhase);
    EXPECT_TRUE(lightPhase < pairedTrig);
    EXPECT_TRUE(pairedTrig < nearLightLoop);
    EXPECT_TRUE(nearLightLoop < coneDirection);
    EXPECT_TRUE(coneDirection < lightHalfStep);
    EXPECT_TRUE(lightHalfStep < lightAdvance);
    EXPECT_TRUE(lightAdvance < lightMacro);
    EXPECT_TRUE(lightMacro < nearBillowVisibility);
    EXPECT_TRUE(nearBillowVisibility < nearErosionVisibility);
    EXPECT_TRUE(nearErosionVisibility < nearDensity);
    EXPECT_TRUE(nearDensity < lightAccumulate);
    EXPECT_TRUE(lightAccumulate < lightFinish);
    EXPECT_TRUE(lightFinish < recurrence);
    EXPECT_TRUE(recurrence < billowResidualCall);
    EXPECT_TRUE(billowResidualCall < cacheCompileOut);
    EXPECT_TRUE(cacheCompileOut < farLightLoop);
    EXPECT_TRUE(Contains(shader, "floatlightStep=cloudLightStepFromBand(sampleUpperBand);" "lightStep*=lerp(0.72,1.28,lightJitter);"));
    EXPECT_FALSE(Contains(shader, "lightStep*=lerp(0.72,1.28,jit);"));
    EXPECT_FALSE(Contains(shader, "sincos(6.2831853*jit,coneSin,coneCos);"));
    if (nearLightLoop != std::string::npos &&
        farLightLoop != std::string::npos) {
        const std::string lightBody = shader.substr(
            nearLightLoop, farLightLoop - nearLightLoop);
        EXPECT_EQ(
            CountOccurrences(
                lightBody,
                "sampleCloudMacroLightingFromSlowFields("
                "lp,viewWeatherMask,coverageTerms.w,"
                "sharedLightProfileTerms,sharedLightBaseLift,sharedLightCurl,"
                "p,viewMacroUvw,macro.height,sharedShapeScale,lightStep)"),
            static_cast<std::size_t>(1));
        EXPECT_FALSE(Contains(
            lightBody, "sampleCloudMacroLighting(lp,coverage,lightStep)"));
        EXPECT_FALSE(Contains(lightBody, "?cloudDensity("));
        EXPECT_FALSE(Contains(lightBody, ":cloudShape("));
        EXPECT_EQ(CountOccurrences(lightBody, "sincos("),
                  static_cast<std::size_t>(0));
        EXPECT_FALSE(Contains(lightBody, "cos(conePhi)"));
        EXPECT_FALSE(Contains(lightBody, "sin(conePhi)"));
    }
    if (viewLoop != std::string::npos &&
        nearLightLoop != std::string::npos) {
        EXPECT_EQ(
            CountOccurrences(
                shader.substr(viewLoop, nearLightLoop - viewLoop), "sincos("),
                static_cast<std::size_t>(1));
    }
    if (nearLightLoop != std::string::npos) {
        const std::string completeLightSection = shader.substr(nearLightLoop);
        EXPECT_EQ(CountOccurrences(completeLightSection, "sampleCloudMacroLightingFromSlowFields(" "lp,viewWeatherMask,coverageTerms.w," "sharedLightProfileTerms,sharedLightBaseLift,sharedLightCurl," "p,viewMacroUvw,macro.height,sharedShapeScale,lightStep)"), static_cast<std::size_t>(1));
        EXPECT_EQ(CountOccurrences(completeLightSection, "sampleCloudMacroLighting(lp,coverage,lightStep)"), static_cast<std::size_t>(0));
        EXPECT_EQ(CountOccurrences(completeLightSection, "float2farLightSample=sampleCloudFarLightingDensityAndScale(" "lp,coverage,sharedLightCurl,lightStep);"), static_cast<usize>(1));
        EXPECT_FALSE(Contains(completeLightSection, "sampleCloudLightingDensityFromSlowFields("));
        EXPECT_EQ(CountOccurrences(completeLightSection, "floatlightDensity=" "cloudDensityFromMacro("), static_cast<std::size_t>(1));
        EXPECT_EQ(CountOccurrences(
                      completeLightSection,
                      "floatlightDensity="
                      "cloudShapeFromPositiveWeatherMacro("),
                  static_cast<std::size_t>(0));
        EXPECT_FALSE(Contains(completeLightSection, "[branch]if(l>=3)"));
    }
}

ACS_TEST(VolumetricClouds, LightMarchSamplesSegmentMidpointsAndPreservesTailOrigin) {
    constexpr f32 kStepLength = 8.0f;
    const auto decreasingLinearDensity = [kStepLength](f32 distance) noexcept { return 1.0f - distance / kStepLength; };
    const f32 exactDepth = 0.5f * kStepLength;
    const f32 rightEndpointDepth = decreasingLinearDensity(kStepLength) * kStepLength;
    const f32 midpointDepth = decreasingLinearDensity(0.5f * kStepLength) * kStepLength;

    // 終点法は区間内で濃度が下がる雲縁を見落とすが、中央法は一次変化の積分値と一致する。
    EXPECT_NEAR(rightEndpointDepth, 0.0f, 0.0f);
    EXPECT_NEAR(midpointDepth, exactDepth, 1e-6f);
    EXPECT_TRUE(std::fabs(rightEndpointDepth - exactDepth) > 0.49f * kStepLength);

    // layer.w は単純な層厚の逆数ではなく 1.6 / 層厚である。実際の単位で採取列を評価し、
    // テスト側だけ距離を1.6倍へ誤算しない。
    constexpr f32 kLayerHeight = 2500.0f;
    constexpr f32 kLayerCanonicalSpan = 1.6f;
    constexpr f32 kMinimumJitterScale = 0.72f;
    constexpr f32 kMaximumJitterScale = 1.28f;
    constexpr f32 kOldBaseStep = 0.012f;
    constexpr f32 kOldStepGrowth = 1.65f;
    constexpr f32 kBaseStep = 0.0075f;
    constexpr f32 kStepGrowth = 1.8f;
    constexpr f32 kRegionalWeatherFinestSpan = 9127.0f / 29.0f;
    constexpr f32 kMaximumCurlValueDifference = 2.0f;
    constexpr f32 kCurlWarpMetres = 22.0f;
    constexpr f32 kMaximumSharedCurlPositionError =
        kMaximumCurlValueDifference * kCurlWarpMetres;
    const f32 layerCanonicalScale =
        kLayerCanonicalSpan / kLayerHeight;
    EXPECT_NEAR(layerCanonicalScale, 0.00064f, 1e-8f);

    // 新しい列は同じ8標本とほぼ同じ到達距離を保ち、遠方の天候再採取範囲を狭めない。
    const f32 jitterScales[]{
        kMinimumJitterScale, 1.0f, kMaximumJitterScale};
    for (const f32 jitterScale : jitterScales) {
        f32 oldStep =
            kOldBaseStep / layerCanonicalScale * jitterScale;
        f32 newStep = kBaseStep / layerCanonicalScale * jitterScale;
        f32 oldDistance = 0.0f;
        f32 newDistance = 0.0f;
        f32 oldFarthestMidpoint = 0.0f;
        f32 newFarthestMidpoint = 0.0f;
        for (u32 lightSample = 0u;
             lightSample < kVolumetricCloudMaxLightMarchSamples;
             ++lightSample) {
            oldFarthestMidpoint = oldDistance + 0.5f * oldStep;
            newFarthestMidpoint = newDistance + 0.5f * newStep;
            oldDistance += oldStep;
            newDistance += newStep;
            oldStep *= kOldStepGrowth;
            newStep *= kStepGrowth;
        }
        EXPECT_TRUE(
            std::fabs(newDistance - oldDistance) < oldDistance * 0.03f);
        EXPECT_TRUE(
            std::fabs(newFarthestMidpoint - oldFarthestMidpoint) <
            oldFarthestMidpoint * 0.005f);
    }

    // 既定層では近距離3点だけが8～49 mの侵食帯域へ入る。4点目は最小位相でも
    // 48 mを越えて高周波侵食が消える一方、低周波の房は全位相で完全に残る。
    // 二つをまとめて低詳細度へ落とさない境界を固定する。
    f32 minimumDetailedStep =
        kBaseStep / layerCanonicalScale * kMinimumJitterScale;
    f32 maximumDetailedStep =
        kBaseStep / layerCanonicalScale * kMaximumJitterScale;
    f32 maximumNearEnd = 0.0f;
    for (u32 lightSample = 0u; lightSample < 3u; ++lightSample) {
        EXPECT_TRUE(
            CloudErosionVisibilityFromSampleSpacingForTest(
                minimumDetailedStep) > 0.0f);
        maximumNearEnd += maximumDetailedStep;
        minimumDetailedStep *= kStepGrowth;
        maximumDetailedStep *= kStepGrowth;
    }
    EXPECT_TRUE(minimumDetailedStep > 48.0f);
    EXPECT_TRUE(maximumDetailedStep > 48.0f);
    EXPECT_NEAR(
        CloudErosionVisibilityFromSampleSpacingForTest(
            maximumDetailedStep / kStepGrowth),
        0.0f, 0.0f);
    EXPECT_NEAR(
        CloudErosionVisibilityFromSampleSpacingForTest(
            minimumDetailedStep),
        0.0f, 0.0f);
    EXPECT_NEAR(
        CloudErosionVisibilityFromSampleSpacingForTest(
            maximumDetailedStep),
        0.0f, 0.0f);
    EXPECT_NEAR(
        CloudBillowVisibilityFromSampleSpacingForTest(
            minimumDetailedStep),
        1.0f, 0.0f);
    EXPECT_NEAR(
        CloudBillowVisibilityFromSampleSpacingForTest(
            maximumDetailedStep),
        1.0f, 0.0f);

    // キャッシュの低詳細度積分へ符号付き差分を足すと、同じ第4区間の房密度積分へ戻る。
    // 正差だけを足す方式は平均密度を増やして雲頂全体を暗くするため採用しない。
    constexpr f32 kLowLodDensity = 0.62f;
    constexpr f32 kBillowedDensity = 0.27f;
    constexpr f32 kOpticalScale = 0.00064f;
    const f32 cachedFourthDepth =
        kLowLodDensity * maximumDetailedStep * kOpticalScale;
    const f32 billowResidual =
        (kBillowedDensity - kLowLodDensity) *
        maximumDetailedStep * kOpticalScale;
    EXPECT_NEAR(
        cachedFourthDepth + billowResidual,
        kBillowedDensity * maximumDetailedStep * kOpticalScale,
        1.0e-7f);
    EXPECT_TRUE(billowResidual < 0.0f);

    f32 expandingStep =
        kBaseStep / layerCanonicalScale * kMaximumJitterScale;
    f32 traveledDistance = 0.0f;
    f32 farthestMidpoint = 0.0f;
    for (u32 lightSample = 0u;
         lightSample < kVolumetricCloudMaxLightMarchSamples;
         ++lightSample) {
        farthestMidpoint = traveledDistance + 0.5f * expandingStep;
        traveledDistance += expandingStep;
        expandingStep *= kStepGrowth;
    }
    EXPECT_NEAR(maximumNearEnd, 90.600f, 0.001f);
    EXPECT_NEAR(farthestMidpoint, 1588.328f, 0.002f);
    EXPECT_NEAR(traveledDistance, 2047.493f, 0.002f);
    EXPECT_TRUE(
        farthestMidpoint > kRegionalWeatherFinestSpan * 5.0f);
    EXPECT_NEAR(kMaximumSharedCurlPositionError, 44.0f, 1e-6f);
    EXPECT_TRUE(
        kMaximumSharedCurlPositionError <
        kRegionalWeatherFinestSpan * 0.15f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    const std::string compactSource = CompactShader(source);
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudBillowLightDepthResidual("
        "float3tailOrigin,floatlightStep,floatcoverage,"
        "float3sun,float3lightTangent,float3lightBitangent,"
        "floatconeSin,floatconeCos){"));
    EXPECT_TRUE(Contains(
        shader,
        "floatbillowVisibility="
        "cloudBillowVisibilityFromSampleSpacing(lightStep);"));
    EXPECT_TRUE(Contains(
        shader,
        "float3samplePosition=tailOrigin+coneDir*(0.5*lightStep);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatbillowedDensity=cloudDensityFromMacro("
        "samplePosition,macro,macro.heightThreshold,"
        "macro.weatherMask,billowVisibility,0.0);"));
    EXPECT_TRUE(Contains(
        shader,
        "residual=(billowedDensity-lowLodDensity)*lightStep*"
        "cloudOpticalDepthScaleFromBand("
        "macro.upperBand>0.5);"));
    EXPECT_TRUE(Contains(
        shader,
        "lightDepth+=cloudBillowLightDepthResidual("
        "lp,lightStep,coverage,sun,"
        "lightTangent,lightBitangent,"
        "coneSin,coneCos);"));
    EXPECT_TRUE(Contains(
        shader, "detailedLightDepth=max(lightDepth,0.0);"));
    EXPECT_TRUE(Contains(
        shader,
        "if(blendCachedTail&&!lightTerminated){"
        "floatexactTail=max(lightDepth-exactFarStart,0.0);"
        "lightDepth=exactFarStart+lerp("
        "exactTail,cachedTailForBlend,cacheBlendWeight);"
        "}lightDepth=max(lightDepth,0.0);"
        "floattauL=lightDepth*density*cloudLightingExtinction.y;"));
    EXPECT_TRUE(Contains(shader, "floatlightStep=0.0075/max(layer.w,1e-4);"));
    EXPECT_EQ(CountOccurrences(shader, "lightStep*=1.8;"), static_cast<std::size_t>(6));
    EXPECT_FALSE(Contains(shader, "lightStep*=1.65;"));
    EXPECT_TRUE(Contains(compactSource, "constf32lightStep=0.0075f/(layerCanonicalScale>0.0001f?layerCanonicalScale:0.0001f);"));
    EXPECT_FALSE(Contains(compactSource, "constf32lightStep=0.012f/"));
    EXPECT_EQ(CountOccurrences(shader, "float3lightHalfStep=coneDir*(0.5*lightStep);"), static_cast<std::size_t>(3));
    EXPECT_EQ(CountOccurrences(shader, "lp+=lightHalfStep;"), static_cast<std::size_t>(6));
    EXPECT_FALSE(Contains(shader, "lp+=coneDir*lightStep;"));

    const std::size_t cacheBegin = shader.find("floattraceCloudShadowPattern(");
    const std::size_t cacheEnd = shader.find("returnlightDepth;}", cacheBegin);
    EXPECT_TRUE(cacheBegin != std::string::npos);
    EXPECT_TRUE(cacheEnd != std::string::npos);
    if (cacheBegin != std::string::npos && cacheEnd != std::string::npos) {
        const std::string cacheBody = shader.substr(cacheBegin, cacheEnd - cacheBegin);
        EXPECT_EQ(CountOccurrences(cacheBody, "float3lightHalfStep=coneDir*(0.5*lightStep);"), static_cast<std::size_t>(1));
        EXPECT_EQ(CountOccurrences(cacheBody, "lp+=lightHalfStep;"), static_cast<std::size_t>(2));
        EXPECT_TRUE(Contains(cacheBody, "lp+=lightHalfStep;CloudMacroSamplelightMacro=sampleCloudMacroLighting(lp,coverage,lightStep);"));
        EXPECT_TRUE(Contains(cacheBody, "lightDepth+=lightDensity*lightStep*cloudOpticalDepthScaleFromBand(lightMacro.upperBand>0.5);lp+=lightHalfStep;lightStep*=1.8;"));
    }
    const std::size_t nearLoop = shader.find("[loop]for(intl=0;l<3;l++)");
    const std::size_t cacheAttempt = shader.find("if(!lightTerminated&&CLOUD_MAIN_SHADOW_CACHE_ENABLED){", nearLoop);
    const std::size_t nearTailOrigin = shader.rfind("lp+=lightHalfStep;", cacheAttempt);
    EXPECT_TRUE(nearLoop != std::string::npos);
    EXPECT_TRUE(cacheAttempt != std::string::npos);
    EXPECT_TRUE(nearTailOrigin != std::string::npos);
    EXPECT_TRUE(nearLoop < nearTailOrigin);
    EXPECT_TRUE(nearTailOrigin < cacheAttempt);
    EXPECT_TRUE(Contains(shader, "float3cachedTailSample=sampleCloudShadowTail(lp,density);"));
}

ACS_TEST(VolumetricClouds, EnvironmentCubemapSharesViewSamplingTermsIncludingUpperLayer)
{
    /** 空白を除去し、式と呼び出し順を検査できる描画実装。 */
    const auto compactSource = CompactShader(ReadSkySource());

    // 共有関数は定義1回と画面・環境キューブマップからの呼び出し2回だけにする。
    EXPECT_EQ(CountOccurrences(compactSource, "ResolveVolumetricCloudSamplingTerms_Internal("), static_cast<usize>(3));
    EXPECT_TRUE(Contains(compactSource, "out.coverage=FVec4{0.72f-0.36f*occupancyCoverage,0.72f-0.36f*safeCoverage,0.50f-0.16f*occupancyHeightCoverage,0.50f-0.16f*densityHeightCoverage};"));
    EXPECT_TRUE(Contains(compactSource, "constf32unclampedFineStep=0.035f/(horizontalNoiseScale>0.001f?horizontalNoiseScale:0.001f);"));
    EXPECT_TRUE(Contains(compactSource, "constf32lightStep=0.0075f/(layerCanonicalScale>0.0001f?layerCanonicalScale:0.0001f);"));
    EXPECT_TRUE(Contains(compactSource, "constf32upperLayerCanonicalScale=hasUpperLayer?1.6f/(upperLayer.top_height-upperLayer.base_height):layerCanonicalScale;"));
    EXPECT_TRUE(Contains(compactSource, "constf32upperLayerLightStep=0.0075f/(upperLayerCanonicalScale>0.0001f?upperLayerCanonicalScale:0.0001f);"));

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
    /** 空領域判定が使う高さ目標。 */
    constexpr f32 occupancyHeightTarget = 0.50f - 0.16f * occupancyCoverage;
    /** 密度積分が使う高さ目標。 */
    constexpr f32 densityHeightTarget = 0.50f - 0.16f * authoredCoverage;
    EXPECT_NEAR(occupancyWeatherThreshold, 0.54f, 1.0e-6f);
    EXPECT_NEAR(densityWeatherThreshold, 0.5688f, 1.0e-6f);
    EXPECT_NEAR(occupancyHeightTarget, 0.42f, 1.0e-6f);
    EXPECT_NEAR(densityHeightTarget, 0.4328f, 1.0e-6f);

    /** 厚さ2500mの下層を基準幅1.6へ写す光学尺度。 */
    constexpr f32 lowerCanonicalScale = 1.6f / 2500.0f;
    /** 下層の太陽方向に進める基準距離。 */
    constexpr f32 lowerLightStep = 0.0075f / lowerCanonicalScale;
    /** 厚さ1800mの上層を基準幅1.6へ写す光学尺度。 */
    constexpr f32 upperCanonicalScale = 1.6f / 1800.0f;
    /** 上層の太陽方向に進める基準距離。 */
    constexpr f32 upperLightStep = 0.0075f / upperCanonicalScale;
    EXPECT_NEAR(lowerCanonicalScale, 0.00064f, 1.0e-8f);
    EXPECT_NEAR(lowerLightStep, 11.71875f, 1.0e-5f);
    EXPECT_NEAR(upperCanonicalScale, 0.0008888889f, 1.0e-8f);
    EXPECT_NEAR(upperLightStep, 8.4375f, 1.0e-5f);
    EXPECT_TRUE(lowerLightStep > 0.0f);
    EXPECT_TRUE(upperLightStep > 0.0f);
}

ACS_TEST(VolumetricClouds, LightDensityAndOpticalScaleStayLayerCorrectAcrossEveryPath) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    // 被覆倍率は飽和前、濃さ倍率は飽和後に適用し、上層の薄さを過大評価しない。
    const std::size_t densityBegin = shader.find("floatcloudDensityFromMacro(");
    const std::size_t densityEnd = shader.find("returndensityResult;}", densityBegin);
    EXPECT_TRUE(densityBegin != std::string::npos);
    EXPECT_TRUE(densityEnd != std::string::npos);
    if (densityBegin != std::string::npos && densityEnd != std::string::npos) {
        const std::string body = shader.substr(densityBegin, densityEnd - densityBegin);
        const std::size_t layer = body.find("boolupperBand=macro.upperBand>0.5;");
        const std::size_t coverage = body.find("if(upperBand)weatherMask*=cloudUpperTerms.x;");
        const std::size_t densityValue = body.find("densityResult=cloudDensityFromPositiveWeatherMacro(");
        const std::size_t densityScale = body.find("if(upperBand)densityResult*=cloudUpperTerms.y;");
        EXPECT_TRUE(layer < coverage);
        EXPECT_TRUE(coverage < densityValue);
        EXPECT_TRUE(densityValue < densityScale);
    }
    const std::size_t profileBegin = shader.find("float2cloudLowLodDensityAndProfileFromMacro(");
    const std::size_t profileEnd = shader.find("returnresult;}", profileBegin);
    EXPECT_TRUE(profileBegin != std::string::npos);
    EXPECT_TRUE(profileEnd != std::string::npos);
    if (profileBegin != std::string::npos && profileEnd != std::string::npos) {
        const std::string body = shader.substr(profileBegin, profileEnd - profileBegin);
        const std::size_t layer = body.find("boolupperBand=macro.upperBand>0.5;");
        const std::size_t coverage = body.find("if(upperBand)weatherMask*=cloudUpperTerms.x;");
        const std::size_t profileValue = body.find("result=cloudLowLodDensityAndProfileFromPositiveWeatherMacro(");
        const std::size_t densityScale = body.find("if(upperBand)result.x*=cloudUpperTerms.y;");
        EXPECT_TRUE(layer < coverage);
        EXPECT_TRUE(coverage < profileValue);
        EXPECT_TRUE(profileValue < densityScale);
    }
    EXPECT_TRUE(Contains(shader, "floatdens=cloudDensityFromMacro(p,macro,densityHeightThreshold,viewWeatherMask,billowVisibility,erosionVisibility)*density*distanceFade;"));
    EXPECT_TRUE(Contains(shader, "floatlightDensity=cloudDensityFromMacro(lp,lightMacro,lightMacro.heightThreshold,lightMacro.weatherMask,lightBillowVisibility,lightErosionVisibility);"));
    EXPECT_TRUE(Contains(shader, "float2lowLodDensityAndProfile=cloudLowLodDensityAndProfileFromMacro(macro,densityHeightThreshold,viewWeatherMask);"));
    EXPECT_TRUE(Contains(shader, "floatlowLodDensity=lowLodDensityAndProfile.x;"));
    EXPECT_FALSE(Contains(shader, "boolinUpperCloudBand(float3p)"));
    EXPECT_TRUE(Contains(shader, "floatupperBand;"));
    EXPECT_TRUE(Contains(shader, "macro.upperBand=upperBand?1.0:0.0;"));

    // 上層用の尺度と光採取間隔は CPU で一度だけ求め、GPU は既に判定した層から選ぶ。
    EXPECT_TRUE(Contains(shader, "floatcloudOpticalDepthScaleFromBand(boolupperBand){floatscale=layer.w;if(upperBand)scale=cloudUpperTerms.z;returnscale;}"));
    EXPECT_TRUE(Contains(shader, "floatcloudLightStepFromBand(boolupperBand){floatlightStep=cloudCoverageReciprocals.w;if(upperBand)lightStep=cloudUpperTerms.w;returnlightStep;}"));
    const auto compactSource = CompactShader(source);
    EXPECT_TRUE(Contains(compactSource, "constf32upperLayerCanonicalScale=hasUpperLayer?1.6f/(upperLayer.top_height-upperLayer.base_height):layerCanonicalScale;"));
    EXPECT_TRUE(Contains(compactSource, "out.upperTerms=FVec4{upperLayer.coverage_scale,upperLayer.density_scale,upperLayerCanonicalScale,upperLayerLightStep};"));
    EXPECT_EQ(CountOccurrences(compactSource, "cb.cloudUpperTerms=samplingTerms.upperTerms;"), static_cast<usize>(2));

    // 高度と降水の補正は詳細密度と低詳細度密度で共有し、遠距離だけ式を落とさない。
    EXPECT_EQ(CountOccurrences(shader, "cloudHeightPrecipitationDensityScale("), static_cast<std::size_t>(4));
    EXPECT_TRUE(Contains(shader, "floatcloudHeightPrecipitationDensityScale(" "floatheight,floatprecipitation){" "returnlerp(1.10,0.92,height)*" "lerp(1.0,1.28,precipitation);}"));
    EXPECT_FALSE(Contains(shader, "sampleCloudLightingDensityFromSlowFields("));
    EXPECT_FALSE(Contains(shader, "sampleCloudLightingShapeFromSlowFields("));
    const std::size_t mainCloudEntry =
        shader.find("[numthreads(8,8,1)]voidCSCloud(");
    const std::size_t farLightLoop =
        shader.find("[loop]for(intl=3;l<8;l++){", mainCloudEntry);
    const std::size_t farLightEnd =
        shader.find("if(blendCachedTail&&!lightTerminated){", farLightLoop);
    EXPECT_TRUE(mainCloudEntry != std::string::npos);
    EXPECT_TRUE(farLightLoop != std::string::npos);
    EXPECT_TRUE(farLightEnd != std::string::npos);
    if (farLightLoop != std::string::npos &&
        farLightEnd != std::string::npos) {
        const std::string farLight =
            shader.substr(farLightLoop, farLightEnd - farLightLoop);
        EXPECT_TRUE(Contains(
            farLight,
            "float2farLightSample=sampleCloudFarLightingDensityAndScale("
            "lp,coverage,sharedLightCurl,lightStep);"));
        EXPECT_TRUE(Contains(
            farLight,
            "lightDepth+=farLightSample.x*lightStep*farLightSample.y;"));
        EXPECT_FALSE(Contains(
            farLight, "sampleCloudMacroLighting(lp,coverage,lightStep)"));
    }

    const std::size_t cacheBegin = shader.find("floattraceCloudShadowPattern(");
    const std::size_t cacheEnd = shader.find("returnlightDepth;}", cacheBegin);
    EXPECT_TRUE(cacheBegin != std::string::npos);
    EXPECT_TRUE(cacheEnd != std::string::npos);
    if (cacheBegin != std::string::npos && cacheEnd != std::string::npos) {
        const std::string cacheBody = shader.substr(cacheBegin, cacheEnd - cacheBegin);
        EXPECT_TRUE(Contains(cacheBody, "floatlightDensity=cloudLowLodDensityFromMacro(" "lightMacro,lightMacro.heightThreshold," "lightMacro.weatherMask);"));
        EXPECT_TRUE(Contains(cacheBody, "cloudOpticalDepthScaleFromBand(" "lightMacro.upperBand>0.5);"));
        EXPECT_FALSE(Contains(cacheBody, "cloudShapeFromMacro(lightMacro)"));
    }

    // 旧遠距離式が落としていた差を、飽和しない代表値で数値として固定する。
    constexpr f32 kBaseDensity = 0.50f;
    constexpr f32 kWeatherMask = 0.80f;
    constexpr f32 kProfileClosure = 0.75f;
    constexpr f32 kHeight = 0.0f;
    constexpr f32 kPrecipitation = 1.0f;
    const FVolumetricCloudUpperLayer upper{};
    const f32 heightPrecipitationScale =
        (1.10f + (0.92f - 1.10f) * kHeight) *
        (1.0f + (1.28f - 1.0f) * kPrecipitation);
    const f32 formerFarDensity =
        kBaseDensity * kWeatherMask * kProfileClosure;
    const f32 lowerDensity = SaturateForTest(formerFarDensity * heightPrecipitationScale);
    const f32 upperDensity = SaturateForTest(formerFarDensity * upper.coverage_scale * heightPrecipitationScale) * upper.density_scale;
    EXPECT_NEAR(formerFarDensity, 0.30f, 1e-6f);
    EXPECT_NEAR(lowerDensity, 0.4224f, 1e-6f);
    EXPECT_NEAR(upperDensity, 0.069696f, 1e-6f);
    EXPECT_TRUE(formerFarDensity > upperDensity * 4.0f);

    // 一様密度の層全体は、厚さによらず同じ 1.6 の基準光学的深さへ写す。
    constexpr f32 kLowerThickness = 2500.0f;
    constexpr f32 kUpperThickness = 900.0f;
    constexpr f32 kCanonicalSpan = 1.6f;
    const f32 lowerOpticalScale = kCanonicalSpan / kLowerThickness;
    const f32 upperOpticalScale = kCanonicalSpan / kUpperThickness;
    const f32 formerUpperDepth = kUpperThickness * lowerOpticalScale;
    const f32 correctedUpperDepth = kUpperThickness * upperOpticalScale;
    EXPECT_NEAR(kLowerThickness * lowerOpticalScale, kCanonicalSpan, 1e-6f);
    EXPECT_NEAR(formerUpperDepth, 0.576f, 1e-6f);
    EXPECT_NEAR(correctedUpperDepth, kCanonicalSpan, 1e-6f);
    EXPECT_TRUE(formerUpperDepth < correctedUpperDepth * 0.37f);

    // 視線、近距離光、遠距離光、影キャッシュ、ワールド影の全積分で標本側の尺度を使う。
    EXPECT_TRUE(Contains(shader, "floatlightStep=cloudLightStepFromBand(sampleUpperBand);"));
    EXPECT_TRUE(Contains(shader, "lightDepth+=lightDensity*lightStep*cloudOpticalDepthScaleFromBand(lightMacro.upperBand>0.5);"));
    EXPECT_TRUE(Contains(shader, "opticalDepth+=sampleDensity*stepLength*cloudOpticalDepthScaleFromBand(macro.upperBand>0.5);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatviewSampleOpticalDepth=dens*stepLength*"
        "sampleOpticalDepthScale*cloudLightingExtinction.x;"));
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
         HeightThresholdCacheRemovesDuplicateInnerLoopEvaluation) {
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
    const std::string viewMacro = functionBody(
        "CloudMacroSamplesampleCloudMacro("
        "float3p,float4coverageTerms,");
    const std::string genericLightMacro = functionBody(
        "CloudMacroSamplesampleCloudMacroLighting("
        "float3p,floatweatherCoverage,floatsampleSpacing)");
    const std::string sharedLightMacro = functionBody(
        "CloudMacroSamplesampleCloudMacroLightingFromSlowFields(");
    EXPECT_TRUE(!viewMacro.empty());
    EXPECT_TRUE(!genericLightMacro.empty());
    EXPECT_TRUE(!sharedLightMacro.empty());

    // The view macro legitimately produces one threshold for conservative
    // occupancy and one for authored density. Their coverage-only targets are
    // computed before the ray loop; every shared light probe receives that
    // same target without rebuilding coverage policy.
    EXPECT_EQ(
        CountOccurrences(viewMacro, "cloudHeightThresholdFromTarget("),
        static_cast<std::size_t>(2));
    EXPECT_EQ(
        CountOccurrences(genericLightMacro, "cloudHeightThreshold("),
        static_cast<std::size_t>(1));
    EXPECT_EQ(
        CountOccurrences(sharedLightMacro, "cloudHeightThresholdFromTarget("),
        static_cast<std::size_t>(1));
    EXPECT_TRUE(Contains(
        viewMacro,
        "macro.heightThreshold=cloudHeightThresholdFromTarget("
        "coverageTerms.z,profileShape)-cloudWeatherCoreShapeOffset("
        "macro.weatherMask,macro.height,upperBand);"
        "densityHeightThreshold=cloudHeightThresholdFromTarget("
        "coverageTerms.w,profileShape)-cloudWeatherCoreShapeOffset("
        "macro.densityWeatherMask,macro.height,upperBand);"));
    EXPECT_TRUE(Contains(
        sharedLightMacro,
        "cloudBaseShapeLighting("
        "lightingUvw,"
        "macro.heightThreshold-cloudBillowMaximumOffset(macro.height),"
        "sampleSpacing,"
        "macro.height,"
        "macro.baseNoise);"));

    const std::size_t shapeBegin =
        shader.find("floatcloudShapeFromPositiveWeatherMacro(");
    const std::size_t densityBegin =
        shader.find("floatcloudDensityFromPositiveWeatherMacro(", shapeBegin);
    const std::size_t densityEnd =
        shader.find("returndensityResult;}", densityBegin);
    EXPECT_TRUE(shapeBegin != std::string::npos);
    EXPECT_TRUE(densityBegin != std::string::npos);
    EXPECT_TRUE(densityEnd != std::string::npos);
    if (shapeBegin != std::string::npos &&
        densityBegin != std::string::npos) {
        const std::string shapeBody =
            shader.substr(shapeBegin, densityBegin - shapeBegin);
        EXPECT_EQ(
            CountOccurrences(shapeBody, "cloudHeightThreshold("),
            static_cast<std::size_t>(0));
        EXPECT_TRUE(Contains(
            shapeBody,
            "floatenvelopeNoise=cloudWeatheredBaseNoise("
            "macro.baseNoise+cloudBillowMaximumOffset(macro.height),"
            "macro.weatherMask);"));
        EXPECT_TRUE(Contains(shapeBody, "floatenvelopeBaseDensity=remapc(" "envelopeNoise,macro.heightThreshold," "min(macro.heightThreshold+0.22,0.98),0.0,1.0);" "envelopeBaseDensity=cloudConvectiveBodyDensity(" "envelopeBaseDensity,macro.height,macro.weather.g,macro.weather.b);" "shapeResult=cloudDimensionalDensity(" "envelopeBaseDensity,macro.heightProfile);"));
    }
    if (densityBegin != std::string::npos &&
        densityEnd != std::string::npos) {
        const std::string densityBody =
            shader.substr(
                densityBegin, densityEnd - densityBegin);
        EXPECT_EQ(
            CountOccurrences(densityBody, "cloudHeightThreshold("),
            static_cast<std::size_t>(0));
        EXPECT_TRUE(Contains(
            densityBody,
            "weatheredBaseNoise,heightThreshold,"
            "min(heightThreshold+0.22,0.98)"));
    }

    // 事前計算した対象しきい値が、代表的な雲量と高さ形状で直接計算と一致する。
    const auto threshold = [](f32 coverage, f32 profileShape) noexcept {
        const f32 saturatedCoverage =
            coverage < 0.0f ? 0.0f : (coverage > 1.0f ? 1.0f : coverage);
        const f32 limitedCoverage =
            saturatedCoverage < 0.72f ? saturatedCoverage : 0.72f;
        const f32 cloudThreshold =
            0.50f + (0.34f - 0.50f) * limitedCoverage;
        return 0.62f +
               (cloudThreshold - 0.62f) * profileShape;
    };
    const auto target = [](f32 coverage) noexcept {
        const f32 saturatedCoverage =
            coverage < 0.0f ? 0.0f : (coverage > 1.0f ? 1.0f : coverage);
        const f32 limitedCoverage =
            saturatedCoverage < 0.72f ? saturatedCoverage : 0.72f;
        return 0.50f + (0.34f - 0.50f) * limitedCoverage;
    };
    const f32 coverages[] = {0.0f, 0.08f, 0.37f, 0.72f, 1.0f};
    const f32 profileShapes[] = {0.0f, 0.01f, 0.5f, 0.99f, 1.0f};
    for (const f32 coverage : coverages) {
        const f32 occupancyCoverage =
            coverage + 0.08f > 1.0f ? 1.0f : coverage + 0.08f;
        for (const f32 profileShape : profileShapes) {
            const f32 formerOccupancy =
                threshold(occupancyCoverage, profileShape);
            const f32 formerDensity =
                threshold(coverage, profileShape);
            const f32 cachedOccupancy =
                0.62f +
                (target(occupancyCoverage) - 0.62f) * profileShape;
            const f32 cachedDensity =
                0.62f +
                (target(coverage) - 0.62f) * profileShape;
            EXPECT_NEAR(cachedOccupancy, formerOccupancy, 0.0f);
            EXPECT_NEAR(cachedDensity, formerDensity, 0.0f);
        }
    }

    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 384u);
    EXPECT_EQ(kVolumetricCloudMaxLightMarchSamples, 8u);
    EXPECT_TRUE(Contains(shader, "[loop]for(intl=0;l<3;l++){"));
    EXPECT_TRUE(Contains(shader, "[loop]for(intl=3;l<8;l++){"));
}

ACS_TEST(VolumetricClouds,
         DenseRayInvariantHoistsPreserveExactSamplingPolicyAndDomains) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    const std::size_t coverageTerms = shader.find(
        "float4coverageTerms=cloudCoverage;");
    const std::size_t viewLoop =
        shader.find("[loop]for(inti=0;i<MAX_STEPS", coverageTerms);
    const std::size_t sharedProfile = shader.find(
        "float4sharedLightProfileTerms=float4("
        "cloudProfileTypeWeights(macro.weather.g),"
        "macro.weather.b,cloudColumnHeightShift("
        "macro.weather,macro.densityWeatherMask));",
        viewLoop);
    const std::size_t sharedBaseLift = shader.find(
        "floatsharedLightBaseLift=cloudColumnBaseLift("
        "macro.weather,macro.densityWeatherMask);",
        sharedProfile);
    const std::size_t nearLightLoop =
        shader.find("[loop]for(intl=0;l<3;l++)", sharedBaseLift);
    EXPECT_TRUE(coverageTerms != std::string::npos);
    EXPECT_TRUE(viewLoop != std::string::npos);
    EXPECT_TRUE(sharedProfile != std::string::npos);
    EXPECT_TRUE(sharedBaseLift != std::string::npos);
    EXPECT_TRUE(nearLightLoop != std::string::npos);
    EXPECT_TRUE(coverageTerms < viewLoop);
    EXPECT_TRUE(viewLoop < sharedProfile);
    EXPECT_TRUE(sharedProfile < sharedBaseLift);
    EXPECT_TRUE(sharedBaseLift < nearLightLoop);
    if (viewLoop != std::string::npos) {
        const std::string denseRay = shader.substr(viewLoop);
        EXPECT_FALSE(Contains(denseRay, "cloudWeatherThreshold("));
        EXPECT_FALSE(Contains(denseRay, "cloudHeightThresholdTarget("));
    }
    if (nearLightLoop != std::string::npos) {
        const std::string lightLoops = shader.substr(nearLightLoop);
        EXPECT_EQ(
            CountOccurrences(lightLoops, "cloudProfileTypeWeights("),
            static_cast<std::size_t>(0));
        EXPECT_EQ(
            CountOccurrences(
                lightLoops, "sampleCloudMacroLightingFromSlowFields("),
            static_cast<std::size_t>(1));
        EXPECT_EQ(
            CountOccurrences(
                lightLoops, "sampleCloudFarLightingDensityAndScale("
                            "lp,coverage,sharedLightCurl,lightStep)"),
            static_cast<std::size_t>(1));
        EXPECT_FALSE(Contains(
            lightLoops, "sampleCloudLightingDensityFromSlowFields("));
    }

    const std::size_t sharedMacro = shader.find(
        "CloudMacroSamplesampleCloudMacroLightingFromSlowFields(");
    const std::size_t sharedMacroEnd =
        shader.find("returnmacro;}", sharedMacro);
    EXPECT_TRUE(sharedMacro != std::string::npos);
    EXPECT_TRUE(sharedMacroEnd != std::string::npos);
    if (sharedMacro != std::string::npos &&
        sharedMacroEnd != std::string::npos) {
        const std::string helper =
            shader.substr(sharedMacro, sharedMacroEnd - sharedMacro);
        EXPECT_TRUE(Contains(
            helper,
            "cloudProfileFromTypeWeights("
            "macro.height,slowProfileTerms.xy,slowProfileTerms.z);"));
        EXPECT_FALSE(Contains(helper, "cloudProfileTypeWeights("));
        EXPECT_FALSE(Contains(helper, "cloudWeatherThreshold("));
    }

    EXPECT_FALSE(Contains(
        shader, "floatsampleCloudLightingDensityFromSlowFields("));

    const std::size_t farDensity = shader.find(
        "float2sampleCloudFarLightingDensityAndScale(");
    const std::size_t farDensityEnd =
        shader.find("returnfloat2(densityResult,cloudOpticalDepthScaleFromBand(upperBand));}", farDensity);
    EXPECT_TRUE(farDensity != std::string::npos);
    EXPECT_TRUE(farDensityEnd != std::string::npos);
    if (farDensity != std::string::npos &&
        farDensityEnd != std::string::npos) {
        const std::string helper =
            shader.substr(farDensity, farDensityEnd - farDensity);
        EXPECT_TRUE(Contains(
            helper,
            "float4weather=cloudWeatherData(p,layerHeight,upperBand);"));
        EXPECT_TRUE(Contains(
            helper,
            "cloudColumnHeightShift(weather,weatherMask),"
            "cloudColumnBaseLift(weather,weatherMask),upperBand);"));
        EXPECT_TRUE(Contains(
            helper,
            "cloudUVW(p,weather,slowCurl,sampleHeight)"));
        const std::size_t upperCoverage = helper.find(
            "if(upperBand)weatherMask*=cloudUpperTerms.x;");
        const std::size_t weatherErosion = helper.find(
            "floatweatheredBaseNoise=cloudWeatheredBaseNoise("
            "baseNoise,weatherMask);");
        const std::size_t densityRemap = helper.find(
            "floatbaseDensity=remapc("
            "weatheredBaseNoise,heightThreshold,");
        const std::size_t dimensionalDensity = helper.find("floatdimensionalDensity=cloudDimensionalDensity(" "baseDensity,heightProfile);");
        const std::size_t densityScale = helper.find(
            "dimensionalDensity*"
            "cloudHeightPrecipitationDensityScale(");
        EXPECT_TRUE(upperCoverage != std::string::npos);
        EXPECT_TRUE(weatherErosion != std::string::npos);
        EXPECT_TRUE(densityRemap != std::string::npos);
        EXPECT_TRUE(densityScale != std::string::npos);
        EXPECT_TRUE(upperCoverage < weatherErosion);
        EXPECT_TRUE(weatherErosion < densityRemap);
        EXPECT_TRUE(densityRemap < dimensionalDensity);
        EXPECT_TRUE(densityRemap < densityScale);
        EXPECT_FALSE(Contains(helper, "baseDensity*weatherMask*heightProfile"));
        EXPECT_FALSE(Contains(helper, "cloudCurlOffset("));
        EXPECT_FALSE(Contains(helper, "CloudMacroSample"));
    }

    EXPECT_TRUE(Contains(
        shader,
        "floatwarpSin,warpCos;"
        "sincos(warpAngle,warpSin,warpCos);"
        "float2weatherWarp=float2(warpCos,warpSin)"));
    EXPECT_FALSE(Contains(
        shader,
        "float2weatherWarp=float2(cos(warpAngle),sin(warpAngle))"));
    EXPECT_TRUE(Contains(
        shader,
        "voidcloudDetailDomains("
        "float2detailXz,floatworldY,"
        "outfloat3detailDomainA,outfloat3detailDomainB)"));
    EXPECT_TRUE(Contains(
        shader,
        "cloudDetailDomains(detailXz,p.y,detailDomainA,detailDomainB);"));
    EXPECT_FALSE(Contains(shader, "rotateNoise(detailPosA)"));
    EXPECT_FALSE(Contains(shader, "rotateNoise(detailPosB)"));

    const std::size_t densityBegin =
        shader.find("floatcloudDensityFromPositiveWeatherMacro(");
    const std::size_t densityEnd =
        shader.find("returndensityResult;}", densityBegin);
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

    const auto profile = [](f32 h, f32 typeWeightA,
                            f32 typeWeightB, f32 precipitation) noexcept {
        const f32 stratus =
            SmoothStepForTest(0.0f, 0.055f, h) *
            (1.0f - SmoothStepForTest(0.30f, 0.56f, h));
        const f32 stratocumulus =
            SmoothStepForTest(0.0f, 0.09f, h) *
            (1.0f - SmoothStepForTest(0.54f, 0.84f, h)) *
            (0.78f + 0.22f *
                SmoothStepForTest(0.12f, 0.45f, h));
        const f32 cumulus =
            SmoothStepForTest(0.0f, 0.13f, h) *
            (1.0f - SmoothStepForTest(0.76f, 1.0f, h)) *
            (0.64f + 0.36f *
                SmoothStepForTest(0.16f, 0.62f, h));
        f32 value =
            stratus + (stratocumulus - stratus) * typeWeightA;
        value = value + (cumulus - value) * typeWeightB;
        const f32 stormBody =
            SmoothStepForTest(0.06f, 0.22f, h) *
            (1.0f - 0.42f * SmoothStepForTest(0.48f, 0.82f, h)) *
            (1.0f - SmoothStepForTest(0.84f, 0.98f, h));
        const f32 anvil =
            SmoothStepForTest(0.58f, 0.72f, h) *
            (1.0f - SmoothStepForTest(0.90f, 0.99f, h)) * 0.84f;
        const f32 storm = stormBody > anvil ? stormBody : anvil;
        const f32 typeTower =
            SmoothStepForTest(0.72f, 0.98f, typeWeightB);
        const f32 precipitationTower =
            SmoothStepForTest(0.25f, 0.85f, precipitation);
        const f32 toweringStrength =
            typeTower > precipitationTower ? typeTower : precipitationTower;
        const f32 stormMix = precipitation * 0.72f > toweringStrength * 0.92f
            ? precipitation * 0.72f
            : toweringStrength * 0.92f;
        return SaturateForTest(
            value + (storm - value) * stormMix);
    };
    const f32 heights[] = {0.0f, 0.03f, 0.19f, 0.51f, 0.83f, 1.0f};
    const f32 cloudTypes[] = {0.0f, 0.23f, 0.51f, 0.77f, 1.0f};
    const f32 precipitation[] = {0.0f, 0.35f, 1.0f};
    for (const f32 cloudType : cloudTypes) {
        const f32 cachedA =
            SmoothStepForTest(0.18f, 0.52f, cloudType);
        const f32 cachedB =
            SmoothStepForTest(0.50f, 0.84f, cloudType);
        for (const f32 h : heights) {
            for (const f32 rain : precipitation) {
                const f32 former = profile(
                    h,
                    SmoothStepForTest(0.18f, 0.52f, cloudType),
                    SmoothStepForTest(0.50f, 0.84f, cloudType),
                    rain);
                const f32 hoisted =
                    profile(h, cachedA, cachedB, rain);
                EXPECT_NEAR(hoisted, former, 0.0f);
            }
        }
    }

    const f32 stormLower = profile(0.10f, 1.0f, 1.0f, 1.0f);
    const f32 stormBody = profile(0.40f, 1.0f, 1.0f, 1.0f);
    const f32 stormShoulder = profile(0.76f, 1.0f, 1.0f, 1.0f);
    const f32 stormAnvil = profile(0.90f, 1.0f, 1.0f, 1.0f);
    EXPECT_TRUE(stormBody > stormLower);
    EXPECT_TRUE(stormAnvil > 0.6f);
    EXPECT_TRUE(stormBody > stormShoulder);

    const auto rotate = [](FVec3 q) noexcept {
        return FVec3{
            q.y * 0.800f + q.z * 0.600f,
            q.x * -0.707f + q.y * -0.424f + q.z * 0.566f,
            q.x * 0.707f + q.y * -0.424f + q.z * 0.566f};
    };
    const FVec3 detailPoints[] = {
        FVec3{0.0f, 0.0f, 0.0f},
        FVec3{123.5f, 1500.0f, -77.25f},
        FVec3{-48000.0f, 8500.0f, 62000.0f},
        FVec3{250000.0f, -1200.0f, -250000.0f}};
    for (const FVec3 point : detailPoints) {
        const FVec3 horizontal{
            point.z * 0.600f,
            -point.x * 0.707f + point.z * 0.566f,
             point.x * 0.707f + point.z * 0.566f};
        const FVec3 vertical{
            point.y * 0.800f,
            point.y * -0.424f,
            point.y * -0.424f};
        const FVec3 formerA = rotate(FVec3{
            point.x * 0.0011f,
            point.y * 0.0018f,
            point.z * 0.0011f});
        const FVec3 formerB = rotate(FVec3{
            point.x * 0.0023f,
            point.y * 0.0030f,
            point.z * 0.0023f});
        const FVec3 hoistedA{
            horizontal.x * 0.0011f + vertical.x * 0.0018f,
            horizontal.y * 0.0011f + vertical.y * 0.0018f,
            horizontal.z * 0.0011f + vertical.z * 0.0018f};
        const FVec3 hoistedB{
            horizontal.x * 0.0023f + vertical.x * 0.0030f,
            horizontal.y * 0.0023f + vertical.y * 0.0030f,
            horizontal.z * 0.0023f + vertical.z * 0.0030f};
        const auto near = [](f32 expected, f32 actual) noexcept {
            return std::fabs(expected - actual) <=
                std::fabs(expected) * 2.0e-6f + 2.0e-6f;
        };
        EXPECT_TRUE(near(formerA.x, hoistedA.x));
        EXPECT_TRUE(near(formerA.y, hoistedA.y));
        EXPECT_TRUE(near(formerA.z, hoistedA.z));
        EXPECT_TRUE(near(formerB.x, hoistedB.x));
        EXPECT_TRUE(near(formerB.y, hoistedB.y));
        EXPECT_TRUE(near(formerB.z, hoistedB.z));
    }

    EXPECT_TRUE(Contains(
        shader,
        "intMAX_STEPS=(int)cloudLightingAmbient.z;"
        "if(MAX_STEPS<32)MAX_STEPS=32;"));
    EXPECT_TRUE(Contains(shader, "[loop]for(intl=0;l<3;l++){"));
    EXPECT_TRUE(Contains(shader, "[loop]for(intl=3;l<8;l++){"));
    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 384u);
    EXPECT_EQ(kVolumetricCloudMaxLightMarchSamples, 8u);
    EXPECT_EQ(kVolumetricCloudUltraTraceDivisor, 4u);
}

ACS_TEST(VolumetricClouds,
         NearLightSharedFieldSpecializationPreservesConsumerAndWorldDomain) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    // The main view consumer remains the sole 0.006 empty-space policy. The
    // rejected pre-fetch product bound must not silently return through a
    // different threshold before the four-lobe macro field is evaluated.
    EXPECT_TRUE(Contains(
        shader,
        "floatshape=cloudShapeFromMacro(macro);"
        "if(shape<=0.006){"));
    EXPECT_FALSE(Contains(shader, "if(macro.weatherMask*macro.heightProfile>0.006){"));

    const std::size_t helperBegin = shader.find(
        "CloudMacroSamplesampleCloudMacroLightingFromSlowFields(");
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
            "float3lightingUvw=referenceUvw+float3("
            "(p.x-referenceP.x)*shapeScale,"
            "(macro.height-referenceHeight)*1.55,"
            "(p.z-referenceP.z)*shapeScale);"));
        EXPECT_FALSE(Contains(helper, "camPos"));
        EXPECT_FALSE(Contains(helper, "cloudWindWorld("));
        EXPECT_FALSE(Contains(helper, "cloudUVW("));
        EXPECT_FALSE(Contains(helper, "cloudCurlOffset("));
    }

    // 近距離では視線標本の天候と渦を共有するため、cloudUVW はワールド XZ と
    // 正規化高度に対して一次式となる。基準座標からの復元はカメラ位置に依存しない。
    const FVec2 wind{183.25f, -91.75f};
    const FVec2 fixedWarp{-37.0f, 52.5f};
    constexpr f32 kShapeScale = 0.00021f;
    constexpr f32 kWeatherType = 0.63f;
    constexpr f32 kWeatherAnvil = 0.28f;
    const auto legacyUvw =
        [&](FVec3 point, f32 height) noexcept {
            const f32 canonicalY =
                height * 1.55f +
                kWeatherType * 0.09f +
                kWeatherAnvil * 0.05f + 0.07f;
            return FVec3{
                (point.x - wind.x + fixedWarp.x) * kShapeScale,
                canonicalY,
                (point.z - wind.y + fixedWarp.y) * kShapeScale};
        };
    const auto affineProbeUvw =
        [&](FVec3 point, f32 height,
            FVec3 referencePoint, f32 referenceHeight) noexcept {
            const FVec3 reference =
                legacyUvw(referencePoint, referenceHeight);
            return FVec3{
                reference.x +
                    (point.x - referencePoint.x) * kShapeScale,
                reference.y +
                    (height - referenceHeight) * 1.55f,
                reference.z +
                    (point.z - referencePoint.z) * kShapeScale};
        };
    struct FUvwCase {
        FVec3 reference;
        f32 reference_height;
        FVec3 probe;
        f32 probe_height;
    };
    const FUvwCase uvwCases[]{
        {{0.0f, 1500.0f, 0.0f}, 0.0f,
         {12.0f, 1600.0f, -8.0f}, 0.04f},
        {{-48000.0f, 2300.0f, 62000.0f}, 0.32f,
         {-47912.0f, 2650.0f, 62141.0f}, 0.46f},
        {{249500.0f, 3900.0f, -249200.0f}, 0.95f,
         {249740.0f, 3990.0f, -248910.0f}, 0.995f}};
    for (const FUvwCase& uvwCase : uvwCases) {
        const FVec3 former =
            legacyUvw(uvwCase.probe, uvwCase.probe_height);
        const FVec3 specialized = affineProbeUvw(
            uvwCase.probe, uvwCase.probe_height,
            uvwCase.reference, uvwCase.reference_height);
        const auto near = [](f32 expected, f32 actual) noexcept {
            return std::fabs(expected - actual) <=
                std::fabs(expected) * 3.0e-6f + 3.0e-6f;
        };
        EXPECT_TRUE(near(former.x, specialized.x));
        EXPECT_TRUE(near(former.y, specialized.y));
        EXPECT_TRUE(near(former.z, specialized.z));
    }

    // 共有場から作った構造体も、最終密度では同じ飽和変換、被覆、層端重みを使う。
    // 空、縁、内部、しきい値上限の代表値を固定する。
    struct FScalarCase {
        f32 base_noise;
        f32 height_threshold;
        f32 weather_mask;
        f32 profile_weight;
    };
    const FScalarCase scalarCases[]{
        {0.0f, 0.78f, 0.0f, 0.0f},
        {0.51f, 0.62f, 0.35f, 0.42f},
        {0.74f, 0.62f, 0.81f, 0.67f},
        {0.97f, 0.78f, 1.0f, 1.0f},
        {1.0f, 0.92f, 0.13f, 0.88f}};
    const auto formerStructConsumer =
        [](const FScalarCase& value) noexcept {
            f32 result = 0.0f;
            if (value.profile_weight > 0.0f) {
                const f32 upper =
                    value.height_threshold + 0.22f < 0.98f
                        ? value.height_threshold + 0.22f
                        : 0.98f;
                const f32 remapped = SaturateForTest(
                    (value.base_noise - value.height_threshold) /
                    ((upper - value.height_threshold) > 1.0e-5f
                         ? upper - value.height_threshold : 1.0e-5f));
                result = remapped *
                    value.weather_mask * value.profile_weight;
            }
            return result;
        };
    const auto specializedScalar =
        [](const FScalarCase& value) noexcept {
            if (value.profile_weight <= 0.0f) return 0.0f;
            const f32 upper =
                value.height_threshold + 0.22f < 0.98f
                    ? value.height_threshold + 0.22f
                    : 0.98f;
            f32 t =
                (value.base_noise - value.height_threshold) /
                ((upper - value.height_threshold) > 1.0e-5f
                     ? upper - value.height_threshold : 1.0e-5f);
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            return t * value.weather_mask * value.profile_weight;
        };
    for (const FScalarCase& scalarCase : scalarCases) {
        EXPECT_NEAR(
            specializedScalar(scalarCase),
            formerStructConsumer(scalarCase),
            0.0f);
    }

    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 384u);
    EXPECT_EQ(kVolumetricCloudMaxLightMarchSamples, 8u);
    EXPECT_EQ(kVolumetricCloudUltraTraceDivisor, 4u);
}

ACS_TEST(VolumetricClouds,
         CurvedShellRayInvariantQuadraticTermsAreCpuHoisted) {
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
        "outfloatnearT,outfloatfarT){"));
    EXPECT_FALSE(Contains(
        shader,
        "boolsphereRoots(float3localOrigin,float3rd,floataltitude,"));
    EXPECT_TRUE(Contains(
        shader,
        "boolintersectCloudShell("
        "float3rayDir,outfloatt0,outfloatt1){"
        "t0=0.0;t1=0.0;"
        "floatinnerC=cloudShellRayOrigin.w;"
        "floatouterC=cloudShellTerms.x;"
        "floatb=dot(cloudShellRayOrigin.xyz,rayDir);"));
    EXPECT_TRUE(Contains(
        shader,
        "boolhitsOuter=sphereRootsFromTerms("
        "b,outerC,outerNear,outerFar);"));
    EXPECT_TRUE(Contains(
        shader,
        "boolhitsInner=sphereRootsFromTerms("
        "b,innerC,innerNear,innerFar);"));
    EXPECT_TRUE(Contains(
        shader,
        "if(!intersectCloudShell(dir,t0,t1)){"));
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
    EXPECT_TRUE(Contains(compactSource, "sizeof(FCloudCb)==704"));
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
    EXPECT_TRUE(Contains(
        compactSource,
        "cb.cloudShellTerms=FVec4{"
        "shellC(shellTopHeight),0.0f,0.0f,0.0f};"));

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
    EXPECT_EQ(kVolumetricCloudMaxLightMarchSamples, 8u);
    EXPECT_EQ(kVolumetricCloudUltraTraceDivisor, 4u);
}

ACS_TEST(VolumetricClouds,
         FrameInvariantDensityTermsAndLightBasisAreCpuHoisted) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    const std::string compactSource = CompactShader(source);
    EXPECT_TRUE(!shader.empty());

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
        "//CPUmirrorsclamp(layer.z*0.006,0.00012,0.00045)"
        "onceperframe.returncloudFrameTerms.z;}"));
    EXPECT_TRUE(Contains(
        shader,
        "float2cloudWindWorld(){"
        "//CPUmirrorsparams.z*float2(0.9284767,0.3713907)"
        "onceperframe.returncloudFrameTerms.xy;}"));
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

    EXPECT_TRUE(Contains(compactSource, "sizeof(FCloudCb)==704"));
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
    EXPECT_TRUE(Contains(compactSource, "cb.cloudShadowUpdate=FVec4{" "static_cast<f32>(shadowUpdateOffsetX)," "static_cast<f32>(shadowUpdateOffsetY)," "static_cast<f32>(shadowUpdateDivisor)," "refreshAllShadows?1.0f:0.0f};"));
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
            EXPECT_NEAR(actual.shape_scale, expected.shape_scale, 1.0e-10f);
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
    EXPECT_TRUE(hostileTerms.shape_scale >= 0.00012f);
    EXPECT_TRUE(hostileTerms.shape_scale <= 0.00045f);
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

        const auto coneDirection =
            [](const FVolumetricCloudLightBasis& basis,
               f32 phi, f32 angle) noexcept {
                const f32 coneSin = Sin(phi);
                const f32 coneCos = Cos(phi);
                const FVec3 lateral{
                    coneCos * basis.tangent.x +
                        coneSin * basis.bitangent.x,
                    coneCos * basis.tangent.y +
                        coneSin * basis.bitangent.y,
                    coneCos * basis.tangent.z +
                        coneSin * basis.bitangent.z};
                const f32 inverseLength =
                    1.0f / Sqrt(1.0f + angle * angle);
                return FVec3{
                    (basis.direction.x + lateral.x * angle) *
                        inverseLength,
                    (basis.direction.y + lateral.y * angle) *
                        inverseLength,
                    (basis.direction.z + lateral.z * angle) *
                        inverseLength};
            };
        const f32 conePhases[] = {0.0f, 1.2345f, 5.991f};
        const f32 coneAngles[] = {0.01f, 0.03f, 0.08f};
        for (const f32 phase : conePhases) {
            for (const f32 angle : coneAngles) {
                ExpectVec3Near(
                    coneDirection(actual, phase, angle),
                    coneDirection(expected, phase, angle),
                    3.0e-6f);
            }
        }
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

    // Optimization changes only invariant ALU placement. Work submission and
    // the CPU-bounded authored sample policy remain explicit in the shader.
    EXPECT_TRUE(Contains(
        shader,
        "intMAX_STEPS=(int)cloudLightingAmbient.z;"
        "if(MAX_STEPS<32)MAX_STEPS=32;"));
    EXPECT_TRUE(Contains(shader, "[loop]for(intl=0;l<3;l++){"));
    EXPECT_TRUE(Contains(shader, "[loop]for(intl=3;l<8;l++){"));
    EXPECT_TRUE(Contains(
        shader,
        "float3lightHalfStep=coneDir*(0.5*lightStep);"
        "lp+=lightHalfStep;"
        "CloudMacroSamplelightMacro="));
    EXPECT_TRUE(Contains(
        compactSource,
        "cl.Dispatch((m_W+7u)/8u,(m_H+7u)/8u,1);"));
    EXPECT_TRUE(Contains(
        compactSource,
        "cl.Dispatch((m_FullW+7u)/8u,(m_FullH+7u)/8u,1);"));
    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 384u);
    EXPECT_EQ(kVolumetricCloudMaxLightMarchSamples, 8u);
    EXPECT_EQ(kVolumetricCloudUltraTraceDivisor, 4u);
}

ACS_TEST(VolumetricClouds,
         EvolutionPhasesMorphIndependentDomainsWithoutAdditionalTextureReads) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    const std::string compactSource = CompactShader(source);
    EXPECT_TRUE(!shader.empty());

    // 基準領域の採取数を増やさず、既存の独立領域だけへ異なる位相ずれを加える。
    EXPECT_EQ(
        CountOccurrences(shader, "shapeNoise.SampleLevel("),
        static_cast<std::size_t>(7));
    EXPECT_EQ(
        CountOccurrences(shader, "weatherMap.SampleLevel("),
        static_cast<std::size_t>(2));
    EXPECT_EQ(
        CountOccurrences(shader, "curlNoise.SampleLevel("),
        static_cast<std::size_t>(2));
    // 実際の侵食2回に加え、二つの任意影パスが登録枠を保持する到達不能参照を持つ。
    EXPECT_EQ(CountOccurrences(shader, "detailNoise.SampleLevel("), static_cast<std::size_t>(4));
    EXPECT_EQ(
        CountOccurrences(
            shader,
            "+float3(cloudEvolution.x,cloudEvolution.y,-cloudEvolution.x);"),
        static_cast<std::size_t>(2));
    EXPECT_EQ(
        CountOccurrences(
            shader,
            "+float3(-cloudEvolution.y,cloudEvolution.x,cloudEvolution.y);"),
        static_cast<std::size_t>(2));
    EXPECT_TRUE(Contains(
        shader,
        "+float3(cloudEvolution.y,-cloudEvolution.x,cloudEvolution.x);"));
    EXPECT_TRUE(Contains(
        shader,
        "curlUv+=float4(0.0,0.0,cloudEvolution.z,cloudEvolution.w);"));
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

ACS_TEST(VolumetricClouds,
         TemporalSuperResolutionRejectsDisocclusionGhostsAndKeepsExactPhases) {
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
        "resolved=current;"
        "resolvedDepth=float2(curDepth,curA);"));
    EXPECT_FALSE(Contains(scheduledPath, "CloudTemporalClipHistory"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "resolved=lerp(histPacked,current,"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "resolvedDepth=float2(curDepth,curA);"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "resolved=lerp(histPacked,current,evolutionMismatch);"
        "resolvedDepth=float2(reprojectionDepth,histD.y);"));
    EXPECT_TRUE(Contains(resolveShader, "float4spatialCurrent=float4(gatheredPremul,gatheredA);float2spatialDepth=float2(gatheredDepth,gatheredA);"));
    EXPECT_TRUE(Contains(resolveShader, "if(temporalSuperRes&&scheduled&&!historyAccepted){resolved=spatialCurrent;resolvedDepth=spatialDepth;}"));
    EXPECT_TRUE(CloudTemporalUsesSpatialFallbackForTest(true, true, false));
    EXPECT_FALSE(CloudTemporalUsesSpatialFallbackForTest(true, true, true));
    EXPECT_FALSE(CloudTemporalUsesSpatialFallbackForTest(true, false, false));
    EXPECT_FALSE(CloudTemporalUsesSpatialFallbackForTest(false, true, false));
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
        "!scheduled&&worldOrigin.w>0.5&&evolutionMismatch<0.08;"));
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

ACS_TEST(VolumetricClouds,
         TemporalSuperResolutionReplacesScheduledExactSample) {
    // 採取画素は現在値へ一度で収束し、未採取15画素は各自の正確な履歴を保つ。
    const f32 currentAlpha = 0.80f;
    const f32 historyAlpha = 0.20f;
    const f32 scheduledResult = currentAlpha;
    const f32 unscheduledResult = historyAlpha;
    EXPECT_NEAR(scheduledResult, 0.80f, 0.0f);
    EXPECT_NEAR(unscheduledResult, 0.20f, 0.0f);
    EXPECT_NEAR(currentAlpha - scheduledResult, 0.0f, 0.0f);
    EXPECT_NEAR(scheduledResult - unscheduledResult, 0.60f, 1e-6f);

    // 旧最小重み20%では、16フレーム周期を10回経ても誤差が10%以上残っていた。
    f32 formerRetainedError = 1.0f;
    for (u32 update = 0u; update < 10u; ++update) {
        formerRetainedError *= 0.80f;
    }
    EXPECT_TRUE(formerRetainedError > 0.10f);

    // 未採取画素の空間再構成は別レイなので、正確な画素別履歴を棄却する根拠にしない。
    EXPECT_FALSE(CloudTemporalOccupancyMismatchForTest(0.0f, 0.8f, true, false));
    EXPECT_FALSE(CloudTemporalOccupancyMismatchForTest(0.8f, 0.0f, true, false));
    EXPECT_TRUE(CloudTemporalOccupancyMismatchForTest(0.0f, 0.8f, true, true));
    EXPECT_TRUE(CloudTemporalOccupancyMismatchForTest(0.8f, 0.0f, false, false));

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudResolveCS"));
    EXPECT_FALSE(Contains(shader, "CloudTemporalSampleResponse"));
    EXPECT_FALSE(Contains(shader, "CloudTemporalCurrentWeight"));
    EXPECT_EQ(CountOccurrences(shader, "resolved=current;"), 3u);
    EXPECT_TRUE(Contains(shader, "resolved=lerp(histPacked,current,"));
    EXPECT_TRUE(Contains(shader, "if(!temporalSuperRes){histPacked=CloudTemporalClipHistory(histPacked,neighborhoodMin,neighborhoodMax);}"));
    EXPECT_FALSE(Contains(shader, "if(!temporalSuperRes||scheduled){histPacked=CloudTemporalClipHistory"));
    EXPECT_TRUE(Contains(shader, "booloccupancyMismatch=(!temporalSuperRes||scheduled)&&((curA<0.02&&hist.a>0.08)||(curA>0.08&&hist.a<0.02));"));
    EXPECT_FALSE(Contains(shader, "CloudTemporalBlockResponse"));
    EXPECT_FALSE(Contains(shader, "CloudTemporalUnscheduledWeight"));
    EXPECT_TRUE(Contains(shader, "resolved=current;resolvedDepth=nativeDepth;"));
    EXPECT_FALSE(Contains(shader, "floatfeedback=max(lerp(0.42,0.62,edgeConfidence),evolutionMismatch);"));
    EXPECT_TRUE(Contains(shader, "floatCloudTemporalEvolutionMismatch(){"));
    EXPECT_TRUE(Contains(shader, "float2slowDelta=abs(cloudEvolution.xy-cloudPreviousEvolution.xy);"));
    EXPECT_TRUE(Contains(shader, "floatevolutionMismatch=CloudTemporalEvolutionMismatch();"));
    EXPECT_TRUE(Contains(shader, "!scheduled&&worldOrigin.w>0.5&&evolutionMismatch<0.08"));
    EXPECT_TRUE(Contains(shader, "resolved=lerp(histPacked,current,evolutionMismatch);"));
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
    EXPECT_TRUE(Contains(shader, "stableHistPacked=CloudTemporalClipHistory(stableHistPacked,stableCurrentMin,stableCurrentMax);}" "resolved=stableHistPacked;"));
    EXPECT_TRUE(Contains(shader, "resolvedDepth=float2(sameScreenDepth.x,stableHistPacked.a);"));
    EXPECT_FALSE(Contains(shader, "resolved=float4(stableHist.rgb*stableHist.a,stableHist.a);"));
}

ACS_TEST(VolumetricClouds,
         SaturatedLightMarchStopsOnlyBelowSubPerceptualTransfer) {
    constexpr f32 cutoffOpticalDepth = 18.0f;
    const f32 remaining =
        0.72f * std::exp(-cutoffOpticalDepth) +
        0.28f * std::exp(-cutoffOpticalDepth * 0.28f);
    EXPECT_TRUE(remaining < 0.002f);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(Contains(
        shader,
        "if(lightDepth*density*cloudLightingExtinction.y>18.0)break;"));
    EXPECT_TRUE(Contains(shader, "boolradianceValid="));
    EXPECT_TRUE(Contains(shader, "all(abs(col)<=65504.0)"));
    EXPECT_TRUE(Contains(shader, "col=max(col,0.0);"));
    EXPECT_TRUE(Contains(
        shader,
        "CloudMacroSamplemacro=sampleCloudMacroLighting("
        "p,coverage,0.0);"));
    EXPECT_TRUE(Contains(
        shader,
        "macro.heightThreshold=cloudHeightThreshold("
        "weatherCoverage,profileShape)-cloudWeatherCoreShapeOffset("
        "macro.weatherMask,macro.height,upperBand);"));
    EXPECT_TRUE(Contains(
        shader,
        "cloudBaseShapeLighting("
        "cloudUVW(p,macro.weather,macro.curl,macro.height),"
        "macro.heightThreshold-cloudBillowMaximumOffset(macro.height),"
        "sampleSpacing,"
        "macro.height,"
        "macro.baseNoise);"));
}

ACS_TEST(VolumetricClouds,
         ShaderBranchOutputsAreExplicitlyInitialized) {
    const std::string source = ReadSkySource();
    const std::string shader =
        ExtractRawShader(source, "const char* kCloudCS");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!shader.empty());

    // FXC's flow analysis does not reliably propagate values through early
    // returns in helpers with out parameters.  Keep every branch-safe output
    // initialized at declaration/entry and use a single explicit return value
    // for density helpers so a future branch cannot publish undefined data.
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
    EXPECT_TRUE(Contains(shader, "float shapeResult=0.0;"));
    EXPECT_TRUE(Contains(shader, "return shapeResult;"));
    EXPECT_TRUE(Contains(shader, "float densityResult=0.0;"));
    EXPECT_TRUE(Contains(shader, "return densityResult;"));
    EXPECT_TRUE(!Contains(shader, "if(disc<0.0){ nearT="));
    EXPECT_TRUE(!Contains(shader, "if(weatherMask<=0.001) return 0.0;"));
    EXPECT_TRUE(!Contains(shader, "if(profile<=0.001) return 0.0;"));
}

ACS_TEST(VolumetricClouds, ReferenceRayJitterIsDeterministicWhileNativeModesAdvance) {
    const std::string source = ReadSkySource();
    const std::string shader =
        ExtractRawShader(source, "const char* kCloudCS");
    const std::string compactShader = CompactShader(shader);
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!shader.empty());

    // 参照描画は時間履歴で平均しないため、区間中央を固定して粒状誤差の時間変化を止める。
    // 通常描画だけが乱数列へ入り、超高品質の時間再構成では 16 段階ごとに列を進める。
    const std::size_t referenceMidpoint = compactShader.find("floatjit=0.5;");
    const std::size_t normalModeGate = compactShader.find("if(cloudLightingAmbient.w<0.5){", referenceMidpoint);
    const std::size_t jitterFrame = compactShader.find("uintjitterFrame=(uint)temporal.z;", normalModeGate);
    EXPECT_TRUE(referenceMidpoint != std::string::npos);
    EXPECT_TRUE(normalModeGate != std::string::npos);
    EXPECT_TRUE(jitterFrame != std::string::npos);
    EXPECT_TRUE(referenceMidpoint < normalModeGate);
    EXPECT_TRUE(normalModeGate < jitterFrame);
    EXPECT_TRUE(Contains(source, "m_ReferenceMode ? 1.0f : 0.0f"));
    EXPECT_TRUE(Contains(shader, "uint jitterFrame=(uint)temporal.z;"));
    EXPECT_TRUE(Contains(
        shader,
        "uint jitterSequence=temporal.w>3.5?"
        "(jitterFrame>>4u):jitterFrame;"));
    EXPECT_TRUE(Contains(shader, "uint2 jitterPixel=rayPixel+uint2("));
    EXPECT_TRUE(Contains(shader, "(jitterSequence*47u)%131u"));
    EXPECT_TRUE(Contains(shader, "(jitterSequence*17u)%127u"));
    EXPECT_TRUE(Contains(
        shader,
        "uint state=pixel.x*747796405u+pixel.y*2891336453u+277803737u;"));
    EXPECT_TRUE(Contains(
        shader,
        "uint word=((state>>((state>>28u)+4u))^state)*277803737u;"));
    EXPECT_TRUE(Contains(shader, "return (word>>22u)^word;"));
    EXPECT_TRUE(Contains(
        shader,
        "float pixelJitter=CloudJitter01(jitterPixel);"));
    EXPECT_TRUE(Contains(shader, "float(jitterSequence)*0.754877666"));
    EXPECT_FALSE(Contains(shader, "52.9829189"));
    EXPECT_FALSE(Contains(shader, "float2(0.06711056,0.00583715)"));
    EXPECT_TRUE(Contains(source, "m_FrameIndex & 4080u"));
    EXPECT_TRUE(!Contains(source, "m_FrameIndex & 7u"));

    constexpr u32 kPixelX = 173u;
    constexpr u32 kPixelY = 91u;
    for (u32 cycle = 0u; cycle < 16u; ++cycle) {
        const f64 cycleValue = CloudJitterForTest(
            kPixelX, kPixelY, cycle * 16u, true);
        for (u32 phase = 1u; phase < 16u; ++phase) {
            EXPECT_NEAR(
                CloudJitterForTest(
                    kPixelX, kPixelY, cycle * 16u + phase, true),
                cycleValue, 1e-12);
        }
        if (cycle > 0u) {
            EXPECT_TRUE(std::fabs(
                cycleValue - CloudJitterForTest(
                    kPixelX, kPixelY, (cycle - 1u) * 16u, true)) > 1e-6);
        }
    }

    f64 nativeJitter[256]{};
    for (u32 frame = 0u; frame < 256u; ++frame) {
        nativeJitter[frame] =
            CloudJitterForTest(kPixelX, kPixelY, frame, false);
    }
    u32 shortCycleDuplicates = 0u;
    for (u32 a = 0u; a < 256u; ++a) {
        for (u32 b = a + 1u; b < 256u; ++b) {
            if (std::fabs(nativeJitter[a] - nativeJitter[b]) < 1e-12) {
                ++shortCycleDuplicates;
            }
        }
    }
    EXPECT_EQ(shortCycleDuplicates, static_cast<u32>(0u));
}

ACS_TEST(VolumetricClouds,
         PixelJitterHashIsWellDistributedWithoutScreenDiagonalCorrelation) {
    constexpr u32 kExtent = 256u;
    constexpr u32 kBinCount = 16u;
    u32 histogram[kBinCount]{};
    f64 sum = 0.0;
    for (u32 y = 0u; y < kExtent; ++y) {
        for (u32 x = 0u; x < kExtent; ++x) {
            const f64 value = CloudJitter01ForTest(x, y);
            sum += value;
            const u32 bin = static_cast<u32>(value * kBinCount);
            ++histogram[bin < kBinCount ? bin : kBinCount - 1u];
        }
    }
    const f64 sampleCount = static_cast<f64>(kExtent * kExtent);
    EXPECT_NEAR(sum / sampleCount, 0.5, 0.01);
    const u32 expectedPerBin = kExtent * kExtent / kBinCount;
    for (const u32 count : histogram) {
        EXPECT_TRUE(count > expectedPerBin * 9u / 10u);
        EXPECT_TRUE(count < expectedPerBin * 11u / 10u);
    }

    const auto spatialCorrelation = [](u32 deltaX, u32 deltaY) noexcept {
        f64 sumA = 0.0, sumB = 0.0;
        f64 sumAA = 0.0, sumBB = 0.0, sumAB = 0.0;
        u32 count = 0u;
        for (u32 y = 0u; y + deltaY < kExtent; ++y) {
            for (u32 x = 0u; x + deltaX < kExtent; ++x) {
                const f64 a = CloudJitter01ForTest(x, y);
                const f64 b = CloudJitter01ForTest(
                    x + deltaX, y + deltaY);
                sumA += a;
                sumB += b;
                sumAA += a * a;
                sumBB += b * b;
                sumAB += a * b;
                ++count;
            }
        }
        const f64 n = static_cast<f64>(count);
        const f64 covariance = sumAB - sumA * sumB / n;
        const f64 varianceA = sumAA - sumA * sumA / n;
        const f64 varianceB = sumBB - sumB * sumB / n;
        return covariance / std::sqrt(varianceA * varianceB);
    };
    EXPECT_NEAR(spatialCorrelation(1u, 0u), 0.0, 0.03);
    EXPECT_NEAR(spatialCorrelation(0u, 1u), 0.0, 0.03);
    EXPECT_NEAR(spatialCorrelation(1u, 1u), 0.0, 0.03);

    // The non-TSR sequence must not merely exchange spatial correlation for a
    // short temporal pattern. Its first lag remains statistically independent.
    f64 sumA = 0.0, sumB = 0.0;
    f64 sumAA = 0.0, sumBB = 0.0, sumAB = 0.0;
    constexpr u32 kFrames = 256u;
    for (u32 frame = 0u; frame + 1u < kFrames; ++frame) {
        const f64 a = CloudJitterForTest(173u, 91u, frame, false);
        const f64 b = CloudJitterForTest(173u, 91u, frame + 1u, false);
        sumA += a;
        sumB += b;
        sumAA += a * a;
        sumBB += b * b;
        sumAB += a * b;
    }
    const f64 n = static_cast<f64>(kFrames - 1u);
    const f64 covariance = sumAB - sumA * sumB / n;
    const f64 varianceA = sumAA - sumA * sumA / n;
    const f64 varianceB = sumBB - sumB * sumB / n;
    EXPECT_NEAR(
        covariance / std::sqrt(varianceA * varianceB), 0.0, 0.15);
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
        "if(groundCutoff>=-1.0&&!cameraInsideCloudLayer){"));
    EXPECT_TRUE(Contains(
        shader, "boolcloudCameraInsideCloudLayer(){"));
    EXPECT_TRUE(Contains(
        shader, "floatcameraAltitude=cloudAltitude(camPos.xyz);"));
    EXPECT_TRUE(Contains(
        shader, "if(groundCutoff>=-1.0&&!cameraInsideCloudLayer){"));
    EXPECT_TRUE(Contains(
        shader, "if(groundHorizonCoverage<=0.001&&!cameraInsideCloudLayer){"));
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
    EXPECT_TRUE(Contains(compactSource, "sizeof(FCloudCb)==704"));
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
        EXPECT_TRUE(Contains(composite, "boolCloudCameraInsideCloudLayer(){"));
        EXPECT_TRUE(Contains(composite, "floatCloudGroundCoverage(VSOutv){floatresult=1.0;boolcameraInsideCloudLayer=CloudCameraInsideCloudLayer();if(groundHorizon.w>=-1.0&&!cameraInsideCloudLayer){"));
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
    EXPECT_TRUE(Contains(resolveShader, "boolstableUnscheduled=temporal.x>0.5&&temporalSuperRes&&!scheduled&&worldOrigin.w>0.5&&evolutionMismatch<0.08;"));

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
        EXPECT_TRUE(Contains(stableAcceptPath, "stableHistPacked=CloudTemporalClipHistory(stableHistPacked,stableCurrentMin,stableCurrentMax);}" "resolved=stableHistPacked;"));
        EXPECT_TRUE(Contains(stableAcceptPath, "resolvedDepth=float2(sameScreenDepth.x,stableHistPacked.a);"));
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
        EXPECT_TRUE(compiled.Value().world_shadow.Get() != nullptr);
        EXPECT_EQ(
            compiled.Value().Status(),
            EShaderStatus::Ready);
    }
#else
    // The Diligent configuration compiles the same source through its backend
    // during renderer initialization; raw CPU compilation is DX12-only.
    EXPECT_TRUE(true);
#endif
}

ACS_TEST(VolumetricClouds,
         ConfigurableCloudPhaseIsFiniteBoundedAndMatchesTheShader) {
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
        EXPECT_NEAR(
            DepthAwareCloudPhaseForTest(cosine, 1.0f, 1.0f),
            phase, 1.0e-5f);
        if (phase < minimumPhase) minimumPhase = phase;
        if (phase > maximumPhase) maximumPhase = phase;
    }

    const f32 backward = DefaultCloudPhaseForTest(-1.0f);
    const f32 side = DefaultCloudPhaseForTest(0.0f);
    const f32 forward = DefaultCloudPhaseForTest(1.0f);
    EXPECT_TRUE(backward >= 0.12f && backward <= 0.15f);
    EXPECT_TRUE(side >= 0.14f && side <= 0.17f);
    EXPECT_TRUE(forward >= 2.6f && forward <= 2.9f);
    EXPECT_TRUE(forward > side * 10.0f);
    EXPECT_TRUE(maximumPhase <= lighting.PhaseMax);
    EXPECT_TRUE(minimumPhase >= lighting.PhaseMin);

    const f32 topBackScatter =
        CloudTopSurfaceScatterForTest(-1.0f, 1.0f, 0.90f, 1.0f);
    EXPECT_NEAR(topBackScatter, 0.22f, 1.0e-6f);
    EXPECT_NEAR(
        CloudTopSurfaceScatterForTest(1.0f, 1.0f, 0.90f, 1.0f),
        0.0f, 0.0f);
    EXPECT_NEAR(
        CloudTopSurfaceScatterForTest(-1.0f, 1.0f, 0.20f, 1.0f),
        0.0f, 0.0f);
    EXPECT_NEAR(
        CloudTopSurfaceScatterForTest(-1.0f, 0.0f, 0.90f, 1.0f),
        0.0f, 0.0f);
    EXPECT_NEAR(
        CloudTopSurfaceScatterForTest(-1.0f, 1.0f, 0.90f, 0.0f),
        0.0f, 0.0f);
    EXPECT_TRUE(
        CloudTopSurfaceScatterForTest(-1.0f, 1.0f, 0.80f, 1.0f) >
        CloudTopSurfaceScatterForTest(-1.0f, 1.0f, 0.50f, 1.0f));
    EXPECT_TRUE(
        CloudTopSurfaceScatterForTest(-1.0f, 1.0f, 0.80f, 1.0f) >
        CloudTopSurfaceScatterForTest(-1.0f, 1.0f, 0.80f, 0.5f));

    // 表面では従来の作者指定混合と一致し、雲芯側では前方散乱だけを連続的に弱める。
    EXPECT_NEAR(CloudForwardPhaseWeightForTest(0.85f, 1.0f, 1.0f), 0.85f, 0.0f);
    EXPECT_NEAR(CloudForwardPhaseWeightForTest(0.85f, 0.5f, 0.5f), 0.2125f, 1.0e-6f);
    EXPECT_NEAR(CloudForwardPhaseWeightForTest(-1.0f, 2.0f, 2.0f), 0.0f, 0.0f);
    EXPECT_NEAR(CloudForwardPhaseWeightForTest(2.0f, 2.0f, 2.0f), 1.0f, 0.0f);
    EXPECT_NEAR(DepthAwareCloudPhaseForTest(1.0f, 1.0f, 1.0f), forward, 1.0e-6f);
    const f32 partlyEnclosedForward = DepthAwareCloudPhaseForTest(1.0f, 0.5f, 1.0f);
    const f32 enclosedForward = DepthAwareCloudPhaseForTest(1.0f, 0.0f, 0.0f);
    EXPECT_TRUE(forward > partlyEnclosedForward);
    EXPECT_TRUE(partlyEnclosedForward > enclosedForward);
    for (u32 lightStep = 0u; lightStep <= 10u; ++lightStep) {
        for (u32 intervalStep = 0u; intervalStep <= 10u; ++intervalStep) {
            const f32 depthPhase = DepthAwareCloudPhaseForTest(
                0.35f,
                static_cast<f32>(lightStep) * 0.1f,
                static_cast<f32>(intervalStep) * 0.1f);
            EXPECT_TRUE(std::isfinite(depthPhase));
            EXPECT_TRUE(depthPhase >= lighting.PhaseMin);
            EXPECT_TRUE(depthPhase <= lighting.PhaseMax);
        }
    }

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(Contains(
        shader,
        "floatphaseBlend=cloudLightingPhase.z;"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudForwardPhaseWeight("
        "floatauthoredForwardWeight,floatlightTransmittance,"
        "floatintervalTransmittance){"
        "returnsaturate(authoredForwardWeight)*"
        "saturate(lightTransmittance)*"
        "saturate(intervalTransmittance);}"));
    EXPECT_TRUE(Contains(
        shader,
        "floatforwardPhase=4.0*hg(cosA,cloudLightingPhase.x);"
        "floatbackwardPhase=4.0*hg(cosA,cloudLightingPhase.y);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatviewSampleOpticalDepth=dens*stepLength*"
        "sampleOpticalDepthScale*cloudLightingExtinction.x;"
        "floatintervalTransmittance=exp(-viewSampleOpticalDepth);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatforwardPhaseWeight=cloudForwardPhaseWeight("
        "phaseBlend,beer,intervalTransmittance);"
        "floatphase=lerp("
        "backwardPhase,forwardPhase,forwardPhaseWeight);"
        "phase=clamp(phase,cloudLightingMulti.y,cloudLightingMulti.z);"));
    EXPECT_FALSE(Contains(
        shader,
        "floatphase=4.0*("
        "hg(cosA,cloudLightingPhase.x)*phaseBlend+"
        "hg(cosA,cloudLightingPhase.y)*(1.0-phaseBlend));"));
    EXPECT_TRUE(Contains(
        shader,
        "floatphaseMulti=4.0*hg(cosA,cloudMultiPhase.x);"));
    EXPECT_TRUE(Contains(shader, "phaseMulti=clamp(" "phaseMulti,cloudLightingMulti.y,cloudLightingMulti.z);"));
    EXPECT_TRUE(Contains(shader, "floatinScatterProbability=inScatterDepth;" "floatinScatterFactor=lerp(" "1.0,inScatterProbability,cloudLightingExtinction.w);" "floatsingleScatter=beer*phase;"));
    EXPECT_FALSE(Contains(shader, "inScatterVertical"));
    EXPECT_TRUE(Contains(shader, "float2lowLodDensityAndProfile=cloudLowLodDensityAndProfileFromMacro(" "macro,densityHeightThreshold,viewWeatherMask);"));
    EXPECT_TRUE(Contains(shader, "floatlowLodDensity=lowLodDensityAndProfile.x;"));
    EXPECT_FALSE(Contains(shader, "pow(saturate(shape),inScatterDepthExponent)"));
    EXPECT_TRUE(Contains(shader, "floatdetailedLightDepth=0.0;"));
    EXPECT_TRUE(Contains(shader, "detailedLightDepth=max(lightDepth,0.0);"));
    EXPECT_EQ(
        CountOccurrences(shader, "detailedLightDepth=max(lightDepth,0.0);"),
        static_cast<std::size_t>(1));
    EXPECT_TRUE(Contains(
        shader,
        "floatdetailedTauL=min("
        "detailedLightDepth*density*cloudLightingExtinction.y,tauL);"
        "floatfarTauL=max(tauL-detailedTauL,0.0);"
        "floatsecondDetailedOcclusion=sqrt(saturate(multiOcclusion));"));
    EXPECT_TRUE(Contains(
        shader,
        "floatsecondScatter=multiContribution*"
        "exp(-(detailedTauL*secondDetailedOcclusion+"
        "farTauL*multiOcclusion))*phaseMulti;"
        "floatthirdContribution=multiContribution*multiContribution;"
        "floatthirdOcclusion=multiOcclusion*multiOcclusion;"
        "floatthirdScatter=thirdContribution*"
        "exp(-(detailedTauL*multiOcclusion+"
        "farTauL*thirdOcclusion))*phaseMulti;"
        "floatmultipleScatter=(secondScatter+thirdScatter)*inScatterFactor;"));
    EXPECT_TRUE(Contains(
        shader,
        "floattopSurfaceScatter=0.22*saturate(sun.y)*"
        "smoothstep(0.35,0.90,h)*ambientSurfaceProbability*"
        "saturate(-cosA);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatscatterTerm=singleScatter+multipleScatter+beer*topSurfaceScatter;"));
    EXPECT_FALSE(Contains(shader, "floatsingleScatter=beer*phase*inScatterFactor;"));
    EXPECT_FALSE(Contains(shader, "floatmultipleScatter=secondScatter+thirdScatter;"));
    EXPECT_FALSE(Contains(shader, "beer*(1.0-multiWeight)*phase"));
    EXPECT_FALSE(Contains(shader, "nearLightDensity"));
    EXPECT_FALSE(Contains(shader, "edgeBoost"));
    EXPECT_TRUE(Contains(shader, "floata=1.0-intervalTransmittance;"));
    EXPECT_FALSE(Contains(
        shader,
        "floata=1.0-exp(-dens*stepLength*"
        "sampleOpticalDepthScale*cloudLightingExtinction.x);"));
    EXPECT_TRUE(Contains(
        shader,
        "float3sunAtCloud=sunCol.rgb*cloudSunTransmittance.rgb;"
        "float3sunL=sunAtCloud*cloudLightingExtinction.z*scatterTerm"));

    const auto splitMultipleScattering = [](
        f32 detailedDepth, f32 farDepth,
        f32 contribution, f32 occlusion) noexcept {
        const f32 secondDetailed = std::sqrt(occlusion);
        const f32 thirdOcclusion = occlusion * occlusion;
        const f32 second = contribution * std::exp(
            -(detailedDepth * secondDetailed + farDepth * occlusion));
        const f32 third = contribution * contribution * std::exp(
            -(detailedDepth * occlusion + farDepth * thirdOcclusion));
        return FVec2{second, third};
    };
    constexpr f32 kContribution = 0.28f;
    constexpr f32 kOcclusion = 0.28f;
    constexpr f32 kDetailedDepth = 1.0f;
    constexpr f32 kFarDepth = 2.0f;
    const FVec2 split = splitMultipleScattering(
        kDetailedDepth, kFarDepth, kContribution, kOcclusion);
    const FVec2 noDetailedSegment = splitMultipleScattering(
        0.0f, kDetailedDepth + kFarDepth,
        kContribution, kOcclusion);
    const f32 formerSecond = kContribution * std::exp(
        -(kDetailedDepth + kFarDepth) * kOcclusion);
    const f32 formerThird = kContribution * kContribution * std::exp(
        -(kDetailedDepth + kFarDepth) * kOcclusion * kOcclusion);
    EXPECT_TRUE(split.x >= 0.0f && split.y >= 0.0f);
    EXPECT_TRUE(split.x < formerSecond);
    EXPECT_TRUE(split.y < formerThird);
    EXPECT_NEAR(noDetailedSegment.x, formerSecond, 1.0e-6f);
    EXPECT_NEAR(noDetailedSegment.y, formerThird, 1.0e-6f);

    // 消散縮小率の両端では、光路を分割しても従来式と一致する。
    for (u32 endpointStep = 0u; endpointStep <= 1u; ++endpointStep) {
        const f32 endpoint = static_cast<f32>(endpointStep);
        const FVec2 endpointSplit = splitMultipleScattering(
            kDetailedDepth, kFarDepth, kContribution, endpoint);
        const f32 endpointSecond = kContribution * std::exp(
            -(kDetailedDepth + kFarDepth) * endpoint);
        const f32 endpointThird = kContribution * kContribution * std::exp(
            -(kDetailedDepth + kFarDepth) * endpoint * endpoint);
        EXPECT_NEAR(endpointSplit.x, endpointSecond, 1.0e-6f);
        EXPECT_NEAR(endpointSplit.y, endpointThird, 1.0e-6f);
    }
}

ACS_TEST(VolumetricClouds,
         SkyAmbientVisibilityUsesColumnDepthAndSurfaceProbability) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());
    const std::size_t ambientBegin = shader.find("floatambientDensityScale=");
    const std::size_t ambientEnd = shader.find(
        "floata=1.0-intervalTransmittance;", ambientBegin);
    EXPECT_TRUE(ambientBegin != std::string::npos);
    EXPECT_TRUE(ambientEnd != std::string::npos);
    const std::string ambient = ambientBegin != std::string::npos && ambientEnd != std::string::npos
        ? shader.substr(ambientBegin, ambientEnd - ambientBegin) : std::string{};
    EXPECT_TRUE(Contains(
        ambient,
        "floatambientDensityScale=max(density*distanceFade,0.0);"));
    EXPECT_FALSE(Contains(
        ambient,
        "floatambientLocalDensity=saturate(lowLodDensity*density);"));
    EXPECT_TRUE(Contains(
        ambient,
        "floatfallbackSkyAmbientDepth=ambientLocalDensity*"
        "(0.35+0.65*(1.0-h));"
        "floatfallbackGroundAmbientDepth=ambientLocalDensity*"
        "(0.35+0.65*h);"
        "float3cachedAmbientDepth=sampleCloudAmbientDepth(p);"));
    EXPECT_TRUE(Contains(
        ambient,
        "floatskyAmbientOpticalDepth=lerp("
        "fallbackSkyAmbientDepth,"
        "cachedAmbientDepth.y*ambientDensityScale,"
        "cachedAmbientDepth.x);"
        "floatgroundAmbientOpticalDepth=lerp("
        "fallbackGroundAmbientDepth,"
        "cachedAmbientDepth.z*ambientDensityScale,"
        "cachedAmbientDepth.x);"));
    EXPECT_TRUE(Contains(
        ambient,
        "floatskyAmbientVisibility=ambientSurfaceProbability*"
        "exp(-reducedAmbientExtinction*skyAmbientOpticalDepth);"));
    EXPECT_TRUE(Contains(
        ambient,
        "floatgroundAmbientVisibility=ambientSurfaceProbability*"
        "exp(-reducedAmbientExtinction*groundAmbientOpticalDepth);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatambientSurfaceProbability=sqrt("
        "saturate(1.0-lowLodDensityAndProfile.y));"));
    EXPECT_FALSE(Contains(
        ambient,
        "saturate(1.0-lowLodDensity*distanceFade)"));
    EXPECT_TRUE(Contains(ambient, "floatskyAmbientZenithWeight=lerp(" "0.3333333,0.6666667,saturate(h));" "float3skyAmbient=cloudSkyZenith.w>0.5?lerp(" "skyCol.rgb,cloudSkyZenith.rgb," "skyAmbientZenithWeight):skyCol.rgb;"));
    EXPECT_FALSE(Contains(ambient, "tauL"));
    EXPECT_FALSE(Contains(ambient, "sun.y"));

    const FVolumetricCloudLighting lighting{};
    const auto visibility = [&lighting](f32 dimensionalProfile, f32 lowLodDensity, f32 height, f32 density, f32 distanceFade, f32 cachedDepth, f32 cacheWeight, bool fromSky) noexcept {
        const f32 h = std::clamp(height, 0.0f, 1.0f);
        const f32 safeFade = std::clamp(distanceFade, 0.0f, 1.0f);
        const f32 ambientDensityScale = density * safeFade > 0.0f
            ? density * safeFade : 0.0f;
        const f32 scaledDensity = lowLodDensity * ambientDensityScale;
        const f32 localDensity = scaledDensity > 0.0f ? scaledDensity : 0.0f;
        const f32 boundaryDistance = fromSky ? 1.0f - h : h;
        const f32 fallbackDepth = localDensity * (0.35f + 0.65f * boundaryDistance);
        const f32 blend = std::clamp(cacheWeight, 0.0f, 1.0f);
        const f32 opticalDepth = fallbackDepth +
            (cachedDepth * ambientDensityScale - fallbackDepth) * blend;
        const f32 diffuseOcclusion = lighting.MultiScatterOcclusion * lighting.MultiScatterOcclusion;
        const f32 reducedExtinction = 0.60f * diffuseOcclusion * lighting.LightExtinction;
        const f32 surfaceProbability = std::sqrt(
            1.0f - std::clamp(dimensionalProfile, 0.0f, 1.0f));
        return surfaceProbability * std::exp(-reducedExtinction * opticalDepth);
    };

    const f32 clear = visibility(0.0f, 0.0f, 0.8f, 1.6f, 1.0f, 0.0f, 1.0f, true);
    const f32 edge = visibility(0.15f, 0.15f, 0.85f, 1.6f, 1.0f, 0.15f, 1.0f, true);
    const f32 enclosed = visibility(0.85f, 0.85f, 0.65f, 1.6f, 1.0f, 1.10f, 1.0f, true);
    const f32 enclosedUnderGap = visibility(0.85f, 0.85f, 0.65f, 1.6f, 1.0f, 0.10f, 1.0f, true);
    EXPECT_NEAR(clear, 1.0f, 1e-6f);
    EXPECT_TRUE(edge < clear);
    EXPECT_TRUE(enclosed < edge);
    EXPECT_TRUE(enclosed < enclosedUnderGap);
    EXPECT_TRUE(enclosed > 0.0f);
    // 作者指定の密度倍率は積算密度の消散に反映し、形状勾配自体は変えない。
    const f32 ordinaryCore = visibility(0.85f, 0.85f, 0.5f, 1.0f, 1.0f, 0.80f, 1.0f, true);
    const f32 denseCore = visibility(0.85f, 0.85f, 0.5f, 2.1f, 1.0f, 0.80f, 1.0f, true);
    EXPECT_TRUE(denseCore < ordinaryCore);
    EXPECT_TRUE(denseCore > 0.0f);
    EXPECT_NEAR(
        visibility(0.85f, 1.0f, 0.5f, 2.1f, 0.0f, 10.0f, 1.0f, true),
        std::sqrt(0.15f), 1.0e-6f);

    // 各高度で半セル分を上下に分けると、両方の深さの和は全列深さと一致する。
    constexpr f32 segments[4]{0.10f, 0.20f, 0.30f, 0.40f};
    constexpr f32 totalDepth = 1.0f;
    f32 groundDepth = 0.0f;
    f32 previousSkyDepth = totalDepth + 1.0f;
    for (u32 index = 0u; index < 4u; ++index) {
        const f32 halfSegment = 0.5f * segments[index];
        const f32 skyDepth = totalDepth - groundDepth - halfSegment;
        const f32 sampleGroundDepth = groundDepth + halfSegment;
        EXPECT_NEAR(skyDepth + sampleGroundDepth, totalDepth, 1.0e-6f);
        EXPECT_TRUE(skyDepth < previousSkyDepth);
        previousSkyDepth = skyDepth;
        groundDepth += segments[index];
    }
    EXPECT_NEAR(groundDepth, totalDepth, 1.0e-6f);
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
         GpuPipelinesAndRg32DistanceTargetsInitializeWhenAvailable) {
    FDeviceConfig config{};
    auto deviceResult = CreateRhiDevice(config);
    if (deviceResult.IsErr()) return;

    CVolumetricClouds clouds;
    const auto initResult =
        clouds.Init(*deviceResult.Value(), EFormat::R16G16B16A16_Float);
    EXPECT_TRUE(initResult.IsOk());
    if (initResult.IsOk()) {
        EXPECT_TRUE(clouds.WorldShadowAvailable());
        EXPECT_TRUE(clouds.EnsureSize(
            *deviceResult.Value(), 32u, 24u, 1.0f));
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

    // The temporal/bilateral representative distance, not scene far depth or
    // a layer constant, determines the atmosphere-volume slice.
    EXPECT_TRUE(Contains(
        shader, "float2 cloudHit = cloudDepth.SampleLevel("));
    EXPECT_TRUE(Contains(
        shader, "sqrt(saturate(cloudHit.x / maxDistance))"));
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
    // 太陽方向二値と上下積算密度二値を、一つのRGBA16F 3次元テクスチャへ保持する。
    EXPECT_EQ(kVolumetricCloudShadowCacheWidth * kVolumetricCloudShadowCacheHeight * kVolumetricCloudShadowCacheDepth * 8u, 2359296u);

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

    EXPECT_TRUE(Contains(shader, "boolintersectCloudShellFromPosition(float3rayOrigin,float3rayDir,outfloatt0,outfloatt1){"));
    EXPECT_TRUE(Contains(shader, "floatinnerC=dot(local.xz,local.xz)+(local.y-layer.x)*(2.0*CLOUD_PLANET_RADIUS+local.y+layer.x);"));
    EXPECT_TRUE(Contains(shader, "[numthreads(8,8,1)]voidCSCloudWorldShadow(uint3tid:SV_DispatchThreadID){"));
    EXPECT_TRUE(Contains(shader, "uint2outputPixel=tid.xy*updateStride+(uint2)cloudShadowUpdate.xy;"));
    EXPECT_TRUE(Contains(shader, "if(any(outputPixel>=uint2(width,height)))return;"));
    EXPECT_TRUE(Contains(shader, "cloudOut[outputPixel]=float4("));
    EXPECT_TRUE(Contains(shader, "constintSAMPLE_COUNT=32;"));
    EXPECT_TRUE(Contains(shader, "floatstepLength=(exit-enter)/float(SAMPLE_COUNT);floatsampleDistance=enter+0.5*stepLength;"));
    EXPECT_TRUE(Contains(shader, "floatsampleDensity=cloudLowLodDensityFromMacro(macro,macro.heightThreshold,macro.weatherMask)*max(params.y,0.0);"));
    EXPECT_TRUE(Contains(shader, "opticalDepth+=sampleDensity*stepLength*cloudOpticalDepthScaleFromBand(macro.upperBand>0.5);"));
    EXPECT_TRUE(Contains(shader, "transmittance=exp(-max(opticalDepth,0.0)*max(cloudLightingExtinction.y,0.0));"));
    // 遠距離5点だけの内部照明キャッシュではなく、雲殻の全区間を32点で積分する。
    const std::size_t worldShadowBegin = shader.find("voidCSCloudWorldShadow(");
    const std::size_t worldShadowEnd = shader.find("uintCloudTemporalBlockPhase4(", worldShadowBegin);
    EXPECT_TRUE(worldShadowBegin != std::string::npos);
    EXPECT_TRUE(worldShadowEnd != std::string::npos);
    if (worldShadowBegin != std::string::npos && worldShadowEnd != std::string::npos) {
        const std::string worldShadow = shader.substr(worldShadowBegin, worldShadowEnd - worldShadowBegin);
        EXPECT_FALSE(Contains(worldShadow, "sampleCloudShadowTail("));
        EXPECT_FALSE(Contains(worldShadow, "traceCloudShadowPattern("));
    }
}

ACS_TEST(VolumetricClouds, WorldShadowResourceIsOptionalAndRebuiltBeforeCloudTracing) {
    const std::string source = ReadSkySource();
    const std::string compact = CompactShader(source);
    EXPECT_TRUE(!compact.empty());

    EXPECT_EQ(CountOccurrences(compact, "\"CSCloudWorldShadow\",\"Clouds.WorldShadowCS\""), static_cast<std::size_t>(2));
    EXPECT_TRUE(Contains(compact, "td.width=kVolumetricCloudWorldShadowMapResolution;td.height=kVolumetricCloudWorldShadowMapResolution;td.format=EFormat::R16G16B16A16_Float;td.is_uav=true;"));
    EXPECT_TRUE(Contains(compact, "pd.uav_slots=1;pd.uav_names[0]=\"cloudOut\";"));
    EXPECT_TRUE(Contains(compact, "cl.BindUav(0,*m_WorldShadowTex);constu32updateWidth=CloudCeilDivisor(kVolumetricCloudWorldShadowMapResolution-shadowUpdateOffsetX,shadowUpdateDivisor);constu32updateHeight=CloudCeilDivisor(kVolumetricCloudWorldShadowMapResolution-shadowUpdateOffsetY,shadowUpdateDivisor);cl.Dispatch((updateWidth+7u)/8u,(updateHeight+7u)/8u,1u);"));
    const std::size_t worldShadowDispatch =
        compact.find("if(rebuildWorldShadowThisFrame){");
    const std::size_t mainCloudDispatch = compact.find("cl.SetComputePipeline(*m_CloudPipe);", worldShadowDispatch);
    EXPECT_TRUE(worldShadowDispatch != std::string::npos);
    EXPECT_TRUE(mainCloudDispatch != std::string::npos);
    EXPECT_TRUE(worldShadowDispatch < mainCloudDispatch);
    EXPECT_TRUE(Contains(compact, "m_WorldShadowValid?m_WorldShadowTex.Get():nullptr;"));
}

ACS_TEST(VolumetricClouds,
         ShadowCacheKeepsNearLightingExactAndGuardsEveryFarLookup) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    EXPECT_TRUE(Contains(
        shader,
        "RWTexture3D<float4>cloudShadowOut:register(u2);"));
    EXPECT_TRUE(Contains(
        shader,
        "Texture3D<float4>cloudShadowCache:register(t4);"));
    EXPECT_TRUE(Contains(
        shader,
        "SamplerStatecloudShadowCache_sampler:register(s4);"));
    EXPECT_TRUE(Contains(shader, "float4cloudFrameTerms;"));
    EXPECT_TRUE(Contains(shader, "float4cloudLightTangent;"));
    EXPECT_TRUE(Contains(shader, "float4cloudLightBitangent;"));
    EXPECT_TRUE(Contains(
        shader,
        "floatradialY=max(CLOUD_PLANET_RADIUS+local.y,1.0);"
        "floatradialXz2=dot(local.xz,local.xz);"
        "floatinverseRadialY=1.0/radialY;"
        "floatq=radialXz2*inverseRadialY;"
        "returnlocal.y+q*(0.5-q*inverseRadialY*0.125);"));
    EXPECT_FALSE(Contains(
        shader,
        "returnlength(p-cloudPlanetCenter())-CLOUD_PLANET_RADIUS;"));
    EXPECT_FALSE(Contains(shader, "abs(sun.y)<0.99"));
    EXPECT_FALSE(Contains(shader, "voidcloudLightBasis("));
    EXPECT_FALSE(Contains(shader, "normalize(sunDir.xyz)"));
    EXPECT_EQ(
        CountOccurrences(
            shader,
            "float3lightTangent=cloudLightTangent.xyz;"),
        static_cast<std::size_t>(2));
    EXPECT_EQ(
        CountOccurrences(
            shader,
            "float3lightBitangent=cloudLightBitangent.xyz;"),
        static_cast<std::size_t>(2));
    EXPECT_TRUE(Contains(
        shader,
        "[numthreads(4,1,4)]voidCSCloudShadow("));
    EXPECT_TRUE(Contains(shader, "uint2outputColumn=uint2(" "tid.x*updateStride+(uint)cloudShadowUpdate.x," "tid.z*updateStride+(uint)cloudShadowUpdate.y);"));
    EXPECT_TRUE(Contains(shader, "cloudShadowOut.GetDimensions(width,height,depth);" "if(any(outputColumn>=uint2(width,depth))" "||height!=CLOUD_SHADOW_CACHE_HEIGHT)return;"));
    EXPECT_TRUE(Contains(shader, "[loop]for(intl=3;l<8;l++){"));
    EXPECT_TRUE(Contains(
        shader,
        "lightStep*=1.8;lightStep*=1.8;lightStep*=1.8;"));
    EXPECT_FALSE(Contains(shader, "lightStep*=5.832"));
    EXPECT_TRUE(Contains(
        shader,
        "float2q=lp.xz-cloudWindWorld();"));
    EXPECT_TRUE(Contains(
        shader,
        "float2worldXz=q+cloudWindWorld();"));
    EXPECT_TRUE(Contains(
        shader,
        "floatsag=d2/max(radius+root,1.0);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatminimumEdgeCells=min("
        "min(edgeCells.x,edgeCells.y),edgeCells.z);"
        "if(minimumEdgeCells>1.5){"));
    EXPECT_TRUE(Contains(
        shader,
        "floatborderWeight=smoothstep(1.5,2.5,minimumEdgeCells);"));
    EXPECT_EQ(
        CountOccurrences(shader, "cloudShadowCache.SampleLevel("),
        static_cast<std::size_t>(2));
    EXPECT_TRUE(Contains(
        shader,
        "floattauDisagreement="
        "cached.y*density*cloudLightingExtinction.y;"));
    EXPECT_TRUE(Contains(
        shader,
        "if(tauDisagreement<=shadowState.y){"));
    EXPECT_FALSE(Contains(
        shader,
        "if(tauDisagreement>shadowState.y)"
        "returnfloat3(0.0,0.0,0.0);"));
    EXPECT_TRUE(Contains(shader, "floatcolumnSegmentDepth[CLOUD_SHADOW_CACHE_HEIGHT];"));
    EXPECT_TRUE(Contains(
        shader,
        "[loop]for(uintdensityHeightIndex=0u;"
        "densityHeightIndex<CLOUD_SHADOW_CACHE_HEIGHT;"
        "++densityHeightIndex){"));
    EXPECT_TRUE(Contains(shader, "floatskyDepth=max(" "totalColumnDepth-groundDepth-halfSegmentDepth,0.0);"));
    EXPECT_TRUE(Contains(shader, "cloudShadowOut[outputVoxel]=float4(" "meanDepth,disagreement,skyDepth,sampleGroundDepth);"));
    EXPECT_TRUE(Contains(shader, "float3cachedAmbientDepth=sampleCloudAmbientDepth(p);"));
    EXPECT_FALSE(Contains(shader, "CSCloudShadowFinalize"));
    EXPECT_FALSE(Contains(shader, "cloudShadowCache.Load("));
    EXPECT_TRUE(Contains(
        shader,
        "float3result=float3(0.0,0.0,0.0);"));
    EXPECT_TRUE(Contains(
        shader,
        "result=float3(1.0,cached.x,cacheWeight);"));
    EXPECT_TRUE(Contains(shader, "returnresult;"));
    EXPECT_FALSE(Contains(shader, "}while(false);returnresult;"));

    const std::size_t viewLoop =
        shader.find("[loop]for(inti=0;i<MAX_STEPS");
    const std::size_t lightPhase = shader.find("floatlightJitter=frac(jit+float(i)*0.61803398875);", viewLoop);
    const std::size_t pairedTrig = shader.find("floatconeSin,coneCos;" "sincos(6.2831853*lightJitter,coneSin,coneCos);", lightPhase);
    const std::size_t nearLightLoop =
        shader.find("[loop]for(intl=0;l<3;l++)", pairedTrig);
    const std::size_t cacheAttempt =
        shader.find(
            "if(!lightTerminated&&CLOUD_MAIN_SHADOW_CACHE_ENABLED){",
            nearLightLoop);
    const std::size_t farLightLoop =
        shader.find("[loop]for(intl=3;l<8;l++)", cacheAttempt);
    const std::size_t coneDirection = shader.find(
        "float3coneDir=cloudConeDirection("
        "sun,lightTangent,lightBitangent,"
        "coneSin,coneCos,coneGeometry);",
        farLightLoop);
    EXPECT_TRUE(nearLightLoop != std::string::npos);
    EXPECT_TRUE(farLightLoop != std::string::npos);
    EXPECT_TRUE(cacheAttempt != std::string::npos);
    EXPECT_TRUE(lightPhase != std::string::npos);
    EXPECT_TRUE(pairedTrig != std::string::npos);
    EXPECT_TRUE(coneDirection != std::string::npos);
    EXPECT_TRUE(viewLoop < lightPhase);
    EXPECT_TRUE(lightPhase < pairedTrig);
    EXPECT_TRUE(pairedTrig < nearLightLoop);
    EXPECT_TRUE(nearLightLoop < cacheAttempt);
    EXPECT_TRUE(cacheAttempt < farLightLoop);
    EXPECT_TRUE(farLightLoop < coneDirection);
    EXPECT_FALSE(Contains(shader, "[branch]if(l>=3)"));
    EXPECT_TRUE(Contains(shader, "staticconstboolCLOUD_MAIN_SHADOW_CACHE_ENABLED=true;"));
    EXPECT_TRUE(Contains(
        shader,
        "float3cachedTailSample=sampleCloudShadowTail(lp,density);"
        "if(cachedTailSample.x>0.5){"));
    EXPECT_TRUE(Contains(
        shader,
        "if(cacheWeight>=0.999){lightDepth+=cachedTail;"
        "cachedFarTail=true;}"));
    EXPECT_TRUE(Contains(
        shader,
        "lightDepth=exactFarStart+lerp("
        "exactTail,cachedTailForBlend,cacheBlendWeight);"));
    // 棄却した参照は各地点の天候と基本形状を再採取する遠距離5点へ戻り、
    // 侵食を含む近距離3点は常に別のループで採取する。
    EXPECT_TRUE(Contains(
        shader,
        "float2farLightSample=sampleCloudFarLightingDensityAndScale("
        "lp,coverage,sharedLightCurl,lightStep);"));
    EXPECT_TRUE(Contains(shader, "floatlightDensity=cloudDensityFromMacro(lp,lightMacro,lightMacro.heightThreshold,lightMacro.weatherMask,lightBillowVisibility,lightErosionVisibility);"));
    // 視線側の天候と渦を共有するのは近距離3点だけで、基本形状は各点を採取する。
    EXPECT_TRUE(Contains(
        shader,
        "float4sharedLightProfileTerms=float4("
        "cloudProfileTypeWeights(macro.weather.g),"
        "macro.weather.b,cloudColumnHeightShift("
        "macro.weather,macro.densityWeatherMask));"
        "floatsharedLightBaseLift=cloudColumnBaseLift("
        "macro.weather,macro.densityWeatherMask);"
        "float2sharedLightCurl=macro.curl;"));
    EXPECT_FALSE(Contains(
        shader, "sharedLightProfileTerms=cloudWeatherData(lp)"));
    EXPECT_FALSE(Contains(shader, "sharedLightCurl=cloudCurlOffset(lp)"));
    EXPECT_TRUE(Contains(
        shader,
        "sampleCloudMacroLightingFromSlowFields("
        "lp,viewWeatherMask,coverageTerms.w,"
        "sharedLightProfileTerms,sharedLightBaseLift,sharedLightCurl,"
        "p,viewMacroUvw,macro.height,sharedShapeScale,lightStep);"));
    EXPECT_FALSE(Contains(
        shader, "sampleCloudLightingDensityFromSlowFields("));
}

ACS_TEST(VolumetricClouds, ShadowCacheRhiBindingIsOptionalOrderedAndUpdatedEveryFrame) {
    const std::string source = ReadSkySource();
    const std::string compact = CompactShader(source);
    EXPECT_TRUE(!compact.empty());
    EXPECT_TRUE(kVolumetricCloudShadowCacheEnabled);
    // どちらの初期化経路でも任意シェーダーを一つだけ用意する。作成に失敗しても
    // 必須の雲描画は公開でき、遠距離の正確な積分へ戻る。
    EXPECT_EQ(
        CountOccurrences(
            compact,
            "autoshadow_result=compile(EShaderStage::Compute,kCloudCS,"
            "\"CSCloudShadow\",\"Clouds.ShadowCacheCS\");"),
        static_cast<std::size_t>(2));
    EXPECT_FALSE(Contains(compact, "CSCloudShadowFinalize"));
    EXPECT_FALSE(Contains(compact, "m_ShadowFinalize"));
    EXPECT_FALSE(Contains(compact, "m_ShadowRawTex"));
    EXPECT_TRUE(Contains(
        compact,
        "boolshadowOk=kVolumetricCloudShadowCacheEnabled&&"
        "shaders.shadow;"
        "if(shadowOk)m_ShadowCs=Move(shaders.shadow);"));
    EXPECT_TRUE(Contains(
        compact,
        "if(kVolumetricCloudShadowCacheEnabled){ACS_LOG_WARN("));
    EXPECT_FALSE(Contains(compact, "Temporarycache-offA/B"));

    EXPECT_TRUE(Contains(compact, "pd.srv_slots=5;"));
    EXPECT_TRUE(Contains(
        compact, "pd.srv_names[4]=\"cloudShadowCache\";"));
    EXPECT_TRUE(Contains(compact, "pd.static_sampler_count=5;"));
    EXPECT_TRUE(Contains(
        compact, "pd.uav_slots=3;pd.uav_names[0]=\"cloudOut\";"
                 "pd.uav_names[1]=\"cloudDepthOut\";"
                 "pd.uav_names[2]=\"cloudShadowOut\";"));
    EXPECT_TRUE(Contains(
        compact,
        "td.width=kVolumetricCloudShadowCacheWidth;"
        "td.height=kVolumetricCloudShadowCacheHeight;"
        "td.depth=kVolumetricCloudShadowCacheDepth;"
        "td.format=EFormat::R16G16B16A16_Float;td.is_uav=true;"));
    EXPECT_EQ(CountOccurrences(compact, "td.width=kVolumetricCloudShadowCacheWidth;" "td.height=kVolumetricCloudShadowCacheHeight;" "td.depth=kVolumetricCloudShadowCacheDepth;" "td.format=EFormat::R16G16B16A16_Float;td.is_uav=true;"), static_cast<std::size_t>(1));
    EXPECT_TRUE(Contains(compact, "constu32updateWidth=CloudCeilDivisor(" "kVolumetricCloudShadowCacheWidth-shadowUpdateOffsetX," "shadowUpdateDivisor);" "constu32updateDepth=CloudCeilDivisor(" "kVolumetricCloudShadowCacheDepth-shadowUpdateOffsetY," "shadowUpdateDivisor);" "cl.Dispatch((updateWidth+3u)/4u," "1u," "(updateDepth+3u)/4u);"));
    EXPECT_TRUE(Contains(compact, "rebuildShadowCacheThisFrame?1.0f:0.0f"));
    EXPECT_TRUE(Contains(
        compact,
        "if(!shadowOk){m_ShadowTex.Reset();"
        "m_ShadowPipe.Reset();m_ShadowCs.Reset();"));
    const std::size_t optionalBegin = compact.find("boolshadowOk=kVolumetricCloudShadowCacheEnabled&&" "shaders.shadow;");
    const std::size_t optionalEnd = compact.find(
        "m_ShadowCacheDispatchCount=0;}", optionalBegin);
    EXPECT_TRUE(optionalBegin != std::string::npos);
    EXPECT_TRUE(optionalEnd != std::string::npos);
    if (optionalBegin != std::string::npos &&
        optionalEnd != std::string::npos) {
        EXPECT_FALSE(Contains(
            compact.substr(optionalBegin, optionalEnd - optionalBegin),
            "returnErr"));
    }

    const std::size_t noiseBake =
        compact.find("if(bakeShapeNoiseThisFrame){");
    const std::size_t shadowBuild =
        compact.find("if(rebuildShadowCacheThisFrame){");
    const std::size_t dummyU0 =
        compact.find("cl.BindUav(0,*m_CloudTex);", shadowBuild);
    const std::size_t dummyU1 =
        compact.find("cl.BindUav(1,*m_CloudDepth);", dummyU0);
    const std::size_t shadowU2 =
        compact.find("cl.BindUav(2,*m_ShadowTex);", dummyU1);
    const std::size_t mainPipeline =
        compact.find("cl.SetComputePipeline(*m_CloudPipe);", shadowU2);
    const std::size_t cacheT4 =
        compact.find("cl.SetTexture(4,*m_ShadowTex);", mainPipeline);
    const std::size_t dummyT4 =
        compact.find("cl.SetTexture(4,*m_ShapeTex);", cacheT4);
    EXPECT_TRUE(noiseBake != std::string::npos);
    EXPECT_TRUE(shadowBuild != std::string::npos);
    EXPECT_TRUE(dummyU0 != std::string::npos);
    EXPECT_TRUE(dummyU1 != std::string::npos);
    EXPECT_TRUE(shadowU2 != std::string::npos);
    EXPECT_TRUE(mainPipeline != std::string::npos);
    EXPECT_TRUE(cacheT4 != std::string::npos);
    EXPECT_TRUE(dummyT4 != std::string::npos);
    EXPECT_TRUE(noiseBake < shadowBuild);
    EXPECT_TRUE(shadowBuild < dummyU0);
    EXPECT_TRUE(dummyU0 < dummyU1);
    EXPECT_TRUE(dummyU1 < shadowU2);
    EXPECT_TRUE(shadowU2 < mainPipeline);
    EXPECT_TRUE(mainPipeline < cacheT4);
    EXPECT_TRUE(cacheT4 < dummyT4);

    EXPECT_TRUE(Contains(compact, "m_ShadowCacheAvailable&&m_ShadowCs&&m_ShadowPipe&&m_ShadowTex;"));
    EXPECT_TRUE(Contains(compact, "constboolrebuildShadowCacheThisFrame=shadowResourcesReady&&" "(m_NoiseBaked||bakeShapeNoiseThisFrame)&&" "(m_WeatherBaked||bakeWeatherThisFrame)&&" "(m_DetailBaked||bakeDetailNoiseThisFrame)&&" "(m_CurlBaked||bakeCurlNoiseThisFrame);"));
    EXPECT_TRUE(Contains(compact, "if(!rebuildShadowCacheThisFrame)m_ShadowCacheValid=false;"));
    EXPECT_TRUE(Contains(compact, "if(!rebuildWorldShadowThisFrame)m_WorldShadowValid=false;"));
    EXPECT_TRUE(Contains(compact, "constboolshadowCacheNeedsFullRefresh=rebuildShadowCacheThisFrame&&(!m_ShadowCacheValid||shadowGridChanged);"));
    EXPECT_TRUE(Contains(compact, "constboolworldShadowNeedsFullRefresh=rebuildWorldShadowThisFrame&&(!m_WorldShadowValid||worldShadowMappingChanged);"));
    EXPECT_TRUE(Contains(compact, "constboolrefreshAllShadows=m_ReferenceMode||!historyValid||shadowCacheNeedsFullRefresh||worldShadowNeedsFullRefresh;"));
    EXPECT_TRUE(Contains(compact, "constu32shadowUpdateDivisor=refreshAllShadows?1u:kVolumetricCloudShadowTemporalDivisor;"));
    EXPECT_FALSE(Contains(compact, "shadowDirty"));
    EXPECT_FALSE(Contains(compact, "shadowWillBuildThisFrame"));
    EXPECT_FALSE(Contains(compact, "shadowCacheUsableThisFrame"));
    EXPECT_FALSE(Contains(compact, "kShadowSunDirectionCosThreshold"));
    EXPECT_TRUE(Contains(
        compact,
        "m_ShadowCacheValid=false;"));
    EXPECT_TRUE(Contains(
        compact,
        "++m_ShadowCacheDispatchCount;"));
    EXPECT_EQ(
        CountOccurrences(compact, "++m_ShadowCacheDispatchCount;"),
        static_cast<std::size_t>(1));
    const std::size_t logicalBuildEnd = compact.find(
        "++m_ShadowCacheDispatchCount;", shadowBuild);
    if (shadowBuild != std::string::npos &&
        logicalBuildEnd != std::string::npos) {
        EXPECT_EQ(CountOccurrences(compact.substr(shadowBuild, logicalBuildEnd - shadowBuild), "cl.Dispatch("), static_cast<std::size_t>(1));
    }
    EXPECT_FALSE(Contains(
        compact, "false&&m_ShadowCacheAvailable"));
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
    EXPECT_TRUE(Contains(compact, "m_PrevCameraRelativeInvViewProj=camera_relative_inv_view_proj;"));
    EXPECT_TRUE(Contains(
        compact, "m_PrevSunColor=safeSunColor;"));
    EXPECT_TRUE(Contains(
        compact, "m_PrevSkyColor=safeSkyColor;"));
}
