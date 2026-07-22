// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "render/Atmosphere.h"
#include "render/IRhiDevice.h"
#include "render/Sky.h"
#include "math/Math.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

using namespace acs;

namespace {

std::string ReadAtmosphereSource() {
    const std::filesystem::path testFile{__FILE__};
    const std::filesystem::path sourcePath =
        testFile.parent_path().parent_path() /
        "src" / "render" / "Atmosphere.cpp";
    std::ifstream stream(sourcePath, std::ios::binary);
    if (!stream) {
        stream.open(
            std::filesystem::path{"acs"} / "src" /
            "render" / "Atmosphere.cpp",
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
            std::filesystem::path{"acs"} / "src" /
            "editor_abi" / "EditorAbi.cpp",
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

} // namespace

ACS_TEST(Atmosphere, CompositeSeparatesLongRangeAtmosphereFromLocalFog) {
    const std::string source = ReadAtmosphereSource();
    const std::string shader =
        ExtractRawShader(source, "const char* kApCompositePS");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!shader.empty());

    // Both physical RGB passes share one slice resolver and must discard clear
    // depth: the physical sky already contains camera-to-space scattering.
    EXPECT_TRUE(Contains(shader, "float PhysicalSlice(VSOut v)"));
    EXPECT_TRUE(Contains(shader, "if (depth >= 1.0) discard;"));
    EXPECT_TRUE(Contains(shader, "float slice = PhysicalSlice(v);"));

    // The scalar PS remains dedicated to local fog. Its physical-mode guard is
    // retained so it cannot accidentally double-apply atmosphere to clear sky.
    EXPECT_TRUE(Contains(shader, "if (compositeParams.x <= 0.5) discard;"));
    EXPECT_FALSE(Contains(shader, "float dist = maxDist;"));

    // Local-fog mode accepts cleared depth. Resolved cloud pixels terminate at
    // cloud distance; only a clear sky samples the short-range far slice.
    EXPECT_TRUE(Contains(shader, "if (depth >= 1.0)"));
    EXPECT_FALSE(Contains(shader, "depth < 0.999999"));
    EXPECT_FALSE(Contains(shader, "depth >= 0.999999"));
    EXPECT_TRUE(Contains(shader, "dist = maxDist;"));
    EXPECT_TRUE(Contains(
        shader,
        "float resolvedCloudDepth =\n"
        "          cloudDepth.SampleLevel"));
    EXPECT_TRUE(Contains(
        shader, "dist = min(dist, resolvedCloudDepth);"));

    // Geometry reconstructs world distance for both volumes.
    EXPECT_TRUE(Contains(
        shader, "dist = length(worldPos - camPosMaxDist.xyz);"));
    EXPECT_TRUE(Contains(
        shader, "apVolume.SampleLevel(apVolume_sampler"));
}

ACS_TEST(Atmosphere,
         AerialPerspectiveWritesRgbTransmittanceAndSeparatesLocalFogCs) {
    const std::string source = ReadAtmosphereSource();
    const std::string shader =
        ExtractRawShader(source, "const char* kApCS");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!shader.empty());

    // Physical AP retains wavelength-dependent Beer-Lambert transfer instead
    // of reducing it to the legacy mean-opacity alpha channel.
    EXPECT_TRUE(Contains(
        shader,
        "RWTexture3D<float4> apOut : register(u0);"));
    EXPECT_TRUE(Contains(
        shader,
        "RWTexture3D<float4> apTransOut : register(u1);"));
    EXPECT_TRUE(Contains(
        shader,
        "void IntegrateAp(uint3 id,uint W,uint H,uint D,"));
    EXPECT_TRUE(Contains(
        shader,
        "float3 L,Tview; IntegrateAp(id,W,H,D,L,Tview);"));
    EXPECT_TRUE(Contains(
        shader,
        "apOut[id]=float4(L,saturate(1.0-meanT));"));
    EXPECT_TRUE(Contains(
        shader,
        "apTransOut[id]=float4(saturate(Tview),1.0);"));

    const std::size_t physicalEntry = shader.find(
        "void CSAp(uint3 id : SV_DispatchThreadID)");
    const std::size_t localFogEntry = shader.find(
        "void CSLocalFog(uint3 id : SV_DispatchThreadID)");
    EXPECT_TRUE(physicalEntry != std::string::npos);
    EXPECT_TRUE(localFogEntry != std::string::npos);
    EXPECT_TRUE(physicalEntry < localFogEntry);
    if (physicalEntry == std::string::npos ||
        localFogEntry == std::string::npos ||
        physicalEntry >= localFogEntry) {
        return;
    }

    const std::string physicalBody =
        shader.substr(physicalEntry, localFogEntry - physicalEntry);
    const std::string localFogBody = shader.substr(localFogEntry);
    EXPECT_TRUE(Contains(physicalBody, "apTransOut[id]"));
    EXPECT_FALSE(Contains(localFogBody, "apTransOut[id]"));
    EXPECT_TRUE(Contains(
        localFogBody,
        "apOut[id]=float4(L,saturate(1.0-meanT));"));

    // Separate entry points and PSOs keep local fog on one UAV while the
    // physical dispatch writes the paired L/T volumes atomically.
    EXPECT_TRUE(Contains(
        source,
        "sd.entry_point = \"CSLocalFog\";"));
    EXPECT_TRUE(Contains(
        source,
        "pd.uav_slots = 2; pd.uav_names[0] = \"apOut\"; "
        "pd.uav_names[1] = \"apTransOut\";"));
    EXPECT_TRUE(Contains(
        source,
        "pd.uav_slots = 1; pd.uav_names[0] = \"apOut\";"));
    EXPECT_TRUE(Contains(
        source,
        "cl.BindUav(1, *m_ApTransVol);"));
    EXPECT_TRUE(Contains(
        source,
        "cl.SetComputePipeline(*m_LocalFogPipe);"));
}

ACS_TEST(Atmosphere,
         AerialPerspectiveCompositesRgbTransferBeforeInScatter) {
    const std::string source = ReadAtmosphereSource();
    const std::string shader =
        ExtractRawShader(source, "const char* kApCompositePS");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!shader.empty());

