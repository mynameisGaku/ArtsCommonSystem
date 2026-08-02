// SPDX-License-Identifier: Apache-2.0

#include "test/Test.h"
#include "test/Expect.h"
#include "asset/MeshAsset.h"
#include "editor_abi/EditorFrustumCulling.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiDevice.h"
#include "render/MotionVector.h"
#include "memory/SystemAllocator.h"

using namespace acs;

namespace {

class CStatisticsCommandList : public IRhiCommandList {
public:
    void Begin() noexcept override { ++begin_count; }
    void End() noexcept override {}
    bool Submit() noexcept override {
        ++submit_count;
        return submit_result;
    }

    void BeginRenderToSwapchain(
        IRhiSwapchain&, u32, const FClearColor&, IRhiTexture*, f32) noexcept override {}
    void BeginRenderToSwapchainLoad(
        IRhiSwapchain&, u32) noexcept override {}
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

    void Draw(
        u32 vertex_count, u32 first_vertex = 0u) noexcept override {
        if (vertex_count != 0u &&
            draw_record_count <
                static_cast<u32>(sizeof(draw_counts) /
                                 sizeof(draw_counts[0]))) {
            draw_counts[draw_record_count] = vertex_count;
            draw_first_vertices[draw_record_count] = first_vertex;
            ++draw_record_count;
        }
        RecordDraw(m_CommandStatistics, vertex_count);
    }

    void DrawIndexed(
        u32 index_count, u32 = 0u, i32 = 0) noexcept override {
        if (index_count != 0u &&
            indexed_draw_record_count <
                static_cast<u32>(sizeof(indexed_draw_counts) /
                                 sizeof(indexed_draw_counts[0]))) {
            indexed_draw_counts[indexed_draw_record_count++] =
                index_count;
        }
        RecordDraw(m_CommandStatistics, index_count);
    }

