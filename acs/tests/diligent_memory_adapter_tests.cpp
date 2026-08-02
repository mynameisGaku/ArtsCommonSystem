// SPDX-License-Identifier: Apache-2.0
// Diligent メモリアダプタの再バインド寿命契約テスト。
#include "test/Test.h"
#include "test/Expect.h"

#include "foundation/Compiler.h"

#if WITH_RENDER_DILIGENT

#    include <windows.h>

#    include "MemoryAllocator.h"
#    include "memory/Memory.h"
#    include "memory/MemorySystem.h"
#    include "memory/SystemAllocator.h"
#    include "foundation/Log.h"
#    include "render/Diligent/DiligentDevice.h"
#    include "render/Diligent/DiligentMemoryAdapter.h"
#    include "render/IRhiBuffer.h"
#    include "render/IRhiCommandList.h"
#    include "render/IRhiPipeline.h"
#    include "render/IRhiShader.h"
#    include "render/IRhiSwapchain.h"
#    include "render/IRhiTexture.h"

using namespace acs;

namespace {

/** 共有 immediate context のPSO切替試験で使う全画面三角形VS。 */
constexpr const char* kSharedContextVertexSource = "struct VSOut { float4 position : SV_POSITION; }; VSOut main(uint vertex_id : SV_VertexID) { float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2); VSOut output; output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); return output; }";

/** 共有 immediate context の最終色を検証する赤PS。 */
constexpr const char* kSharedContextRedPixelSource = "float4 main() : SV_TARGET { return float4(1.0, 0.0, 0.0, 1.0); }";

/** 別command listがnative PSOを上書きする緑PS。 */
constexpr const char* kSharedContextGreenPixelSource = "float4 main() : SV_TARGET { return float4(0.0, 1.0, 0.0, 1.0); }";

} // namespace

ACS_TEST(FDiligentDevice, RequiresInitializedMemorySystem)
{
    EXPECT_TRUE(CMemorySystem::Get(ESegment::Resource) == nullptr);

    CDiligentDevice device;
    FDeviceConfig configuration{};
    configuration.backend = ERhiBackendKind::D3D12;
    const auto result = device.Init(configuration);
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(result.Error().subcode, static_cast<u16>(105));
    }
}

ACS_TEST(FDiligentMemoryAdapter, RejectsSameAddressRebindWhileAllocationIsOutstanding)
{
    CSystemAllocator allocator;
    void* const raw_adapter = CDiligentMemoryAdapter::Create(&allocator);
    EXPECT_TRUE(raw_adapter != nullptr);
    if (!raw_adapter) return;

    auto* const adapter = static_cast<Diligent::IMemoryAllocator*>(raw_adapter);
    const u64 first_generation = CDiligentMemoryAdapter::BindingGeneration();
    EXPECT_TRUE(first_generation != 0);
    EXPECT_EQ(CDiligentMemoryAdapter::BackingLifetimeGeneration(), allocator.LifetimeGeneration());

    void* const allocation = adapter->Allocate(128u, "lifetime-test", __FILE__, __LINE__);
    EXPECT_TRUE(allocation != nullptr);
    EXPECT_EQ(CDiligentMemoryAdapter::OutstandingAllocationCount(), 1ull);
    EXPECT_EQ(CDiligentMemoryAdapter::OutstandingRequestedBytes(), 128ull);

    // MemorySystem の再初期化で同じアドレスが再利用される場合も、この分岐で旧寿命を隔離する。
    EXPECT_TRUE(CDiligentMemoryAdapter::Create(&allocator) == nullptr);
    EXPECT_EQ(CDiligentMemoryAdapter::BindingGeneration(), first_generation);

    adapter->Free(allocation);
    EXPECT_EQ(CDiligentMemoryAdapter::OutstandingAllocationCount(), 0ull);
    EXPECT_EQ(CDiligentMemoryAdapter::OutstandingRequestedBytes(), 0ull);

    EXPECT_TRUE(CDiligentMemoryAdapter::Create(&allocator) == raw_adapter);
    EXPECT_TRUE(CDiligentMemoryAdapter::BindingGeneration() > first_generation);

    // 静的アダプタへテストローカル allocator の参照を残さない。
    EXPECT_TRUE(CDiligentMemoryAdapter::Create(&DefaultAllocator()) == raw_adapter);
    EXPECT_EQ(CDiligentMemoryAdapter::BackingLifetimeGeneration(), DefaultAllocator().LifetimeGeneration());
}