    EXPECT_TRUE(Contains(shader, "float4 PSMultiply(VSOut v)"));
    EXPECT_TRUE(Contains(shader, "float4 PSAddScatter(VSOut v)"));
    EXPECT_TRUE(Contains(
        shader, "transfer.a >= 0.5 && transfer.a <= 1.01"));
    EXPECT_TRUE(Contains(
        shader, ": float4(1.0, 1.0, 1.0, 1.0);"));
    EXPECT_TRUE(Contains(
        shader, "valid ? max(inScatter, 0.0) : float3(0,0,0)"));
    EXPECT_TRUE(Contains(
        source, "pd.blend_mode = EBlendMode::Multiply;"));
    EXPECT_TRUE(Contains(
        source,
        "pd.blend_mode = EBlendMode::AdditivePreserveAlpha;"));

    const std::size_t compositeBegin = source.find(
        "void FSkyAtmosphere::CompositeAerialPerspective");
    const std::size_t compositeEnd = source.find(
        "void FSkyAtmosphere::CompositeLocalFog", compositeBegin);
    EXPECT_TRUE(compositeBegin != std::string::npos);
    EXPECT_TRUE(compositeEnd != std::string::npos);
    if (compositeBegin == std::string::npos ||
        compositeEnd == std::string::npos ||
        compositeBegin >= compositeEnd) {
        return;
    }

    const std::string composite =
        source.substr(compositeBegin, compositeEnd - compositeBegin);
    const std::size_t multiply = composite.find(
        "cl.SetPipeline(*m_ApMultiplyPipe);");
    const std::size_t bindTransmittance = composite.find(
        "cl.SetTexture(1, transmittance_volume);", multiply);
    const std::size_t multiplyDraw = composite.find(
        "cl.Draw(3, 0);", bindTransmittance);
    const std::size_t additive = composite.find(
        "cl.SetPipeline(*m_ApAddPipe);", multiplyDraw);
    const std::size_t bindScatter = composite.find(
        "cl.SetTexture(1, ap_volume);", additive);
    const std::size_t additiveDraw = composite.find(
        "cl.Draw(3, 0);", bindScatter);
    EXPECT_TRUE(multiply != std::string::npos);
    EXPECT_TRUE(bindTransmittance != std::string::npos);
    EXPECT_TRUE(multiplyDraw != std::string::npos);
    EXPECT_TRUE(additive != std::string::npos);
    EXPECT_TRUE(bindScatter != std::string::npos);
    EXPECT_TRUE(additiveDraw != std::string::npos);
    EXPECT_TRUE(multiply < bindTransmittance);
    EXPECT_TRUE(bindTransmittance < multiplyDraw);
    EXPECT_TRUE(multiplyDraw < additive);
    EXPECT_TRUE(additive < bindScatter);
    EXPECT_TRUE(bindScatter < additiveDraw);

