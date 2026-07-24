// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Render — テクスチャ (array / cubemap / mip / per-slice RTV) 実 GPU 検証
// -----------------------------------------------------------------------------
// ヘッドレスで RHI デバイスを作り、cubemap / 2D 配列 / mip テクスチャを実際に
// 生成して、リソース desc・SRV・per-slice RTV 作成パスが GPU に受理されること、
// および ArraySize()/IsCubemap()/MipLevels() が正しく返ることを検証する。
// GPU が無い環境ではデバイス生成失敗で graceful にスキップする (致命にしない)。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "render/IRhiDevice.h"
#include "render/IRhiTexture.h"
#include "render/Font.h"
#include "render/PbrShader.h"
#include "render/RenderAssets.h"
#include "render/SpriteBatch.h"
#include "asset/MeshAsset.h"
#if WITH_RENDER_DX12_RAW
#    include "render/Dx12/Dx12Buffer.h"
#    include "render/Dx12/Dx12CommandList.h"
#    include "render/Dx12/Dx12Device.h"
#    include "render/Dx12/Dx12Pipeline.h"
#    include "render/Dx12/Dx12Shader.h"
#    include "render/Dx12/Dx12Texture.h"
#endif
#if WITH_RENDER_DILIGENT
#    include "render/Diligent/DiligentDevice.h"
#    include "render/Diligent/DiligentTexture.h"
#endif
#include "memory/UniquePtr.h"

using namespace acs;

ACS_TEST(Render, Utf8DecoderRejectsNonCanonicalScalars)
{
    const char valid[] = {
        static_cast<char>(0xF0), static_cast<char>(0x9F),
        static_cast<char>(0x98), static_cast<char>(0x80), '\0'};
    const char* cursor = valid;
    u32 codepoint = 0;
    EXPECT_TRUE(TryDecodeUtf8(&cursor, codepoint));
    EXPECT_EQ(codepoint, 0x1F600u);
    EXPECT_TRUE(*cursor == '\0');

    const char replacement[] = {
        static_cast<char>(0xEF), static_cast<char>(0xBF),
        static_cast<char>(0xBD), '\0'};
    cursor = replacement;
    EXPECT_TRUE(TryDecodeUtf8(&cursor, codepoint));
    EXPECT_EQ(codepoint, 0xFFFDu);
    EXPECT_TRUE(*cursor == '\0');

    const char overlong[] = {
        static_cast<char>(0xC0), static_cast<char>(0xAF), '\0'};
    cursor = overlong;
    EXPECT_FALSE(TryDecodeUtf8(&cursor, codepoint));
    EXPECT_EQ(codepoint, 0xFFFDu);
    EXPECT_TRUE(cursor == overlong + 1);

    const char surrogate[] = {
        static_cast<char>(0xED), static_cast<char>(0xA0),
        static_cast<char>(0x80), '\0'};
    cursor = surrogate;
    EXPECT_FALSE(TryDecodeUtf8(&cursor, codepoint));
    EXPECT_EQ(codepoint, 0xFFFDu);
    EXPECT_TRUE(cursor == surrogate + 1);

    const char out_of_range[] = {
        static_cast<char>(0xF4), static_cast<char>(0x90),
        static_cast<char>(0x80), static_cast<char>(0x80), '\0'};
    cursor = out_of_range;
    EXPECT_FALSE(TryDecodeUtf8(&cursor, codepoint));
    EXPECT_EQ(codepoint, 0xFFFDu);
    EXPECT_TRUE(cursor == out_of_range + 1);

    const char truncated[] = {
        static_cast<char>(0xF0), static_cast<char>(0x9F), '\0'};
    cursor = truncated;
    EXPECT_FALSE(TryDecodeUtf8(&cursor, codepoint));
    EXPECT_EQ(codepoint, 0xFFFDu);
    EXPECT_TRUE(cursor == truncated + 2);

    cursor = replacement;
    EXPECT_EQ(DecodeUtf8(&cursor), 0xFFFDu);
    EXPECT_TRUE(*cursor == '\0');
}

