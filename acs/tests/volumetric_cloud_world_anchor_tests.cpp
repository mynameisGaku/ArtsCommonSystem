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
        12.566370f *
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

f32 SmoothStepForTest(f32 edge0, f32 edge1, f32 value) noexcept {
    const f32 t = SaturateForTest(
        (value - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

f32 CloudWeatherMaskForTest(f32 weatherCoverage, f32 coverage) noexcept {
    const f32 threshold =
        0.90f + (0.35f - 0.90f) * SaturateForTest(coverage);
    const f32 upper =
        threshold + 0.14f < 0.98f ? threshold + 0.14f : 0.98f;
    return SmoothStepForTest(
        threshold, upper, weatherCoverage);
}

f32 CloudColumnHeightShiftForTest(
    f32 weatherCoverage, f32 cloudType, f32 precipitation,
    f32 warp, f32 evolutionPhase) noexcept {
    const f32 core = SmoothStepForTest(
        0.38f, 0.74f, weatherCoverage);
    const f32 verticalType = SaturateForTest(
        cloudType > precipitation ? cloudType : precipitation);
    const f32 amplitude =
        0.025f + (0.18f - 0.025f) * verticalType;
    f32 evolvingWarp = warp - 0.5f + evolutionPhase * 0.45f;
    if (evolvingWarp < -0.5f) evolvingWarp = -0.5f;
    if (evolvingWarp > 0.5f) evolvingWarp = 0.5f;
    f32 signal =
        (core - 0.45f) * 1.45f + evolvingWarp * 0.65f;
    if (signal < -1.0f) signal = -1.0f;
    if (signal > 1.0f) signal = 1.0f;
    return signal * amplitude;
}

f32 CloudConvectiveHeightForTest(
    f32 height, f32 columnShift, bool upperBand) noexcept {
    const f32 boundedHeight = SaturateForTest(height);
    const f32 interior =
        4.0f * boundedHeight * boundedHeight * (1.0f - boundedHeight);
    const f32 bandScale = upperBand ? 0.30f : 1.0f;
    return SaturateForTest(
        boundedHeight - columnShift * interior * bandScale);
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
    const std::size_t producerCall = editorSource.find(
        "builtAp = h.sky_atmo.BuildAerialPerspective(");
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
        "*cdev,scW,scH,h.q_cloud_render_scale)){"
        "cloudsActive=true;}";
    EXPECT_TRUE(Contains(editorSource, volumetricGate));
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
         WorkloadDiagnosticsSeparateSteadyBakeAndShadowDispatches) {
    FVolumetricCloudFrameWorkloadPlan steadyPlan{};
    steadyPlan.trace_width = 480u;
    steadyPlan.trace_height = 270u;
    steadyPlan.output_width = 1920u;
    steadyPlan.output_height = 1080u;
    const FVolumetricCloudFrameWorkload steady =
        PlanVolumetricCloudFrameWorkload(steadyPlan);

    EXPECT_EQ(steady.steady_dispatches, 2u);
    EXPECT_EQ(steady.one_time_bake_dispatches, 0u);
    EXPECT_EQ(steady.shadow_cache_dispatches, 0u);
    EXPECT_EQ(steady.total_compute_dispatches, 2u);
    EXPECT_EQ(steady.trace_logical_invocations, 129600u);
    EXPECT_EQ(steady.trace_launched_threads, 130560u);
    EXPECT_EQ(steady.resolve_logical_invocations, 2073600u);
    EXPECT_EQ(steady.resolve_launched_threads, 2073600u);
    EXPECT_EQ(steady.total_logical_invocations, 2203200u);
    EXPECT_EQ(steady.total_launched_threads, 2204160u);
    EXPECT_EQ(steady.maximum_view_samples, 24883200u);
    EXPECT_EQ(steady.maximum_light_samples, 199065600u);
    EXPECT_TRUE(steady.temporal_super_resolution);
    EXPECT_FALSE(steady.attempted);
    EXPECT_FALSE(steady.submitted);

    FVolumetricCloudFrameWorkloadPlan coldPlan = steadyPlan;
    coldPlan.bake_shape_noise = true;
    coldPlan.bake_weather = true;
    coldPlan.bake_detail_noise = true;
    coldPlan.bake_curl_noise = true;
    coldPlan.rebuild_shadow_cache = true;
    const FVolumetricCloudFrameWorkload cold =
        PlanVolumetricCloudFrameWorkload(coldPlan);

    EXPECT_EQ(cold.steady_dispatches, 2u);
    EXPECT_EQ(cold.one_time_bake_dispatches, 4u);
    EXPECT_EQ(cold.shadow_cache_dispatches, 2u);
    EXPECT_EQ(cold.total_compute_dispatches, 8u);
    EXPECT_EQ(cold.one_time_bake_logical_invocations, 2637824u);
    EXPECT_EQ(cold.one_time_bake_launched_threads, 2637824u);
    EXPECT_EQ(cold.shadow_cache_logical_invocations, 589824u);
    EXPECT_EQ(cold.shadow_cache_launched_threads, 589824u);
    EXPECT_EQ(cold.total_logical_invocations, 5430848u);
    EXPECT_EQ(cold.total_launched_threads, 5431808u);
    EXPECT_EQ(cold.maximum_view_samples, steady.maximum_view_samples);
    EXPECT_EQ(cold.maximum_light_samples, steady.maximum_light_samples);
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
    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 192u);
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
    EXPECT_TRUE(vertical.fine_step * 168.0f >=
                vertical.exit - vertical.enter);
    EXPECT_TRUE(oblique.fine_step * 168.0f >=
                oblique.exit - oblique.enter);
    EXPECT_TRUE(vertical.coarse_step * 84.0f >=
                vertical.exit - vertical.enter);
    EXPECT_TRUE(oblique.coarse_step * 84.0f >=
                oblique.exit - oblique.enter);
    EXPECT_EQ(vertical.max_samples, 192u);
    EXPECT_EQ(oblique.max_samples, 192u);
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
    EXPECT_TRUE(horizon.fine_step * 168.0f >=
                horizon.exit - horizon.enter);
    EXPECT_TRUE(horizon.coarse_step * 84.0f >=
                horizon.exit - horizon.enter);
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
        EXPECT_TRUE(plan.fine_step * 168.0f >= span);
        EXPECT_TRUE(plan.coarse_step * 84.0f >= span);
        EXPECT_EQ(plan.max_samples, 192u);
    }

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(Contains(
        shader,
        "intMAX_STEPS=(int)cloudLightingAmbient.z;"
        "if(MAX_STEPS<32)MAX_STEPS=32;"));
    EXPECT_TRUE(Contains(shader, "floatspan=t1-t0;"));
    EXPECT_TRUE(Contains(
        shader, "floatbaseFineStep=cloudCoverageReciprocals.z;"));
    EXPECT_TRUE(Contains(
        shader, "floatfineStep=max(baseFineStep,span/168.0);"));
    EXPECT_TRUE(Contains(
        shader, "floatcoarseStep=max(fineStep*2.0,span/84.0);"));
    EXPECT_FALSE(Contains(shader, "constintMAX_STEPS=128;"));
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

    // The ray origin is camera-dependent, but the resulting sample position is
    // an absolute world point.  Noise/weather lookups must consume that point
    // directly instead of subtracting camera position again.
    EXPECT_TRUE(Contains(
        shader, "float3 p=camPos.xyz+dir*t;"));
    EXPECT_TRUE(Contains(
        shader, "float MAX_DISTANCE=cloudRange.x;"));
    EXPECT_TRUE(Contains(
        shader, "float3 local=p-worldOrigin.xyz;"));
    EXPECT_TRUE(Contains(
        shader, "macro.weather=cloudWeatherData(p);"));
    EXPECT_TRUE(Contains(
        CompactShader(shader),
        "sampleUvw=cloudUVW("
        "p,macro.weather,macro.curl,macro.height);"
        "cloudBaseShape("
        "sampleUvw,macro.heightThreshold,macro.baseNoise);"));
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
    const std::string detailShader = CompactShader(
        ExtractRawShader(source, "const char* kDetailGenCS"));
    const std::string compactSource = CompactShader(source);
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(!noiseShader.empty());
    EXPECT_TRUE(!detailShader.empty());

    // Macro layout, base shape, edge erosion, and shear are deliberately
    // separate data domains.  Reusing the shape volume for all four brings
    // back the conspicuous tiled motif this architecture is meant to avoid.
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

    // Only channels consumed by runtime density are stored.  The surviving
    // values remain FP16, while RG16F halves both volume footprint and fetch
    // bandwidth compared with the former RGBA16F resources.
    EXPECT_TRUE(Contains(
        noiseShader, "RWTexture3D<float2>noiseOut:register(u0);"));
    EXPECT_TRUE(Contains(noiseShader, "noiseOut[id]=float2(pw,w0);"));
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
    EXPECT_TRUE(Contains(compactMarch, "floatbasePerlinWorley(float2ns)"));
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
    EXPECT_TRUE(Contains(
        resolveShader,
        "floatcurA=exactCurrent?saturate(refC.a):"
        "saturate(alphaSum/max(weightSum,1e-5));"));
}

ACS_TEST(VolumetricClouds,
         DetailErosionPrecedesSoftProfileAndWeatherAttenuation) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    const std::size_t profile =
        shader.find("floatsampledProfile=cloudProfile(");
    const std::size_t threshold = shader.find(
        "macro.heightThreshold=cloudHeightThresholdFromTarget(", profile);
    const std::size_t base = shader.find("floatbaseDensity=remapc(", threshold);
    const std::size_t erosion = shader.find(
        "remapc(baseDensity,detail*erosion,1.0,0.0,1.0)");
    const std::size_t attenuation = shader.find(
        "d*weatherMask", erosion);
    EXPECT_TRUE(profile != std::string::npos);
    EXPECT_TRUE(threshold != std::string::npos);
    EXPECT_TRUE(base != std::string::npos);
    EXPECT_TRUE(erosion != std::string::npos);
    EXPECT_TRUE(attenuation != std::string::npos);
    EXPECT_TRUE(profile < threshold);
    EXPECT_TRUE(threshold < base);
    EXPECT_TRUE(base < erosion);
    EXPECT_TRUE(erosion < attenuation);
    EXPECT_FALSE(Contains(shader, "d*profile*weatherMask"));
}