    // Preserve the channel-wise transfer contract numerically. A scalar mean T
    // would not produce these three distinct results.
    const FVec3 scene{8.0f, 4.0f, 2.0f};
    const FVec3 transmittance{0.25f, 0.50f, 0.75f};
    const FVec3 inScatter{0.10f, 0.20f, 0.30f};
    const FVec3 result{
        scene.x * transmittance.x + inScatter.x,
        scene.y * transmittance.y + inScatter.y,
        scene.z * transmittance.z + inScatter.z};
    EXPECT_NEAR(result.x, 2.10f, 1e-6f);
    EXPECT_NEAR(result.y, 2.20f, 1e-6f);
    EXPECT_NEAR(result.z, 1.80f, 1e-6f);
}

ACS_TEST(Atmosphere,
         PhysicalAndLocalFogGraphicsDrawsUseIndependentConstantBuffers) {
    const std::string source = ReadAtmosphereSource();
    EXPECT_TRUE(!source.empty());

    const std::size_t physicalBegin = source.find(
        "void FSkyAtmosphere::CompositeAerialPerspective");
    const std::size_t localFogBegin = source.find(
        "void FSkyAtmosphere::CompositeLocalFog", physicalBegin);
    const std::size_t localFogEnd = source.find(
        "bool FSkyAtmosphere::BakeEquirect", localFogBegin);
    EXPECT_TRUE(physicalBegin != std::string::npos);
    EXPECT_TRUE(localFogBegin != std::string::npos);
    EXPECT_TRUE(localFogEnd != std::string::npos);
    if (physicalBegin == std::string::npos ||
        localFogBegin == std::string::npos ||
        localFogEnd == std::string::npos ||
        physicalBegin >= localFogBegin ||
        localFogBegin >= localFogEnd) {
        return;
    }

    const std::string physical =
        source.substr(physicalBegin, localFogBegin - physicalBegin);
    const std::string localFog =
        source.substr(localFogBegin, localFogEnd - localFogBegin);

    // These draws are recorded before GPU submission. Sharing one writable CB
    // would let the later local-fog update overwrite both physical AP draws.
    EXPECT_TRUE(Contains(
        source,
        "m_ApCompositeCb = Move(r.Value())"));
    EXPECT_TRUE(Contains(
        source,
        "m_LocalFogCompositeCb = Move(r.Value())"));
    EXPECT_TRUE(Contains(
        physical,
        "m_ApCompositeCb->Update(&cb, sizeof(cb));"));
    EXPECT_TRUE(Contains(
        physical,
        "cl.SetConstantBuffer(0, *m_ApCompositeCb);"));
    EXPECT_FALSE(Contains(physical, "m_LocalFogCompositeCb"));
    EXPECT_TRUE(Contains(
        localFog,
        "m_LocalFogCompositeCb->Update(&cb, sizeof(cb));"));
    EXPECT_TRUE(Contains(
        localFog,
        "cl.SetConstantBuffer(0, *m_LocalFogCompositeCb);"));
    EXPECT_FALSE(Contains(localFog, "m_ApCompositeCb"));
    EXPECT_TRUE(Contains(
        source,
        "m_ApCompositeCb.Reset(); m_LocalFogCompositeCb.Reset();"));
}

ACS_TEST(Atmosphere, LocalFogVolumeExcludesRayleighAndMie) {
    const std::string source = ReadAtmosphereSource();
    const std::string editorSource = ReadEditorAbiSource();
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!editorSource.empty());

    // A second texture and constant buffer prevent the fog-only dispatch from
    // overwriting parameters still referenced by the combined AP dispatch.
    EXPECT_TRUE(Contains(source, "m_LocalFogVol"));
    EXPECT_TRUE(Contains(source, "m_LocalFogCb"));
    EXPECT_TRUE(Contains(source, "FApCB fogOnlyCb = cb;"));
    EXPECT_TRUE(Contains(
        source, "atmosphereCb.fogColorDensity.w = 0.0f;"));

    // scene_to_km=0 removes both atmospheric extinction and scattering from
    // the shared Beer-Lambert integrator while retaining local height fog.
    EXPECT_TRUE(Contains(source, "fogOnlyCb.apParams.x = 0.0f;"));
    EXPECT_TRUE(Contains(
        source, "CompositeLocalFog"));
    EXPECT_TRUE(Contains(
        source,
        "cloud_depth != nullptr ? 1.0f : 0.0f"));
    EXPECT_TRUE(Contains(
        editorSource,
        "cloudsActive ? h.vclouds3d.ResolvedDepth() : nullptr"));
}

