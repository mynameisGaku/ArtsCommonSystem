// SPDX-License-Identifier: Apache-2.0

#include "test/Test.h"
#include "test/Expect.h"
#include "asset/MeshAsset.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiDevice.h"
#include "render/MotionVector.h"
#include "memory/SystemAllocator.h"

using namespace acs;

namespace {

class FStatisticsCommandList : public IRhiCommandList {
public:
    void Begin() noexcept override { ++begin_count; }
    void End() noexcept override {}
    bool Submit() noexcept override {
        ++submit_count;
        return submit_result;
    }

    void BeginRenderToSwapchain(
        IRhiSwapchain&, u32, const FClearColor&, IRhiTexture*, f32) noexcept override {}
    void EndRenderToSwapchain(IRhiSwapchain&, u32) noexcept override {}
    void BeginShadowPass(IRhiTexture&, f32) noexcept override {}
    void EndShadowPass(IRhiTexture&) noexcept override {}
    void BeginRenderToTexture(
        IRhiTexture&, const FClearColor&, IRhiTexture*, f32) noexcept override {}
    void EndRenderToTexture(IRhiTexture&) noexcept override {
        ++single_rt_end_count;
    }
    void BeginRenderToTextureLoad(IRhiTexture&, IRhiTexture*) noexcept override {}
    void BeginRenderToTextureSlice(
        IRhiTexture&, u32, u32, const FClearColor&) noexcept override {}
    bool BeginRenderToTextureMrt(
        IRhiTexture* const* rts, u32 rt_count, const FClearColor&, IRhiTexture*,
        f32) noexcept override {
        ++mrt_begin_count;
        mrt_begin_target_count = rt_count;
        for (u32 i = 0; i < rt_count && i < 8u; ++i) {
            mrt_begin_targets[i] = rts ? rts[i] : nullptr;
        }
        return mrt_begin_result;
    }
    bool BeginRenderToTextureMrtLoad(
        IRhiTexture* const*, u32, const FClearColor&, u32,
        IRhiTexture*, bool, f32) noexcept override {
        return true;
    }
    void EndRenderToTextureMrt(
        IRhiTexture* const* rts, u32 rt_count) noexcept override {
        ++mrt_end_count;
        mrt_end_target_count = rt_count;
        for (u32 i = 0; i < rt_count && i < 8u; ++i) {
            mrt_end_targets[i] = rts ? rts[i] : nullptr;
        }
    }

    void SetViewport(const FViewport&) noexcept override {}
    void SetScissor(const FScissorRect&) noexcept override {}
    void SetStencilRef(u32) noexcept override {}
    void SetPipeline(IRhiPipeline&) noexcept override {
        ++set_pipeline_count;
    }
    void SetVertexBuffer(IRhiBuffer&, u32) noexcept override {
        ++set_vertex_buffer_count;
    }
    void SetIndexBuffer(IRhiBuffer&) noexcept override {
        ++set_index_buffer_count;
    }
    void SetConstantBuffer(u32, IRhiBuffer&) noexcept override {
        ++set_constant_buffer_count;
    }
    void SetTexture(u32, IRhiTexture&) noexcept override {}

    void Draw(u32 vertex_count, u32 = 0u) noexcept override {
        RecordDraw(vertex_count);
    }

    void DrawIndexed(
        u32 index_count, u32 = 0u, i32 = 0) noexcept override {
        RecordDraw(index_count);
    }

    void Dispatch(u32 gx, u32 gy, u32 gz) noexcept override {
        if (gx == 0u || gy == 0u || gz == 0u) return;
        RecordDispatch();
    }

    void* NativeHandle() noexcept override { return nullptr; }