ACS_TEST(Render, FontFailedReloadReturnsToEmptyState)
{
    FDeviceConfig config{};
    auto device_result = CreateRhiDevice(config);
    if (device_result.IsErr()) return; // GPU が使えない環境ではスキップする。

    FFont font;
    const wchar_t* candidates[] = {
        L"C:/Windows/Fonts/segoeui.ttf",
        L"C:/Windows/Fonts/arial.ttf",
        L"C:/Windows/Fonts/meiryo.ttc",
    };
    bool loaded = false;
    for (const wchar_t* path : candidates) {
        if (font.LoadFromFile(*device_result.Value(), path, 18.0f, 512, false).IsOk()) {
            loaded = true;
            break;
        }
    }
    if (!loaded) return; // 利用可能なシステムフォントが無ければスキップする。

    EXPECT_TRUE(font.AtlasTexture() != nullptr);
    EXPECT_TRUE(font.Ascent() != 0.0f);

    // ファイル読込段階の失敗でも、以前のatlas・glyph・メトリクスを公開し続けない。
    const auto failed_reload = font.LoadFromFile(*device_result.Value(), L"C:/__acs_missing_font_contract__/font.ttf",
                                                 18.0f);
    EXPECT_TRUE(failed_reload.IsErr());
    EXPECT_TRUE(font.AtlasTexture() == nullptr);
    EXPECT_EQ(font.AtlasSize(), 0u);
    EXPECT_EQ(font.PixelSize(), 0.0f);
    EXPECT_EQ(font.Ascent(), 0.0f);
    EXPECT_EQ(font.Descent(), 0.0f);
    EXPECT_EQ(font.LineGap(), 0.0f);
    EXPECT_EQ(font.LineHeight(), 0.0f);
    FGlyphInfo glyph{};
    EXPECT_FALSE(font.GetGlyph(static_cast<u32>('A'), glyph));
}

ACS_TEST(Render, SpriteBatchRejectsMaxSpritesBeyondU16IndexLimit)
{
    FDeviceConfig dcfg{};
    auto dev_r = CreateRhiDevice(dcfg);
    if (dev_r.IsErr()) {
        // GPU / D3D12 が無い環境 (CI 等) ではスキップ。
        return;
    }
    IRhiDevice& dev = *dev_r.Value();

    // u16 インデックス上限 (16384 sprite = 65536 頂点) を超える max_sprites は拒否される。
    // 許すと idx_count=u32(max_sprites*6) の overflow / index wrap で heap overflow し得る。
    FSpriteBatch OversizedBatch;
    EXPECT_TRUE(OversizedBatch.Init(dev, EFormat::B8G8R8A8_UNorm, 16385u, 1u).IsErr());

    // 上限ちょうどは許容される。
    FSpriteBatch BoundaryBatch;
    EXPECT_TRUE(BoundaryBatch.Init(dev, EFormat::B8G8R8A8_UNorm, 16384u, 1u).IsOk());
    BoundaryBatch.Shutdown();
}

ACS_TEST(Render, TextureArrayCubemapMip)
{
    FDeviceConfig dcfg{};
    dcfg.enable_debug_layer = true; // debug layer があれば view desc も検証される (best-effort)
    auto dev_r = CreateRhiDevice(dcfg);
    if (dev_r.IsErr()) {
        // GPU / D3D12 が使えない環境 (CI 等) ではスキップ。テスト自体は失敗にしない。
        return;
    }
    IRhiDevice& dev = *dev_r.Value();

    // --- cubemap (6 面) + 複数 mip + per-slice RTV (IBL prefilter 相当) ---
    {
        FTextureDesc d{};
        d.width = 64;
        d.height = 64;
        d.format = EFormat::R16G16B16A16_Float;
        d.array_size = 6;
        d.is_cubemap = true;
        d.mip_levels = 4;
        d.is_render_target = true;
        d.per_slice_rtv = true;
        auto t_r = CreateRhiTexture(dev, d);
        EXPECT_TRUE(t_r.IsOk());
        if (t_r.IsOk()) {
            IRhiTexture& t = *t_r.Value();
            EXPECT_EQ(t.ArraySize(), 6u);
            EXPECT_TRUE(t.IsCubemap());
            EXPECT_EQ(t.MipLevels(), 4u);
            EXPECT_EQ(t.Width(), 64u);
        }
    }

    // --- 2D テクスチャ配列 (cube でない) + per-slice RTV ---
    {
        FTextureDesc d{};
        d.width = 32;
        d.height = 32;
        d.format = EFormat::R8G8B8A8_UNorm;
        d.array_size = 4;
        d.mip_levels = 1;
        d.is_render_target = true;
        d.per_slice_rtv = true;
        auto t_r = CreateRhiTexture(dev, d);
        EXPECT_TRUE(t_r.IsOk());
        if (t_r.IsOk()) {
            EXPECT_EQ(t_r.Value()->ArraySize(), 4u);
            EXPECT_FALSE(t_r.Value()->IsCubemap());
        }
    }

    // --- 単一 2D + 複数 mip (initial_data 無し) ---
    {
        FTextureDesc d{};
        d.width = 16;
        d.height = 16;
        d.format = EFormat::R8G8B8A8_UNorm;
        d.mip_levels = 4;
        auto t_r = CreateRhiTexture(dev, d);
        EXPECT_TRUE(t_r.IsOk());
        if (t_r.IsOk()) EXPECT_EQ(t_r.Value()->MipLevels(), 4u);
    }

    // --- 不正: cubemap で array_size が 6 の倍数でない → honest にエラー ---
    {
        FTextureDesc d{};
        d.width = 16;
        d.height = 16;
        d.array_size = 5;
        d.is_cubemap = true;
        auto t_r = CreateRhiTexture(dev, d);
        EXPECT_TRUE(t_r.IsErr());
    }

    // --- 不正: per_slice_rtv だが RT でない → honest にエラー ---
    {
        FTextureDesc d{};
        d.width = 16;
        d.height = 16;
        d.array_size = 2;
        d.per_slice_rtv = true;
        d.is_render_target = false;
        auto t_r = CreateRhiTexture(dev, d);
        EXPECT_TRUE(t_r.IsErr());
    }
}

