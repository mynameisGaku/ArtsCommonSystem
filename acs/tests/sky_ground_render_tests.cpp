// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "render/Sky.h"
#include "render/Atmosphere.h"
#include "render/IRhiDevice.h"
#include "math/Camera.h"
#include "foundation/Move.h"
#include "platform/FileSystem.h"
#include "container/String.h"
#include <cstring>
#include <cmath>

using namespace acs;

// 倍精度値の指数が全て1ならNaNまたは無限大。近似比較がNaNを見逃すことを防ぐ。
static bool IsFiniteProbeValue_Internal(f64 value)
{
    // 数値変換せずIEEE 754の指数部分を調べる。
    u64 bits = 0u;
    ::memcpy(&bits,&value,sizeof(bits));
    return (bits & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
}

// 検査と同じソースツリーから指定したrender実装を読む。読取り不能なら空文字列を返す。
static FString ReadRenderSource_Internal(const wchar_t* suffix)
{
    // コンパイル元のファイル名から検査対象のソースだけを解決する。
    constexpr wchar_t compiledPath[] = L"" __FILE__;
    // 最後の区切りの直後。作業ディレクトリには依存しない。
    usize directoryLength = 0u;
    for (usize index = 0u; compiledPath[index] != L'\0'; ++index) {
        if (compiledPath[index] == L'/' || compiledPath[index] == L'\\') directoryLength = index + 1u;
    }
    // ソースパスの追加領域。容量を超える接尾辞は切り詰めず拒否する。
    wchar_t sourcePath[sizeof(compiledPath) / sizeof(wchar_t) + 128u]{};
    // 接尾辞の終端までの文字数。
    usize suffixLength = 0u;
    while (suffix[suffixLength] != L'\0') ++suffixLength;
    if (directoryLength + suffixLength >= sizeof(sourcePath) / sizeof(wchar_t)) return {};
    for (usize index = 0u; index < directoryLength; ++index) sourcePath[index] = compiledPath[index];
    for (usize index = 0u; index <= suffixLength; ++index) sourcePath[directoryLength + index] = suffix[index];
    // 本体は既存のファイル読み取りと配列を使う。
    auto source = CFileSystem::ReadAllText(sourcePath);
    if (source.IsErr()) return {};
    return FString(source.Value().GetData());
}

// 製品HLSLを直接読み、検査入口だけを追加する。読取り不能や宣言欠落なら空文字列を返す。
static FString ReadPhysicalSkyShader_Internal()
{
    // 本体の生文字列を含む現在の実装。
    const FString source = ReadRenderSource_Internal(L"../src/render/Sky.cpp");
    if (source.Size() == 0u) return {};
    // 宣言名と生文字列の境界を検査し、別シェーダーを誤抽出しない。
    const char* declaration = ::strstr(source.Data(), "const char* kSkyHLSL");
    if (!declaration) return {};
    // HLSLの開始位置。
    const char* begin = ::strstr(declaration, "R\"(");
    if (!begin) return {};
    begin += 3u;
    // HLSLの終了位置。
    const char* end = ::strstr(begin, ")\";");
    if (!end) return {};
    return FString(FStringView(begin, static_cast<usize>(end - begin)));
}

// 製品の共通HLSLマクロだけを復元する。未対応のエスケープや宣言形式なら失敗する。
static FString ReadAtmosphereCommonShader_Internal()
{
    // 同じソースツリーにある大気表の実装。
    const FString source = ReadRenderSource_Internal(L"../src/render/Atmosphere.cpp");
    if (source.Size() == 0u) return {};
    // マクロ宣言後の最初の文字列から、行継続が途切れるまでを読む。
    const char* cursor = ::strstr(source.Data(), "#define ATMO_COMMON_HLSL");
    if (!cursor) return {};
    cursor = ::strchr(cursor, '\n');
    if (!cursor) return {};
    ++cursor;
    // 引用符と改行のエスケープを戻したHLSL本体。
    FString result;
    for (;;) {
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor++ != '"') return {};
        while (*cursor && *cursor != '"') {
            // 現在の1文字。C++文字列の改行などだけを復元する。
            char value = *cursor++;
            if (value == '\\') {
                value = *cursor++;
                if (value == 'n') value = '\n';
                else if (value == 't') value = '\t';
                else if (value != '\\' && value != '"') return {};
            }
            result.Append(value);
        }
        if (*cursor++ != '"') return {};
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor != '\\') return result;
        ++cursor;
        if (*cursor == '\r') ++cursor;
        if (*cursor++ != '\n') return {};
    }
}

// kmで与えた鉛直光路を倍精度の中点則で積む。段数の再現と積分精度の合格は区別する。
static f64 ReferenceVerticalTransmittance_Internal(f64 startKm, f64 distanceKm, u32 steps, u32 channel)
{
    // RGBの分子散乱係数とオゾン吸収係数。単位はkmの逆数。
    constexpr f64 rayleigh[3] = {0.005802,0.013558,0.0331};
    constexpr f64 ozone[3] = {0.000650,0.001881,0.000085};
    // 積分区間の幅と、累積した無次元の光学的厚さ。
    const f64 width = distanceKm / steps;
    f64 depth = 0.0;
    for (u32 index = 0u; index < steps; ++index) {
        // 地表からの高度と、10〜40kmで三角形をなすオゾン密度。
        const f64 altitude = startKm + (index + 0.5) * width;
        const f64 ozoneDensity = 1.0 - ::fabs(altitude - 25.0) / 15.0;
        depth += (rayleigh[channel] * ::exp(-altitude / 8.0) + 0.0044 * ::exp(-altitude / 1.2) + ozone[channel] * (ozoneDensity > 0.0 ? ozoneDensity : 0.0)) * width;
    }
    return ::exp(-depth);
}