ACS_TEST(FDiligentDevice, InitializedMemorySystemOwnsAndReleasesDiligentAllocations)
{
    const auto memory_result = CMemorySystem::Init(CMemorySystem::DefaultConfig());
    EXPECT_TRUE(memory_result.IsOk());
    if (memory_result.IsErr()) return;

    bool device_initialized = false;
    {
        CDiligentDevice device;
        FDeviceConfig configuration{};
        configuration.backend = ERhiBackendKind::D3D12;
        configuration.enable_debug_layer = true;
        const auto device_result = device.Init(configuration);
        if (device_result.IsErr()) {
            const u16 subcode = device_result.Error().subcode;
            const bool gpu_unavailable = subcode >= 101u && subcode <= 104u;
            EXPECT_TRUE(gpu_unavailable);
            ACS_LOG_INFO("[acs][memory] tracker=diligent_device_integration_test record=skip "
                         "skipped=%s reason=%s subcode=%u",
                         gpu_unavailable ? "true" : "false",
                         gpu_unavailable ? "gpu_or_debug_runtime_unavailable" : "unexpected_initialization_failure",
                         static_cast<unsigned int>(subcode));
        } else {
            device_initialized = true;
            // Diligent device features are opt-in.  The editor relies on this
            // capability to keep shader warm-up off the HWND owner thread, so
            // a successfully created production D3D12 device must advertise
            // the feature that was requested in EngineD3D12CreateInfo.
            EXPECT_TRUE(device.SupportsAsyncShaderCompilation());
        }
    }

    EXPECT_EQ(CDiligentMemoryAdapter::OutstandingAllocationCount(), 0ull);
    EXPECT_EQ(CDiligentMemoryAdapter::OutstandingRequestedBytes(), 0ull);
    CMemorySystem::Shutdown();

    if (device_initialized) {
        ACS_LOG_INFO("[acs][memory] tracker=diligent_device_integration_test record=verdict "
                     "skipped=false leak_detected=false status=ok");
    }
}