#if WITH_RENDER_DX12_RAW
#    if !WITH_RENDER_DILIGENT
ACS_TEST(Render, RawDx12PbrSh9FallbackPreservesBaseColor)
{
    FDx12Device device;
    FDeviceConfig config{};
    if (device.Init(config).IsErr()) {
        // GPU / DX12 が使えない CI では既存 GPU tests と同様にスキップする。
        return;
    }

    auto command_result = CreateRhiCommandList(device);
    EXPECT_TRUE(command_result.IsOk());
    if (command_result.IsErr()) return;
    TUniquePtr<IRhiCommandList> command = Move(command_result.Value());

    FTextureDesc color_desc{};
    color_desc.width = 32;
    color_desc.height = 32;
    color_desc.format = EFormat::R8G8B8A8_UNorm;
    color_desc.is_render_target = true;
    auto color_result = CreateRhiTexture(device, color_desc);
    EXPECT_TRUE(color_result.IsOk());
    if (color_result.IsErr()) return;

    FTextureDesc depth_desc{};
    depth_desc.width = 32;
    depth_desc.height = 32;
    depth_desc.format = EFormat::D32_Float;
    depth_desc.is_depth_target = true;
    auto depth_result = CreateRhiTexture(device, depth_desc);
    EXPECT_TRUE(depth_result.IsOk());
    if (depth_result.IsErr()) return;

    FMeshAsset triangle;
    triangle.Vertices().PushBack(
        FMeshVertex{FVec3{-0.8f, -0.8f, 0.5f}, FVec3{0, 0, 1}, 0, 1});
    triangle.Vertices().PushBack(
        FMeshVertex{FVec3{0.0f, 0.8f, 0.5f}, FVec3{0, 0, 1}, 0.5f, 0});
    triangle.Vertices().PushBack(
        FMeshVertex{FVec3{0.8f, -0.8f, 0.5f}, FVec3{0, 0, 1}, 1, 1});
    triangle.Indices().PushBack(0);
    triangle.Indices().PushBack(1);
    triangle.Indices().PushBack(2);
    FGpuMesh gpu_mesh{};
    EXPECT_TRUE(UploadMesh(device, triangle, gpu_mesh).IsOk());
    if (!gpu_mesh.vertex_buffer || !gpu_mesh.index_buffer) return;

    FPbrShader shader;
    EXPECT_TRUE(shader.Init(
        device, EFormat::R8G8B8A8_UNorm, EFormat::D32_Float,
        ECullMode::None).IsOk());
    FDirLight light{};
    light.direction = FVec3{0, 0, 1};
    light.color = FVec3{1.5f, 1.5f, 1.5f};
    EXPECT_TRUE(shader.BeginFrame(1u));
    shader.SetLights(
        FMat4::Identity(), FVec3{0, 0, 2}, &light, 1,
        FVec3{0.2f, 0.2f, 0.2f});
    FVec4 sh9[9]{};
    sh9[0] = FVec4{0.45f, 0.45f, 0.45f, 0};
    shader.SetSh9(sh9);
    shader.SetIbl(nullptr, nullptr, nullptr, 0);
    shader.SetShadowMap(nullptr, FMat4::Identity());
    shader.SetSsao(nullptr, 0.0f, 32, 32);

    command->Begin();
    command->BeginRenderToTexture(
        *color_result.Value(), FClearColor{0, 0, 0, 1},
        depth_result.Value().Get(), 1.0f);
    EXPECT_TRUE(shader.DrawMesh(
        *command, gpu_mesh, FMat4::Identity(),
        FVec3{0.85f, 0.20f, 0.10f}, 0.0f, 0.5f, 1.0f));
    command->EndRenderToTexture(*color_result.Value());
    command->End();
    command->Submit();
    device.WaitIdle();

    u8 pixels[32 * 32 * 4]{};
    EXPECT_TRUE(device.ReadTexture(
        *color_result.Value(), pixels, static_cast<u32>(sizeof(pixels))));
    const usize center = (16u * 32u + 16u) * 4u;
    EXPECT_TRUE(pixels[center + 0u] >= 40u);
    EXPECT_TRUE(pixels[center + 0u] > pixels[center + 1u] * 2u);
    EXPECT_TRUE(pixels[center + 1u] > pixels[center + 2u]);
}

