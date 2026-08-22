// SPDX-License-Identifier: Apache-2.0
// SpriteBatchの部分更新とframe間再利用を実GPUで検証する。
#include "test/Test.h"
#include "test/Expect.h"

#include "foundation/Move.h"
#include "memory/MemorySystem.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiDevice.h"
#include "render/IRhiTexture.h"
#include "render/SpriteBatch.h"
#if WITH_RENDER_DX12_RAW && !WITH_RENDER_DILIGENT
#    include "render/Dx12/Dx12Buffer.h"
#endif

using namespace acs;

namespace {

/** 必要な場合だけtest process用memory systemを所有する。 */
class CTestMemorySystemScope {
public:
    /** memory systemを利用可能にする。 */
    bool Init() noexcept {
        if (CMemorySystem::Get(ESegment::Resource) != nullptr) return true;
        const TResult<void> result = CMemorySystem::Init(CMemorySystem::DefaultConfig());
        m_Owned = result.IsOk();
        return result.IsOk();
    }

    /** このscopeが初期化したmemory systemだけを終了する。 */
    ~CTestMemorySystemScope() noexcept {
        if (m_Owned) CMemorySystem::Shutdown();
    }

private:
    /** このscopeが終了責任を持つ場合はtrue。 */
    bool m_Owned = false;
};

/** 一つのoff-screen frameを送信してbackendのframe slotを進める。 */
bool SubmitEmptyFrame(IRhiDevice& device) noexcept {
    auto command_result = CreateRhiCommandList(device);
    if (command_result.IsErr()) return false;
    auto command = Move(command_result.Value());
    command->Begin();
    command->End();
    return command->Submit();
}

/** 指定pixelが期待する色成分を強く持つことを検証する。 */
void ExpectDominantColor(const u8* pixels, u32 width, u32 x, u32 y, u32 channel) noexcept {
    const usize offset = (static_cast<usize>(y) * width + x) * 4u;
    EXPECT_TRUE(pixels[offset + channel] >= 200u);
    for (u32 index = 0u; index < 3u; ++index) {
        if (index != channel) EXPECT_TRUE(pixels[offset + index] <= 48u);
    }
    EXPECT_TRUE(pixels[offset + 3u] >= 200u);
}

} // namespace

ACS_TEST(SpriteBatchUpload, CpuWritableVertexBufferCyclesAcrossInFlightFrames) {
    CTestMemorySystemScope memory;
    EXPECT_TRUE(memory.Init());
    if (CMemorySystem::Get(ESegment::Resource) == nullptr) return;

    FDeviceConfig configuration{};
    configuration.enable_debug_layer = true;
    auto device_result = CreateRhiDevice(configuration);
    EXPECT_TRUE(device_result.IsOk());
    if (device_result.IsErr()) return;
    auto device = Move(device_result.Value());

    FBufferDesc description{};
    description.size = 1024u;
    description.usage = EBufferUsage::Vertex;
    description.cpu_writable = true;
    auto buffer_result = CreateRhiBuffer(*device, description);
    EXPECT_TRUE(buffer_result.IsOk());
    if (buffer_result.IsErr()) return;
    auto buffer = Move(buffer_result.Value());

#if WITH_RENDER_DX12_RAW && !WITH_RENDER_DILIGENT
    const auto first_address = static_cast<FDx12Buffer*>(buffer.Get())->Gpu();
#else
    const usize first_offset = buffer->BindingOffset();
#endif
    EXPECT_TRUE(SubmitEmptyFrame(*device));
#if WITH_RENDER_DX12_RAW && !WITH_RENDER_DILIGENT
    const auto second_address = static_cast<FDx12Buffer*>(buffer.Get())->Gpu();
    EXPECT_TRUE(first_address != second_address);
#else
    const usize second_offset = buffer->BindingOffset();
    EXPECT_TRUE(first_offset != second_offset);
#endif
    EXPECT_TRUE(SubmitEmptyFrame(*device));
#if WITH_RENDER_DX12_RAW && !WITH_RENDER_DILIGENT
    EXPECT_EQ(static_cast<FDx12Buffer*>(buffer.Get())->Gpu(), first_address);
#else
    EXPECT_EQ(buffer->BindingOffset(), first_offset);
#endif
    EXPECT_EQ(buffer->Size(), static_cast<usize>(1024u));
    device->WaitIdle();
}