ACS_TEST(Atmosphere, EnvironmentBakeExcludesAnalyticSunDisc) {
    const std::string source = ReadAtmosphereSource();
    EXPECT_TRUE(!source.empty());

    const std::size_t cpuBegin =
        source.find("TArray<f32> FAtmosphere::BakeEquirect");
    const std::size_t cpuEnd =
        source.find("// ===================== GPU", cpuBegin);
    const std::size_t gpuBegin =
        source.find("const char* kBakeCS");
    const std::size_t gpuEnd =
        source.find("const char* kApCS", gpuBegin);
    EXPECT_TRUE(cpuBegin != std::string::npos);
    EXPECT_TRUE(cpuEnd != std::string::npos);
    EXPECT_TRUE(gpuBegin != std::string::npos);
    EXPECT_TRUE(gpuEnd != std::string::npos);

    const std::string cpuBake =
        source.substr(cpuBegin, cpuEnd - cpuBegin);
    const std::string gpuBake =
        source.substr(gpuBegin, gpuEnd - gpuBegin);

    EXPECT_FALSE(Contains(cpuBake, "cos_to_sun"));
    EXPECT_FALSE(Contains(cpuBake, "sun_inner"));
    EXPECT_FALSE(Contains(cpuBake, "disc_strength"));
    EXPECT_FALSE(Contains(gpuBake, "cosToSun"));
    EXPECT_FALSE(Contains(gpuBake, "0.9995"));
    EXPECT_FALSE(Contains(gpuBake, "*30.0*fade"));
}

ACS_TEST(Atmosphere,
         CpuEnvironmentGroundHemisphereIsFiniteNonnegativeAndContinuous) {
    FAtmosphereParams params{};
    params.ray_steps = 16u;
    params.sun_steps = 4u;
    params.ground_albedo = FVec3{0.18f, 0.17f, 0.15f};

    constexpr u32 kWidth = 32u;
    constexpr u32 kHeight = 128u;
    const TArray<f32> pixels =
        FAtmosphere::BakeEquirect(kWidth, kHeight, params);
    EXPECT_EQ(pixels.Size(),
              static_cast<usize>(kWidth) * kHeight * 4u);

    for (usize index = 0; index < pixels.Size(); ++index) {
        EXPECT_TRUE(std::isfinite(static_cast<double>(pixels[index])));
        if ((index & 3u) != 3u) {
            EXPECT_TRUE(pixels[index] >= 0.0f);
        }
    }

    auto luminance = [&pixels](u32 x, u32 y) {
        const usize base =
            (static_cast<usize>(y) * kWidth + x) * 4u;
        return pixels[base + 0u] * 0.2126f +
               pixels[base + 1u] * 0.7152f +
               pixels[base + 2u] * 0.0722f;
    };

    // The first texel rows on either side of the horizon must meet without the
    // old constant-colour step. Use a relative bound so the contract remains
    // valid for different physically meaningful sun intensities.
    for (u32 x = 0; x < kWidth; ++x) {
        const f32 above = luminance(x, kHeight / 2u - 1u);
        const f32 below = luminance(x, kHeight / 2u);
        const f32 scale =
            above > below ? (above > 1e-4f ? above : 1e-4f)
                          : (below > 1e-4f ? below : 1e-4f);
        EXPECT_TRUE(Abs(above - below) <= scale * 0.85f + 0.02f);
    }

    // A physical view-path transfer is not a flat lower-hemisphere swatch.
    const f32 nearHorizon =
        luminance(kWidth / 2u, kHeight / 2u);
    const f32 nadir =
        luminance(kWidth / 2u, kHeight - 1u);
    EXPECT_TRUE(Abs(nearHorizon - nadir) > 1e-5f);
}