ACS_TEST(Render, RawDx12MrtDoesNotCorruptPreviouslyBoundTarget)
{
    FDx12Device device;
    FDeviceConfig config{};
    if (device.Init(config).IsErr()) return;

    auto command_result = CreateRhiCommandList(device);
    EXPECT_TRUE(command_result.IsOk());
    if (command_result.IsErr()) return;
    TUniquePtr<IRhiCommandList> command = Move(command_result.Value());

    FTextureDesc target_desc{};
    target_desc.width = 16;
    target_desc.height = 16;
    target_desc.format = EFormat::R8G8B8A8_UNorm;
    target_desc.is_render_target = true;
    auto sentinel_result = CreateRhiTexture(device, target_desc);
    auto first_result = CreateRhiTexture(device, target_desc);
    auto second_result = CreateRhiTexture(device, target_desc);
    EXPECT_TRUE(sentinel_result.IsOk());
    EXPECT_TRUE(first_result.IsOk());
    EXPECT_TRUE(second_result.IsOk());
    if (sentinel_result.IsErr() || first_result.IsErr() || second_result.IsErr()) return;

    static const char* vertex_source = R"(
struct VSOut { float4 pos : SV_POSITION; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
})";
    static const char* pixel_source = R"(
struct PSOut {
    float4 first  : SV_TARGET0;
    float4 second : SV_TARGET1;
};
PSOut PSMain() {
    PSOut o;
    o.first  = float4(0.10, 0.80, 0.20, 1.0);
    o.second = float4(0.15, 0.25, 0.90, 1.0);
    return o;
})";

    FShaderDesc vertex_desc{};
    vertex_desc.stage = EShaderStage::Vertex;
    vertex_desc.hlsl_source = vertex_source;
    vertex_desc.entry_point = "VSMain";
    auto vertex_result = CreateRhiShader(device, vertex_desc);
    FShaderDesc pixel_desc{};
    pixel_desc.stage = EShaderStage::Pixel;
    pixel_desc.hlsl_source = pixel_source;
    pixel_desc.entry_point = "PSMain";
    auto pixel_result = CreateRhiShader(device, pixel_desc);
    EXPECT_TRUE(vertex_result.IsOk());
    EXPECT_TRUE(pixel_result.IsOk());
    if (vertex_result.IsErr() || pixel_result.IsErr()) return;

    FPipelineDesc pipeline_desc{};
    pipeline_desc.vs = vertex_result.Value().Get();
    pipeline_desc.ps = pixel_result.Value().Get();
    pipeline_desc.topology = EPrimitiveTopology::TriangleList;
    pipeline_desc.rt_count = 2;
    pipeline_desc.rt_formats[0] = EFormat::R8G8B8A8_UNorm;
    pipeline_desc.rt_formats[1] = EFormat::R8G8B8A8_UNorm;
    pipeline_desc.cull_mode = ECullMode::None;
    auto pipeline_result = CreateRhiPipeline(device, pipeline_desc);
    EXPECT_TRUE(pipeline_result.IsOk());
    if (pipeline_result.IsErr()) return;

    command->Begin();
    command->BeginRenderToTexture(
        *sentinel_result.Value(), FClearColor{1, 0, 0, 1}, nullptr, 1.0f);
    command->EndRenderToTexture(*sentinel_result.Value());

    IRhiTexture* mrt[2] = {first_result.Value().Get(), second_result.Value().Get()};
    EXPECT_FALSE(command->BeginRenderToTextureMrt(
        nullptr, 2u, FClearColor{0, 0, 0, 1}, nullptr, 1.0f));
    EXPECT_FALSE(command->BeginRenderToTextureMrtLoad(
        nullptr, 2u, FClearColor{0, 0, 0, 1}, 0u, nullptr, false,
        1.0f));
    EXPECT_TRUE(command->BeginRenderToTextureMrt(
        mrt, 2u, FClearColor{0, 0, 0, 1}, nullptr, 1.0f));
    command->SetPipeline(*pipeline_result.Value());
    command->Draw(3, 0);
    command->EndRenderToTextureMrt(mrt, 2u);
    command->End();
    command->Submit();
    device.WaitIdle();

    u8 sentinel[16 * 16 * 4]{};
    u8 first[16 * 16 * 4]{};
    u8 second[16 * 16 * 4]{};
    EXPECT_TRUE(device.ReadTexture(
        *sentinel_result.Value(), sentinel, static_cast<u32>(sizeof(sentinel))));
    EXPECT_TRUE(device.ReadTexture(
        *first_result.Value(), first, static_cast<u32>(sizeof(first))));
    EXPECT_TRUE(device.ReadTexture(
        *second_result.Value(), second, static_cast<u32>(sizeof(second))));
    constexpr usize center = (8u * 16u + 8u) * 4u;
    EXPECT_TRUE(sentinel[center + 0u] > 240u);
    EXPECT_TRUE(sentinel[center + 1u] < 8u);
    EXPECT_TRUE(first[center + 1u] > first[center + 0u] * 4u);
    EXPECT_TRUE(first[center + 1u] > first[center + 2u] * 3u);
    EXPECT_TRUE(second[center + 2u] > second[center + 0u] * 4u);
    EXPECT_TRUE(second[center + 2u] > second[center + 1u] * 3u);
}

