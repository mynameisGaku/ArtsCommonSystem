// RHI 共通型（フォーマット、Viewport、ClearColor など、バックエンド非依存）
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs {

// ピクセルフォーマット（DX12 / Vulkan 等で共通の論理フォーマット）
enum class Format : u8 {
    Unknown = 0,
    R8G8B8A8_UNorm,        // 8bit × 4 チャンネル、画像系の標準
    R8G8B8A8_UNorm_sRGB,   // sRGB 解釈
    R8G8B8A8_UInt,         // 整数 4 ch（ボーン indices 等）
    B8G8R8A8_UNorm,        // BGRA 配置（Win32 ネイティブ）
    R16G16_Float,          // BRDF LUT 等の 2 ch 半精度
    R16G16B16A16_Float,    // HDR レンダーターゲット用
    R11G11B10_Float,       // 32bit にパック済みコンパクト HDR (環境/IBL prefilter 向け)
    R32G32_Float,          // 2D 座標バッファ
    R32G32B32_Float,       // 3D 座標バッファ
    R32G32B32A32_Float,    // 4D ベクトル
    D24_UNorm_S8_UInt,     // 深度 + ステンシル
    D32_Float,             // 深度のみ
};

// ビューポート
struct Viewport {
    f32 x         = 0.0f;
    f32 y         = 0.0f;
    f32 width     = 0.0f;
    f32 height    = 0.0f;
    f32 min_depth = 0.0f;
    f32 max_depth = 1.0f;
};

// シザー矩形
struct ScissorRect {
    i32 left   = 0;
    i32 top    = 0;
    i32 right  = 0;
    i32 bottom = 0;
};

// クリア色（RGBA、各 0..1）
struct ClearColor {
    f32 r = 0.0f;
    f32 g = 0.0f;
    f32 b = 0.0f;
    f32 a = 1.0f;
};

// プリミティブ種別
enum class PrimitiveTopology : u8 {
    PointList,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
};

// リソース状態（DX12 と Vulkan barrier の共通抽象）
enum class ResourceState : u8 {
    Common,
    RenderTarget,
    Present,
    CopySrc,
    CopyDst,
    UnorderedAccess,
    PixelShaderResource,
    DepthWrite,
    DepthRead,
};

} // namespace acs
