// SPDX-License-Identifier: Apache-2.0
// Diligent メモリアダプタの再バインド寿命契約テスト。
#include "test/Test.h"
#include "test/Expect.h"

#include "foundation/Compiler.h"

#if WITH_RENDER_DILIGENT

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
#    include "render/IRhiTexture.h"

using namespace acs;

ACS_TEST(FDiligentDevice, RequiresInitializedMemorySystem)
{
    EXPECT_TRUE(FMemorySystem::Get(ESegment::Resource) == nullptr);

    FDiligentDevice device;
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
    FSystemAllocator allocator;
    void* const raw_adapter = FDiligentMemoryAdapter::Create(&allocator);
    EXPECT_TRUE(raw_adapter != nullptr);
    if (!raw_adapter) return;

    auto* const adapter = static_cast<Diligent::IMemoryAllocator*>(raw_adapter);
    const u64 first_generation = FDiligentMemoryAdapter::BindingGeneration();
    EXPECT_TRUE(first_generation != 0);
    EXPECT_EQ(FDiligentMemoryAdapter::BackingLifetimeGeneration(), allocator.LifetimeGeneration());

    void* const allocation = adapter->Allocate(128u, "lifetime-test", __FILE__, __LINE__);
    EXPECT_TRUE(allocation != nullptr);
    EXPECT_EQ(FDiligentMemoryAdapter::OutstandingAllocationCount(), 1ull);
    EXPECT_EQ(FDiligentMemoryAdapter::OutstandingRequestedBytes(), 128ull);

    // MemorySystem の再初期化で同じアドレスが再利用される場合も、この分岐で旧寿命を隔離する。
    EXPECT_TRUE(FDiligentMemoryAdapter::Create(&allocator) == nullptr);
    EXPECT_EQ(FDiligentMemoryAdapter::BindingGeneration(), first_generation);

    adapter->Free(allocation);
    EXPECT_EQ(FDiligentMemoryAdapter::OutstandingAllocationCount(), 0ull);
    EXPECT_EQ(FDiligentMemoryAdapter::OutstandingRequestedBytes(), 0ull);

    EXPECT_TRUE(FDiligentMemoryAdapter::Create(&allocator) == raw_adapter);
    EXPECT_TRUE(FDiligentMemoryAdapter::BindingGeneration() > first_generation);

    // 静的アダプタへテストローカル allocator の参照を残さない。
    EXPECT_TRUE(FDiligentMemoryAdapter::Create(&DefaultAllocator()) == raw_adapter);
    EXPECT_EQ(FDiligentMemoryAdapter::BackingLifetimeGeneration(), DefaultAllocator().LifetimeGeneration());
}

ACS_TEST(FDiligentDevice, InitializedMemorySystemOwnsAndReleasesDiligentAllocations)
{
    const auto memory_result = FMemorySystem::Init(FMemorySystem::DefaultConfig());
    EXPECT_TRUE(memory_result.IsOk());
    if (memory_result.IsErr()) return;

    bool device_initialized = false;
    {
        FDiligentDevice device;
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

    EXPECT_EQ(FDiligentMemoryAdapter::OutstandingAllocationCount(), 0ull);
    EXPECT_EQ(FDiligentMemoryAdapter::OutstandingRequestedBytes(), 0ull);
    FMemorySystem::Shutdown();

    if (device_initialized) {
        ACS_LOG_INFO("[acs][memory] tracker=diligent_device_integration_test record=verdict "
                     "skipped=false leak_detected=false status=ok");
    }
}

ACS_TEST(FDiligentDevice, OffscreenSubmissionsRecycleDynamicDescriptors)
{
    const auto memory_result = FMemorySystem::Init(FMemorySystem::DefaultConfig());
    EXPECT_TRUE(memory_result.IsOk());
    if (memory_result.IsErr()) return;

    {
        FDiligentDevice device;
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

    EXPECT_EQ(FDiligentMemoryAdapter::OutstandingAllocationCount(), 0ull);
    EXPECT_EQ(FDiligentMemoryAdapter::OutstandingRequestedBytes(), 0ull);
    FMemorySystem::Shutdown();
}

#endif // WITH_RENDER_DILIGENT