ACS_TEST(Render, RawDx12MixedFormatMrtSupportsCloudHistoryTargets)
{
    FDx12Device device;
    FDeviceConfig config{};
    if (device.Init(config).IsErr()) return;

    auto command_result = CreateRhiCommandList(device);
    EXPECT_TRUE(command_result.IsOk());
    if (command_result.IsErr()) return;
    TUniquePtr<IRhiCommandList> command = Move(command_result.Value());

    FTextureDesc color_desc{};
    color_desc.width = 16;
    color_desc.height = 16;
    color_desc.format = EFormat::R16G16B16A16_Float;
    color_desc.is_render_target = true;
    auto color_result = CreateRhiTexture(device, color_desc);

    FTextureDesc depth_desc{};
    depth_desc.width = 16;
    depth_desc.height = 16;
    depth_desc.format = EFormat::R32G32_Float;
    depth_desc.is_render_target = true;
    auto depth_result = CreateRhiTexture(device, depth_desc);
    EXPECT_TRUE(color_result.IsOk());
    EXPECT_TRUE(depth_result.IsOk());
    if (color_result.IsErr() || depth_result.IsErr()) return;

    static const char* vertex_source = R"(
struct VSOut { float4 pos : SV_POSITION; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
})";
    static const char* pixel_source = R"(
struct PSOut {
    float4 color : SV_TARGET0;
    float2 depth : SV_TARGET1;
};
PSOut PSMain() {
    PSOut o;
    o.color = float4(0.5, 0.25, 0.75, 1.0);
    o.depth = float2(1234.5, 0.625);
    return o;
})";

    FShaderDesc vertex_desc{};
    vertex_desc.stage = EShaderStage::Vertex;
    vertex_desc.hlsl_source = vertex_source;
    vertex_desc.entry_point = "VSMain";
    auto vertex_result = CreateRhiShader(device, vertex_desc);
    FShaderDesc pixel_desc{};
    pixel_desc.stage = EShaderStage::Pixel;
    pixel_desc.hlsl_source = pixel_source;
    pixel_desc.entry_point = "PSMain";
    auto pixel_result = CreateRhiShader(device, pixel_desc);
    EXPECT_TRUE(vertex_result.IsOk());
    EXPECT_TRUE(pixel_result.IsOk());
    if (vertex_result.IsErr() || pixel_result.IsErr()) return;

    FPipelineDesc pipeline_desc{};
    pipeline_desc.vs = vertex_result.Value().Get();
    pipeline_desc.ps = pixel_result.Value().Get();
    pipeline_desc.topology = EPrimitiveTopology::TriangleList;
    pipeline_desc.rt_count = 2;
    pipeline_desc.rt_formats[0] = EFormat::R16G16B16A16_Float;
    pipeline_desc.rt_formats[1] = EFormat::R32G32_Float;
    pipeline_desc.cull_mode = ECullMode::None;
    auto pipeline_result = CreateRhiPipeline(device, pipeline_desc);
    EXPECT_TRUE(pipeline_result.IsOk());
    if (pipeline_result.IsErr()) return;

    command->Begin();
    IRhiTexture* targets[2] = {
        color_result.Value().Get(), depth_result.Value().Get()};
    EXPECT_TRUE(command->BeginRenderToTextureMrt(
        targets, 2u, FClearColor{0, 0, 0, 0}, nullptr, 1.0f));
    command->SetPipeline(*pipeline_result.Value());
    command->Draw(3, 0);
    command->EndRenderToTextureMrt(targets, 2u);
    command->End();
    command->Submit();
    device.WaitIdle();

    u16 color[16 * 16 * 4]{};
    f32 depth[16 * 16 * 2]{};
    EXPECT_TRUE(device.ReadTexture(
        *color_result.Value(), color, static_cast<u32>(sizeof(color))));
    EXPECT_TRUE(device.ReadTexture(
        *depth_result.Value(), depth, static_cast<u32>(sizeof(depth))));
    constexpr usize color_center = (8u * 16u + 8u) * 4u;
    constexpr usize depth_center = (8u * 16u + 8u) * 2u;
    EXPECT_EQ(color[color_center + 0u], static_cast<u16>(0x3800u));
    EXPECT_EQ(color[color_center + 1u], static_cast<u16>(0x3400u));
    EXPECT_EQ(color[color_center + 2u], static_cast<u16>(0x3A00u));
    EXPECT_EQ(color[color_center + 3u], static_cast<u16>(0x3C00u));
    EXPECT_NEAR(depth[depth_center + 0u], 1234.5f, 0.001f);
    EXPECT_NEAR(depth[depth_center + 1u], 0.625f, 0.0001f);
}

