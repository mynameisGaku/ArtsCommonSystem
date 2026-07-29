// SPDX-License-Identifier: Apache-2.0
// RHI 共通型（フォーマット、FViewport、FClearColor など、バックエンド非依存）
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs {

/**
 * ピクセルフォーマット (DX12 / Vulkan 等で共通の論理フォーマット)。
 */
enum class EFormat : u8 {
    /** 未指定 / 無効。 */
    Unknown = 0,

    /** 8bit × 4 チャンネル。画像系の標準フォーマット。 */
    R8G8B8A8_UNorm,

    /** R8G8B8A8 を sRGB として解釈する。 */
    R8G8B8A8_UNorm_sRGB,

    /** 整数 4 ch (ボーン indices 等)。 */
    R8G8B8A8_UInt,

    /** BGRA 配置 (Win32 ネイティブ)。 */
    B8G8R8A8_UNorm,

    /** 2 ch 半精度 (BRDF LUT 等)。 */
    R16G16_Float,

    /** 4 ch 半精度。HDR レンダーターゲット用。 */
    R16G16B16A16_Float,

    /** 32bit にパック済みコンパクト HDR (環境/IBL prefilter 向け)。 */
    R11G11B10_Float,

    /** 2 ch 単精度。2D 座標バッファ。 */
    R32G32_Float,

    /** 3 ch 単精度。3D 座標バッファ。 */
    R32G32B32_Float,

    /** 4 ch 単精度。4D ベクトル。 */
    R32G32B32A32_Float,

    /** 深度 24bit + ステンシル 8bit。 */
    D24_UNorm_S8_UInt,

    /** 深度のみ (32bit float)。 */
    D32_Float,

    /** 列挙・表の網羅性を検証する終端値。GPU 形式としては使用しない。 */
    Count,
};

enum class EFormatAspect : u8 {
    None = 0u,
    Color = 1u,
    Depth = 2u,
    Stencil = 4u,
};

constexpr EFormatAspect operator|(EFormatAspect a, EFormatAspect b) noexcept
{
    return static_cast<EFormatAspect>(
        static_cast<u8>(a) | static_cast<u8>(b));
}

struct FFormatTraits {
    u8 bytes_per_block = 0u;
    u8 block_width = 1u;
    u8 block_height = 1u;
    EFormatAspect aspects = EFormatAspect::None;
    bool compressed = false;
};

inline constexpr FFormatTraits kFormatTraits[] = {
    {0u, 1u, 1u, EFormatAspect::None, false},
    {4u, 1u, 1u, EFormatAspect::Color, false},
    {4u, 1u, 1u, EFormatAspect::Color, false},
    {4u, 1u, 1u, EFormatAspect::Color, false},
    {4u, 1u, 1u, EFormatAspect::Color, false},
    {4u, 1u, 1u, EFormatAspect::Color, false},
    {8u, 1u, 1u, EFormatAspect::Color, false},
    {4u, 1u, 1u, EFormatAspect::Color, false},
    {8u, 1u, 1u, EFormatAspect::Color, false},
    {12u, 1u, 1u, EFormatAspect::Color, false},
    {16u, 1u, 1u, EFormatAspect::Color, false},
    {4u, 1u, 1u, EFormatAspect::Depth | EFormatAspect::Stencil, false},
    {4u, 1u, 1u, EFormatAspect::Depth, false},
};
static_assert(
    sizeof(kFormatTraits) / sizeof(kFormatTraits[0]) ==
    static_cast<usize>(EFormat::Count));

constexpr FFormatTraits GetFormatTraits(EFormat format) noexcept
{
    const usize index = static_cast<usize>(format);
    return index < static_cast<usize>(EFormat::Count)
        ? kFormatTraits[index]
        : FFormatTraits{};
}

constexpr bool FormatHasAspect(
    EFormat format, EFormatAspect aspect) noexcept
{
    return (static_cast<u8>(GetFormatTraits(format).aspects) &
            static_cast<u8>(aspect)) != 0u;
}

constexpr bool IsDepthFormat(EFormat format) noexcept
{
    return FormatHasAspect(format, EFormatAspect::Depth);
}

constexpr bool IsColorFormat(EFormat format) noexcept
{
    return FormatHasAspect(format, EFormatAspect::Color);
}

constexpr bool IsFormatUsageLegal(
    EFormat format, bool depth_target) noexcept
{
    const FFormatTraits traits = GetFormatTraits(format);
    return traits.bytes_per_block != 0u &&
           traits.block_width != 0u && traits.block_height != 0u &&
           (depth_target ? IsDepthFormat(format) : IsColorFormat(format));
}

/** バックエンドのパイプライン束縛キャッシュをコンパイル時に特殊化する領域。 */
enum class ERhiPipelineBindDomain : u8 {
    Graphics,
    Compute,
};

template<ERhiPipelineBindDomain Domain>
struct TRhiPipelineBindPolicy {
    static constexpr bool Accepts(bool is_compute) noexcept
    {
        if constexpr (Domain == ERhiPipelineBindDomain::Compute)
            return is_compute;
        else
            return !is_compute;
    }

    template<typename Pipeline>
    static constexpr bool NeedsBind(
        const Pipeline* current, const Pipeline* requested) noexcept
    {
        return requested != nullptr && current != requested;
    }
};

/**
 * ビューポート (描画先矩形と深度レンジ)。
 */
struct FViewport {
    /** 左上 X 座標 (ピクセル)。 */
    f32 x         = 0.0f;

    /** 左上 Y 座標 (ピクセル)。 */
    f32 y         = 0.0f;

    /** 幅 (ピクセル)。 */
    f32 width     = 0.0f;

    /** 高さ (ピクセル)。 */
    f32 height    = 0.0f;

    /** 深度レンジの最小値。 */
    f32 min_depth = 0.0f;

    /** 深度レンジの最大値。 */
    f32 max_depth = 1.0f;
};

/**
 * シザー矩形 (描画をクリップする領域)。
 */
struct FScissorRect {
    /** 左端 X 座標 (ピクセル)。 */
    i32 left   = 0;

    /** 上端 Y 座標 (ピクセル)。 */
    i32 top    = 0;

    /** 右端 X 座標 (ピクセル、排他)。 */
    i32 right  = 0;

    /** 下端 Y 座標 (ピクセル、排他)。 */
    i32 bottom = 0;
};

/**
 * クリア色 (RGBA、各成分 0..1)。
 */
struct FClearColor {
    /** 赤成分 (0..1)。 */
    f32 r = 0.0f;

    /** 緑成分 (0..1)。 */
    f32 g = 0.0f;

    /** 青成分 (0..1)。 */
    f32 b = 0.0f;

    /** アルファ成分 (0..1)。 */
    f32 a = 1.0f;
};

/**
 * プリミティブ種別 (頂点をどう組み立てるか)。
 */
enum class EPrimitiveTopology : u8 {
    /** 点リスト。 */
    PointList,

    /** 独立した線分リスト。 */
    LineList,

    /** 連続した折れ線。 */
    LineStrip,

    /** 独立した三角形リスト。 */
    TriangleList,

    /** 連続した三角形ストリップ。 */
    TriangleStrip,
};

/**
 * リソース状態 (DX12 と Vulkan barrier の共通抽象)。
 */
enum class EResourceState : u8 {
    /** 既定状態。 */
    Common,

    /** レンダーターゲットとして書き込み中。 */
    RenderTarget,

    /** 画面提示 (present) 用。 */
    Present,

    /** コピー元。 */
    CopySrc,

    /** コピー先。 */
    CopyDst,

    /** UAV (順不同アクセス)。 */
    UnorderedAccess,

    /** ピクセルシェーダのリソース (sample 用)。 */
    PixelShaderResource,

    /** 深度書き込み。 */
    DepthWrite,

    /** 深度読み取り (テストのみ)。 */
    DepthRead,
};

} // namespace acs
