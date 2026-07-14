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

using namespace acs;

ACS_TEST(DiligentDevice, RequiresInitializedMemorySystem)
{
    EXPECT_TRUE(FMemorySystem::Get(ESegment::Resource) == nullptr);

    DiligentDevice device;
    DeviceConfig configuration{};
    configuration.backend = ERhiBackendKind::D3D12;
    const auto result = device.Init(configuration);
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(result.Error().subcode, static_cast<u16>(105));
    }
}

ACS_TEST(DiligentMemoryAdapter, RejectsSameAddressRebindWhileAllocationIsOutstanding)
{
    FSystemAllocator allocator;
    void* const raw_adapter = DiligentMemoryAdapter::Create(&allocator);
    EXPECT_TRUE(raw_adapter != nullptr);
    if (!raw_adapter) return;

    auto* const adapter = static_cast<Diligent::IMemoryAllocator*>(raw_adapter);
    const u64 first_generation = DiligentMemoryAdapter::BindingGeneration();
    EXPECT_TRUE(first_generation != 0);
    EXPECT_EQ(DiligentMemoryAdapter::BackingLifetimeGeneration(), allocator.LifetimeGeneration());

    void* const allocation = adapter->Allocate(128u, "lifetime-test", __FILE__, __LINE__);
    EXPECT_TRUE(allocation != nullptr);
    EXPECT_EQ(DiligentMemoryAdapter::OutstandingAllocationCount(), 1ull);
    EXPECT_EQ(DiligentMemoryAdapter::OutstandingRequestedBytes(), 128ull);

    // MemorySystem の再初期化で同じアドレスが再利用される場合も、この分岐で旧寿命を隔離する。
    EXPECT_TRUE(DiligentMemoryAdapter::Create(&allocator) == nullptr);
    EXPECT_EQ(DiligentMemoryAdapter::BindingGeneration(), first_generation);

    adapter->Free(allocation);
    EXPECT_EQ(DiligentMemoryAdapter::OutstandingAllocationCount(), 0ull);
    EXPECT_EQ(DiligentMemoryAdapter::OutstandingRequestedBytes(), 0ull);

    EXPECT_TRUE(DiligentMemoryAdapter::Create(&allocator) == raw_adapter);
    EXPECT_TRUE(DiligentMemoryAdapter::BindingGeneration() > first_generation);

    // 静的アダプタへテストローカル allocator の参照を残さない。
    EXPECT_TRUE(DiligentMemoryAdapter::Create(&DefaultAllocator()) == raw_adapter);
    EXPECT_EQ(DiligentMemoryAdapter::BackingLifetimeGeneration(), DefaultAllocator().LifetimeGeneration());
}

ACS_TEST(DiligentDevice, InitializedMemorySystemOwnsAndReleasesDiligentAllocations)
{
    const auto memory_result = FMemorySystem::Init(FMemorySystem::DefaultConfig());
    EXPECT_TRUE(memory_result.IsOk());
    if (memory_result.IsErr()) return;

    bool device_initialized = false;
    {
        DiligentDevice device;
        DeviceConfig configuration{};
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
        }
    }

    EXPECT_EQ(DiligentMemoryAdapter::OutstandingAllocationCount(), 0ull);
    EXPECT_EQ(DiligentMemoryAdapter::OutstandingRequestedBytes(), 0ull);
    FMemorySystem::Shutdown();

    if (device_initialized) {
        ACS_LOG_INFO("[acs][memory] tracker=diligent_device_integration_test record=verdict "
                     "skipped=false leak_detected=false status=ok");
    }
}

#endif // WITH_RENDER_DILIGENT