ACS_TEST(Render, Dx12ComputePipelineRejectsMissingShader)
{
    FDx12Device device;
    FComputePipelineDesc description{};
    auto result = CreateRhiComputePipeline(device, description);
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(static_cast<u16>(result.Error().category), static_cast<u16>(EErrCategory::Render));
    }
}

ACS_TEST(Render, RawDx12DefersOpenListResourceAndDescriptorRetirement)
{
    FDx12Device device;
    FDeviceConfig config{};
    if (device.Init(config).IsErr()) return;

    FDx12CommandList command;
    EXPECT_TRUE(command.Init(device).IsOk());
    if (command.NativeHandle() == nullptr) return;

    FTextureDesc texture_desc{};
    texture_desc.width = 16u;
    texture_desc.height = 16u;
    texture_desc.format = EFormat::R8G8B8A8_UNorm;
    texture_desc.is_render_target = true;
    auto old_texture = CreateRhiTexture(device, texture_desc);
    EXPECT_TRUE(old_texture.IsOk());

    const f32 vertices[6] = {
        -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, -1.0f};
    FBufferDesc buffer_desc{};
    buffer_desc.size = sizeof(vertices);
    buffer_desc.usage = EBufferUsage::Vertex;
    buffer_desc.cpu_writable = true;
    buffer_desc.initial_data = vertices;
    auto old_buffer = CreateRhiBuffer(device, buffer_desc);
    EXPECT_TRUE(old_buffer.IsOk());
    if (old_texture.IsErr() || old_buffer.IsErr()) return;

    auto* dx_texture =
        static_cast<FDx12Texture*>(old_texture.Value().Get());
    auto* dx_buffer =
        static_cast<FDx12Buffer*>(old_buffer.Value().Get());
    ID3D12Resource* old_texture_resource = dx_texture->Resource();
    ID3D12Resource* old_buffer_resource = dx_buffer->Resource();
    const UINT64 old_srv = dx_texture->SrvGpuHandle().ptr;
    const SIZE_T old_rtv = dx_texture->RtvCpuHandle().ptr;
    EXPECT_TRUE(old_texture_resource != nullptr);
    EXPECT_TRUE(old_buffer_resource != nullptr);
    EXPECT_TRUE(old_srv != 0u);
    EXPECT_TRUE(old_rtv != 0u);

    command.Begin();
    command.BeginRenderToTexture(
        *old_texture.Value(), FClearColor{0, 0, 0, 1}, nullptr, 1.0f);
    command.SetVertexBuffer(*old_buffer.Value(), sizeof(f32) * 2u);
    command.EndRenderToTexture(*old_texture.Value());

    // These objects are gone from the editor-facing graph while the command
    // list which references them is still open and has not been submitted.
    old_texture.Value().Reset();
    old_buffer.Value().Reset();
    EXPECT_EQ(device.RetiredResourceCount(), static_cast<usize>(2u));

    // Calling AddRef after transfer is a direct lifetime probe. The retirement
    // queue, not the destroyed wrapper, still owns the original COM reference.
    EXPECT_TRUE(old_texture_resource->AddRef() >= 2u);
    old_texture_resource->Release();
    EXPECT_TRUE(old_buffer_resource->AddRef() >= 2u);
    old_buffer_resource->Release();

    // The old descriptor slots cannot be recycled before the submission fence
    // because recorded RTV/descriptor-table handles identify slots, not views.
    auto replacement = CreateRhiTexture(device, texture_desc);
    EXPECT_TRUE(replacement.IsOk());
    if (replacement.IsErr()) return;
    auto* replacement_texture =
        static_cast<FDx12Texture*>(replacement.Value().Get());
    EXPECT_TRUE(replacement_texture->SrvGpuHandle().ptr != old_srv);
    EXPECT_TRUE(replacement_texture->RtvCpuHandle().ptr != old_rtv);

    // A transient upload executes and signals before the still-open main list.
    // Its earlier fence must not seal resources referenced by that main list.
    FBufferDesc one_off_upload_desc{};
    one_off_upload_desc.size = sizeof(vertices);
    one_off_upload_desc.usage = EBufferUsage::Vertex;
    one_off_upload_desc.initial_data = vertices;
    auto one_off_upload =
        CreateRhiBuffer(device, one_off_upload_desc);
    EXPECT_TRUE(one_off_upload.IsOk());
    device.CollectRetiredResources();
    EXPECT_EQ(device.RetiredResourceCount(), static_cast<usize>(2u));
    if (device.RetiredResourceCount() == static_cast<usize>(2u)) {
        EXPECT_TRUE(old_texture_resource->AddRef() >= 2u);
        old_texture_resource->Release();
        EXPECT_TRUE(old_buffer_resource->AddRef() >= 2u);
        old_buffer_resource->Release();
    }

    command.End();
    command.Submit();
    device.WaitIdle();
    EXPECT_EQ(device.RetiredResourceCount(), static_cast<usize>(0u));

    // Once the fence completes, the free lists may safely recycle both slots.
    auto recycled = CreateRhiTexture(device, texture_desc);
    EXPECT_TRUE(recycled.IsOk());
    if (recycled.IsErr()) return;
    auto* recycled_texture =
        static_cast<FDx12Texture*>(recycled.Value().Get());
    EXPECT_EQ(recycled_texture->SrvGpuHandle().ptr, old_srv);
    EXPECT_EQ(recycled_texture->RtvCpuHandle().ptr, old_rtv);

    replacement.Value().Reset();
    recycled.Value().Reset();
    EXPECT_EQ(device.RetiredResourceCount(), static_cast<usize>(2u));
    // Generic WaitIdle signals do not claim pending main-list retirements.
    // An empty main submission is sufficient when no draw references remain.
    command.Begin();
    command.End();
    command.Submit();
    device.WaitIdle();
    EXPECT_EQ(device.RetiredResourceCount(), static_cast<usize>(0u));
}