    void Dispatch(u32 gx, u32 gy, u32 gz) noexcept override {
        if (gx == 0u || gy == 0u || gz == 0u) return;
        RecordDispatch(m_CommandStatistics);
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
    u32 draw_record_count = 0u;
    u32 draw_counts[64]{};
    u32 draw_first_vertices[64]{};
    u32 indexed_draw_record_count = 0u;
    u32 indexed_draw_counts[64]{};

private:
    /** fakeが所有する変更可能な命令統計を返す。 */
    FRhiCommandStatistics& StatisticsStorage() noexcept override { return m_CommandStatistics; }

    /** fakeが所有する読み取り専用の命令統計を返す。 */
    const FRhiCommandStatistics& StatisticsStorage() const noexcept override { return m_CommandStatistics; }

    /** このfakeコマンドリストだけに属する命令統計。 */
    FRhiCommandStatistics m_CommandStatistics{};
};

/** I接頭辞のcommand list interfaceがvptr以外の状態を持たないことを固定する。 */
static_assert(sizeof(IRhiCommandList) == sizeof(void*));

ACS_TEST(Render,
         NonBlockingCommandListDefaultsPreserveLegacyBackendContract)
{
    CStatisticsCommandList command;
    EXPECT_TRUE(command.CanBeginWithoutGpuWait());
    EXPECT_TRUE(command.TryBeginWithoutGpuWait());
    EXPECT_TRUE(command.SubmitWithoutGpuWait());
    EXPECT_EQ(command.begin_count, 1u);
    EXPECT_EQ(command.submit_count, 1u);
    command.submit_result = false;
    EXPECT_FALSE(command.SubmitWithoutGpuWait());
    EXPECT_EQ(command.submit_count, 2u);
}

class CTimingCommandList final : public CStatisticsCommandList {
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

class CDepthCopyTexture final : public IRhiTexture {
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

class CSwitchableMotionPoolAllocator final : public IAllocator {
public:
    explicit CSwitchableMotionPoolAllocator(IAllocator& backing) noexcept
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
    IAllocator* m_Backing = nullptr;
    bool m_Failing = false;
};

} // namespace

ACS_TEST(Render, RhiCommandStatisticsCountAndReset)
{
    CStatisticsCommandList command_list;

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

ACS_TEST(Render, RhiCommandStatisticsStorageBelongsToConcreteCommandList)
{
    /** draw統計だけを記録する一つ目のfake。 */
    CStatisticsCommandList first;
    /** dispatch統計だけを記録する二つ目のfake。 */
    CStatisticsCommandList second;

    first.Draw(6u);
    second.Dispatch(1u, 1u, 1u);

    EXPECT_TRUE(&first.Statistics() != &second.Statistics());
    EXPECT_EQ(first.Statistics().draw_calls, 1u);
    EXPECT_EQ(first.Statistics().dispatch_calls, 0u);
    EXPECT_EQ(second.Statistics().draw_calls, 0u);
    EXPECT_EQ(second.Statistics().dispatch_calls, 1u);

    first.ResetStatistics();
    EXPECT_EQ(first.Statistics().draw_calls, 0u);
    EXPECT_EQ(second.Statistics().dispatch_calls, 1u);
}

ACS_TEST(Render, EditorSceneGeometryPassPolicyMatchesProductionCommands)
{
    using namespace editor_frustum_culling;

    struct FExpectedPolicy {
        bool masked;
        ESubmissionCommandForm command;
    };
    constexpr FExpectedPolicy expected[] = {
        {true, ESubmissionCommandForm::Draw},
        {true, ESubmissionCommandForm::DrawIndexed},
        {true, ESubmissionCommandForm::None},
        {true, ESubmissionCommandForm::DrawIndexed},
        {true, ESubmissionCommandForm::DrawIndexed},
        {true, ESubmissionCommandForm::None},
        {true, ESubmissionCommandForm::DrawIndexed},
        {false, ESubmissionCommandForm::Draw},
        {false, ESubmissionCommandForm::Dispatch},
    };
    static_assert(
        static_cast<u32>(ESceneGeometryPass::Count) ==
        static_cast<u32>(sizeof(expected) / sizeof(expected[0])));

    const u8 visibility[] = {1u, 0u, 1u, 1u};
    const FSubmissionMaskView main_view_mask{
        true, visibility,
        static_cast<u32>(sizeof(visibility) / sizeof(visibility[0]))};
    for (u32 pass_index = 0u;
         pass_index < static_cast<u32>(ESceneGeometryPass::Count);
         ++pass_index) {
        const ESceneGeometryPass pass =
            static_cast<ESceneGeometryPass>(pass_index);
        const FSceneGeometryPassPolicy policy =
            SceneGeometryPassPolicy(pass);
        EXPECT_EQ(policy.uses_main_view_mask, expected[pass_index].masked);
        EXPECT_EQ(
            static_cast<u32>(policy.command_form),
            static_cast<u32>(expected[pass_index].command));
        const FSubmissionMaskView pass_mask =
            SubmissionMaskForPass(pass, main_view_mask);
        EXPECT_EQ(pass_mask.ShouldSubmit(1u), !expected[pass_index].masked);
    }
}

ACS_TEST(Render, EditorMainViewCullingRecordsTruthfulCommandsForEveryPass)
{
    using namespace editor_frustum_culling;

    const u8 visibility[] = {1u, 0u, 1u, 1u};
    const u32 vertex_offsets[] = {0u, 3u, 6u, 9u};
    const u32 vertex_counts[] = {3u, 3u, 3u, 3u};
    const FSubmissionMaskView main_view_mask{
        true, visibility,
        static_cast<u32>(sizeof(visibility) / sizeof(visibility[0]))};
    CStatisticsCommandList command_list;

    // The normal/depth prepass is the one production non-indexed camera pass.
    // Visible nodes 2 and 3 are adjacent in the aggregate VB and coalesce.
    const FSubmissionMaskView normal_mask = SubmissionMaskForPass(
        ESceneGeometryPass::NormalDepthPrepass, main_view_mask);
    const u32 normal_ranges = ForEachSubmittedVertexRange(
        normal_mask, 4u,
        [&](u32 index) noexcept { return vertex_offsets[index]; },
        [&](u32 index) noexcept { return vertex_counts[index]; },
        [&](u32 offset, u32 count) noexcept {
            command_list.Draw(count, offset);
        });
    EXPECT_EQ(normal_ranges, 2u);
    EXPECT_EQ(command_list.draw_record_count, 2u);
    EXPECT_EQ(command_list.draw_counts[0], 3u);
    EXPECT_EQ(command_list.draw_first_vertices[0], 0u);
    EXPECT_EQ(command_list.draw_counts[1], 6u);
    EXPECT_EQ(command_list.draw_first_vertices[1], 6u);

    constexpr ESceneGeometryPass indexed_passes[] = {
        ESceneGeometryPass::MotionVectors,
        ESceneGeometryPass::PbrOpaqueDraw,
        ESceneGeometryPass::InteractiveWaterDraw,
        ESceneGeometryPass::RefractionDraw,
    };
    for (ESceneGeometryPass pass : indexed_passes) {
        const u32 submitted = ForEachSubmittedNode(
            SubmissionMaskForPass(pass, main_view_mask), 4u,
            [&](u32 node_index) noexcept {
                // A unique count identifies the submitted node.
                command_list.DrawIndexed((node_index + 1u) * 3u);
            });
        EXPECT_EQ(submitted, 3u);
    }
    EXPECT_EQ(command_list.indexed_draw_record_count, 12u);
    for (u32 record = 0u; record < 12u; record += 3u) {
        EXPECT_EQ(command_list.indexed_draw_counts[record], 3u);
        EXPECT_EQ(command_list.indexed_draw_counts[record + 1u], 9u);
        EXPECT_EQ(command_list.indexed_draw_counts[record + 2u], 12u);
    }

    // Count/preflight are mask-aware eligibility queries and emit no command.
    const u32 draws_before_queries =
        command_list.Statistics().draw_calls;
    EXPECT_EQ(
        ForEachSubmittedNode(
            SubmissionMaskForPass(
                ESceneGeometryPass::PbrOpaqueCount, main_view_mask),
            4u, [](u32) noexcept {}),
        3u);
    EXPECT_FALSE(AnySubmittedNode(
        SubmissionMaskForPass(
            ESceneGeometryPass::RefractionPreflight, main_view_mask),
        4u, [](u32 node_index) noexcept { return node_index == 1u; }));
    EXPECT_TRUE(AnySubmittedNode(
        SubmissionMaskForPass(
            ESceneGeometryPass::RefractionPreflight, main_view_mask),
        4u, [](u32 node_index) noexcept { return node_index == 2u; }));
    EXPECT_EQ(
        command_list.Statistics().draw_calls,
        draws_before_queries);

    // Shadow uses one aggregate non-indexed light-space draw; the hidden
    // camera node remains eligible. VXGI similarly sees the whole world and
    // records compute dispatches, never camera-filtered per-node draws.
    const FSubmissionMaskView shadow_mask = SubmissionMaskForPass(
        ESceneGeometryPass::ShadowCaster, main_view_mask);
    const FSubmissionMaskView vxgi_mask = SubmissionMaskForPass(
        ESceneGeometryPass::VxgiVoxelization, main_view_mask);
    EXPECT_TRUE(shadow_mask.ShouldSubmit(1u));
    EXPECT_TRUE(vxgi_mask.ShouldSubmit(1u));
    command_list.Draw(12u, 0u);
    command_list.Dispatch(1u, 1u, 1u);

    EXPECT_EQ(command_list.draw_record_count, 3u);
    EXPECT_EQ(command_list.draw_counts[2], 12u);
    EXPECT_EQ(command_list.draw_first_vertices[2], 0u);
    EXPECT_EQ(command_list.Statistics().draw_calls, 15u);
    EXPECT_EQ(command_list.Statistics().dispatch_calls, 1u);
    EXPECT_EQ(command_list.Statistics().triangles, 39u);
}

ACS_TEST(Render, EditorNormalPrepassPreservesAggregateFastPath)
{
    using namespace editor_frustum_culling;

    const u8 all_visible[] = {1u, 1u, 1u, 1u};
    const FSubmissionMaskView enabled_mask{
        true, all_visible,
        static_cast<u32>(sizeof(all_visible) / sizeof(all_visible[0]))};
    EXPECT_TRUE(ShouldUseAggregateVertexDraw(enabled_mask, 0u));
    EXPECT_FALSE(ShouldUseAggregateVertexDraw(enabled_mask, 1u));
    EXPECT_TRUE(ShouldUseAggregateVertexDraw(
        FSubmissionMaskView{false, all_visible, 4u}, 4u));

    CStatisticsCommandList command_list;
    if (ShouldUseAggregateVertexDraw(enabled_mask, 0u))
        command_list.Draw(12u, 0u);
    EXPECT_EQ(command_list.draw_record_count, 1u);
    EXPECT_EQ(command_list.draw_counts[0], 12u);
    EXPECT_EQ(command_list.draw_first_vertices[0], 0u);
}

ACS_TEST(Render, EditorVisibleVertexRangesCoalesceWithoutOverflow)
{
    using namespace editor_frustum_culling;

    const u8 visibility[] = {1u, 1u, 0u, 1u, 1u};
    const u32 offsets[] = {0u, 3u, 6u, 9u, 12u};
    const u32 counts[] = {3u, 3u, 3u, 3u, 3u};
    const FSubmissionMaskView mask{
        true, visibility,
        static_cast<u32>(sizeof(visibility) / sizeof(visibility[0]))};
    u32 emitted_offsets[4]{};
    u32 emitted_counts[4]{};
    u32 recorded = 0u;
    EXPECT_EQ(
        ForEachSubmittedVertexRange(
            mask, 5u,
            [&](u32 index) noexcept { return offsets[index]; },
            [&](u32 index) noexcept { return counts[index]; },
            [&](u32 offset, u32 count) noexcept {
                emitted_offsets[recorded] = offset;
                emitted_counts[recorded] = count;
                ++recorded;
            }),
        2u);
    EXPECT_EQ(recorded, 2u);
    EXPECT_EQ(emitted_offsets[0], 0u);
    EXPECT_EQ(emitted_counts[0], 6u);
    EXPECT_EQ(emitted_offsets[1], 9u);
    EXPECT_EQ(emitted_counts[1], 6u);

    // Hostile metadata must not wrap range_end and accidentally merge spans.
    const u32 hostile_offsets[] = {~u32{0} - 2u, ~u32{0}, 0u};
    const u32 hostile_counts[] = {2u, 1u, 3u};
    recorded = 0u;
    EXPECT_EQ(
        ForEachSubmittedVertexRange(
            FSubmissionMaskView{}, 3u,
            [&](u32 index) noexcept { return hostile_offsets[index]; },
            [&](u32 index) noexcept { return hostile_counts[index]; },
            [&](u32 offset, u32 count) noexcept {
                emitted_offsets[recorded] = offset;
                emitted_counts[recorded] = count;
                ++recorded;
            }),
        3u);
    EXPECT_EQ(recorded, 3u);
    EXPECT_EQ(emitted_offsets[0], ~u32{0} - 2u);
    EXPECT_EQ(emitted_counts[0], 2u);
    EXPECT_EQ(emitted_offsets[1], ~u32{0});
    EXPECT_EQ(emitted_counts[1], 1u);
    EXPECT_EQ(emitted_offsets[2], 0u);
    EXPECT_EQ(emitted_counts[2], 3u);
}

ACS_TEST(Render, EditorMainViewCullingSubmissionMaskFailsOpen)
{
    using namespace editor_frustum_culling;

    const u8 hidden[] = {0u, 0u, 0u};
    u32 submitted = 0u;
    EXPECT_EQ(
        ForEachSubmittedNode(
            FSubmissionMaskView{false, hidden, 3u}, 3u,
            [&](u32) noexcept { ++submitted; }),
        3u);
    EXPECT_EQ(submitted, 3u);

    submitted = 0u;
    EXPECT_EQ(
        ForEachSubmittedNode(
            FSubmissionMaskView{true, nullptr, 3u}, 3u,
            [&](u32) noexcept { ++submitted; }),
        3u);
    EXPECT_EQ(submitted, 3u);

    // An undersized buffer may cull only an in-range explicit decision.
    // Missing decisions stay visible rather than disappearing.
    submitted = 0u;
    EXPECT_EQ(
        ForEachSubmittedNode(
            FSubmissionMaskView{true, hidden, 1u}, 3u,
            [&](u32) noexcept { ++submitted; }),
        2u);
    EXPECT_EQ(submitted, 2u);
}

ACS_TEST(Render, RhiGpuTimingDefaultsAndScopeBalance)
{
    CStatisticsCommandList unsupported;
    FRhiGpuTimingSnapshot unavailable{};
    unavailable.valid = true;
    EXPECT_FALSE(unsupported.BeginGpuTimingFrame(7u));
    EXPECT_FALSE(unsupported.TryGetGpuTiming(unavailable));
    EXPECT_FALSE(unavailable.valid);
    {
        FScopedRhiGpuTiming scope(
            &unsupported, ERhiGpuTimingPass::Cloud);
    }

    CTimingCommandList supported;
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
    CStatisticsCommandList command_list;
    CDepthCopyTexture attachment;
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

    CMotionVector motion;
    const auto init_result = motion.Init(*device_result.Value(), 16u, 16u);
    EXPECT_TRUE(init_result.IsOk());
    if (init_result.IsErr()) return;

    CStatisticsCommandList rejected;
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
    CStatisticsCommandList accepted;
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

    CSystemAllocator backing;
    CSwitchableMotionPoolAllocator pool_allocator{backing};
    CMotionVector motion{pool_allocator};
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

    CStatisticsCommandList command_list;
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
    CDepthCopyTexture source;
    CDepthCopyTexture destination;
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

    CStatisticsCommandList unsupported;
    EXPECT_FALSE(unsupported.CopyDepthTexture(source, destination));
}
