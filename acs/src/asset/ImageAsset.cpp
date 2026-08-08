// SPDX-License-Identifier: Apache-2.0
// 画像アセット実装（stb_image によるデコード）
#include "asset/ImageAsset.h"
#include "foundation/Limits.h"
#include "memory/Memory.h"

#if defined(ACS_IMAGE_ASSET_TEST_HOOKS)
namespace acs::image_asset_test_detail {

/** stb_image が試験中に使う追跡可能な確保関数。 */
void* StbiMalloc(usize size) noexcept;

/** stb_image が試験中に使う追跡可能な再確保関数。 */
void* StbiRealloc(void* pointer, usize size) noexcept;

/** stb_image が試験中に使う追跡可能な解放関数。 */
void StbiFree(void* pointer) noexcept;

} // namespace acs::image_asset_test_detail

#define STBI_MALLOC(size) ::acs::image_asset_test_detail::StbiMalloc(size)
#define STBI_REALLOC(pointer, size) ::acs::image_asset_test_detail::StbiRealloc(pointer, size)
#define STBI_FREE(pointer) ::acs::image_asset_test_detail::StbiFree(pointer)
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO         // ファイル I/O は使わない（メモリから読む）
#include <stb_image.h>

namespace acs {

namespace {

/** decoderへ渡せない入力長を表すAsset error subcode。 */
inline constexpr u16 kImageSubInvalidInputLength = 102u;

/** decoderが返した寸法またはchannel数の不整合を表すAsset error subcode。 */
inline constexpr u16 kImageSubInvalidDecodedLayout = 103u;

/** pixel配列の確保失敗を表すMemory error subcode。 */
inline constexpr u16 kImageSubPixelAllocationFailed = 104u;

/** 画像asset本体の確保失敗を表すMemory error subcode。 */
inline constexpr u16 kImageSubAssetAllocationFailed = 105u;

/** stb_imageが返したpixel領域を全return経路で解放する所有guard。 */
class FStbiImageScope final {
public:
    /** 解放対象のpixel領域を受け取る。 */
    explicit FStbiImageScope(void* pixels) noexcept : m_Pixels(pixels) {}

    /** 保持しているpixel領域をstb_imageへ返す。 */
    ~FStbiImageScope() noexcept { ::stbi_image_free(m_Pixels); }