ACS_TEST(Render, RawDx12ReinitializeDrainsRetirementQueue)
{
    FDx12Device device;
    FDeviceConfig config{};
    if (device.Init(config).IsErr()) return;

    FTextureDesc texture_desc{};
    texture_desc.width = 8u;
    texture_desc.height = 8u;
    texture_desc.is_render_target = true;
    auto texture = CreateRhiTexture(device, texture_desc);
    EXPECT_TRUE(texture.IsOk());
    if (texture.IsErr()) return;

    texture.Value().Reset();
    EXPECT_EQ(device.RetiredResourceCount(), static_cast<usize>(1u));

    // Init begins with the same Reset path as destruction. A successful final
    // Signal must drain the retirement record before heaps/device are replaced.
    EXPECT_TRUE(device.Init(config).IsOk());
    EXPECT_EQ(device.RetiredResourceCount(), static_cast<usize>(0u));
}
#    endif

ACS_TEST(Render, Dx12ReinitializeAndRollback)
{
    FDx12Device device;
    FDeviceConfig config{};
    config.enable_debug_layer = true;
    if (device.Init(config).IsErr()) {
        // GPU / DX12 が使えない CI では既存テストと同様にスキップする。
        return;
    }

    const auto submit_retirement_fence =
        [&device](FDx12CommandList& command_list) {
            command_list.Begin();
            command_list.End();
            command_list.Submit();
            device.WaitIdle();
        };

    {
        FDx12Texture texture;
        FDx12CommandList retirement_command;
        EXPECT_TRUE(retirement_command.Init(device).IsOk());

        // 公開ファクトリが Diligent を選ぶ構成でも、raw DX12 の契約を直接検証する。
        FTextureDesc invalid{};
        invalid.width = 16;
        invalid.height = 16;
        invalid.array_size = 2;
        invalid.per_slice_rtv = true;
        invalid.is_render_target = false;
        EXPECT_TRUE(texture.Init(device, invalid).IsErr());
        EXPECT_TRUE(texture.Resource() == nullptr);
        EXPECT_FALSE(texture.HasRtv());

        FTextureDesc render_target{};
        render_target.width = 16;
        render_target.height = 16;
        render_target.format = EFormat::R8G8B8A8_UNorm;
        render_target.array_size = 6;
        render_target.is_cubemap = true;
        render_target.mip_levels = 2;
        render_target.is_render_target = true;
        render_target.per_slice_rtv = true;

        // 1 回 12 RTV を使うため、旧実装なら 22 回目までに 256 枠が枯渇する。
        // Reset が全スロットを返却していれば、同じインスタンスを 32 回再利用できる。
        for (u32 i = 0; i < 32; ++i) {
            EXPECT_TRUE(texture.Init(device, render_target).IsOk());
            EXPECT_TRUE(texture.Resource() != nullptr);
            EXPECT_TRUE(texture.HasRtv());
            submit_retirement_fence(retirement_command);
        }

        // 成功後の失敗でも、直前のリソースを残さず空状態へ戻る。
        EXPECT_TRUE(texture.Init(device, invalid).IsErr());
        EXPECT_TRUE(texture.Resource() == nullptr);
        EXPECT_FALSE(texture.HasSrv());
        EXPECT_FALSE(texture.HasRtv());
        submit_retirement_fence(retirement_command);
    }

    {
        const u32 initial_data[4] = {1u, 2u, 3u, 4u};
        FDx12Buffer buffer;
        FDx12CommandList retirement_command;
        EXPECT_TRUE(retirement_command.Init(device).IsOk());
        FBufferDesc static_desc{};
        static_desc.size = sizeof(initial_data);
        static_desc.initial_data = initial_data;
        EXPECT_TRUE(buffer.Init(device, static_desc).IsOk());

        FBufferDesc dynamic_desc{};
        dynamic_desc.size = sizeof(initial_data);
        dynamic_desc.cpu_writable = true;
        dynamic_desc.initial_data = initial_data;
        for (u32 i = 0; i < 16; ++i) {
            EXPECT_TRUE(buffer.Init(device, dynamic_desc).IsOk());
            EXPECT_TRUE(buffer.Resource() != nullptr);
            submit_retirement_fence(retirement_command);
        }

        FBufferDesc invalid_desc{};
        EXPECT_TRUE(buffer.Init(device, invalid_desc).IsErr());
        EXPECT_TRUE(buffer.Resource() == nullptr);
        EXPECT_EQ(buffer.Size(), static_cast<usize>(0));
        submit_retirement_fence(retirement_command);
    }

    {
        static const char* vertex_source = "float4 main(uint vertex_identifier : SV_VertexID) : SV_Position {"
                                           "  return float4(vertex_identifier == 1 ? 1.0 : -1.0,"
                                           "                vertex_identifier == 2 ? 1.0 : -1.0, 0.0, 1.0);"
                                           "}";
        static const char* pixel_source = "float4 main() : SV_Target { return float4(1.0, 1.0, 1.0, 1.0); }";

        FDx12Shader vertex_shader;
        FDx12Shader pixel_shader;
        FShaderDesc vertex_desc{};
        vertex_desc.stage = EShaderStage::Vertex;
        vertex_desc.hlsl_source = vertex_source;
        FShaderDesc pixel_desc{};
        pixel_desc.stage = EShaderStage::Pixel;
        pixel_desc.hlsl_source = pixel_source;

        for (u32 i = 0; i < 8; ++i) {
            EXPECT_TRUE(vertex_shader.Init(vertex_desc).IsOk());
            EXPECT_TRUE(pixel_shader.Init(pixel_desc).IsOk());
        }

        FDx12Pipeline pipeline;
        FPipelineDesc pipeline_desc{};
        pipeline_desc.vs = &vertex_shader;
        pipeline_desc.ps = &pixel_shader;
        pipeline_desc.rt_format = EFormat::R8G8B8A8_UNorm;
        for (u32 i = 0; i < 8; ++i) {
            EXPECT_TRUE(pipeline.Init(device, pipeline_desc).IsOk());
            EXPECT_TRUE(pipeline.Pso() != nullptr);
            EXPECT_TRUE(pipeline.RootSignature() != nullptr);
        }

        pipeline_desc.layout_count = 9;
        EXPECT_TRUE(pipeline.Init(device, pipeline_desc).IsErr());
        EXPECT_TRUE(pipeline.Pso() == nullptr);
        EXPECT_TRUE(pipeline.RootSignature() == nullptr);

        FShaderDesc invalid_shader{};
        invalid_shader.hlsl_source = nullptr;
        EXPECT_TRUE(vertex_shader.Init(invalid_shader).IsErr());
        EXPECT_TRUE(vertex_shader.Bytecode() == nullptr);
    }

    {
        FDx12CommandList command_list;
        for (u32 i = 0; i < 8; ++i) {
            EXPECT_TRUE(command_list.Init(device).IsOk());
            EXPECT_TRUE(command_list.NativeHandle() != nullptr);
        }
    }

    // 子リソースをすべて破棄した後なら、デバイス本体も同一インスタンスで再初期化できる。
    EXPECT_TRUE(device.Init(config).IsOk());
}
#endif

