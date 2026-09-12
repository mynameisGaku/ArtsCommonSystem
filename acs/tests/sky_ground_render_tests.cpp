// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "render/Sky.h"
#include "render/Atmosphere.h"
#include "render/IRhiDevice.h"
#include "math/Camera.h"
#include "foundation/Move.h"
#include "platform/FileSystem.h"
#include "platform/Time.h"
#include "container/Array.h"
#include "container/String.h"
#include <cstring>
#include <cmath>

using namespace acs;

#if !WITH_RENDER_DILIGENT
/** RawDX12の製品CSkyと同じ作成経路で、共有する物理式を検証する。 */
static constexpr const char* kPhysicalSkyProbeTarget = "cs_6_0";
#else
/** 他の描画基盤へのSM6接続は別途検証する。従来形式をこの試験だけで変更しない。 */
static constexpr const char* kPhysicalSkyProbeTarget = "cs_5_1";
#endif

// 倍精度値の指数が全て1ならNaNまたは無限大。近似比較がNaNを見逃すことを防ぐ。
static bool IsFiniteProbeValue_Internal(f64 value)
{
    // 数値変換せずIEEE 754の指数部分を調べる。
    u64 bits = 0u;
    ::memcpy(&bits,&value,sizeof(bits));
    return (bits & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
}

// 検査用の極小値・無限大・NaNを、数値計算による丸めを挟まず構成する。
static f32 ProbeFloatFromBits_Internal(u32 bits)
{
    // IEEE 754の表現をそのまま転記する。
    f32 value = 0.0f;
    ::memcpy(&value,&bits,sizeof(bits));
    return value;
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

// 独立した複合Gaussと適応Simpsonで照合した薄明の高度kmと太陽俯角度。高度は製品入力と同じfloat丸め。
static constexpr f32 kTwilightHeights[8] = {9.999f,10.001f,24.999f,25.001f,39.999f,40.001f,0.0f,0.0f};
// 太陽方向は角度から計算した後にfloatへ丸める。
static constexpr f64 kTwilightDepressions[8] = {4.5,4.5,6.0,6.0,7.0,7.0,1.15,1.17};
// 入射量(1,0.8f,0.6f)、鉛直上向き視線、地表反射なしの線形RGB。導出と収束記録はAtmosphereViewIntegralResearch.md。
static constexpr f64 kTwilightRadiance[8][3] = {{5.06155276567e-5,2.40263419402e-5,3.91648261205e-5},{5.06156961097e-5,2.40265287261e-5,3.91655693352e-5},{7.27381226837e-6,3.51013732328e-6,6.00519899557e-6},{7.27382542530e-6,3.51015470043e-6,6.00521747316e-6},{1.48402430398e-6,7.11428840054e-7,1.20477405915e-6},{1.48402441998e-6,7.11428970009e-7,1.20477459626e-6},{7.11539208024e-4,4.17741485401e-4,4.56221077879e-4},{7.04672328299e-4,4.12748453855e-4,4.51433589626e-4}};

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

// 鉛直光路のオゾン積分を、三角形の面積から求める。入力はkm。
static f64 ReferenceOzonePrimitive_Internal(f64 height)
{
    if (height <= 10.0) return 0.0;
    if (height <= 25.0) return (height-10.0)*(height-10.0)/30.0;
    if (height <= 40.0) return 15.0-(40.0-height)*(40.0-height)/30.0;
    return 15.0;
}

// 鉛直上向きの指数密度とオゾンを解析積分する。固定した中点則の再現値を正解にしない。
static f64 ReferenceVerticalExactTransmittance_Internal(f64 height, f64 distance, u32 channel)
{
    // 地表での散乱・吸収。単位はkmの逆数。
    constexpr f64 rayleigh[3] = {0.005802,0.013558,0.0331};
    constexpr f64 ozone[3] = {0.000650,0.001881,0.000085};
    // 短区間では指数関数同士の差を作らない。
    const f64 rayleighLength = -8.0*::exp(-height/8.0)*::expm1(-distance/8.0);
    const f64 mieLength = -1.2*::exp(-height/1.2)*::expm1(-distance/1.2);
    const f64 ozoneLength = ReferenceOzonePrimitive_Internal(height+distance)-ReferenceOzonePrimitive_Internal(height);
    return ::exp(-rayleigh[channel]*rayleighLength-0.0044*mieLength-ozone[channel]*ozoneLength);
}

// 太陽と視線が同じ鉛直方向の場合の単散乱を解析積分する。入射量1、距離km、円盤と地表反射なし。
static f64 ReferenceVerticalExactScattering_Internal(f64 height, u32 channel)
{
    // 単位距離の散乱係数と、前方散乱における正規化済みの位相値。
    constexpr f64 rayleigh[3] = {0.005802,0.013558,0.0331};
    constexpr f64 pi = 3.14159265358979323846;
    constexpr f64 phaseRayleigh = 3.0/(8.0*pi);
    constexpr f64 phaseMie = 1.8/(4.0*pi*0.2*0.2);
    // 指数密度の残り面積。視線と太陽の透過率の積は、散乱点によらず全区間の透過率になる。
    const f64 distance = 100.0-height;
    const f64 rayleighColumn = -8.0*::exp(-height/8.0)*::expm1(-distance/8.0);
    const f64 mieColumn = -1.2*::exp(-height/1.2)*::expm1(-distance/1.2);
    const f64 transmission = ReferenceVerticalExactTransmittance_Internal(height,distance,channel);
    return transmission*(rayleigh[channel]*phaseRayleigh*rayleighColumn+0.003996*phaseMie*mieColumn);
}

// 同方向の視線と太陽では透過率の積が一定になる。球面密度だけを細かい中点則で独立積分する。
static void ReferenceCurvedParallelScattering_Internal(f64 height, FVec3 direction, u32 divisions, f64 (&radiance)[3])
{
    // 実際の単精度入力を倍精度へ広げてから単位化する。入力のyをそのまま余弦とみなさない。
    const f64 directionLength = ::sqrt(static_cast<f64>(direction.x)*direction.x+static_cast<f64>(direction.y)*direction.y+static_cast<f64>(direction.z)*direction.z);
    const f64 cosine = direction.y/directionLength;
    const f64 radius = 6360.0+height;
    const f64 projection = radius*cosine;
    const f64 topDifference = (6460.0-radius)*(6460.0+radius);
    const f64 root = ::sqrt(projection*projection+topDifference);
    const f64 distance = projection >= 0.0 ? topDifference/(root+projection) : root-projection;
    const f64 width = distance/static_cast<f64>(divisions);
    // 製品の密度変換・求積点・透過率関数を使わない。
    f64 rayleighColumn = 0.0;
    f64 mieColumn = 0.0;
    f64 ozoneColumn = 0.0;
    for (u32 index = 0u; index < divisions; ++index) {
        const f64 along = (static_cast<f64>(index)+0.5)*width;
        const f64 sampleHeight = ::sqrt(radius*radius+along*(along+2.0*projection))-6360.0;
        rayleighColumn += ::exp(-sampleHeight/8.0)*width;
        mieColumn += ::exp(-sampleHeight/1.2)*width;
        const f64 ozoneDensity = 1.0-::fabs(sampleHeight-25.0)/15.0;
        ozoneColumn += (ozoneDensity > 0.0 ? ozoneDensity : 0.0)*width;
    }
    // 位相の余弦は視線と太陽が同じなので1。鉛直方向に対するcosineとは別の量。
    constexpr f64 rayleigh[3] = {0.005802,0.013558,0.0331};
    constexpr f64 ozone[3] = {0.000650,0.001881,0.000085};
    constexpr f64 phaseRayleigh = 3.0/(8.0*3.14159265358979323846);
    constexpr f64 phaseMie = 1.8/(4.0*3.14159265358979323846*0.2*0.2);
    for (u32 channel = 0u; channel < 3u; ++channel) {
        const f64 transmission = ::exp(-rayleigh[channel]*rayleighColumn-0.0044*mieColumn-ozone[channel]*ozoneColumn);
        radiance[channel] = transmission*(rayleigh[channel]*phaseRayleigh*rayleighColumn+0.003996*phaseMie*mieColumn);
    }
}

// 地平線と最接近点を持つ光路を通常C++で照合し、鉛直だけ合う積分を合格にしない。
ACS_TEST(Atmosphere, CpuViewScatteringMatchesCurvedParallelIntegral)
{
    // 高度kmと天頂方向に対する余弦。地表を横切る視線は含めない。
    constexpr f64 heights[] = {0.0,0.0,0.0,20.0,80.0};
    constexpr f64 cosines[] = {0.0,0.05,0.2,-0.05,-0.14};
    for (u32 sample = 0u; sample < 5u; ++sample) {
        const FVec3 direction{static_cast<f32>(::sqrt(1.0-cosines[sample]*cosines[sample])),static_cast<f32>(cosines[sample]),0.0f};
        f64 expected[3]{};
        f64 refined[3]{};
        ReferenceCurvedParallelScattering_Internal(heights[sample],direction,65536u,expected);
        ReferenceCurvedParallelScattering_Internal(heights[sample],direction,131072u,refined);
        FAtmosphereParams parameters{};
        parameters.sun_dir = direction;
        parameters.sun_intensity = FVec3{1.0f,1.0f,1.0f};
        parameters.ground_albedo = FVec3{};
        const FVec3 actual = CAtmosphere::EvaluateSkyRadiance(static_cast<f32>(heights[sample]*1000.0),direction,parameters);
        const f32 channels[] = {actual.x,actual.y,actual.z};
        for (u32 channel = 0u; channel < 3u; ++channel) {
            test::RecordInfo(FSourceLoc::Current(),"curved_view_cpu sample=%u channel=%u actual=%.12g expected=%.12g",sample,channel,channels[channel],refined[channel]);
            EXPECT_TRUE(IsFiniteProbeValue_Internal(channels[channel]));
            EXPECT_TRUE(IsFiniteProbeValue_Internal(refined[channel]));
            EXPECT_TRUE(refined[channel] > 0.0 && channels[channel] > 0.0f);
            EXPECT_TRUE(::fabs(expected[channel]-refined[channel]) <= 1.0e-6*refined[channel]);
            EXPECT_TRUE(::fabs(static_cast<f64>(channels[channel])-refined[channel]) <= 1.0e-3*refined[channel]);
        }
    }
}

// 少数点指定でも、鉛直解析形へ近づく方向に求積則の切替による段差を作らない。
ACS_TEST(Atmosphere, CpuLowOrderViewIntegralJoinsVerticalLimit)
{
    // 微小な水平成分だけを変え、ほぼ鉛直の既知積分へ近づける。
    constexpr f32 horizontal[] = {0.0f,0.00001f,0.0001f,0.001f};
    constexpr u32 orders[] = {1u,3u,9u};
    for (u32 order : orders) {
        for (f32 x : horizontal) {
            const FVec3 direction{x,static_cast<f32>(::sqrt(1.0-static_cast<f64>(x)*x)),0.0f};
            f64 expected[3]{};
            ReferenceCurvedParallelScattering_Internal(0.0,direction,131072u,expected);
            FAtmosphereParams parameters{};
            parameters.sun_dir = direction;
            parameters.sun_intensity = FVec3{1.0f,1.0f,1.0f};
            parameters.ground_albedo = FVec3{};
            parameters.sun_steps = order;
            const FVec3 actual = CAtmosphere::EvaluateSkyRadiance(0.0f,direction,parameters);
            const f32 channels[] = {actual.x,actual.y,actual.z};
            for (u32 channel = 0u; channel < 3u; ++channel) {
                test::RecordInfo(FSourceLoc::Current(),"vertical_join_cpu order=%u x=%g channel=%u actual=%.12g expected=%.12g",order,x,channel,channels[channel],expected[channel]);
                EXPECT_TRUE(IsFiniteProbeValue_Internal(channels[channel]));
                EXPECT_TRUE(::fabs(static_cast<f64>(channels[channel])-expected[channel]) <= 1.0e-5*expected[channel]);
            }
        }
    }
}

// 既存の視線近似を期待値へ写さず、通常C++公開APIの単散乱を連続積分の解析解と比べる。
ACS_TEST(Atmosphere, CpuViewScatteringMatchesVerticalAnalyticIntegral)
{
    // 地表から上端まで。CPUにはmで渡し、参照だけkmへ換算する。
    constexpr f32 heights[] = {0.0f,1200.0f,8000.0f,10000.0f,25000.0f,40000.0f,80000.0f,90000.0f,99937.5f,100000.0f};
    // 標準の32点と既存係数試験の50点でも、連続積分の期待値は変わらない。
    constexpr u32 budgets[] = {32u,50u};
    // 色ごとの入射量。倍精度参照にも実際の単精度入力を広げて使う。
    constexpr f32 incident[] = {1.0f,0.8f,0.6f};
    FAtmosphereParams parameters{};
    parameters.sun_dir = FVec3{0.0f,1.0f,0.0f};
    parameters.sun_intensity = FVec3{incident[0],incident[1],incident[2]};
    parameters.ground_albedo = FVec3{};
    parameters.sun_steps = 8u;
    for (u32 budget : budgets) {
        parameters.ray_steps = budget;
        for (f32 height : heights) {
            // 現製品の公開関数を評価する。
            const FVec3 radiance = CAtmosphere::EvaluateSkyRadiance(height,FVec3{0.0f,1.0f,0.0f},parameters);
            const f32 channels[] = {radiance.x,radiance.y,radiance.z};
            for (u32 channel = 0u; channel < 3u; ++channel) {
                // 微小な高高度の光も、絶対誤差の下限を置いて0に潰すことを許さない。
                const f64 expected = incident[channel]*ReferenceVerticalExactScattering_Internal(static_cast<f64>(height)/1000.0,channel);
                test::RecordInfo(FSourceLoc::Current(),"view_cpu altitude_m=%g steps=%u channel=%u actual=%.12g expected=%.12g",height,budget,channel,channels[channel],expected);
                EXPECT_TRUE(IsFiniteProbeValue_Internal(channels[channel]));
                EXPECT_TRUE(IsFiniteProbeValue_Internal(expected));
                if (height == 100000.0f) EXPECT_EQ(channels[channel],0.0f);
                else {
                    EXPECT_TRUE(channels[channel] > 0.0f && expected > 0.0);
                    EXPECT_TRUE(::fabs(static_cast<f64>(channels[channel])-expected) <= 1.0e-3*expected);
                }
            }
        }
    }
    // 0点指定を1点へ補正する既存の公開契約は、精度の試験とは分離する。
    parameters.ray_steps = 0u;
    const FVec3 zeroBudget = CAtmosphere::EvaluateSkyRadiance(0.0f,FVec3{0.0f,1.0f,0.0f},parameters);
    parameters.ray_steps = 1u;
    const FVec3 oneBudget = CAtmosphere::EvaluateSkyRadiance(0.0f,FVec3{0.0f,1.0f,0.0f},parameters);
    EXPECT_EQ(zeroBudget.x,oneBudget.x);
    EXPECT_EQ(zeroBudget.y,oneBudget.y);
    EXPECT_EQ(zeroBudget.z,oneBudget.z);
}

// 太陽が届かない手前の空気も、奥の散乱光を減衰させる。二視点の関係を独立した透過率で検査する。
ACS_TEST(Atmosphere, CpuShadowedViewSegmentStillAttenuatesDistantScattering)
{
    // この太陽方向では影の上端が約7.965kmなので、0〜6kmは全て影の中になる。
    FAtmosphereParams parameters{};
    parameters.sun_dir = FVec3{static_cast<f32>(::sqrt(1.0-0.05*0.05)),-0.05f,0.0f};
    parameters.sun_intensity = FVec3{1.0f,0.8f,0.6f};
    parameters.ground_albedo = FVec3{};
    parameters.sun_steps = 8u;
    // 旧方式の共通部分を100m幅へそろえ、点配置の差を影の消散と取り違えない。
    parameters.ray_steps = 1000u;
    const FVec3 low = CAtmosphere::EvaluateSkyRadiance(0.0f,FVec3{0.0f,1.0f,0.0f},parameters);
    parameters.ray_steps = 940u;
    const FVec3 high = CAtmosphere::EvaluateSkyRadiance(6000.0f,FVec3{0.0f,1.0f,0.0f},parameters);
    const f32 lowChannels[] = {low.x,low.y,low.z};
    const f32 highChannels[] = {high.x,high.y,high.z};
    for (u32 channel = 0u; channel < 3u; ++channel) {
        // 二視点間に散乱源はないため、L(0)=T(0,6) L(6)となる。
        const f64 transmission = ReferenceVerticalExactTransmittance_Internal(0.0,6.0,channel);
        const f64 expected = highChannels[channel]*transmission;
        test::RecordInfo(FSourceLoc::Current(),"shadow_view channel=%u low=%.12g high=%.12g transmission=%.12g expected=%.12g",channel,lowChannels[channel],highChannels[channel],transmission,expected);
        EXPECT_TRUE(IsFiniteProbeValue_Internal(lowChannels[channel]));
        EXPECT_TRUE(IsFiniteProbeValue_Internal(highChannels[channel]));
        EXPECT_TRUE(lowChannels[channel] > 0.0f && highChannels[channel] > 0.0f);
        EXPECT_TRUE(::fabs(static_cast<f64>(lowChannels[channel])-expected) <= 1.0e-3*expected);
    }
}

// 地表上から真下を向く光路は長さ0。黒い地表から地下の散乱光を返してはいけない。
ACS_TEST(Atmosphere, CpuGroundSurfaceDoesNotIntegrateThroughPlanet)
{
    // 地面だけを照らす太陽と、反射しない地表。空気中の有限距離は含まれない。
    FAtmosphereParams parameters{};
    parameters.sun_dir = FVec3{0.0f,1.0f,0.0f};
    parameters.sun_intensity = FVec3{1.0f,1.0f,1.0f};
    parameters.ground_albedo = FVec3{};
    const FVec3 actual = CAtmosphere::EvaluateSkyRadiance(0.0f,FVec3{0.0f,-1.0f,0.0f},parameters);
    test::RecordInfo(FSourceLoc::Current(),"surface_down_cpu actual=(%.12g,%.12g,%.12g)",actual.x,actual.y,actual.z);
    EXPECT_EQ(actual.x,0.0f);
    EXPECT_EQ(actual.y,0.0f);
    EXPECT_EQ(actual.z,0.0f);
}

// 最適化の影響を大気の求積から切り離し、製品の影判定を短いGPU検査で先に実行する。
ACS_TEST(Atmosphere, GpuShadowCoefficientsPreserveGrazingInterval)
{
    FString source = ReadPhysicalSkyShader_Internal();
    EXPECT_TRUE(source.Size() > 0u);
    if (source.Size() == 0u) return;
    source.Append(R"(
RWTexture2D<float4> shadowCoefficientsOutput : register(u0);
[numthreads(1,1,1)]
void CSShadowCoefficientsProbe(uint3 id : SV_DispatchThreadID) {
    float4 coefficients = float4(0.0,0.0,0.0,0.0);
    float2 interval = PhysicalViewShadowIntervalWithCoefficients(physical_params.y,camera_pos.xyz,sun_dir.xyz,physical_params.w,coefficients);
    shadowCoefficientsOutput[uint2(0,0)] = float4(interval,0.0,1.0);
    shadowCoefficientsOutput[uint2(1,0)] = coefficients;
    shadowCoefficientsOutput[uint2(2,0)] = camera_pos;
    shadowCoefficientsOutput[uint2(3,0)] = sun_dir;
    shadowCoefficientsOutput[uint2(4,0)] = physical_params;
}
)");
    // GPUがない場合を合格にしない。
    FDeviceConfig configuration{};
    auto device = CreateRhiDevice(configuration);
    EXPECT_TRUE(device.IsOk());
    if (device.IsErr()) return;
    // 現在の共通シェーダーと同じSM5.1で検査する。
    FShaderDesc shaderDescription{};
    shaderDescription.stage = EShaderStage::Compute;
    shaderDescription.hlsl_source = source.Data();
    shaderDescription.entry_point = "CSShadowCoefficientsProbe";
    shaderDescription.target = kPhysicalSkyProbeTarget;
    auto shader = CreateRhiShader(*device.Value(),shaderDescription);
    EXPECT_TRUE(shader.IsOk());
    if (shader.IsErr()) return;
    // 入力の配置は製品のまま、検査出力だけを結び付ける。
    FComputePipelineDesc pipelineDescription{};
    pipelineDescription.cs = shader.Value().Get();
    pipelineDescription.cbuffer_slots = 1u;
    pipelineDescription.cbuffer_names[0] = "CSky";
    pipelineDescription.uav_slots = 1u;
    pipelineDescription.uav_names[0] = "shadowCoefficientsOutput";
    auto pipeline = CreateRhiComputePipeline(*device.Value(),pipelineDescription);
    EXPECT_TRUE(pipeline.IsOk());
    if (pipeline.IsErr()) return;
    // 区間・係数・実際の入力を別画素へ出し、半精度や表示変換を挟まない。
    FTextureDesc textureDescription{};
    textureDescription.width = 5u;
    textureDescription.height = 1u;
    textureDescription.format = EFormat::R32G32B32A32_Float;
    textureDescription.is_uav = true;
    auto texture = CreateRhiTexture(*device.Value(),textureDescription);
    EXPECT_TRUE(texture.IsOk());
    if (texture.IsErr()) return;
    // 行5が太陽方向、行13が高度、行14が入射量。
    FVec4 constants[16]{};
    constants[4] = FVec4{0.0f,1.0f,0.0f,0.0f};
    constants[5] = FVec4{0.0f,1.0f,0.0f,0.0f};
    constants[14] = FVec4{1.0f,0.8f,0.6f,0.0f};
    FBufferDesc bufferDescription{};
    bufferDescription.size = sizeof(constants);
    bufferDescription.usage = EBufferUsage::Uniform;
    bufferDescription.cpu_writable = true;
    auto buffer = CreateRhiBuffer(*device.Value(),bufferDescription);
    EXPECT_TRUE(buffer.IsOk());
    if (buffer.IsErr()) return;
    // 条件ごとに提出・完了・読戻しを確認する。
    auto command = CreateRhiCommandList(*device.Value());
    EXPECT_TRUE(command.IsOk());
    if (command.IsErr()) return;
    // 隣接する二入力の真の判別式は正と負。丸めで符号が反転する反例を保持する。
    constexpr f32 heights[2] = {97.52274322509766f,97.52275848388672f};
    constexpr f32 sunY[2] = {-0.07742907106876373f,-0.07742909342050552f};
    constants[4] = FVec4{-0.012026628479361534f,-0.15485814213752747f,0.9878634810447693f,0.0f};
    constants[13].w = 2000.0f;
    for (u32 sample = 0u; sample < 2u; ++sample) {
        constants[5] = FVec4{0.9969978332519531f,sunY[sample],0.0f,0.0f};
        constants[13].y = heights[sample];
        buffer.Value()->Update(constants,sizeof(constants));
        command.Value()->Begin();
        command.Value()->SetComputePipeline(*pipeline.Value());
        command.Value()->SetConstantBuffer(0u,*buffer.Value());
        command.Value()->BindUav(0u,*texture.Value());
        command.Value()->Dispatch(1u,1u,1u);
        command.Value()->End();
        const bool submitted = command.Value()->Submit();
        EXPECT_TRUE(submitted);
        if (!submitted) return;
        device.Value()->WaitIdle();
        f32 values[20]{};
        const bool read = device.Value()->ReadTexture(*texture.Value(),values,sizeof(values));
        EXPECT_TRUE(read);
        if (!read) return;
        test::RecordInfo(FSourceLoc::Current(),"shadow_isolated_gpu sample=%u begin=%.12g end=%.12g a=%.12g b=%.12g c=%.12g D=%.12g",sample,values[0],values[1],values[4],values[5],values[6],values[7]);
        test::RecordInfo(FSourceLoc::Current(),"shadow_inputs_gpu camera=(%.17g,%.17g,%.17g,%.17g) sun=(%.17g,%.17g,%.17g,%.17g) physical=(%.17g,%.17g,%.17g,%.17g)",values[8],values[9],values[10],values[11],values[12],values[13],values[14],values[15],values[16],values[17],values[18],values[19]);
        EXPECT_EQ(values[3],1.0f);
        for (u32 index = 0u; index < 8u; ++index) EXPECT_TRUE(IsFiniteProbeValue_Internal(values[index]));
        if (sample == 0u) {
            EXPECT_TRUE(values[7] > 0.0f);
            EXPECT_TRUE(::fabs(static_cast<f64>(values[0])-999.821317034506) <= 0.001);
            EXPECT_TRUE(::fabs(static_cast<f64>(values[1])-1000.17875073943) <= 0.001);
        } else {
            EXPECT_TRUE(values[7] < 0.0f);
            EXPECT_EQ(values[0],values[1]);
        }
    }
}


// 加算の残差をGPUが保持するかを、空や影の式から切り離して確認する。
ACS_TEST(Atmosphere, GpuCompensatedSumPreservesDynamicResiduals)
{
    FString source = ReadPhysicalSkyShader_Internal();
    EXPECT_TRUE(source.Size() > 0u);
    if (source.Size() == 0u) return;
    source.Append(R"(
RWTexture2D<float4> sumOutput : register(u0);
[numthreads(1,1,1)]
void CSSumProbe(uint3 id : SV_DispatchThreadID) {
    precise float2 forward = PhysicalCompensatedSum(camera_pos.x,camera_pos.y);
    precise float2 backward = PhysicalCompensatedSum(camera_pos.y,camera_pos.x);
    sumOutput[uint2(0,0)] = float4(forward,backward);
}
)");
    // GPUがない場合を合格にしない。
    FDeviceConfig configuration{};
    auto device = CreateRhiDevice(configuration);
    EXPECT_TRUE(device.IsOk());
    if (device.IsErr()) return;
    // 現在の共通シェーダーと同じSM5.1で検査する。
    FShaderDesc shaderDescription{};
    shaderDescription.stage = EShaderStage::Compute;
    shaderDescription.hlsl_source = source.Data();
    shaderDescription.entry_point = "CSSumProbe";
    shaderDescription.target = kPhysicalSkyProbeTarget;
    auto shader = CreateRhiShader(*device.Value(),shaderDescription);
    EXPECT_TRUE(shader.IsOk());
    if (shader.IsErr()) return;
    // 入力の配置は製品のまま、検査出力だけを結び付ける。
    FComputePipelineDesc pipelineDescription{};
    pipelineDescription.cs = shader.Value().Get();
    pipelineDescription.cbuffer_slots = 1u;
    pipelineDescription.cbuffer_names[0] = "CSky";
    pipelineDescription.uav_slots = 1u;
    pipelineDescription.uav_names[0] = "sumOutput";
    auto pipeline = CreateRhiComputePipeline(*device.Value(),pipelineDescription);
    EXPECT_TRUE(pipeline.IsOk());
    if (pipeline.IsErr()) return;
    // 主値と残差を両入力順で読み、途中値の出力によって最適化条件を変えない。
    FTextureDesc textureDescription{};
    textureDescription.width = 1u;
    textureDescription.height = 1u;
    textureDescription.format = EFormat::R32G32B32A32_Float;
    textureDescription.is_uav = true;
    auto texture = CreateRhiTexture(*device.Value(),textureDescription);
    EXPECT_TRUE(texture.IsOk());
    if (texture.IsErr()) return;
    // 行4の二成分だけを動的な加算入力として使う。
    FVec4 constants[16]{};
    FBufferDesc bufferDescription{};
    bufferDescription.size = sizeof(constants);
    bufferDescription.usage = EBufferUsage::Uniform;
    bufferDescription.cpu_writable = true;
    auto buffer = CreateRhiBuffer(*device.Value(),bufferDescription);
    EXPECT_TRUE(buffer.IsOk());
    if (buffer.IsErr()) return;
    // 条件ごとに提出・完了・読戻しを確認する。
    auto command = CreateRhiCommandList(*device.Value());
    EXPECT_TRUE(command.IsOk());
    if (command.IsErr()) return;
    // 影の反例を構成する和と、符号反転・大きさの差・完全な相殺を含める。定数畳込みで代用しない。
    constexpr f32 inputs[10][2] = {{9510.685546875f,1240489.25f},{-0.07648935168981552f,9.763745367763477e-12f},{0.0009312106994912028f,0.15439322590827942f},{1.0f,2.98023223876953125e-8f},{-1.0f,-2.98023223876953125e-8f},{1.0e10f,1.0f},{-1.0e10f,1.0f},{1.0f,-1.0f},{0.0f,0.25f},{-9510.685546875f,-1240489.25f}};
    for (u32 sample = 0u; sample < 10u; ++sample) {
        constants[4] = FVec4{inputs[sample][0],inputs[sample][1],0.0f,0.0f};
        buffer.Value()->Update(constants,sizeof(constants));
        command.Value()->Begin();
        command.Value()->SetComputePipeline(*pipeline.Value());
        command.Value()->SetConstantBuffer(0u,*buffer.Value());
        command.Value()->BindUav(0u,*texture.Value());
        command.Value()->Dispatch(1u,1u,1u);
        command.Value()->End();
        const bool submitted = command.Value()->Submit();
        EXPECT_TRUE(submitted);
        if (!submitted) return;
        device.Value()->WaitIdle();
        f32 values[4]{};
        const bool read = device.Value()->ReadTexture(*texture.Value(),values,sizeof(values));
        EXPECT_TRUE(read);
        if (!read) return;
        // 非零の期待残差は倍精度の分解能より十分大きい。主値と残差の組を入力二項の倍精度加算と照合する。
        const f64 expected = static_cast<f64>(inputs[sample][0])+static_cast<f64>(inputs[sample][1]);
        test::RecordInfo(FSourceLoc::Current(),"sum_gpu sample=%u forward=(%.17g,%.17g) backward=(%.17g,%.17g)",sample,values[0],values[1],values[2],values[3]);
        for (u32 index = 0u; index < 4u; ++index) EXPECT_TRUE(IsFiniteProbeValue_Internal(values[index]));
        EXPECT_EQ(static_cast<f64>(values[0])+static_cast<f64>(values[1]),expected);
        EXPECT_EQ(static_cast<f64>(values[2])+static_cast<f64>(values[3]),expected);
    }
}

// 分割予算を配る製品HLSLの計算を直接呼び、CPU側の反例と照合する。
ACS_TEST(Atmosphere, GpuAdaptivePriorityIgnoresConvergedColors)
{
    FString source = ReadPhysicalSkyShader_Internal();
    EXPECT_TRUE(source.Size() > 0u);
    if (source.Size() == 0u) return;
    source.Append(R"(
RWTexture2D<float4> priorityOutput : register(u0);
[numthreads(1,1,1)]
void CSAdaptivePriorityProbe(uint3 id : SV_DispatchThreadID) {
    float first = PhysicalAdaptiveErrorPriority(sun_color.xyz,camera_pos.xyz,sun_dir.xyz,physical_params.x);
    float second = PhysicalAdaptiveErrorPriority(sun_params.xyz,camera_pos.xyz,sun_dir.xyz,physical_params.x);
    float selected = second > first ? 1.0 : (first > 0.0 ? 0.0 : 2.0);
    priorityOutput[uint2(0,0)] = float4(first,second,selected,1.0);
}
)");
    // GPUがない場合を合格にしない。
    FDeviceConfig configuration{};
    auto device = CreateRhiDevice(configuration);
    EXPECT_TRUE(device.IsOk());
    if (device.IsErr()) return;
    // 現在の共通シェーダーと同じSM5.1で検査する。
    FShaderDesc shaderDescription{};
    shaderDescription.stage = EShaderStage::Compute;
    shaderDescription.hlsl_source = source.Data();
    shaderDescription.entry_point = "CSAdaptivePriorityProbe";
    shaderDescription.target = kPhysicalSkyProbeTarget;
    auto shader = CreateRhiShader(*device.Value(),shaderDescription);
    EXPECT_TRUE(shader.IsOk());
    if (shader.IsErr()) return;
    // 入力の配置は製品のまま、検査出力だけを結び付ける。
    FComputePipelineDesc pipelineDescription{};
    pipelineDescription.cs = shader.Value().Get();
    pipelineDescription.cbuffer_slots = 1u;
    pipelineDescription.cbuffer_names[0] = "CSky";
    pipelineDescription.uav_slots = 1u;
    pipelineDescription.uav_names[0] = "priorityOutput";
    auto pipeline = CreateRhiComputePipeline(*device.Value(),pipelineDescription);
    EXPECT_TRUE(pipeline.IsOk());
    if (pipeline.IsErr()) return;
    // 二候補の優先度と選択結果を、実際のGPUから読む。
    FTextureDesc textureDescription{};
    textureDescription.width = 1u;
    textureDescription.height = 1u;
    textureDescription.format = EFormat::R32G32B32A32_Float;
    textureDescription.is_uav = true;
    auto texture = CreateRhiTexture(*device.Value(),textureDescription);
    EXPECT_TRUE(texture.IsOk());
    if (texture.IsErr()) return;
    // 総量、全体誤差、二候補の誤差、許容値を明示的な入力にする。
    FVec4 constants[16]{};
    FBufferDesc bufferDescription{};
    bufferDescription.size = sizeof(constants);
    bufferDescription.usage = EBufferUsage::Uniform;
    bufferDescription.cpu_writable = true;
    auto buffer = CreateRhiBuffer(*device.Value(),bufferDescription);
    EXPECT_TRUE(buffer.IsOk());
    if (buffer.IsErr()) return;
    // 条件ごとに提出・完了・読戻しを確認する。
    auto command = CreateRhiCommandList(*device.Value());
    EXPECT_TRUE(command.IsOk());
    if (command.IsErr()) return;
    // 許容内のRを無視する反例、全色許容内、総量0の同率、色別の相対順位。
    const FVec4 totals[4] = {{1.0f,1.0f,1.0f,0.0f},{1.0f,1.0f,1.0f,0.0f},{1.0f,0.0f,1.0f,0.0f},{10.0f,1.0f,1.0f,0.0f}};
    const FVec4 total_errors[4] = {{9.0e-5f,2.0e-4f,0.0f,0.0f},{9.0e-5f,1.0e-4f,0.0f,0.0f},{0.0f,1.0e-8f,0.0f,0.0f},{2.0e-3f,3.0e-4f,1.0e-4f,0.0f}};
    const FVec4 first_errors[4] = {{9.0e-5f,0.0f,0.0f,0.0f},{9.0e-5f,0.0f,0.0f,0.0f},{0.0f,1.0e-8f,0.0f,0.0f},{2.0e-3f,0.0f,1.0e-4f,0.0f}};
    const FVec4 second_errors[4] = {{0.0f,1.0e-5f,0.0f,0.0f},{0.0f,1.0e-5f,0.0f,0.0f},{0.0f,1.0e-8f,0.0f,0.0f},{0.0f,3.0e-4f,0.0f,0.0f}};
    // 各二候補の期待優先度と、候補なしを2で表す選択結果。
    constexpr f32 expected[4][3] = {{0.0f,1.0e-5f,1.0f},{0.0f,0.0f,2.0f},{1.0f,1.0f,0.0f},{2.0e-4f,3.0e-4f,1.0f}};
    for (u32 sample = 0u; sample < 4u; ++sample) {
        constants[4] = totals[sample];
        constants[5] = total_errors[sample];
        constants[6] = first_errors[sample];
        constants[7] = second_errors[sample];
        constants[13].x = 1.0e-4f;
        buffer.Value()->Update(constants,sizeof(constants));
        command.Value()->Begin();
        command.Value()->SetComputePipeline(*pipeline.Value());
        command.Value()->SetConstantBuffer(0u,*buffer.Value());
        command.Value()->BindUav(0u,*texture.Value());
        command.Value()->Dispatch(1u,1u,1u);
        command.Value()->End();
        const bool submitted = command.Value()->Submit();
        EXPECT_TRUE(submitted);
        if (!submitted) return;
        device.Value()->WaitIdle();
        f32 values[4]{};
        const bool read = device.Value()->ReadTexture(*texture.Value(),values,sizeof(values));
        EXPECT_TRUE(read);
        if (!read) return;
        test::RecordInfo(FSourceLoc::Current(),"adaptive_priority_gpu sample=%u first=%.12g second=%.12g selected=%.12g",sample,values[0],values[1],values[2]);
        for (u32 index = 0u; index < 4u; ++index) EXPECT_TRUE(IsFiniteProbeValue_Internal(values[index]));
        EXPECT_NEAR(values[0],expected[sample][0],1.0e-9);
        EXPECT_NEAR(values[1],expected[sample][1],1.0e-9);
        EXPECT_EQ(values[2],expected[sample][2]);
        EXPECT_EQ(values[3],1.0f);
    }
}

// 通常C++の標準設定も独立した薄明の絶対値と比較し、高度ペアの共通偏りを検出する。
ACS_TEST(Atmosphere, CpuTwilightMatchesIndependentIntegrals)
{
    FAtmosphereParams parameters{};
    parameters.sun_intensity = FVec3{1.0f,0.8f,0.6f};
    parameters.ground_albedo = FVec3{};
    for (u32 sample = 0u; sample < 8u; ++sample) {
        const f64 angle = kTwilightDepressions[sample]*(3.14159265358979323846/180.0);
        parameters.sun_dir = FVec3{static_cast<f32>(::cos(angle)),static_cast<f32>(-::sin(angle)),0.0f};
        const FVec3 radiance = CAtmosphere::EvaluateSkyRadiance(kTwilightHeights[sample]*1000.0f,FVec3{0.0f,1.0f,0.0f},parameters);
        const f32 values[3] = {radiance.x,radiance.y,radiance.z};
        for (u32 channel = 0u; channel < 3u; ++channel) {
            const f64 expected = kTwilightRadiance[sample][channel];
            test::RecordInfo(FSourceLoc::Current(),"twilight_absolute_cpu sample=%u channel=%u actual=%.12g expected=%.12g",sample,channel,values[channel],expected);
            EXPECT_TRUE(IsFiniteProbeValue_Internal(values[channel]));
            EXPECT_TRUE(::fabs(static_cast<f64>(values[channel])-expected) <= 1.0e-3*expected);
        }
    }
}

// 製品のGPU入口を独立した解析形・積分値・連続性の条件で検査する。
ACS_TEST(Atmosphere, GpuViewScatteringMatchesIndependentIntegrals)
{
    // 実装を複製せず、検査入口だけを加える。
    FString source = ReadPhysicalSkyShader_Internal();
    EXPECT_TRUE(source.Size() > 0u);
    if (source.Size() == 0u) return;
    source.Append(R"(
RWTexture2D<float4> viewIntegralOutput : register(u0);
[numthreads(1,1,1)]
void CSViewIntegralProbe(uint3 id : SV_DispatchThreadID) {
    if (physical_params.z == 5.0) {
        float4 coefficients = float4(0.0,0.0,0.0,0.0);
        PhysicalViewShadowIntervalWithCoefficients(physical_params.y,camera_pos.xyz,sun_dir.xyz,physical_params.w,coefficients);
        viewIntegralOutput[uint2(0,0)] = coefficients;
        return;
    }
    if (physical_params.z >= 2.0) {
        // 特定の可視帯を切り出し、分割上限と残る絶対誤差を観測する。
        float3 error = float3(0.0,0.0,0.0);
        uint intervals = 0u;
        float3 integral = PhysicalViewDensityIntegral(float3(0.0,physical_params.y,0.0),camera_pos.xyz,sun_dir.xyz,1.0,sun_color.x,1.0,sun_color.y,sun_color.z,sun_color.w,sun_params.x,error,intervals);
        viewIntegralOutput[uint2(0,0)] = float4(physical_params.z >= 3.0 ? error : integral,float(intervals));
        return;
    }
    if (physical_params.z > 0.0) {
        viewIntegralOutput[uint2(0,0)] = float4(PhysicalViewShadowInterval(physical_params.y,camera_pos.xyz,sun_dir.xyz,physical_params.w),0.0,1.0);
        return;
    }
    float integration_error = 0.0;
    float3 radiance = EvaluatePhysicalSkyWithIntegrationError(camera_pos.xyz,sun_dir.xyz,physical_sun_intensity.xyz,physical_params.y,float3(0.0,0.0,0.0),integration_error);
    viewIntegralOutput[uint2(0,0)] = float4(radiance,1.0+integration_error);
}
)");
    // GPUがない場合を合格にしない。
    FDeviceConfig configuration{};
    auto device = CreateRhiDevice(configuration);
    EXPECT_TRUE(device.IsOk());
    if (device.IsErr()) return;
    // 現在の共通シェーダーと同じSM5.1で検査する。
    FShaderDesc shaderDescription{};
    shaderDescription.stage = EShaderStage::Compute;
    shaderDescription.hlsl_source = source.Data();
    shaderDescription.entry_point = "CSViewIntegralProbe";
    shaderDescription.target = kPhysicalSkyProbeTarget;
    auto shader = CreateRhiShader(*device.Value(),shaderDescription);
    EXPECT_TRUE(shader.IsOk());
    if (shader.IsErr()) return;
    // 入力の配置は製品のまま、検査出力だけを結び付ける。
    FComputePipelineDesc pipelineDescription{};
    pipelineDescription.cs = shader.Value().Get();
    pipelineDescription.cbuffer_slots = 1u;
    pipelineDescription.cbuffer_names[0] = "CSky";
    pipelineDescription.uav_slots = 1u;
    pipelineDescription.uav_names[0] = "viewIntegralOutput";
    auto pipeline = CreateRhiComputePipeline(*device.Value(),pipelineDescription);
    EXPECT_TRUE(pipeline.IsOk());
    if (pipeline.IsErr()) return;
    // 半精度や表示変換を挟まない1画素の出力。
    FTextureDesc textureDescription{};
    textureDescription.width = 1u;
    textureDescription.height = 1u;
    textureDescription.format = EFormat::R32G32B32A32_Float;
    textureDescription.is_uav = true;
    auto texture = CreateRhiTexture(*device.Value(),textureDescription);
    EXPECT_TRUE(texture.IsOk());
    if (texture.IsErr()) return;
    // 行5が太陽方向、行13が高度、行14が入射量。
    FVec4 constants[16]{};
    constants[4] = FVec4{0.0f,1.0f,0.0f,0.0f};
    constants[5] = FVec4{0.0f,1.0f,0.0f,0.0f};
    constants[14] = FVec4{1.0f,0.8f,0.6f,0.0f};
    FBufferDesc bufferDescription{};
    bufferDescription.size = sizeof(constants);
    bufferDescription.usage = EBufferUsage::Uniform;
    bufferDescription.cpu_writable = true;
    auto buffer = CreateRhiBuffer(*device.Value(),bufferDescription);
    EXPECT_TRUE(buffer.IsOk());
    if (buffer.IsErr()) return;
    // 条件ごとに提出・完了・読戻しを確認する。
    auto command = CreateRhiCommandList(*device.Value());
    EXPECT_TRUE(command.IsOk());
    if (command.IsErr()) return;
    // GPUはkmで受け取り、参照にも実際のfloat入力を渡す。
    constexpr f32 heights[] = {0.0f,1.2f,8.0f,10.0f,25.0f,40.0f,80.0f,90.0f,99.9375f,100.0f};
    constexpr f32 incident[] = {1.0f,0.8f,0.6f};
    for (f32 height : heights) {
        constants[13].y = height;
        buffer.Value()->Update(constants,sizeof(constants));
        command.Value()->Begin();
        command.Value()->SetComputePipeline(*pipeline.Value());
        command.Value()->SetConstantBuffer(0u,*buffer.Value());
        command.Value()->BindUav(0u,*texture.Value());
        command.Value()->Dispatch(1u,1u,1u);
        command.Value()->End();
        const bool submitted = command.Value()->Submit();
        EXPECT_TRUE(submitted);
        if (!submitted) return;
        device.Value()->WaitIdle();
        // RGBと書込み完了を示す値をそのまま読む。
        f32 values[4]{};
        const bool read = device.Value()->ReadTexture(*texture.Value(),values,sizeof(values));
        EXPECT_TRUE(read);
        if (!read) return;
        test::RecordInfo(FSourceLoc::Current(),"view_integration_gpu estimated_relative_error=%.9g",values[3]-1.0f);
        // 書込み未完了の0と、上限打切りによる推定誤差超過の両方を不合格にする。
        EXPECT_TRUE(values[3] >= 1.0f && values[3] <= 1.0f+1.0e-4f);
        for (u32 channel = 0u; channel < 3u; ++channel) {
            // 解析解は評価点数や製品の積分順序を使わない。
            const f64 expected = incident[channel]*ReferenceVerticalExactScattering_Internal(height,channel);
            test::RecordInfo(FSourceLoc::Current(),"view_gpu altitude_km=%g channel=%u actual=%.12g expected=%.12g",height,channel,values[channel],expected);
            EXPECT_TRUE(IsFiniteProbeValue_Internal(values[channel]));
            EXPECT_TRUE(IsFiniteProbeValue_Internal(expected));
            if (height == 100.0f) EXPECT_EQ(values[channel],0.0f);
            else {
                EXPECT_TRUE(values[channel] > 0.0f && expected > 0.0);
                EXPECT_TRUE(::fabs(static_cast<f64>(values[channel])-expected) <= 1.0e-3*expected);
            }
        }
    }
    // オゾン帯の前後2mを、両方とも地球の影になる太陽方向で比較する。
    constexpr f32 boundaryHeights[6] = {9.999f,10.001f,24.999f,25.001f,39.999f,40.001f};
    // 影の上端は各境界より高い。二視点の間には散乱源がなく、透過率の関係だけで比較できる。
    constexpr f64 depressions[3] = {4.5,6.0,7.0};
    f32 boundaryRadiance[6][3]{};
    for (u32 sample = 0u; sample < 6u; ++sample) {
        const f64 angle = depressions[sample/2u]*(3.14159265358979323846/180.0);
        constants[5] = FVec4{static_cast<f32>(::cos(angle)),static_cast<f32>(-::sin(angle)),0.0f,0.0f};
        constants[13].y = boundaryHeights[sample];
        buffer.Value()->Update(constants,sizeof(constants));
        command.Value()->Begin();
        command.Value()->SetComputePipeline(*pipeline.Value());
        command.Value()->SetConstantBuffer(0u,*buffer.Value());
        command.Value()->BindUav(0u,*texture.Value());
        command.Value()->Dispatch(1u,1u,1u);
        command.Value()->End();
        const bool submitted = command.Value()->Submit();
        EXPECT_TRUE(submitted);
        if (!submitted) return;
        device.Value()->WaitIdle();
        f32 values[4]{};
        const bool read = device.Value()->ReadTexture(*texture.Value(),values,sizeof(values));
        EXPECT_TRUE(read);
        if (!read) return;
        test::RecordInfo(FSourceLoc::Current(),"view_integration_gpu estimated_relative_error=%.9g",values[3]-1.0f);
        // 書込み未完了の0と、上限打切りによる推定誤差超過の両方を不合格にする。
        EXPECT_TRUE(values[3] >= 1.0f && values[3] <= 1.0f+1.0e-4f);
        for (u32 channel = 0u; channel < 3u; ++channel) {
            boundaryRadiance[sample][channel] = values[channel];
            const f64 expected = kTwilightRadiance[sample][channel];
            test::RecordInfo(FSourceLoc::Current(),"twilight_absolute_gpu sample=%u channel=%u actual=%.12g expected=%.12g",sample,channel,values[channel],expected);
            EXPECT_TRUE(::fabs(static_cast<f64>(values[channel])-expected) <= 1.0e-3*expected);
            EXPECT_TRUE(IsFiniteProbeValue_Internal(values[channel]));
            EXPECT_TRUE(values[channel] > 0.0f);
        }
    }
    for (u32 pair = 0u; pair < 3u; ++pair) {
        // 入力の単精度高度を倍精度へ広げてから、二視点間の物理的な透過率を求める。
        const u32 low = pair*2u;
        const f64 distance = static_cast<f64>(boundaryHeights[low+1u])-boundaryHeights[low];
        for (u32 channel = 0u; channel < 3u; ++channel) {
            const f64 transmission = ReferenceVerticalExactTransmittance_Internal(boundaryHeights[low],distance,channel);
            const f64 expected = boundaryRadiance[low+1u][channel]*transmission;
            test::RecordInfo(FSourceLoc::Current(),"shadow_boundary_gpu boundary=%u channel=%u low=%.12g high=%.12g expected=%.12g",pair,channel,boundaryRadiance[low][channel],boundaryRadiance[low+1u][channel],expected);
            EXPECT_TRUE(::fabs(static_cast<f64>(boundaryRadiance[low][channel])-expected) <= 1.0e-3*expected);
        }
    }
    // 薄明の各可視帯を単独評価し、細分上限に達している場所を数値で残す。
    for (u32 pair = 1u; pair < 3u; ++pair) {
        const f32 height = boundaryHeights[pair*2u];
        const f64 angle = (pair == 1u ? 6.0 : 7.0)*(3.14159265358979323846/180.0);
        constants[5] = FVec4{static_cast<f32>(::cos(angle)),static_cast<f32>(-::sin(angle)),0.0f,0.0f};
        constants[13].y = height;
        // 地球影を抜ける鉛直高度。float入力の太陽方向を倍精度で規格化して求める。
        const f64 sunLength = ::sqrt(static_cast<f64>(constants[5].x)*constants[5].x+static_cast<f64>(constants[5].y)*constants[5].y);
        const f64 shadowHeight = 6360.0*(sunLength/constants[5].x-1.0);
        for (u32 species = 0u; species < 2u; ++species) {
            constants[7].x = species == 0u ? 8.0f : 1.2f;
            for (u32 band = 0u; band < 2u; ++band) {
                const f32 begin = static_cast<f32>(Max(shadowHeight,band == 0u ? 25.0 : 40.0));
                const f32 end = band == 0u ? 40.0f : 100.0f;
                if (end <= begin) continue;
                constants[6] = FVec4{begin-height,begin,6360.0f+begin,end-begin};
                // 同じ区間の値と誤差を別々に読み、零に近い密度の相対値も隠さない。
                f32 diagnostic[2][4]{};
                for (u32 mode = 0u; mode < 2u; ++mode) {
                    constants[13].z = static_cast<f32>(mode+2u);
                    buffer.Value()->Update(constants,sizeof(constants));
                    command.Value()->Begin();
                    command.Value()->SetComputePipeline(*pipeline.Value());
                    command.Value()->SetConstantBuffer(0u,*buffer.Value());
                    command.Value()->BindUav(0u,*texture.Value());
                    command.Value()->Dispatch(1u,1u,1u);
                    command.Value()->End();
                    const bool submitted = command.Value()->Submit();
                    EXPECT_TRUE(submitted);
                    if (!submitted) return;
                    device.Value()->WaitIdle();
                    const bool read = device.Value()->ReadTexture(*texture.Value(),diagnostic[mode],sizeof(diagnostic[mode]));
                    EXPECT_TRUE(read);
                    if (!read) return;
                    EXPECT_TRUE(diagnostic[mode][3] >= 1.0f && diagnostic[mode][3] <= 64.0f);
                }
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    test::RecordInfo(FSourceLoc::Current(),"view_band_budget_gpu pair=%u species=%u band=%u channel=%u value=%.12g error=%.12g leaves=%.0f",pair,species,band,channel,diagnostic[0][channel],diagnostic[1][channel],diagnostic[0][3]);
                    EXPECT_TRUE(IsFiniteProbeValue_Internal(diagnostic[0][channel]) && IsFiniteProbeValue_Internal(diagnostic[1][channel]));
                }
            }
        }
    }
    constants[13].z = 0.0f;
    // 最初の分子標本を地球影の境界が横切る角度を掃引し、一標本の点灯・消灯による跳びを検出する。
    f32 previousRadiance[3]{};
    f64 largestJump[3]{};
    f64 jumpAngle[3]{};
    constants[13].y = 0.0f;
    for (u32 sample = 0u; sample <= 200u; ++sample) {
        // 0.0001度刻みの微小な太陽移動。媒質や入射量は固定する。
        const f64 degrees = 1.15+static_cast<f64>(sample)*0.0001;
        const f64 angle = degrees*(3.14159265358979323846/180.0);
        constants[5] = FVec4{static_cast<f32>(::cos(angle)),static_cast<f32>(-::sin(angle)),0.0f,0.0f};
        buffer.Value()->Update(constants,sizeof(constants));
        command.Value()->Begin();
        command.Value()->SetComputePipeline(*pipeline.Value());
        command.Value()->SetConstantBuffer(0u,*buffer.Value());
        command.Value()->BindUav(0u,*texture.Value());
        command.Value()->Dispatch(1u,1u,1u);
        command.Value()->End();
        const bool submitted = command.Value()->Submit();
        EXPECT_TRUE(submitted);
        if (!submitted) return;
        device.Value()->WaitIdle();
        f32 values[4]{};
        const bool read = device.Value()->ReadTexture(*texture.Value(),values,sizeof(values));
        EXPECT_TRUE(read);
        if (!read) return;
        test::RecordInfo(FSourceLoc::Current(),"view_integration_gpu estimated_relative_error=%.9g",values[3]-1.0f);
        // 書込み未完了の0と、上限打切りによる推定誤差超過の両方を不合格にする。
        EXPECT_TRUE(values[3] >= 1.0f && values[3] <= 1.0f+1.0e-4f);
        for (u32 channel = 0u; channel < 3u; ++channel) {
            EXPECT_TRUE(IsFiniteProbeValue_Internal(values[channel]));
            EXPECT_TRUE(values[channel] > 0.0f);
            if (sample > 0u && previousRadiance[channel] > 0.0f) {
                const f64 jump = ::fabs(static_cast<f64>(values[channel])-previousRadiance[channel])/previousRadiance[channel];
                if (jump > largestJump[channel]) {
                    largestJump[channel] = jump;
                    jumpAngle[channel] = degrees;
                }
            }
            if (sample == 0u || sample == 200u) {
                const f64 expected = kTwilightRadiance[sample == 0u ? 6u : 7u][channel];
                test::RecordInfo(FSourceLoc::Current(),"twilight_sweep_absolute_gpu sample=%u channel=%u actual=%.12g expected=%.12g",sample,channel,values[channel],expected);
                EXPECT_TRUE(::fabs(static_cast<f64>(values[channel])-expected) <= 1.0e-3*expected);
            }
            previousRadiance[channel] = values[channel];
        }
    }
    for (u32 channel = 0u; channel < 3u; ++channel) {
        test::RecordInfo(FSourceLoc::Current(),"solar_shadow_step_gpu channel=%u max_relative_jump=%.12g degrees=%.9g",channel,largestJump[channel],jumpAngle[channel]);
        EXPECT_TRUE(largestJump[channel] <= 1.0e-3);
    }
    // 鉛直以外も同じ製品入口で検証する。参照は球面密度を独立に積分し、二倍の分割数で収束を確認する。
    constexpr f64 curvedHeights[] = {0.0,0.0,0.0,20.0,80.0};
    constexpr f64 curvedCosines[] = {0.0,0.05,0.2,-0.05,-0.14};
    constants[14] = FVec4{1.0f,1.0f,1.0f,0.0f};
    for (u32 sample = 0u; sample < 5u; ++sample) {
        const FVec3 direction{static_cast<f32>(::sqrt(1.0-curvedCosines[sample]*curvedCosines[sample])),static_cast<f32>(curvedCosines[sample]),0.0f};
        constants[4] = FVec4{direction.x,direction.y,direction.z,0.0f};
        constants[5] = constants[4];
        constants[13].y = static_cast<f32>(curvedHeights[sample]);
        buffer.Value()->Update(constants,sizeof(constants));
        command.Value()->Begin();
        command.Value()->SetComputePipeline(*pipeline.Value());
        command.Value()->SetConstantBuffer(0u,*buffer.Value());
        command.Value()->BindUav(0u,*texture.Value());
        command.Value()->Dispatch(1u,1u,1u);
        command.Value()->End();
        const bool submitted = command.Value()->Submit();
        EXPECT_TRUE(submitted);
        if (!submitted) return;
        device.Value()->WaitIdle();
        f32 values[4]{};
        const bool read = device.Value()->ReadTexture(*texture.Value(),values,sizeof(values));
        EXPECT_TRUE(read);
        if (!read) return;
        test::RecordInfo(FSourceLoc::Current(),"view_integration_gpu estimated_relative_error=%.9g",values[3]-1.0f);
        // 書込み未完了の0と、上限打切りによる推定誤差超過の両方を不合格にする。
        EXPECT_TRUE(values[3] >= 1.0f && values[3] <= 1.0f+1.0e-4f);
        f64 expected[3]{};
        f64 refined[3]{};
        ReferenceCurvedParallelScattering_Internal(curvedHeights[sample],direction,65536u,expected);
        ReferenceCurvedParallelScattering_Internal(curvedHeights[sample],direction,131072u,refined);
        for (u32 channel = 0u; channel < 3u; ++channel) {
            test::RecordInfo(FSourceLoc::Current(),"curved_view_gpu sample=%u channel=%u actual=%.12g expected=%.12g",sample,channel,values[channel],refined[channel]);
            EXPECT_TRUE(IsFiniteProbeValue_Internal(values[channel]));
            EXPECT_TRUE(IsFiniteProbeValue_Internal(refined[channel]));
            EXPECT_TRUE(refined[channel] > 0.0 && values[channel] > 0.0f);
            EXPECT_TRUE(::fabs(expected[channel]-refined[channel]) <= 1.0e-6*refined[channel]);
            EXPECT_TRUE(::fabs(static_cast<f64>(values[channel])-refined[channel]) <= 1.0e-3*refined[channel]);
        }
    }
    // 外積を作った時点の丸めで、円柱の接触判別式が反転する独立監査の二つの入力。
    constexpr f32 shadowHeights[] = {97.52274322509766f,97.52275848388672f,0.125f};
    constexpr f32 shadowSunY[] = {-0.07742907106876373f,-0.07742909342050552f,0.0f};
    constants[4] = FVec4{-0.012026628479361534f,-0.15485814213752747f,0.9878634810447693f,0.0f};
    constants[13].z = 1.0f;
    constants[13].w = 2000.0f;
    for (u32 sample = 0u; sample < 3u; ++sample) {
        constants[5] = FVec4{0.9969978332519531f,shadowSunY[sample],0.0f,0.0f};
        constants[13].y = shadowHeights[sample];
        if (sample == 2u) {
            // 分割点の最後の加算が有限光路を1ulpだけ越える反例。
            constants[4] = FVec4{0.8796840906143188f,-0.000212153943721205f,0.4755585193634033f,0.0f};
            constants[5] = FVec4{1.0f,0.0f,0.0f,0.0f};
            constants[13].w = 90.3276138305664f;
        }
        buffer.Value()->Update(constants,sizeof(constants));
        command.Value()->Begin();
        command.Value()->SetComputePipeline(*pipeline.Value());
        command.Value()->SetConstantBuffer(0u,*buffer.Value());
        command.Value()->BindUav(0u,*texture.Value());
        command.Value()->Dispatch(1u,1u,1u);
        command.Value()->End();
        const bool submitted = command.Value()->Submit();
        EXPECT_TRUE(submitted);
        if (!submitted) return;
        device.Value()->WaitIdle();
        f32 values[4]{};
        const bool read = device.Value()->ReadTexture(*texture.Value(),values,sizeof(values));
        EXPECT_TRUE(read);
        if (!read) return;
        test::RecordInfo(FSourceLoc::Current(),"view_integration_gpu estimated_relative_error=%.9g",values[3]-1.0f);
        // 書込み未完了の0と、上限打切りによる推定誤差超過の両方を不合格にする。
        EXPECT_TRUE(values[3] >= 1.0f && values[3] <= 1.0f+1.0e-4f);
        test::RecordInfo(FSourceLoc::Current(),"shadow_interval_gpu sample=%u begin=%.12g end=%.12g",sample,values[0],values[1]);
        EXPECT_TRUE(IsFiniteProbeValue_Internal(values[0]) && IsFiniteProbeValue_Internal(values[1]));
        EXPECT_TRUE(values[0] >= 0.0f && values[1] <= constants[13].w);
        if (sample == 0u) {
            // FP32入力を倍精度へ広げた独立計算の根。km単位で1m以内、幅約357mの影を保持する。
            EXPECT_TRUE(::fabs(static_cast<f64>(values[0])-999.821317034506) <= 0.001);
            EXPECT_TRUE(::fabs(static_cast<f64>(values[1])-1000.17875073943) <= 0.001);
        } else {
            // 真の判別式は負。影なしの分割点自体は自由だが、誤った正の幅は許さない。
            EXPECT_EQ(values[0],values[1]);
        }
        // 最適化後に誤る中間値を、同じ製品関数の演算結果として取得する。
        constants[13].z = 5.0f;
        buffer.Value()->Update(constants,sizeof(constants));
        command.Value()->Begin();
        command.Value()->SetComputePipeline(*pipeline.Value());
        command.Value()->SetConstantBuffer(0u,*buffer.Value());
        command.Value()->BindUav(0u,*texture.Value());
        command.Value()->Dispatch(1u,1u,1u);
        command.Value()->End();
        const bool coefficientsSubmitted = command.Value()->Submit();
        EXPECT_TRUE(coefficientsSubmitted);
        if (!coefficientsSubmitted) return;
        device.Value()->WaitIdle();
        const bool coefficientsRead = device.Value()->ReadTexture(*texture.Value(),values,sizeof(values));
        EXPECT_TRUE(coefficientsRead);
        if (!coefficientsRead) return;
        test::RecordInfo(FSourceLoc::Current(),"shadow_coefficients_gpu sample=%u a=%.12g b=%.12g c=%.12g D=%.12g",sample,values[0],values[1],values[2],values[3]);
        for (u32 coefficient = 0u; coefficient < 4u; ++coefficient) EXPECT_TRUE(IsFiniteProbeValue_Internal(values[coefficient]));
        if (sample == 0u) EXPECT_TRUE(values[3] > 0.0f);
        if (sample == 1u) EXPECT_TRUE(values[3] < 0.0f);
        constants[13].z = 1.0f;
    }
    // 影を通らない視線を太陽の反対向きへ近づける。零幅の影分割が消えるだけで光を跳ばさない。
    constexpr f64 perturbations[] = {0.0,0.0001,0.0003,0.001};
    // 独立した適応Simpsonと複合Gaussで照合済みの絶対値。先頭は反平行の1次元積分でも確認した。
    constexpr f64 parallelReference[4][3] = {{0.032974054863,0.027415840825,0.041916772117},{0.032974054639,0.027415840739,0.041916772102},{0.032974055081,0.027415840560,0.041916770448},{0.032974078037,0.027415842614,0.041916739318}};
    const f64 sunX = ::sqrt(1.0-0.05*0.05);
    f32 parallelBaseline[3]{};
    constants[5] = FVec4{static_cast<f32>(sunX),0.05f,0.0f,0.0f};
    constants[13] = FVec4{0.0f,20.0f,0.0f,0.0f};
    for (u32 sample = 0u; sample < 4u; ++sample) {
        // 一次の変化は球対称性に直交するZ方向、二次の変化だけを視線と太陽の平面へ加える。
        const f64 epsilon = perturbations[sample];
        const f64 correction = -500.0*epsilon*epsilon/(6380.0*sunX);
        const f64 dx = -sunX-0.05*correction;
        const f64 dy = -0.05+sunX*correction;
        const f64 length = ::sqrt(dx*dx+dy*dy+epsilon*epsilon);
        constants[4] = FVec4{static_cast<f32>(dx/length),static_cast<f32>(dy/length),static_cast<f32>(epsilon/length),0.0f};
        // 通常C++も同じ入力と絶対値で確認し、見た目の連続性だけを合格条件にしない。
        FAtmosphereParams parameters{};
        parameters.sun_dir = FVec3{constants[5].x,constants[5].y,constants[5].z};
        parameters.sun_intensity = FVec3{1.0f,1.0f,1.0f};
        parameters.ground_albedo = FVec3{};
        const FVec3 cpu = CAtmosphere::EvaluateSkyRadiance(20000.0f,FVec3{constants[4].x,constants[4].y,constants[4].z},parameters);
        const f32 cpuChannels[] = {cpu.x,cpu.y,cpu.z};
        buffer.Value()->Update(constants,sizeof(constants));
        command.Value()->Begin();
        command.Value()->SetComputePipeline(*pipeline.Value());
        command.Value()->SetConstantBuffer(0u,*buffer.Value());
        command.Value()->BindUav(0u,*texture.Value());
        command.Value()->Dispatch(1u,1u,1u);
        command.Value()->End();
        const bool submitted = command.Value()->Submit();
        EXPECT_TRUE(submitted);
        if (!submitted) return;
        device.Value()->WaitIdle();
        f32 values[4]{};
        const bool read = device.Value()->ReadTexture(*texture.Value(),values,sizeof(values));
        EXPECT_TRUE(read);
        if (!read) return;
        test::RecordInfo(FSourceLoc::Current(),"view_integration_gpu estimated_relative_error=%.9g",values[3]-1.0f);
        // 書込み未完了の0と、上限打切りによる推定誤差超過の両方を不合格にする。
        EXPECT_TRUE(values[3] >= 1.0f && values[3] <= 1.0f+1.0e-4f);
        for (u32 channel = 0u; channel < 3u; ++channel) {
            if (sample == 0u) parallelBaseline[channel] = values[channel];
            test::RecordInfo(FSourceLoc::Current(),"parallel_join_gpu epsilon=%g channel=%u actual=%.12g baseline=%.12g",epsilon,channel,values[channel],parallelBaseline[channel]);
            EXPECT_TRUE(IsFiniteProbeValue_Internal(values[channel]));
            EXPECT_TRUE(values[channel] > 0.0f && parallelBaseline[channel] > 0.0f);
            EXPECT_TRUE(::fabs(static_cast<f64>(values[channel])-parallelBaseline[channel]) <= 1.0e-3*parallelBaseline[channel]);
            test::RecordInfo(FSourceLoc::Current(),"antiparallel_absolute sample=%u channel=%u gpu=%.12g cpu=%.12g expected=%.12g",sample,channel,values[channel],cpuChannels[channel],parallelReference[sample][channel]);
            EXPECT_TRUE(IsFiniteProbeValue_Internal(cpuChannels[channel]));
            EXPECT_TRUE(::fabs(static_cast<f64>(cpuChannels[channel])-parallelReference[sample][channel]) <= 1.0e-3*parallelReference[sample][channel]);
            EXPECT_TRUE(::fabs(static_cast<f64>(values[channel])-parallelReference[sample][channel]) <= 1.0e-3*parallelReference[sample][channel]);
        }
    }
}

// 通常C++の太陽透過率を解析解と比較し、濃い低高度を標本が見落とす回帰を防ぐ。
ACS_TEST(Atmosphere, CpuSunTransmittanceMatchesVerticalAnalyticIntegral)
{
    // 高度はm。地表、雲層、分子とオゾンが優勢な高度を含める。
    constexpr f32 altitudes[6] = {0.0f,1200.0f,8000.0f,10000.0f,25000.0f,40000.0f};
    for (u32 index = 0u; index < 6u; ++index) {
        // 公開APIから得る実製品の透過率。
        const FVec3 actual = SunTransmittanceAtAltitude(altitudes[index],FVec3{0.0f,1.0f,0.0f});
        // RGBを同じ参照計算へ渡す。
        const f32 channels[3] = {actual.x,actual.y,actual.z};
        for (u32 channel = 0u; channel < 3u; ++channel) {
            // mからkmへ換算し、独立した解析解と比べる。
            const f64 expected = ReferenceVerticalExactTransmittance_Internal(altitudes[index]*0.001,100.0-altitudes[index]*0.001,channel);
            EXPECT_TRUE(IsFiniteProbeValue_Internal(channels[channel]));
            EXPECT_TRUE(IsFiniteProbeValue_Internal(expected));
            test::RecordInfo(FSourceLoc::Current(),"mie_cpu altitude_m=%g channel=%u actual=%.9g expected=%.9g",altitudes[index],channel,channels[channel],expected);
            EXPECT_NEAR(channels[channel],expected,2.0e-5);
        }
    }
}

// 直線上の位置から球面高度を直接計算し、製品とは異なる細かい中点則で消散を積む。
static f64 ReferenceCurvedTransmittance_Internal(f64 height, f64 horizontal, f64 vertical, f64 distance, u32 channel, u32 steps, f64 horizontalOrigin = 0.0, f64 forwardOrigin = 0.0, f64 forward = 0.0)
{
    // kmの逆数で表した係数。
    constexpr f64 rayleigh[3] = {0.005802,0.013558,0.0331};
    constexpr f64 ozone[3] = {0.000650,0.001881,0.000085};
    // 密度の積分をそのまま光学的厚さへ集約する。
    const f64 width = distance/steps;
    f64 depth = 0.0;
    for (u32 sample = 0u; sample < steps; ++sample) {
        // 大きい地球中心座標を倍精度で評価する。製品の変数変換は使わない。
        const f64 t = (sample+0.5)*width;
        const f64 x = horizontalOrigin+horizontal*t;
        const f64 y = 6360.0+height+vertical*t;
        const f64 z = forwardOrigin+forward*t;
        const f64 altitude = ::sqrt(x*x+y*y+z*z)-6360.0;
        const f64 ozoneDensity = 1.0-::fabs(altitude-25.0)/15.0;
        depth += (rayleigh[channel]*::exp(-altitude/8.0)+0.0044*::exp(-altitude/1.2)+ozone[channel]*(ozoneDensity>0.0?ozoneDensity:0.0))*width*::sqrt(horizontal*horizontal+vertical*vertical+forward*forward);
    }
    return ::exp(-depth);
}

// 公開C++経路を水平・低い太陽・高高度の下向き光路で検査する。既存の遮蔽判定は別課題。
ACS_TEST(Atmosphere, CpuSunTransmittanceMatchesCurvedReference)
{
    // 高度kmと太陽方向の鉛直成分。全て地球に遮られず大気上端へ達する。
    constexpr FVec2 cases[] = {{0.0f,0.0f},{0.0f,0.02f},{1.2f,0.01f},{20.0f,-0.03f},{70.0f,-0.14f},{99.0f,-0.17f}};
    for (u32 sample = 0u; sample < sizeof(cases)/sizeof(cases[0]); ++sample) {
        // 入力自体を単精度で作り、参照にはその入力の方向を正規化して渡す。
        const FVec3 direction{static_cast<f32>(::sqrt(1.0-cases[sample].y*cases[sample].y)),cases[sample].y,0.0f};
        // 参照に使う単位方向。単精度入力の長さの誤差を正規化する。
        const f64 norm = ::sqrt(static_cast<f64>(direction.x)*direction.x+static_cast<f64>(direction.y)*direction.y);
        const f64 dx = direction.x/norm;
        const f64 dy = direction.y/norm;
        // 大気上端への交点を独立した倍精度の球面式から求める。
        const f64 radius = 6360.0+cases[sample].x;
        const f64 projection = radius*dy;
        const f64 distance = -projection+::sqrt(projection*projection+(6460.0-radius)*(6460.0+radius));
        // 公開APIの結果をRGBの配列として参照値と比較する。
        const FVec3 actual = SunTransmittanceAtAltitude(cases[sample].x*1000.0f,direction);
        const f32 channels[3] = {actual.x,actual.y,actual.z};
        for (u32 channel = 0u; channel < 3u; ++channel) {
            // 参照自身の収束を、評価点を倍にして確認する。
            const f64 expected = ReferenceCurvedTransmittance_Internal(cases[sample].x,dx,dy,distance,channel,32768u);
            const f64 refined = ReferenceCurvedTransmittance_Internal(cases[sample].x,dx,dy,distance,channel,65536u);
            EXPECT_TRUE(IsFiniteProbeValue_Internal(channels[channel]));
            EXPECT_TRUE(IsFiniteProbeValue_Internal(expected));
            EXPECT_TRUE(::fabs(expected-refined) <= 1.0e-7);
            test::RecordInfo(FSourceLoc::Current(),"cpu_curved_path case=%u channel=%u actual=%.9g expected=%.9g",sample,channel,channels[channel],expected);
            EXPECT_TRUE(::fabs(static_cast<f64>(channels[channel])-expected) <= 2.0e-4);
        }
    }
}

// 同一の実GPU条件で旧積分と現積分を交互に実行する。時間は診断値であり速度の合格基準ではない。
ACS_TEST(Atmosphere, OpticalPathGpuCostDiagnostic)
{
    // 旧8点則は計算量の比較専用。数値の正解としては使わない。
    FString source = ReadPhysicalSkyShader_Internal();
    EXPECT_TRUE(source.Size() > 0u);
    if (source.Size() == 0u) return;
    source.Append(R"(
RWTexture2D<float4> costOutput : register(u0);
float3 HistoricalMidpointCost(float3 origin, float3 direction, float distance) {
    // 旧式と現式で共通の地表遮蔽判定。
    float ground = PhysicalRaySphereNear(origin,direction,kPhysicalGroundRadiusKm);
    if (ground >= 0.0 && ground < distance) return 0.0;
    // 旧式の区間幅と光学的厚さ。
    float3 opticalDepth = 0.0;
    float width = distance/8.0;
    [loop]
    for (int index = 0; index < 8; ++index) {
        // 光路の中点で評価する球面高度と各媒質の出力。
        float altitude = PhysicalAltitude(origin+direction*((float(index)+0.5)*width));
        float3 rayleigh;
        float mie;
        float3 extinction;
        SamplePhysicalMedium(altitude,rayleigh,mie,extinction);
        opticalDepth += extinction*width;
    }
    return exp(-opticalDepth);
}
[numthreads(8,8,1)]
void CSCostProbe(uint3 id : SV_DispatchThreadID) {
    if (id.x >= 640 || id.y >= 360) return;
    // 画素ごとに高度と太陽方向を変え、定数の1光路だけへ最適化されることを防ぐ。
    float3 origin = float3(1600.0*((float(id.x)+0.5)/640.0-0.5),40.0*(float(id.y)+0.5)/360.0,0.0);
    // 地平線から天頂へ走査する方向と、大気上端への距離。
    float mu = (float(id.x)+0.5)/640.0;
    float3 direction = float3(sqrt(1.0-mu*mu),mu,0.0);
    float distance = PhysicalRaySphereOuter(origin,direction,kPhysicalTopRadiusKm);
    // 実行時に選んだ積分法の結果。
    float3 result;
    if (cloud_params1.x < 0.5) result = HistoricalMidpointCost(origin,direction,distance);
    else result = PhysicalTransmittance(origin,direction,distance);
    costOutput[id.xy] = float4(result,1.0);
}
)");
    // 現在の検証環境で実GPUを作成する。
    FDeviceConfig configuration{};
    auto device = CreateRhiDevice(configuration);
    EXPECT_TRUE(device.IsOk());
    if (device.IsErr()) return;
    // 製品HLSLと旧式比較を含む検査用の計算シェーダー。
    FShaderDesc shaderDescription{};
    shaderDescription.stage = EShaderStage::Compute;
    shaderDescription.hlsl_source = source.Data();
    shaderDescription.entry_point = "CSCostProbe";
    shaderDescription.target = kPhysicalSkyProbeTarget;
    auto shader = CreateRhiShader(*device.Value(),shaderDescription);
    EXPECT_TRUE(shader.IsOk());
    if (shader.IsErr()) return;
    // 製品の定数バッファと、比較用画像を結び付ける。
    FComputePipelineDesc pipelineDescription{};
    pipelineDescription.cs = shader.Value().Get();
    pipelineDescription.cbuffer_slots = 1u;
    pipelineDescription.cbuffer_names[0] = "CSky";
    pipelineDescription.uav_slots = 1u;
    pipelineDescription.uav_names[0] = "costOutput";
    auto pipeline = CreateRhiComputePipeline(*device.Value(),pipelineDescription);
    EXPECT_TRUE(pipeline.IsOk());
    if (pipeline.IsErr()) return;
    // 同一解像度・形式で旧式と現式を計算する出力先。
    FTextureDesc textureDescription{};
    textureDescription.width = 640u;
    textureDescription.height = 360u;
    textureDescription.format = EFormat::R32G32B32A32_Float;
    textureDescription.is_uav = true;
    auto texture = CreateRhiTexture(*device.Value(),textureDescription);
    EXPECT_TRUE(texture.IsOk());
    if (texture.IsErr()) return;
    // 行12の先頭だけで旧式と現式を切り替える。コンパイルと読戻しは計測区間外。
    FVec4 constants[16]{};
    FBufferDesc bufferDescription{};
    bufferDescription.size = sizeof(constants);
    bufferDescription.usage = EBufferUsage::Uniform;
    bufferDescription.cpu_writable = true;
    auto buffer = CreateRhiBuffer(*device.Value(),bufferDescription);
    EXPECT_TRUE(buffer.IsOk());
    if (buffer.IsErr()) return;
    // 各実行の記録と提出を行う命令列。
    auto command = CreateRhiCommandList(*device.Value());
    EXPECT_TRUE(command.IsOk());
    if (command.IsErr()) return;
    // 先に各2回動かした後、順番も交互にして各12回の最短・平均時間を記録する。
    f64 elapsed[2]{};
    f64 minimum[2] = {1.0e30,1.0e30};
    for (u32 round = 0u; round < 14u; ++round) {
        for (u32 entry = 0u; entry < 2u; ++entry) {
            // 各対の実行順を反転し、片方だけが常に先になる偏りを減らす。
            const u32 variant = entry^((round+1u)&1u);
            constants[12].x = static_cast<f32>(variant);
            buffer.Value()->Update(constants,sizeof(constants));
            command.Value()->Begin();
            command.Value()->SetComputePipeline(*pipeline.Value());
            command.Value()->SetConstantBuffer(0u,*buffer.Value());
            command.Value()->BindUav(0u,*texture.Value());
            command.Value()->Dispatch(80u,45u,1u);
            command.Value()->End();
            // CPUでの提出開始から、GPU完了待ちの復帰までを計る。
            const f64 begin = CClock::SecondsSinceStartup();
            const bool submitted = command.Value()->Submit();
            EXPECT_TRUE(submitted);
            if (!submitted) return;
            device.Value()->WaitIdle();
            // 準備・読戻しを除いた今回の経過時間。
            const f64 milliseconds = (CClock::SecondsSinceStartup()-begin)*1000.0;
            if (round >= 2u) {
                elapsed[variant] += milliseconds;
                if (milliseconds < minimum[variant]) minimum[variant] = milliseconds;
            }
        }
    }
    // 最後に生成した画像の全画素が有限か検査する。NaNを性能値だけで見逃さない。
    TArray<f32> values;
    values.SetNum(640u*360u*4u);
    // 読戻し失敗を未初期化の画素値で検査しない。
    const bool read = device.Value()->ReadTexture(*texture.Value(),values.GetData(),values.Num()*sizeof(f32));
    EXPECT_TRUE(read);
    if (!read) return;
    for (usize index = 0u; index < values.Num(); ++index) EXPECT_TRUE(IsFiniteProbeValue_Internal(values[index]));
    test::RecordInfo(FSourceLoc::Current(),"optical_cost backend=%s adapter=%s pixels=230400 measured=12 cpu_submit_wait_ms old_mean=%.6f new_mean=%.6f old_min=%.6f new_min=%.6f",device.Value()->BackendName(),device.Value()->AdapterName(),elapsed[0]/12.0,elapsed[1]/12.0,minimum[0],minimum[1]);
}

// 実製品の光路積分を鉛直解析解・球面参照・反転・区間分割と照合する。
ACS_TEST(Atmosphere, PhysicalTransmittanceMatchesIndependentOpticalPaths)
{
    // 現在の製品HLSLへ、結果だけを書き出す入口を追加する。
    FString source = ReadPhysicalSkyShader_Internal();
    EXPECT_TRUE(source.Size() > 0u);
    if (source.Size() == 0u) return;
    source.Append(R"(
RWTexture2D<float4> opticalProbeOutput : register(u0);
[numthreads(1,1,1)]
void CSOpticalProbe(uint3 id : SV_DispatchThreadID) {
    // 入力は全て実行時。方向の単精度丸めも出力して参照計算へ渡す。
    float3 origin = float3(0.0,physical_params.y,0.0);
    float3 direction = float3(sqrt(saturate(1.0-cloud_params0.y*cloud_params0.y)),cloud_params0.y,0.0);
    float distance = cloud_params0.x;
    opticalProbeOutput[uint2(0,0)] = float4(PhysicalTransmittance(origin,direction,distance),1.0);
    opticalProbeOutput[uint2(1,0)] = float4(PhysicalTransmittance(origin,direction,distance*0.5)*PhysicalTransmittance(origin+direction*(distance*0.5),direction,distance*0.5),1.0);
    opticalProbeOutput[uint2(2,0)] = float4(PhysicalTransmittance(origin+direction*distance,-direction,distance),1.0);
    opticalProbeOutput[uint2(3,0)] = float4(direction,distance);
}
)");
    // GPUが使えない場合は未検証として失敗する。
    FDeviceConfig configuration{};
    auto device = CreateRhiDevice(configuration);
    EXPECT_TRUE(device.IsOk());
    if (device.IsErr()) return;
    // 共通関数を含む実HLSLをSM5.1へコンパイルする。
    FShaderDesc shaderDescription{};
    shaderDescription.stage = EShaderStage::Compute;
    shaderDescription.hlsl_source = source.Data();
    shaderDescription.entry_point = "CSOpticalProbe";
    shaderDescription.target = kPhysicalSkyProbeTarget;
    auto shader = CreateRhiShader(*device.Value(),shaderDescription);
    EXPECT_TRUE(shader.IsOk());
    if (shader.IsErr()) return;
    // 製品の定数バッファと検査用出力だけを結び付ける。
    FComputePipelineDesc pipelineDescription{};
    pipelineDescription.cs = shader.Value().Get();
    pipelineDescription.cbuffer_slots = 1u;
    pipelineDescription.cbuffer_names[0] = "CSky";
    pipelineDescription.uav_slots = 1u;
    pipelineDescription.uav_names[0] = "opticalProbeOutput";
    auto pipeline = CreateRhiComputePipeline(*device.Value(),pipelineDescription);
    EXPECT_TRUE(pipeline.IsOk());
    if (pipeline.IsErr()) return;
    // 全体、二分割、反転、実方向の4画素を丸めずに記録する。
    FTextureDesc textureDescription{};
    textureDescription.width = 4u;
    textureDescription.height = 1u;
    textureDescription.format = EFormat::R32G32B32A32_Float;
    textureDescription.is_uav = true;
    auto texture = CreateRhiTexture(*device.Value(),textureDescription);
    EXPECT_TRUE(texture.IsOk());
    if (texture.IsErr()) return;
    // 行13が高度、行11が光路長と鉛直方向成分。
    FVec4 constants[16]{};
    FBufferDesc bufferDescription{};
    bufferDescription.size = sizeof(constants);
    bufferDescription.usage = EBufferUsage::Uniform;
    bufferDescription.cpu_writable = true;
    auto buffer = CreateRhiBuffer(*device.Value(),bufferDescription);
    EXPECT_TRUE(buffer.IsOk());
    if (buffer.IsErr()) return;
    // 同期してから次の入力へ進む命令列。
    auto command = CreateRhiCommandList(*device.Value());
    EXPECT_TRUE(command.IsOk());
    if (command.IsErr()) return;
    // 高度、鉛直方向、光路長。全てkm。添字14だけ地表へ入り、末尾は高度差が丸まる短い接線光路。
    constexpr FVec3 cases[] = {{0.0f,1.0f,100.0f},{0.0f,1.0f,0.001f},{0.0f,1.0f,1.2f},{1.2f,1.0f,98.8f},{10.0f,1.0f,15.0f},{25.0f,1.0f,15.0f},{40.0f,1.0f,60.0f},{100.0f,-1.0f,100.0f},{0.0f,0.02f,1000.0f},{0.0f,0.0f,1000.0f},{70.0f,-0.14f,1800.0f},{20.0f,-0.03f,400.0f},{0.0001f,-1.0f,0.0001f},{0.0f,1.0f,0.0f},{0.0f,-1.0f,10.0f},{25.0f,0.0f,0.11f},{25.0f,0.0f,0.1104f},{25.0f,0.0f,0.22f},{20.0f,0.0f,0.1f},{0.0f,0.0f,1.0e-15f}};
    for (u32 sample = 0u; sample < sizeof(cases)/sizeof(cases[0]); ++sample) {
        constants[13].y = cases[sample].x;
        constants[11] = FVec4{cases[sample].z,cases[sample].y,0.0f,0.0f};
        buffer.Value()->Update(constants,sizeof(constants));
        command.Value()->Begin();
        command.Value()->SetComputePipeline(*pipeline.Value());
        command.Value()->SetConstantBuffer(0u,*buffer.Value());
        command.Value()->BindUav(0u,*texture.Value());
        command.Value()->Dispatch(1u,1u,1u);
        command.Value()->End();
        // 提出失敗は数値0として比較しない。
        const bool submitted = command.Value()->Submit();
        EXPECT_TRUE(submitted);
        if (!submitted) return;
        device.Value()->WaitIdle();
        f32 values[16]{};
        const bool read = device.Value()->ReadTexture(*texture.Value(),values,sizeof(values));
        EXPECT_TRUE(read);
        if (!read) return;
        for (u32 index = 0u; index < 16u; ++index) EXPECT_TRUE(IsFiniteProbeValue_Internal(values[index]));
        for (u32 channel = 0u; channel < 3u; ++channel) {
            // 全体を通過する条件と、地表に遮られる条件を混ぜない。
            f64 expected = 0.0;
            if (sample != 14u) {
                if (values[12] == 0.0f) {
                    // 下向きの鉛直区間は低高度側からの解析積分へ反転する。
                    const f64 lowHeight = cases[sample].x+(values[13]<0.0f?-cases[sample].z:0.0f);
                    expected = ReferenceVerticalExactTransmittance_Internal(lowHeight,cases[sample].z,channel);
                } else {
                    expected = ReferenceCurvedTransmittance_Internal(cases[sample].x,values[12],values[13],cases[sample].z,channel,16384u);
                    // 参照の段数を倍にし、参照自身の未収束を見逃さない。
                    const f64 refined = ReferenceCurvedTransmittance_Internal(cases[sample].x,values[12],values[13],cases[sample].z,channel,32768u);
                    EXPECT_TRUE(::fabs(expected-refined) <= 1.0e-7);
                }
            }
            EXPECT_TRUE(IsFiniteProbeValue_Internal(expected));
            test::RecordInfo(FSourceLoc::Current(),"optical_path case=%u channel=%u actual=%.9g expected=%.9g split=%.9g reverse=%.9g",sample,channel,values[channel],expected,values[4u+channel],values[8u+channel]);
            // 曲がった高度変化を含む許容誤差は、1近傍の半精度の半ULPより小さくする。
            EXPECT_TRUE(::fabs(static_cast<f64>(values[channel])-expected) <= 2.0e-4);
            if (sample != 14u) {
                EXPECT_TRUE(::fabs(static_cast<f64>(values[channel])-values[4u+channel]) <= 2.0e-4);
                EXPECT_TRUE(::fabs(static_cast<f64>(values[channel])-values[8u+channel]) <= 2.0e-4);
            }
            // 鉛直では近似する必要がないため、より厳しく解析解へ照合する。
            if (values[12] == 0.0f) EXPECT_TRUE(::fabs(static_cast<f64>(values[channel])-expected) <= 2.0e-6);
        }
    }
}

// 外側から地表への入口を倍精度で求める。外向きは負値、内向きの地表・地中始点は0。
static f64 ReferenceGroundEntry_Internal(FVec3 origin, FVec3 direction)
{
    // 単精度の入力を、演算する前に倍精度へ広げる。
    const f64 x = origin.x;
    const f64 y = origin.y;
    const f64 z = origin.z;
    const f64 dx = direction.x;
    const f64 dy = direction.y;
    const f64 dz = direction.z;
    // 独立参照の球面二次式。製品の上下位ペア演算は使わない。
    const f64 a = dx*dx+dy*dy+dz*dz;
    const f64 b = x*dx+(6360.0+y)*dy+z*dz;
    const f64 c = x*x+z*z+y*(12720.0+y);
    if (a <= 0.0) return -1.0;
    if (b >= 0.0) return -1.0;
    if (c <= 0.0) return 0.0;
    // 光路を単位方向と仮定しない判別式と近い根。
    const f64 discriminant = b*b-a*c;
    if (discriminant < 0.0) return -1.0;
    return c/(-b+::sqrt(discriminant));
}

// 有限区間の二次式の最小値から地中への侵入を調べる。点接触だけなら内部長が0なので遮蔽しない。
static bool ReferenceGroundBlocks_Internal(FVec3 origin, FVec3 direction, f64 distance)
{
    // 原点と方向を先に倍精度化する。
    const f64 x = origin.x;
    const f64 y = origin.y;
    const f64 z = origin.z;
    const f64 dx = direction.x;
    const f64 dy = direction.y;
    const f64 dz = direction.z;
    // 球方程式の三係数。実装の符号分岐を複製せず、最小点を直接評価する。
    const f64 a = dx*dx+dy*dy+dz*dz;
    const f64 b = x*dx+(6360.0+y)*dy+z*dz;
    const f64 c = x*x+z*z+y*(12720.0+y);
    if (a <= 0.0 || distance <= 0.0) return false;
    // 放物線の頂点を有限光路内へ収めた距離。
    const f64 vertex = -b/a;
    const f64 closest = vertex < 0.0 ? 0.0 : (vertex > distance ? distance : vertex);
    return (a*closest+2.0*b)*closest+c < 0.0;
}

// 一般位置の±1mmと球面終点±1ULPを、実GPUの交点・有限光路・反転で検査する。
ACS_TEST(Atmosphere, PhysicalFiniteGroundPathsKeepEndpointAndTangency)
{
    // 任意の原点、方向、距離を実行時に渡し、製品関数そのものを呼ぶ。
    FString source = ReadPhysicalSkyShader_Internal();
    EXPECT_TRUE(source.Size() > 0u);
    if (source.Size() == 0u) return;
    source.Append(R"(
RWTexture2D<float4> finiteGroundOutput : register(u0);
[numthreads(1,1,1)]
void CSFiniteGroundProbe(uint3 id : SV_DispatchThreadID) {
    // 地表相対kmの原点・方向・距離。方向長1を前提にしない。
    float3 origin = cloud_params0.xyz;
    float3 direction = cloud_params1.xyz;
    float distance = cloud_params0.w;
    // GPU演算で実際に渡す中点と終点。
    float3 middle = origin+direction*(0.5*distance);
    float3 end = origin+direction*distance;
    // 四つの光路を独立スレッドへ渡し、製品の一画素と同じ一回の光路評価を実行する。
    float3 path_origin = id.x == 2u ? middle : (id.x == 3u ? end : origin);
    float3 path_direction = id.x == 3u ? -direction : direction;
    float path_distance = id.x == 1u || id.x == 2u ? 0.5*distance : distance;
    finiteGroundOutput[uint2(id.x,0)] = float4(PhysicalTransmittance(path_origin,path_direction,path_distance),PhysicalRaySphereNear(path_origin,path_direction,kPhysicalGroundRadiusKm));
    if (id.x == 0u) {
        finiteGroundOutput[uint2(4,0)] = float4(origin,distance);
        finiteGroundOutput[uint2(5,0)] = float4(direction,0.0);
        finiteGroundOutput[uint2(6,0)] = float4(end,0.0);
        // 未正規化方向の極小値は、交点距離の近似と分離して有限区間の符号だけを調べる。
        finiteGroundOutput[uint2(7,0)] = float4(PhysicalGroundBlocksSegment(origin,direction,distance)?1.0:0.0,0.0,0.0,0.0);
    }
}
)");
    // GPUが使えない場合も失敗として記録する。
    FDeviceConfig configuration{};
    auto device = CreateRhiDevice(configuration);
    EXPECT_TRUE(device.IsOk());
    if (device.IsErr()) return;
    // 現製品のHLSLを含む検査用入口。
    FShaderDesc shaderDescription{};
    shaderDescription.stage = EShaderStage::Compute;
    shaderDescription.hlsl_source = source.Data();
    shaderDescription.entry_point = "CSFiniteGroundProbe";
    shaderDescription.target = kPhysicalSkyProbeTarget;
    auto shader = CreateRhiShader(*device.Value(),shaderDescription);
    EXPECT_TRUE(shader.IsOk());
    if (shader.IsErr()) return;
    // 四光路の結果、実入力、独立した遮蔽判定を8画素へ結び付ける。
    FComputePipelineDesc pipelineDescription{};
    pipelineDescription.cs = shader.Value().Get();
    pipelineDescription.cbuffer_slots = 1u;
    pipelineDescription.cbuffer_names[0] = "CSky";
    pipelineDescription.uav_slots = 1u;
    pipelineDescription.uav_names[0] = "finiteGroundOutput";
    auto pipeline = CreateRhiComputePipeline(*device.Value(),pipelineDescription);
    EXPECT_TRUE(pipeline.IsOk());
    if (pipeline.IsErr()) return;
    // 合成結果と実入力を丸めずに読み戻す出力先。
    FTextureDesc textureDescription{};
    textureDescription.width = 8u;
    textureDescription.height = 1u;
    textureDescription.format = EFormat::R32G32B32A32_Float;
    textureDescription.is_uav = true;
    auto texture = CreateRhiTexture(*device.Value(),textureDescription);
    EXPECT_TRUE(texture.IsOk());
    if (texture.IsErr()) return;
    // 検査入力は既存定数配置の行11・12へ格納する。
    FVec4 constants[16]{};
    FBufferDesc bufferDescription{};
    bufferDescription.size = sizeof(constants);
    bufferDescription.usage = EBufferUsage::Uniform;
    bufferDescription.cpu_writable = true;
    auto buffer = CreateRhiBuffer(*device.Value(),bufferDescription);
    EXPECT_TRUE(buffer.IsOk());
    if (buffer.IsErr()) return;
    // 1条件ごとに完了待ちして読み戻す命令列。
    auto command = CreateRhiCommandList(*device.Value());
    EXPECT_TRUE(command.IsOk());
    if (command.IsErr()) return;
    // 一つの有限光路と、解析的に固定した遮蔽の有無。
    struct FGroundPathCase {
        // 北極地表を原点とする位置km。
        FVec3 origin;
        // 正規化を要求しない進行方向。
        FVec3 direction;
        // 方向を掛ける有限区間の上端。
        f32 distance;
        // 元の光路が地表内部へ入るか。
        bool blocked;
    };
    // 先頭5条件は1mmの通過／遮蔽と方向長の変更、後半は厳密球面終点とその前後。
    constexpr FGroundPathCase cases[] = {{{1000.0f,1.0e-6f,0.0f},{-1.0f,0.0f,0.0f},2000.0f,false},{{1000.0f,-1.0e-6f,0.0f},{-1.0f,0.0f,0.0f},2000.0f,true},{{1000.0f,-1.0e-6f,0.0f},{-1.0f,0.0f,0.0f},999.9f,true},{{1000.0f,1.0e-6f,0.0f},{-2.0f,0.0f,0.0f},1000.0f,false},{{1000.0f,1.0e-6f,0.0f},{-0.125f,0.0f,0.0f},16000.0f,false},{{3816.0f,-1270.34375f,0.0f},{0.0f,-1.0f,0.0f},1.65625f,false},{{3816.0f,-1270.34375f,0.0f},{0.0f,-1.0f,0.0f},1.6562498807907104f,false},{{3816.0f,-1270.34375f,0.0f},{0.0f,-1.0f,0.0f},1.6562501192092896f,true},{{0.0f,0.0f,0.0f},{0.0f,-1.0f,0.0f},0.0f,false},{{0.0f,0.0f,0.0f},{1.0f,0.0f,0.0f},1.0f,false},{{1000.0f,0.0f,0.0f},{-1.0f,0.0f,0.0f},2000.0f,false},{{0.0f,1.0e-6f,1000.0f},{0.0f,0.0f,-1.0f},2000.0f,false},{{0.0f,-1.0e-6f,1000.0f},{0.0f,0.0f,-1.0f},999.9f,true},{{1000.0f,1.0e-6f,0.0f},{-16.0f,0.0f,0.0f},125.0f,false},{{3816.0f,-1272.0f,0.0f},{4.0f,-3.0f,0.0f},10.0f,false}};
    // 係数の大きい項が相殺する前に下位桁を失う、非北極の厳密接線。
    constexpr FGroundPathCase cancellationCases[] = {{{3815.5048828125f,-1271.628662109375f,0.0f},{4.0f,-3.0f,0.0f},0.24755859375f,false},{{3815.99951171875f,-1271.9996337890625f,0.0f},{4.0f,-3.0f,0.0f},0.000244140625f,false}};
    // 正の最小値も正長の光路。無限大とNaNは不正入力として光を通さない規約で検査する。
    const FGroundPathCase exceptionalCases[] = {{{0.0f,-1.0f,0.0f},{0.0f,1.0f,0.0f},ProbeFloatFromBits_Internal(1u),true},{{0.0f,0.0f,0.0f},{0.0f,1.0f,0.0f},ProbeFloatFromBits_Internal(0x7f800000u),true},{{0.0f,0.0f,0.0f},{0.0f,ProbeFloatFromBits_Internal(0x7fc00000u),0.0f},1.0f,true},{{ProbeFloatFromBits_Internal(0xff800000u),0.0f,0.0f},{1.0f,0.0f,0.0f},1.0f,true}};
    // −2^-127の方向と大きな長さの積は約1km。地表直前／接触／通過を元のbitで固定する。
    const FGroundPathCase tinyDirectionCases[] = {{{0.0f,1.0f,0.0f},{0.0f,ProbeFloatFromBits_Internal(0x80400000u),0.0f},ProbeFloatFromBits_Internal(0x7effffffu),false},{{0.0f,1.0f,0.0f},{0.0f,ProbeFloatFromBits_Internal(0x80400000u),0.0f},ProbeFloatFromBits_Internal(0x7f000000u),false},{{0.0f,1.0f,0.0f},{0.0f,ProbeFloatFromBits_Internal(0x80400000u),0.0f},ProbeFloatFromBits_Internal(0x7f000001u),true}};
    // 境界条件と残差消失条件を続けて同じ検査へ渡す。
    constexpr u32 baseCount = sizeof(cases)/sizeof(cases[0]);
    constexpr u32 cancellationCount = sizeof(cancellationCases)/sizeof(cancellationCases[0]);
    constexpr u32 exceptionalCount = sizeof(exceptionalCases)/sizeof(exceptionalCases[0]);
    constexpr u32 ordinaryCount = baseCount+cancellationCount+exceptionalCount;
    constexpr u32 tinyDirectionCount = sizeof(tinyDirectionCases)/sizeof(tinyDirectionCases[0]);
    for (u32 sample = 0u; sample < ordinaryCount+tinyDirectionCount; ++sample) {
        // 今回の入力。種類によって検査の許容差は変えない。
        const FGroundPathCase& input = sample < baseCount ? cases[sample] : (sample < baseCount+cancellationCount ? cancellationCases[sample-baseCount] : (sample < ordinaryCount ? exceptionalCases[sample-baseCount-cancellationCount] : tinyDirectionCases[sample-ordinaryCount]));
        constants[11] = FVec4{input.origin.x,input.origin.y,input.origin.z,input.distance};
        constants[12] = FVec4{input.direction.x,input.direction.y,input.direction.z,0.0f};
        buffer.Value()->Update(constants,sizeof(constants));
        command.Value()->Begin();
        command.Value()->SetComputePipeline(*pipeline.Value());
        command.Value()->SetConstantBuffer(0u,*buffer.Value());
        command.Value()->BindUav(0u,*texture.Value());
        command.Value()->Dispatch(4u,1u,1u);
        command.Value()->End();
        // 提出失敗を数値検査へ混ぜない。
        const bool submitted = command.Value()->Submit();
        EXPECT_TRUE(submitted);
        if (!submitted) return;
        device.Value()->WaitIdle();
        // 実GPUから読み戻した透過率・交点と入力。
        f32 values[32]{};
        const bool read = device.Value()->ReadTexture(*texture.Value(),values,sizeof(values));
        EXPECT_TRUE(read);
        if (!read) return;
        EXPECT_EQ(values[28],input.blocked?1.0f:0.0f);
        if (sample >= ordinaryCount) {
            // この契約は球面多項式の符号。極端な倍率での交点距離・密度積分の精度とは区別する。
            test::RecordInfo(FSourceLoc::Current(),"tiny_direction case=%u blocked=%.9g expected=%u whole=%.9g",sample-ordinaryCount,values[28],input.blocked?1u:0u,values[0]);
            if (input.blocked) {
                for (u32 channel = 0u; channel < 3u; ++channel) EXPECT_EQ(values[channel],0.0f);
            }
            continue;
        }
        // 入力自体が非有限の場合は参照積分せず、全体光路の拒否だけを調べる。
        const bool finiteInput = IsFiniteProbeValue_Internal(input.origin.x) && IsFiniteProbeValue_Internal(input.origin.y) && IsFiniteProbeValue_Internal(input.origin.z) && IsFiniteProbeValue_Internal(input.direction.x) && IsFiniteProbeValue_Internal(input.direction.y) && IsFiniteProbeValue_Internal(input.direction.z) && IsFiniteProbeValue_Internal(input.distance);
        if (!finiteInput) {
            for (u32 channel = 0u; channel < 3u; ++channel) EXPECT_EQ(values[channel],0.0f);
            EXPECT_TRUE(IsFiniteProbeValue_Internal(values[3]));
            continue;
        }
        for (u32 index = 0u; index < 28u; ++index) EXPECT_TRUE(IsFiniteProbeValue_Internal(values[index]));
        // 反転ではGPU演算で丸まった終点を原点に使う。理想的な反転と取り違えない。
        const FVec3 origins[2] = {{values[16],values[17],values[18]},{values[24],values[25],values[26]}};
        const FVec3 directions[2] = {{values[20],values[21],values[22]},{-values[20],-values[21],-values[22]}};
        for (u32 orientation = 0u; orientation < 2u; ++orientation) {
            // 参照側の入口と、有限区間内での遮蔽。
            const f64 entry = ReferenceGroundEntry_Internal(origins[orientation],directions[orientation]);
            const bool blocked = ReferenceGroundBlocks_Internal(origins[orientation],directions[orientation],values[19]);
            if (orientation == 0u) EXPECT_EQ(blocked,input.blocked);
            // 実際の入口距離も調べ、Tの0だけで誤った入口を見逃さない。
            const u32 offset = orientation*12u;
            if (entry < 0.0) EXPECT_TRUE(values[offset+3u] < 0.0f);
            else {
                // 方向の倍率を変えても、交点誤差は物理的な距離kmで同じ上限にする。
                const f64 directionLength = ::sqrt(static_cast<f64>(directions[orientation].x)*directions[orientation].x+static_cast<f64>(directions[orientation].y)*directions[orientation].y+static_cast<f64>(directions[orientation].z)*directions[orientation].z);
                EXPECT_TRUE(::fabs(static_cast<f64>(values[offset+3u])-entry)*directionLength <= 2.0e-4);
            }
            for (u32 channel = 0u; channel < 3u; ++channel) {
                // 製品とは独立した細分光路積分。遮蔽時は厳密0を要求する。
                const f64 expected = blocked ? 0.0 : ReferenceCurvedTransmittance_Internal(origins[orientation].y,directions[orientation].x,directions[orientation].y,values[19],channel,32768u,origins[orientation].x,origins[orientation].z,directions[orientation].z);
                const f64 refined = blocked ? 0.0 : ReferenceCurvedTransmittance_Internal(origins[orientation].y,directions[orientation].x,directions[orientation].y,values[19],channel,65536u,origins[orientation].x,origins[orientation].z,directions[orientation].z);
                EXPECT_TRUE(IsFiniteProbeValue_Internal(expected));
                EXPECT_TRUE(::fabs(expected-refined) <= 1.0e-7);
                const f64 split = static_cast<f64>(values[4u+channel])*values[8u+channel];
                test::RecordInfo(FSourceLoc::Current(),"finite_ground case=%u reverse=%u channel=%u entry=%.12g gpu_entry=%.12g blocked=%u actual=%.9g expected=%.9g split=%.9g",sample,orientation,channel,entry,values[offset+3u],blocked?1u:0u,values[offset+channel],expected,split);
                if (blocked) EXPECT_EQ(values[offset+channel],0.0f);
                else {
                    EXPECT_TRUE(values[offset+channel] > 0.0f);
                    EXPECT_TRUE(values[offset+channel] <= 1.0f);
                    EXPECT_TRUE(::fabs(static_cast<f64>(values[offset+channel])-expected) <= 2.0e-4);
                    // 最小の正値の半分はfloatで0になるため、その場合だけ分割は別の光路になる。
                    if (orientation == 0u && input.distance >= ProbeFloatFromBits_Internal(0x00800000u)) EXPECT_TRUE(::fabs(static_cast<f64>(values[channel])-split) <= 2.0e-4);
                }
            }
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
    // 比較する各成分の係数と入射光。積分経路の単位はkm。
    constexpr f64 incident[3] = {1.0,0.8,0.6};
    // 視線の近似を固定する旧50区間参照は廃止し、消散係数の検査も連続積分の解析解へ引き継ぐ。
    f64 expected[3]{};
    for (u32 channel = 0u; channel < 3u; ++channel) {
        expected[channel] = ReferenceVerticalExactScattering_Internal(0.0,channel)*incident[channel];
    }
    // 0の補正、各求積則と複数区間への分割を通す。鉛直の期待値は全て同じ解析解。
    constexpr u32 budgets[] = {0u,1u,2u,3u,4u,8u,20u,24u};
    for (u32 budget : budgets) {
        parameters.sun_steps = budget;
        // 地表から大気上端までの実製品の単散乱値。
        const FVec3 actual = CAtmosphere::EvaluateSkyRadiance(0.0f,FVec3{0.0f,1.0f,0.0f},parameters);
        const f32 channels[3] = {actual.x,actual.y,actual.z};
        for (u32 channel = 0u; channel < 3u; ++channel) {
            test::RecordInfo(FSourceLoc::Current(),"mie_cpu_scatter budget=%u channel=%u actual=%.9g expected=%.9g",budget,channel,channels[channel],expected[channel]);
            EXPECT_TRUE(IsFiniteProbeValue_Internal(channels[channel]));
            EXPECT_TRUE(IsFiniteProbeValue_Internal(expected[channel]));
            EXPECT_TRUE(::fabs(static_cast<f64>(channels[channel])-expected[channel]) <= 2.0e-6);
        }
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
    shaderDescription.target = kPhysicalSkyProbeTarget;
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
                const f64 referenceTransmittance = ReferenceVerticalExactTransmittance_Internal(heights[height],distances[distance],channel);
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
    shaderDescription.target = kPhysicalSkyProbeTarget;
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