// 通常C++の太陽透過率にも、GPUと同じ消散係数を適用する。積分段数24は変更しない。
ACS_TEST(Atmosphere, CpuSunTransmittanceUsesSharedMieExtinction)
{
    // 高度はm。地表、雲層、分子とオゾンが優勢な高度を含める。
    constexpr f32 altitudes[6] = {0.0f,1200.0f,8000.0f,10000.0f,25000.0f,40000.0f};
    for (u32 index = 0u; index < 6u; ++index) {
        // 公開APIから得る実製品の透過率。
        const FVec3 actual = SunTransmittanceAtAltitude(altitudes[index],FVec3{0.0f,1.0f,0.0f});
        // RGBを同じ参照計算へ渡す。
        const f32 channels[3] = {actual.x,actual.y,actual.z};
        for (u32 channel = 0u; channel < 3u; ++channel) {
            // mからkmへ換算し、製品と同じ24点の中点積分と比べる。
            const f64 expected = ReferenceVerticalTransmittance_Internal(altitudes[index] * 0.001,100.0 - altitudes[index] * 0.001,24u,channel);
            EXPECT_TRUE(IsFiniteProbeValue_Internal(channels[channel]));
            EXPECT_TRUE(IsFiniteProbeValue_Internal(expected));
            test::RecordInfo(FSourceLoc::Current(),"mie_cpu altitude_m=%g channel=%u actual=%.9g expected=%.9g",altitudes[index],channel,channels[channel],expected);
            EXPECT_NEAR(channels[channel],expected,2.0e-5);
        }
    }
}

// CPUの視線側も検査し、太陽透過率だけ直して単散乱内の二重加算を残す回帰を防ぐ。
ACS_TEST(Atmosphere, CpuSingleScatterUsesSharedMieExtinction)
{
    // 鉛直上向き同士の散乱を、地表遮蔽や地平線の交差誤差から切り離す。
    FAtmosphereParams parameters{};
    parameters.sun_dir = FVec3{0.0f,1.0f,0.0f};
    parameters.sun_intensity = FVec3{1.0f,0.8f,0.6f};
    parameters.ray_steps = 50u;
    parameters.sun_steps = 20u;
    // 地表から大気上端までの実製品の単散乱値。
    const FVec3 actual = CAtmosphere::EvaluateSkyRadiance(0.0f,FVec3{0.0f,1.0f,0.0f},parameters);
    // 比較する各成分の係数と入射光。積分経路の単位はkm。
    constexpr f64 rayleigh[3] = {0.005802,0.013558,0.0331};
    constexpr f64 ozone[3] = {0.000650,0.001881,0.000085};
    constexpr f64 incident[3] = {1.0,0.8,0.6};
    const f32 channels[3] = {actual.x,actual.y,actual.z};
    // 前方散乱の位相値を、それぞれの正規化式から評価する。
    constexpr f64 pi = 3.14159265358979323846;
    const f64 phaseRayleigh = 3.0 / (8.0 * pi);
    const f64 phaseMie = 1.8 / (4.0 * pi * 0.2 * 0.2);
    for (u32 channel = 0u; channel < 3u; ++channel) {
        // 区間入口の透過率と、それまでに届いた散乱光。
        f64 transmission = 1.0;
        f64 radiance = 0.0;
        for (u32 segment = 0u; segment < 50u; ++segment) {
            // 2km幅の中点で媒質を一定とする。段数を増やす精度改善ではない。
            const f64 altitude = 2.0 * segment + 1.0;
            const f64 densityR = ::exp(-altitude / 8.0);
            const f64 densityM = ::exp(-altitude / 1.2);
            const f64 densityO = 1.0 - ::fabs(altitude - 25.0) / 15.0;
            // 消散係数と2km幅の積。散乱を二重に加算しない。
            const f64 depth = 2.0 * (rayleigh[channel] * densityR + 0.0044 * densityM + ozone[channel] * (densityO > 0.0 ? densityO : 0.0));
            // 太陽光路だけは既存の20点則を独立計算し、視線側の係数契約を分離する。
            const f64 sun = ReferenceVerticalTransmittance_Internal(altitude,100.0-altitude,20u,channel);
            const f64 source = rayleigh[channel] * densityR * phaseRayleigh + 0.003996 * densityM * phaseMie;
            // 区間内のBeer-Lambert積分を桁落ちしない倍精度の式で計算する。
            radiance += transmission * sun * source * 2.0 * (-::expm1(-depth) / depth);
            transmission *= ::exp(-depth);
        }
        test::RecordInfo(FSourceLoc::Current(),"mie_cpu_scatter channel=%u actual=%.9g expected=%.9g",channel,channels[channel],radiance*incident[channel]);
        EXPECT_TRUE(IsFiniteProbeValue_Internal(channels[channel]));
        EXPECT_TRUE(IsFiniteProbeValue_Internal(radiance*incident[channel]));
        EXPECT_NEAR(channels[channel],radiance*incident[channel],2.0e-6);
    }
}

