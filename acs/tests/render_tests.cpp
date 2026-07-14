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

ACS_TEST(Render, FontFailedReloadReturnsToEmptyState)
{
    DeviceConfig config{};
    auto device_result = CreateRhiDevice(config);
    if (device_result.IsErr()) return; // GPU が使えない環境ではスキップする。

    Font font;
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
    GlyphInfo glyph{};
    EXPECT_FALSE(font.GetGlyph(static_cast<u32>('A'), glyph));
}

ACS_TEST(Render, TextureArrayCubemapMip)
{
    DeviceConfig dcfg{};
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
ACS_TEST(Render, Dx12ComputePipelineReportsUnsupported)
{
    Dx12Device device;
    FComputePipelineDesc description{};
    auto result = CreateRhiComputePipeline(device, description);
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(static_cast<u16>(result.Error().category), static_cast<u16>(ErrCategory::Render));
        EXPECT_EQ(result.Error().subcode, static_cast<u16>(53));
    }
}
#    endif

ACS_TEST(Render, Dx12ReinitializeAndRollback)
{
    Dx12Device device;
    DeviceConfig config{};
    config.enable_debug_layer = true;
    if (device.Init(config).IsErr()) {
        // GPU / DX12 が使えない CI では既存テストと同様にスキップする。
        return;
    }

    {
        Dx12Texture texture;

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
        }

        // 成功後の失敗でも、直前のリソースを残さず空状態へ戻る。
        EXPECT_TRUE(texture.Init(device, invalid).IsErr());
        EXPECT_TRUE(texture.Resource() == nullptr);
        EXPECT_FALSE(texture.HasSrv());
        EXPECT_FALSE(texture.HasRtv());
    }

    {
        const u32 initial_data[4] = {1u, 2u, 3u, 4u};
        Dx12Buffer buffer;
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
        }

        FBufferDesc invalid_desc{};
        EXPECT_TRUE(buffer.Init(device, invalid_desc).IsErr());
        EXPECT_TRUE(buffer.Resource() == nullptr);
        EXPECT_EQ(buffer.Size(), static_cast<usize>(0));
    }

    {
        static const char* vertex_source = "float4 main(uint vertex_identifier : SV_VertexID) : SV_Position {"
                                           "  return float4(vertex_identifier == 1 ? 1.0 : -1.0,"
                                           "                vertex_identifier == 2 ? 1.0 : -1.0, 0.0, 1.0);"
                                           "}";
        static const char* pixel_source = "float4 main() : SV_Target { return float4(1.0, 1.0, 1.0, 1.0); }";

        Dx12Shader vertex_shader;
        Dx12Shader pixel_shader;
        FShaderDesc vertex_desc{};
        vertex_desc.stage = EShaderStage::Vertex;
        vertex_desc.hlsl_source = vertex_source;
        FShaderDesc pixel_desc{};
        pixel_desc.stage = EShaderStage::Pixel;
        pixel_desc.hlsl_source = pixel_source;

        for (u32 i = 0; i < 8; ++i) {
            EXPECT_TRUE(vertex_shader.Init(device, vertex_desc).IsOk());
            EXPECT_TRUE(pixel_shader.Init(device, pixel_desc).IsOk());
        }

        Dx12Pipeline pipeline;
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
        EXPECT_TRUE(vertex_shader.Init(device, invalid_shader).IsErr());
        EXPECT_TRUE(vertex_shader.Bytecode() == nullptr);
    }

    {
        Dx12CommandList command_list;
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
    DiligentDevice device;
    DeviceConfig config{};
    config.enable_debug_layer = true;
    if (device.Init(config).IsErr()) return;

    DiligentTexture texture;
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
