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
class FImageAsset : public FAsset {
public:
    ACS_ASSET_TYPE("FImageAsset")

    FImageAsset() noexcept = default;
    FImageAsset(u32 w, u32 h, EPixelFormat fmt, TArray<byte>&& pixels) noexcept
        : _width(w), _height(h), _format(fmt), _pixels(Move(pixels)) {}

    u32         Width()  const noexcept { return _width; }
    u32         Height() const noexcept { return _height; }
    EPixelFormat EFormat() const noexcept { return _format; }
    const byte* Pixels() const noexcept { return _pixels.Data(); }
    usize       PixelByteCount() const noexcept { return _pixels.Size(); }

private:
    u32         _width  = 0;
    u32         _height = 0;
    EPixelFormat _format = EPixelFormat::R8G8B8A8;
    TArray<byte> _pixels;
};

// 画像ローダ (stb_image)
class FImageAssetLoader final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return FImageAsset::StaticType(); }
    const char* Extension() const noexcept override { return "png"; }
    TResult<TRc<FAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

} // namespace acs