ACS_TEST(VolumetricClouds,
         ConvectiveHeightWarpIsBoundedMonotonicAndSharedWithLighting) {
    const f32 tallCore = CloudColumnHeightShiftForTest(
        1.0f, 1.0f, 0.0f, 1.0f, 0.0f);
    const f32 compressedEdge = CloudColumnHeightShiftForTest(
        0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
    const f32 stratusCore = CloudColumnHeightShiftForTest(
        1.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    EXPECT_NEAR(tallCore, 0.18f, 1e-6f);
    EXPECT_NEAR(compressedEdge, -0.17595f, 1e-6f);
    EXPECT_NEAR(stratusCore, 0.025f, 1e-6f);

    // 高さ変形は層の両端を固定し、全許容変形量で折り返さない。
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
        "floatcloudColumnHeightShift(float4weather){"
        "floatcore=smoothstep(0.38,0.74,weather.r);"
        "floatverticalType=saturate(max(weather.g,weather.b));"
        "floatamplitude=lerp(0.025,0.18,verticalType);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudConvectiveHeight("
        "floath,floatcolumnShift,boolupperBand){"
        "h=saturate(h);"
        "floatinterior=4.0*h*h*(1.0-h);"
        "floatbandScale=upperBand?0.30:1.0;"
        "returnsaturate(h-columnShift*interior*bandScale);}"));
    EXPECT_TRUE(Contains(
        shader,
        "float4sharedLightProfileTerms=float4("
        "cloudProfileTypeWeights(macro.weather.g),"
        "macro.weather.b,cloudColumnHeightShift(macro.weather));"));
    EXPECT_TRUE(Contains(
        shader,
        "macro.height=cloudConvectiveHeight("
        "heightFractionFromAltitude(altitude,upperBand),"
        "slowProfileTerms.w,upperBand);"));
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

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    const std::string compactSource = CompactShader(source);
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(Contains(
        shader,
        "floatcloudWeatherThreshold(floatcoverage){"
        "returnlerp(0.90,0.35,saturate(coverage));}"
        "floatcloudWeatherMaskFromThreshold(float4weather,floatthreshold){"
        "returnsmoothstep("
        "threshold,min(threshold+0.14,0.98),weather.r);}"));
    EXPECT_TRUE(Contains(shader, "float4cloudCoverage;"));
    EXPECT_TRUE(Contains(
        compactSource,
        "constf32occupancyCoverage="
        "safeCoverage+0.08f<1.0f?safeCoverage+0.08f:1.0f;"));
    EXPECT_TRUE(Contains(
        compactSource,
        "cb.cloudCoverage=FVec4{"
        "0.90f-0.55f*occupancyCoverage,"
        "0.90f-0.55f*safeCoverage,"));
    const char* declarations[]{
        "CloudMacroSamplesampleCloudMacro(",
        "CloudMacroSamplesampleCloudMacroLighting("};
    const char* initializers[]{
        "macro.weather=float4(0,0,0,0);",
        "macro.curl=float2(0,0);",
        "macro.baseNoise=0.0;",
        "macro.weatherMask=0.0;",
        "macro.profileWeight=0.0;",
        "macro.heightThreshold=0.78;",
        "macro.height=0.0;"};
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
        const std::size_t weather =
            function.find("macro.weather=cloudWeatherData(p);");
        const std::size_t mask =
            function.find("macro.weatherMask=cloudWeatherMask");
        const std::size_t maskBranch =
            function.find("if(macro.weatherMask>0.001){");
        const std::size_t height =
            function.find("macro.height=cloudConvectiveHeight(");
        const std::size_t profile =
            function.find("floatsampledProfile=cloudProfile(");
        const std::size_t profileBranch =
            function.find("if(macro.profileWeight>0.0){");
        const std::size_t profileWeight =
            function.find("macro.profileWeight=smoothstep(");
        const std::size_t profileShape =
            function.find("floatprofileShape=pow(");
        const std::size_t heightThreshold =
            function.find(
                "macro.heightThreshold=cloudHeightThreshold");
        const std::size_t curl =
            function.find("macro.curl=cloudCurlOffset(p);");
        const std::size_t shape =
            function.find("cloudBaseShape");
        for (const char* initializer : initializers) {
            const std::size_t initialized =
                function.find(initializer);
            EXPECT_TRUE(initialized != std::string::npos);
            EXPECT_TRUE(initialized < weather);
        }
        EXPECT_TRUE(weather < mask);
        EXPECT_TRUE(mask < maskBranch);
        EXPECT_TRUE(maskBranch < height);
        EXPECT_TRUE(height < profile);
        EXPECT_TRUE(profile < profileWeight);
        EXPECT_TRUE(profileWeight < profileBranch);
        EXPECT_TRUE(profileBranch < profileShape);
        EXPECT_TRUE(profileShape < heightThreshold);
        EXPECT_TRUE(heightThreshold < curl);
        EXPECT_TRUE(curl < shape);
    }

}

ACS_TEST(VolumetricClouds,
         MacroDensityIsReusedAndShapeUsesFourWorldSpaceDomains) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    // The view ray obtains one macro sample and reuses it for conservative
    // occupancy and detailed density. This removes a duplicate weather/curl/
    // base-volume evaluation from every occupied view sample.
    EXPECT_TRUE(Contains(
        shader,
        "float3viewMacroUvw;"
        "floatdensityHeightThreshold;"
        "CloudMacroSamplemacro=sampleCloudMacro("
        "p,coverageTerms,"
        "viewMacroUvw,densityHeightThreshold);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatshape=cloudShapeFromMacro(macro);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatviewWeatherMask=cloudWeatherMaskFromTerms("
        "macro.weather,coverageTerms.y,cloudCoverageReciprocals.y);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatdens=cloudDensityFromMacro("
        "p,macro,densityHeightThreshold,"
        "viewWeatherMask,detailWeight)*density;"));
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

    // Four independently transformed domains make the shared tile period much
    // longer while all inputs remain absolute-world coordinates.
    EXPECT_TRUE(Contains(shader, "float3uvwD=float3("));
    EXPECT_TRUE(Contains(
        shader,
        ")*4.73+float3(0.263,0.887,0.491)"
        "+float3(cloudEvolution.y,-cloudEvolution.x,cloudEvolution.x);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatshape=basePerlinWorley(a)*0.45;"));
    EXPECT_TRUE(Contains(
        shader,
        "shape+=basePerlinWorley(b)*0.27;"));
    EXPECT_TRUE(Contains(
        shader,
        "shape+=basePerlinWorley(c)*0.17;"));
    EXPECT_TRUE(Contains(
        shader,
        "shapeResult=saturate(shape+basePerlinWorley(d)*0.11);"));
    const std::size_t viewShapeBegin =
        shader.find(
            "voidcloudBaseShape("
            "float3uvw,floatrejectionThreshold,outfloatshapeResult){");
    const std::size_t lightShapeBegin =
        shader.find(
            "voidcloudBaseShapeLighting("
            "float3uvw,floatrejectionThreshold,outfloatshapeResult){");
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