#if WITH_RENDER_DILIGENT
ACS_TEST(Render, DiligentTextureReinitializeAndRollback)
{
    FDiligentDevice device;
    FDeviceConfig config{};
    config.enable_debug_layer = true;
    if (device.Init(config).IsErr()) return;

    FDiligentTexture texture;
    FTextureDesc invalid{};
    invalid.width = 16;
    invalid.height = 16;
    invalid.array_size = 2;
    invalid.per_slice_rtv = true;
    EXPECT_TRUE(texture.Init(device, invalid).IsErr());
    EXPECT_TRUE(texture.Native() == nullptr);

    FTextureDesc render_target{};
    render_target.width = 16;
    render_target.height = 16;
    render_target.format = EFormat::R8G8B8A8_UNorm;
    render_target.array_size = 6;
    render_target.is_cubemap = true;
    render_target.mip_levels = 2;
    render_target.is_render_target = true;
    render_target.per_slice_rtv = true;

    for (u32 iteration = 0; iteration < 32; ++iteration) {
        EXPECT_TRUE(texture.Init(device, render_target).IsOk());
        EXPECT_TRUE(texture.Native() != nullptr);
        EXPECT_TRUE(texture.RtvSlice(5, 1) != nullptr);
    }

    EXPECT_TRUE(texture.Init(device, invalid).IsErr());
    EXPECT_TRUE(texture.Native() == nullptr);
    EXPECT_TRUE(texture.RtvSlice(0, 0) == nullptr);
}
#endif
