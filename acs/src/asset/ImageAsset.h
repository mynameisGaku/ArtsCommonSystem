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

/**
 * 画像のピクセルフォーマット (チャンネル数と要素型で識別)。
 */
enum class EPixelFormat : u8 {
    /** 8-bit 単チャンネル (グレースケール)。 */
    R8,

    /** 8-bit RG (グレースケール + アルファ)。 */
    R8G8,

    /** 8-bit RGB。 */
    R8G8B8,

    /** 8-bit RGBA。 */
    R8G8B8A8,

    /** 32-bit float RGB (HDR)。 */
    R32G32B32_F,

    /** 32-bit float RGBA (HDR)。 */
    R32G32B32A32_F,
};

/**
 * 画像 1 枚分の CPU 側ピクセルデータを保持するアセット。
 */
class FImageAsset : public FAsset {
public:
    ACS_ASSET_TYPE("FImageAsset")

    /** 空の画像アセットを構築する。 */
    FImageAsset() noexcept = default;

    /**
     * ピクセルデータから画像アセットを構築する。
     *
     * @param w 画像の幅 (ピクセル)。
     * @param h 画像の高さ (ピクセル)。
     * @param fmt ピクセルフォーマット。
     * @param pixels 生ピクセルバイト列 (ムーブで所有を奪う)。
     */
    FImageAsset(u32 w, u32 h, EPixelFormat fmt, TArray<byte>&& pixels) noexcept
        : m_Width(w), m_Height(h), m_Format(fmt), m_Pixels(Move(pixels)) {}

    /**
     * 画像の幅を返す。
     *
     * @return 幅 (ピクセル)。
     */
    u32         Width()  const noexcept { return m_Width; }

    /**
     * 画像の高さを返す。
     *
     * @return 高さ (ピクセル)。
     */
    u32         Height() const noexcept { return m_Height; }

    /**
     * ピクセルフォーマットを返す。
     *
     * @return EPixelFormat。
     */
    EPixelFormat Format() const noexcept { return m_Format; }

    /**
     * 生ピクセルデータの先頭ポインタを返す。
     *
     * @return ピクセルバイト列の先頭。
     */
    const byte* Pixels() const noexcept { return m_Pixels.Data(); }

    /**
     * 生ピクセルデータのバイト数を返す。
     *
     * @return ピクセルバイト列のサイズ。
     */
    usize       PixelByteCount() const noexcept { return m_Pixels.Size(); }

private:
    /** 画像の幅 (ピクセル)。 */
    u32         m_Width  = 0;

    /** 画像の高さ (ピクセル)。 */
    u32         m_Height = 0;

    /** ピクセルフォーマット。 */
    EPixelFormat m_Format = EPixelFormat::R8G8B8A8;

    /** 生ピクセルデータ。 */
    TArray<byte> m_Pixels;
};

/**
 * 各種画像ファイルを FImageAsset にデコードするローダ (stb_image)。
 */
class FImageAssetLoader final : public IAssetLoader {
public:
    /**
     * 生成するアセット型 ID を返す。
     *
     * @return FImageAsset の型 ID。
     */
    AssetType   TypeId()    const noexcept override { return FImageAsset::StaticType(); }

    /**
     * 担当する主要拡張子を返す。
     *
     * @return "png" (jpg/bmp/hdr などはレジストリが別名で登録する)。
     */
    const char* Extension() const noexcept override { return "png"; }

    /**
     * 画像バイト列をデコードし FImageAsset を生成する。
     *
     * @details HDR (.hdr 等) は 32-bit float RGBA、それ以外は 8-bit RGBA に統一する。
     * @param id 生成アセットに割り当てる ID。
     * @param bytes 画像ファイル全体のバイト列。
     * @return 成功なら FImageAsset、失敗ならエラー。
     */
    TResult<TSharedPtr<FAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

} // namespace acs
