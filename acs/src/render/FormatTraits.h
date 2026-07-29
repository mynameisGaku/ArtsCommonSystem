// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "render/FormatAspect.h"
#include "render/RhiTypes.h"

namespace acs {

/** 一つの GPU 形式が持つ固定属性。 */
struct FFormatTraits {
    /** 一ブロックのバイト数。 */
    u8 bytes_per_block = 0u;
    /** 一ブロックの横画素数。 */
    u8 block_width = 1u;
    /** 一ブロックの縦画素数。 */
    u8 block_height = 1u;
    /** 形式が持つ画像成分。 */
    EFormatAspect aspects = EFormatAspect::None;
    /** ブロック圧縮形式なら true。 */
    bool compressed = false;
};

/** EFormat と同じ添字で参照する固定属性表。 */
inline constexpr FFormatTraits kFormatTraits[] = {{0u, 1u, 1u, EFormatAspect::None, false}, {4u, 1u, 1u, EFormatAspect::Color, false}, {4u, 1u, 1u, EFormatAspect::Color, false}, {4u, 1u, 1u, EFormatAspect::Color, false}, {4u, 1u, 1u, EFormatAspect::Color, false}, {4u, 1u, 1u, EFormatAspect::Color, false}, {8u, 1u, 1u, EFormatAspect::Color, false}, {4u, 1u, 1u, EFormatAspect::Color, false}, {8u, 1u, 1u, EFormatAspect::Color, false}, {12u, 1u, 1u, EFormatAspect::Color, false}, {16u, 1u, 1u, EFormatAspect::Color, false}, {4u, 1u, 1u, EFormatAspect::Depth | EFormatAspect::Stencil, false}, {4u, 1u, 1u, EFormatAspect::Depth, false}};
static_assert(sizeof(kFormatTraits) / sizeof(kFormatTraits[0]) == static_cast<usize>(EFormat::Count));

/** 指定形式の固定属性を返し、範囲外なら無効属性を返す。 */
constexpr FFormatTraits GetFormatTraits(EFormat format) noexcept {
    // 属性表を参照する添字。
    const usize index = static_cast<usize>(format);
    return index < static_cast<usize>(EFormat::Count) ? kFormatTraits[index] : FFormatTraits{};
}

/** 指定形式が画像成分を持つか返す。 */
constexpr bool FormatHasAspect(EFormat format, EFormatAspect aspect) noexcept {
    return (static_cast<u8>(GetFormatTraits(format).aspects) & static_cast<u8>(aspect)) != 0u;
}

/** 指定形式が深度成分を持つか返す。 */
constexpr bool IsDepthFormat(EFormat format) noexcept {
    return FormatHasAspect(format, EFormatAspect::Depth);
}

/** 指定形式が色成分を持つか返す。 */
constexpr bool IsColorFormat(EFormat format) noexcept {
    return FormatHasAspect(format, EFormatAspect::Color);
}

/** 指定形式を色または深度対象へ使用できるか返す。 */
constexpr bool IsFormatUsageLegal(EFormat format, bool depth_target) noexcept {
    // 検査する固定属性。
    const FFormatTraits traits = GetFormatTraits(format);
    return traits.bytes_per_block != 0u && traits.block_width != 0u && traits.block_height != 0u &&
           (depth_target ? IsDepthFormat(format) : IsColorFormat(format));
}

} // namespace acs
