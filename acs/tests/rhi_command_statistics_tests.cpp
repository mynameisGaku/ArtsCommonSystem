// SPDX-License-Identifier: Apache-2.0

#include "test/Test.h"
#include "test/Expect.h"
#include "render/IRhiCommandList.h"

using namespace acs;

namespace {

class FStatisticsCommandList : public IRhiCommandList {
public:
    void Begin() noexcept override {}
    void End() noexcept override {}
    void Submit() noexcept override {}

    void BeginRenderToSwapchain(
        IRhiSwapchain&, u32, const FClearColor&, IRhiTexture*, f32) noexcept override {}
    void EndRenderToSwapchain(IRhiSwapchain&, u32) noexcept override {}
    void BeginShadowPass(IRhiTexture&, f32) noexcept override {}
    void EndShadowPass(IRhiTexture&) noexcept override {}
    void BeginRenderToTexture(
        IRhiTexture&, const FClearColor&, IRhiTexture*, f32) noexcept override {}
    void EndRenderToTexture(IRhiTexture&) noexcept override {}
    void BeginRenderToTextureLoad(IRhiTexture&, IRhiTexture*) noexcept override {}
    void BeginRenderToTextureSlice(
        IRhiTexture&, u32, u32, const FClearColor&) noexcept override {}
    void BeginRenderToTextureMrt(
        IRhiTexture* const*, u32, const FClearColor&, IRhiTexture*, f32) noexcept override {}

    void SetViewport(const FViewport&) noexcept override {}
    void SetScissor(const FScissorRect&) noexcept override {}
    void SetStencilRef(u32) noexcept override {}
    void SetPipeline(IRhiPipeline&) noexcept override {}
    void SetVertexBuffer(IRhiBuffer&, u32) noexcept override {}
    void SetIndexBuffer(IRhiBuffer&) noexcept override {}
    void SetConstantBuffer(u32, IRhiBuffer&) noexcept override {}
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
};

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