ACS_TEST(FDiligentDevice, OffscreenSubmissionsRecycleDynamicDescriptors)
{
    const auto memory_result = CMemorySystem::Init(CMemorySystem::DefaultConfig());
    EXPECT_TRUE(memory_result.IsOk());
    if (memory_result.IsErr()) return;

    {
        CDiligentDevice device;
        FDeviceConfig configuration{};
        configuration.backend = ERhiBackendKind::D3D12;
        const auto device_result = device.Init(configuration);
        if (device_result.IsErr()) {
            const u16 subcode = device_result.Error().subcode;
            EXPECT_TRUE(subcode >= 101u && subcode <= 107u);
        } else {
            static constexpr const char* kVertexSource = R"(
struct VSOut { float4 position : SV_POSITION; };
VSOut main(uint vertex_id : SV_VertexID)
{
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    VSOut output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
})";
            static constexpr const char* kPixelSource = R"(
cbuffer Params
{
    float4 color;
};
Texture2D InputTexture;
SamplerState InputTexture_sampler;
float4 main() : SV_TARGET
{
    return InputTexture.SampleLevel(InputTexture_sampler, float2(0.5f, 0.5f), 0.0f) * color;
})";

            FShaderDesc vertex_desc{};
            vertex_desc.stage = EShaderStage::Vertex;
            vertex_desc.hlsl_source = kVertexSource;
            vertex_desc.entry_point = "main";
            const auto vertex_result = CreateRhiShader(device, vertex_desc);
            EXPECT_TRUE(vertex_result.IsOk());

            FShaderDesc pixel_desc{};
            pixel_desc.stage = EShaderStage::Pixel;
            pixel_desc.hlsl_source = kPixelSource;
            pixel_desc.entry_point = "main";
            const auto pixel_result = CreateRhiShader(device, pixel_desc);
            EXPECT_TRUE(pixel_result.IsOk());

            FBufferDesc buffer_desc{};
            alignas(256) const f32 color_data[64] = {0.25f, 0.50f, 0.75f, 1.0f};
            buffer_desc.size = sizeof(color_data);
            buffer_desc.usage = EBufferUsage::Uniform;
            buffer_desc.initial_data = color_data;
            const auto buffer_result = CreateRhiBuffer(device, buffer_desc);
            EXPECT_TRUE(buffer_result.IsOk());

            FTextureDesc texture_desc{};
            texture_desc.width = 4u;
            texture_desc.height = 4u;
            texture_desc.format = EFormat::R8G8B8A8_UNorm;
            texture_desc.is_render_target = true;
            const auto texture_result = CreateRhiTexture(device, texture_desc);
            EXPECT_TRUE(texture_result.IsOk());

            FTextureDesc sampled_texture_desc{};
            sampled_texture_desc.width = 1u;
            sampled_texture_desc.height = 1u;
            sampled_texture_desc.format = EFormat::R8G8B8A8_UNorm;
            sampled_texture_desc.is_render_target = true;
            const auto sampled_texture_result = CreateRhiTexture(device, sampled_texture_desc);
            EXPECT_TRUE(sampled_texture_result.IsOk());

            const auto command_result = CreateRhiCommandList(device);
            EXPECT_TRUE(command_result.IsOk());

            if (vertex_result.IsOk() && pixel_result.IsOk() && buffer_result.IsOk() && texture_result.IsOk() &&
                sampled_texture_result.IsOk() && command_result.IsOk()) {
                FPipelineDesc pipeline_desc{};
                pipeline_desc.vs = vertex_result.Value().Get();
                pipeline_desc.ps = pixel_result.Value().Get();
                pipeline_desc.rt_format = EFormat::R8G8B8A8_UNorm;
                pipeline_desc.cbuffer_slots = 1u;
                pipeline_desc.cbuffer_names[0] = "Params";
                pipeline_desc.texture_slots = 1u;
                pipeline_desc.texture_names[0] = "InputTexture";
                pipeline_desc.static_sampler_count = 1u;
                const auto pipeline_result = CreateRhiPipeline(device, pipeline_desc);
                EXPECT_TRUE(pipeline_result.IsOk());

                if (pipeline_result.IsOk()) {
                    IRhiCommandList* command = command_result.Value().Get();
                    // 140 * 64 = 8960 dynamic SRV descriptor-table commits. Without
                    // an off-screen FinishFrame boundary this exceeds Diligent's
                    // 8192-descriptor dynamic heap and reproduces the preview failure.
                    for (u32 frame = 0u; frame < 140u; ++frame) {
                        command->Begin();
                        command->BeginRenderToTexture(*texture_result.Value(), FClearColor{0.0f, 0.0f, 0.0f, 1.0f});
                        command->SetPipeline(*pipeline_result.Value());
                        command->SetConstantBuffer(0u, *buffer_result.Value());
                        command->SetTexture(0u, *sampled_texture_result.Value());
                        for (u32 draw = 0u; draw < 64u; ++draw)
                            command->Draw(3u, 0u);
                        command->EndRenderToTexture(*texture_result.Value());
                        command->End();
                        command->Submit();
                    }
                    device.WaitIdle();
                }
            }
        }
    }

    EXPECT_EQ(CDiligentMemoryAdapter::OutstandingAllocationCount(), 0ull);
    EXPECT_EQ(CDiligentMemoryAdapter::OutstandingRequestedBytes(), 0ull);
    CMemorySystem::Shutdown();
}