    bool mrt_begin_result = true;
    u32 mrt_begin_count = 0u;
    u32 mrt_begin_target_count = 0u;
    IRhiTexture* mrt_begin_targets[8]{};
    u32 mrt_end_count = 0u;
    u32 mrt_end_target_count = 0u;
    IRhiTexture* mrt_end_targets[8]{};
    u32 single_rt_end_count = 0u;
    u32 begin_count = 0u;
    u32 submit_count = 0u;
    bool submit_result = true;
    u32 set_pipeline_count = 0u;
    u32 set_vertex_buffer_count = 0u;
    u32 set_index_buffer_count = 0u;
    u32 set_constant_buffer_count = 0u;
};

ACS_TEST(Render,
         NonBlockingCommandListDefaultsPreserveLegacyBackendContract)
{
    FStatisticsCommandList command;
    EXPECT_TRUE(command.CanBeginWithoutGpuWait());
    EXPECT_TRUE(command.TryBeginWithoutGpuWait());
    EXPECT_TRUE(command.SubmitWithoutGpuWait());
    EXPECT_EQ(command.begin_count, 1u);
    EXPECT_EQ(command.submit_count, 1u);
    command.submit_result = false;
    EXPECT_FALSE(command.SubmitWithoutGpuWait());
    EXPECT_EQ(command.submit_count, 2u);
}

class FTimingCommandList final : public FStatisticsCommandList {
public:
    bool BeginGpuTimingFrame(u64 frame_index) noexcept override {
        frame = frame_index;
        return true;
    }

    bool BeginGpuTimingPass(ERhiGpuTimingPass pass) noexcept override {
        last_pass = pass;
        ++begin_count;
        return true;
    }

    void EndGpuTimingPass() noexcept override { ++end_count; }

    u64 frame = 0;
    u32 begin_count = 0;
    u32 end_count = 0;
    ERhiGpuTimingPass last_pass = ERhiGpuTimingPass::Count;
};

class FDepthCopyTexture final : public IRhiTexture {
public:
    u32 Width() const noexcept override { return width; }
    u32 Height() const noexcept override { return height; }
    EFormat PixelFormat() const noexcept override { return format; }
    u32 MipLevels() const noexcept override { return mip_levels; }
    u32 ArraySize() const noexcept override { return array_size; }
    bool IsCubemap() const noexcept override { return cubemap; }
    bool IsDepthTarget() const noexcept override { return depth; }
    bool IsShaderVisibleDepth() const noexcept override {
        return shader_visible;
    }
    u32 SampleCount() const noexcept override { return samples; }

    u32 width = 1280u;
    u32 height = 720u;
    EFormat format = EFormat::D32_Float;
    u32 mip_levels = 1u;
    u32 array_size = 1u;
    u32 samples = 1u;
    bool cubemap = false;
    bool depth = true;
    bool shader_visible = false;
};

class FSwitchableMotionPoolAllocator final : public FAllocator {
public:
    explicit FSwitchableMotionPoolAllocator(FAllocator& backing) noexcept
        : m_Backing(&backing) {}

    void SetFailing(bool failing) noexcept { m_Failing = failing; }

    void* Alloc(
        usize size, usize alignment,
        FSourceLoc location) noexcept override {
        return m_Failing
            ? nullptr
            : m_Backing->Alloc(size, alignment, location);
    }

    void Free(void* pointer) noexcept override {
        m_Backing->Free(pointer);
    }

private:
    FAllocator* m_Backing = nullptr;
    bool m_Failing = false;
};

} // namespace

