// SPDX-License-Identifier: Apache-2.0
// 2D テクスチャ抽象（GPU 上の画像。シェーダから読める SRV を持つ）
//
// 使い方:
//   FTextureDesc d{};
//   d.width = 256; d.height = 256;
//   d.format = EFormat::R8G8B8A8_UNorm;
//   d.initial_data = pixels;          // RGBA 8bit、上から下、左から右の順
//   d.initial_data_size = 256*256*4;
//   auto tex = CreateRhiTexture(device, d).Value();
//
// 描画時:
//   cmd.SetTexture(0, *tex);          // パイプラインで texture_slots>=1 が必要
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/RhiTypes.h"

namespace acs {

class IRhiDevice;

/**
 * テクスチャ生成パラメータ。
 *
 * @details
 * サイズ・フォーマット・ミップ・配列段数のほか、RT / 深度ターゲット / cubemap などの
 * 用途フラグと初期ピクセルデータを指定する。CreateRhiTexture に渡して生成する。
 */
struct FTextureDesc {
    /** テクスチャの幅（ピクセル）。 */
    u32         width            = 0;

    /** テクスチャの高さ（ピクセル）。 */
    u32         height           = 0;

    /** ピクセルフォーマット。 */
    EFormat      format           = EFormat::R8G8B8A8_UNorm;

    /** ミップレベル数（1 = ベースのみ。>1 で生成する場合は per_slice_rtv を併用してミップ毎に描画する）。 */
    u32         mip_levels       = 1;

    /** 配列段数（1 = 単一、6 = cubemap、>1 = テクスチャ配列）。 */
    u32         array_size       = 1;

    /** cubemap として扱うか（true なら array_size==6 を要求。Diligent: RESOURCE_DIM_TEX_CUBE）。 */
    bool        is_cubemap       = false;

    /** レンダーターゲットとして使うか。 */
    bool        is_render_target = false;

    /** 深度バッファとして使うか。 */
    bool        is_depth_target  = false;

    /** is_depth_target=true のとき SRV も作るか（シャドウマップ用）。 */
    bool        shader_visible_depth = false;

    /** is_render_target=true のとき array_size*mip_levels 個の RTV を per-slice 作成するか（cubemap 面／ミップ別パスを書くのに必要）。 */
    bool        per_slice_rtv    = false;

    /** MSAA サンプル数 (1=非 MSAA)。>1 は is_render_target=true 専用で SRV は作られない
     *  (MS テクスチャは通常 sample 不可)。描画後に IRhiCommandList::ResolveToSwapchain で解決する。 */
    u32         sample_count     = 1;

    /** 初期ピクセルデータ（RGBA 等、tightly-packed。cubemap/array 初期化は未サポート）。 */
    const void* initial_data     = nullptr;

    /** 初期ピクセルデータのバイト数。 */
    usize       initial_data_size = 0;

    /** UAV (RWTexture) として compute から書き込み可能にするか (Phase 0)。BIND_UNORDERED_ACCESS + UAV view。 */
    bool        is_uav           = false;

    /** 深度 (3D テクスチャ用。>1 で RESOURCE_DIM_TEX_3D。volumetric clouds の shape/detail noise 用)。 */
    u32         depth            = 1;
};

/**
 * GPU 上の 2D 画像を表すテクスチャ抽象（シェーダから読める SRV を持つ）。
 *
 * @details
 * CreateRhiTexture で生成し、描画時に cmd.SetTexture でスロットへバインドして使う。
 * RT / 深度ターゲット / cubemap など FTextureDesc の用途フラグに応じたリソースを表す。
 */
class IRhiTexture {
public:
    /** 派生バックエンド実装を正しく破棄するための仮想デストラクタ。 */
    virtual ~IRhiTexture() noexcept = default;

    /**
     * テクスチャの幅を返す。
     *
     * @return 幅（ピクセル）。
     */
    virtual u32    Width()      const noexcept = 0;

    /**
     * テクスチャの高さを返す。
     *
     * @return 高さ（ピクセル）。
     */
    virtual u32    Height()     const noexcept = 0;

    /**
     * テクスチャのピクセルフォーマットを返す。
     *
     * @return ピクセルフォーマット。
     */
    virtual EFormat PixelFormat() const noexcept = 0;

    /**
     * ミップレベル数を返す。
     *
     * @details 既存バックエンドは安全な既定値 1 を返してよい。
     * @return ミップレベル数。
     */
    virtual u32    MipLevels()  const noexcept { return 1; }

    /**
     * 配列段数を返す。
     *
     * @details 既存バックエンドは安全な既定値 1 を返してよい。
     * @return 配列段数。
     */
    virtual u32    ArraySize()  const noexcept { return 1; }

    /**
     * cubemap かどうかを返す。
     *
     * @details 既存バックエンドは安全な既定値 false を返してよい。
     * @return cubemap なら true。
     */
    virtual bool   IsCubemap()  const noexcept { return false; }

    /**
     * Returns whether the texture owns a depth-stencil target view.
     *
     * The default is false so lightweight test/back-end textures remain
     * source compatible. Production depth implementations override it.
     */
    virtual bool IsDepthTarget() const noexcept { return false; }

    /**
     * Returns whether a depth texture also owns a shader-resource view.
     *
     * A depth snapshot used while the live scene depth is bound as a DSV must
     * return true here. This is deliberately separate from IsDepthTarget():
     * shadow-only or transient depth resources do not necessarily expose an
     * SRV.
     */
    virtual bool IsShaderVisibleDepth() const noexcept { return false; }

    /** Number of samples stored per texel (one for non-MSAA textures). */
    virtual u32 SampleCount() const noexcept { return 1; }
};

/**
 * テクスチャを生成する（初期データがあれば同期アップロードして即使用可能）。
 *
 * @param device テクスチャ生成に使う RHI デバイス。
 * @param desc テクスチャ生成パラメータ。
 * @return 成功なら所有権付きの IRhiTexture、生成失敗ならエラー。
 */
TResult<TUniquePtr<IRhiTexture>> CreateRhiTexture(IRhiDevice& device,
                                                     const FTextureDesc& desc) noexcept;

} // namespace acs