ACS_TEST(Atmosphere,
         GpuEnvironmentGroundHemisphereUsesGroundHitTransfer) {
    const std::string source = ReadAtmosphereSource();
    const std::size_t begin = source.find("const char* kBakeCS");
    const std::size_t end = source.find("const char* kApCS", begin);
    EXPECT_TRUE(begin != std::string::npos);
    EXPECT_TRUE(end != std::string::npos);
    if (begin == std::string::npos || end == std::string::npos) return;

    const std::string shader = source.substr(begin, end - begin);
    EXPECT_TRUE(Contains(shader, "float RaySphereNearGround"));
    EXPECT_TRUE(Contains(shader, "bool hitGround="));
    EXPECT_TRUE(Contains(shader, "float tMax=hitGround?tGround:tAtm"));
    EXPECT_TRUE(Contains(shader, "L+=Tview*groundUnit;"));
    EXPECT_TRUE(Contains(shader, "max(groundAlbedo.xyz,0.0)"));
    EXPECT_FALSE(Contains(shader, "if(dir.y<-0.02)"));
    EXPECT_FALSE(Contains(shader, "col=float3(0.02,0.02,0.03)"));
}

ACS_TEST(Atmosphere,
         AerialPerspectiveCachesIndependentPhysicalAndLocalFogDispatches) {
    FDeviceConfig config{};
    auto deviceResult = CreateRhiDevice(config);
    if (deviceResult.IsErr()) return;
    auto commandResult =
        CreateRhiCommandList(*deviceResult.Value());
    if (commandResult.IsErr()) return;
    TUniquePtr<IRhiCommandList> command =
        Move(commandResult.Value());

    FSkyAtmosphere atmosphere;
    const auto initResult = atmosphere.Init(
        *deviceResult.Value(), EFormat::R16G16B16A16_Float);
    EXPECT_TRUE(initResult.IsOk());
    if (initResult.IsErr()) return;

    FVolumetricFogParams fog{};
    fog.density = 0.004f;
    const FMat4 inverseViewProjection = FMat4::Identity();
    const FVec3 camera{0.0f, 2.0f, 0.0f};
    const FVec3 sunDirection{0.35f, 0.8f, 0.25f};
    const FVec3 sunIntensity{4.0f, 3.8f, 3.5f};
    command->Begin();

    auto build = [&](f32 sceneToKm, f32 cameraAltitude,
                     const FVolumetricFogParams& fogParams) {
        return atmosphere.BuildAerialPerspective(
            *deviceResult.Value(), *command, inverseViewProjection,
            camera, sunDirection, sunIntensity, 250000.0f,
            sceneToKm, cameraAltitude, fogParams);
    };

    // Fog-only mode must not initialize or overwrite the physical L/T pair.
    EXPECT_TRUE(build(0.0f, 0.0f, fog) == nullptr);
    EXPECT_EQ(atmosphere.PhysicalApDispatchCount(), 0u);
    EXPECT_EQ(atmosphere.LocalFogDispatchCount(), 1u);
    EXPECT_TRUE(build(0.0f, 0.0f, fog) == nullptr);
    EXPECT_EQ(atmosphere.PhysicalApDispatchCount(), 0u);
    EXPECT_EQ(atmosphere.LocalFogDispatchCount(), 1u);

    EXPECT_TRUE(build(0.001f, 0.0f, fog) != nullptr);
    EXPECT_EQ(atmosphere.PhysicalApDispatchCount(), 1u);
    EXPECT_EQ(atmosphere.LocalFogDispatchCount(), 1u);

    // Exact sanitized state is losslessly reusable.
    EXPECT_TRUE(build(0.001f, 0.0f, fog) != nullptr);
    EXPECT_EQ(atmosphere.PhysicalApDispatchCount(), 1u);
    EXPECT_EQ(atmosphere.LocalFogDispatchCount(), 1u);

    // Disabling and restoring AP at the same effective key reuses its prior
    // lossless volume; local fog remains independently valid throughout.
    EXPECT_TRUE(build(0.0f, 0.0f, fog) == nullptr);
    EXPECT_EQ(atmosphere.PhysicalApDispatchCount(), 1u);
    EXPECT_EQ(atmosphere.LocalFogDispatchCount(), 1u);
    EXPECT_TRUE(build(0.001f, 0.0f, fog) != nullptr);
    EXPECT_EQ(atmosphere.PhysicalApDispatchCount(), 1u);
    EXPECT_EQ(atmosphere.LocalFogDispatchCount(), 1u);

    // Local fog changes do not invalidate the long-range physical volume.
    fog.color = FVec3{0.31f, 0.42f, 0.58f};
    EXPECT_TRUE(build(0.001f, 0.0f, fog) != nullptr);
    EXPECT_EQ(atmosphere.PhysicalApDispatchCount(), 1u);
    EXPECT_EQ(atmosphere.LocalFogDispatchCount(), 2u);

    // Conversely scene-to-km only affects physical Rayleigh/Mie transfer:
    // the fog-only shader forces this scale to exactly zero.
    EXPECT_TRUE(build(0.002f, 0.0f, fog) != nullptr);
    EXPECT_EQ(atmosphere.PhysicalApDispatchCount(), 2u);
    EXPECT_EQ(atmosphere.LocalFogDispatchCount(), 2u);

    // Non-finite values canonicalize to the same finite defaults before the
    // key is compared, rather than dispatching forever on NaN != NaN.
    FVolumetricFogParams nonFiniteFog = fog;
    nonFiniteFog.height_base =
        std::numeric_limits<f32>::quiet_NaN();
    EXPECT_TRUE(build(
        0.002f, std::numeric_limits<f32>::quiet_NaN(),
        nonFiniteFog) != nullptr);
    EXPECT_EQ(atmosphere.PhysicalApDispatchCount(), 2u);
    EXPECT_EQ(atmosphere.LocalFogDispatchCount(), 2u);

    command->End();
    command->Submit();
    deviceResult.Value()->WaitIdle();

    // Read the first 3D-volume slice. Every texel must have been written by
    // u1, whose alpha is exactly half(1.0). This catches a silently missing
    // second UAV binding that source-token tests cannot observe.
    TArray<u16> transmittance;
    transmittance.Resize(
        static_cast<usize>(kSkyAtmosphereFroxelXyResolution) *
        kSkyAtmosphereFroxelXyResolution * 4u);
    const bool readbackOk = deviceResult.Value()->ReadTexture(
        *atmosphere.ApTransmittanceVolume(), transmittance.Data(),
        static_cast<u32>(transmittance.Size() * sizeof(u16)));
    EXPECT_TRUE(readbackOk);
    if (readbackOk) {
        usize writtenTexels = 0;
        for (usize texel = 0; texel < transmittance.Size() / 4u;
             ++texel) {
            const usize base = texel * 4u;
            if (transmittance[base + 3u] == 0x3c00u &&
                (transmittance[base + 0u] != 0u ||
                 transmittance[base + 1u] != 0u ||
                 transmittance[base + 2u] != 0u)) {
                ++writtenTexels;
            }
        }
        EXPECT_EQ(
            writtenTexels,
            static_cast<usize>(kSkyAtmosphereFroxelXyResolution) *
                kSkyAtmosphereFroxelXyResolution);
    }
    atmosphere.Shutdown();
}

