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
#include "memory/UniquePtr.h"

using namespace acs;

ACS_TEST(Render, TextureArrayCubemapMip) {
    DeviceConfig dcfg{};
    dcfg.enable_debug_layer = true;   // debug layer があれば view desc も検証される (best-effort)
    auto dev_r = CreateRhiDevice(dcfg);
    if (dev_r.IsErr()) {
        // GPU / D3D12 が使えない環境 (CI 等) ではスキップ。テスト自体は失敗にしない。
        return;
    }
    IRhiDevice& dev = *dev_r.Value();

    // --- cubemap (6 面) + 複数 mip + per-slice RTV (IBL prefilter 相当) ---
    {
        FTextureDesc d{};
        d.width = 64; d.height = 64;
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
        d.width = 32; d.height = 32;
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
        d.width = 16; d.height = 16;
        d.format = EFormat::R8G8B8A8_UNorm;
        d.mip_levels = 4;
        auto t_r = CreateRhiTexture(dev, d);
        EXPECT_TRUE(t_r.IsOk());
        if (t_r.IsOk()) EXPECT_EQ(t_r.Value()->MipLevels(), 4u);
    }

    // --- 不正: cubemap で array_size が 6 の倍数でない → honest にエラー ---
    {
        FTextureDesc d{};
        d.width = 16; d.height = 16;
        d.array_size = 5;
        d.is_cubemap = true;
        auto t_r = CreateRhiTexture(dev, d);
        EXPECT_TRUE(t_r.IsErr());
    }

    // --- 不正: per_slice_rtv だが RT でない → honest にエラー ---
    {
        FTextureDesc d{};
        d.width = 16; d.height = 16;
        d.array_size = 2;
        d.per_slice_rtv = true;
        d.is_render_target = false;
        auto t_r = CreateRhiTexture(dev, d);
        EXPECT_TRUE(t_r.IsErr());
    }
}