ACS_TEST(VolumetricClouds,
         ShapeLobeUpperBoundsSkipOnlyProvablyEmptyDensity) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    // The early-outs retain all four de-tiling domains whenever an unvisited
    // lobe could still cross the exact Nubis height threshold. They eliminate
    // only texture reads whose [0,1] upper bound proves zero final density.
    EXPECT_TRUE(Contains(
        shader,
        "[branch]if(shape+0.55<rejectionThreshold-1e-5)return;"));
    EXPECT_TRUE(Contains(
        shader,
        "[branch]if(shape+0.28<rejectionThreshold-1e-5)return;"));
    EXPECT_TRUE(Contains(
        shader,
        "[branch]if(shape+0.11<rejectionThreshold-1e-5)return;"));
    EXPECT_TRUE(Contains(
        shader,
        "[branch]if(shape+0.49<rejectionThreshold-1e-5)return;"));
    EXPECT_TRUE(Contains(
        shader,
        "[branch]if(shape+0.19<rejectionThreshold-1e-5)return;"));

    // Keep FXC's flow analysis from treating an inlined helper out value as
    // potentially undefined. Both fields initialize their out storage before
    // every conservative early return while retaining the exact progressive
    // texture-fetch gates.
    const std::size_t viewShapeBegin =
        shader.find(
            "voidcloudBaseShape("
            "float3uvw,floatrejectionThreshold,outfloatshapeResult){");
    const std::size_t lightShapeBegin =
        shader.find(
            "voidcloudBaseShapeLighting("
            "float3uvw,floatrejectionThreshold,outfloatshapeResult){");
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

    u32 fourLobeRejects = 0u;
    u32 fourLobeViolations = 0u;
    u32 threeLobeRejects = 0u;
    u32 threeLobeViolations = 0u;
    for (u32 state = 0u; state < 65536u; ++state) {
        const f32 a = static_cast<f32>(state & 15u) / 15.0f;
        const f32 b = static_cast<f32>((state >> 4u) & 15u) / 15.0f;
        const f32 c = static_cast<f32>((state >> 8u) & 15u) / 15.0f;
        const f32 d = static_cast<f32>((state >> 12u) & 15u) / 15.0f;
        const f32 threshold =
            0.50f + 0.28f *
                static_cast<f32>((state * 37u) & 255u) / 255.0f;

        const f32 fullFour =
            a * 0.45f + b * 0.27f + c * 0.17f + d * 0.11f;
        f32 partialFour = a * 0.45f;
        bool rejectedFour =
            partialFour + 0.55f < threshold - 1.0e-5f;
        if (!rejectedFour) {
            partialFour += b * 0.27f;
            rejectedFour =
                partialFour + 0.28f < threshold - 1.0e-5f;
        }
        if (!rejectedFour) {
            partialFour += c * 0.17f;
            rejectedFour =
                partialFour + 0.11f < threshold - 1.0e-5f;
        }
        if (rejectedFour) {
            ++fourLobeRejects;
            if (fullFour > threshold) ++fourLobeViolations;
        }

        const f32 fullThree =
            a * 0.51f + b * 0.30f + c * 0.19f;
        f32 partialThree = a * 0.51f;
        bool rejectedThree =
            partialThree + 0.49f < threshold - 1.0e-5f;
        if (!rejectedThree) {
            partialThree += b * 0.30f;
            rejectedThree =
                partialThree + 0.19f < threshold - 1.0e-5f;
        }
        if (rejectedThree) {
            ++threeLobeRejects;
            if (fullThree > threshold) ++threeLobeViolations;
        }
    }
    EXPECT_TRUE(fourLobeRejects > 0u);
    EXPECT_TRUE(threeLobeRejects > 0u);
    EXPECT_EQ(fourLobeViolations, 0u);
    EXPECT_EQ(threeLobeViolations, 0u);

    // Occupancy uses coverage+0.08. Because higher coverage lowers the shape
    // threshold, its rejection threshold is never stricter than the detailed
    // density pass that reuses the same macro sample.
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
                    0.72f + (0.50f - 0.72f) * clampedCoverage;
                return 0.78f +
                       (cloudThreshold - 0.78f) * profile;
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
    EXPECT_TRUE(Contains(
        shader, "floatbaseLightStep=cloudCoverageReciprocals.w;"));
    EXPECT_TRUE(Contains(
        shader, "floatlightStep=baseLightStep;"));

    const std::size_t pairedTrig = shader.find(
        "floatconeSin,coneCos;"
        "sincos(6.2831853*jit,coneSin,coneCos);",
        viewLoop);
    const std::size_t nearLightLoop =
        shader.find("[loop]for(intl=0;l<3;l++)", pairedTrig);
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
    const std::size_t lightAdvance =
        shader.find("lp+=coneDir*lightStep;", coneDirection);
    const std::size_t lightMacro = shader.find(
        "CloudMacroSamplelightMacro="
        "sampleCloudMacroLightingFromSlowFields("
        "lp,viewWeatherMask,coverageTerms.w,"
        "sharedLightProfileTerms,sharedLightCurl,"
        "p,viewMacroUvw,macro.height,sharedShapeScale);",
        lightAdvance);
    const std::size_t nearDensity = shader.find(
        "floatlightDensity=cloudDensityFromPositiveWeatherMacro("
        "lp,lightMacro,lightMacro.heightThreshold,"
        "lightMacro.weatherMask,0.65);",
        lightMacro);
    const std::size_t lightAccumulate = shader.find(
        "lightDepth+=lightDensity*lightStep*layer.w;",
        nearDensity);
    const std::size_t recurrence = shader.find(
        "floatpreviousConeCos=coneCos;"
        "coneCos=previousConeCos*(-0.737368878)"
        "-coneSin*(-0.675490294);"
        "coneSin=coneSin*(-0.737368878)"
        "+previousConeCos*(-0.675490294);",
        lightAccumulate);
    EXPECT_TRUE(Contains(
        shader, "staticconstboolCLOUD_MAIN_SHADOW_CACHE_ENABLED=false;"));
    EXPECT_TRUE(nearLightLoop != std::string::npos);
    EXPECT_TRUE(farLightLoop != std::string::npos);
    EXPECT_TRUE(pairedTrig != std::string::npos);
    EXPECT_TRUE(cacheCompileOut != std::string::npos);
    EXPECT_TRUE(coneDirection != std::string::npos);
    EXPECT_TRUE(lightAdvance != std::string::npos);
    EXPECT_TRUE(lightMacro != std::string::npos);
    EXPECT_TRUE(nearDensity != std::string::npos);
    EXPECT_TRUE(lightAccumulate != std::string::npos);
    EXPECT_TRUE(recurrence != std::string::npos);
    EXPECT_TRUE(pairedTrig < nearLightLoop);
    EXPECT_TRUE(nearLightLoop < coneDirection);
    EXPECT_TRUE(coneDirection < lightAdvance);
    EXPECT_TRUE(lightAdvance < lightMacro);
    EXPECT_TRUE(lightMacro < nearDensity);
    EXPECT_TRUE(nearDensity < lightAccumulate);
    EXPECT_TRUE(lightAccumulate < recurrence);
    EXPECT_TRUE(recurrence < cacheCompileOut);
    EXPECT_TRUE(cacheCompileOut < farLightLoop);
    if (nearLightLoop != std::string::npos &&
        farLightLoop != std::string::npos) {
        const std::string lightBody = shader.substr(
            nearLightLoop, farLightLoop - nearLightLoop);
        EXPECT_EQ(
            CountOccurrences(
                lightBody,
                "sampleCloudMacroLightingFromSlowFields("
                "lp,viewWeatherMask,coverageTerms.w,"
                "sharedLightProfileTerms,sharedLightCurl,"
                "p,viewMacroUvw,macro.height,sharedShapeScale)"),
            static_cast<std::size_t>(1));
        EXPECT_FALSE(Contains(
            lightBody, "sampleCloudMacroLighting(lp,coverage)"));
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
        EXPECT_EQ(CountOccurrences(
                      completeLightSection,
                      "sampleCloudMacroLightingFromSlowFields("
                      "lp,viewWeatherMask,coverageTerms.w,"
                      "sharedLightProfileTerms,sharedLightCurl,"
                      "p,viewMacroUvw,macro.height,sharedShapeScale)"),
                  static_cast<std::size_t>(1));
        EXPECT_EQ(CountOccurrences(
                      completeLightSection,
                      "sampleCloudLightingShapeFromSlowFields("
                      "lp,viewWeatherMask,coverageTerms.w,"
                      "sharedLightProfileTerms,"
                      "p,viewMacroUvw,macro.height,sharedShapeScale)"),
                  static_cast<std::size_t>(1));
        EXPECT_EQ(CountOccurrences(
                      completeLightSection,
                      "floatlightDensity="
                      "cloudDensityFromPositiveWeatherMacro("),
                  static_cast<std::size_t>(1));
        EXPECT_EQ(CountOccurrences(
                      completeLightSection,
                      "floatlightDensity="
                      "cloudShapeFromPositiveWeatherMacro("),
                  static_cast<std::size_t>(0));
        EXPECT_FALSE(Contains(completeLightSection, "[branch]if(l>=3)"));
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
        "float3p,floatweatherCoverage)");
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
        "coverageTerms.z,profileShape);"
        "densityHeightThreshold=cloudHeightThresholdFromTarget("
        "coverageTerms.w,profileShape);"));
    EXPECT_TRUE(Contains(
        sharedLightMacro,
        "cloudBaseShapeLighting("
        "lightingUvw,macro.heightThreshold,macro.baseNoise);"));

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
            "macro.baseNoise,macro.heightThreshold,"
            "min(macro.heightThreshold+0.22,0.98)"));
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
            "macro.baseNoise,heightThreshold,"
            "min(heightThreshold+0.22,0.98)"));
    }

    // Reuse must preserve the former threshold exactly for representative
    // authored coverage and every meaningful profile shape.
    const auto threshold = [](f32 coverage, f32 profileShape) noexcept {
        const f32 saturatedCoverage =
            coverage < 0.0f ? 0.0f : (coverage > 1.0f ? 1.0f : coverage);
        const f32 limitedCoverage =
            saturatedCoverage < 0.72f ? saturatedCoverage : 0.72f;
        const f32 cloudThreshold =
            0.72f + (0.50f - 0.72f) * limitedCoverage;
        return 0.78f +
               (cloudThreshold - 0.78f) * profileShape;
    };
    const auto target = [](f32 coverage) noexcept {
        const f32 saturatedCoverage =
            coverage < 0.0f ? 0.0f : (coverage > 1.0f ? 1.0f : coverage);
        const f32 limitedCoverage =
            saturatedCoverage < 0.72f ? saturatedCoverage : 0.72f;
        return 0.72f + (0.50f - 0.72f) * limitedCoverage;
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
                0.78f +
                (target(occupancyCoverage) - 0.78f) * profileShape;
            const f32 cachedDensity =
                0.78f +
                (target(coverage) - 0.78f) * profileShape;
            EXPECT_NEAR(cachedOccupancy, formerOccupancy, 0.0f);
            EXPECT_NEAR(cachedDensity, formerDensity, 0.0f);
        }
    }

    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 192u);
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
        "macro.weather.b,cloudColumnHeightShift(macro.weather));",
        viewLoop);
    const std::size_t nearLightLoop =
        shader.find("[loop]for(intl=0;l<3;l++)", sharedProfile);
    EXPECT_TRUE(coverageTerms != std::string::npos);
    EXPECT_TRUE(viewLoop != std::string::npos);
    EXPECT_TRUE(sharedProfile != std::string::npos);
    EXPECT_TRUE(nearLightLoop != std::string::npos);
    EXPECT_TRUE(coverageTerms < viewLoop);
    EXPECT_TRUE(viewLoop < sharedProfile);
    EXPECT_TRUE(sharedProfile < nearLightLoop);
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
                lightLoops, "sampleCloudLightingShapeFromSlowFields("),
            static_cast<std::size_t>(1));
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

    const std::size_t farShapeMacro = shader.find(
        "floatsampleCloudLightingShapeFromSlowFields(");
    const std::size_t farShapeMacroEnd =
        shader.find("returnshapeResult;}", farShapeMacro);
    EXPECT_TRUE(farShapeMacro != std::string::npos);
    EXPECT_TRUE(farShapeMacroEnd != std::string::npos);
    if (farShapeMacro != std::string::npos &&
        farShapeMacroEnd != std::string::npos) {
        const std::string helper =
            shader.substr(
                farShapeMacro, farShapeMacroEnd - farShapeMacro);
        EXPECT_TRUE(Contains(
            helper,
            "floatsampleHeight=cloudConvectiveHeight("
            "heightFractionFromAltitude(altitude,upperBand),"
            "slowProfileTerms.w,upperBand);"));
        EXPECT_TRUE(Contains(
            helper,
            "floatsampledProfile=cloudProfileFromTypeWeights("
            "sampleHeight,slowProfileTerms.xy,slowProfileTerms.z);"));
        EXPECT_TRUE(Contains(
            helper,
            "floatheightThreshold=cloudHeightThresholdFromTarget("
            "heightThresholdTarget,profileShape);"));
        EXPECT_TRUE(Contains(
            helper,
            "cloudBaseShapeLighting("
            "lightingUvw,heightThreshold,baseNoise);"));
        EXPECT_TRUE(Contains(
            helper,
            "shapeResult=remapc("
            "baseNoise,heightThreshold,"
            "min(heightThreshold+0.22,0.98),0.0,1.0)"
            "*slowWeatherMask*profileWeight;"));
        EXPECT_FALSE(Contains(helper, "cloudCurlOffset("));
        EXPECT_FALSE(Contains(helper, "detailNoise.SampleLevel("));
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
        const f32 storm =
            SmoothStepForTest(0.0f, 0.10f, h) *
            (1.0f - SmoothStepForTest(0.88f, 1.0f, h));
        return SaturateForTest(
            value + (storm - value) * precipitation * 0.72f);
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
    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 192u);
    EXPECT_EQ(kVolumetricCloudMaxLightMarchSamples, 8u);
    EXPECT_EQ(kVolumetricCloudUltraTraceDivisor, 4u);
}