ACS_TEST(Atmosphere, EnvironmentBakeInterpolatesAtmosphereLuts) {
    const std::string source = ReadAtmosphereSource();
    const std::size_t begin = source.find("const char* kBakeCS");
    const std::size_t end = source.find("const char* kApCS", begin);
    EXPECT_TRUE(begin != std::string::npos);
    EXPECT_TRUE(end != std::string::npos);
    if (begin == std::string::npos || end == std::string::npos) return;

    const std::string shader = source.substr(begin, end - begin);
    // Nearest LUT loads form latitude rings around the equirectangular pole.
    // Manual bilinear reconstruction keeps the generated environment and the
    // camera-volume path on the same continuous transfer functions.
    EXPECT_TRUE(Contains(shader, "int2 p0=int2(floor(p))"));
    EXPECT_TRUE(Contains(shader, "return lerp(a,b,f.y)"));
    EXPECT_TRUE(Contains(shader, "p=saturate(uv)*31.0"));
    EXPECT_FALSE(Contains(
        shader, "int2 px=int2(uv*float2(255.0,63.0)+0.5)"));
    EXPECT_FALSE(Contains(
        shader, "int2 px=int2(uv*float2(31.0,31.0)+0.5)"));
}

ACS_TEST(Atmosphere, MultiScatteringUsesUniformSolidAngleDirections) {
    const std::string source = ReadAtmosphereSource();
    const std::size_t begin = source.find("const char* kMultiCS");
    const std::size_t end = source.find("const char* kBakeCS", begin);
    EXPECT_TRUE(begin != std::string::npos);
    EXPECT_TRUE(end != std::string::npos);
    if (begin == std::string::npos || end == std::string::npos) return;

    const std::string shader = source.substr(begin, end - begin);
    EXPECT_TRUE(Contains(
        shader, "float cosPolar=1.0-2.0*v;"));
    EXPECT_TRUE(Contains(
        shader, "float sinPolar=sqrt(saturate(1.0-cosPolar*cosPolar));"));
    EXPECT_TRUE(Contains(
        shader,
        "float2 p=saturate(TransParamsToUv(r,mu))*"
        "float2(255.0,63.0);"));
    EXPECT_TRUE(Contains(
        shader, "return lerp(a,b,f.y);"));
    EXPECT_FALSE(Contains(
        shader, "float phi=PI*(fj/8.0);"));
    EXPECT_FALSE(Contains(
        shader,
        "int2 px=int2(uv*float2(255.0,63.0)+0.5)"));

    double meanX = 0.0;
    double meanY = 0.0;
    double meanZ = 0.0;
    double meanX2 = 0.0;
    double meanY2 = 0.0;
    double meanZ2 = 0.0;
    constexpr int kSide = 8;
    for (int sample = 0; sample < kSide * kSide; ++sample) {
        const double u =
            (static_cast<double>(sample % kSide) + 0.5) / kSide;
        const double v =
            (static_cast<double>(sample / kSide) + 0.5) / kSide;
        const double azimuth = 2.0 * 3.14159265358979323846 * u;
        const double z = 1.0 - 2.0 * v;
        const double ring = std::sqrt(std::fmax(0.0, 1.0 - z * z));
        const double x = std::cos(azimuth) * ring;
        const double y = std::sin(azimuth) * ring;
        meanX += x;
        meanY += y;
        meanZ += z;
        meanX2 += x * x;
        meanY2 += y * y;
        meanZ2 += z * z;
    }
    constexpr double kInvSamples = 1.0 / 64.0;
    meanX *= kInvSamples;
    meanY *= kInvSamples;
    meanZ *= kInvSamples;
    meanX2 *= kInvSamples;
    meanY2 *= kInvSamples;
    meanZ2 *= kInvSamples;

    EXPECT_NEAR(meanX, 0.0, 1e-6);
    EXPECT_NEAR(meanY, 0.0, 1e-6);
    EXPECT_NEAR(meanZ, 0.0, 1e-6);
    EXPECT_NEAR(meanX2, 1.0 / 3.0, 0.01);
    EXPECT_NEAR(meanY2, 1.0 / 3.0, 0.01);
    EXPECT_NEAR(meanZ2, 1.0 / 3.0, 0.01);
}