ACS_TEST(Render, RhiCommandStatisticsCountAndReset)
{
    FStatisticsCommandList command_list;

    command_list.Draw(0u);
    command_list.DrawIndexed(0u);
    command_list.Dispatch(0u, 1u, 1u);
    EXPECT_EQ(command_list.Statistics().draw_calls, 0u);
    EXPECT_EQ(command_list.Statistics().dispatch_calls, 0u);
    EXPECT_EQ(command_list.Statistics().triangles, 0u);

    command_list.Draw(3u);
    command_list.DrawIndexed(4u);
    command_list.Draw(6u);
    command_list.Dispatch(1u, 2u, 3u);

    const FRhiCommandStatistics& populated = command_list.Statistics();
    EXPECT_EQ(populated.draw_calls, 3u);
    EXPECT_EQ(populated.dispatch_calls, 1u);
    EXPECT_EQ(populated.triangles, 4u);

    command_list.ResetStatistics();
    const FRhiCommandStatistics& reset = command_list.Statistics();
    EXPECT_EQ(reset.draw_calls, 0u);
    EXPECT_EQ(reset.dispatch_calls, 0u);
    EXPECT_EQ(reset.triangles, 0u);
}

ACS_TEST(Render, RhiGpuTimingDefaultsAndScopeBalance)
{
    FStatisticsCommandList unsupported;
    FRhiGpuTimingSnapshot unavailable{};
    unavailable.valid = true;
    EXPECT_FALSE(unsupported.BeginGpuTimingFrame(7u));
    EXPECT_FALSE(unsupported.TryGetGpuTiming(unavailable));
    EXPECT_FALSE(unavailable.valid);
    {
        FScopedRhiGpuTiming scope(
            &unsupported, ERhiGpuTimingPass::Cloud);
    }

    FTimingCommandList supported;
    EXPECT_TRUE(supported.BeginGpuTimingFrame(42u));
    {
        FScopedRhiGpuTiming scope(
            &supported, ERhiGpuTimingPass::Atmosphere);
        EXPECT_EQ(supported.begin_count, 1u);
        EXPECT_EQ(supported.end_count, 0u);
    }
    EXPECT_EQ(supported.frame, 42u);
    EXPECT_EQ(
        static_cast<u32>(supported.last_pass),
        static_cast<u32>(ERhiGpuTimingPass::Atmosphere));
    EXPECT_EQ(supported.begin_count, 1u);
    EXPECT_EQ(supported.end_count, 1u);
}

ACS_TEST(Render, RhiMrtBeginResultIsObservableToCallers)
{
    FStatisticsCommandList command_list;
    FDepthCopyTexture attachment;
    IRhiTexture* targets[1] = {&attachment};

    EXPECT_TRUE(command_list.BeginRenderToTextureMrt(
        targets, 1u, FClearColor{}, nullptr, 1.0f));
    EXPECT_TRUE(command_list.BeginRenderToTextureMrtLoad(
        targets, 1u, FClearColor{}, 0u, nullptr, false, 1.0f));
}

