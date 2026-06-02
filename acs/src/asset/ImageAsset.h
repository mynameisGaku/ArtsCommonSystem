// SPDX-License-Identifier: Apache-2.0
// 画像アセット
//
// 対応拡張子（stb_image 経由）:
//   png / jpg / jpeg / bmp / tga / gif / hdr / pic / pnm / ppm / pgm / psd
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "asset/Asset.h"
#include "asset/IAssetLoader.h"
#include "container/Hash.h"

namespace acs {

// ピクセルフォーマット（チャンネル数で識別）
enum class EPixelFormat : u8 {
    R8,           // 8-bit 単チャンネル（グレースケール）
    R8G8,         // 8-bit RG（グレースケール + アルファ）
    R8G8B8,       // 8-bit RGB
    R8G8B8A8,     // 8-bit RGBA
    R32G32B32_F,  // 32-bit float RGB（HDR）
    R32G32B32A32_F, // 32-bit float RGBA（HDR）
};

// 画像 1 枚分のデータ（CPU 側のピクセル列）
class FImageAsset : public Asset {
public:
    ACS_ASSET_TYPE("FImageAsset")

    FImageAsset() noexcept = default;
    FImageAsset(u32 w, u32 h, EPixelFormat fmt, TArray<byte>&& pixels) noexcept
        : m_Width(w), m_Height(h), m_Format(fmt), m_Pixels(Move(pixels)) {}

    u32         Width()  const noexcept { return m_Width; }
    u32         Height() const noexcept { return m_Height; }
    EPixelFormat EFormat() const noexcept { return m_Format; }
    const byte* Pixels() const noexcept { return m_Pixels.Data(); }
    usize       PixelByteCount() const noexcept { return m_Pixels.Size(); }

private:
    u32         m_Width  = 0;
    u32         m_Height = 0;
    EPixelFormat m_Format = EPixelFormat::R8G8B8A8;
    TArray<byte> m_Pixels;
};

// 画像ローダ (stb_image)
class ImageAssetLoader final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return FImageAsset::StaticType(); }
    const char* Extension() const noexcept override { return "png"; }
    TResult<TSharedPtr<Asset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

} // namespace acs