ACS_TEST(Atmosphere, EditorPhysicalSunUsesConfiguredLightingAndRealRadius) {
    const std::string source = ReadEditorAbiSource();
    EXPECT_TRUE(!source.empty());

    EXPECT_TRUE(Contains(
        source, "constexpr float kPhysicalSunAngularRadius = 0.004653f;"));
    EXPECT_TRUE(Contains(
        source, "h.sun_color.x * h.sun_intensity *"));
    EXPECT_TRUE(Contains(
        source, "h.sun_dir, sunDiscRadiance, kPhysicalSunAngularRadius"));

    // The physical atmosphere no longer carves a replacement cone out of its
    // indirect environment, because the direct disc was never baked there.
    EXPECT_TRUE(Contains(
        source,
        "h.q_sky_mode == 1\n"
        "                    ? 2.0f"));
}

ACS_TEST(Atmosphere, EditorPhysicalAtmosphereTracksConfiguredSunIntensity) {
    const std::string source = ReadEditorAbiSource();
    EXPECT_TRUE(!source.empty());

    // One conversion drives the physical sky environment and the camera-space
    // aerial-perspective volume, so time-of-day intensity cannot leave either
    // indirect lighting or haze at the default solar radiance.
    EXPECT_TRUE(Contains(
        source,
        "float PhysicalAtmosphereSunRadiance("
        "float configured_intensity) noexcept"));
    EXPECT_TRUE(Contains(
        source,
        "PhysicalAtmosphereSunRadiance(h.sun_intensity);"));
    EXPECT_TRUE(Contains(
        source,
        "PhysicalAtmosphereSunRadiance(h.sun_intensity) *\n"
        "                    kAtmosScale;"));
    EXPECT_FALSE(Contains(
        source, "22.0f * h.sun_color.x"));
    EXPECT_FALSE(Contains(
        source, "const f32 kApSunInt = 22.0f"));
}