ACS_TEST(SpriteBatchUpload, AlternatingWhiteAndAtlasRangesRemainCompleteAcrossFrames) {
    CTestMemorySystemScope memory;
    EXPECT_TRUE(memory.Init());
    if (CMemorySystem::Get(ESegment::Resource) == nullptr) return;

    FDeviceConfig configuration{};
    configuration.enable_debug_layer = true;
    auto device_result = CreateRhiDevice(configuration);
    EXPECT_TRUE(device_result.IsOk());
    if (device_result.IsErr()) return;
    auto device = Move(device_result.Value());

    constexpr u32 kWidth = 224u;
    constexpr u32 kHeight = 128u;
    FTextureDesc target_description{};
    target_description.width = kWidth;
    target_description.height = kHeight;
    target_description.format = EFormat::R8G8B8A8_UNorm;
    target_description.is_render_target = true;
    constexpr u32 kFrameCount = 24u;
    TUniquePtr<IRhiTexture> targets[kFrameCount];
    for (u32 frame = 0u; frame < kFrameCount; ++frame) {
        auto target_result = CreateRhiTexture(*device, target_description);
        EXPECT_TRUE(target_result.IsOk());
        if (target_result.IsErr()) return;
        targets[frame] = Move(target_result.Value());
    }

    const u8 atlas_pixel[4] = {255u, 255u, 255u, 255u};
    FTextureDesc atlas_description{};
    atlas_description.width = 1u;
    atlas_description.height = 1u;
    atlas_description.format = EFormat::R8G8B8A8_UNorm;
    atlas_description.initial_data = atlas_pixel;
    atlas_description.initial_data_size = sizeof(atlas_pixel);
    auto atlas_result = CreateRhiTexture(*device, atlas_description);
    EXPECT_TRUE(atlas_result.IsOk());
    if (atlas_result.IsErr()) return;
    auto atlas = Move(atlas_result.Value());

    CSpriteBatch sprites;
    EXPECT_TRUE(sprites.Init(*device, EFormat::R8G8B8A8_UNorm, 256u).IsOk());
    auto command_result = CreateRhiCommandList(*device);
    EXPECT_TRUE(command_result.IsOk());
    if (command_result.IsErr()) return;
    auto command = Move(command_result.Value());

    for (u32 frame = 0u; frame < kFrameCount; ++frame) {
        command->Begin();
        command->BeginRenderToTexture(*targets[frame], FClearColor{0, 0, 0, 1}, nullptr, 1.0f);
        sprites.Begin(*command, kWidth, kHeight);

        // WorldLabels相当のatlas連続範囲。
        const FVec4 world_color = (frame & 1u) == 0u ? FVec4{1, 0, 0, 1} : FVec4{0, 1, 0, 1};
        for (u32 label = 0u; label < 16u; ++label) sprites.Draw(*atlas, 4.0f + label * 12.0f, 4.0f, 8.0f, 8.0f, world_color);

        // interaction reticle相当の白texture矩形を10個挟む。
        const FVec4 reticle_color = (frame & 1u) == 0u ? FVec4{0, 1, 0, 1} : FVec4{1, 0, 0, 1};
        for (u32 line = 0u; line < 10u; ++line) sprites.DrawRect(4.0f + line * 12.0f, 20.0f, 8.0f, 8.0f, reticle_color);

        // 通常UIの背景と12 glyph相当を交互に積み、部分uploadとtexture切替を繰り返す。
        const FVec4 glyph_color = (frame & 1u) == 0u ? FVec4{0, 0, 1, 1} : FVec4{1, 0, 0, 1};
        for (u32 row = 0u; row < 10u; ++row) {
            const f32 y = 36.0f + row * 9.0f;
            sprites.DrawRect(4.0f, y, 210.0f, 8.0f, FVec4{0, 0, 1, 1});
            for (u32 glyph = 0u; glyph < 12u; ++glyph) sprites.Draw(*atlas, 12.0f + glyph * 12.0f, y + 1.0f, 8.0f, 6.0f, glyph_color);
        }
        sprites.End();
        command->EndRenderToTexture(*targets[frame]);
        command->End();
        EXPECT_TRUE(command->Submit());
    }
    device->WaitIdle();

    for (u32 frame = 0u; frame < kFrameCount; ++frame) {
        u8 pixels[kWidth * kHeight * 4u]{};
        EXPECT_TRUE(device->ReadTexture(*targets[frame], pixels, static_cast<u32>(sizeof(pixels))));
        const u32 world_channel = (frame & 1u) == 0u ? 0u : 1u;
        const u32 reticle_channel = (frame & 1u) == 0u ? 1u : 0u;
        const u32 glyph_channel = (frame & 1u) == 0u ? 2u : 0u;
        ExpectDominantColor(pixels, kWidth, 8u, 8u, world_channel);
        ExpectDominantColor(pixels, kWidth, 8u, 24u, reticle_channel);
        for (u32 row = 0u; row < 10u; ++row) {
            const u32 y = 36u + row * 9u;
            ExpectDominantColor(pixels, kWidth, 6u, y + 4u, 2u);
            for (u32 glyph = 0u; glyph < 12u; ++glyph) ExpectDominantColor(pixels, kWidth, 16u + glyph * 12u, y + 4u, glyph_channel);
        }
    }
}
