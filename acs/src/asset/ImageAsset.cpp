// SPDX-License-Identifier: Apache-2.0
// 画像アセット実装（stb_image によるデコード）
#include "asset/ImageAsset.h"
#include "memory/Memory.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO         // ファイル I/O は使わない（メモリから読む）
#include <stb_image.h>

namespace acs {

TResult<TSharedPtr<FAsset>> FImageAssetLoader::LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept {
    int w = 0, h = 0, channels = 0;
    // HDR ファイル（.hdr など）は float ピクセル、それ以外は 8-bit に統一
    const bool is_hdr = ::stbi_is_hdr_from_memory(reinterpret_cast<const stbi_uc*>(bytes.Data()),
                                            static_cast<int>(bytes.Size())) != 0;

    if (is_hdr) {
        // HDR: 32-bit float RGBA に強制（4ch 固定で扱いを統一）
        float* px = ::stbi_loadf_from_memory(
            reinterpret_cast<const stbi_uc*>(bytes.Data()),
            static_cast<int>(bytes.Size()), &w, &h, &channels, 4);
        if (!px) return ACS_ERR(Asset, 100, "stbi_loadf_from_memory failed");

        const usize byte_count = static_cast<usize>(w) * h * 4 * sizeof(float);
        TArray<byte> pixels;
        pixels.Resize(byte_count);
        MemCopy(pixels.Data(), px, byte_count);
        ::stbi_image_free(px);

        TSharedPtr<FImageAsset> a = MakeShared<FImageAsset>(static_cast<u32>(w), static_cast<u32>(h),
                                               EPixelFormat::R32G32B32A32_F, Move(pixels));
        a->SetId(id);
        a->SetState(EAssetState::Ready);
        return TResult<TSharedPtr<FAsset>>(OkInit, TSharedPtr<FAsset>(Move(a)));
    }

    // LDR: 8-bit RGBA に強制
    stbi_uc* px = ::stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.Data()),
        static_cast<int>(bytes.Size()), &w, &h, &channels, 4);
    if (!px) return ACS_ERR(Asset, 101, "stbi_load_from_memory failed");

    const usize byte_count = static_cast<usize>(w) * h * 4;
    TArray<byte> pixels;
    pixels.Resize(byte_count);
    MemCopy(pixels.Data(), px, byte_count);
    ::stbi_image_free(px);

    TSharedPtr<FImageAsset> a = MakeShared<FImageAsset>(static_cast<u32>(w), static_cast<u32>(h),
                                           EPixelFormat::R8G8B8A8, Move(pixels));
    a->SetId(id);
    a->SetState(EAssetState::Ready);
    return TResult<TSharedPtr<FAsset>>(OkInit, TSharedPtr<FAsset>(Move(a)));
}

} // namespace acs