// 二つの実製品HLSLを同じ実行時高度で計算し、密度と散乱係数の違いも換算して比較する。
ACS_TEST(Atmosphere, ActualGpuMediumPathsUseSharedMieExtinction)
{
    // 空本体と大気表の共通部分を、複製せず現在のソースから取り出す。
    FString source = ReadPhysicalSkyShader_Internal();
    const FString common = ReadAtmosphereCommonShader_Internal();
    EXPECT_TRUE(source.Size() > 0u && common.Size() > 0u);
    if (source.Size() == 0u || common.Size() == 0u) return;
    source.Append(common.View());
    source.Append(R"(
RWTexture2D<float4> mediumProbeOutput : register(u0);
[numthreads(1,1,1)]
void CSMediumProbe(uint3 id : SV_DispatchThreadID) {
    // 密度だけを返す空本体の関数と、係数まで掛ける大気表の関数を同じ単位へそろえる。
    float3 densityR; float densityM; float3 skyExtinction;
    SamplePhysicalMedium(physical_params.y,densityR,densityM,skyExtinction);
    float3 scatteringR; float scatteringM; float3 tableExtinction;
    SampleMedium(physical_params.y,scatteringR,scatteringM,tableExtinction);
    mediumProbeOutput[uint2(0,0)] = float4(skyExtinction,1.0);
    mediumProbeOutput[uint2(1,0)] = float4(tableExtinction,1.0);
    mediumProbeOutput[uint2(2,0)] = float4(kPhysicalRayleighBeta*densityR,kPhysicalMieBeta*densityM);
    mediumProbeOutput[uint2(3,0)] = float4(scatteringR,scatteringM);
    // 高度だけでなく光路長も定数バッファから与え、0長と微小区間を含めて評価する。
    mediumProbeOutput[uint2(4,0)] = float4(PhysicalTransmittance(float3(0.0,physical_params.y,0.0),float3(0.0,1.0,0.0),cloud_params0.x),1.0);
}
)");
    // 実GPUがない場合は黙って合格させない。
    FDeviceConfig configuration{};
    auto device = CreateRhiDevice(configuration);
    EXPECT_TRUE(device.IsOk());
    if (device.IsErr()) return;
    // 製品関数を呼ぶ検査入口。
    FShaderDesc shaderDescription{};
    shaderDescription.stage = EShaderStage::Compute;
    shaderDescription.hlsl_source = source.Data();
    shaderDescription.entry_point = "CSMediumProbe";
    shaderDescription.target = "cs_5_1";
    auto shader = CreateRhiShader(*device.Value(),shaderDescription);
    EXPECT_TRUE(shader.IsOk());
    if (shader.IsErr()) return;
    // 入力は製品の定数配置を維持し、検査用出力だけを追加する。
    FComputePipelineDesc pipelineDescription{};
    pipelineDescription.cs = shader.Value().Get();
    pipelineDescription.cbuffer_slots = 1u;
    pipelineDescription.cbuffer_names[0] = "CSky";
    pipelineDescription.uav_slots = 1u;
    pipelineDescription.uav_names[0] = "mediumProbeOutput";
    auto pipeline = CreateRhiComputePipeline(*device.Value(),pipelineDescription);
    EXPECT_TRUE(pipeline.IsOk());
    if (pipeline.IsErr()) return;
    // 散乱・消散の2経路と空本体の透過率を保持する5画素。
    FTextureDesc textureDescription{};
    textureDescription.width = 5u;
    textureDescription.height = 1u;
    textureDescription.format = EFormat::R32G32B32A32_Float;
    textureDescription.is_uav = true;
    auto texture = CreateRhiTexture(*device.Value(),textureDescription);
    EXPECT_TRUE(texture.IsOk());
    if (texture.IsErr()) return;
    // 製品定数の行13が高度、行11が検査用光路長。
    FVec4 constants[16]{};
    FBufferDesc bufferDescription{};
    bufferDescription.size = sizeof(constants);
    bufferDescription.usage = EBufferUsage::Uniform;
    bufferDescription.cpu_writable = true;
    auto buffer = CreateRhiBuffer(*device.Value(),bufferDescription);
    EXPECT_TRUE(buffer.IsOk());
    if (buffer.IsErr()) return;
    // 各入力で提出と完了を確認する命令列。
    auto command = CreateRhiCommandList(*device.Value());
    EXPECT_TRUE(command.IsOk());
    if (command.IsErr()) return;
    // スケール高度、オゾンの折れ目と、大気上端。単位はkm。
    constexpr f32 heights[7] = {0.0f,1.2f,8.0f,10.0f,25.0f,40.0f,100.0f};
    // 光路長を全高度で変更し、関数全体の定数計算を避ける。
    constexpr f32 distances[3] = {0.0f,0.001f,1.2f};
    // 経路間比較とは独立した、kmの逆数で表す散乱と吸収の基準。
    constexpr f64 rayleigh[3] = {0.005802,0.013558,0.0331};
    constexpr f64 ozone[3] = {0.000650,0.001881,0.000085};
    for (u32 height = 0u; height < 7u; ++height) {
        for (u32 distance = 0u; distance < 3u; ++distance) {
            constants[13].y = heights[height];
            constants[11].x = distances[distance];
            buffer.Value()->Update(constants,sizeof(constants));
            command.Value()->Begin();
            command.Value()->SetComputePipeline(*pipeline.Value());
            command.Value()->SetConstantBuffer(0u,*buffer.Value());
            command.Value()->BindUav(0u,*texture.Value());
            command.Value()->Dispatch(1u,1u,1u);
            command.Value()->End();
            // 実行失敗や読戻し失敗を数値0と取り違えない。
            const bool submitted = command.Value()->Submit();
            EXPECT_TRUE(submitted);
            if (!submitted) return;
            device.Value()->WaitIdle();
            f32 values[20]{};
            const bool read = device.Value()->ReadTexture(*texture.Value(),values,sizeof(values));
            EXPECT_TRUE(read);
            if (!read) return;
            for (u32 index = 0u; index < 20u; ++index) EXPECT_TRUE(IsFiniteProbeValue_Internal(values[index]));
            // 指定高度における独立した密度の参照値。
            const f64 densityR = ::exp(-heights[height] / 8.0);
            const f64 densityM = ::exp(-heights[height] / 1.2);
            const f64 densityO = 1.0 - ::fabs(heights[height] - 25.0) / 15.0;
            for (u32 channel = 0u; channel < 3u; ++channel) {
                // 消散は散乱を含むので、0.0044へ散乱係数をもう一度足してはいけない。
                const f64 expected = rayleigh[channel] * densityR + 0.0044 * densityM + ozone[channel] * (densityO > 0.0 ? densityO : 0.0);
                // 参照側も有限値を要求し、NaN同士が近似比較を通ることを防ぐ。
                const f64 referenceTransmittance = ReferenceVerticalTransmittance_Internal(heights[height],distances[distance],8u,channel);
                EXPECT_TRUE(IsFiniteProbeValue_Internal(expected));
                EXPECT_TRUE(IsFiniteProbeValue_Internal(referenceTransmittance));
                EXPECT_TRUE(IsFiniteProbeValue_Internal(densityR));
                EXPECT_TRUE(IsFiniteProbeValue_Internal(densityM));
                EXPECT_NEAR(values[channel],expected,2.0e-8);
                EXPECT_NEAR(values[4u+channel],expected,2.0e-8);
                EXPECT_NEAR(values[channel],values[4u+channel],2.0e-8);
                EXPECT_NEAR(values[8u+channel],rayleigh[channel]*densityR,2.0e-8);
                EXPECT_NEAR(values[8u+channel],values[12u+channel],2.0e-8);
                EXPECT_NEAR(values[16u+channel],referenceTransmittance,2.0e-6);
            }
            EXPECT_NEAR(values[11],0.003996*densityM,2.0e-9);
            EXPECT_NEAR(values[15],0.003996*densityM,2.0e-9);
            EXPECT_EQ(values[3],1.0f);
            EXPECT_EQ(values[7],1.0f);
            EXPECT_EQ(values[19],1.0f);
            if (distance == 0u) test::RecordInfo(FSourceLoc::Current(),"mie_gpu altitude_km=%g sky_R=%.9g table_R=%.9g",heights[height],values[0],values[4]);
        }
    }
}

