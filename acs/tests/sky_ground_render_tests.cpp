// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "render/Sky.h"
#include "render/IRhiDevice.h"
#include "math/Camera.h"
#include "foundation/Move.h"
#include "platform/FileSystem.h"
#include "container/String.h"
#include <cstring>
#include <cmath>

using namespace acs;

// 製品HLSLを直接読み、検査入口だけを追加する。読取り不能や宣言欠落なら空文字列を返す。
static FString ReadPhysicalSkyShader_Internal()
{
    // コンパイル元のファイル名から検査対象のソースだけを解決する。
    constexpr wchar_t compiledPath[] = L"" __FILE__;
    // 最後の区切りの直後。作業ディレクトリには依存しない。
    usize directoryLength = 0u;
    for (usize index = 0u; compiledPath[index] != L'\0'; ++index) {
        if (compiledPath[index] == L'/' || compiledPath[index] == L'\\') directoryLength = index + 1u;
    }
    // testsからrenderの実装への固定した相対経路。
    constexpr wchar_t suffix[] = L"../src/render/Sky.cpp";
    // 入力パスと固定接尾辞を切り詰めずに収める。
    wchar_t sourcePath[sizeof(compiledPath) / sizeof(wchar_t) + sizeof(suffix) / sizeof(wchar_t)]{};
    for (usize index = 0u; index < directoryLength; ++index) sourcePath[index] = compiledPath[index];
    for (usize index = 0u; index < sizeof(suffix) / sizeof(wchar_t); ++index) sourcePath[directoryLength + index] = suffix[index];
    // 本体は既存のファイル読み取りと配列を使う。
    auto source = CFileSystem::ReadAllText(sourcePath);
    if (source.IsErr()) return {};
    // 宣言名と生文字列の境界を検査し、別シェーダーを誤抽出しない。
    const char* declaration = ::strstr(source.Value().GetData(), "const char* kSkyHLSL");
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