ACS_TEST(VolumetricClouds,
         FarLightScalarSpecializationPreservesConsumerAndWorldDomain) {
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
    EXPECT_FALSE(Contains(
        shader,
        "if(macro.weatherMask*macro.profileWeight>0.006){"));

    const std::size_t helperBegin = shader.find(
        "floatsampleCloudLightingShapeFromSlowFields(");
    const std::size_t helperEnd =
        shader.find("returnshapeResult;}", helperBegin);
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
            "(sampleHeight-referenceHeight)*0.78,"
            "(p.z-referenceP.z)*shapeScale);"));
        EXPECT_FALSE(Contains(helper, "camPos"));
        EXPECT_FALSE(Contains(helper, "cloudWindWorld("));
        EXPECT_FALSE(Contains(helper, "cloudUVW("));
        EXPECT_FALSE(Contains(helper, "cloudCurlOffset("));
    }

    // With weather and curl shared from the view sample, cloudUVW is affine
    // in world XZ and normalized height. Reconstructing a probe from the
    // reference UVW therefore stays independent of camera position.
    const FVec2 wind{183.25f, -91.75f};
    const FVec2 fixedWarp{-37.0f, 52.5f};
    constexpr f32 kShapeScale = 0.00021f;
    constexpr f32 kWeatherType = 0.63f;
    constexpr f32 kWeatherAnvil = 0.28f;
    const auto legacyUvw =
        [&](FVec3 point, f32 height) noexcept {
            const f32 canonicalY =
                height * 0.78f +
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
                    (height - referenceHeight) * 0.78f,
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

    // The former struct consumer and the scalar helper apply the identical
    // saturated remap, weather mask and profile weight. Cover empty, edge,
    // dense and clamped-threshold representatives.
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

    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 192u);
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
    EXPECT_TRUE(Contains(
        compactSource,
        "sizeof(FCloudCb)==640"));
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

    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 192u);
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
    EXPECT_TRUE(Contains(shader, "float4cloudLightTangent;"));
    EXPECT_TRUE(Contains(shader, "float4cloudLightBitangent;"));
    EXPECT_TRUE(Contains(
        shader,
        "floatheightFractionFromAltitude("
        "floataltitude,boolupperBand){"
        "if(upperBand)"
        "returnsaturate((altitude-cloudUpperLayer.x)*"
        "cloudUpperLayer.z);"
        "returnsaturate((altitude-layer.x)*cloudFrameTerms.w);}"));
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
    EXPECT_EQ(
        CountOccurrences(shader, "float3sun=sunDir.xyz;"),
        static_cast<std::size_t>(2));
    EXPECT_FALSE(Contains(shader, "normalize(sunDir.xyz)"));
    EXPECT_FALSE(Contains(shader, "cloudLightBasis("));

    EXPECT_TRUE(Contains(
        compactSource,
        "sizeof(FCloudCb)==640"));
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
        "lp+=coneDir*lightStep;"
        "CloudMacroSamplelightMacro="));
    EXPECT_TRUE(Contains(
        compactSource,
        "cl.Dispatch((m_W+7u)/8u,(m_H+7u)/8u,1);"));
    EXPECT_TRUE(Contains(
        compactSource,
        "cl.Dispatch((m_FullW+7u)/8u,(m_FullH+7u)/8u,1);"));
    EXPECT_EQ(kVolumetricCloudMaxViewMarchSamples, 192u);
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
    EXPECT_EQ(
        CountOccurrences(shader, "detailNoise.SampleLevel("),
        static_cast<std::size_t>(3));
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
    EXPECT_TRUE(Contains(
        resolveShader,
        "booloccupancyMismatch=(curA<0.02&&hist.a>0.08)||"
        "(curA>0.08&&hist.a<0.02);"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "boolalphaOk=!occupancyMismatch&&"
        "abs(hist.a-seedDepth.y)<0.42;"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "floatcurrentWeight=worldOrigin.w>0.5?0.125:0.70;"
        "resolved=lerp(histPacked,current,currentWeight);"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "resolvedDepth=float2(curDepth,curA);"));
    EXPECT_TRUE(Contains(resolveShader, "resolved=histPacked;"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "resolvedDepth=float2(reprojectionDepth,histD.y);"));
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
        "!scheduled&&worldOrigin.w>0.5;"));
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
        "p,coverage);"));
    EXPECT_TRUE(Contains(
        shader,
        "macro.heightThreshold=cloudHeightThreshold("
        "weatherCoverage,profileShape);"));
    EXPECT_TRUE(Contains(
        shader,
        "cloudBaseShapeLighting("
        "cloudUVW(p,macro.weather,macro.curl,macro.height),"
        "macro.heightThreshold,macro.baseNoise);"));
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
    EXPECT_TRUE(Contains(shader, "float shapeResult=0.0;"));
    EXPECT_TRUE(Contains(shader, "return shapeResult;"));
    EXPECT_TRUE(Contains(shader, "float densityResult=0.0;"));
    EXPECT_TRUE(Contains(shader, "return densityResult;"));
    EXPECT_TRUE(!Contains(shader, "if(disc<0.0){ nearT="));
    EXPECT_TRUE(!Contains(shader, "if(weatherMask<=0.001) return 0.0;"));
    EXPECT_TRUE(!Contains(shader, "if(profile<=0.001) return 0.0;"));
}