// カメラ復元と交差判定をGPUから取り出し、CPUの正規化との差を交点の不具合と混同しない。
ACS_TEST(Atmosphere, PhysicalSkyGroundKernelMatchesActualGpuDirections)
{
    // 製品HLSLのコピーでなく、現在の実装へ検査入口を付ける。
    FString source = ReadPhysicalSkyShader_Internal();
    EXPECT_TRUE(source.Size() > 0u);
    if (source.Size() == 0u) return;
    source.Append(R"(
RWTexture2D<float4> groundProbeOutput : register(u0);
[numthreads(1,1,1)]
void CSPhysicalGroundProbe(uint3 id : SV_DispatchThreadID) {
    // 実行時の逆変換から、製品関数そのものが返す方向を記録する。
    float3 direction = CameraRelativeViewDirection(float2(0.0,0.0));
    // カメラの高度と、直接与えた反例方向を比較する。
    float3 origin = float3(0.0,physical_params.y,0.0);
    groundProbeOutput[uint2(0,0)] = float4(direction,1.0);
    groundProbeOutput[uint2(1,0)] = float4(PhysicalPolarDiscriminant(origin.y,direction,kPhysicalGroundRadiusKm),PhysicalRaySphereNear(origin,direction,kPhysicalGroundRadiusKm),PhysicalRaySphereOuter(origin,direction,kPhysicalTopRadiusKm),1.0);
    groundProbeOutput[uint2(2,0)] = float4(sun_dir.xyz,1.0);
    groundProbeOutput[uint2(3,0)] = float4(PhysicalPolarDiscriminant(origin.y,sun_dir.xyz,kPhysicalGroundRadiusKm),PhysicalRaySphereNear(origin,sun_dir.xyz,kPhysicalGroundRadiusKm),PhysicalSegmentTransfer(cloud_params0.x),1.0);
    // 地表遮蔽と上端接線を実際の関数呼出しで確認する。
    groundProbeOutput[uint2(4,0)] = float4(PhysicalTransmittance(origin,sun_dir.xyz,PhysicalRaySphereOuter(origin,sun_dir.xyz,kPhysicalTopRadiusKm)),PhysicalRaySphereOuter(float3(0.0,100.0,0.0),float3(1.0,0.0,0.0),kPhysicalTopRadiusKm));
    // 下位桁が途中で消える回帰を、最終の交点だけでなく各段でも検出する。
    precise float2 radial = PhysicalCompensatedProduct(kPhysicalGroundRadiusKm,sun_dir.y);
    precise float2 horizontal = PhysicalCompensatedProduct(sun_dir.x,sun_dir.x);
    precise float2 offset = PhysicalCompensatedPairSum(PhysicalCompensatedProduct(origin.y,origin.y),PhysicalCompensatedProduct(12720.0,origin.y));
    precise float2 squared = PhysicalCompensatedPairProduct(radial,radial);
    precise float2 other = PhysicalCompensatedPairProduct(offset,horizontal);
    groundProbeOutput[uint2(5,0)] = float4(radial,horizontal);
    groundProbeOutput[uint2(6,0)] = float4(offset,squared);
    groundProbeOutput[uint2(7,0)] = float4(other,PhysicalCompensatedPairSum(squared,-other));
}
)");
    // GPUは必須で、使えなければ試験を失敗させる。
    FDeviceConfig configuration{};
    auto device = CreateRhiDevice(configuration);
    EXPECT_TRUE(device.IsOk());
    if (device.IsErr()) return;
    // 検査入口だけをSM5.1の計算シェーダーへコンパイルする。
    FShaderDesc shaderDescription{};
    shaderDescription.stage = EShaderStage::Compute;
    shaderDescription.hlsl_source = source.Data();
    shaderDescription.entry_point = "CSPhysicalGroundProbe";
    shaderDescription.target = "cs_5_1";
    auto shader = CreateRhiShader(*device.Value(), shaderDescription);
    EXPECT_TRUE(shader.IsOk());
    if (shader.IsErr()) return;
    // 製品定数バッファをそのまま使い、出力1枚だけを検査用に追加する。
    FComputePipelineDesc pipelineDescription{};
    pipelineDescription.cs = shader.Value().Get();
    pipelineDescription.cbuffer_slots = 1u;
    pipelineDescription.cbuffer_names[0] = "CSky";
    pipelineDescription.uav_slots = 1u;
    pipelineDescription.uav_names[0] = "groundProbeOutput";
    auto pipeline = CreateRhiComputePipeline(*device.Value(), pipelineDescription);
    EXPECT_TRUE(pipeline.IsOk());
    if (pipeline.IsErr()) return;
    // 方向2組とそれぞれの交差結果を、丸めない形式で保持する。
    FTextureDesc textureDescription{};
    textureDescription.width = 8u;
    textureDescription.height = 1u;
    textureDescription.format = EFormat::R32G32B32A32_Float;
    textureDescription.is_uav = true;
    auto texture = CreateRhiTexture(*device.Value(), textureDescription);
    EXPECT_TRUE(texture.IsOk());
    if (texture.IsErr()) return;
    // 公開描画試験と同一の逆変換を設定する。
    CCamera camera;
    camera.SetOrthographic(2.0f,2.0f,0.1f,10.0f);
    camera.SetLookDirection(FVec3{},FVec3{0.9891135096549988f,-0.14715440571308136f,0.0f},FVec3{0.0f,0.0f,1.0f});
    // 製品cbufferの16行。行0..3が逆変換、行5が太陽方向、行13が物理設定。
    FVec4 constants[16]{};
    // 同じ配置を使う行列を、成分の解釈を変えずにコピーする。
    const FMat4 inverse = BuildCameraRelativeInverseViewProjection(camera.View(),camera.Projection());
    ::memcpy(constants,&inverse,sizeof(inverse));
    // 実行時の入力を使い、式全体の定数計算でGPUの不具合を隠さない。
    FBufferDesc bufferDescription{};
    bufferDescription.size = sizeof(constants);
    bufferDescription.usage = EBufferUsage::Uniform;
    bufferDescription.cpu_writable = true;
    auto buffer = CreateRhiBuffer(*device.Value(),bufferDescription);
    EXPECT_TRUE(buffer.IsOk());
    if (buffer.IsErr()) return;
    // 提出と完了を確認してから出力を読み戻す。
    auto command = CreateRhiCommandList(*device.Value());
    EXPECT_TRUE(command.IsOk());
    if (command.IsErr()) return;
    // 反例、隣接方向、地表、接線、高度上限、微小高度をkmで列挙する。
    const f32 heights[10] = {70.0f,70.0f,0.0f,0.0f,100.0f,0.00048828125f,1.0f,0.0001f,70.0f,2.0f};
    // 内外の2値は1ULPだけ異なり、判別式に許容差を足す回避を検出する。
    const FVec3 directions[10] = {{0.9891135096549988f,-0.14715442061424255f,0.0f},{0.9891135096549988f,-0.14715440571308136f,0.0f},{0.0f,-1.0f,0.0f},{1.0f,0.0f,0.0f},{1.0f,0.0f,0.0f},{0.9999999403953552f,-0.0004149999876972288f,0.0f},{0.9998428225517273f,-0.01773073337972164f,0.0f},{0.0f,-1.0f,0.0f},{0.7f,-0.148f,0.7f},{0.5f,-0.25f,0.5f}};
    // 平均透過率のゼロ極限、級数の切替内外、厚い媒質も調べる。
    const f32 depths[12] = {0.0f,1.0e-8f,1.0e-6f,0.009999f,0.01f,0.010001f,0.12499999f,0.125f,0.12500001f,0.25f,1.0f,100.0f};
    for (u32 sample = 0u; sample < 12u; ++sample) {
        // 光学的深さの追加条件にも、幾何条件を巡回して割り当てる。
        const u32 geometry = sample % 10u;
        constants[5] = FVec4{directions[geometry].x,directions[geometry].y,directions[geometry].z,0.0f};
        constants[13] = FVec4{1.0f,heights[geometry],0.0f,0.0f};
        constants[11].x = depths[sample];
        buffer.Value()->Update(constants,sizeof(constants));
        command.Value()->Begin();
        command.Value()->SetComputePipeline(*pipeline.Value());
        command.Value()->SetConstantBuffer(0u,*buffer.Value());
        command.Value()->BindUav(0u,*texture.Value());
        command.Value()->Dispatch(1u,1u,1u);
        command.Value()->End();
        // 未提出の結果を判定しない。
        const bool submitted = command.Value()->Submit();
        EXPECT_TRUE(submitted);
        if (!submitted) return;
        device.Value()->WaitIdle();
        // GPUが実際に使った方向と交点を記録する。
        f32 values[32]{};
        // 全画素の取得に失敗したら、その場で未検査のまま終了する。
        const bool read = device.Value()->ReadTexture(*texture.Value(),values,sizeof(values));
        EXPECT_TRUE(read);
        if (!read) return;
        if (sample == 0u) {
            for (u32 index = 5u; index < 8u; ++index) test::RecordInfo(FSourceLoc::Current(),"sky_pair index=%u (%.12g,%.12g,%.12g,%.12g)",index,values[index*4u],values[index*4u+1u],values[index*4u+2u],values[index*4u+3u]);
            // 主値と下位を足しても倍精度で区別できる誤差を検査し、途中の下位0化を捕まえる。
            const f64 radial = 6360.0 * directions[geometry].y;
            // 地表からの二乗距離差。
            const f64 offset = (6360.0+heights[geometry])*(6360.0+heights[geometry])-6360.0*6360.0;
            EXPECT_NEAR(static_cast<f64>(values[26])+values[27],radial*radial,2.0e-6);
            EXPECT_NEAR(static_cast<f64>(values[28])+values[29],offset*directions[geometry].x*directions[geometry].x,2.0e-6);
        }
        for (u32 index = 0u; index < 4u; ++index) {
            test::RecordInfo(FSourceLoc::Current(),"physical_sky_kernel_gpu case=%u index=%u value=(%.12g,%.12g,%.12g,%.12g)",sample,index,values[index*4u],values[index*4u+1u],values[index*4u+2u],values[index*4u+3u]);
            EXPECT_EQ(values[index*4u+3u],1.0f);
        }
        for (u32 ray = 0u; ray < 2u; ++ray) {
            // CPU復元値でなく、GPUから得た方向自体を参照計算へ使う。
            const f32* direction = values + ray * 8u;
            // 単位長を仮定しない二次係数。
            const f64 a = static_cast<f64>(direction[0])*direction[0]+static_cast<f64>(direction[1])*direction[1]+static_cast<f64>(direction[2])*direction[2];
            // 中心からの距離は倍精度で作り、GPUの展開式を複製しない。
            const f64 radius = 6360.0 + heights[geometry];
            // 一次係数の半分。
            const f64 b = radius * direction[1];
            // 視点から地表球への二乗距離差。
            const f64 c = radius*radius-6360.0*6360.0;
            // 独立した倍精度の判別式。
            const f64 discriminant = b*b-a*c;
            // GPU計算は上下位で補償するが、最後のfloat格納の丸めは許容する。
            EXPECT_NEAR(direction[4],discriminant,::fabs(discriminant)*2.0e-6+2.0e-6);
            // 球面に入る方向だけが地面を終点にする。
            const bool hitsGround = b < 0.0 && discriminant >= 0.0;
            if (hitsGround) {
                // 入口距離は倍精度の元の二次式から求める。
                const f64 distance = (-b-::sqrt(discriminant))/a;
                EXPECT_NEAR(direction[5],distance,::fabs(distance)*2.0e-6+1.0e-6);
            } else {
                EXPECT_EQ(direction[5],-1.0f);
            }
            if (ray == 1u) {
                // 直接入力のビット値がGPUへ届いたことも検査する。
                EXPECT_EQ(direction[0],directions[geometry].x);
                EXPECT_EQ(direction[1],directions[geometry].y);
                EXPECT_EQ(direction[2],directions[geometry].z);
                for (u32 component = 0u; component < 3u; ++component) {
                    if (hitsGround) EXPECT_EQ(values[16u+component],0.0f);
                    else EXPECT_TRUE(values[16u+component] > 0.0f && values[16u+component] <= 1.0f);
                }
            }
        }
        // 数学ライブラリの倍精度expm1で、小さい差を失わない独立参照を作る。
        const f64 expectedTransfer = depths[sample] == 0.0f ? 1.0 : -::expm1(-static_cast<f64>(depths[sample]))/depths[sample];
        EXPECT_NEAR(values[14],expectedTransfer,2.0e-6);
        // 大気上端の接線は距離0であり、NaNや無限遠にはならない。
        EXPECT_EQ(values[19],0.0f);
    }
}