ACS_TEST(Render, MotionVectorMrtFailureSkipsPipelineDrawAndEnd)
{
    FDeviceConfig config{};
    auto device_result = CreateRhiDevice(config);
    if (device_result.IsErr()) return;

    FMotionVector motion;
    const auto init_result = motion.Init(*device_result.Value(), 16u, 16u);
    EXPECT_TRUE(init_result.IsOk());
    if (init_result.IsErr()) return;

    FStatisticsCommandList rejected;
    rejected.mrt_begin_result = false;
    EXPECT_FALSE(motion.Begin(
        rejected, FMat4::Identity(), FMat4::Identity()));
    EXPECT_EQ(rejected.mrt_begin_count, 1u);
    EXPECT_EQ(rejected.mrt_begin_target_count, 2u);
    EXPECT_TRUE(rejected.mrt_begin_targets[0] == motion.OutputTexture());
    EXPECT_TRUE(
        rejected.mrt_begin_targets[1] == motion.OutputNormalTexture());
    EXPECT_EQ(rejected.set_pipeline_count, 0u);

    // A rejected Begin must gate every draw command even when the mesh itself
    // is otherwise valid. The buffers only need to satisfy the public
    // FGpuMesh ownership contract because the inactive pass returns first.
    FBufferDesc vertex_desc{};
    vertex_desc.size = sizeof(FMeshVertex) * 3u;
    vertex_desc.usage = EBufferUsage::Vertex;
    auto vertex_result =
        CreateRhiBuffer(*device_result.Value(), vertex_desc);
    FBufferDesc index_desc{};
    index_desc.size = sizeof(u16) * 3u;
    index_desc.usage = EBufferUsage::Index16;
    auto index_result =
        CreateRhiBuffer(*device_result.Value(), index_desc);
    EXPECT_TRUE(vertex_result.IsOk());
    EXPECT_TRUE(index_result.IsOk());
    if (vertex_result.IsOk() && index_result.IsOk()) {
        FGpuMesh mesh{};
        mesh.vertex_buffer = Move(vertex_result.Value());
        mesh.index_buffer = Move(index_result.Value());
        mesh.vertex_stride = sizeof(FMeshVertex);
        mesh.index_count = 3u;
        EXPECT_FALSE(motion.DrawMesh(
            rejected, mesh, FMat4::Identity(), FMat4::Identity()));
    }
    EXPECT_EQ(rejected.set_constant_buffer_count, 0u);
    EXPECT_EQ(rejected.set_vertex_buffer_count, 0u);
    EXPECT_EQ(rejected.set_index_buffer_count, 0u);
    EXPECT_EQ(rejected.Statistics().draw_calls, 0u);

    motion.End(rejected);
    EXPECT_EQ(rejected.mrt_end_count, 0u);
    EXPECT_EQ(rejected.single_rt_end_count, 0u);

    // The next valid pass must recover normally and end the same complete
    // two-attachment set, including the world-normal target.
    FStatisticsCommandList accepted;
    EXPECT_TRUE(motion.Begin(
        accepted, FMat4::Identity(), FMat4::Identity()));
    EXPECT_EQ(accepted.set_pipeline_count, 1u);
    motion.End(accepted);
    EXPECT_EQ(accepted.mrt_end_count, 1u);
    EXPECT_EQ(accepted.mrt_end_target_count, 2u);
    EXPECT_TRUE(accepted.mrt_end_targets[0] == motion.OutputTexture());
    EXPECT_TRUE(
        accepted.mrt_end_targets[1] == motion.OutputNormalTexture());
    EXPECT_EQ(accepted.single_rt_end_count, 0u);
    motion.End(accepted);
    EXPECT_EQ(accepted.mrt_end_count, 1u);
}