ACS_TEST(VolumetricClouds,
         UltraRayJitterAdvancesOncePerCycleWhileNativeModesAvoidShortCycles) {
    const std::string source = ReadSkySource();
    const std::string shader =
        ExtractRawShader(source, "const char* kCloudCS");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!shader.empty());

    // Ultra refreshes an exact pixel once per complete phase cycle, so the
    // low-discrepancy sample advances at that same cadence. Native/scaled modes
    // still rotate every frame because all pixels are current.
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
        "if(groundCutoff>=-1.0){"));
    EXPECT_FALSE(Contains(
        shader, "floatcameraAltitude=cloudAltitude(camPos.xyz);"));
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
    EXPECT_TRUE(Contains(shader, "xWp/=xWp.w;yWp/=yWp.w;"));
    EXPECT_FALSE(Contains(shader, "xWp/=max(abs(xWp.w),1e-6);"));
    EXPECT_TRUE(Contains(
        shader,
        "groundHorizonCoverage=smoothstep("
        "groundCutoff-coverageHalfWidth,"
        "groundCutoff+coverageHalfWidth,signedElevation);"));
    EXPECT_FALSE(Contains(shader, "floatverticalOffset="));
    EXPECT_FALSE(Contains(shader, "groundCutoff+coverageWidth"));
    EXPECT_TRUE(Contains(shader, "floathFade=rangeFade;"));
    EXPECT_FALSE(Contains(
        shader, "floathFade=rangeFade*groundHorizonCoverage;"));
    EXPECT_FALSE(Contains(
        shader,
        "cameraAltitude<layer.x&&signedElevation<-0.002"));
    EXPECT_EQ(
        CountOccurrences(shader, "float4groundHorizon;"),
        static_cast<std::size_t>(1));

    const std::string resolveShader = CompactShader(
        ExtractRawShader(source, "const char* kCloudResolveCS"));
    const std::string compactSource = CompactShader(source);
    EXPECT_EQ(
        CountOccurrences(resolveShader, "float4groundHorizon;"),
        static_cast<std::size_t>(1));
    EXPECT_TRUE(Contains(
        compactSource,
        "offsetof(FCloudCb,groundHorizon)==320u"));
    EXPECT_TRUE(Contains(
        compactSource, "sizeof(FCloudCb)==640"));
    EXPECT_TRUE(Contains(
        compactSource,
        "offsetof(FCloudCb,cloudFrameTerms)==336u"));
    EXPECT_TRUE(Contains(
        compactSource, "CBSize<FCloudCb>()==768u"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "floatcurrentViewElevation=0.0;"
        "boolcurrentViewElevationReady=false;"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "currentViewElevation=dot("
        "stableRay,groundHorizon.xyz);"
        "currentViewElevationReady=true;"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "currentViewElevation=dot(ray,groundHorizon.xyz);"
        "currentViewElevationReady=true;"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "if(possibleGroundEdge&&groundHorizon.w>=-1.0){"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "floatoutputElevation=currentViewElevation;"
        "if(!currentViewElevationReady){"));
    EXPECT_FALSE(Contains(
        resolveShader, "floatoutputCameraAltitude="));
    EXPECT_FALSE(Contains(
        resolveShader,
        "floatradiusRatio=CLOUD_PLANET_RADIUS/"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "if(outputElevation<outputGroundCutoff-0.02){"
        "outputGroundCoverage=0.0;}"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "float2outputCenter=float2(tid.xy)+0.5;"
        "floatoutputXOffset=tid.x+1u<fullW?1.0:-1.0;"
        "floatoutputYOffset=tid.y+1u<fullH?1.0:-1.0;"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "floatoutputCoverageHalfWidth=max("
        "0.5*(abs(outputXElevation-outputElevation)+"
        "abs(outputYElevation-outputElevation)),1e-6);"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "outputXFarP/=outputXFarP.w;"
        "outputYFarP/=outputYFarP.w;"));
    EXPECT_FALSE(Contains(
        resolveShader,
        "outputXFarP/=max(abs(outputXFarP.w),1e-6);"));
    // Reusing the already unprojected centre ray must never remove the two
    // independent neighbouring rays that define the analytic pixel footprint.
    EXPECT_EQ(
        CountOccurrences(
            resolveShader, "outputXFarP=mul(outputXClip,invViewProj);"),
        static_cast<std::size_t>(1));
    EXPECT_EQ(
        CountOccurrences(
            resolveShader, "outputYFarP=mul(outputYClip,invViewProj);"),
        static_cast<std::size_t>(1));
    EXPECT_TRUE(Contains(
        resolveShader,
        "outputGroundCoverage=smoothstep("
        "outputGroundCutoff-outputCoverageHalfWidth,"
        "outputGroundCutoff+outputCoverageHalfWidth,"
        "outputElevation);"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "resolved.rgb*=outputGroundCoverage;"
        "outA*=outputGroundCoverage;resolvedDepth.y=outA;"));
    // The former edge repair borrowed six history pixels with unrelated
    // radiance/depth. It could hide a staircase in one frame but introduced
    // dots and partial-history contamination on the next.
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