// 公開描画から地表境界と微小光路を検査する。GPUを利用できなければ試験を失敗させる。
ACS_TEST(Atmosphere, PhysicalSkyPublicDrawKeepsGroundBoundaryContinuous)
{
    // この試験は実GPUを必須とし、初期化失敗を未実行の合格へ読み替えない。
    FDeviceConfig configuration{};
    // 実機の作成結果を検査し、未実行を成功と区別する。
    auto deviceResult = CreateRhiDevice(configuration);
    EXPECT_TRUE(deviceResult.IsOk());
    if (deviceResult.IsErr()) {
        test::RecordInfo(FSourceLoc::Current(), "physical_sky_ground_gpu unavailable");
        return;
    }
    // 試験中の描画資源を所有するデバイス。
    auto device = Move(deviceResult.Value());
    test::RecordInfo(FSourceLoc::Current(), "physical_sky_ground_device backend=%s adapter=%s", device->BackendName(), device->AdapterName());
    // 条件ごとに提出する描画命令の作成結果。
    auto commandResult = CreateRhiCommandList(*device);
    EXPECT_TRUE(commandResult.IsOk());
    if (commandResult.IsErr()) return;
    // 定数バッファを再使用する前に提出と完了を待つ命令列。
    auto command = Move(commandResult.Value());
    // 半精度の丸めと表示用の色変換を入れず、線形HDRを比較する。
    FTextureDesc targetDescription{};
    targetDescription.width = 2u;
    targetDescription.height = 2u;
    targetDescription.format = EFormat::R32G32B32A32_Float;
    targetDescription.is_render_target = true;
    // 4画素の実描画先を作成した結果。
    auto targetResult = CreateRhiTexture(*device, targetDescription);
    EXPECT_TRUE(targetResult.IsOk());
    if (targetResult.IsErr()) return;
    // 各条件の描画と読戻しに使う線形HDR画像。
    auto target = Move(targetResult.Value());
    // 試験用複製でなく、実際の公開描画経路を持つスカイ。
    CSky sky;
    // 製品シェーダーのコンパイルと描画設定の作成結果。
    const auto initialized = sky.Init(*device, targetDescription.format);
    EXPECT_TRUE(initialized.IsOk());
    if (initialized.IsErr()) return;
    sky.SetFallbackCloudsEnabled(false);
    sky.SetSunDirection(FVec3{0.0f, 1.0f, 0.0f});
    // 正射影の全画素へ同じ視線を渡し、画素微分や円盤境界の影響を除く。
    CCamera camera;
    camera.SetOrthographic(2.0f, 2.0f, 0.1f, 10.0f);
    // 初期色とアルファも検査し、未描画を有効な黒として合格させない。
    const FClearColor clear{7.0f, 8.0f, 9.0f, 0.0f};
    // 大気圏外の太陽入力。表示変換と円盤の影響を除いて相対変化を比べる。
    const FVec3 irradiance{1.0f, 0.8f, 0.6f};
    // 地表へ到達した場合だけ最終色へ寄与する反射率。
    const FVec3 albedo{0.4f, 0.3f, 0.2f};
    // 条件ごとの4画素RGBAを、丸めずに保持する。
    f32 results[13][16]{};
    // 地表近傍、反射率の対照、表面接線、地平線の内外を独立提出する。
    for (u32 sample = 0u; sample < 13u; ++sample) {
        // 高度はm。二つの地平線反例は、同じ単精度入力を倍精度で解いた符号と比較する。
        const f32 altitude = sample == 1u || sample == 10u ? 0.5f : (sample == 5u || sample == 11u ? 0.1f : (sample == 8u || sample == 9u ? 1000.0f : (sample == 6u || sample == 7u ? 0.48828125f : 0.0f)));
        // 直下・表面接線・地平線内外を切り替える。反例の値は単精度の入力として固定する。
        const FVec3 direction = sample == 4u ? FVec3{0.0f, 0.0f, 1.0f} : (sample == 8u || sample == 9u ? FVec3{0.9998428225517273f, -0.01773073337972164f, 0.0f} : (sample == 6u || sample == 7u ? FVec3{0.9999999403953552f, -0.0004149999876972288f, 0.0f} : FVec3{0.0f, -1.0f, 0.0f}));
        // 視線と平行にならないカメラ上方向。
        const FVec3 up = sample == 4u ? FVec3{0.0f, 1.0f, 0.0f} : FVec3{0.0f, 0.0f, 1.0f};
        // 黒い地面を対照にし、散乱と地表反射の寄与を分離する。
        const FVec3 reflectance = sample == 2u || sample == 7u || sample == 9u || sample == 10u || sample == 11u ? FVec3{} : (sample == 3u ? albedo * 2.0f : albedo);
        camera.SetLookDirection(FVec3{}, direction, up);
        if (sample == 6u || sample == 8u) {
            // カメラの逆変換後にも、独立した倍精度の球方程式で内外の反例性を保つ。
            FVec3 restored{};
            // 公開描画と同じカメラから得る逆変換。ただし球方程式は倍精度で独立に解く。
            const FMat4 inverse = BuildCameraRelativeInverseViewProjection(camera.View(), camera.Projection());
            EXPECT_TRUE(TryBuildCameraRelativeViewDirection(inverse, 0.0f, 0.0f, restored));
            // 惑星中心から視点までの距離km。
            const f64 radius = 6360.0 + static_cast<f64>(altitude) * 0.001;
            // 視線の正規化丸めも含めた二次係数。
            const f64 a = static_cast<f64>(restored.x) * restored.x + static_cast<f64>(restored.y) * restored.y + static_cast<f64>(restored.z) * restored.z;
            // 一次係数の半分。視点は北極上のためY成分だけを使う。
            const f64 b = radius * restored.y;
            // 地表球に対する視点の距離の二乗差。
            const f64 c = radius * radius - 6360.0 * 6360.0;
            // 期待する地平線内外を決める独立計算値。
            const f64 discriminant = b * b - a * c;
            EXPECT_TRUE(sample == 6u ? discriminant > 0.1 : discriminant < -0.1);
            test::RecordInfo(FSourceLoc::Current(), "physical_sky_ground_reference case=%u discriminant=%.12g direction=(%.9g,%.9g,%.9g)", sample, discriminant, restored.x, restored.y, restored.z);
        }
        command->Begin();
        command->BeginRenderToTexture(*target, clear);
        if (sample == 12u) sky.Render(*command, camera);
        else sky.RenderPhysicalAtmosphere(*command, camera, irradiance, altitude, reflectance);
        command->EndRenderToTexture(*target);
        command->End();
        // 提出されなかった描画は読戻しで偶然一致しても合格にしない。
        const bool submitted = command->Submit();
        EXPECT_TRUE(submitted);
        if (!submitted) return;
        device->WaitIdle();
        // GPU完了後の線形色を全画素取得できたか。
        const bool read = device->ReadTexture(*target, results[sample], sizeof(results[sample]));
        EXPECT_TRUE(read);
        if (!read) return;
        test::RecordInfo(FSourceLoc::Current(), "physical_sky_ground_gpu case=%u rgba=(%.9g,%.9g,%.9g,%.9g)", sample, results[sample][0], results[sample][1], results[sample][2], results[sample][3]);
        for (u32 pixel = 0u; pixel < 4u; ++pixel) {
            EXPECT_EQ(results[sample][pixel * 4u + 3u], 1.0f);
            for (u32 component = 0u; component < 3u; ++component) {
                // 初期色・非有限値・画素間の不一致を検出する成分値。
                const f32 value = results[sample][pixel * 4u + component];
                EXPECT_TRUE(value >= 0.0f && value < 1.0f);
                EXPECT_NEAR(value, results[sample][component], 1.0e-6f);
            }
        }
    }
    for (u32 component = 0u; component < 3u; ++component) {
        // 太陽が真上なら有限の大気を通った反射は正。絶対精度は別の積分参照試験で判定する。
        EXPECT_TRUE(results[0][component] > 0.01f);
        // 最大消散0.044/km・最大散乱源0.019/km・地表輝度1/piから、0.5mの差は1.7e-5以下。
        EXPECT_NEAR(results[0][component], results[1][component], 2.0e-5f);
        EXPECT_NEAR(results[0][component], results[5][component], 2.0e-5f);
        EXPECT_EQ(results[2][component], 0.0f);
        EXPECT_NEAR(results[3][component], results[0][component] * 2.0f, 1.0e-6f);
        // 表面の接線は地球内部へ入らない。交点0を無条件に遮蔽するとここが消える。
        EXPECT_TRUE(results[4][component] > 0.0f);
        // 地平線の内側では地表反射が加わり、外側では反射率を変えても描画が変わらない。
        EXPECT_TRUE(results[6][component] - results[7][component] > 0.001f);
        EXPECT_EQ(results[8][component], results[9][component]);
        EXPECT_TRUE(results[8][component] > 0.0f);
        // 黒い地面までの散乱だけを比べ、微小高度が地表へ丸まる回帰を検出する。
        EXPECT_TRUE(results[11][component] > 0.0f);
        EXPECT_NEAR(results[10][component], results[11][component] * 5.0f, results[10][component] * 0.001f + 1.0e-10f);
    }
    // 従来の色指定経路は、地面を向けば設定した地面色のまま描く。
    const FVec3 artGround = sky.GroundColor();
    EXPECT_NEAR(results[12][0], artGround.x, 1.0e-6f);
    EXPECT_NEAR(results[12][1], artGround.y, 1.0e-6f);
    EXPECT_NEAR(results[12][2], artGround.z, 1.0e-6f);
    sky.Shutdown();
}
