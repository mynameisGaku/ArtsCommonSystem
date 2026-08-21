// SPDX-License-Identifier: Apache-2.0
#pragma once
// RHI 共通型（フォーマット、FViewport、FClearColor など、バックエンド非依存）

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs {

/**
 * D3D12 外部描画へ一時的に貸し出すネイティブ資源。
 *
 * @details
 * 各ポインタは ACS が所有し、呼出し中だけ有効な借用値である。外部側は
 * AddRef/Release せず、FramesInFlight は外部描画側のフレーム資源管理に使う。
 * 非 D3D12 バックエンドでは IRhiDevice の取得 API が false を返す。
 */
struct FRhiD3D12DeviceInterop final {
    /** ACS が所有する ID3D12Device 相当の借用ポインタ。 */
    void* Device = nullptr;

    /** ACS が所有する DIRECT グラフィックスキュー相当の借用ポインタ。 */
    void* GraphicsQueue = nullptr;

    /** ACS が同時に保持するフレームスロット数。 */
    u32 FramesInFlight = 0u;

    /** device / queue / frame 数が揃い、外部描画へ渡せる値かを返す。 */
    bool IsValid() const noexcept
    {
        return Device != nullptr && GraphicsQueue != nullptr &&
               FramesInFlight > 0u;
    }
};

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