ACS_TEST(VolumetricClouds,
         StableHistoryStillRunsExactGroundCutoffForSmallCameraMotion) {
    const std::string source = ReadSkySource();
    const std::string compactSource = CompactShader(source);
    const std::string resolveShader = CompactShader(
        ExtractRawShader(source, "const char* kCloudResolveCS"));
    EXPECT_TRUE(!resolveShader.empty());

    // Small camera/matrix deltas deliberately retain history. Therefore the
    // stable unscheduled shortcut cannot publish a reprojected pixel before the
    // current frame's exact full-resolution planet/ground cutoff is evaluated.
    EXPECT_TRUE(Contains(
        compactSource,
        "constbooltemporalHistoryStationary=historyValid&&"
        "cameraDeltaSquared<=0.0025f&&matrixDelta<=0.002f;"));
    EXPECT_TRUE(Contains(
        resolveShader,
        "boolstableUnscheduled=temporal.x>0.5&&temporalSuperRes&&"
        "!scheduled&&worldOrigin.w>0.5;"));

    const std::size_t stableAccept = resolveShader.find(
        "if(stableDepthOk&&stableAlphaOk){");
    const std::size_t fallbackGate = resolveShader.find(
        "if(!stableHistoryResolved){", stableAccept);
    const std::size_t exactGroundCutoff = resolveShader.find(
        "if(possibleGroundEdge&&groundHorizon.w>=-1.0){",
        fallbackGate);
    const std::size_t finalColorWrite = resolveShader.find(
        "historyColorOut[tid.xy]=float4(", exactGroundCutoff);
    EXPECT_TRUE(stableAccept != std::string::npos);
    EXPECT_TRUE(fallbackGate != std::string::npos);
    EXPECT_TRUE(exactGroundCutoff != std::string::npos);
    EXPECT_TRUE(finalColorWrite != std::string::npos);
    EXPECT_TRUE(stableAccept < fallbackGate);
    EXPECT_TRUE(fallbackGate < exactGroundCutoff);
    EXPECT_TRUE(exactGroundCutoff < finalColorWrite);

    if (stableAccept != std::string::npos &&
        fallbackGate != std::string::npos &&
        stableAccept < fallbackGate) {
        const std::string stableAcceptPath = resolveShader.substr(
            stableAccept, fallbackGate - stableAccept);
        EXPECT_TRUE(Contains(
            stableAcceptPath,
            "resolved=float4(stableHist.rgb*stableHist.a,stableHist.a);"));
        EXPECT_TRUE(Contains(
            stableAcceptPath,
            "resolvedDepth=float2(sameScreenDepth.x,stableHist.a);"));
        EXPECT_TRUE(Contains(
            stableAcceptPath, "stableHistoryResolved=true;"));
        EXPECT_FALSE(Contains(
            stableAcceptPath, "historyColorOut[tid.xy]=stableHist;"));
        EXPECT_FALSE(Contains(stableAcceptPath, "return;"));
    }
}

ACS_TEST(VolumetricClouds,
         RawDx12ShaderCompilationAcceptsHoistedCloudCbLayout) {
#if !WITH_RENDER_DILIGENT
    auto compiled = CVolumetricClouds::CompileShadersCpu();
    EXPECT_TRUE(compiled.IsOk());
    if (compiled.IsOk()) {
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
        if (phase < minimumPhase) minimumPhase = phase;
        if (phase > maximumPhase) maximumPhase = phase;
    }

    const f32 backward = DefaultCloudPhaseForTest(-1.0f);
    const f32 side = DefaultCloudPhaseForTest(0.0f);
    const f32 forward = DefaultCloudPhaseForTest(1.0f);
    EXPECT_TRUE(backward >= 0.30f && backward <= 0.50f);
    EXPECT_TRUE(side >= 0.40f && side <= 0.60f);
    EXPECT_NEAR(forward, lighting.PhaseMax, 1e-4f);
    EXPECT_TRUE(forward > side * 10.0f);
    EXPECT_TRUE(maximumPhase <= lighting.PhaseMax);
    EXPECT_TRUE(minimumPhase >= lighting.PhaseMin);

    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());
    EXPECT_TRUE(Contains(
        shader,
        "floatphaseBlend=cloudLightingPhase.z;"
        "floatphase=12.566370*("
        "hg(cosA,cloudLightingPhase.x)*phaseBlend+"
        "hg(cosA,cloudLightingPhase.y)*(1.0-phaseBlend));"));
    EXPECT_TRUE(Contains(
        shader,
        "phase=clamp(phase,cloudLightingMulti.y,cloudLightingMulti.z);"));
    EXPECT_TRUE(Contains(shader, "phaseMulti=clamp(" "phaseMulti,cloudLightingMulti.y,cloudLightingMulti.z);"));
    EXPECT_TRUE(Contains(shader, "floatinScatterProbability=inScatterDepth*inScatterVertical;" "floatinScatterFactor=lerp(" "1.0,inScatterProbability,cloudLightingExtinction.w);" "floatsingleScatter=beer*phase*inScatterFactor;"));
    EXPECT_TRUE(Contains(shader, "floatlowLodDensity=cloudLowLodDensityFromMacro(" "p,macro,densityHeightThreshold,viewWeatherMask);"));
    EXPECT_FALSE(Contains(shader, "pow(saturate(shape),inScatterDepthExponent)"));
    EXPECT_TRUE(Contains(shader, "floatmultipleScatter=" "multiContribution*multi*phaseMulti;" "floatscatterTerm=singleScatter+multipleScatter;"));
    EXPECT_FALSE(Contains(shader, "beer*(1.0-multiWeight)*phase"));
    EXPECT_FALSE(Contains(shader, "nearLightDensity"));
    EXPECT_FALSE(Contains(shader, "edgeBoost"));
    EXPECT_TRUE(Contains(
        shader,
        "float3sunAtCloud=sunCol.rgb*cloudSunTransmittance.rgb;"
        "float3sunL=sunAtCloud*cloudLightingExtinction.z*scatterTerm"));
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
    EXPECT_EQ(kVolumetricCloudShadowCacheWidth *
                  kVolumetricCloudShadowCacheHeight *
                  kVolumetricCloudShadowCacheDepth * 4u,
              1179648u);
    EXPECT_EQ(kVolumetricCloudShadowCacheWidth *
                  kVolumetricCloudShadowCacheHeight *
                  kVolumetricCloudShadowCacheDepth * 4u * 2u,
              2359296u); // raw + finalized RG16F volumes

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