ACS_TEST(FDiligentDevice, SharedContextPipelineStateRemainsCoherent)
{
    // Diligent実GPU試験用のメモリ初期化結果。
    const auto memory_result = CMemorySystem::Init(CMemorySystem::DefaultConfig());
    EXPECT_TRUE(memory_result.IsOk());
    if (memory_result.IsErr()) return;

    {
        // 共有immediate contextを所有する試験デバイス。
        CDiligentDevice device;
        // D3D12試験デバイスの生成条件。
        FDeviceConfig configuration{};
        configuration.backend = ERhiBackendKind::D3D12;
        // GPU未利用環境を既存様式でskipする初期化結果。
        const auto device_result = device.Init(configuration);
        if (device_result.IsErr()) {
            // Diligent初期化失敗の詳細コード。
            const u16 subcode = device_result.Error().subcode;
            EXPECT_TRUE(subcode >= 101u && subcode <= 107u);
        } else {
            // 全画面三角形VSの生成条件。
            FShaderDesc vertex_desc{};
            vertex_desc.stage = EShaderStage::Vertex;
            vertex_desc.hlsl_source = kSharedContextVertexSource;
            vertex_desc.entry_point = "main";
            // 全画面三角形VSの生成結果。
            const auto vertex_result = CreateRhiShader(device, vertex_desc);
            // 最終色を示す赤PSの生成条件。
            FShaderDesc red_pixel_desc{};
            red_pixel_desc.stage = EShaderStage::Pixel;
            red_pixel_desc.hlsl_source = kSharedContextRedPixelSource;
            red_pixel_desc.entry_point = "main";
            // 最終色を示す赤PSの生成結果。
            const auto red_pixel_result = CreateRhiShader(device, red_pixel_desc);
            // 共有contextを途中で上書きする緑PSの生成条件。
            FShaderDesc green_pixel_desc{};
            green_pixel_desc.stage = EShaderStage::Pixel;
            green_pixel_desc.hlsl_source = kSharedContextGreenPixelSource;
            green_pixel_desc.entry_point = "main";
            // 共有contextを途中で上書きする緑PSの生成結果。
            const auto green_pixel_result = CreateRhiShader(device, green_pixel_desc);
            // 最終ピクセルをreadbackするoff-screen RTの生成条件。
            FTextureDesc texture_desc{};
            texture_desc.width = 4u;
            texture_desc.height = 4u;
            texture_desc.format = EFormat::R8G8B8A8_UNorm;
            texture_desc.is_render_target = true;
            // 最終ピクセルをreadbackするoff-screen RTの生成結果。
            const auto texture_result = CreateRhiTexture(device, texture_desc);
            // 赤PSを論理状態として保持する第1 command list。
            const auto first_command_result = CreateRhiCommandList(device);
            // 同一immediate contextを緑PSへ切り替える第2 command list。
            const auto second_command_result = CreateRhiCommandList(device);

            EXPECT_TRUE(vertex_result.IsOk());
            EXPECT_TRUE(red_pixel_result.IsOk());
            EXPECT_TRUE(green_pixel_result.IsOk());
            EXPECT_TRUE(texture_result.IsOk());
            EXPECT_TRUE(first_command_result.IsOk());
            EXPECT_TRUE(second_command_result.IsOk());

            if (vertex_result.IsOk() && red_pixel_result.IsOk() && green_pixel_result.IsOk() && texture_result.IsOk() && first_command_result.IsOk() && second_command_result.IsOk()) {
                // 赤・緑PSOで共有するgraphics pipelineの生成条件。
                FPipelineDesc pipeline_desc{};
                pipeline_desc.vs = vertex_result.Value().Get();
                pipeline_desc.ps = red_pixel_result.Value().Get();
                pipeline_desc.rt_format = EFormat::R8G8B8A8_UNorm;
                // 第1 command listが再指定する赤PSOの生成結果。
                const auto red_pipeline_result = CreateRhiPipeline(device, pipeline_desc);
                pipeline_desc.ps = green_pixel_result.Value().Get();
                // 第2 command listが共有contextへ設定する緑PSOの生成結果。
                const auto green_pipeline_result = CreateRhiPipeline(device, pipeline_desc);
                EXPECT_TRUE(red_pipeline_result.IsOk());
                EXPECT_TRUE(green_pipeline_result.IsOk());

                if (red_pipeline_result.IsOk() && green_pipeline_result.IsOk()) {
                    // 赤PSOをlocal lookup状態として保持する第1 wrapper。
                    IRhiCommandList* const first = first_command_result.Value().Get();
                    // 同一native contextを緑PSOへ切り替える第2 wrapper。
                    IRhiCommandList* const second = second_command_result.Value().Get();

                    // 両 wrapper は同じ immediate context を共有する。A の local cache が
                    // red のままでも、B が green を bind した後の再指定は必ず native へ届く。
                    first->Begin();
                    first->BeginRenderToTexture(*texture_result.Value(), FClearColor{0.0f, 0.0f, 0.0f, 1.0f});
                    first->SetPipeline(*red_pipeline_result.Value());
                    first->Draw(3u, 0u);

                    second->Begin();
                    second->BeginRenderToTextureLoad(*texture_result.Value());
                    second->SetPipeline(*green_pipeline_result.Value());
                    second->Draw(3u, 0u);
                    second->End();

                    first->SetPipeline(*red_pipeline_result.Value());
                    first->Draw(3u, 0u);
                    first->EndRenderToTexture(*texture_result.Value());
                    first->End();
                    EXPECT_TRUE(first->Submit());
                    device.WaitIdle();

                    // 4x4 RGBA8 RTのreadback格納先。
                    u8 pixels[4u * 4u * 4u]{};
                    // 最終赤ピクセルをCPUへ取得できたか。
                    const bool read = device.ReadTexture(*texture_result.Value(), pixels, sizeof(pixels));
                    EXPECT_TRUE(read);
                    if (read) {
                        // ラスタ端を避ける中央ピクセルのbyte offset。
                        constexpr u32 kCenterPixel = ((2u * 4u) + 2u) * 4u;
                        EXPECT_TRUE(pixels[kCenterPixel] >= 250u);
                        EXPECT_TRUE(pixels[kCenterPixel + 1u] <= 5u);
                        EXPECT_TRUE(pixels[kCenterPixel + 2u] <= 5u);
                        EXPECT_TRUE(pixels[kCenterPixel + 3u] >= 250u);
                    }

                    // primary PresentがFinishFrameする経路のhidden HWND。
                    const HWND window = ::CreateWindowExW(0, L"STATIC", L"ACS Diligent frame boundary test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 64, 64, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
                    if (window != nullptr) {
                        {
                            // hidden HWND用primary swapchainの生成条件。
                            FSwapchainConfig swapchain_desc{};
                            swapchain_desc.external_hwnd = window;
                            swapchain_desc.external_width = 64u;
                            swapchain_desc.external_height = 64u;
                            swapchain_desc.format = EFormat::R8G8B8A8_UNorm;
                            swapchain_desc.vsync = false;
                            // primary swapchainの生成結果。
                            const auto swapchain_result = CreateRhiSwapchain(device, swapchain_desc);
                            EXPECT_TRUE(swapchain_result.IsOk());
                            if (swapchain_result.IsOk()) {
                                // Present対象のprimary swapchain。
                                IRhiSwapchain* const swapchain = swapchain_result.Value().Get();
                                // 同じimmediate contextへ交互に記録するwrapper一覧。
                                IRhiCommandList* const commands[2] = {first, second};
                                // 両wrapperがPresent後に同じ赤PSOを再指定する4フレーム。
                                for (u32 frame = 0u; frame < 4u; ++frame) {
                                    // 現フレームを担当するwrapper。
                                    IRhiCommandList* const command = commands[frame & 1u];
                                    command->Begin();
                                    // Diligentが内部管理する現backbuffer index。
                                    const u32 buffer_index = swapchain->AcquireNextImage();
                                    command->BeginRenderToSwapchain(*swapchain, buffer_index, FClearColor{0.0f, 0.0f, 0.0f, 1.0f});
                                    // hidden backbuffer全体を覆うviewport。
                                    FViewport viewport{};
                                    viewport.width = 64.0f;
                                    viewport.height = 64.0f;
                                    command->SetViewport(viewport);
                                    // hidden backbuffer全体を覆うscissor。
                                    FScissorRect scissor{};
                                    scissor.right = 64;
                                    scissor.bottom = 64;
                                    command->SetScissor(scissor);
                                    command->SetPipeline(*red_pipeline_result.Value());
                                    command->Draw(3u, 0u);
                                    command->EndRenderToSwapchain(*swapchain, buffer_index);
                                    command->End();
                                    EXPECT_TRUE(command->Submit());
                                    EXPECT_TRUE(swapchain->Present());
                                }
                            }
                        }
                        ::DestroyWindow(window);
                    }
                }
            }
        }
    }

    EXPECT_EQ(CDiligentMemoryAdapter::OutstandingAllocationCount(), 0ull);
    EXPECT_EQ(CDiligentMemoryAdapter::OutstandingRequestedBytes(), 0ull);
    CMemorySystem::Shutdown();
}

#endif // WITH_RENDER_DILIGENT