ACS_TEST(Atmosphere, CpuSkyFallbackHasBoundedSynchronousResolution) {
    const std::string source = ReadEditorAbiSource();
    EXPECT_TRUE(!source.empty());

    EXPECT_TRUE(Contains(
        source,
        "constexpr u32 kPhysicalSkyCpuFallbackWidth = 512u;"));
    EXPECT_TRUE(Contains(
        source,
        "constexpr u32 kPhysicalSkyCpuFallbackHeight = 256u;"));
    EXPECT_TRUE(Contains(
        source,
        "skyWidth = kPhysicalSkyCpuFallbackWidth;"));
    EXPECT_TRUE(Contains(
        source,
        "skyHeight = kPhysicalSkyCpuFallbackHeight;"));
    EXPECT_TRUE(Contains(
        source,
        "skyWidth,\n"
        "                        skyHeight).IsOk();"));
}

ACS_TEST(Atmosphere, CloudRangeFroxelBudgetKeepsNearPrecisionBounded) {
    EXPECT_EQ(kSkyAtmosphereFroxelXyResolution, 48u);
    EXPECT_EQ(kSkyAtmosphereFroxelZResolution, 96u);
    EXPECT_EQ(kSkyAtmosphereFroxelIntegrationSteps, 24u);
    EXPECT_NEAR(kVolumetricCloudMaxDistance, 250000.0f, 1e-3f);

    // RGBA16F is eight bytes per froxel. Physical in-scatter, physical RGB
    // transmittance, and local fog are three equally sized volumes.
    const u64 bytesPerVolume =
        static_cast<u64>(kSkyAtmosphereFroxelXyResolution) *
        kSkyAtmosphereFroxelXyResolution *
        kSkyAtmosphereFroxelZResolution * 8u;
    constexpr u64 kVolumeCount = 3u;
    const u64 totalVolumeBytes = bytesPerVolume * kVolumeCount;
    EXPECT_EQ(bytesPerVolume, static_cast<u64>(1769472u));
    EXPECT_TRUE(bytesPerVolume <= 2u * 1024u * 1024u);
    EXPECT_EQ(totalVolumeBytes, static_cast<u64>(5308416u));
    EXPECT_TRUE(totalVolumeBytes <= 6u * 1024u * 1024u);

    // The dedicated short-range fog volume preserves at least 33 continuous
    // slices inside the first 300 metres. Long-range atmospheric AP remains
    // independent and reaches horizon clouds.
    const f32 nearSliceCount =
        Sqrt(300.0f / kLocalVolumetricFogMaxDistance) *
        static_cast<f32>(kSkyAtmosphereFroxelZResolution);
    EXPECT_TRUE(nearSliceCount >= 33.0f);

    // Bound the integration workload per dispatch (~5.31M samples). L and T
    // are written by one physical dispatch; local fog dispatches only when on.
    const u64 samplesPerVolume =
        static_cast<u64>(kSkyAtmosphereFroxelXyResolution) *
        kSkyAtmosphereFroxelXyResolution *
        kSkyAtmosphereFroxelZResolution *
        kSkyAtmosphereFroxelIntegrationSteps;
    EXPECT_TRUE(samplesPerVolume <= 5400000u);

    const std::string source = ReadAtmosphereSource();
    EXPECT_TRUE(Contains(source, "const int N=24;"));
    EXPECT_TRUE(Contains(
        source, "if (m_LocalFogVolumeValid) {"));
    EXPECT_TRUE(Contains(
        source, "fogOnlyCb.apParams.z = m_LocalFogMaxDistance;"));
    EXPECT_TRUE(Contains(source, "m_ApVol = Move(r.Value())"));
    EXPECT_TRUE(Contains(source, "m_ApTransVol = Move(r.Value())"));
    EXPECT_TRUE(Contains(source, "m_LocalFogVol = Move(r.Value())"));
}

ACS_TEST(Atmosphere, AerialPerspectiveShadersInitializeOnAvailableGpu) {
    FDeviceConfig config{};
    auto deviceResult = CreateRhiDevice(config);
    if (deviceResult.IsErr()) return;

    FSkyAtmosphere atmosphere;
    const auto initResult =
        atmosphere.Init(*deviceResult.Value(),
                        EFormat::R16G16B16A16_Float);
    EXPECT_TRUE(initResult.IsOk());
    if (initResult.IsOk()) {
        EXPECT_TRUE(atmosphere.ApVolume() != nullptr);
        EXPECT_TRUE(atmosphere.ApTransmittanceVolume() != nullptr);
        EXPECT_TRUE(atmosphere.LocalFogVolume() == nullptr);
    }
    atmosphere.Shutdown();
}