ACS_TEST(VolumetricClouds,
         ShadowCacheKeepsNearLightingExactAndGuardsEveryFarLookup) {
    const std::string source = ReadSkySource();
    const std::string shader = CompactShader(
        ExtractRawShader(source, "const char* kCloudCS"));
    EXPECT_TRUE(!shader.empty());

    EXPECT_TRUE(Contains(
        shader,
        "RWTexture3D<float2>cloudShadowOut:register(u2);"));
    EXPECT_TRUE(Contains(
        shader,
        "Texture3D<float2>cloudShadowCache:register(t4);"));
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
        "[numthreads(4,4,4)]voidCSCloudShadow("));
    EXPECT_TRUE(Contains(
        shader,
        "cloudShadowOut.GetDimensions(width,height,depth);"
        "if(any(tid>=uint3(width,height,depth)))return;"));
    EXPECT_TRUE(Contains(shader, "[loop]for(intl=3;l<8;l++){"));
    EXPECT_TRUE(Contains(
        shader,
        "lightStep*=1.65;lightStep*=1.65;lightStep*=1.65;"));
    EXPECT_FALSE(Contains(shader, "lightStep*=4.492125"));
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
        static_cast<std::size_t>(1));
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
    EXPECT_TRUE(Contains(
        shader,
        "[numthreads(4,4,4)]voidCSCloudShadowFinalize("));
    EXPECT_TRUE(Contains(
        shader,
        "float2rawCenter=cloudShadowCache.Load(int4(center,0));"));
    EXPECT_TRUE(Contains(
        shader,
        "float3positiveGradient=abs(positiveTau-rawCenter.xxx);"));
    EXPECT_TRUE(Contains(
        shader,
        "float3negativeGradient=abs(negativeTau-rawCenter.xxx);"));
    EXPECT_TRUE(Contains(
        shader,
        "cloudShadowOut[tid]=float2("
        "rawCenter.x,max(rawCenter.y,spatialDisagreement));"));
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
    const std::size_t pairedTrig = shader.find(
        "floatconeSin,coneCos;"
        "sincos(6.2831853*jit,coneSin,coneCos);",
        viewLoop);
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
    EXPECT_TRUE(pairedTrig != std::string::npos);
    EXPECT_TRUE(coneDirection != std::string::npos);
    EXPECT_TRUE(pairedTrig < nearLightLoop);
    EXPECT_TRUE(nearLightLoop < cacheAttempt);
    EXPECT_TRUE(cacheAttempt < farLightLoop);
    EXPECT_TRUE(farLightLoop < coneDirection);
    EXPECT_FALSE(Contains(shader, "[branch]if(l>=3)"));
    EXPECT_TRUE(Contains(
        shader,
        "staticconstboolCLOUD_MAIN_SHADOW_CACHE_ENABLED=false;"));
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
    // A rejected lookup simply falls through to the unchanged five-probe
    // macro tail; the three detailed near probes remain a separate exact loop.
    EXPECT_TRUE(Contains(
        shader,
        "floatlightDensity="
        "sampleCloudLightingShapeFromSlowFields("
        "lp,viewWeatherMask,coverageTerms.w,"
        "sharedLightProfileTerms,"
        "p,viewMacroUvw,macro.height,sharedShapeScale);"));
    EXPECT_TRUE(Contains(
        shader,
        "floatlightDensity=cloudDensityFromPositiveWeatherMacro("
        "lp,lightMacro,lightMacro.heightThreshold,"
        "lightMacro.weatherMask,0.65);"));
    // Weather/curl are slow fields. The view value must feed the light probes,
    // while macro shape remains exact at every probe through the helper.
    EXPECT_TRUE(Contains(
        shader,
        "float4sharedLightProfileTerms=float4("
        "cloudProfileTypeWeights(macro.weather.g),"
        "macro.weather.b,cloudColumnHeightShift(macro.weather));"
        "float2sharedLightCurl=macro.curl;"));
    EXPECT_FALSE(Contains(
        shader, "sharedLightProfileTerms=cloudWeatherData(lp)"));
    EXPECT_FALSE(Contains(shader, "sharedLightCurl=cloudCurlOffset(lp)"));
    EXPECT_TRUE(Contains(
        shader,
        "sampleCloudMacroLightingFromSlowFields("
        "lp,viewWeatherMask,coverageTerms.w,"
        "sharedLightProfileTerms,sharedLightCurl,"
        "p,viewMacroUvw,macro.height,sharedShapeScale);"));
    EXPECT_TRUE(Contains(
        shader,
        "sampleCloudLightingShapeFromSlowFields("
        "lp,viewWeatherMask,coverageTerms.w,"
        "sharedLightProfileTerms,"
        "p,viewMacroUvw,macro.height,sharedShapeScale);"));
}

ACS_TEST(VolumetricClouds,
         ShadowCacheRhiBindingIsOptionalOrderedAndWorldSpaceDirty) {
    const std::string source = ReadSkySource();
    const std::string compact = CompactShader(source);
    EXPECT_TRUE(!compact.empty());
    EXPECT_FALSE(kVolumetricCloudShadowCacheEnabled);
    // Optional shader compilation is staged on both supported initialization
    // paths. A missing optional pair disables only the cache; the mandatory
    // cloud candidate and exact-lighting path remain publishable.
    EXPECT_EQ(
        CountOccurrences(
            compact,
            "autoshadow_result=compile(EShaderStage::Compute,kCloudCS,"
            "\"CSCloudShadow\",\"Clouds.ShadowCacheCS\");"),
        static_cast<std::size_t>(2));
    EXPECT_EQ(
        CountOccurrences(
            compact,
            "autofinalize_result=compile(EShaderStage::Compute,kCloudCS,"
            "\"CSCloudShadowFinalize\","
            "\"Clouds.ShadowCacheFinalizeCS\");"),
        static_cast<std::size_t>(2));
    EXPECT_EQ(
        CountOccurrences(compact, "shaders.shadow.Reset();"),
        static_cast<std::size_t>(2));
    EXPECT_TRUE(Contains(
        compact,
        "boolshadowOk=kVolumetricCloudShadowCacheEnabled&&"
        "shaders.shadow&&shaders.shadow_finalize;"
        "if(shadowOk)m_ShadowCs=Move(shaders.shadow);"));
    EXPECT_TRUE(Contains(
        compact,
        "if(shadowOk)m_ShadowFinalizeCs="
        "Move(shaders.shadow_finalize);"));
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
        "td.format=EFormat::R16G16_Float;td.is_uav=true;"));
    EXPECT_EQ(
        CountOccurrences(
            compact,
            "td.width=kVolumetricCloudShadowCacheWidth;"
            "td.height=kVolumetricCloudShadowCacheHeight;"
            "td.depth=kVolumetricCloudShadowCacheDepth;"
            "td.format=EFormat::R16G16_Float;td.is_uav=true;"),
        static_cast<std::size_t>(2));
    EXPECT_TRUE(Contains(
        compact,
        "cl.Dispatch((kVolumetricCloudShadowCacheWidth+3u)/4u,"
        "(kVolumetricCloudShadowCacheHeight+3u)/4u,"
        "(kVolumetricCloudShadowCacheDepth+3u)/4u);"));
    EXPECT_TRUE(Contains(
        compact,
        "shadowCacheUsableThisFrame?1.0f:0.0f"));
    EXPECT_TRUE(Contains(
        compact,
        "if(!shadowOk){m_ShadowTex.Reset();m_ShadowRawTex.Reset();"
        "m_ShadowFinalizePipe.Reset();m_ShadowFinalizeCs.Reset();"
        "m_ShadowPipe.Reset();m_ShadowCs.Reset();"));
    const std::size_t optionalBegin = compact.find(
        "boolshadowOk=kVolumetricCloudShadowCacheEnabled&&"
        "shaders.shadow&&shaders.shadow_finalize;");
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
    const std::size_t rawU2 =
        compact.find("cl.BindUav(2,*m_ShadowRawTex);", dummyU1);
    const std::size_t finalizePipeline = compact.find(
        "cl.SetComputePipeline(*m_ShadowFinalizePipe);", rawU2);
    const std::size_t finalizeDummyT0 =
        compact.find("cl.SetTexture(0,*m_ShapeTex);", finalizePipeline);
    const std::size_t finalizeDummyT1 =
        compact.find("cl.SetTexture(1,*m_WeatherTex);", finalizeDummyT0);
    const std::size_t finalizeDummyT2 =
        compact.find("cl.SetTexture(2,*m_DetailTex);", finalizeDummyT1);
    const std::size_t finalizeDummyT3 =
        compact.find("cl.SetTexture(3,*m_CurlTex);", finalizeDummyT2);
    const std::size_t finalizeRawT4 =
        compact.find("cl.SetTexture(4,*m_ShadowRawTex);", finalizeDummyT3);
    const std::size_t finalizeDummyU0 =
        compact.find("cl.BindUav(0,*m_CloudTex);", finalizeRawT4);
    const std::size_t finalizeDummyU1 =
        compact.find("cl.BindUav(1,*m_CloudDepth);", finalizeDummyU0);
    const std::size_t finalU2 =
        compact.find("cl.BindUav(2,*m_ShadowTex);", finalizeDummyU1);
    const std::size_t mainPipeline =
        compact.find("cl.SetComputePipeline(*m_CloudPipe);", finalU2);
    const std::size_t cacheT4 =
        compact.find("cl.SetTexture(4,*m_ShadowTex);", mainPipeline);
    const std::size_t dummyT4 =
        compact.find("cl.SetTexture(4,*m_ShapeTex);", cacheT4);
    EXPECT_TRUE(noiseBake != std::string::npos);
    EXPECT_TRUE(shadowBuild != std::string::npos);
    EXPECT_TRUE(dummyU0 != std::string::npos);
    EXPECT_TRUE(dummyU1 != std::string::npos);
    EXPECT_TRUE(rawU2 != std::string::npos);
    EXPECT_TRUE(finalizePipeline != std::string::npos);
    EXPECT_TRUE(finalizeDummyT0 != std::string::npos);
    EXPECT_TRUE(finalizeDummyT1 != std::string::npos);
    EXPECT_TRUE(finalizeDummyT2 != std::string::npos);
    EXPECT_TRUE(finalizeDummyT3 != std::string::npos);
    EXPECT_TRUE(finalizeRawT4 != std::string::npos);
    EXPECT_TRUE(finalizeDummyU0 != std::string::npos);
    EXPECT_TRUE(finalizeDummyU1 != std::string::npos);
    EXPECT_TRUE(finalU2 != std::string::npos);
    EXPECT_TRUE(mainPipeline != std::string::npos);
    EXPECT_TRUE(cacheT4 != std::string::npos);
    EXPECT_TRUE(dummyT4 != std::string::npos);
    EXPECT_TRUE(noiseBake < shadowBuild);
    EXPECT_TRUE(shadowBuild < dummyU0);
    EXPECT_TRUE(dummyU0 < dummyU1);
    EXPECT_TRUE(dummyU1 < rawU2);
    EXPECT_TRUE(rawU2 < finalizePipeline);
    EXPECT_TRUE(finalizePipeline < finalizeDummyT0);
    EXPECT_TRUE(finalizeDummyT0 < finalizeDummyT1);
    EXPECT_TRUE(finalizeDummyT1 < finalizeDummyT2);
    EXPECT_TRUE(finalizeDummyT2 < finalizeDummyT3);
    EXPECT_TRUE(finalizeDummyT3 < finalizeRawT4);
    EXPECT_TRUE(finalizeRawT4 < finalizeDummyU0);
    EXPECT_TRUE(finalizeDummyU0 < finalizeDummyU1);
    EXPECT_TRUE(finalizeDummyU1 < finalU2);
    EXPECT_TRUE(finalU2 < mainPipeline);
    EXPECT_TRUE(mainPipeline < cacheT4);
    EXPECT_TRUE(cacheT4 < dummyT4);

    const std::size_t dirtyBegin = compact.find(
        "if(shadowResourcesReady&&m_ShadowCacheValid){");
    const std::size_t dirtyEnd = compact.find(
        "constboolshadowWillBuildThisFrame=", dirtyBegin);
    EXPECT_TRUE(dirtyBegin != std::string::npos);
    EXPECT_TRUE(dirtyEnd != std::string::npos);
    if (dirtyBegin != std::string::npos &&
        dirtyEnd != std::string::npos) {
        const std::string dirty =
            compact.substr(dirtyBegin, dirtyEnd - dirtyBegin);
        EXPECT_TRUE(Contains(dirty, "coverageDelta>0.001f"));
        EXPECT_TRUE(Contains(
            dirty, "sunDot<kShadowSunDirectionCosThreshold"));
        EXPECT_TRUE(Contains(
            dirty, "lightBasisHemisphereChanged"));
        EXPECT_TRUE(Contains(dirty, "layerChanged"));
        EXPECT_TRUE(Contains(
            dirty, "curvatureError>verticalQuarterCell"));
        EXPECT_TRUE(Contains(
            dirty,
            "constf32halfDiagonal="
            "kVolumetricCloudShadowCacheExtent*0.5f*kSqrtTwo;"));
        EXPECT_TRUE(Contains(dirty, "centerAnchorOffset"));
        EXPECT_TRUE(Contains(dirty, "maximumShellRadius"));
        EXPECT_FALSE(Contains(dirty, "safeDensity"));
        EXPECT_FALSE(Contains(dirty, "sun_color"));
        EXPECT_FALSE(Contains(dirty, "sky_color"));
        EXPECT_FALSE(Contains(dirty, "matrixDelta"));
    }
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
        EXPECT_EQ(
            CountOccurrences(
                compact.substr(shadowBuild, logicalBuildEnd - shadowBuild),
                "cl.Dispatch("),
            static_cast<std::size_t>(2));
    }
    EXPECT_TRUE(Contains(
        compact,
        "m_ShadowCacheAvailable&&m_ShadowCs&&m_ShadowPipe&&"
        "m_ShadowFinalizeCs&&m_ShadowFinalizePipe&&"
        "m_ShadowRawTex&&m_ShadowTex;"));
    EXPECT_FALSE(Contains(
        compact, "false&&m_ShadowCacheAvailable"));
    EXPECT_TRUE(Contains(
        compact,
        "constboolshadowBakePrerequisites="
        "(m_NoiseBaked||(m_NoisePipe&&m_ShapeTex))&&"));
    EXPECT_TRUE(Contains(
        compact,
        "constboolshadowCacheUsableThisFrame="
        "(m_ShadowCacheValid&&!shadowDirty)||shadowWillBuildThisFrame;"));
}