ACS_TEST(Render, MotionVectorPoolGrowsPastLegacyLimitAndRecoversAllocation)
{
    FDeviceConfig config{};
    auto device_result = CreateRhiDevice(config);
    if (device_result.IsErr()) return;

    FSystemAllocator backing;
    FSwitchableMotionPoolAllocator pool_allocator{backing};
    FMotionVector motion{pool_allocator};
    const auto init_result = motion.Init(*device_result.Value(), 16u, 16u);
    EXPECT_TRUE(init_result.IsOk());
    if (init_result.IsErr()) return;

    const u32 initial_capacity = motion.ObjectBufferCapacity();
    EXPECT_TRUE(initial_capacity > 0u);
    EXPECT_TRUE(initial_capacity < 512u);

    // UINT32_MAX is the invalid draw-cursor sentinel. Reject it before any
    // geometric growth arithmetic or giant owner-array allocation is reached.
    constexpr u32 kInvalidDrawCount = ~u32{0};
    EXPECT_FALSE(motion.BeginFrame(kInvalidDrawCount));
    EXPECT_EQ(motion.ObjectBufferCapacity(), initial_capacity);
    EXPECT_EQ(motion.ObjectDrawCount(), 0u);
    EXPECT_TRUE(motion.BeginFrame(initial_capacity));
    EXPECT_EQ(motion.ObjectBufferCapacity(), initial_capacity);

    // Owner-array allocation failure is transactional: the retained GPU
    // buffers remain available, and the following frame can retry growth.
    pool_allocator.SetFailing(true);
    EXPECT_FALSE(motion.BeginFrame(initial_capacity + 1u));
    EXPECT_EQ(motion.ObjectBufferCapacity(), initial_capacity);
    EXPECT_EQ(motion.ObjectDrawCount(), 0u);

    pool_allocator.SetFailing(false);
    EXPECT_TRUE(motion.BeginFrame(512u));
    EXPECT_TRUE(motion.ObjectBufferCapacity() >= 512u);

    FBufferDesc vertex_desc{};
    vertex_desc.size = sizeof(FMeshVertex) * 3u;
    vertex_desc.usage = EBufferUsage::Vertex;
    auto vertex_result =
        CreateRhiBuffer(*device_result.Value(), vertex_desc);
    FBufferDesc index_desc{};
    index_desc.size = sizeof(u16) * 3u;
    index_desc.usage = EBufferUsage::Index16;
    auto index_result =
        CreateRhiBuffer(*device_result.Value(), index_desc);
    EXPECT_TRUE(vertex_result.IsOk());
    EXPECT_TRUE(index_result.IsOk());
    if (vertex_result.IsErr() || index_result.IsErr()) return;

    FGpuMesh mesh{};
    mesh.vertex_buffer = Move(vertex_result.Value());
    mesh.index_buffer = Move(index_result.Value());
    mesh.vertex_stride = sizeof(FMeshVertex);
    mesh.index_count = 3u;

    FStatisticsCommandList command_list;
    EXPECT_TRUE(motion.Begin(
        command_list, FMat4::Identity(), FMat4::Identity()));
    for (u32 i = 0; i < 512u; ++i) {
        EXPECT_TRUE(motion.DrawMesh(
            command_list, mesh, FMat4::Identity(), FMat4::Identity()));
    }
    motion.End(command_list);

    EXPECT_EQ(motion.ObjectDrawCount(), 512u);
    EXPECT_EQ(command_list.Statistics().draw_calls, 512u);
    EXPECT_EQ(command_list.set_constant_buffer_count, 512u);
    EXPECT_EQ(command_list.set_vertex_buffer_count, 512u);
    EXPECT_EQ(command_list.set_index_buffer_count, 512u);

    // The persistent pool is retained across frames; BeginFrame only resets
    // the logical cursor and never causes frame-to-frame allocation churn.
    EXPECT_TRUE(motion.BeginFrame(1u));
    EXPECT_TRUE(motion.ObjectBufferCapacity() >= 512u);
    EXPECT_EQ(motion.ObjectDrawCount(), 0u);
}

ACS_TEST(Render, DepthTextureCopyContractRejectsUnsafeResources)
{
    FDepthCopyTexture source;
    FDepthCopyTexture destination;
    destination.shader_visible = true;

    EXPECT_TRUE(IsDepthTextureCopyCompatible(source, destination));
    EXPECT_FALSE(IsDepthTextureCopyCompatible(source, source));

    destination.shader_visible = false;
    EXPECT_FALSE(IsDepthTextureCopyCompatible(source, destination));
    destination.shader_visible = true;

    destination.width = source.width + 1u;
    EXPECT_FALSE(IsDepthTextureCopyCompatible(source, destination));
    destination.width = source.width;

    destination.format = EFormat::D24_UNorm_S8_UInt;
    EXPECT_FALSE(IsDepthTextureCopyCompatible(source, destination));
    destination.format = EFormat::D32_Float;

    source.samples = 4u;
    destination.samples = 4u;
    EXPECT_FALSE(IsDepthTextureCopyCompatible(source, destination));
    source.samples = destination.samples = 1u;

    destination.array_size = 2u;
    EXPECT_FALSE(IsDepthTextureCopyCompatible(source, destination));
    destination.array_size = 1u;

    FStatisticsCommandList unsupported;
    EXPECT_FALSE(unsupported.CopyDepthTexture(source, destination));
}