    FStbiImageScope(const FStbiImageScope&) = delete;
    FStbiImageScope& operator=(const FStbiImageScope&) = delete;

private:
    /** stb_imageが所有権を呼出側へ渡したpixel領域。 */
    void* m_Pixels = nullptr;
};

/** usizeの入力長をstb_imageのint引数へ安全に変換する。 */
bool TryConvertDecoderInputLength(usize input_size, int& decoder_input_length) noexcept
{
    if (input_size == 0u || input_size > static_cast<usize>(TNumLimits<i32>::Max())) return false;
    decoder_input_length = static_cast<int>(input_size);
    return true;
}

/** decoder結果からpixel配列のbyte数をoverflowなしで算出する。 */
bool TryCalculateDecodedByteCount(int width, int height, int channels, usize bytes_per_pixel, usize& byte_count) noexcept
{
    if (width <= 0 || height <= 0 || channels <= 0 || channels > 4 || bytes_per_pixel == 0u) return false;

    /** 符号検査済みの画像幅。 */
    const usize checked_width = static_cast<usize>(width);

    /** 符号検査済みの画像高さ。 */
    const usize checked_height = static_cast<usize>(height);

    /** usizeで表現できる最大byte数。 */
    constexpr usize maximum_byte_count = ~usize(0);

    if (checked_width > maximum_byte_count / checked_height) return false;

    /** overflow検査済みのpixel数。 */
    const usize pixel_count = checked_width * checked_height;
    if (pixel_count > maximum_byte_count / bytes_per_pixel) return false;

    byte_count = pixel_count * bytes_per_pixel;
    return true;
}

} // namespace

#if defined(ACS_IMAGE_ASSET_TEST_HOOKS)
namespace image_asset_test_detail {

/** 入力長preflightを専用試験から直接検証する。 */
bool TryConvertDecoderInputLengthForTesting(usize input_size, int& decoder_input_length) noexcept
{
    return TryConvertDecoderInputLength(input_size, decoder_input_length);
}

/** decode後layout preflightを専用試験から直接検証する。 */
bool TryCalculateDecodedByteCountForTesting(int width, int height, int channels, usize bytes_per_pixel, usize& byte_count) noexcept
{
    return TryCalculateDecodedByteCount(width, height, channels, bytes_per_pixel, byte_count);
}

} // namespace image_asset_test_detail
#endif

TResult<TSharedPtr<AAsset>> CImageAssetLoader::LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept {
    /** stb_imageへ渡すint表現の入力byte数。 */
    int decoder_input_length = 0;
    if (bytes.GetData() == nullptr || !TryConvertDecoderInputLength(bytes.Num(), decoder_input_length)) {
        return ACS_ERR(Asset, kImageSubInvalidInputLength, "image byte count is invalid or exceeds decoder range");
    }

    /** stb_imageが返す画像幅。 */
    int width = 0;

    /** stb_imageが返す画像高さ。 */
    int height = 0;

    /** stb_imageが返す入力画像のchannel数。 */
    int channels = 0;

    /** stb_imageへ渡す入力先頭。 */
    const stbi_uc* const input_bytes = reinterpret_cast<const stbi_uc*>(bytes.GetData());

    // HDR ファイル（.hdr など）は float ピクセル、それ以外は 8-bit に統一
    const bool is_hdr = ::stbi_is_hdr_from_memory(input_bytes, decoder_input_length) != 0;

    if (is_hdr) {
        // HDR: 32-bit float RGBA に強制（4ch 固定で扱いを統一）
        /** stb_imageが呼出側へ所有権を渡すfloat RGBA領域。 */
        float* const px = ::stbi_loadf_from_memory(input_bytes, decoder_input_length, &width, &height, &channels, 4);
        if (!px) return ACS_ERR(Asset, 100, "stbi_loadf_from_memory failed");

        /** 以降の全return経路でdecode領域を解放するguard。 */
        const FStbiImageScope decoded_pixels(px);

        /** float RGBA配列へコピーするbyte数。 */
        usize byte_count = 0u;
        if (!TryCalculateDecodedByteCount(width, height, channels, 4u * sizeof(float), byte_count)) {
            return ACS_ERR(Asset, kImageSubInvalidDecodedLayout, "decoded HDR image layout is invalid");
        }

        /** assetへ移すfloat RGBA byte列。 */
        TArray<byte> pixels;
        if (!pixels.TrySetNum(byte_count)) return ACS_ERR(Memory, kImageSubPixelAllocationFailed, "HDR image pixel allocation failed");
        MemCopy(pixels.GetData(), px, byte_count);

        /** 全pixelを保持する画像asset。 */
        TSharedPtr<AImageAsset> a = MakeShared<AImageAsset>(static_cast<u32>(width), static_cast<u32>(height), EPixelFormat::R32G32B32A32_F, Move(pixels));
        if (!a) return ACS_ERR(Memory, kImageSubAssetAllocationFailed, "HDR image asset allocation failed");
        a->SetId(id);
        a->SetState(EAssetState::Ready);
        return TResult<TSharedPtr<AAsset>>(OkInit, TSharedPtr<AAsset>(Move(a)));
    }

    // LDR: 8-bit RGBA に強制
    /** stb_imageが呼出側へ所有権を渡す8-bit RGBA領域。 */
    stbi_uc* const px = ::stbi_load_from_memory(input_bytes, decoder_input_length, &width, &height, &channels, 4);
    if (!px) return ACS_ERR(Asset, 101, "stbi_load_from_memory failed");

    /** 以降の全return経路でdecode領域を解放するguard。 */
    const FStbiImageScope decoded_pixels(px);

    /** 8-bit RGBA配列へコピーするbyte数。 */
    usize byte_count = 0u;
    if (!TryCalculateDecodedByteCount(width, height, channels, 4u, byte_count)) {
        return ACS_ERR(Asset, kImageSubInvalidDecodedLayout, "decoded LDR image layout is invalid");
    }

    /** assetへ移す8-bit RGBA byte列。 */
    TArray<byte> pixels;
    if (!pixels.TrySetNum(byte_count)) return ACS_ERR(Memory, kImageSubPixelAllocationFailed, "LDR image pixel allocation failed");
    MemCopy(pixels.GetData(), px, byte_count);

    /** 全pixelを保持する画像asset。 */
    TSharedPtr<AImageAsset> a = MakeShared<AImageAsset>(static_cast<u32>(width), static_cast<u32>(height), EPixelFormat::R8G8B8A8, Move(pixels));
    if (!a) return ACS_ERR(Memory, kImageSubAssetAllocationFailed, "LDR image asset allocation failed");
    a->SetId(id);
    a->SetState(EAssetState::Ready);
    return TResult<TSharedPtr<AAsset>>(OkInit, TSharedPtr<AAsset>(Move(a)));
}

} // namespace acs