ACS_TEST(VolumetricClouds,
         ViewCutDetectionRetainsHistoryAcrossOrdinaryEditorTranslation) {
    CCamera previousCamera;
    previousCamera.SetPerspective(60.0f * kDeg2Rad, 16.0f / 9.0f,
                                  0.05f, 250000.0f);
    const FVec3 previousEye{12.0f, 24.0f, -36.0f};
    previousCamera.SetLookAt(
        previousEye, FVec3{12.0f, 24.0f, -35.0f});

    CCamera translatedCamera;
    translatedCamera.SetPerspective(60.0f * kDeg2Rad, 16.0f / 9.0f,
                                    0.05f, 250000.0f);
    const FVec3 translatedEye{92.0f, 40.0f, 4.0f};
    translatedCamera.SetLookAt(
        translatedEye, FVec3{92.0f, 40.0f, 5.0f});

    EXPECT_FALSE(VolumetricCloudViewCutDetected(
        Inverse(previousCamera.ViewProjection()), previousEye,
        Inverse(translatedCamera.ViewProjection()), translatedEye));
}

ACS_TEST(VolumetricClouds,
         ViewCutDetectionRejectsTeleportsAndAbruptOrientationChanges) {
    CCamera previousCamera;
    previousCamera.SetPerspective(60.0f * kDeg2Rad, 16.0f / 9.0f,
                                  0.05f, 250000.0f);
    const FVec3 previousEye{0.0f, 8.0f, 0.0f};
    previousCamera.SetLookAt(
        previousEye, FVec3{0.0f, 8.0f, 1.0f});
    const FMat4 previousInverse =
        Inverse(previousCamera.ViewProjection());

    CCamera smallRotation;
    smallRotation.SetPerspective(60.0f * kDeg2Rad, 16.0f / 9.0f,
                                 0.05f, 250000.0f);
    smallRotation.SetLookAt(
        previousEye,
        FVec3{Sin(5.0f * kDeg2Rad), 8.0f,
              Cos(5.0f * kDeg2Rad)});
    EXPECT_FALSE(VolumetricCloudViewCutDetected(
        previousInverse, previousEye,
        Inverse(smallRotation.ViewProjection()), previousEye));

    CCamera abruptRotation;
    abruptRotation.SetPerspective(60.0f * kDeg2Rad, 16.0f / 9.0f,
                                  0.05f, 250000.0f);
    abruptRotation.SetLookAt(
        previousEye,
        FVec3{Sin(35.0f * kDeg2Rad), 8.0f,
              Cos(35.0f * kDeg2Rad)});
    EXPECT_TRUE(VolumetricCloudViewCutDetected(
        previousInverse, previousEye,
        Inverse(abruptRotation.ViewProjection()), previousEye));

    const FVec3 teleportedEye{400.0f, 8.0f, 0.0f};
    CCamera teleportedCamera;
    teleportedCamera.SetPerspective(60.0f * kDeg2Rad, 16.0f / 9.0f,
                                    0.05f, 250000.0f);
    teleportedCamera.SetLookAt(
        teleportedEye, FVec3{400.0f, 8.0f, 1.0f});
    EXPECT_TRUE(VolumetricCloudViewCutDetected(
        previousInverse, previousEye,
        Inverse(teleportedCamera.ViewProjection()), teleportedEye));
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

    EXPECT_TRUE(Contains(
        compact,
        "if(!IsFiniteCloudMatrix(inv_view_proj)||"
        "!IsFiniteCloudVector(cam_pos)){"));
    EXPECT_TRUE(Contains(
        compact,
        "constFMat4viewProj=Inverse(inv_view_proj);"
        "if(!IsFiniteCloudMatrix(viewProj)){"));
    EXPECT_TRUE(Contains(
        compact,
        "constf32finiteTime=std::isfinite(time)?time:fallbackTime;"));
    EXPECT_TRUE(Contains(
        compact,
        "constf32safeTime=finiteTime<-10000000.0f?-10000000.0f:"));
    EXPECT_TRUE(Contains(
        compact,
        "VolumetricCloudViewCutDetected("
        "m_PrevInvViewProj,m_PrevCamPos,inv_view_proj,cam_pos)"));
    EXPECT_FALSE(Contains(compact, "matrixDelta>0.35f"));
    EXPECT_TRUE(Contains(
        compact, "m_PrevInvViewProj=inv_view_proj;"));
    EXPECT_TRUE(Contains(
        compact, "m_PrevSunColor=safeSunColor;"));
    EXPECT_TRUE(Contains(
        compact, "m_PrevSkyColor=safeSkyColor;"));
}
